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


def run_smoke_test():
    base = _get_base_url()
    print(f"[post-test] Running settings smoke tests on {base}")

    _wait_device_ready(base)

    status = _get_json_retry(f"{base}/api/status")
    _assert("display_mode" in status, "status missing display_mode")

    display = _get_json_retry(f"{base}/api/display?mode=1")
    _assert(display.get("new_mode") == 1, "failed to set mode=1")

    volatile = _get_json_retry(
        f"{base}/api/mode/config?set=1&persist=0&mode=1"
        "&hr=11&hg=22&hb=33&mr=44&mg=55&mb=66&sr=77&sg=88&sb=99"
        "&hw=5&mw=3&sw=7&sp=2"
    )
    _assert(volatile.get("ok") is True, "volatile apply failed")

    check1 = _get_json_retry(f"{base}/api/mode/config?mode=1")
    _assert(check1.get("hour", {}).get("r") == 11, "volatile hour.r mismatch")
    _assert(check1.get("width", {}).get("second") == 7, "volatile sw mismatch")

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

    reset = _get_json_retry(f"{base}/api/mode/config?reset=1&persist=1&mode=1")
    _assert(reset.get("ok") is True and reset.get("reset") is True, "reset failed")

    _ = _get_json_retry(f"{base}/api/mode/config?mode=1")

    print("[post-test] PASS: settings endpoints working")


def _after_upload(*args, **kwargs):
    env_obj = kwargs.get("env", env)
    try:
        run_smoke_test()
    except (URLError, HTTPError, TimeoutError, RuntimeError, ValueError) as exc:
        print(f"[post-test] FAIL: {exc}")
        env_obj.Exit(1)


env.AddPostAction("upload", _after_upload)
