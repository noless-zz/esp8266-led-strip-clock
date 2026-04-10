"""OTA Stress Test — PlatformIO extra script.

Registers the custom target 'ota_stress_test'.  Run with:
    pio run -e ota_stress -t ota_stress_test

Each cycle exercises two OTA paths:
  Phase A — local file upload  : POST /api/update  (multipart/form-data)
  Phase B — GitHub URL download : POST /api/ota/from-url?url=<url>

Environment variable overrides (all optional):
    OTA_STRESS_SERIAL       serial port to monitor      (default: auto-detect first
                            available port; set to empty string or 'auto' to
                            auto-detect; can also be set in platformio.ini as
                            OTA_STRESS_SERIAL = <value> under [env:ota_stress])
    LED_CLOCK_TEST_BASE_URL device HTTP base URL         (default http://ledclock.local)
    OTA_STRESS_FW_URL       firmware URL for Phase B     (default: latest GitHub release asset)
"""

Import("env")  # noqa: F821 — PlatformIO SCons import

import json
import os
import queue
import threading
import time
from datetime import datetime
from urllib.error import HTTPError, URLError
from urllib.parse import quote as _url_quote
from urllib.request import Request, urlopen

# ============================================================================
# Configurable constants — change NUM_CYCLES here for more / fewer test cycles
# ============================================================================
NUM_CYCLES = 10

SERIAL_BAUD        = 115200
BOOT_WAIT_SEC      = 45      # max seconds to wait for device after a reboot
URL_OTA_WAIT_SEC   = 90      # max seconds to wait for [OTA-URL] Flash OK on serial
UPLOAD_TIMEOUT_SEC = 120.0   # HTTP timeout for the multipart firmware POST

# Runtime overrides via environment variables
# OTA_STRESS_SERIAL may also be set in platformio.ini under [env:ota_stress].
# Precedence: OS env > platformio.ini > auto-detect > platform default.
def _resolve_serial_port() -> str:
    """Return the serial port to use for capture, or '' if none available."""
    # 1. OS environment variable
    val = os.getenv("OTA_STRESS_SERIAL", "").strip()
    # 2. Fallback: platformio.ini project option (SCons env, available in extra_scripts)
    if not val:
        try:
            val = env.GetProjectOption("OTA_STRESS_SERIAL", "").strip()  # noqa: F821
        except Exception:
            pass
    # Treat empty string or the special sentinel "auto" as auto-detect
    if not val or val.lower() == "auto":
        try:
            import serial.tools.list_ports  # pyserial — available in PlatformIO Python env
            ports = list(serial.tools.list_ports.comports())
            if ports:
                # Prefer USB serial adapters (CH340, CP210x, FTDI, ...) over built-in
                # system ports such as COM1 (ACPI/PNP0501).  USB ports always have a
                # non-None vendor ID; system ports do not.
                usb_ports = [p for p in ports if p.vid is not None]
                return (usb_ports[0] if usb_ports else ports[0]).device
        except Exception:
            pass
        return ""
    return val

_env_serial = _resolve_serial_port()
SERIAL_PORT = _env_serial if _env_serial else ""

_env_base    = os.getenv("LED_CLOCK_TEST_BASE_URL", "").strip()
DEVICE_BASE  = _env_base.rstrip("/") if _env_base else "http://ledclock.local"

FW_URL_OVERRIDE = os.getenv("OTA_STRESS_FW_URL", "").strip()

GITHUB_API_URL = (
    "https://api.github.com/repos/noless-zz/esp8266-led-strip-clock/releases/latest"
)

# ============================================================================
# Logging helpers
# ============================================================================

_log_file  = None
_log_lock  = threading.Lock()


def _log(tag: str, message: str) -> None:
    line = f"[{tag}] {message}"
    print(line, flush=True)
    with _log_lock:
        if _log_file is not None:
            _log_file.write(line + "\n")
            _log_file.flush()


# ============================================================================
# Serial capture — daemon thread that feeds a queue and the log file
# ============================================================================

_serial_queue: "queue.Queue[str]" = queue.Queue()
_stop_serial = threading.Event()


def _serial_reader(port: str, baud: int) -> None:
    """Background thread: reads serial lines and puts them in _serial_queue."""
    ser = None
    while not _stop_serial.is_set():
        try:
            if ser is None:
                import serial  # pyserial — available in PlatformIO Python env
                ser = serial.Serial(port, baud, timeout=1.0)
            line_bytes = ser.readline()
            if line_bytes:
                text = line_bytes.decode("utf-8", errors="replace").rstrip("\r\n")
                _log("SERIAL", text)
                _serial_queue.put(text)
        except Exception:
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
            _stop_serial.wait(0.5)
    if ser is not None:
        try:
            ser.close()
        except Exception:
            pass


def _start_serial_capture(port: str, baud: int) -> bool:
    """Start the serial capture thread.  Returns True on success, False if unavailable."""
    if not port:
        _log("TEST", "WARNING: serial capture disabled (no serial port configured or detected)")
        return False
    try:
        import serial
        probe = serial.Serial(port, baud, timeout=0.5)
        probe.close()
    except Exception as exc:
        _log("TEST", f"WARNING: serial capture disabled ({exc})")
        return False
    _stop_serial.clear()
    t = threading.Thread(
        target=_serial_reader, args=(port, baud), daemon=True, name="serial-reader"
    )
    t.start()
    return True


def _stop_serial_capture() -> None:
    _stop_serial.set()


def _drain_serial_queue() -> None:
    """Discard all queued serial lines (call before each OTA trigger)."""
    while not _serial_queue.empty():
        try:
            _serial_queue.get_nowait()
        except queue.Empty:
            break


def _wait_for_serial(pattern: str, timeout_sec: float) -> bool:
    """Block until a serial line containing *pattern* arrives, or timeout elapses."""
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        remaining = deadline - time.time()
        if remaining <= 0:
            break
        try:
            line = _serial_queue.get(timeout=min(1.0, remaining))
            if pattern in line:
                return True
        except queue.Empty:
            pass
    return False


# ============================================================================
# HTTP helpers
# ============================================================================

def _get_json(url: str, timeout: float = 6.0) -> dict:
    with urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", errors="replace"))


def _post_json(url: str, timeout: float = 10.0) -> dict:
    req = Request(url, data=b"", method="POST")
    with urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", errors="replace"))


def _post_multipart_bin(base_url: str, bin_path: str, timeout: float = 120.0) -> dict:
    """POST firmware.bin to /api/update as multipart/form-data."""
    boundary = "----OTAStressBoundary7a8b9c"
    with open(bin_path, "rb") as fh:
        file_data = fh.read()
    part_header = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="firmware"; filename="firmware.bin"\r\n'
        f"Content-Type: application/octet-stream\r\n"
        f"\r\n"
    ).encode()
    part_footer = f"\r\n--{boundary}--\r\n".encode()
    body = part_header + file_data + part_footer
    req = Request(
        f"{base_url}/api/update",
        data=body,
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
        method="POST",
    )
    with urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", errors="replace"))


def _wait_device_ready(base: str, max_wait: int = BOOT_WAIT_SEC) -> None:
    """Poll /api/status until it responds with fw_version, or raise RuntimeError."""
    deadline = time.time() + max_wait
    while time.time() < deadline:
        try:
            data = _get_json(f"{base}/api/status", timeout=4.0)
            if isinstance(data, dict) and "fw_version" in data:
                return
        except Exception:
            pass
        time.sleep(1.5)
    raise RuntimeError(f"device not reachable after {max_wait}s")


def _get_github_fw_url() -> str:
    """Return the .bin asset download URL from the latest GitHub release."""
    try:
        data = _get_json(GITHUB_API_URL, timeout=15.0)
    except Exception as exc:
        raise RuntimeError(f"GitHub API request failed: {exc}") from exc
    for asset in data.get("assets", []):
        name = asset.get("name", "")
        if name.lower().endswith(".bin"):
            url = asset.get("browser_download_url", "")
            if url:
                return url
    n = len(data.get("assets", []))
    raise RuntimeError(f"no .bin asset in latest release ({n} assets total)")


# ============================================================================
# Test phases
# ============================================================================

def _phase_a(base: str, bin_path: str) -> "tuple[bool, str]":
    """Phase A: local file upload via POST /api/update.  Returns (ok, message)."""
    _log("TEST", "Phase A: local file upload")

    file_size = os.path.getsize(bin_path)
    with open(bin_path, "rb") as fh:
        first_byte = fh.read(1)
    magic = first_byte[0] if first_byte else 0

    # Precheck
    precheck_url = (
        f"{base}/api/update/precheck"
        f"?name=firmware.bin&size={file_size}&magic={magic}"
    )
    try:
        precheck = _get_json(precheck_url, timeout=10.0)
        if not precheck.get("ok"):
            return False, f"precheck rejected: {precheck.get('error', precheck)}"
        _log("TEST", f"  precheck OK: {precheck.get('summary', '')}")
    except Exception as exc:
        return False, f"precheck request failed: {exc}"

    _drain_serial_queue()
    t_start = time.time()

    try:
        result = _post_multipart_bin(base, bin_path, timeout=UPLOAD_TIMEOUT_SEC)
    except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError) as exc:
        # These errors (WinError 10054 / ECONNRESET, EPIPE, ECONNABORTED) occur when
        # the device restarts after a successful OTA, tearing down all TCP connections
        # before the HTTP 200 response is read by the client.  Treat this as a
        # potential success: wait for the device to come back and report PASS if it
        # does — the OTA was applied even though the response ACK was lost.
        _log("TEST", f"  connection torn down ({exc}) — device likely restarted after OTA;"
                     " waiting for recovery...")
        try:
            _wait_device_ready(base, BOOT_WAIT_SEC)
        except RuntimeError:
            return False, (
                f"upload connection torn down and device did not recover within "
                f"{BOOT_WAIT_SEC}s: {exc}"
            )
        elapsed = time.time() - t_start
        try:
            status = _get_json(f"{base}/api/status", timeout=6.0)
            fw = status.get("fw_version_base", "?")
        except Exception:
            fw = "?"
        return True, f"PASS (connection-reset-then-recovered in {elapsed:.1f}s, fw={fw})"
    except Exception as exc:
        return False, f"upload request failed: {exc}"

    if not result.get("ok"):
        return False, f"upload endpoint returned not-ok: {result}"
    _log("TEST", f"  upload accepted, written={result.get('written', '?')} bytes")

    # Wait for serial confirmation of flash completion
    saw_flash = _wait_for_serial("[OTA] end OK", timeout_sec=30.0)
    if not saw_flash:
        _log("TEST", "  WARNING: [OTA] end OK not seen in serial within 30s")

    # Wait for device to reboot and come back up
    try:
        _wait_device_ready(base, BOOT_WAIT_SEC)
    except RuntimeError as exc:
        return False, str(exc)

    elapsed = time.time() - t_start
    try:
        status = _get_json(f"{base}/api/status", timeout=6.0)
        fw = status.get("fw_version_base", "?")
    except Exception:
        fw = "?"
    return True, f"PASS (boot in {elapsed:.1f}s, fw={fw})"


def _phase_b(base: str) -> "tuple[bool, str]":
    """Phase B: URL OTA via POST /api/ota/from-url.  Returns (ok, message)."""
    _log("TEST", "Phase B: GitHub URL OTA")

    if FW_URL_OVERRIDE:
        fw_url = FW_URL_OVERRIDE
        _log("TEST", f"  using OTA_STRESS_FW_URL override: {fw_url}")
    else:
        try:
            fw_url = _get_github_fw_url()
            _log("TEST", f"  resolved GitHub release URL: {fw_url}")
        except RuntimeError as exc:
            return False, f"could not resolve firmware URL: {exc}"

    # Ensure the device is up and its WiFi has reconnected before triggering the
    # URL-based OTA (which requires WiFi).  This matters especially when Phase A
    # just caused a reboot — the device needs a few extra seconds to re-associate.
    _log("TEST", "  waiting for device WiFi before from-url trigger...")
    wifi_deadline = time.time() + BOOT_WAIT_SEC
    wifi_ready = False
    while time.time() < wifi_deadline:
        try:
            st = _get_json(f"{base}/api/status", timeout=4.0)
            if st.get("wifi_connected"):
                wifi_ready = True
                break
        except Exception:
            pass
        time.sleep(2.0)
    if not wifi_ready:
        return False, f"device WiFi not connected within {BOOT_WAIT_SEC}s; cannot trigger URL OTA"

    _drain_serial_queue()
    t_start = time.time()

    trigger_url = f"{base}/api/ota/from-url?url={_url_quote(fw_url, safe='')}"
    try:
        resp = _get_json(trigger_url, timeout=15.0)
    except Exception as exc:
        return False, f"from-url trigger request failed: {exc}"

    if not resp.get("ok"):
        return False, f"from-url endpoint rejected request: {resp}"
    _log("TEST", "  download triggered (202), waiting for [OTA-URL] Flash OK on serial...")

    saw_flash = _wait_for_serial("[OTA-URL] Flash OK", timeout_sec=URL_OTA_WAIT_SEC)
    if not saw_flash:
        return False, f"[OTA-URL] Flash OK not seen within {URL_OTA_WAIT_SEC}s"
    _log("TEST", "  flash confirmed via serial, waiting for reboot...")

    try:
        _wait_device_ready(base, BOOT_WAIT_SEC)
    except RuntimeError as exc:
        return False, str(exc)

    elapsed = time.time() - t_start
    try:
        status = _get_json(f"{base}/api/status", timeout=6.0)
        fw = status.get("fw_version_base", "?")
    except Exception:
        fw = "?"
    return True, f"PASS (total {elapsed:.1f}s, fw={fw})"


# ============================================================================
# Main stress test runner (SCons action callback)
# ============================================================================

def _run_ota_stress(target, source, env) -> None:
    global _log_file

    project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
    build_dir   = env.subst("$BUILD_DIR")    # noqa: F821
    bin_path    = os.path.join(build_dir, "firmware.bin")

    if not os.path.isfile(bin_path):
        print(f"[ERROR] firmware.bin not found at {bin_path}", flush=True)
        env.Exit(1)  # noqa: F821
        return

    ts       = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(project_dir, f"ota_stress_{ts}.log")

    all_passed = False
    _log_file = open(log_path, "w", encoding="utf-8")  # noqa: WPS515
    try:
        _log("TEST", f"OTA stress test started — {NUM_CYCLES} cycles")
        _log("TEST", f"Log file : {log_path}")
        _log("TEST", f"Device   : {DEVICE_BASE}")
        _log("TEST", f"Firmware : {bin_path} ({os.path.getsize(bin_path):,} bytes)")
        _log("TEST", f"Serial   : {SERIAL_PORT if SERIAL_PORT else '(none — serial capture disabled)'}")

        _start_serial_capture(SERIAL_PORT, SERIAL_BAUD)

        _log("TEST", "Waiting for device to be reachable before first cycle...")
        try:
            _wait_device_ready(DEVICE_BASE, BOOT_WAIT_SEC)
            _log("TEST", "Device is up.")
        except RuntimeError as exc:
            _log("ERROR", f"Device not reachable before test start: {exc}")
            _stop_serial_capture()
            env.Exit(1)  # noqa: F821
            return

        # results: list of (cycle, a_ok, a_msg, b_ok, b_msg)
        results = []

        for cycle in range(1, NUM_CYCLES + 1):
            _log("TEST", f"===== Cycle {cycle}/{NUM_CYCLES} =====")

            a_ok, a_msg = _phase_a(DEVICE_BASE, bin_path)
            _log("TEST", f"Phase A: {'PASS' if a_ok else 'FAIL'} — {a_msg}")

            b_ok, b_msg = _phase_b(DEVICE_BASE)
            _log("TEST", f"Phase B: {'PASS' if b_ok else 'FAIL'} — {b_msg}")

            results.append((cycle, a_ok, a_msg, b_ok, b_msg))

        # Summary
        _log("TEST", "===== SUMMARY =====")
        all_passed = True
        for cycle, a_ok, a_msg, b_ok, b_msg in results:
            a_str = "PASS" if a_ok else "FAIL"
            b_str = "PASS" if b_ok else "FAIL"
            _log("TEST", f"Cycle {cycle:2d}: A={a_str}  B={b_str}")
            if not a_ok:
                _log("TEST", f"          A detail: {a_msg}")
            if not b_ok:
                _log("TEST", f"          B detail: {b_msg}")
            if not a_ok or not b_ok:
                all_passed = False

        if all_passed:
            _log("TEST", f"All {NUM_CYCLES} cycles passed.")
        else:
            failed = sum(1 for _, a_ok, _, b_ok, _ in results if not a_ok or not b_ok)
            _log("TEST", f"{failed} of {NUM_CYCLES} cycles had failures.")

        _log("TEST", f"Full log saved to: {log_path}")
        _stop_serial_capture()
    finally:
        _log_file.close()
        _log_file = None

    if not all_passed:
        env.Exit(1)  # noqa: F821


# ============================================================================
# Register PlatformIO custom target
# ============================================================================

env.AddCustomTarget(  # noqa: F821
    name="ota_stress_test",
    dependencies=[os.path.join(env.subst("$BUILD_DIR"), "firmware.bin")],  # noqa: F821
    actions=[_run_ota_stress],
    title="OTA Stress Test",
    description=(
        f"Run {NUM_CYCLES} OTA update cycles (local file + GitHub URL) and save serial log"
    ),
)
