Import("env")

import json
import os
import time
from urllib.error import URLError, HTTPError
from urllib.request import urlopen


def _get_base_url():
    override = os.getenv("LED_CLOCK_TEST_BASE_URL", "").strip()
    if override:
        return override.rstrip("/")

    host = str(env.get("UPLOAD_PORT", "")).strip()
    if host.startswith("http://") or host.startswith("https://"):
        return host.rstrip("/")
    return f"http://{host}"


def _get_json(url: str, timeout: float = 6.0):
    with urlopen(url, timeout=timeout) as response:
        raw = response.read().decode("utf-8", errors="replace")
        return json.loads(raw)


def _get_json_retry(url: str, attempts: int = 5, timeout: float = 6.0, delay_sec: float = 1.2):
    last_error = None
    for _ in range(attempts):
        try:
            return _get_json(url, timeout=timeout)
        except Exception as exc:
            last_error = exc
            time.sleep(delay_sec)
    raise last_error


def _wait_device_ready(base: str, max_wait_sec: int = 45):
    start = time.time()
    while time.time() - start < max_wait_sec:
        try:
            status = _get_json(f"{base}/api/status", timeout=4.0)
            if isinstance(status, dict):
                return
        except Exception:
            pass
        time.sleep(1.5)
    raise RuntimeError("device not reachable after upload reboot")


def _assert(condition: bool, message: str):
    if not condition:
        raise RuntimeError(message)


def _build_mode_config_url(base: str, mode: int, cfg: dict, persist: bool) -> str:
    """Reconstruct a mode/config set URL from a previously fetched config dict."""
    h = cfg.get("hour",   {})
    m = cfg.get("minute", {})
    s = cfg.get("second", {})
    w = cfg.get("width",  {})
    sp = cfg.get("spectrum", 0)
    p = 1 if persist else 0
    return (
        f"{base}/api/mode/config?set=1&persist={p}&mode={mode}"
        f"&hr={h.get('r',0)}&hg={h.get('g',0)}&hb={h.get('b',0)}"
        f"&mr={m.get('r',0)}&mg={m.get('g',0)}&mb={m.get('b',0)}"
        f"&sr={s.get('r',0)}&sg={s.get('g',0)}&sb={s.get('b',0)}"
        f"&hw={w.get('hour',1)}&mw={w.get('minute',1)}&sw={w.get('second',1)}"
        f"&sp={sp}"
    )


def run_smoke_test():
    base = _get_base_url()
    print(f"[post-test] Running settings smoke tests on {base}")

    _wait_device_ready(base)

    # ── Snapshot current state so we can restore it afterwards ──────────────
    status = _get_json_retry(f"{base}/api/status")
    _assert("display_mode" in status, "status missing display_mode")
    original_mode = status.get("display_mode", 0)

    # Snapshot mode=1 config (the mode the test will touch)
    original_mode1_cfg = _get_json_retry(f"{base}/api/mode/config?mode=1")

    try:
        # ── Smoke tests ─────────────────────────────────────────────────────

        # Switch to mode 1 for the config tests
        display = _get_json_retry(f"{base}/api/display?mode=1")
        _assert(display.get("new_mode") == 1, "failed to set mode=1")

        # Volatile (non-persistent) config apply
        volatile = _get_json_retry(
            f"{base}/api/mode/config?set=1&persist=0&mode=1"
            "&hr=11&hg=22&hb=33&mr=44&mg=55&mb=66&sr=77&sg=88&sb=99"
            "&hw=5&mw=3&sw=7&sp=2"
        )
        _assert(volatile.get("ok") is True, "volatile apply failed")

        check1 = _get_json_retry(f"{base}/api/mode/config?mode=1")
        _assert(check1.get("hour", {}).get("r") == 11, "volatile hour.r mismatch")
        _assert(check1.get("width", {}).get("second") == 7, "volatile sw mismatch")

        # Persistent config apply (written to EEPROM — will be restored in finally)
        persisted = _get_json_retry(
            f"{base}/api/mode/config?set=1&persist=1&mode=1"
            "&hr=101&hg=102&hb=103&mr=104&mg=105&mb=106&sr=107&sg=108&sb=109"
            "&hw=9&mw=7&sw=11&sp=1"
        )
        _assert(persisted.get("ok") is True, "persistent apply failed")
        _assert(persisted.get("persisted") is True, "persistent apply not marked persisted")

        check2 = _get_json_retry(f"{base}/api/mode/config?mode=1")
        _assert(check2.get("hour", {}).get("r") == 101, "persisted hour.r mismatch")
        _assert(check2.get("width", {}).get("hour") == 9, "persisted hw mismatch")

        # Reset to defaults (written to EEPROM — will be restored in finally)
        reset = _get_json_retry(f"{base}/api/mode/config?reset=1&persist=1&mode=1")
        _assert(reset.get("ok") is True and reset.get("reset") is True, "reset failed")

        print("[post-test] PASS: settings endpoints working")

    finally:
        # ── Always restore original settings ────────────────────────────────
        try:
            restore_url = _build_mode_config_url(base, 1, original_mode1_cfg, persist=True)
            _get_json_retry(restore_url)
        except Exception as e:
            print(f"[post-test] WARNING: could not restore mode 1 config: {e}")

        try:
            _get_json_retry(f"{base}/api/display?mode={original_mode}")
        except Exception as e:
            print(f"[post-test] WARNING: could not restore display mode: {e}")


def _after_upload(*args, **kwargs):
    env_obj = kwargs.get("env", env)
    try:
        run_smoke_test()
    except (URLError, HTTPError, TimeoutError, RuntimeError, ValueError) as exc:
        print(f"[post-test] FAIL: {exc}")
        env_obj.Exit(1)


if not os.getenv("CI"):
    env.AddPostAction("upload", _after_upload)
