from __future__ import annotations

import asyncio
import contextlib
import hashlib as _hashlib
import io
import json as _json
import os as _os
import platform
import re as _re
import secrets as _secrets
import shutil as _shutil
import subprocess as _subprocess
import sys
import threading as _threading
import time
import traceback as _traceback
import uuid as _uuid
from collections import deque
from typing import Any
from urllib.parse import urlparse as _urlparse

from playwright.async_api import Page, BrowserContext

MAX_LOG_SIZE = 2000
MAX_BODY_SIZE = 200_000
DEFAULT_WINDOW_SIZE = (1280, 900)
MIN_WINDOW_SIZE = (900, 640)
WINDOW_WORK_AREA_MARGIN = 80
PROFILE_CLEANUP_MAX_AGE_SEC = 3600
PROFILE_CLEANUP_MAX_REMOVALS = 32
PRIVACY_VERIFY_URL = "data:text/html,%3C!doctype%20html%3E%3Ctitle%3EAiDA%20Camoufox%3C/title%3E"
AIDA_CAMOUFOX_BRIDGE_PATCH_ID = "aida_camoufox_bridge_20260620_crash_diag_1"
AIDA_DEFAULT_ADDON_POLICY_MARKER = "aida_default_addon_policy_v1"
AIDA_FAST_VISIBLE_POLICY_MARKER = "aida_fast_visible_policy_v1"
AIDA_CONTEXT_VIEWPORT_SANITIZER_MARKER = "aida_context_viewport_sanitizer_v1"
AIDA_VISIBLE_READINESS_MAX_MS = 40000
AIDA_LAUNCH_BUDGET_POLICY_MARKER = "aida_launch_budget_policy_v1"
AIDA_LAUNCH_MAX_TIMEOUT_MS = AIDA_VISIBLE_READINESS_MAX_MS
AIDA_LAUNCH_FLOOR_MS = 5000
AIDA_LAUNCH_DEFAULT_BUNDLED_VISIBLE_MS = AIDA_LAUNCH_MAX_TIMEOUT_MS
AIDA_LAUNCH_DEFAULT_FAST_PROBE_MS = AIDA_LAUNCH_MAX_TIMEOUT_MS
AIDA_LAUNCH_DEFAULT_NORMAL_MS = 30000
AIDA_NAVIGATION_MIN_TIMEOUT_MS = 1000
AIDA_NAVIGATION_DEFAULT_TIMEOUT_MS = 30000
AIDA_NAVIGATION_MAX_TIMEOUT_MS = 120000
AIDA_LAUNCH_PHASE_POLICY = {
    "fast_probe": {
        "context_enter_floor_s": 8.0,
        "context_enter_ratio": 0.55,
        "context_create_floor_s": 5.0,
        "context_create_ratio": 0.20,
        "context_create_cap_s": 8.0,
        "page_create_floor_s": 5.0,
        "page_create_ratio": 0.18,
        "page_create_cap_s": 8.0,
        "late_page_wait_floor_s": 0.50,
        "late_page_wait_ratio": 0.025,
        "late_page_wait_cap_s": 1.0,
        "persistent_page_wait_floor_s": 1.0,
        "persistent_page_wait_ratio": 0.12,
        "persistent_page_wait_cap_s": 3.0,
        "persistent_page_create_floor_s": 2.0,
        "persistent_page_create_ratio": 0.25,
        "persistent_page_create_cap_s": 5.0,
        "persistent_page_nav_floor_s": 2.0,
        "persistent_page_nav_ratio": 0.25,
        "persistent_page_nav_cap_s": 5.0,
    },
    "bundled_visible": {
        "context_enter_floor_s": 20.0,
        "context_enter_ratio": 0.55,
        "context_create_floor_s": 6.0,
        "context_create_ratio": 0.20,
        "context_create_cap_s": 10.0,
        "page_create_floor_s": 8.0,
        "page_create_ratio": 0.30,
        "page_create_cap_s": 12.0,
        "late_page_wait_floor_s": 0.75,
        "late_page_wait_ratio": 0.05,
        "late_page_wait_cap_s": 2.0,
        "persistent_page_wait_floor_s": 1.0,
        "persistent_page_wait_ratio": 0.12,
        "persistent_page_wait_cap_s": 3.0,
        "persistent_page_create_floor_s": 2.0,
        "persistent_page_create_ratio": 0.25,
        "persistent_page_create_cap_s": 5.0,
        "persistent_page_nav_floor_s": 2.0,
        "persistent_page_nav_ratio": 0.25,
        "persistent_page_nav_cap_s": 5.0,
    },
    "normal": {
        "context_enter_floor_s": 20.0,
        "context_enter_ratio": 0.85,
        "context_create_floor_s": 6.0,
        "context_create_ratio": 0.20,
        "context_create_cap_s": 15.0,
        "page_create_floor_s": 14.0,
        "page_create_ratio": 0.40,
        "page_create_cap_s": 18.0,
        "late_page_wait_floor_s": 2.0,
        "late_page_wait_ratio": 0.12,
        "late_page_wait_cap_s": 5.0,
        "persistent_page_wait_floor_s": 1.0,
        "persistent_page_wait_ratio": 0.12,
        "persistent_page_wait_cap_s": 3.0,
        "persistent_page_create_floor_s": 2.0,
        "persistent_page_create_ratio": 0.25,
        "persistent_page_create_cap_s": 5.0,
        "persistent_page_nav_floor_s": 2.0,
        "persistent_page_nav_ratio": 0.25,
        "persistent_page_nav_cap_s": 5.0,
    },
}
_PROCESS_STARTED = time.perf_counter()
_PENDING_TIMEOUT_TASKS: set[asyncio.Task] = set()
_SUBPROCESS_DIAG_LOCK = _threading.Lock()
_SUBPROCESS_DIAG_RECORDS: deque[dict[str, Any]] = deque(maxlen=160)
_SUBPROCESS_DIAG_SEQ = 0
_SUBPROCESS_CAPTURE_INSTALLED = False
_ORIGINAL_CREATE_SUBPROCESS_EXEC = None
_ACTIVE_LAUNCH_MANAGER: Any = None
_LAST_ADDON_POLICY: dict[str, Any] = {}
_PROCESS_CMDLINE_FALLBACK_CACHE: dict[str, Any] = {"ts_ms": 0, "records": {}}


def _safe_text(value: Any, limit: int = 700) -> str:
    text = str(value)
    if not text and isinstance(value, BaseException):
        text = type(value).__name__
    text = _re.sub(r"://([^:/@\s]+):([^@\s]+)@", r"://<redacted>:<redacted>@", text)
    text = text.replace("\r", " ").replace("\n", " ")
    if len(text) > limit:
        return text[:limit] + f"...({len(text)} chars)"
    return text


def _camoufox_debug(event_name: str = "", **fields: Any) -> None:
    safe_fields = dict(fields)
    if "event" in safe_fields:
        safe_fields["payload_event"] = safe_fields.pop("event")
    payload = {
        "event": event_name,
        "pid": _os.getpid(),
        "ppid": _os.getppid() if hasattr(_os, "getppid") else 0,
        "tid": _threading.get_ident(),
        "ts_ms": int(time.time() * 1000),
        "uptime_ms": int((time.perf_counter() - _PROCESS_STARTED) * 1000),
    }
    payload.update(safe_fields)
    try:
        line = "AIDA_CAMOUFOX " + _json.dumps(payload, sort_keys=True, separators=(",", ":"))
        stderr_mode = _os.environ.get("AIDA_CAMOUFOX_DEBUG_STDERR", "1").strip().lower()
        if stderr_mode not in {"0", "false", "no", "off"}:
            print(line, file=sys.stderr, flush=True)
        log_path = _os.environ.get("AIDA_CAMOUFOX_DEBUG_LOG", "")
        if log_path:
            with open(log_path, "a", encoding="utf-8") as fp:
                fp.write(line + "\n")
    except Exception:
        pass


def _hash_summary(value: Any) -> dict[str, Any]:
    try:
        encoded = _json.dumps(value, sort_keys=True, default=str, separators=(",", ":"))
    except Exception:
        encoded = repr(value)
    data = encoded.encode("utf-8", "replace")
    return {
        "sha256": _hashlib.sha256(data).hexdigest()[:32],
        "length": len(encoded),
    }


def _argv_hash_summary(argv: Any) -> dict[str, Any]:
    args = [str(part) for part in (argv or [])]
    flags = []
    for arg in args:
        if arg.startswith("--"):
            flags.append(arg.split("=", 1)[0])
    joined = "\0".join(args)
    exe_name = ""
    if args:
        try:
            exe_name = _os.path.basename(args[0])
        except Exception:
            exe_name = _safe_text(args[0], 120)
    return {
        "argc": len(args),
        "joined_len": len(joined),
        "sha256": _hashlib.sha256(joined.encode("utf-8", "replace")).hexdigest()[:32],
        "exe_name": _safe_text(exe_name, 180),
        "flags": sorted(set(flags))[:96],
    }


def _env_hash_summary(env: Any) -> dict[str, Any]:
    data = env if isinstance(env, dict) else {}
    keys = sorted(str(key) for key in data.keys())
    value_shape = []
    for key in keys:
        value = data.get(key)
        text = str(value)
        value_shape.append({
            "key": key,
            "type": type(value).__name__,
            "length": len(text),
            "sha256": _hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()[:16],
        })
    return {
        "key_count": len(keys),
        "keys": keys[:96],
        "keys_sha256": _hash_summary(keys)["sha256"],
        "values_shape_sha256": _hash_summary(value_shape)["sha256"],
    }


def _cmdline_string_summary(value: Any, source: str) -> dict[str, Any]:
    text = str(value or "")
    out: dict[str, Any] = {
        "cmdline_available": bool(text),
        "cmdline_source": source,
        "cmdline_argc": 0,
        "cmdline_len": len(text),
        "cmdline_sha256": "",
    }
    if text:
        out["cmdline_argc"] = max(1, text.count(" ") + 1)
        out["cmdline_sha256"] = _hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()[:32]
    return out


def _parse_wmi_creation_time(value: Any) -> int | None:
    text = str(value or "")
    if len(text) < 14:
        return None
    try:
        import datetime as _datetime
        dt = _datetime.datetime(
            int(text[0:4]),
            int(text[4:6]),
            int(text[6:8]),
            int(text[8:10]),
            int(text[10:12]),
            int(text[12:14]),
            tzinfo=_datetime.timezone.utc,
        )
        return int(dt.timestamp() * 1000)
    except Exception:
        return None


def _windows_process_cmdline_fallback(pids: list[int]) -> dict[int, dict[str, Any]]:
    wanted = sorted({int(pid) for pid in pids if int(pid) > 0})
    if not wanted or platform.system().lower() != "windows":
        return {}
    if str(_os.environ.get("AIDA_CAMOUFOX_ENABLE_CIM_CMDLINE", "")).strip().lower() not in {"1", "true", "yes", "on"}:
        return {pid: {"cmdline_available": False, "cmdline_source": "cim_disabled"} for pid in wanted}
    now = int(time.time() * 1000)
    cached = _PROCESS_CMDLINE_FALLBACK_CACHE
    cached_records = cached.get("records") if isinstance(cached.get("records"), dict) else {}
    if now - int(cached.get("ts_ms") or 0) < 2000 and all(pid in cached_records for pid in wanted):
        return {pid: dict(cached_records.get(pid) or {}) for pid in wanted}
    try:
        ids = ",".join(str(pid) for pid in wanted[:96])
        ps = (
            "$ids=@(" + ids + ");"
            "Get-CimInstance Win32_Process | Where-Object { $ids -contains [int]$_.ProcessId } | "
            "Select-Object ProcessId,CommandLine,CreationDate,ExecutablePath | ConvertTo-Json -Compress -Depth 2"
        )
        result = _subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", ps],
            capture_output=True,
            text=True,
            timeout=1,
            creationflags=0x08000000 if hasattr(_subprocess, "CREATE_NO_WINDOW") else 0,
        )
        if result.returncode != 0 or not result.stdout.strip():
            return {}
        parsed = _json.loads(result.stdout)
        rows = parsed if isinstance(parsed, list) else [parsed]
        records: dict[int, dict[str, Any]] = {}
        for row in rows:
            if not isinstance(row, dict):
                continue
            try:
                pid = int(row.get("ProcessId") or 0)
            except Exception:
                continue
            if pid <= 0:
                continue
            summary = _cmdline_string_summary(row.get("CommandLine"), "cim")
            created_ms = _parse_wmi_creation_time(row.get("CreationDate"))
            if created_ms is not None:
                summary["create_time_ms"] = created_ms
            exe_path = str(row.get("ExecutablePath") or "")
            if exe_path:
                summary["exe_path_sha256"] = _hashlib.sha256(exe_path.encode("utf-8", "replace")).hexdigest()[:32]
                summary["exe_path_len"] = len(exe_path)
            records[pid] = summary
        _PROCESS_CMDLINE_FALLBACK_CACHE["ts_ms"] = now
        _PROCESS_CMDLINE_FALLBACK_CACHE["records"] = records
        return {pid: dict(records.get(pid) or {}) for pid in wanted}
    except Exception as exc:
        _PROCESS_CMDLINE_FALLBACK_CACHE["ts_ms"] = now
        _PROCESS_CMDLINE_FALLBACK_CACHE["records"] = {}
        return {pid: {"cmdline_available": False, "cmdline_source": "cim", "cmdline_error_type": type(exc).__name__, "cmdline_error": _safe_text(exc, 180)} for pid in wanted}


def _driver_stderr_path(seq: int, pid: int | None = None) -> str:
    root = _os.environ.get("AIDA_CAMOUFOX_DRIVER_LOG_ROOT") or _os.path.dirname(_os.environ.get("AIDA_CAMOUFOX_DEBUG_LOG", "") or "") or _os.environ.get("TEMP") or _os.getcwd()
    try:
        _os.makedirs(root, exist_ok=True)
    except Exception:
        root = _os.getcwd()
    suffix = f"{seq:04d}"
    if pid:
        suffix += f"_{int(pid)}"
    return _os.path.join(root, f"aida_playwright_driver_{suffix}.stderr.log")


def _file_tail(path: str, limit: int = 6000) -> dict[str, Any]:
    if not path:
        return {"path": "", "exists": False, "size": 0, "tail": ""}
    try:
        size = _os.path.getsize(path)
        with open(path, "rb") as fp:
            if size > limit:
                fp.seek(max(0, size - limit))
            data = fp.read(limit)
        return {
            "path": path,
            "exists": True,
            "size": int(size),
            "tail": data.decode("utf-8", "replace"),
        }
    except Exception as exc:
        return {"path": path, "exists": False, "size": 0, "tail": "", "error_type": type(exc).__name__, "error": _safe_text(exc, 180)}


def _subprocess_record_update(record_id: int, **fields: Any) -> None:
    with _SUBPROCESS_DIAG_LOCK:
        for record in _SUBPROCESS_DIAG_RECORDS:
            if int(record.get("id") or 0) == int(record_id):
                record.update(fields)
                return


async def _watch_subprocess_exit(proc: Any, record_id: int, started_perf: float, stderr_handle: Any = None, stderr_path: str = "") -> None:
    try:
        return_code = await proc.wait()
        exited_ms = int(time.time() * 1000)
        update = {
            "exited": True,
            "exit_code": int(return_code) if return_code is not None else None,
            "exit_ts_ms": exited_ms,
            "exit_elapsed_ms": int((time.perf_counter() - started_perf) * 1000),
            "process_after_exit": _process_tree_detailed_snapshot(),
        }
        if stderr_path:
            update["stderr_capture"] = _file_tail(stderr_path, 6000)
        _subprocess_record_update(record_id, **update)
        _camoufox_debug("subprocess_exit", record_id=record_id, **update)
        if return_code not in (None, 0):
            manager = _ACTIVE_LAUNCH_MANAGER
            try:
                if manager is not None:
                    manager._mark_launch_terminal(
                        f"subprocess_exit_{int(return_code)}",
                        "subprocess_exit",
                        record_id=record_id,
                        exit_code=int(return_code),
                        exit_elapsed_ms=update["exit_elapsed_ms"],
                        stderr_capture=update.get("stderr_capture", ""),
                        process_after_exit=update.get("process_after_exit", {}),
                    )
            except Exception:
                pass
    except Exception as exc:
        _subprocess_record_update(record_id, exit_watch_error_type=type(exc).__name__, exit_watch_error=_safe_text(exc, 300))
    finally:
        if stderr_handle is not None:
            with contextlib.suppress(Exception):
                stderr_handle.close()


async def _aida_create_subprocess_exec(*cmd: Any, **kwargs: Any) -> Any:
    original = _ORIGINAL_CREATE_SUBPROCESS_EXEC
    if original is None:
        return await asyncio.create_subprocess_exec(*cmd, **kwargs)
    cmd_text = [str(part) for part in cmd]
    driver_like = any(part == "run-driver" for part in cmd_text) or any("playwright" in part.lower() for part in cmd_text[:2])
    if not driver_like:
        return await original(*cmd, **kwargs)
    global _SUBPROCESS_DIAG_SEQ
    _SUBPROCESS_DIAG_SEQ += 1
    record_id = _SUBPROCESS_DIAG_SEQ
    started_perf = time.perf_counter()
    started_ms = int(time.time() * 1000)
    local_kwargs = dict(kwargs)
    stderr_handle = None
    stderr_path = ""
    stderr_setting = local_kwargs.get("stderr")
    stderr_capture_enabled = _os.environ.get("AIDA_CAMOUFOX_CAPTURE_DRIVER_STDERR", "1").strip().lower() not in {"0", "false", "no", "off"}
    if stderr_capture_enabled and stderr_setting is not asyncio.subprocess.PIPE:
        stderr_path = _driver_stderr_path(record_id)
        try:
            stderr_handle = open(stderr_path, "ab", buffering=0)
            local_kwargs["stderr"] = stderr_handle
        except Exception as exc:
            stderr_handle = None
            stderr_path = ""
            _camoufox_debug("subprocess_stderr_capture_open_failed", record_id=record_id, error_type=type(exc).__name__, error_summary=_safe_text(exc, 300))
    record = {
        "id": record_id,
        "kind": "playwright_driver",
        "driver_like": driver_like,
        "started_ms": started_ms,
        "pid": 0,
        "argv": _argv_hash_summary(cmd_text),
        "env": _env_hash_summary(local_kwargs.get("env")),
        "cwd": _safe_text(local_kwargs.get("cwd") or _os.getcwd(), 700),
        "stdout_pipe": local_kwargs.get("stdout") is asyncio.subprocess.PIPE,
        "stdout_capture": "protocol_pipe_not_consumed" if local_kwargs.get("stdout") is asyncio.subprocess.PIPE else "not_pipe",
        "stderr_pipe": local_kwargs.get("stderr") is asyncio.subprocess.PIPE,
        "stderr_capture_path": stderr_path,
        "exited": False,
        "exit_code": None,
        "exit_ts_ms": None,
    }
    with _SUBPROCESS_DIAG_LOCK:
        _SUBPROCESS_DIAG_RECORDS.append(record)
    _camoufox_debug("subprocess_spawn_begin", record_id=record_id, record=record)
    try:
        proc = await original(*cmd, **local_kwargs)
    except Exception as exc:
        if stderr_handle is not None:
            with contextlib.suppress(Exception):
                stderr_handle.close()
        _subprocess_record_update(
            record_id,
            spawn_error_type=type(exc).__name__,
            spawn_error=_safe_text(exc, 500),
            exited=True,
            exit_ts_ms=int(time.time() * 1000),
        )
        _camoufox_debug("subprocess_spawn_failed", record_id=record_id, error_type=type(exc).__name__, error_summary=_safe_text(exc, 500))
        raise
    pid = int(getattr(proc, "pid", 0) or 0)
    _subprocess_record_update(record_id, pid=pid, process_after_spawn=_process_tree_detailed_snapshot())
    if stderr_path and pid:
        renamed = _driver_stderr_path(record_id, pid)
        if renamed != stderr_path:
            with contextlib.suppress(Exception):
                if _os.path.exists(stderr_path):
                    _os.replace(stderr_path, renamed)
                    stderr_path = renamed
                    _subprocess_record_update(record_id, stderr_capture_path=stderr_path)
    _camoufox_debug("subprocess_spawn_ok", record_id=record_id, pid=pid, process_tree=_process_tree_detailed_snapshot())
    try:
        task = asyncio.create_task(_watch_subprocess_exit(proc, record_id, started_perf, stderr_handle, stderr_path))
        _track_timeout_task(task)
    except Exception as exc:
        _subprocess_record_update(record_id, exit_watch_error_type=type(exc).__name__, exit_watch_error=_safe_text(exc, 300))
    return proc


def _install_subprocess_diagnostics() -> None:
    global _SUBPROCESS_CAPTURE_INSTALLED, _ORIGINAL_CREATE_SUBPROCESS_EXEC
    if _SUBPROCESS_CAPTURE_INSTALLED:
        return
    _ORIGINAL_CREATE_SUBPROCESS_EXEC = asyncio.create_subprocess_exec
    asyncio.create_subprocess_exec = _aida_create_subprocess_exec
    _SUBPROCESS_CAPTURE_INSTALLED = True
    _camoufox_debug("subprocess_diagnostics_installed")


def _subprocess_diagnostics_snapshot() -> dict[str, Any]:
    with _SUBPROCESS_DIAG_LOCK:
        records = [dict(record) for record in list(_SUBPROCESS_DIAG_RECORDS)]
    for record in records:
        path = str(record.get("stderr_capture_path") or "")
        if path:
            record["stderr_capture"] = _file_tail(path, 6000)
    driver_records = [record for record in records if record.get("driver_like")]
    latest = driver_records[-1] if driver_records else {}
    return {
        "installed": _SUBPROCESS_CAPTURE_INSTALLED,
        "record_count": len(records),
        "driver_record_count": len(driver_records),
        "latest_driver": latest,
        "records": records[-24:],
    }


_install_subprocess_diagnostics()


def _track_timeout_task(task: asyncio.Task) -> None:
    _PENDING_TIMEOUT_TASKS.add(task)

    def _done(done_task: asyncio.Task) -> None:
        _PENDING_TIMEOUT_TASKS.discard(done_task)
        try:
            done_task.result()
        except BaseException:
            pass

    task.add_done_callback(_done)


async def _await_no_cancel_wait(awaitable: Any, timeout: float) -> Any:
    task = asyncio.create_task(awaitable)
    done, _ = await asyncio.wait({task}, timeout=timeout)
    if task in done:
        return task.result()
    task.cancel()
    _track_timeout_task(task)
    raise asyncio.TimeoutError


async def _await_late_result(awaitable: Any, timeout: float) -> tuple[Any | None, asyncio.Task | None]:
    task = asyncio.create_task(awaitable)
    done, _ = await asyncio.wait({task}, timeout=timeout)
    if task in done:
        return task.result(), None
    _track_timeout_task(task)
    return None, task


async def _await_page_result(awaitable: Any, timeout: float) -> tuple[Any | None, asyncio.Task, bool]:
    task = asyncio.create_task(awaitable)
    done, _ = await asyncio.wait({task}, timeout=timeout)
    if task in done:
        return task.result(), task, False
    _track_timeout_task(task)
    return None, task, True


def _abandon_late_task(task: asyncio.Task | None) -> None:
    if task is None:
        return
    if not task.done():
        task.cancel()
    _track_timeout_task(task)


def _int_config(value: Any, fallback: int) -> int:
    if isinstance(value, bool):
        return fallback
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return fallback
    return parsed if parsed > 0 else fallback


def _flag_enabled(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return False
    return str(value).strip().lower() in {"1", "true", "yes", "on", "enable", "enabled"}


def _aida_launch_mode(bundled_visible_launch: bool, fast_probe: bool) -> str:
    if fast_probe:
        return "fast_probe"
    if bundled_visible_launch:
        return "bundled_visible"
    return "normal"


def _aida_default_launch_timeout_ms(mode: str) -> int:
    if mode == "fast_probe":
        return AIDA_LAUNCH_DEFAULT_FAST_PROBE_MS
    if mode == "bundled_visible":
        return AIDA_LAUNCH_DEFAULT_BUNDLED_VISIBLE_MS
    return AIDA_LAUNCH_DEFAULT_NORMAL_MS


def _aida_clamp_int(value: Any, fallback: int, floor: int, ceiling: int) -> int:
    parsed = _int_config(value, fallback)
    return min(max(parsed, floor), ceiling)


def _aida_phase_budget_seconds(launch_timeout_s: float, floor_s: float, ratio: float, cap_s: float | None = None) -> float:
    timeout_s = max(0.0, float(launch_timeout_s))
    candidate = max(float(floor_s), timeout_s * float(ratio))
    if cap_s is not None:
        candidate = min(candidate, float(cap_s))
    return max(0.0, min(candidate, timeout_s))


def aida_launch_budget_policy_snapshot() -> dict[str, Any]:
    return {
        "marker": AIDA_LAUNCH_BUDGET_POLICY_MARKER,
        "floor_ms": AIDA_LAUNCH_FLOOR_MS,
        "max_ms": AIDA_LAUNCH_MAX_TIMEOUT_MS,
        "defaults_ms": {
            "bundled_visible": AIDA_LAUNCH_DEFAULT_BUNDLED_VISIBLE_MS,
            "fast_probe": AIDA_LAUNCH_DEFAULT_FAST_PROBE_MS,
            "normal": AIDA_LAUNCH_DEFAULT_NORMAL_MS,
        },
        "phase_policy": {name: dict(values) for name, values in AIDA_LAUNCH_PHASE_POLICY.items()},
        "navigation_timeout_ms": {
            "floor": AIDA_NAVIGATION_MIN_TIMEOUT_MS,
            "default": AIDA_NAVIGATION_DEFAULT_TIMEOUT_MS,
            "max": AIDA_NAVIGATION_MAX_TIMEOUT_MS,
        },
    }


def aida_resolve_launch_budget_policy(requested_timeout_ms: Any = None, *, bundled_visible_launch: bool = False, fast_probe: bool = False) -> dict[str, Any]:
    mode = _aida_launch_mode(bool(bundled_visible_launch), bool(fast_probe))
    mode_policy = dict(AIDA_LAUNCH_PHASE_POLICY[mode])
    default_ms = _aida_default_launch_timeout_ms(mode)
    launch_timeout_ms = _aida_clamp_int(requested_timeout_ms, default_ms, AIDA_LAUNCH_FLOOR_MS, AIDA_LAUNCH_MAX_TIMEOUT_MS)
    launch_timeout_s = max(AIDA_LAUNCH_FLOOR_MS / 1000.0, launch_timeout_ms / 1000.0)
    phase_budgets_s = {
        "context_enter_timeout_s": _aida_phase_budget_seconds(
            launch_timeout_s,
            mode_policy["context_enter_floor_s"],
            mode_policy["context_enter_ratio"],
            mode_policy.get("context_enter_cap_s"),
        ),
        "context_create_timeout_s": _aida_phase_budget_seconds(
            launch_timeout_s,
            mode_policy["context_create_floor_s"],
            mode_policy["context_create_ratio"],
            mode_policy.get("context_create_cap_s"),
        ),
        "page_create_timeout_s": _aida_phase_budget_seconds(
            launch_timeout_s,
            mode_policy["page_create_floor_s"],
            mode_policy["page_create_ratio"],
            mode_policy.get("page_create_cap_s"),
        ),
        "late_page_wait_s": _aida_phase_budget_seconds(
            launch_timeout_s,
            mode_policy["late_page_wait_floor_s"],
            mode_policy["late_page_wait_ratio"],
            mode_policy.get("late_page_wait_cap_s"),
        ),
        "persistent_page_wait_s": _aida_phase_budget_seconds(
            launch_timeout_s,
            mode_policy["persistent_page_wait_floor_s"],
            mode_policy["persistent_page_wait_ratio"],
            mode_policy.get("persistent_page_wait_cap_s"),
        ),
        "persistent_page_create_timeout_s": _aida_phase_budget_seconds(
            launch_timeout_s,
            mode_policy["persistent_page_create_floor_s"],
            mode_policy["persistent_page_create_ratio"],
            mode_policy.get("persistent_page_create_cap_s"),
        ),
        "persistent_page_nav_timeout_s": _aida_phase_budget_seconds(
            launch_timeout_s,
            mode_policy["persistent_page_nav_floor_s"],
            mode_policy["persistent_page_nav_ratio"],
            mode_policy.get("persistent_page_nav_cap_s"),
        ),
    }
    return {
        "marker": AIDA_LAUNCH_BUDGET_POLICY_MARKER,
        "mode": mode,
        "floor_ms": AIDA_LAUNCH_FLOOR_MS,
        "max_ms": AIDA_LAUNCH_MAX_TIMEOUT_MS,
        "default_ms": default_ms,
        "requested_timeout_ms": requested_timeout_ms,
        "launch_timeout_ms": launch_timeout_ms,
        "launch_timeout_s": launch_timeout_s,
        "bundled_visible_launch": bool(bundled_visible_launch),
        "fast_probe": bool(fast_probe),
        "phase_policy": mode_policy,
        "phase_budgets_s": phase_budgets_s,
        **phase_budgets_s,
    }


def aida_retry_launch_timeout_ms(original_timeout_ms: Any, remaining_global_ms: Any, reserve_ms: int = 500) -> int:
    original_ms = _aida_clamp_int(original_timeout_ms, AIDA_LAUNCH_MAX_TIMEOUT_MS, AIDA_LAUNCH_FLOOR_MS, AIDA_LAUNCH_MAX_TIMEOUT_MS)
    try:
        remaining_ms = int(remaining_global_ms)
    except (TypeError, ValueError):
        return 0
    retry_ms = max(0, remaining_ms - max(0, int(reserve_ms)))
    if retry_ms < AIDA_LAUNCH_FLOOR_MS:
        return 0
    return min(original_ms, retry_ms, AIDA_LAUNCH_MAX_TIMEOUT_MS)


def aida_clamp_navigation_timeout_ms(timeout_ms: Any, default_ms: int = AIDA_NAVIGATION_DEFAULT_TIMEOUT_MS) -> int:
    return _aida_clamp_int(timeout_ms, default_ms, AIDA_NAVIGATION_MIN_TIMEOUT_MS, AIDA_NAVIGATION_MAX_TIMEOUT_MS)


def aida_validate_launch_budget_policy() -> dict[str, Any]:
    failures: list[str] = []
    invariants: dict[str, bool] = {}

    def check(name: str, condition: bool) -> None:
        invariants[name] = bool(condition)
        if not condition:
            failures.append(name)

    snapshot = aida_launch_budget_policy_snapshot()
    defaults = snapshot["defaults_ms"]
    check("policy_marker_present", snapshot["marker"] == AIDA_LAUNCH_BUDGET_POLICY_MARKER)
    check("policy_max_not_above_40000", int(snapshot["max_ms"]) <= 40000)
    check("policy_floor_positive", int(snapshot["floor_ms"]) > 0)
    for name, value in defaults.items():
        check(f"default_{name}_within_bounds", int(snapshot["floor_ms"]) <= int(value) <= int(snapshot["max_ms"]))

    oversized = aida_resolve_launch_budget_policy(AIDA_LAUNCH_MAX_TIMEOUT_MS * 4, bundled_visible_launch=True)
    undersized = aida_resolve_launch_budget_policy(1, bundled_visible_launch=True)
    check("requested_timeout_clamps_to_policy_max", oversized["launch_timeout_ms"] == AIDA_LAUNCH_MAX_TIMEOUT_MS)
    check("requested_timeout_clamps_to_policy_floor", undersized["launch_timeout_ms"] == AIDA_LAUNCH_FLOOR_MS)

    resolved_cases = {
        "fast_probe": aida_resolve_launch_budget_policy(None, fast_probe=True),
        "bundled_visible": aida_resolve_launch_budget_policy(None, bundled_visible_launch=True),
        "normal": aida_resolve_launch_budget_policy(None),
        "oversized": oversized,
        "undersized": undersized,
    }
    for name, policy in resolved_cases.items():
        launch_timeout_s = float(policy["launch_timeout_s"])
        budgets = dict(policy["phase_budgets_s"])
        check(f"{name}_phase_budgets_nonnegative", all(float(value) >= 0.0 for value in budgets.values()))
        check(f"{name}_phase_budgets_within_launch_timeout", all(float(value) <= launch_timeout_s for value in budgets.values()))

    bundled_page_budget = float(resolved_cases["bundled_visible"]["page_create_timeout_s"])
    normal_page_budget = float(resolved_cases["normal"]["page_create_timeout_s"])
    check("bundled_visible_page_budget_lower_than_normal", bundled_page_budget < normal_page_budget)

    retry_full = aida_retry_launch_timeout_ms(AIDA_LAUNCH_MAX_TIMEOUT_MS, AIDA_LAUNCH_MAX_TIMEOUT_MS * 3)
    retry_remaining = aida_retry_launch_timeout_ms(AIDA_LAUNCH_MAX_TIMEOUT_MS, 16000)
    retry_too_small = aida_retry_launch_timeout_ms(AIDA_LAUNCH_MAX_TIMEOUT_MS, AIDA_LAUNCH_FLOOR_MS - 1)
    check("retry_never_exceeds_original_timeout", 0 <= retry_full <= AIDA_LAUNCH_MAX_TIMEOUT_MS)
    check("retry_never_exceeds_remaining_budget", 0 <= retry_remaining <= 15500)
    check("retry_rejects_budget_below_floor", retry_too_small == 0)

    return {
        "marker": AIDA_LAUNCH_BUDGET_POLICY_MARKER,
        "ok": not failures,
        "failures": failures,
        "invariants": invariants,
        "snapshot": snapshot,
        "resolved_cases": resolved_cases,
        "retry_samples": {
            "full": retry_full,
            "remaining": retry_remaining,
            "too_small": retry_too_small,
        },
    }


def _target_closed_error(exc: Exception) -> bool:
    name = type(exc).__name__
    text = str(exc)
    return "TargetClosed" in name or "target closed" in text.lower() or "has been closed" in text.lower()


def _protocol_schema_viewport_error(exc: Any) -> bool:
    text = str(exc or "").lower()
    if "browser.setdefaultviewport" not in text and "page.setviewportsize" not in text:
        return False
    schema_markers = (
        "<root>.viewport",
        "<root>.screensize",
        "screensize",
        "viewport.ismobile",
        "viewport.devicescalefactor",
        "viewport.device_scale_factor",
        "found property",
        "not described in this scheme",
    )
    return any(marker in text for marker in schema_markers)


def _page_error_kind(exc: Exception | str | None) -> str:
    if _protocol_schema_viewport_error(exc):
        return "protocol_schema_viewport"
    text = str(exc or "").lower()
    name = type(exc).__name__ if isinstance(exc, BaseException) else ""
    if "page crashed" in text or "page crash" in text:
        return "page_crash"
    if "browser has been closed" in text or "browser closed" in text or "browser disconnected" in text:
        return "browser_closed"
    if "context has been closed" in text or "context closed" in text:
        return "context_closed"
    if "target page, context or browser has been closed" in text or "target closed" in text or "has been closed" in text or "TargetClosed" in name:
        return "target_closed"
    if "timeout" in text or "exceeded" in text or "waiting" in text:
        return "timeout"
    return "error"


def _launch_error_kind(exc: Exception, browser_connected: bool | None = None) -> str:
    text = str(exc or "").lower()
    if _protocol_schema_viewport_error(exc):
        return "protocol_schema_viewport"
    if isinstance(exc, asyncio.TimeoutError) or type(exc).__name__ == "TimeoutError":
        return "timeout"
    if "launch terminal event" in text:
        if "browser_disconnected" in text or "subprocess_exit" in text:
            return "browser_disconnected"
        if "page_closed" in text or "page closed" in text:
            return "target_closed"
    if _target_closed_error(exc):
        return "target_closed"
    if browser_connected is False or "browser disconnected" in text or "browser has been closed" in text or "browser closed" in text:
        return "browser_disconnected"
    if "camoufox privacy verification failed" in text or "privacy verification failed" in text:
        return "privacy_assertion_failed"
    return _page_error_kind(exc)


def _launch_error_summary(exc: Exception, phase: str) -> str:
    err_type = type(exc).__name__ or "Exception"
    text = _safe_text(exc, 900)
    if not text or text == err_type:
        return f"{err_type} during {phase or 'launch_browser'}"
    if phase and phase not in text:
        return f"{err_type} during {phase}: {text}"
    return text


_CONTEXT_VIEWPORT_DEVICE_KEYS = frozenset({
    "viewport",
    "viewportSize",
    "screen",
    "device_scale_factor",
    "deviceScaleFactor",
    "is_mobile",
    "isMobile",
})
_VIEWPORT_SIZE_KEYS = frozenset({"width", "height"})


def _viewport_int(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return None
    if 1 <= parsed <= 10000:
        return parsed
    return None


def _coerce_page_viewport(value: Any) -> dict[str, int] | None:
    candidate = value
    if isinstance(candidate, dict) and isinstance(candidate.get("viewportSize"), dict):
        nested = _coerce_page_viewport(candidate.get("viewportSize"))
        if nested:
            return nested
    if isinstance(candidate, dict):
        width = _viewport_int(candidate.get("width"))
        height = _viewport_int(candidate.get("height"))
        if width is not None and height is not None:
            return {"width": width, "height": height}
    if isinstance(candidate, (list, tuple)) and len(candidate) >= 2:
        width = _viewport_int(candidate[0])
        height = _viewport_int(candidate[1])
        if width is not None and height is not None:
            return {"width": width, "height": height}
    return None


def _page_viewport_from_plan(plan: dict[str, Any] | None, fallback: Any = None) -> dict[str, int] | None:
    data = plan if isinstance(plan, dict) else {}
    for key in ("page_viewport", "viewport", "window", "window_size"):
        viewport = _coerce_page_viewport(data.get(key))
        if viewport:
            return viewport
    context_options = data.get("context_options")
    if isinstance(context_options, dict):
        viewport = _coerce_page_viewport(context_options.get("viewport"))
        if viewport:
            return viewport
        viewport = _coerce_page_viewport(context_options.get("screen"))
        if viewport:
            return viewport
    return _coerce_page_viewport(fallback)


def _context_option_nested_strips(key: str, value: Any) -> list[str]:
    stripped = [key]
    if isinstance(value, dict):
        for nested_key in sorted(str(item) for item in value.keys()):
            if key == "viewport" and nested_key in _VIEWPORT_SIZE_KEYS:
                continue
            stripped.append(f"{key}.{nested_key}")
    return stripped


def _sanitize_camoufox_context_options(options: dict[str, Any] | None, fallback_viewport: Any = None) -> tuple[dict[str, Any], dict[str, int] | None, dict[str, Any]]:
    raw = dict(options or {})
    sanitized: dict[str, Any] = {}
    stripped: list[str] = []
    page_viewport = _coerce_page_viewport(fallback_viewport)
    for key, value in raw.items():
        key_text = str(key)
        if key_text in _CONTEXT_VIEWPORT_DEVICE_KEYS:
            stripped.extend(_context_option_nested_strips(key_text, value))
            if page_viewport is None and key_text in {"viewport", "viewportSize", "screen"}:
                page_viewport = _coerce_page_viewport(value)
            continue
        sanitized[key] = value
    sanitized["no_viewport"] = True
    diagnostics = {
        "sanitizer_marker": AIDA_CONTEXT_VIEWPORT_SANITIZER_MARKER,
        "original_keys": sorted(str(k) for k in raw.keys()),
        "sanitized_keys": sorted(str(k) for k in sanitized.keys()),
        "stripped_keys": sorted(set(stripped)),
        "stripped_count": len(set(stripped)),
        "viewport_policy": "context_no_viewport_page_set" if page_viewport else "context_no_viewport_no_page_size",
        "page_viewport": dict(page_viewport or {}),
        "context_no_viewport": True,
        "preserved_security_keys": sorted(str(k) for k in sanitized.keys() if str(k) in {"proxy", "service_workers", "permissions", "storage_state", "locale", "timezone_id", "geolocation", "user_agent"}),
    }
    return sanitized, page_viewport, diagnostics


async def _create_camoufox_safe_context(browser: Any, options: dict[str, Any] | None, timeout_s: float, phase: str, fallback_viewport: Any = None, started: float | None = None) -> tuple[BrowserContext, dict[str, int] | None, dict[str, Any]]:
    opts, page_viewport, diagnostics = _sanitize_camoufox_context_options(options, fallback_viewport)
    phase_started = time.perf_counter()
    _camoufox_debug(
        "context_options_sanitized",
        launch_phase=phase,
        timeout_s=timeout_s,
        elapsed_ms=int((phase_started - started) * 1000) if started else 0,
        **diagnostics,
    )
    try:
        ctx = await _await_no_cancel_wait(browser.new_context(**opts), timeout=timeout_s)
    except Exception as exc:
        _camoufox_debug(
            "context_new_context_failed",
            launch_phase=phase,
            elapsed_ms=int((time.perf_counter() - phase_started) * 1000),
            error_type=type(exc).__name__,
            error_kind=_launch_error_kind(exc, None),
            error_summary=_safe_text(exc),
            **diagnostics,
        )
        raise
    _camoufox_debug(
        "context_new_context_ok",
        launch_phase=phase,
        elapsed_ms=int((time.perf_counter() - phase_started) * 1000),
        context_page_count=_context_page_count(ctx),
        **diagnostics,
    )
    return ctx, page_viewport, diagnostics


async def _apply_page_viewport_size(page: Page | None, viewport_size: dict[str, int] | None, phase: str, timeout_s: float = 5.0, started: float | None = None) -> dict[str, Any]:
    target = _coerce_page_viewport(viewport_size)
    if page is None or not target:
        result = {
            "applied": False,
            "viewport_policy": "context_no_viewport_no_page_size",
            "target": dict(target or {}),
            "page_present": page is not None,
        }
        _camoufox_debug("page_viewport_set_skip", launch_phase=phase, **result)
        return result
    before = {}
    with contextlib.suppress(Exception):
        before = dict(page.viewport_size or {})
    phase_started = time.perf_counter()
    _camoufox_debug(
        "page_viewport_set_begin",
        launch_phase=phase,
        sanitizer_marker=AIDA_CONTEXT_VIEWPORT_SANITIZER_MARKER,
        viewport_policy="context_no_viewport_page_set",
        target=target,
        observed_before=before,
        elapsed_ms=int((phase_started - started) * 1000) if started else 0,
    )
    try:
        await _await_no_cancel_wait(page.set_viewport_size(target), timeout=min(max(timeout_s, 1.0), 8.0))
    except Exception as exc:
        error_kind = _launch_error_kind(exc, None)
        _camoufox_debug(
            "page_viewport_set_fail",
            launch_phase=phase,
            sanitizer_marker=AIDA_CONTEXT_VIEWPORT_SANITIZER_MARKER,
            viewport_policy="context_no_viewport_page_set",
            target=target,
            observed_before=before,
            elapsed_ms=int((time.perf_counter() - phase_started) * 1000),
            error_type=type(exc).__name__,
            error_kind=error_kind,
            error_summary=_safe_text(exc),
        )
        if error_kind == "protocol_schema_viewport":
            result = {
                "applied": False,
                "ignored": True,
                "ignore_reason": "protocol_schema_viewport",
                "viewport_policy": "camoufox_window_controls_page_size",
                "target": target,
                "observed_before": before,
                "elapsed_ms": int((time.perf_counter() - phase_started) * 1000),
                "error_type": type(exc).__name__,
                "error_kind": error_kind,
                "error_summary": _safe_text(exc),
            }
            _camoufox_debug("page_viewport_set_ignored", launch_phase=phase, sanitizer_marker=AIDA_CONTEXT_VIEWPORT_SANITIZER_MARKER, **result)
            return result
        raise
    observed = {}
    with contextlib.suppress(Exception):
        observed = dict(page.viewport_size or {})
    result = {
        "applied": True,
        "viewport_policy": "context_no_viewport_page_set",
        "target": target,
        "observed": observed,
        "elapsed_ms": int((time.perf_counter() - phase_started) * 1000),
    }
    _camoufox_debug("page_viewport_set_ok", launch_phase=phase, sanitizer_marker=AIDA_CONTEXT_VIEWPORT_SANITIZER_MARKER, **result)
    return result


async def _apply_plan_page_viewport_size(page: Page | None, plan: dict[str, Any] | None, phase: str, timeout_s: float = 5.0, fallback_viewport: Any = None, started: float | None = None) -> dict[str, Any]:
    return await _apply_page_viewport_size(page, _page_viewport_from_plan(plan, fallback_viewport), phase, timeout_s, started)


def _page_privacy_probe_state(page: Page | None, diagnostics: dict[str, Any] | None = None, phase: str = "") -> dict[str, Any]:
    diag = diagnostics if isinstance(diagnostics, dict) else {}
    out: dict[str, Any] = {
        "phase": phase,
        "page_id": _safe_text(diag.get("page_id") or "", 160),
        "active_page_id": _safe_text(diag.get("active_page_id") or "", 160),
        "browser_open": bool(diag.get("browser_open", False)),
        "browser_connected": bool(diag.get("browser_connected", True)),
        "registered_pages": int(diag.get("registered_pages") or 0),
        "context_page_count": int(diag.get("context_page_count") or -1),
        "recent_page_events": diag.get("recent_page_events") if isinstance(diag.get("recent_page_events"), list) else [],
        "page_present": page is not None,
        "page_closed": True,
        "page_url": "",
    }
    if page is None:
        return out
    try:
        out["page_closed"] = bool(page.is_closed())
    except Exception as exc:
        out["page_closed_error_type"] = type(exc).__name__
        out["page_closed_error"] = _safe_text(exc, 240)
    try:
        out["page_url"] = _safe_page_url(page, 240)
    except Exception as exc:
        out["page_url_error_type"] = type(exc).__name__
        out["page_url_error"] = _safe_text(exc, 240)
    try:
        out["context_page_count"] = len(getattr(page.context, "pages", []) or [])
    except Exception as exc:
        out["context_page_count_error_type"] = type(exc).__name__
        out["context_page_count_error"] = _safe_text(exc, 240)
    return out


def _work_area() -> dict[str, int]:
    if platform.system().lower() != "windows":
        return {}
    try:
        import ctypes

        class RECT(ctypes.Structure):
            _fields_ = [
                ("left", ctypes.c_long),
                ("top", ctypes.c_long),
                ("right", ctypes.c_long),
                ("bottom", ctypes.c_long),
            ]

        rect = RECT()
        if not ctypes.windll.user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(rect), 0):
            return {}
        return {
            "left": int(rect.left),
            "top": int(rect.top),
            "right": int(rect.right),
            "bottom": int(rect.bottom),
            "width": int(rect.right - rect.left),
            "height": int(rect.bottom - rect.top),
        }
    except Exception:
        return {}


def _clamp_window_dimension(value: int, preferred: int, minimum: int, available: int) -> int:
    if available > 0:
        max_value = available - WINDOW_WORK_AREA_MARGIN
        if max_value < 480:
            max_value = available
    else:
        max_value = preferred
    if max_value <= 0:
        max_value = preferred
    min_value = minimum if max_value >= minimum else max_value
    return max(min_value, min(value, max_value))


def _resolve_window_size(config: dict[str, Any]) -> tuple[tuple[int, int], dict[str, Any]]:
    work_area = _work_area()
    requested_width = _int_config(config.get("window_width"), DEFAULT_WINDOW_SIZE[0])
    requested_height = _int_config(config.get("window_height"), DEFAULT_WINDOW_SIZE[1])
    width = _clamp_window_dimension(requested_width, DEFAULT_WINDOW_SIZE[0], MIN_WINDOW_SIZE[0], work_area.get("width", 0))
    height = _clamp_window_dimension(requested_height, DEFAULT_WINDOW_SIZE[1], MIN_WINDOW_SIZE[1], work_area.get("height", 0))
    return (width, height), {
        "requested_width": requested_width,
        "requested_height": requested_height,
        "width": width,
        "height": height,
        "work_area": work_area,
    }


def detect_host_os() -> str:
    """Return the Camoufox os identifier matching the current host."""
    system = platform.system().lower()
    if system == "darwin":
        return "macos"
    if system == "linux":
        return "linux"
    return "windows"


def detect_system_locale() -> str:
    """Best-effort detection of the host's locale (e.g. 'zh-CN')."""
    for var in ("LANG", "LC_ALL", "LC_MESSAGES"):
        val = _os.environ.get(var, "")
        if val and val not in ("C", "POSIX"):
            return val.split(".")[0].replace("_", "-")
    return "en-US"


def _addon_path_text(value: Any) -> str:
    try:
        return _os.fspath(value)
    except TypeError:
        return str(value)


def _absolute_path(value: Any) -> str:
    return _os.path.abspath(_os.path.expandvars(_os.path.expanduser(_addon_path_text(value))))


def _real_norm_path(value: Any) -> str:
    return _os.path.normcase(_os.path.realpath(_absolute_path(value)))


def _append_unique_path(paths: list[str], seen: set[str], value: Any) -> None:
    if value is None:
        return
    text = _addon_path_text(value).strip()
    if not text:
        return
    try:
        resolved = _absolute_path(text)
        key = _os.path.normcase(_os.path.realpath(resolved))
    except Exception:
        return
    if key not in seen:
        seen.add(key)
        paths.append(resolved)


def _append_path_roots(paths: list[str], seen: set[str], value: Any, parent_depth: int) -> None:
    if value is None:
        return
    text = _addon_path_text(value).strip()
    if not text:
        return
    try:
        current = _absolute_path(text)
    except Exception:
        return
    if _os.path.isfile(current):
        current = _os.path.dirname(current)
    for _ in range(max(0, parent_depth)):
        _append_unique_path(paths, seen, current)
        parent = _os.path.dirname(current)
        if not parent or parent == current:
            break
        current = parent


def _approved_addon_roots(options_kwargs: dict[str, Any]) -> list[str]:
    roots: list[str] = []
    seen: set[str] = set()
    for env_name in ("AIDA_CAMOUFOX_EXECUTABLE", "AIDA_CAMOUFOX_PYTHON", "AIDA_CAMOUFOX_MCP_EXECUTABLE"):
        _append_path_roots(roots, seen, _os.environ.get(env_name), 4)
    _append_path_roots(roots, seen, options_kwargs.get("executable_path"), 4)
    _append_path_roots(roots, seen, sys.executable, 2)
    _append_path_roots(roots, seen, __file__, 4)
    _append_unique_path(roots, seen, _os.getcwd())
    deps_root = _os.path.join(_os.getcwd(), "deps")
    _append_unique_path(roots, seen, deps_root)
    extra_roots = _os.environ.get("AIDA_CAMOUFOX_ADDON_ROOTS", "")
    for entry in extra_roots.split(_os.pathsep):
        _append_unique_path(roots, seen, entry)
    return roots


def _path_under_root(path: str, root: str) -> bool:
    try:
        path_norm = _real_norm_path(path)
        root_norm = _real_norm_path(root)
        return _os.path.commonpath([path_norm, root_norm]) == root_norm
    except Exception:
        return False


def _path_under_any_root(path: str, roots: list[str]) -> bool:
    return any(_path_under_root(path, root) for root in roots)


def _addon_sequence(value: Any) -> list[Any]:
    if value is None:
        return []
    if isinstance(value, (list, tuple, set)):
        return list(value)
    return [value]


def _validate_explicit_addons(value: Any, approved_roots: list[str]) -> list[str]:
    resolved: list[str] = []
    seen: set[str] = set()
    for raw in _addon_sequence(value):
        if raw is None:
            raise ValueError("Explicit addon path is null")
        raw_text = _addon_path_text(raw).strip()
        if not raw_text:
            raise ValueError("Explicit addon path is empty")
        candidate = raw_text if _os.path.isabs(raw_text) else _os.path.join(_os.getcwd(), raw_text)
        addon_path = _absolute_path(candidate)
        if approved_roots and not _path_under_any_root(addon_path, approved_roots):
            raise PermissionError(f"Explicit addon path is outside approved roots: {addon_path}")
        if not _os.path.isdir(addon_path):
            raise FileNotFoundError(f"Explicit addon path is not a directory: {addon_path}")
        manifest = _os.path.join(addon_path, "manifest.json")
        if not _os.path.isfile(manifest):
            raise FileNotFoundError(f"manifest.json missing for explicit addon path: {addon_path}")
        key = _real_norm_path(addon_path)
        if key not in seen:
            seen.add(key)
            resolved.append(addon_path)
    return resolved


def _addon_exclusion_contains_ubo(value: Any, DefaultAddons: Any) -> bool:
    for item in _addon_sequence(value):
        if item is DefaultAddons.UBO:
            return True
        name = str(getattr(item, "name", "") or "")
        item_value = str(getattr(item, "value", "") or "")
        text = str(item)
        if name == "UBO" or text in {"UBO", "DefaultAddons.UBO"} or item_value == str(DefaultAddons.UBO.value):
            return True
    return False


def _addon_cache_snapshot(addons_dir: Any = None, install_dir: Any = None) -> dict[str, Any]:
    install_path = _absolute_path(install_dir) if install_dir is not None else ""
    addons_path = _absolute_path(addons_dir) if addons_dir is not None else ""
    ubo_path = _os.path.join(addons_path, "UBO") if addons_path else ""
    manifest_path = _os.path.join(ubo_path, "manifest.json") if ubo_path else ""
    return {
        "install_dir": _safe_text(install_path, 700),
        "install_dir_exists": bool(install_path and _os.path.isdir(install_path)),
        "addons_dir": _safe_text(addons_path, 700),
        "addons_dir_exists": bool(addons_path and _os.path.isdir(addons_path)),
        "ubo_dir": _safe_text(ubo_path, 700),
        "ubo_dir_exists": bool(ubo_path and _os.path.isdir(ubo_path)),
        "ubo_manifest": _safe_text(manifest_path, 700),
        "ubo_manifest_exists": bool(manifest_path and _os.path.isfile(manifest_path)),
    }


def _apply_default_addon_policy(options_kwargs: dict[str, Any], DefaultAddons: Any, addons_dir: Any, install_dir: Any) -> dict[str, Any]:
    approved_roots = _approved_addon_roots(options_kwargs)
    raw_addons_present = "addons" in options_kwargs and options_kwargs.get("addons") is not None
    explicit_addons = _validate_explicit_addons(options_kwargs.get("addons"), approved_roots) if raw_addons_present else []
    if raw_addons_present:
        options_kwargs["addons"] = explicit_addons
    explicit_addon_count = len(explicit_addons)
    exclude_list = _addon_sequence(options_kwargs.get("exclude_addons"))
    policy_applied = False
    if not _addon_exclusion_contains_ubo(exclude_list, DefaultAddons):
        exclude_list.append(DefaultAddons.UBO)
        policy_applied = True
    options_kwargs["exclude_addons"] = exclude_list
    default_excluded = _addon_exclusion_contains_ubo(options_kwargs.get("exclude_addons"), DefaultAddons)
    policy = {
        "marker": AIDA_DEFAULT_ADDON_POLICY_MARKER,
        "policy_reference": "DefaultAddons.UBO",
        "default_exclusion_scope": "all_launches",
        "default_addons_excluded": bool(default_excluded),
        "exclude_default_ubo": bool(default_excluded),
        "default_exclusion_applied": bool(policy_applied),
        "explicit_addon_count": explicit_addon_count,
        "approved_addon_root_count": len(approved_roots),
        "from_options_has_addons": bool(explicit_addon_count),
        "exclude_addon_count": len(_addon_sequence(options_kwargs.get("exclude_addons"))),
        "raw_addons_present": bool(raw_addons_present),
        "explicit_addons_validated": bool(raw_addons_present),
    }
    policy.update(_addon_cache_snapshot(addons_dir, install_dir))
    return policy


def _addon_exception_matches(exc: Exception) -> bool:
    text = str(exc).lower()
    return type(exc).__name__ == "InvalidAddonPath" or "manifest.json" in text or "explicit addon path" in text


def _addon_count_from_options(options: dict[str, Any] | None) -> int:
    if not isinstance(options, dict):
        return 0
    addons = options.get("addons")
    if isinstance(addons, (list, tuple, set)):
        return len(addons)
    return 1 if addons else 0


def _build_camoufox_launch_options(headless: bool, kwargs: dict[str, Any]) -> dict[str, Any]:
    global _LAST_ADDON_POLICY
    _launch_options_started = time.perf_counter()
    options_kwargs = {
        k: v for k, v in kwargs.items() if k not in ("headless", "from_options", "persistent_context")
    }
    _launch_keys = sorted(str(k) for k in options_kwargs.keys())
    _camoufox_debug(
        "launch_options_begin",
        headless=bool(headless),
        keys=_launch_keys,
        has_executable=bool(options_kwargs.get("executable_path")),
        has_prefs=bool(options_kwargs.get("firefox_user_prefs")),
        has_user_data_dir=bool(options_kwargs.get("user_data_dir")),
    )
    try:
        _camoufox_debug("launch_options_import_begin")
        from camoufox.addons import ADDONS_DIR, DefaultAddons, INSTALL_DIR
        from camoufox.utils import launch_options as _cfx_launch_options
        _camoufox_debug(
            "launch_options_import_ok",
            elapsed_ms=int((time.perf_counter() - _launch_options_started) * 1000),
        )
    except Exception as exc:
        _camoufox_debug(
            "launch_options_import_fail",
            elapsed_ms=int((time.perf_counter() - _launch_options_started) * 1000),
            error_type=type(exc).__name__,
            error_len=len(str(exc)),
            error_summary=_safe_text(exc),
            error_traceback=_safe_text("".join(_traceback.format_exception(type(exc), exc, exc.__traceback__)), 4000),
        )
        raise
    addon_policy: dict[str, Any] = {}
    try:
        addon_policy = _apply_default_addon_policy(options_kwargs, DefaultAddons, ADDONS_DIR, INSTALL_DIR)
        _LAST_ADDON_POLICY = dict(addon_policy)
        _launch_keys = sorted(str(k) for k in options_kwargs.keys())
        _camoufox_debug("launch_options_addon_policy", **addon_policy)
        _camoufox_debug(
            "launch_options_build_begin",
            elapsed_ms=int((time.perf_counter() - _launch_options_started) * 1000),
            key_count=len(_launch_keys),
        )
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            result = _cfx_launch_options(headless=headless, **options_kwargs)
        stdout = out.getvalue()
        if stdout:
            _camoufox_debug("launch_options_stdout", summary=_safe_text(stdout), length=len(stdout))
        _camoufox_debug(
            "launch_options_build_ok",
            elapsed_ms=int((time.perf_counter() - _launch_options_started) * 1000),
            option_keys=sorted(str(k) for k in result.keys()) if isinstance(result, dict) else [],
            from_options_has_executable=bool(result.get("executable_path")) if isinstance(result, dict) else False,
            from_options_args=len(result.get("args") or []) if isinstance(result, dict) else 0,
            from_options_has_env=bool(result.get("env")) if isinstance(result, dict) else False,
            from_options_has_addons=bool(_addon_count_from_options(result)),
            addon_count=_addon_count_from_options(result),
            default_addons_excluded=bool(addon_policy.get("default_addons_excluded")),
            exclude_default_ubo=bool(addon_policy.get("exclude_default_ubo")),
            explicit_addon_count=int(addon_policy.get("explicit_addon_count") or 0),
            addon_policy=addon_policy,
        )
        return result
    except Exception as exc:
        if _addon_exception_matches(exc):
            invalid_policy = dict(addon_policy or _LAST_ADDON_POLICY or {})
            invalid_diag = {
                "marker": AIDA_DEFAULT_ADDON_POLICY_MARKER,
                "child_pid": _os.getpid(),
                "elapsed_ms": int((time.perf_counter() - _launch_options_started) * 1000),
                "error_type": type(exc).__name__,
                "error_len": len(str(exc)),
                "error_summary": _safe_text(exc),
                "default_addons_excluded": bool(invalid_policy.get("default_addons_excluded")),
                "exclude_default_ubo": bool(invalid_policy.get("exclude_default_ubo")),
                "explicit_addon_count": int(invalid_policy.get("explicit_addon_count") or 0),
                "approved_addon_root_count": int(invalid_policy.get("approved_addon_root_count") or 0),
            }
            invalid_diag.update(_addon_cache_snapshot(locals().get("ADDONS_DIR"), locals().get("INSTALL_DIR")))
            _camoufox_debug("launch_options_addon_invalid", **invalid_diag)
        _camoufox_debug(
            "launch_options_build_fail",
            elapsed_ms=int((time.perf_counter() - _launch_options_started) * 1000),
            error_type=type(exc).__name__,
            error_len=len(str(exc)),
            error_summary=_safe_text(exc),
            error_traceback=_safe_text("".join(_traceback.format_exception(type(exc), exc, exc.__traceback__)), 4000),
            addon_policy=dict(addon_policy or _LAST_ADDON_POLICY or {}),
        )
        raise


def _profile_root() -> str:
    root = _os.environ.get("AIDA_CAMOUFOX_PROFILE_ROOT", "")
    if not root:
        base = _os.environ.get("LOCALAPPDATA") or _os.environ.get("TEMP") or _os.getcwd()
        root = _os.path.join(base, "AiDA", "camoufox-profiles")
    return _os.path.abspath(_os.path.expandvars(_os.path.expanduser(root)))


def _cleanup_stale_profiles(root: str) -> dict[str, int]:
    summary = {"seen": 0, "removed": 0, "failed": 0}
    max_age_sec = _int_config(_os.environ.get("AIDA_CAMOUFOX_PROFILE_MAX_AGE_SEC"), PROFILE_CLEANUP_MAX_AGE_SEC)
    now = time.time()
    try:
        for entry in _os.scandir(root):
            if summary["removed"] >= PROFILE_CLEANUP_MAX_REMOVALS:
                break
            if not entry.name.startswith("profile-"):
                continue
            try:
                if not entry.is_dir(follow_symlinks=False):
                    continue
                summary["seen"] += 1
                age_sec = now - entry.stat(follow_symlinks=False).st_mtime
                if age_sec < max_age_sec:
                    continue
                _shutil.rmtree(entry.path, ignore_errors=False)
                summary["removed"] += 1
            except Exception:
                summary["failed"] += 1
    except Exception:
        summary["failed"] += 1
    if summary["removed"] or summary["failed"]:
        _camoufox_debug("profile_cleanup", root=root, max_age_sec=max_age_sec, **summary)
    return summary


def _new_profile_dir() -> str:
    root = _profile_root()
    _os.makedirs(root, exist_ok=True)
    _cleanup_stale_profiles(root)
    return _os.path.join(root, f"profile-{_os.getpid()}-{int(time.time() * 1000)}-{_uuid.uuid4().hex[:8]}")


def _path_info(path: str | None) -> dict[str, Any]:
    if not path:
        return {"path": "", "exists": False, "is_file": False, "is_dir": False}
    resolved = _os.path.abspath(_os.path.expandvars(_os.path.expanduser(str(path))))
    info: dict[str, Any] = {"path": resolved, "exists": False, "is_file": False, "is_dir": False}
    try:
        st = _os.stat(resolved)
        info.update({
            "exists": True,
            "is_file": _os.path.isfile(resolved),
            "is_dir": _os.path.isdir(resolved),
            "size": int(st.st_size),
            "mtime_ms": int(st.st_mtime * 1000),
        })
    except OSError as exc:
        info["error_type"] = type(exc).__name__
        info["errno"] = getattr(exc, "errno", 0) or 0
    return info


def _derive_ff_version(executable_path: str | None) -> int | None:
    if not executable_path:
        return None
    parts = _os.path.abspath(executable_path).split(_os.sep)
    for part in reversed(parts):
        for match in _re.finditer(r"(?<!\d)(\d{2,3})(?:\.\d+){0,3}", part):
            value = int(match.group(1))
            if 60 <= value <= 300:
                return value
    return None


def _profile_lock_info(profile_dir: str) -> list[dict[str, Any]]:
    locks = []
    for name in ("parent.lock", ".parentlock", "lock"):
        path = _os.path.join(profile_dir, name)
        if not _os.path.exists(path):
            continue
        item = _path_info(path)
        item["name"] = name
        locks.append(item)
    return locks


def _profile_lock_snapshot(profile_dir: str | None) -> list[dict[str, Any]]:
    if not profile_dir:
        return []
    try:
        return _profile_lock_info(profile_dir)
    except Exception as exc:
        return [{"error_type": type(exc).__name__, "error": _safe_text(exc, 240)}]


def _crash_report_snapshot(profile_dir: str | None, since_ms: int | None = None, max_files: int = 24) -> dict[str, Any]:
    resolved = _os.path.abspath(_os.path.expandvars(_os.path.expanduser(str(profile_dir)))) if profile_dir else ""
    out: dict[str, Any] = {
        "profile_dir": resolved,
        "profile_exists": bool(resolved and _os.path.isdir(resolved)),
        "since_ms": int(since_ms or 0),
        "file_count": 0,
        "matching_since_count": 0,
        "files": [],
    }
    if not resolved or not _os.path.isdir(resolved):
        return out
    suffixes = (".dmp", ".mdmp", ".dump", ".extra", ".crash", ".json", ".txt")
    path_tokens = ("crash", "minidump", "dump")
    found: list[dict[str, Any]] = []
    scanned_dirs = 0
    scanned_files = 0
    try:
        for root, dirs, files in _os.walk(resolved):
            scanned_dirs += 1
            if scanned_dirs > 2000:
                out["truncated"] = True
                break
            dirs[:] = [item for item in dirs if item not in {"cache2", "startupCache", "shader-cache"}][:64]
            root_lower = root.lower()
            for name in files:
                scanned_files += 1
                lower = name.lower()
                if not lower.endswith(suffixes) and not any(token in root_lower or token in lower for token in path_tokens):
                    continue
                path = _os.path.join(root, name)
                try:
                    st = _os.stat(path)
                    mtime_ms = int(st.st_mtime * 1000)
                    item = {
                        "path": _safe_text(path, 700),
                        "relative_path": _safe_text(_os.path.relpath(path, resolved), 500),
                        "name": _safe_text(name, 260),
                        "size": int(st.st_size),
                        "mtime_ms": mtime_ms,
                        "age_ms": max(0, int(time.time() * 1000) - mtime_ms),
                        "since_match": bool(since_ms and mtime_ms >= int(since_ms)),
                    }
                    found.append(item)
                except Exception as exc:
                    found.append({
                        "path": _safe_text(path, 700),
                        "name": _safe_text(name, 260),
                        "error_type": type(exc).__name__,
                        "error": _safe_text(exc, 180),
                    })
                if len(found) >= max(200, max_files):
                    out["truncated"] = True
                    break
            if len(found) >= max(200, max_files):
                break
    except Exception as exc:
        out["scan_error_type"] = type(exc).__name__
        out["scan_error"] = _safe_text(exc, 240)
    found.sort(key=lambda item: int(item.get("mtime_ms") or 0), reverse=True)
    out["scanned_dirs"] = scanned_dirs
    out["scanned_files"] = scanned_files
    out["file_count"] = len(found)
    out["matching_since_count"] = sum(1 for item in found if item.get("since_match"))
    out["files"] = found[:max(1, min(max_files, 64))]
    if found:
        out["newest_mtime_ms"] = int(found[0].get("mtime_ms") or 0)
        out["newest_age_ms"] = int(found[0].get("age_ms") or 0)
    return out


def _safe_page_url(page: Page | None, limit: int = 300) -> str:
    if page is None:
        return ""
    try:
        return _safe_text(str(page.url or ""), limit)
    except Exception as exc:
        return f"<url_error:{type(exc).__name__}>"


def _target_domain(value: Any) -> str:
    text = str(value or "").strip()
    if not text:
        return ""
    try:
        parsed = _urlparse(text)
        host = parsed.netloc or parsed.path.split("/", 1)[0]
        if "@" in host:
            host = host.rsplit("@", 1)[-1]
        if ":" in host:
            host = host.split(":", 1)[0]
        return host.lower()
    except Exception:
        return ""


def _env_presence(source: dict[str, Any] | None = None) -> dict[str, bool]:
    env = _os.environ if source is None else source
    keys = (
        "AIDA_CAMOUFOX_DEBUG_LOG",
        "AIDA_CAMOUFOX_DEBUG_STDERR",
        "AIDA_CAMOUFOX_EXECUTABLE",
        "AIDA_CAMOUFOX_PROFILE_ROOT",
        "AIDA_CAMOUFOX_SESSION_ID",
        "AIDA_CAMOUFOX_TESTLAB_FAST_PROBE",
        "CAMOU_CONFIG",
        "LOCALAPPDATA",
        "MOZ_DISABLE_CONTENT_SANDBOX",
        "PYTHONIOENCODING",
        "TEMP",
    )
    return {key: bool(env.get(key)) for key in keys}


def _is_build_tool_env_key(key: str) -> bool:
    upper = str(key or "").upper()
    exact = {
        "COMMANDPROMPTTYPE",
        "DEVENVDIR",
        "EXTERNAL_INCLUDE",
        "FRAMEWORK40VERSION",
        "FRAMEWORKDIR",
        "FRAMEWORKDIR64",
        "FRAMEWORKVERSION",
        "FRAMEWORKVERSION64",
        "HTMLHELPDIR",
        "INCLUDE",
        "LIB",
        "LIBPATH",
        "PLATFORM",
        "SCCACHE_CACHE_SIZE",
        "SCCACHE_DIR",
        "SCCACHE_IDLE_TIMEOUT",
        "UCRTVERSION",
        "UNIVERSALCRTSDKDIR",
        "VCIDEINSTALLDIR",
        "VCINSTALLDIR",
        "VCPKG_ROOT",
        "VCTOOLSINSTALLDIR",
        "VCTOOLSREDISTDIR",
        "VCTOOLSVERSION",
        "VISUALSTUDIOVERSION",
        "VSINSTALLDIR",
        "WINDOWSLIBPATH",
        "WINDOWSSDKBINPATH",
        "WINDOWSSDKDIR",
        "WINDOWSSDKLIBVERSION",
        "WINDOWSSDKVERBINPATH",
        "WINDOWSSDKVERSION",
    }
    prefixes = ("VS", "VSCMD_")
    return upper in exact or any(upper.startswith(prefix) for prefix in prefixes)


def _sanitize_browser_launch_env(options: dict[str, Any] | None) -> dict[str, Any]:
    if not isinstance(options, dict):
        return {"before": 0, "after": 0, "removed": []}
    env = options.get("env")
    if not isinstance(env, dict):
        return {"before": 0, "after": 0, "removed": []}
    removed = sorted(str(key) for key in env.keys() if _is_build_tool_env_key(str(key)))
    if not removed:
        return {"before": len(env), "after": len(env), "removed": []}
    removed_set = set(removed)
    options["env"] = {key: value for key, value in env.items() if str(key) not in removed_set}
    return {"before": len(env), "after": len(options["env"]), "removed": removed}


def _launch_policy_info(options: dict[str, Any] | None) -> dict[str, Any]:
    data = options if isinstance(options, dict) else {}
    args = [str(arg) for arg in (data.get("args") or [])]
    env = data.get("env") if isinstance(data.get("env"), dict) else {}
    addon_policy = dict(_LAST_ADDON_POLICY or {})
    flags = []
    profile_args = []
    profile_path_candidates = []
    for idx, arg in enumerate(args):
        if arg.startswith("--"):
            flags.append(arg.split("=", 1)[0])
        lower = arg.lower()
        if "profile" in lower or "user-data-dir" in lower:
            profile_args.append(_safe_text(arg, 260))
        if lower in {"-profile", "--profile", "--user-data-dir"} and idx + 1 < len(args):
            profile_path_candidates.append(_safe_text(args[idx + 1], 700))
        elif lower.startswith("--profile=") or lower.startswith("--user-data-dir="):
            profile_path_candidates.append(_safe_text(arg.split("=", 1)[1], 700))
    return {
        "arg_count": len(args),
        "arg_hash": _argv_hash_summary(args),
        "arg_flags": sorted(set(flags))[:96],
        "has_user_data_arg": any("user-data-dir" in arg for arg in args),
        "has_profile_arg": any("profile" in arg.lower() for arg in args),
        "profile_args": profile_args[:16],
        "profile_path_candidates": profile_path_candidates[:8],
        "env_key_count": len(env),
        "env_keys": sorted(str(key) for key in env.keys())[:96],
        "env_hash": _env_hash_summary(env),
        "env_presence": _env_presence(env),
        "has_executable_path": bool(data.get("executable_path")),
        "has_firefox_user_prefs": bool(data.get("firefox_user_prefs")),
        "prefs_hash": _hash_summary(data.get("firefox_user_prefs") or {}),
        "from_options_has_addons": bool(_addon_count_from_options(data)),
        "addon_count": _addon_count_from_options(data),
        "addon_policy": addon_policy,
        "default_addons_excluded": bool(addon_policy.get("default_addons_excluded")),
        "exclude_default_ubo": bool(addon_policy.get("exclude_default_ubo")),
        "explicit_addon_count": int(addon_policy.get("explicit_addon_count") or 0),
    }


def _prefs_summary(prefs: dict[str, Any] | None) -> dict[str, Any]:
    data = prefs if isinstance(prefs, dict) else {}
    selected_keys = (
        "media.peerconnection.enabled",
        "media.navigator.enabled",
        "media.getusermedia.screensharing.enabled",
        "media.peerconnection.ice.no_host",
        "media.peerconnection.ice.default_address_only",
        "media.peerconnection.ice.proxy_only_if_behind_proxy",
        "dom.ipc.processPrelaunch.enabled",
        "dom.ipc.processCount",
        "fission.webContentIsolationStrategy",
        "browser.sessionstore.resume_from_crash",
        "toolkit.telemetry.reportingpolicy.firstRun",
        "datareporting.healthreport.uploadEnabled",
    )
    selected: dict[str, Any] = {}
    for key in selected_keys:
        if key in data:
            value = data.get(key)
            if isinstance(value, (str, int, float, bool)) or value is None:
                selected[key] = value
            else:
                selected[key] = _safe_text(value, 160)
    return {
        "count": len(data),
        "keys": sorted(str(key) for key in data.keys())[:160],
        "selected": selected,
        "hash": _hash_summary(data),
        "webrtc_disabled": data.get("media.peerconnection.enabled") is False,
        "media_devices_disabled": data.get("media.navigator.enabled") is False,
        "forced_single_content_process": "dom.ipc.processCount" in data or "fission.webContentIsolationStrategy" in data,
    }


def _prepare_profile_dir(profile_dir: str, generated: bool) -> tuple[str, dict[str, Any]]:
    resolved = _os.path.abspath(_os.path.expandvars(_os.path.expanduser(str(profile_dir))))
    existed = _os.path.exists(resolved)
    if generated and existed:
        _shutil.rmtree(resolved, ignore_errors=True)
        existed = False
    _os.makedirs(resolved, exist_ok=True)
    locks = _profile_lock_info(resolved)
    info = {
        "profile_dir": resolved,
        "generated": generated,
        "existed": existed,
        "locks": len(locks),
        "lock_names": [item.get("name", "") for item in locks],
    }
    return resolved, info


def _default_firefox_user_prefs() -> dict[str, Any]:
    return {
        "browser.shell.checkDefaultBrowser": False,
        "browser.startup.homepage": "about:blank",
        "browser.startup.page": 0,
        "browser.startup.homepage_override.mstone": "ignore",
        "browser.aboutConfig.showWarning": False,
        "browser.search.geoip.url": "",
        "browser.search.region": "US",
        "browser.search.update": False,
        "browser.search.suggest.enabled": False,
        "browser.search.separatePrivateDefault": False,
        "browser.search.separatePrivateDefault.ui.enabled": False,
        "browser.tabs.warnOnClose": False,
        "browser.urlbar.quicksuggest.enabled": False,
        "browser.urlbar.suggest.searches": False,
        "browser.warnOnQuit": False,
        "datareporting.healthreport.uploadEnabled": False,
        "datareporting.policy.dataSubmissionEnabled": False,
        "extensions.getAddons.cache.enabled": False,
        "toolkit.telemetry.reportingpolicy.firstRun": False,
        "toolkit.winRegisterApplicationRestart": False,
        "browser.sessionstore.resume_from_crash": False,
        "media.peerconnection.enabled": False,
        "media.navigator.enabled": False,
        "media.getusermedia.screensharing.enabled": False,
        "media.peerconnection.ice.no_host": True,
        "media.peerconnection.ice.default_address_only": True,
        "media.peerconnection.ice.proxy_only_if_behind_proxy": True,
        "dom.ipc.processPrelaunch.enabled": True,
    }


def _minimal_firefox_user_prefs() -> dict[str, Any]:
    return {
        "browser.shell.checkDefaultBrowser": False,
        "browser.startup.homepage": "about:blank",
        "browser.startup.page": 0,
        "media.peerconnection.enabled": False,
        "media.navigator.enabled": False,
        "media.getusermedia.screensharing.enabled": False,
        "media.peerconnection.ice.no_host": True,
        "media.peerconnection.ice.default_address_only": True,
        "media.peerconnection.ice.proxy_only_if_behind_proxy": True,
        "dom.ipc.processPrelaunch.enabled": True,
    }


def _normalize_proxy(value: Any) -> dict[str, Any] | None:
    if isinstance(value, dict):
        server = str(value.get("server") or "").strip()
        if not server:
            return None
        out = dict(value)
        out["server"] = server
        return out
    if isinstance(value, str):
        server = value.strip()
        if server:
            return {"server": server}
    return None


def _ff_version_text(value: Any) -> str:
    try:
        parsed = int(value)
        if 60 <= parsed <= 999:
            return str(parsed)
    except (TypeError, ValueError):
        pass
    return "135"


def _firefox_user_agent(os_type: str, ff_version: Any) -> str:
    version = _ff_version_text(ff_version)
    os_key = str(os_type or "windows").strip().lower()
    if os_key == "macos":
        return f"Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:{version}.0) Gecko/20100101 Firefox/{version}.0"
    if os_key == "linux":
        return f"Mozilla/5.0 (X11; Linux x86_64; rv:{version}.0) Gecko/20100101 Firefox/{version}.0"
    return f"Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:{version}.0) Gecko/20100101 Firefox/{version}.0"


def _clean_user_agent(value: Any) -> str | None:
    if value is None:
        return None
    text = str(value).strip()
    if not text:
        return None
    if "\r" in text or "\n" in text or "\0" in text:
        raise ValueError("Invalid user agent")
    if len(text) < 20 or len(text) > 512:
        raise ValueError("Invalid user agent length")
    if "Mozilla/" not in text:
        raise ValueError("Invalid user agent family")
    return text


def _resolve_user_agent(cfg: dict[str, Any], os_type: str, ff_version: Any) -> tuple[str | None, str]:
    custom = _clean_user_agent(cfg.get("user_agent") or cfg.get("userAgent"))
    if custom:
        return custom, "custom"
    policy = str(
        cfg.get("ua_policy")
        or cfg.get("user_agent_profile")
        or cfg.get("user_agent_mode")
        or "camoufox_native"
    ).strip().lower().replace("-", "_")
    if policy in {"", "auto", "native", "camoufox", "camoufox_native"}:
        return None, "camoufox_native"
    if policy in {"camoufox_auto", "camoufox_desktop"}:
        return _firefox_user_agent(os_type, ff_version), "camoufox_auto"
    profile_os = {
        "windows": "windows",
        "win": "windows",
        "camoufox_windows": "windows",
        "windows_camoufox": "windows",
        "mac": "macos",
        "macos": "macos",
        "camoufox_macos": "macos",
        "macos_camoufox": "macos",
        "linux": "linux",
        "camoufox_linux": "linux",
        "linux_camoufox": "linux",
    }
    if policy in {"random", "random_camoufox", "random_camoufox_desktop", "rotate", "rotating"}:
        selected = _secrets.choice(("windows", "macos", "linux"))
        return _firefox_user_agent(selected, ff_version), f"random_camoufox_desktop:{selected}"
    mapped = profile_os.get(policy)
    if mapped:
        return _firefox_user_agent(mapped, ff_version), f"camoufox_{mapped}"
    raise ValueError(f"Unsupported user agent policy: {policy}")


def _context_os(os_type: str) -> str | None:
    value = str(os_type or "").strip().lower()
    if value in {"windows", "macos", "linux"}:
        return value
    return None


def _context_os_from_user_agent(user_agent: str | None) -> str | None:
    text = str(user_agent or "")
    if "Windows NT" in text:
        return "windows"
    if "Macintosh" in text or "Mac OS X" in text:
        return "macos"
    if "Linux" in text or "X11" in text:
        return "linux"
    return None


def _user_agent_fingerprint_overrides(user_agent: str | None) -> dict[str, str]:
    os_name = _context_os_from_user_agent(user_agent)
    if os_name == "windows":
        return {
            "navigator.userAgent": str(user_agent),
            "navigator.platform": "Win32",
            "navigator.oscpu": "Windows NT 10.0; Win64; x64",
        }
    if os_name == "macos":
        return {
            "navigator.userAgent": str(user_agent),
            "navigator.platform": "MacIntel",
            "navigator.oscpu": "Intel Mac OS X 10.15",
        }
    if os_name == "linux":
        return {
            "navigator.userAgent": str(user_agent),
            "navigator.platform": "Linux x86_64",
            "navigator.oscpu": "Linux x86_64",
        }
    return {"navigator.userAgent": str(user_agent)} if user_agent else {}


def _privacy_override_script(plan: dict[str, Any]) -> str | None:
    overrides = dict(plan.get("fingerprint_overrides") or {})
    user_agent = overrides.get("navigator.userAgent")
    if not user_agent:
        return None
    platform_value = overrides.get("navigator.platform")
    oscpu_value = overrides.get("navigator.oscpu")
    app_version = str(user_agent).removeprefix("Mozilla/")
    return f"""(() => {{
        const define = (name, value) => {{
            if (value === null || value === undefined) return;
            try {{ Object.defineProperty(Navigator.prototype, name, {{ get: () => value, configurable: true }}); }} catch (_) {{}}
            try {{ Object.defineProperty(navigator, name, {{ get: () => value, configurable: true }}); }} catch (_) {{}}
        }};
        define("userAgent", {_json.dumps(user_agent)});
        define("appVersion", {_json.dumps(app_version)});
        define("platform", {_json.dumps(platform_value)});
        define("oscpu", {_json.dumps(oscpu_value)});
        define("vendor", "");
    }})()"""


def _empty_ice_probe_result(status: str) -> dict[str, Any]:
    return {
        "status": status,
        "blocked": status == "constructor_absent",
        "ok": status == "constructor_absent",
        "supported": False,
        "candidate_count": 0,
        "candidate_samples": [],
        "candidate_types": [],
        "ip_candidates": [],
        "host_ip_candidates": [],
        "private_ip_candidates": [],
        "public_ip_candidates": [],
        "mdns_host_candidates": [],
        "leak_detected": False,
        "error": "",
    }


def _build_context_plan(
    cfg: dict[str, Any],
    os_type: str,
    locale: str,
    locale_requested: Any,
    ff_version: Any,
) -> dict[str, Any]:
    user_agent, ua_policy = _resolve_user_agent(cfg, os_type, ff_version)
    fingerprint_overrides = _user_agent_fingerprint_overrides(user_agent)
    context_options: dict[str, Any] = {}
    service_worker_policy = str(cfg.get("service_workers") or "").strip().lower()
    if _flag_enabled(cfg.get("block_service_workers")) or service_worker_policy in {"block", "blocked", "disable", "disabled"}:
        context_options["service_workers"] = "block"
    elif service_worker_policy in {"allow", "allowed", "enable", "enabled"}:
        context_options["service_workers"] = "allow"
    if user_agent:
        context_options["user_agent"] = user_agent
    if locale_requested != "auto" and locale:
        context_options["locale"] = str(locale)
    return {
        "context_options": context_options,
        "proxy": _normalize_proxy(cfg.get("proxy")),
        "os": _context_os_from_user_agent(user_agent) or _context_os(os_type),
        "locale": str(locale) if locale_requested != "auto" and locale else None,
        "ff_version": _ff_version_text(ff_version),
        "user_agent": user_agent,
        "ua_policy": ua_policy,
        "persistent_context": bool(cfg.get("persistent_context") or cfg.get("profile_dir") or cfg.get("user_data_dir")),
        "profile_dir": str(cfg.get("profile_dir") or ""),
        "user_data_dir": str(cfg.get("user_data_dir") or ""),
        "fingerprint_overrides": fingerprint_overrides,
        "block_webrtc": True,
        "privacy_fail_closed": bool(cfg.get("privacy_fail_closed", True)),
    }


def _fast_visible_privacy_plan(plan: dict[str, Any]) -> dict[str, Any]:
    return dict(plan)


def _fast_visible_firefox_user_prefs(base_prefs: dict[str, Any], plan: dict[str, Any]) -> dict[str, Any]:
    prefs = dict(base_prefs)
    prefs.update({
        "media.peerconnection.enabled": False,
        "media.navigator.enabled": False,
        "media.getusermedia.screensharing.enabled": False,
        "media.peerconnection.ice.no_host": True,
        "media.peerconnection.ice.default_address_only": True,
        "media.peerconnection.ice.proxy_only_if_behind_proxy": True,
        "dom.ipc.processPrelaunch.enabled": True,
    })
    user_agent = str(plan.get("user_agent") or "")
    overrides = dict(plan.get("fingerprint_overrides") or {})
    if user_agent:
        prefs["general.useragent.override"] = user_agent
        prefs["general.appversion.override"] = user_agent.removeprefix("Mozilla/")
    platform_value = overrides.get("navigator.platform")
    oscpu_value = overrides.get("navigator.oscpu")
    if platform_value:
        prefs["general.platform.override"] = str(platform_value)
    if oscpu_value:
        prefs["general.oscpu.override"] = str(oscpu_value)
    return prefs


def _fast_visible_page_options(plan: dict[str, Any], window_size: tuple[int, int]) -> dict[str, Any]:
    opts = dict(plan.get("context_options") or {})
    if plan.get("user_agent"):
        opts["user_agent"] = str(plan["user_agent"])
    if plan.get("locale"):
        opts["locale"] = str(plan["locale"])
    if plan.get("proxy"):
        opts["proxy"] = plan["proxy"]
    opts["viewport"] = {"width": int(window_size[0]), "height": int(window_size[1])}
    sanitized, _, diagnostics = _sanitize_camoufox_context_options(opts, window_size)
    _camoufox_debug("context_options_sanitized", launch_phase="fast_visible_page_options", **diagnostics)
    return sanitized


def _use_fast_visible_launch(cfg: dict[str, Any], bundled_visible_launch: bool, profile_requested: bool) -> bool:
    return False


def _preflight_context_plan(cfg: dict[str, Any]) -> dict[str, Any]:
    cfg_local = dict(cfg)
    proxy_cfg = _normalize_proxy(cfg_local.get("proxy"))
    if proxy_cfg:
        cfg_local["proxy"] = proxy_cfg
    os_requested = cfg_local.get("os", cfg_local.get("os_type", "auto"))
    host_os = detect_host_os()
    os_type = host_os if os_requested == "auto" else os_requested
    locale_requested = cfg_local.get("locale", "auto")
    locale = detect_system_locale() if locale_requested == "auto" else locale_requested
    ff_version = cfg_local.get("ff_version")
    executable_path = cfg_local.get("executable_path") or __import__("os").environ.get("AIDA_CAMOUFOX_EXECUTABLE")
    if ff_version is None and executable_path:
        try:
            ff_version = _derive_ff_version(str(executable_path))
        except Exception:
            ff_version = None
    plan = _build_context_plan(cfg_local, str(os_type), str(locale), locale_requested, ff_version)
    headless = bool(cfg_local.get("headless", False))
    bundled_visible_launch = bool(executable_path) and not headless
    profile_requested = bool(cfg_local.get("profile_dir") or cfg_local.get("user_data_dir") or cfg_local.get("persistent_context"))
    if _use_fast_visible_launch(cfg_local, bundled_visible_launch, profile_requested):
        plan = _fast_visible_privacy_plan(plan)
    return plan


def _context_plans_equal(a: dict[str, Any], b: dict[str, Any]) -> bool:
    return _context_plan_mismatch_reason(a, b) == ""


def _plan_text(value: Any, fallback: str = "") -> str:
    text = str(value or "").strip()
    return text if text else fallback


def _context_plan_path_matches(active: Any, requested: Any) -> bool:
    req = _plan_text(requested)
    if not req:
        return True
    cur = _plan_text(active)
    if not cur:
        return False
    return _os.path.normcase(_os.path.normpath(cur)) == _os.path.normcase(_os.path.normpath(req))


def _context_plan_mismatch_reason(a: dict[str, Any], b: dict[str, Any]) -> str:
    keys = (
        "context_options",
        "proxy",
        "os",
        "locale",
        "ff_version",
        "user_agent",
        "ua_policy",
        "fingerprint_overrides",
        "block_webrtc",
        "privacy_fail_closed",
    )
    for key in keys:
        if a.get(key) != b.get(key):
            return key
    if bool(b.get("persistent_context")) and not bool(a.get("persistent_context")):
        return "persistent_context"
    if not _context_plan_path_matches(a.get("profile_dir"), b.get("profile_dir")):
        return "profile_dir"
    if not _context_plan_path_matches(a.get("user_data_dir"), b.get("user_data_dir")):
        return "user_data_dir"
    return ""


async def _probe_webrtc_ice_leak(page: Page) -> dict[str, Any]:
    try:
        probe = await _await_no_cancel_wait(
            page.evaluate(
                """async () => {
                    const result = {
                        status: "started",
                        supported: false,
                        blocked: false,
                        ok: false,
                        candidate_count: 0,
                        candidate_samples: [],
                        candidate_types: [],
                        ip_candidates: [],
                        host_ip_candidates: [],
                        private_ip_candidates: [],
                        public_ip_candidates: [],
                        mdns_host_candidates: [],
                        leak_detected: false,
                        error: ""
                    };
                    const Ctor = window.RTCPeerConnection || window.webkitRTCPeerConnection || window.mozRTCPeerConnection;
                    if (typeof Ctor !== "function") {
                        result.status = "constructor_absent";
                        result.supported = false;
                        result.blocked = true;
                        result.ok = true;
                        return result;
                    }
                    result.supported = true;
                    const classifyIpv4 = (ip) => {
                        const parts = String(ip).split(".").map((p) => Number(p));
                        if (parts.length !== 4 || parts.some((p) => !Number.isInteger(p) || p < 0 || p > 255)) return "invalid";
                        if (parts[0] === 10 || parts[0] === 127 || parts[0] === 0) return "private";
                        if (parts[0] === 192 && parts[1] === 168) return "private";
                        if (parts[0] === 172 && parts[1] >= 16 && parts[1] <= 31) return "private";
                        if (parts[0] === 169 && parts[1] === 254) return "private";
                        if (parts[0] >= 224) return "reserved";
                        return "public";
                    };
                    const classifyIpv6 = (ip) => {
                        const value = String(ip).toLowerCase();
                        if (value === "::1" || value.startsWith("fc") || value.startsWith("fd") || value.startsWith("fe80:")) return "private";
                        if (value.startsWith("ff")) return "reserved";
                        return "public";
                    };
                    const pushUnique = (arr, value) => {
                        if (value && !arr.includes(value)) arr.push(value);
                    };
                    let pc = null;
                    const candidates = [];
                    try {
                        pc = new Ctor({ iceServers: [], iceCandidatePoolSize: 0 });
                        pc.createDataChannel("aida-ice-probe");
                        pc.onicecandidate = (event) => {
                            if (event && event.candidate && event.candidate.candidate) {
                                candidates.push(String(event.candidate.candidate));
                            }
                        };
                        const offer = await pc.createOffer({ offerToReceiveAudio: false, offerToReceiveVideo: false });
                        await pc.setLocalDescription(offer);
                        await new Promise((resolve) => setTimeout(resolve, 900));
                    } catch (e) {
                        result.status = "probe_error";
                        result.error = e && e.message ? String(e.message) : String(e);
                    } finally {
                        try { if (pc) pc.close(); } catch (_) {}
                    }
                    const ipv4Re = /(?:^|\\s)((?:\\d{1,3}\\.){3}\\d{1,3})(?=\\s|$)/g;
                    const ipv6Re = /(?:^|\\s)([a-fA-F0-9]{0,4}:[a-fA-F0-9:.]{2,})(?=\\s|$)/g;
                    for (const candidate of candidates) {
                        const sample = candidate.slice(0, 240);
                        if (result.candidate_samples.length < 8) result.candidate_samples.push(sample);
                        const typeMatch = / typ ([a-zA-Z0-9_-]+)/.exec(candidate);
                        const type = typeMatch ? typeMatch[1] : "unknown";
                        pushUnique(result.candidate_types, type);
                        if (/\\.local(?:\\s|$)/i.test(candidate) && type === "host") {
                            pushUnique(result.mdns_host_candidates, sample);
                        }
                        for (const match of candidate.matchAll(ipv4Re)) {
                            const ip = match[1];
                            const kind = classifyIpv4(ip);
                            pushUnique(result.ip_candidates, ip);
                            if (type === "host") pushUnique(result.host_ip_candidates, ip);
                            if (kind === "private") pushUnique(result.private_ip_candidates, ip);
                            if (kind === "public") pushUnique(result.public_ip_candidates, ip);
                        }
                        for (const match of candidate.matchAll(ipv6Re)) {
                            const ip = match[1];
                            if (!ip.includes(":")) continue;
                            const kind = classifyIpv6(ip);
                            pushUnique(result.ip_candidates, ip);
                            if (type === "host") pushUnique(result.host_ip_candidates, ip);
                            if (kind === "private") pushUnique(result.private_ip_candidates, ip);
                            if (kind === "public") pushUnique(result.public_ip_candidates, ip);
                        }
                    }
                    result.candidate_count = candidates.length;
                    result.leak_detected = result.host_ip_candidates.length > 0 || result.private_ip_candidates.length > 0 || result.public_ip_candidates.length > 0;
                    if (result.error) {
                        result.ok = false;
                    } else {
                        result.status = candidates.length ? "candidates_observed" : "no_candidates";
                        result.ok = !result.leak_detected;
                    }
                    return result;
                }"""
            ),
            timeout=4.0,
        )
    except Exception as exc:
        return {
            **_empty_ice_probe_result("probe_exception"),
            "ok": False,
            "blocked": False,
            "error": _safe_text(exc, 400),
            "error_type": type(exc).__name__,
        }
    if not isinstance(probe, dict):
        return {**_empty_ice_probe_result("invalid_result"), "ok": False, "blocked": False}
    out = _empty_ice_probe_result(str(probe.get("status") or "unknown"))
    for key, value in probe.items():
        out[key] = value
    for key in ("candidate_samples", "candidate_types", "ip_candidates", "host_ip_candidates", "private_ip_candidates", "public_ip_candidates", "mdns_host_candidates"):
        if not isinstance(out.get(key), list):
            out[key] = []
    out["candidate_count"] = int(out.get("candidate_count") or 0)
    out["blocked"] = bool(out.get("blocked"))
    out["ok"] = bool(out.get("ok"))
    out["supported"] = bool(out.get("supported"))
    out["leak_detected"] = bool(out.get("leak_detected"))
    return out


def _write_user_js_prefs(profile_dir: str | None, prefs: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {"profile_dir": profile_dir or "", "prefs": sorted(prefs.keys()), "written": False}
    if not profile_dir:
        return out
    try:
        _os.makedirs(profile_dir, exist_ok=True)
        path = _os.path.join(profile_dir, "user.js")
        existing = ""
        try:
            with open(path, "r", encoding="utf-8") as fp:
                existing = fp.read()
        except FileNotFoundError:
            pass
        keys = tuple(prefs.keys())
        lines = [
            line for line in existing.splitlines()
            if not any(line.strip().startswith(f"user_pref(\"{key}\"") for key in keys)
        ]
        rendered: list[str] = []
        for key, value in sorted(prefs.items()):
            if isinstance(value, bool):
                value_text = "true" if value else "false"
            elif isinstance(value, (int, float)):
                value_text = str(value)
            else:
                value_text = _json.dumps(value)
            rendered.append(f"user_pref(\"{key}\", {value_text});")
        with open(path, "w", encoding="utf-8", newline="\n") as fp:
            if lines:
                fp.write("\n".join(lines).rstrip() + "\n")
            fp.write("\n".join(rendered) + "\n")
        out["written"] = True
        out["path"] = path
    except Exception as exc:
        out["error_type"] = type(exc).__name__
        out["error"] = _safe_text(exc, 300)
    return out


def _launch_error_retryable(exc: Exception, phase: str) -> bool:
    text = str(exc).lower()
    kind = _launch_error_kind(exc)
    if kind in {"privacy_assertion_failed", "protocol_schema_viewport"}:
        return False
    if phase in {"privacy_verify", "privacy_page_navigate", "before_privacy_verify"}:
        return kind in {"timeout", "target_closed", "browser_disconnected", "context_closed", "browser_closed"}
    tokens = (
        "target page, context or browser has been closed",
        "browser has been closed",
        "context has been closed",
        "page closed during launch",
        "closed page during launch",
        "searchservice",
        "searchsettings",
        "defaultsconfig is undefined",
        "cannot write without any engine",
    )
    return phase in {"context_enter", "fast_visible_page", "new_page", "persistent_new_page", "page_ready"} or any(token in text for token in tokens)


def _windows_descendant_pids(root_pid: int) -> list[int]:
    if platform.system().lower() != "windows":
        return []
    try:
        import ctypes
        from ctypes import wintypes

        ULONG_PTR = wintypes.WPARAM

        class PROCESSENTRY32W(ctypes.Structure):
            _fields_ = [
                ("dwSize", wintypes.DWORD),
                ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ULONG_PTR),
                ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", wintypes.LONG),
                ("dwFlags", wintypes.DWORD),
                ("szExeFile", wintypes.WCHAR * 260),
            ]

        kernel32 = ctypes.windll.kernel32
        snapshot = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)
        if snapshot == wintypes.HANDLE(-1).value:
            return []
        try:
            entry = PROCESSENTRY32W()
            entry.dwSize = ctypes.sizeof(entry)
            children: dict[int, list[int]] = {}
            if not kernel32.Process32FirstW(snapshot, ctypes.byref(entry)):
                return []
            while True:
                pid = int(entry.th32ProcessID)
                ppid = int(entry.th32ParentProcessID)
                children.setdefault(ppid, []).append(pid)
                if not kernel32.Process32NextW(snapshot, ctypes.byref(entry)):
                    break
            out: list[int] = []
            stack = list(children.get(root_pid, []))
            while stack:
                pid = stack.pop()
                if pid == root_pid or pid in out:
                    continue
                out.append(pid)
                stack.extend(children.get(pid, []))
            return out
        finally:
            kernel32.CloseHandle(snapshot)
    except Exception:
        return []


def _windows_process_table() -> dict[int, dict[str, Any]]:
    if platform.system().lower() != "windows":
        return {}
    try:
        import ctypes
        from ctypes import wintypes

        ULONG_PTR = wintypes.WPARAM

        class PROCESSENTRY32W(ctypes.Structure):
            _fields_ = [
                ("dwSize", wintypes.DWORD),
                ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ULONG_PTR),
                ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", wintypes.LONG),
                ("dwFlags", wintypes.DWORD),
                ("szExeFile", wintypes.WCHAR * 260),
            ]

        kernel32 = ctypes.windll.kernel32
        snapshot = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)
        if snapshot == wintypes.HANDLE(-1).value:
            return {}
        try:
            entry = PROCESSENTRY32W()
            entry.dwSize = ctypes.sizeof(entry)
            table: dict[int, dict[str, Any]] = {}
            if not kernel32.Process32FirstW(snapshot, ctypes.byref(entry)):
                return {}
            while True:
                pid = int(entry.th32ProcessID)
                table[pid] = {
                    "pid": pid,
                    "ppid": int(entry.th32ParentProcessID),
                    "exe": _safe_text(str(entry.szExeFile or ""), 260),
                    "threads": int(entry.cntThreads),
                }
                if not kernel32.Process32NextW(snapshot, ctypes.byref(entry)):
                    break
            return table
        finally:
            kernel32.CloseHandle(snapshot)
    except Exception:
        return {}


def _windows_process_liveness(pid: int) -> dict[str, Any]:
    if platform.system().lower() != "windows":
        return {"alive": None, "exit_code": None}
    try:
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(0x1000, False, int(pid))
        if not handle:
            return {"alive": False, "exit_code": None, "open_error": int(kernel32.GetLastError())}
        try:
            code = wintypes.DWORD()
            if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
                return {"alive": None, "exit_code": None, "query_error": int(kernel32.GetLastError())}
            return {"alive": int(code.value) == 259, "exit_code": int(code.value)}
        finally:
            kernel32.CloseHandle(handle)
    except Exception as exc:
        return {"alive": None, "exit_code": None, "error_type": type(exc).__name__, "error": _safe_text(exc, 180)}


def _process_cmdline_fingerprint(pid: int, fallback: dict[str, Any] | None = None) -> dict[str, Any]:
    out: dict[str, Any] = {
        "cmdline_available": False,
        "cmdline_source": "",
        "cmdline_argc": 0,
        "cmdline_len": 0,
        "cmdline_sha256": "",
    }
    try:
        psutil = __import__("psutil")
        proc = psutil.Process(int(pid))
        cmdline = [str(part) for part in proc.cmdline()]
        joined = "\0".join(cmdline)
        out["cmdline_available"] = True
        out["cmdline_source"] = "psutil"
        out["cmdline_argc"] = len(cmdline)
        out["cmdline_len"] = len(joined)
        out["cmdline_sha256"] = _hashlib.sha256(joined.encode("utf-8", "replace")).hexdigest()[:32]
        with contextlib.suppress(Exception):
            out["create_time_ms"] = int(float(proc.create_time()) * 1000)
    except Exception as exc:
        if fallback:
            out.update(fallback)
            out["cmdline_fallback_after_error_type"] = type(exc).__name__
        else:
            out["cmdline_error_type"] = type(exc).__name__
            out["cmdline_error"] = _safe_text(exc, 180)
    return out


def _process_tree_detailed_snapshot(root_pid: int | None = None) -> dict[str, Any]:
    pid = int(root_pid or _os.getpid())
    basic = _process_tree_snapshot(pid)
    table = _windows_process_table()
    if not table:
        return {**basic, "records": [], "record_count": 0}
    children: dict[int, list[int]] = {}
    for record in table.values():
        children.setdefault(int(record.get("ppid") or 0), []).append(int(record.get("pid") or 0))
    ordered: list[int] = []
    stack = [pid]
    while stack and len(ordered) < 80:
        current = stack.pop(0)
        if current in ordered:
            continue
        ordered.append(current)
        stack.extend(children.get(current, []))
    try:
        __import__("psutil")
        fallback_cmdlines: dict[int, dict[str, Any]] = {}
    except Exception:
        fallback_cmdlines = _windows_process_cmdline_fallback(ordered)
    records: list[dict[str, Any]] = []
    for current in ordered:
        record = dict(table.get(current) or {"pid": current, "ppid": 0, "exe": ""})
        record.update(_windows_process_liveness(current))
        record.update(_process_cmdline_fingerprint(current, fallback_cmdlines.get(current)))
        records.append(record)
    return {
        "root_pid": pid,
        "descendant_count": max(0, len(records) - 1),
        "descendant_pids": [int(record.get("pid") or 0) for record in records[1:49]],
        "record_count": len(records),
        "records": records[:64],
    }


def _process_tree_delta(before: dict[str, Any] | None, after: dict[str, Any] | None) -> dict[str, Any]:
    before_records = [record for record in (before or {}).get("records", []) if isinstance(record, dict)]
    after_records = [record for record in (after or {}).get("records", []) if isinstance(record, dict)]
    before_map = {int(record.get("pid") or 0): record for record in before_records if int(record.get("pid") or 0) > 0}
    after_map = {int(record.get("pid") or 0): record for record in after_records if int(record.get("pid") or 0) > 0}
    before_pids = set(before_map)
    after_pids = set(after_map)
    exited = sorted(before_pids - after_pids)
    started = sorted(after_pids - before_pids)
    observed_ms = int(time.time() * 1000)
    exited_records = []
    for pid in exited[:24]:
        rec = dict(before_map[pid])
        rec["observed_exit_ts_ms"] = observed_ms
        rec["exit_observed_by"] = "process_tree_delta"
        exited_records.append(rec)
    return {
        "before_count": len(before_pids),
        "after_count": len(after_pids),
        "exited_count": len(exited),
        "started_count": len(started),
        "survived_count": len(before_pids & after_pids),
        "observed_ts_ms": observed_ms,
        "exited_pids": exited[:48],
        "started_pids": started[:48],
        "exited_records": exited_records,
        "started_records": [after_map[pid] for pid in started[:24]],
    }


def _process_tree_snapshot(root_pid: int | None = None) -> dict[str, Any]:
    pid = int(root_pid or _os.getpid())
    descendants = _windows_descendant_pids(pid)
    return {
        "root_pid": pid,
        "descendant_count": len(descendants),
        "descendant_pids": descendants[:48],
    }


def _visible_window_snapshot(root_pid: int | None = None) -> dict[str, Any]:
    pid = int(root_pid or _os.getpid())
    out: dict[str, Any] = {
        "root_pid": pid,
        "visible_window_count": 0,
        "first_pid": 0,
        "first_rect": {},
        "first_title_len": 0,
        "process_tree": _process_tree_snapshot(pid),
    }
    if platform.system().lower() != "windows":
        return out
    try:
        import ctypes
        from ctypes import wintypes

        class RECT(ctypes.Structure):
            _fields_ = [
                ("left", ctypes.c_long),
                ("top", ctypes.c_long),
                ("right", ctypes.c_long),
                ("bottom", ctypes.c_long),
            ]

        target_pids = {pid, *_windows_descendant_pids(pid)}
        user32 = ctypes.windll.user32
        records: list[dict[str, Any]] = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def enum_proc(hwnd, _lparam):
            try:
                if not user32.IsWindowVisible(hwnd):
                    return True
                window_pid = wintypes.DWORD()
                user32.GetWindowThreadProcessId(hwnd, ctypes.byref(window_pid))
                current_pid = int(window_pid.value)
                if current_pid not in target_pids:
                    return True
                rect = RECT()
                if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
                    return True
                width = int(rect.right - rect.left)
                height = int(rect.bottom - rect.top)
                if width <= 0 or height <= 0:
                    return True
                title_len = int(user32.GetWindowTextLengthW(hwnd))
                records.append({
                    "pid": current_pid,
                    "rect": {
                        "left": int(rect.left),
                        "top": int(rect.top),
                        "right": int(rect.right),
                        "bottom": int(rect.bottom),
                        "width": width,
                        "height": height,
                    },
                    "title_len": title_len,
                })
            except Exception:
                pass
            return True

        user32.EnumWindows(enum_proc, 0)
        out["visible_window_count"] = len(records)
        out["windows"] = records[:8]
        if records:
            out["first_pid"] = int(records[0].get("pid") or 0)
            out["first_rect"] = records[0].get("rect") or {}
            out["first_title_len"] = int(records[0].get("title_len") or 0)
    except Exception as exc:
        out["probe_error_type"] = type(exc).__name__
        out["probe_error"] = _safe_text(exc, 240)
    return out


def _debug_log_tail(limit: int = 4000) -> str:
    path = _os.environ.get("AIDA_CAMOUFOX_DEBUG_LOG", "")
    if not path:
        return ""
    try:
        size = _os.path.getsize(path)
        with open(path, "rb") as f:
            f.seek(max(0, size - max(1, limit)))
            return f.read(max(1, limit)).decode("utf-8", errors="replace").strip()[-limit:]
    except Exception:
        return ""


def _browser_context_count(browser: Any) -> int:
    try:
        contexts = getattr(browser, "contexts", None)
        if callable(contexts):
            contexts = contexts()
        return len(contexts or [])
    except Exception:
        return -1


def _context_page_count(ctx: BrowserContext | None) -> int:
    if ctx is None:
        return -1
    try:
        return len(getattr(ctx, "pages", []) or [])
    except Exception:
        return -1


def _pending_page_task_state(task: asyncio.Task | None) -> dict[str, Any]:
    if task is None:
        return {"present": False, "done": False, "cancelled": False}
    return {
        "present": True,
        "done": bool(task.done()),
        "cancelled": bool(task.cancelled()),
    }


def _pid_from_object(value: Any) -> int | None:
    if value is None:
        return None
    for attr in ("pid", "_pid"):
        try:
            pid = getattr(value, attr, None)
            if isinstance(pid, int) and pid > 0:
                return pid
        except Exception:
            pass
    for attr in ("process", "_process"):
        try:
            proc = getattr(value, attr, None)
            if callable(proc):
                proc = proc()
            pid = getattr(proc, "pid", None)
            if isinstance(pid, int) and pid > 0:
                return pid
        except Exception:
            pass
    return None


def _browser_runtime_snapshot(value: Any) -> dict[str, Any]:
    objects: list[Any] = []
    candidates = [value]
    for attr in ("browser", "_browser"):
        try:
            candidates.append(getattr(value, attr, None))
        except Exception:
            pass
    try:
        candidates.append(getattr(getattr(value, "_impl_obj", None), "_browser", None))
    except Exception:
        pass
    for obj in candidates:
        if obj is not None and all(obj is not known for known in objects):
            objects.append(obj)
    pids = []
    for obj in objects:
        pid = _pid_from_object(obj)
        if pid and pid not in pids:
            pids.append(pid)
    version = ""
    for obj in objects:
        try:
            candidate = getattr(obj, "version", "")
            version = str(candidate() if callable(candidate) else candidate)
            if version:
                break
        except Exception:
            pass
    return {
        "object_type": type(value).__name__ if value is not None else "",
        "browser_pids": pids[:16],
        "browser_version": _safe_text(version, 120) if version else "",
        "browser_context_count": _browser_context_count(value),
        "process_tree": _process_tree_snapshot(),
    }


def _terminate_descendant_processes(reason: str) -> dict[str, Any]:
    started = time.perf_counter()
    pids = _windows_descendant_pids(_os.getpid())
    summary: dict[str, Any] = {"reason": reason, "count": len(pids), "pids": pids[:24], "results": []}
    for pid in reversed(pids):
        try:
            proc_started = time.perf_counter()
            result = _subprocess.run(
                ["taskkill.exe", "/PID", str(pid), "/T", "/F"],
                capture_output=True,
                text=True,
                timeout=6,
            )
            summary["results"].append({
                "pid": pid,
                "returncode": int(result.returncode),
                "elapsed_ms": int((time.perf_counter() - proc_started) * 1000),
                "stdout": _safe_text(result.stdout, 240),
                "stderr": _safe_text(result.stderr, 240),
            })
        except Exception as exc:
            summary["results"].append({
                "pid": pid,
                "error_type": type(exc).__name__,
                "error": _safe_text(exc, 240),
            })
    summary["elapsed_ms"] = int((time.perf_counter() - started) * 1000)
    if pids or reason == "launch_error":
        _camoufox_debug("descendant_cleanup", **summary)
    return summary


async def _private_context_material(plan: dict[str, Any]) -> tuple[dict[str, Any], list[str], dict[str, int] | None, dict[str, Any]]:
    context_options = dict(plan.get("context_options") or {})
    from camoufox.fingerprints import generate_context_fingerprint
    config_overrides = dict(plan.get("fingerprint_overrides") or {}) or None
    loop = asyncio.get_event_loop()
    fp = await loop.run_in_executor(
        None,
        lambda: generate_context_fingerprint(
            os=plan.get("os"),
            ff_version=plan.get("ff_version"),
            locale=plan.get("locale"),
            config_overrides=config_overrides,
        ),
    )
    opts = {**dict(fp.get("context_options") or {}), **context_options}
    if plan.get("proxy"):
        opts["proxy"] = plan["proxy"]
    init_scripts: list[str] = []
    init_script = fp.get("init_script")
    if init_script:
        init_scripts.append(str(init_script))
    override_script = _privacy_override_script(plan)
    if override_script:
        init_scripts.append(override_script)
    sanitized, page_viewport, diagnostics = _sanitize_camoufox_context_options(opts, _page_viewport_from_plan(plan))
    return sanitized, init_scripts, page_viewport, diagnostics


async def _apply_context_init_scripts(ctx: BrowserContext, scripts: list[str], timeout_s: float) -> None:
    for script in scripts:
        await _await_no_cancel_wait(ctx.add_init_script(script), timeout=min(5.0, timeout_s))


async def _create_private_context(browser: Any, plan: dict[str, Any], timeout_s: float, phase: str = "new_context", started: float | None = None) -> tuple[BrowserContext, str]:
    opts, init_scripts, page_viewport, diagnostics = await _private_context_material(plan)
    _camoufox_debug(
        "context_options_sanitized",
        launch_phase=phase,
        timeout_s=timeout_s,
        elapsed_ms=int((time.perf_counter() - started) * 1000) if started else 0,
        **diagnostics,
    )
    try:
        ctx = await _await_no_cancel_wait(browser.new_context(**opts), timeout=timeout_s)
    except Exception as exc:
        _camoufox_debug(
            "context_new_context_failed",
            launch_phase=phase,
            elapsed_ms=int((time.perf_counter() - started) * 1000) if started else 0,
            error_type=type(exc).__name__,
            error_kind=_launch_error_kind(exc, None),
            error_summary=_safe_text(exc),
            **diagnostics,
        )
        raise
    _camoufox_debug(
        "context_new_context_ok",
        launch_phase=phase,
        context_page_count=_context_page_count(ctx),
        elapsed_ms=int((time.perf_counter() - started) * 1000) if started else 0,
        **diagnostics,
    )
    await _apply_context_init_scripts(ctx, init_scripts, timeout_s)
    return ctx, "camoufox_fingerprint_context"


async def _create_private_browser_page_context(browser: Any, plan: dict[str, Any], timeout_s: float, phase: str = "browser_new_page_context", started: float | None = None) -> tuple[BrowserContext, Page, str]:
    opts, init_scripts, page_viewport, diagnostics = await _private_context_material(plan)
    _camoufox_debug(
        "context_options_sanitized",
        launch_phase=phase,
        context_api="browser.new_page",
        timeout_s=timeout_s,
        elapsed_ms=int((time.perf_counter() - started) * 1000) if started else 0,
        **diagnostics,
    )
    try:
        page = await _await_no_cancel_wait(browser.new_page(**opts), timeout=timeout_s)
    except Exception as exc:
        _camoufox_debug(
            "context_browser_new_page_failed",
            launch_phase=phase,
            elapsed_ms=int((time.perf_counter() - started) * 1000) if started else 0,
            error_type=type(exc).__name__,
            error_kind=_launch_error_kind(exc, None),
            error_summary=_safe_text(exc),
            **diagnostics,
        )
        raise
    ctx = page.context
    _camoufox_debug(
        "context_browser_new_page_ok",
        launch_phase=phase,
        context_page_count=_context_page_count(ctx),
        page_closed=False,
        page_url=_safe_page_url(page, 160),
        elapsed_ms=int((time.perf_counter() - started) * 1000) if started else 0,
        **diagnostics,
    )
    await _apply_context_init_scripts(ctx, init_scripts, timeout_s)
    await _apply_page_viewport_size(page, page_viewport, phase, min(5.0, timeout_s), started)
    return ctx, page, "browser_new_page_context"


async def _verify_page_privacy(page: Page, plan: dict[str, Any], diagnostics: dict[str, Any] | None = None) -> dict[str, Any]:
    _camoufox_debug("privacy_verify_begin", **_page_privacy_probe_state(page, diagnostics, "navigator_snapshot"), timeout_s=5.0)
    try:
        snapshot = await _await_no_cancel_wait(
            page.evaluate(
                """() => ({
                    userAgent: String(navigator.userAgent || ""),
                    userAgentLength: String(navigator.userAgent || "").length,
                    userAgentPrefix: String(navigator.userAgent || "").slice(0, 96),
                    appVersionPrefix: String(navigator.appVersion || "").slice(0, 96),
                    platform: String(navigator.platform || ""),
                    oscpu: String(navigator.oscpu || ""),
                    language: String(navigator.language || ""),
                    languages: Array.isArray(navigator.languages) ? navigator.languages.slice(0, 8) : [],
                    webdriver: navigator.webdriver === undefined ? "undefined" : String(navigator.webdriver),
                    rtcPeerConnection: typeof window.RTCPeerConnection,
                    mozRtcPeerConnection: typeof window.mozRTCPeerConnection,
                    mediaDevices: !!(navigator.mediaDevices && navigator.mediaDevices.enumerateDevices)
                })"""
            ),
            timeout=5.0,
        )
    except Exception as exc:
        _camoufox_debug(
            "privacy_verify_exception",
            **_page_privacy_probe_state(page, diagnostics, "navigator_snapshot"),
            error_type=type(exc).__name__,
            error_kind=_launch_error_kind(exc, bool((diagnostics or {}).get("browser_connected", True))),
            error_summary=_launch_error_summary(exc, "privacy_verify"),
            process_tree=_process_tree_snapshot(),
        )
        raise
    if not isinstance(snapshot, dict):
        _camoufox_debug(
            "privacy_verify_invalid_snapshot",
            **_page_privacy_probe_state(page, diagnostics, "navigator_snapshot"),
            snapshot_type=type(snapshot).__name__,
        )
        raise RuntimeError("Camoufox privacy verification returned invalid data")
    expected_ua = plan.get("user_agent")
    ua = str(snapshot.get("userAgent") or "")
    ua_ok = not expected_ua or ua == expected_ua
    overrides = dict(plan.get("fingerprint_overrides") or {})
    expected_platform = overrides.get("navigator.platform")
    expected_oscpu = overrides.get("navigator.oscpu")
    expected_app_version = str(expected_ua).removeprefix("Mozilla/") if expected_ua else None
    app_version = str(snapshot.get("appVersionPrefix") or "")
    page_diag = dict(diagnostics or {})
    platform_ok = not expected_platform or str(snapshot.get("platform") or "") == expected_platform
    oscpu_ok = not expected_oscpu or str(snapshot.get("oscpu") or "") == expected_oscpu
    app_version_ok = not expected_app_version or app_version == expected_app_version[:96]
    webrtc_blocked = (
        str(snapshot.get("rtcPeerConnection")) == "undefined"
        and str(snapshot.get("mozRtcPeerConnection")) == "undefined"
    )
    _camoufox_debug("privacy_verify_probe_begin", **_page_privacy_probe_state(page, diagnostics, "webrtc_ice"), timeout_s=5.0)
    try:
        ice_probe = await _probe_webrtc_ice_leak(page)
    except Exception as exc:
        _camoufox_debug(
            "privacy_verify_exception",
            **_page_privacy_probe_state(page, diagnostics, "webrtc_ice"),
            error_type=type(exc).__name__,
            error_kind=_launch_error_kind(exc, bool((diagnostics or {}).get("browser_connected", True))),
            error_summary=_launch_error_summary(exc, "privacy_verify"),
            process_tree=_process_tree_snapshot(),
        )
        raise
    webdriver_ok = str(snapshot.get("webdriver")) != "true"
    info = {
        "ua_policy": plan.get("ua_policy"),
        "effective_ua_policy": plan.get("ua_policy"),
        "ua_override": bool(expected_ua),
        "ua_override_string": str(expected_ua or ""),
        "expected_user_agent": str(expected_ua or ""),
        "actual_user_agent": ua,
        "ua_ok": ua_ok,
        "user_agent": ua,
        "ua_len": int(snapshot.get("userAgentLength") or len(ua)),
        "ua_prefix": str(snapshot.get("userAgentPrefix") or "")[:96],
        "expected_app_version_prefix": str(expected_app_version or "")[:96],
        "actual_app_version_prefix": str(snapshot.get("appVersionPrefix") or "")[:96],
        "app_version_prefix": str(snapshot.get("appVersionPrefix") or "")[:96],
        "app_version_ok": app_version_ok,
        "expected_platform": str(expected_platform or ""),
        "actual_platform": str(snapshot.get("platform") or ""),
        "platform": str(snapshot.get("platform") or ""),
        "platform_ok": platform_ok,
        "expected_oscpu": str(expected_oscpu or ""),
        "actual_oscpu": str(snapshot.get("oscpu") or ""),
        "oscpu": str(snapshot.get("oscpu") or ""),
        "oscpu_ok": oscpu_ok,
        "language": str(snapshot.get("language") or ""),
        "languages": snapshot.get("languages") if isinstance(snapshot.get("languages"), list) else [],
        "webrtc_blocked": webrtc_blocked,
        "rtc_peer_connection": str(snapshot.get("rtcPeerConnection")),
        "moz_rtc_peer_connection": str(snapshot.get("mozRtcPeerConnection")),
        "media_devices": bool(snapshot.get("mediaDevices")),
        "ice_probe": ice_probe,
        "ice_probe_ok": bool(ice_probe.get("ok")),
        "ice_probe_status": str(ice_probe.get("status") or ""),
        "ice_probe_blocked": bool(ice_probe.get("blocked")),
        "ice_candidate_count": int(ice_probe.get("candidate_count") or 0),
        "ice_candidate_ip_count": len(ice_probe.get("ip_candidates") or []),
        "ice_host_ip_candidate_count": len(ice_probe.get("host_ip_candidates") or []),
        "ice_private_ip_candidate_count": len(ice_probe.get("private_ip_candidates") or []),
        "ice_public_ip_candidate_count": len(ice_probe.get("public_ip_candidates") or []),
        "ice_candidate_leak_detected": bool(ice_probe.get("leak_detected")),
        "webdriver": str(snapshot.get("webdriver")),
        "webdriver_ok": webdriver_ok,
        "page_url": _safe_page_url(page),
        "page_reused": bool(page_diag.get("page_reused")),
        "page_fresh": bool(page_diag.get("page_fresh")),
        "init_script_installed": bool(page_diag.get("init_script_installed")),
        "persistent_context": bool(page_diag.get("persistent_context")),
        "context_source": str(page_diag.get("context_source") or ""),
    }
    if plan.get("privacy_fail_closed", True):
        failures: list[tuple[str, str]] = []
        if expected_ua and not ua_ok:
            failures.append(("user_agent", "user agent override mismatch"))
        if expected_app_version and not app_version_ok:
            failures.append(("app_version", "appVersion override mismatch"))
        if expected_platform and not platform_ok:
            failures.append(("platform", "platform override mismatch"))
        if expected_oscpu and not oscpu_ok:
            failures.append(("oscpu", "oscpu override mismatch"))
        if plan.get("block_webrtc", True) and not webrtc_blocked:
            failures.append(("webrtc_exposed", "WebRTC is exposed"))
        if plan.get("block_webrtc", True) and not bool(ice_probe.get("ok")):
            failures.append(("ice_probe", "WebRTC ICE candidate probe failed closed"))
        if plan.get("block_webrtc", True) and bool(ice_probe.get("leak_detected")):
            failures.append(("ice_leak", "WebRTC ICE candidate leak detected"))
        if not webdriver_ok:
            failures.append(("webdriver", "webdriver is exposed"))
        if failures:
            _camoufox_debug(
                "privacy_verify_failed",
                failures=[item[0] for item in failures],
                failure_messages=[item[1] for item in failures],
                error_kind="privacy_assertion_failed",
                probe_state=_page_privacy_probe_state(page, diagnostics, "assertions"),
                **info,
            )
            raise RuntimeError(f"Camoufox privacy verification failed: {failures[0][1]}")
    _camoufox_debug(
        "privacy_verify_ok",
        **_page_privacy_probe_state(page, diagnostics, "complete"),
        webrtc_blocked=bool(info.get("webrtc_blocked")),
        ice_probe_ok=bool(info.get("ice_probe_ok")),
        webdriver_ok=bool(info.get("webdriver_ok")),
    )
    return info


class BrowserManager:
    """Manages the Camoufox browser lifecycle, contexts, and pages."""

    default_config: dict[str, Any] = {}

    def __init__(self) -> None:
        self.browser = None
        self.contexts: dict[str, BrowserContext] = {}
        self.pages: dict[str, Page] = {}
        self.page_meta: dict[str, dict[str, Any]] = {}
        self.context_ids: dict[int, str] = {}
        self._page_guid_to_id: dict[str, str] = {}
        self._listener_page_ids: set[str] = set()
        self._page_terminal_ids: set[str] = set()
        self._pending_page_ids_by_context: dict[str, list[dict[str, Any]]] = {}
        self._page_counter = 0
        self.active_page_name: str | None = None
        self.active_page_id: str | None = None
        self.session_id: str = _os.environ.get("AIDA_CAMOUFOX_SESSION_ID", "default") or "default"
        self._aida_multipage_patch = 4
        self._cm = None  # AsyncCamoufox context manager
        self._playwright = None
        self._console_logs: deque[dict] = deque(maxlen=MAX_LOG_SIZE)
        self._network_requests: deque[dict] = deque(maxlen=MAX_LOG_SIZE)
        self._request_id_counter = 0
        self._capturing = False
        self._capture_pattern: str = "**/*"
        self._capture_body = False
        self._init_scripts: list[str] = []
        self._persistent_scripts: list[dict] = []
        self._persistent_traces: dict[str, list] = {}
        self._nav_responses: list[dict] = []  # 最近一次 navigate 记录到的响应链路
        self._nav_responses_by_page: dict[str, list[dict]] = {}
        self._route_handlers: dict[str, Any] = {}  # 已注册的 route handler 映射
        self._profile_dir: str | None = None
        self._profile_generated = False
        self._context_plan: dict[str, Any] = {}
        self._browser_lifecycle_listener_ids: set[int] = set()
        self._diagnostic_generation = 0
        self._browser_generation = 0
        self._operation_counter = 0
        self._active_page_operations: dict[str, dict[str, Any]] = {}
        self._last_launch_summary: dict[str, Any] = {}
        self._page_event_counter = 0
        self._recent_page_events: deque[dict[str, Any]] = deque(maxlen=160)
        self._page_recovery_task: asyncio.Task | None = None
        self._last_error = ""
        self._last_launch_failure: dict[str, Any] = {}
        self._active_launch_phase = ""
        self._active_launch_started = 0.0
        self._active_launch_attempt_id = ""
        self._active_launch_privacy_verified = False
        self._active_launch_terminal_reason = ""
        self._active_launch_terminal_event: asyncio.Event | None = None
        self._active_launch_terminal_payload: dict[str, Any] = {}
        self._last_successful_launch_config: dict[str, Any] = {}

    async def launch(self, config: dict | None = None) -> dict:
        """Launch the Camoufox browser with the given or default config."""
        global _ACTIVE_LAUNCH_MANAGER
        launch_started = time.perf_counter()
        cfg = {**self.default_config, **(config or {})}
        self._last_launch_failure = {}
        self._active_launch_privacy_verified = False
        self._active_launch_terminal_reason = ""
        self._active_launch_terminal_payload = {}
        self._active_launch_terminal_event = asyncio.Event()
        _ACTIVE_LAUNCH_MANAGER = self
        self._set_launch_phase("entry", launch_started, cfg.get("bridge_attempt_id"))
        diagnostic_variant = str(cfg.get("aida_diagnostic_variant") or "").strip().lower()
        diagnostic_minimal_prefs = diagnostic_variant in {"minimal_prefs", "original_style_bundled"}
        diagnostic_original_style = diagnostic_variant == "original_style_bundled"
        _camoufox_debug(
            "aida_bridge_patch_active",
            patch_version=AIDA_CAMOUFOX_BRIDGE_PATCH_ID,
            default_launch_path="async_camoufox",
            fast_visible_default=False,
            service_workers_default="allow",
            ua_policy_default="camoufox_native",
            request_marker=_safe_text(cfg.get("aida_launch_policy_marker")),
            config_keys=sorted(str(k) for k in cfg.keys()),
            diagnostic_variant=diagnostic_variant,
        )
        requested_context_plan = _preflight_context_plan(cfg)
        if self.browser is not None:
            context_mismatch_reason = _context_plan_mismatch_reason(self._context_plan, requested_context_plan)
            if context_mismatch_reason:
                _camoufox_debug(
                    "launch_existing_config_mismatch",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    reason=context_mismatch_reason,
                    current_ua_policy=self._context_plan.get("ua_policy"),
                    requested_ua_policy=requested_context_plan.get("ua_policy"),
                    current_ua_override=bool(self._context_plan.get("user_agent")),
                    requested_ua_override=bool(requested_context_plan.get("user_agent")),
                    current_proxy=bool(self._context_plan.get("proxy")),
                    requested_proxy=bool(requested_context_plan.get("proxy")),
                    current_persistent=bool(self._context_plan.get("persistent_context")),
                    requested_persistent=bool(requested_context_plan.get("persistent_context")),
                    current_profile_dir=bool(self._context_plan.get("profile_dir")),
                    requested_profile_dir=bool(requested_context_plan.get("profile_dir")),
                    current_user_data_dir=bool(self._context_plan.get("user_data_dir")),
                    requested_user_data_dir=bool(requested_context_plan.get("user_data_dir")),
                )
                await self.close()
            else:
                pages_info = await self.list_pages()
                active_page = await self.resolve_page(None)
                active_bounds = await self._page_bounds_limited(active_page) if active_page else {}
                privacy_info = await _verify_page_privacy(
                    active_page,
                    self._context_plan,
                    {
                        "page_reused": True,
                        "page_fresh": False,
                        "init_script_installed": bool(_privacy_override_script(self._context_plan)),
                        "persistent_context": bool(self._context_plan.get("persistent_context")),
                        "context_source": "already_running",
                    },
                ) if active_page and self._context_plan else {}
                elapsed_ms = int((time.perf_counter() - launch_started) * 1000)
                visible_window_probe = _visible_window_snapshot()
                _camoufox_debug(
                    "launch_already_running",
                    elapsed_ms=elapsed_ms,
                    pages=len(self.pages),
                    contexts=len(self.contexts),
                    browser_open=True,
                    privacy=privacy_info,
                    visible_window_probe=visible_window_probe,
                )
                self._active_launch_phase = ""
                self._active_launch_started = 0.0
                self._active_launch_attempt_id = ""
                self._active_launch_privacy_verified = False
                self._active_launch_terminal_reason = ""
                self._active_launch_terminal_payload = {}
                self._active_launch_terminal_event = None
                if _ACTIVE_LAUNCH_MANAGER is self:
                    _ACTIVE_LAUNCH_MANAGER = None
                return {
                    "status": "already_running",
                    "session_id": self.session_id,
                    "active_page": self.active_page_id or self.active_page_name,
                    "active_page_id": self.active_page_id or self.active_page_name,
                    "page_count": len(self.pages),
                    "pages": pages_info,
                    "contexts": list(self.contexts.keys()),
                    "capturing": self._capturing,
                    "diagnostics": {
                        "elapsed_ms": elapsed_ms,
                        "page_bounds": active_bounds,
                        "pages": len(self.pages),
                        "contexts": len(self.contexts),
                        "privacy": privacy_info,
                        "visible_window_probe": visible_window_probe,
                        "process_tree": _process_tree_snapshot(),
                        "subprocesses": _subprocess_diagnostics_snapshot(),
                    },
                }

        if self.browser is not None:
            _camoufox_debug(
                "launch_existing_close_failed",
                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                pages=len(self.pages),
                contexts=len(self.contexts),
                browser_open=True,
            )
            return {"error": "browser close did not release active instance"}

        self._browser_generation += 1
        _camoufox_debug(
            "browser_generation_begin",
            session_id=self.session_id,
            browser_generation=self._browser_generation,
            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
            process_tree=_process_tree_detailed_snapshot(),
        )

        kwargs: dict[str, Any] = {}

        proxy_cfg = _normalize_proxy(cfg.get("proxy"))
        if proxy_cfg:
            cfg["proxy"] = proxy_cfg
            kwargs["proxy"] = proxy_cfg

        os_requested = cfg.get("os", cfg.get("os_type", "auto"))
        host_os = detect_host_os()
        os_type = host_os if os_requested == "auto" else os_requested

        if cfg.get("humanize"):
            kwargs["humanize"] = True
        if cfg.get("geoip"):
            kwargs["geoip"] = True
        if cfg.get("block_images"):
            kwargs["block_images"] = True
        cfg["block_webrtc"] = True
        kwargs["block_webrtc"] = True

        executable_path = cfg.get("executable_path") or __import__("os").environ.get("AIDA_CAMOUFOX_EXECUTABLE")
        if executable_path:
            executable_path = _os.path.abspath(_os.path.expandvars(_os.path.expanduser(str(executable_path))))
            exe_info = _path_info(executable_path)
            if not exe_info.get("is_file"):
                _camoufox_debug("launch_executable_invalid", executable=exe_info, cwd=_os.getcwd())
                raise FileNotFoundError(f"Camoufox executable is not available: {executable_path}")
            kwargs["executable_path"] = executable_path

        ff_version = cfg.get("ff_version")
        ff_version_source = "none"
        if ff_version is not None:
            try:
                kwargs["ff_version"] = int(ff_version)
                kwargs["i_know_what_im_doing"] = True
                ff_version_source = "config"
            except (TypeError, ValueError):
                _camoufox_debug("launch_ff_version_invalid", value_type=type(ff_version).__name__)
        if "ff_version" not in kwargs and executable_path:
            derived_ff_version = _derive_ff_version(executable_path)
            if derived_ff_version is not None:
                kwargs["ff_version"] = derived_ff_version
                kwargs["i_know_what_im_doing"] = True
                ff_version_source = "executable_path"

        locale_requested = cfg.get("locale", "auto")
        locale = detect_system_locale() if locale_requested == "auto" else locale_requested

        headless = cfg.get("headless", False)
        kwargs["headless"] = headless

        bundled_visible_launch = bool(executable_path) and not headless
        if not bundled_visible_launch or os_requested != "auto":
            kwargs["os"] = os_type
        if not bundled_visible_launch or locale_requested != "auto":
            kwargs["locale"] = locale

        profile_dir = None
        profile_info: dict[str, Any] = {}
        profile_requested = bool(cfg.get("profile_dir") or cfg.get("user_data_dir") or cfg.get("persistent_context"))
        if profile_requested and not bundled_visible_launch:
            raise ValueError("persistent Camoufox context requires a visible AiDA-controlled browser executable")
        if bundled_visible_launch:
            prefs = _minimal_firefox_user_prefs() if diagnostic_minimal_prefs else _default_firefox_user_prefs()
            if isinstance(cfg.get("firefox_user_prefs"), dict):
                prefs.update(cfg["firefox_user_prefs"])
            kwargs["firefox_user_prefs"] = prefs
            if profile_requested:
                generated_profile = not bool(cfg.get("profile_dir") or cfg.get("user_data_dir"))
                profile_dir = str(cfg.get("profile_dir") or cfg.get("user_data_dir") or _new_profile_dir())
                profile_dir, profile_info = _prepare_profile_dir(profile_dir, generated_profile)
                kwargs["persistent_context"] = True
                kwargs["user_data_dir"] = profile_dir
                profile_pref_info = _write_user_js_prefs(profile_dir, prefs)
                _camoufox_debug("launch_profile_prefs", **profile_pref_info)
            else:
                profile_info = {"profile_dir": "", "generated": False, "existed": False, "locks": 0, "lock_names": [], "mode": "non_persistent"}
                _camoufox_debug("launch_ephemeral_prefs", prefs=sorted(prefs.keys()))

        window_size, window_diag = _resolve_window_size(cfg)
        if not headless:
            kwargs["window"] = window_size
        fast_probe = bool(cfg.get("aida_testlab_fast_probe") or cfg.get("testlab_fast_probe")) or str(_os.environ.get("AIDA_CAMOUFOX_TESTLAB_FAST_PROBE", "")).lower() in {"1", "true", "yes", "on"}
        launch_budget_policy = aida_resolve_launch_budget_policy(
            cfg.get("launch_timeout_ms"),
            bundled_visible_launch=bundled_visible_launch,
            fast_probe=fast_probe,
        )
        launch_timeout_ms = int(launch_budget_policy["launch_timeout_ms"])
        launch_timeout_s = float(launch_budget_policy["launch_timeout_s"])
        context_enter_timeout_s = float(launch_budget_policy["context_enter_timeout_s"])
        context_create_timeout_s = float(launch_budget_policy["context_create_timeout_s"])
        page_create_floor_s = float(launch_budget_policy["phase_policy"]["page_create_floor_s"])
        page_create_ceiling_s = float(launch_budget_policy["phase_policy"]["page_create_cap_s"])
        page_create_timeout_s = float(launch_budget_policy["page_create_timeout_s"])
        persistent_page_wait_s = float(launch_budget_policy["persistent_page_wait_s"])
        persistent_page_create_timeout_s = float(launch_budget_policy["persistent_page_create_timeout_s"])
        persistent_page_nav_timeout_s = float(launch_budget_policy["persistent_page_nav_timeout_s"])
        late_page_wait_s = float(launch_budget_policy["late_page_wait_s"])
        _camoufox_debug(
            "launch_budget_allocation",
            launch_budget_policy_marker=AIDA_LAUNCH_BUDGET_POLICY_MARKER,
            launch_budget_policy=launch_budget_policy,
            launch_budget_policy_invariants=aida_validate_launch_budget_policy()["invariants"],
            launch_timeout_ms=launch_timeout_ms,
            launch_timeout_s=launch_timeout_s,
            fast_probe=bool(fast_probe),
            bundled_visible_launch=bool(bundled_visible_launch),
            context_enter_timeout_s=context_enter_timeout_s,
            context_create_timeout_s=context_create_timeout_s,
            page_create_floor_s=page_create_floor_s,
            page_create_ceiling_s=page_create_ceiling_s,
            page_create_timeout_s=page_create_timeout_s,
            persistent_page_wait_s=persistent_page_wait_s,
            persistent_page_create_timeout_s=persistent_page_create_timeout_s,
            persistent_page_nav_timeout_s=persistent_page_nav_timeout_s,
            late_page_wait_s=late_page_wait_s,
        )
        context_plan = _build_context_plan(cfg, os_type, locale, locale_requested, kwargs.get("ff_version"))
        context_plan["diagnostic_variant"] = diagnostic_variant
        context_plan["page_viewport"] = {"width": int(window_size[0]), "height": int(window_size[1])}
        context_plan["launch_attempt_id"] = str(cfg.get("bridge_attempt_id") or "")
        context_plan["bridge_generation"] = cfg.get("bridge_generation")
        fast_visible_launch = _use_fast_visible_launch(cfg, bundled_visible_launch, profile_requested)
        fast_visible_requested = _flag_enabled(cfg.get("aida_fast_visible_launch"))
        fast_visible_env_requested = _flag_enabled(_os.environ.get("AIDA_CAMOUFOX_FAST_VISIBLE_FALLBACK"))
        if fast_visible_launch or fast_visible_requested or fast_visible_env_requested:
            _camoufox_debug(
                "aida_fast_visible_fallback_ignored",
                marker=AIDA_FAST_VISIBLE_POLICY_MARKER,
                patch_version=AIDA_CAMOUFOX_BRIDGE_PATCH_ID,
                requested=bool(fast_visible_requested),
                env=bool(fast_visible_env_requested),
                forced_launch_path="async_camoufox",
            )
        fast_visible_launch = False
        fast_visible_source = "default"
        if cfg.get("aida_fast_visible_launch") is not None:
            fast_visible_source = "request"
        elif _os.environ.get("AIDA_CAMOUFOX_FAST_VISIBLE_FALLBACK") is not None:
            fast_visible_source = "env"
        selected_launch_path = "async_camoufox"
        if diagnostic_original_style:
            fast_visible_launch = False
            selected_launch_path = "diagnostic_original_style_bundled"
        _camoufox_debug(
            "aida_launch_policy_resolved",
            patch_version=AIDA_CAMOUFOX_BRIDGE_PATCH_ID,
            selected_launch_path=selected_launch_path,
            fast_visible_source=fast_visible_source,
            bundled_visible=bool(bundled_visible_launch),
            profile_requested=bool(profile_requested),
            fast_visible_enabled=bool(fast_visible_launch),
            fast_visible_requested=_safe_text(cfg.get("aida_fast_visible_launch")),
            fast_visible_env=bool(_os.environ.get("AIDA_CAMOUFOX_FAST_VISIBLE_FALLBACK")),
            block_service_workers=bool(_flag_enabled(cfg.get("block_service_workers"))),
            service_workers=_safe_text(cfg.get("service_workers")),
            service_workers_default="allow",
            context_service_workers=context_plan.get("context_options", {}).get("service_workers", ""),
            ua_policy=context_plan.get("ua_policy"),
            ua_override=bool(context_plan.get("user_agent")),
            request_marker=_safe_text(cfg.get("aida_launch_policy_marker")),
            diagnostic_variant=diagnostic_variant,
        )
        if fast_visible_launch:
            context_plan = _fast_visible_privacy_plan(context_plan)
            _camoufox_debug(
                "aida_fast_visible_fallback_ignored_after_plan",
                requested=cfg.get("aida_fast_visible_launch"),
                env=bool(_os.environ.get("AIDA_CAMOUFOX_FAST_VISIBLE_FALLBACK")),
                ua_policy=context_plan.get("ua_policy"),
                service_workers=context_plan.get("context_options", {}).get("service_workers", "allow"),
            )
        self._remember_launch_summary(
            selected_launch_path,
            executable_path,
            profile_info,
            kwargs.get("firefox_user_prefs") or {},
            None,
            context_plan,
            window_diag,
            launch_timeout_ms,
            kwargs.get("ff_version"),
            ff_version_source,
        )
        if context_plan.get("user_agent"):
            camoufox_config = dict(kwargs.get("config") or {})
            camoufox_config.update(dict(context_plan.get("fingerprint_overrides") or {}))
            kwargs["config"] = camoufox_config
        launch_phase = "configured"
        self._set_launch_phase(launch_phase, launch_started, cfg.get("bridge_attempt_id"))

        _camoufox_debug(
            "launch_start",
            headless=bool(headless),
            os=os_type,
            locale=locale,
            has_proxy=bool(cfg.get("proxy")),
            humanize=bool(cfg.get("humanize")),
            geoip=bool(cfg.get("geoip")),
            block_images=bool(cfg.get("block_images")),
            block_webrtc=bool(context_plan.get("block_webrtc")),
            enable_trace=bool(cfg.get("enable_trace")),
            window=window_diag,
            persistent=bool(kwargs.get("persistent_context")),
            timeout_ms=launch_timeout_ms,
            remaining_budget_ms=launch_timeout_ms,
            fast_probe=fast_probe,
            executable=_path_info(executable_path),
            profile=profile_info,
            ff_version=kwargs.get("ff_version"),
            ff_version_source=ff_version_source,
            cwd=_os.getcwd(),
            python=sys.executable,
            argv_len=len(sys.argv),
            env={
                "debug_log": bool(_os.environ.get("AIDA_CAMOUFOX_DEBUG_LOG")),
                "profile_root": bool(_os.environ.get("AIDA_CAMOUFOX_PROFILE_ROOT")),
                "browser_executable": bool(__import__("os").environ.get("AIDA_CAMOUFOX_EXECUTABLE")),
                "pythonio": bool(_os.environ.get("PYTHONIOENCODING")),
                "localappdata": bool(_os.environ.get("LOCALAPPDATA")),
                "temp": bool(_os.environ.get("TEMP")),
            },
            prefs=len(kwargs.get("firefox_user_prefs") or {}),
            ua_policy=context_plan.get("ua_policy"),
            ua_override=bool(context_plan.get("user_agent")),
            service_workers=context_plan.get("context_options", {}).get("service_workers", ""),
            patch_version=AIDA_CAMOUFOX_BRIDGE_PATCH_ID,
            selected_launch_path=selected_launch_path,
            fast_visible_requested=_safe_text(cfg.get("aida_fast_visible_launch")),
            fast_visible_env=bool(_os.environ.get("AIDA_CAMOUFOX_FAST_VISIBLE_FALLBACK")),
            service_workers_default="allow",
            request_marker=_safe_text(cfg.get("aida_launch_policy_marker")),
            launch_summary=self._last_launch_summary,
            diagnostic_variant=diagnostic_variant,
        )

        enable_trace = cfg.get("enable_trace", False)

        from_options = None
        if executable_path and not fast_visible_launch:
            launch_phase = "bundled_options"
            _camoufox_debug(
                "launch_bundled_options_begin",
                executable_path=str(executable_path),
                persistent=bool(kwargs.get("persistent_context")),
                profile_dir=profile_dir or "",
                timeout_ms=launch_timeout_ms,
                fast_probe=fast_probe,
            )
            from_options = _build_camoufox_launch_options(headless, kwargs)
            env_sanitized = _sanitize_browser_launch_env(from_options)
            if env_sanitized.get("removed"):
                _camoufox_debug("launch_browser_env_sanitized", **env_sanitized)
            if kwargs.get("persistent_context"):
                for key, value in dict(context_plan.get("context_options") or {}).items():
                    from_options[key] = value
            kwargs["from_options"] = from_options
            self._remember_launch_summary(
                selected_launch_path,
                executable_path,
                profile_info,
                kwargs.get("firefox_user_prefs") or from_options.get("firefox_user_prefs") or {},
                _launch_policy_info(from_options),
                context_plan,
                window_diag,
                launch_timeout_ms,
                kwargs.get("ff_version"),
                ff_version_source,
            )
            _camoufox_debug(
                "launch_bundled_options",
                executable_path=str(executable_path),
                from_options_has_executable=bool(from_options.get("executable_path")),
                from_options_args=len(from_options.get("args") or []),
                from_options_has_env=bool(from_options.get("env")),
                launch_policy=_launch_policy_info(from_options),
                launch_summary=self._last_launch_summary,
                persistent=bool(kwargs.get("persistent_context")),
                profile_dir=profile_dir or "",
                ff_version=kwargs.get("ff_version"),
                prefs=len(from_options.get("firefox_user_prefs") or {}),
            )

        if enable_trace:
            launch_phase = "trace_setup"
            trace_started = time.perf_counter()
            _camoufox_debug("launch_trace_setup_begin", elapsed_ms=int((time.perf_counter() - launch_started) * 1000), fast_probe=fast_probe)
            try:
                from .property_trace import build_property_trace_config, ensure_dirs, cleanup_old_traces, cleanup_traces, CACHE_DIR
                import json as _json
                from functools import partial
                _camoufox_debug("launch_trace_import_ok", elapsed_ms=int((time.perf_counter() - trace_started) * 1000))
                ensure_dirs()
                _camoufox_debug("launch_trace_dirs_ok", elapsed_ms=int((time.perf_counter() - trace_started) * 1000))
                cleanup_old_traces(keep_days=7)
                _camoufox_debug("launch_trace_cleanup_old_ok", elapsed_ms=int((time.perf_counter() - trace_started) * 1000))
                cleanup_traces()
                _camoufox_debug("launch_trace_cleanup_current_ok", elapsed_ms=int((time.perf_counter() - trace_started) * 1000))
                values_dir = CACHE_DIR / "values"
                values_removed = 0
                if values_dir.exists():
                    for f in values_dir.glob("*"):
                        try:
                            f.unlink()
                            values_removed += 1
                        except Exception:
                            pass
                _camoufox_debug("launch_trace_values_cleanup_ok", elapsed_ms=int((time.perf_counter() - trace_started) * 1000), values_removed=values_removed)
                trace_config = build_property_trace_config()
                _camoufox_debug("launch_trace_config_ok", elapsed_ms=int((time.perf_counter() - trace_started) * 1000))

                if from_options is None:
                    from_options = _build_camoufox_launch_options(headless, kwargs)
                env = from_options.get("env", {})
                merged = False
                for key in sorted(env.keys()):
                    if key.startswith("CAMOU_CONFIG"):
                        try:
                            existing = _json.loads(env[key])
                            existing["propertyTrace"] = trace_config
                            env[key] = _json.dumps(existing)
                            merged = True
                            break
                        except (ValueError, TypeError):
                            pass
                if not merged:
                    env["CAMOU_CONFIG"] = _json.dumps({"propertyTrace": trace_config})
                env["MOZ_DISABLE_CONTENT_SANDBOX"] = "1"
                from_options["env"] = env

                kwargs["from_options"] = from_options
                self._remember_launch_summary(
                    selected_launch_path,
                    executable_path,
                    profile_info,
                    kwargs.get("firefox_user_prefs") or from_options.get("firefox_user_prefs") or {},
                    _launch_policy_info(from_options),
                    context_plan,
                    window_diag,
                    launch_timeout_ms,
                    kwargs.get("ff_version"),
                    ff_version_source,
                )
                _camoufox_debug("launch_trace_setup_ok", elapsed_ms=int((time.perf_counter() - trace_started) * 1000), merged=bool(merged), env_keys=len(env))
            except Exception as exc:
                _camoufox_debug(
                    "launch_trace_setup_failed",
                    elapsed_ms=int((time.perf_counter() - trace_started) * 1000),
                    error_type=type(exc).__name__,
                    error_len=len(str(exc)),
                    error_summary=_safe_text(exc),
                )
                raise

        ctx: BrowserContext | None = None
        page: Page | None = None
        context_source = ""
        browser_ready_ms = 0
        camoufox_launch_ms = 0
        try:
            self._profile_dir = profile_dir
            self._profile_generated = bool(profile_info.get("generated"))
            if fast_visible_launch:
                launch_phase = "fast_visible_playwright"
                context_plan = _fast_visible_privacy_plan(context_plan)
                fast_prefs = _fast_visible_firefox_user_prefs(kwargs.get("firefox_user_prefs") or {}, context_plan)
                self._remember_launch_summary(
                    selected_launch_path,
                    executable_path,
                    profile_info,
                    fast_prefs,
                    _launch_policy_info(from_options),
                    context_plan,
                    window_diag,
                    launch_timeout_ms,
                    kwargs.get("ff_version"),
                    ff_version_source,
                )
                _camoufox_debug(
                    "launch_fast_visible_begin",
                    timeout_ms=launch_timeout_ms,
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    executable=_path_info(executable_path),
                    prefs=len(fast_prefs),
                    prefs_summary=_prefs_summary(fast_prefs),
                    ua_policy=context_plan.get("ua_policy"),
                    ua_override=bool(context_plan.get("user_agent")),
                    proxy=bool(context_plan.get("proxy")),
                    window=window_diag,
                    launch_policy=_launch_policy_info(from_options),
                    launch_summary=self._last_launch_summary,
                    process_tree=_process_tree_snapshot(),
                )
                from playwright.async_api import async_playwright
                playwright_started = time.perf_counter()
                self._playwright = await _await_no_cancel_wait(async_playwright().start(), timeout=min(5.0, launch_timeout_s))
                _camoufox_debug(
                    "launch_fast_visible_playwright_ok",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    phase_elapsed_ms=int((time.perf_counter() - playwright_started) * 1000),
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                )
                launch_kwargs: dict[str, Any] = {
                    "executable_path": executable_path,
                    "headless": False,
                    "firefox_user_prefs": fast_prefs,
                    "timeout": launch_timeout_ms,
                }
                if proxy_cfg:
                    launch_kwargs["proxy"] = proxy_cfg
                browser_started = time.perf_counter()
                self.browser = await _await_no_cancel_wait(
                    self._playwright.firefox.launch(**launch_kwargs),
                    timeout=self._launch_timeout_s(launch_started, launch_timeout_ms, "launch_fast_visible_browser"),
                )
                self._attach_browser_lifecycle_listeners()
                browser_ready_at = time.perf_counter()
                browser_ready_ms = int((browser_ready_at - launch_started) * 1000)
                camoufox_launch_ms = int((browser_ready_at - browser_started) * 1000)
                _camoufox_debug(
                    "launch_fast_visible_browser_ok",
                    elapsed_ms=browser_ready_ms,
                    phase_elapsed_ms=camoufox_launch_ms,
                    camoufox_launch_ms=camoufox_launch_ms,
                    runtime=_browser_runtime_snapshot(self.browser),
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    state=await self._launch_debug_snapshot("fast_visible_browser", launch_started, launch_timeout_ms),
                )
                launch_phase = "fast_visible_page"
                page_options = _fast_visible_page_options(context_plan, window_size)
                _camoufox_debug(
                    "launch_fast_visible_page_begin",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    option_keys=sorted(str(k) for k in page_options.keys()),
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms),
                )
                page_started = time.perf_counter()
                ctx, _, _ = await _create_camoufox_safe_context(
                    self.browser,
                    page_options,
                    self._launch_timeout_s(launch_started, launch_timeout_ms, "launch_fast_visible_context"),
                    "launch_fast_visible_context",
                    window_size,
                    launch_started,
                )
                self.contexts["default"] = ctx
                self._register_context("default", ctx)
                _camoufox_debug(
                    "launch_fast_visible_context_created",
                    session_id=self.session_id,
                    context_id="default",
                    ctx_pages=_context_page_count(ctx),
                    browser_context_count=_browser_context_count(self.browser),
                    registered_contexts=len(self.contexts),
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                )
                context_source = "fast_visible_firefox"
                page_from_browser_fallback = False
                existing_pages: list[Page] = []
                try:
                    existing_pages = list(ctx.pages)
                except Exception as exc:
                    _camoufox_debug(
                        "launch_fast_visible_existing_pages_snapshot_failed",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        error_type=type(exc).__name__,
                        error_len=len(str(exc)),
                        error_summary=_safe_text(exc),
                    )
                fast_page_remaining_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "launch_fast_visible_new_page_budget", reserve_ms=500)
                fast_page_timeout_s = min(
                    max(12.0, fast_page_remaining_s * 0.40),
                    max(1.0, fast_page_remaining_s - 18.0),
                    24.0,
                    fast_page_remaining_s,
                )
                fast_page_late_wait_s = min(4.0, self._launch_remaining_timeout_s(launch_started, launch_timeout_ms, reserve_ms=500))
                _camoufox_debug(
                    "launch_fast_visible_new_page_begin",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    existing_pages=len(existing_pages),
                    timeout_s=fast_page_timeout_s,
                    late_wait_s=fast_page_late_wait_s,
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    state=await self._launch_debug_snapshot("fast_visible_new_page", launch_started, launch_timeout_ms, ctx=ctx),
                )
                self._queue_pending_page_id("default", "default")
                pending_page_task: asyncio.Task | None = None
                try:
                    page, pending_page_task = await _await_late_result(ctx.new_page(), timeout=fast_page_timeout_s)
                    if pending_page_task is not None:
                        raise asyncio.TimeoutError
                except asyncio.TimeoutError as exc:
                    _camoufox_debug(
                        "launch_fast_visible_new_page_timeout",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        timeout_s=fast_page_timeout_s,
                        late_wait_s=fast_page_late_wait_s,
                        pending_page_task=_pending_page_task_state(pending_page_task),
                        process_tree=_process_tree_snapshot(),
                        remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                        state=await self._launch_debug_snapshot("fast_visible_new_page", launch_started, launch_timeout_ms, ctx=ctx, exc=exc),
                    )
                    fast_page_late_budget_s = min(fast_page_late_wait_s, self._launch_remaining_timeout_s(launch_started, launch_timeout_ms, reserve_ms=500))
                    page = await self._wait_for_late_page(ctx, existing_pages, fast_page_late_budget_s, "launch_fast_visible_new_page", launch_started, pending_page_task)
                    browser_page_fallback_available = hasattr(self.browser, "new_page")
                    if page is None and not browser_page_fallback_available:
                        page = await self._final_late_page_recovery(ctx, existing_pages, "launch_fast_visible_new_page_final", launch_started, launch_timeout_ms, pending_page_task)
                    elif page is None:
                        _camoufox_debug(
                            "launch_fast_visible_new_page_final_wait_skipped_for_fallback",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                            pending_page_task=_pending_page_task_state(pending_page_task),
                            process_tree=_process_tree_snapshot(),
                        )
                    if page is None:
                        _abandon_late_task(pending_page_task)
                        self._discard_pending_page_id("default", "default")
                        if not browser_page_fallback_available:
                            _camoufox_debug(
                                "launch_fast_visible_new_page_retry_stall",
                                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                                timeout_s=fast_page_timeout_s,
                                late_wait_s=fast_page_late_wait_s,
                                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                                state=await self._launch_debug_snapshot("fast_visible_new_page_retry_stall", launch_started, launch_timeout_ms, ctx=ctx, exc=exc),
                            )
                            raise
                        _camoufox_debug(
                            "launch_fast_visible_browser_page_fallback_begin",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            old_context_pages=len(existing_pages),
                            timeout_s=self._launch_remaining_timeout_s(launch_started, launch_timeout_ms),
                            remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                            state=await self._launch_debug_snapshot("fast_visible_browser_page_fallback", launch_started, launch_timeout_ms, ctx=ctx, exc=exc),
                        )
                        with contextlib.suppress(Exception):
                            close_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "launch_fast_visible_fallback_close", cap_s=5.0)
                            await _await_no_cancel_wait(ctx.close(), timeout=close_timeout_s)
                        self.context_ids.pop(id(ctx), None)
                        try:
                            ctx, page, context_source = await _create_private_browser_page_context(
                                self.browser,
                                context_plan,
                                self._launch_timeout_s(launch_started, launch_timeout_ms, "launch_fast_visible_browser_page_fallback"),
                                "launch_fast_visible_browser_page_fallback",
                                launch_started,
                            )
                        except Exception as fallback_exc:
                            _camoufox_debug(
                                "launch_fast_visible_browser_page_fallback_failed",
                                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                                error_type=type(fallback_exc).__name__,
                                error_len=len(str(fallback_exc)),
                                error_summary=_safe_text(fallback_exc),
                                process_tree=_process_tree_snapshot(),
                                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                                state=await self._launch_debug_snapshot("fast_visible_browser_page_fallback", launch_started, launch_timeout_ms, ctx=ctx, exc=fallback_exc),
                            )
                            raise
                        self.contexts["default"] = ctx
                        self._register_context("default", ctx)
                        _camoufox_debug(
                            "launch_fast_visible_fallback_context_created",
                            session_id=self.session_id,
                            context_id="default",
                            ctx_pages=_context_page_count(ctx),
                            browser_context_count=_browser_context_count(self.browser),
                            registered_contexts=len(self.contexts),
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        )
                        page_from_browser_fallback = True
                        fallback_nav_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "launch_fast_visible_fallback_nav", cap_s=15.0)
                        await _await_no_cancel_wait(
                            page.goto(PRIVACY_VERIFY_URL, wait_until="load", timeout=max(1, int(fallback_nav_timeout_s * 1000))),
                            timeout=fallback_nav_timeout_s,
                        )
                    else:
                        _camoufox_debug(
                            "launch_fast_visible_new_page_late_recovered",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                            state=await self._launch_debug_snapshot("fast_visible_late_recovered", launch_started, launch_timeout_ms, ctx=ctx, page=page, exc=exc),
                        )
                except Exception as exc:
                    _camoufox_debug(
                        "launch_fast_visible_new_page_failed",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        error_type=type(exc).__name__,
                        error_len=len(str(exc)),
                        error_summary=_safe_text(exc),
                        pending_page_task=_pending_page_task_state(pending_page_task),
                        process_tree=_process_tree_snapshot(),
                        remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                        state=await self._launch_debug_snapshot("fast_visible_new_page", launch_started, launch_timeout_ms, ctx=ctx, exc=exc),
                    )
                    fast_page_failed_late_budget_s = min(5.0, fast_page_late_wait_s, self._launch_remaining_timeout_s(launch_started, launch_timeout_ms, reserve_ms=500))
                    page = await self._wait_for_late_page(ctx, existing_pages, fast_page_failed_late_budget_s, "launch_fast_visible_new_page_failed", launch_started, pending_page_task)
                    browser_page_fallback_available = hasattr(self.browser, "new_page")
                    if page is None and not browser_page_fallback_available:
                        page = await self._final_late_page_recovery(ctx, existing_pages, "launch_fast_visible_new_page_failed_final", launch_started, launch_timeout_ms, pending_page_task)
                    elif page is None:
                        _camoufox_debug(
                            "launch_fast_visible_new_page_failed_final_wait_skipped_for_fallback",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                            pending_page_task=_pending_page_task_state(pending_page_task),
                            process_tree=_process_tree_snapshot(),
                        )
                    if page is None:
                        _abandon_late_task(pending_page_task)
                        self._discard_pending_page_id("default", "default")
                        if not browser_page_fallback_available:
                            _camoufox_debug(
                                "launch_fast_visible_new_page_retry_failure",
                                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                                error_type=type(exc).__name__,
                                error_len=len(str(exc)),
                                error_summary=_safe_text(exc),
                                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                                state=await self._launch_debug_snapshot("fast_visible_new_page_retry_failure", launch_started, launch_timeout_ms, ctx=ctx, exc=exc),
                            )
                            raise
                        _camoufox_debug(
                            "launch_fast_visible_browser_page_fallback_begin",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            old_context_pages=len(existing_pages),
                            timeout_s=self._launch_remaining_timeout_s(launch_started, launch_timeout_ms),
                            source_error=type(exc).__name__,
                            remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                            state=await self._launch_debug_snapshot("fast_visible_browser_page_fallback", launch_started, launch_timeout_ms, ctx=ctx, exc=exc),
                        )
                        with contextlib.suppress(Exception):
                            close_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "launch_fast_visible_fallback_close", cap_s=5.0)
                            await _await_no_cancel_wait(ctx.close(), timeout=close_timeout_s)
                        self.context_ids.pop(id(ctx), None)
                        try:
                            ctx, page, context_source = await _create_private_browser_page_context(
                                self.browser,
                                context_plan,
                                self._launch_timeout_s(launch_started, launch_timeout_ms, "launch_fast_visible_browser_page_fallback"),
                                "launch_fast_visible_browser_page_fallback",
                                launch_started,
                            )
                        except Exception as fallback_exc:
                            _camoufox_debug(
                                "launch_fast_visible_browser_page_fallback_failed",
                                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                                error_type=type(fallback_exc).__name__,
                                error_len=len(str(fallback_exc)),
                                error_summary=_safe_text(fallback_exc),
                                process_tree=_process_tree_snapshot(),
                                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                                state=await self._launch_debug_snapshot("fast_visible_browser_page_fallback", launch_started, launch_timeout_ms, ctx=ctx, exc=fallback_exc),
                            )
                            raise
                        self.contexts["default"] = ctx
                        self._register_context("default", ctx)
                        _camoufox_debug(
                            "launch_fast_visible_fallback_context_created",
                            session_id=self.session_id,
                            context_id="default",
                            ctx_pages=_context_page_count(ctx),
                            browser_context_count=_browser_context_count(self.browser),
                            registered_contexts=len(self.contexts),
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        )
                        page_from_browser_fallback = True
                        fallback_nav_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "launch_fast_visible_fallback_nav", cap_s=15.0)
                        await _await_no_cancel_wait(
                            page.goto(PRIVACY_VERIFY_URL, wait_until="load", timeout=max(1, int(fallback_nav_timeout_s * 1000))),
                            timeout=fallback_nav_timeout_s,
                        )
                finally:
                    self._discard_pending_page_id("default", "default")
                page_viewport_info = await _apply_plan_page_viewport_size(
                    page,
                    context_plan,
                    "launch_fast_visible_page",
                    5.0,
                    window_size,
                    launch_started,
                )
                self._context_plan = dict(context_plan)
                _camoufox_debug(
                    "launch_fast_visible_page_ok",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    phase_elapsed_ms=int((time.perf_counter() - page_started) * 1000),
                    browser_fallback=page_from_browser_fallback,
                    context_source=context_source,
                    viewport_policy=page_viewport_info,
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx, page=page),
                )
                launch_phase = "privacy_verify"
                privacy_info = await _verify_page_privacy(
                    page,
                    context_plan,
                    {
                        "page_reused": False,
                        "page_fresh": True,
                        "init_script_installed": False,
                        "persistent_context": False,
                        "context_source": context_source,
                        "browser_fallback": page_from_browser_fallback,
                    },
                )
                _camoufox_debug(
                    "launch_privacy_verified",
                    **privacy_info,
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    state=await self._launch_debug_snapshot("privacy_verify", launch_started, launch_timeout_ms, ctx=ctx, page=page),
                )
                launch_phase = "page_ready"
                page_id = self._register_page(page, "default", True, "launch_fast_visible", "default")
                page = self.pages[page_id]
                _camoufox_debug(
                    "launch_page_registered",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    state=await self._launch_debug_snapshot("page_ready", launch_started, launch_timeout_ms, ctx=ctx, page=page, page_id=page_id),
                )
                page_bounds = await self._page_bounds_limited(page)
                elapsed_ms = int((time.perf_counter() - launch_started) * 1000)
                visible_window_probe = _visible_window_snapshot()
                diagnostics = {
                    "elapsed_ms": elapsed_ms,
                    "window": window_diag,
                    "page_bounds": page_bounds,
                    "viewport": page.viewport_size or {},
                    "contexts": len(self.contexts),
                    "pages": len(self.pages),
                    "active_page_id": self.active_page_id,
                    "profile": profile_info,
                    "ff_version": kwargs.get("ff_version"),
                    "ff_version_source": ff_version_source,
                    "privacy": privacy_info,
                    "context_source": context_source,
                    "fast_visible": True,
                    "browser_fallback": page_from_browser_fallback,
                    "viewport_policy": page_viewport_info,
                    "browser_ready_ms": browser_ready_ms,
                    "camoufox_launch_ms": camoufox_launch_ms,
                    "visible_window_probe": visible_window_probe,
                    "process_tree": _process_tree_snapshot(),
                    "subprocesses": _subprocess_diagnostics_snapshot(),
                    "launch": self._last_launch_summary,
                }
                _camoufox_debug(
                    "launch_ready",
                    elapsed_ms=elapsed_ms,
                    browser_open=True,
                    pages=len(self.pages),
                    contexts=len(self.contexts),
                    window=window_diag,
                    page_bounds=page_bounds,
                    profile=profile_info,
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    visible_window_probe=visible_window_probe,
                    launch_summary=self._last_launch_summary,
                    state=await self._launch_debug_snapshot("launch_ready", launch_started, launch_timeout_ms, ctx=ctx, page=page, page_id=page_id),
                )
                self._last_successful_launch_config = dict(cfg)
                return {
                    "status": "launched",
                    "headless": headless,
                    "os": os_type,
                    "locale": locale,
                    "session_id": self.session_id,
                    "active_page": self.active_page_id,
                    "active_page_id": self.active_page_id,
                    "page_count": len(self.pages),
                    "pages": await self.list_pages(),
                    "window_width": window_size[0],
                    "window_height": window_size[1],
                    "diagnostics": diagnostics,
                    "context_source": context_source,
                    "effective_ua_policy": privacy_info.get("effective_ua_policy"),
                    "browser_ready_ms": browser_ready_ms,
                    "camoufox_launch_ms": camoufox_launch_ms,
                }
            launch_phase = "import_async_api"
            import_started = time.perf_counter()
            _camoufox_debug("launch_import_async_begin")
            from camoufox.async_api import AsyncCamoufox
            _camoufox_debug("launch_import_async_ok", elapsed_ms=int((time.perf_counter() - import_started) * 1000))
            launch_phase = "construct_context"
            _camoufox_debug("launch_context_construct_begin", kwargs_keys=sorted(kwargs.keys()))
            self._cm = AsyncCamoufox(**kwargs)
            _camoufox_debug("launch_context_construct_ok", elapsed_ms=int((time.perf_counter() - launch_started) * 1000))
            launch_phase = "context_enter"
            self._set_launch_phase(launch_phase, launch_started, cfg.get("bridge_attempt_id"))
            _camoufox_debug(
                "launch_context_enter_begin",
                persistent=bool(kwargs.get("persistent_context")),
                timeout_ms=int(context_enter_timeout_s * 1000),
                global_timeout_ms=launch_timeout_ms,
                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                playwright_phase=launch_phase,
                executable=_path_info(executable_path),
                profile_dir=profile_dir or "",
                profile_locks=_profile_lock_snapshot(profile_dir),
                launch_policy=_launch_policy_info(from_options),
                env_presence=_env_presence(),
                process_tree=_process_tree_snapshot(),
                state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms),
            )
            try:
                context_enter_budget_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "context_enter", cap_s=context_enter_timeout_s)
                self.browser = await _await_no_cancel_wait(self._cm.__aenter__(), timeout=context_enter_budget_s)
                self._attach_browser_lifecycle_listeners()
            except asyncio.TimeoutError as exc:
                _camoufox_debug(
                    "launch_context_enter_timeout",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    timeout_ms=int(context_enter_timeout_s * 1000),
                    global_timeout_ms=launch_timeout_ms,
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    persistent=bool(kwargs.get("persistent_context")),
                    playwright_phase=launch_phase,
                    executable=_path_info(executable_path),
                    profile_dir=profile_dir or "",
                    profile_locks=_profile_lock_snapshot(profile_dir),
                    launch_policy=_launch_policy_info(from_options),
                    process_tree=_process_tree_snapshot(),
                    state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, exc=exc),
                )
                raise
            except Exception as enter_exc:
                _camoufox_debug(
                    "launch_context_enter_failed",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    timeout_ms=int(context_enter_timeout_s * 1000),
                    global_timeout_ms=launch_timeout_ms,
                    persistent=bool(kwargs.get("persistent_context")),
                    playwright_phase=launch_phase,
                    executable=_path_info(executable_path),
                    profile_dir=profile_dir or "",
                    profile_locks=_profile_lock_snapshot(profile_dir),
                    launch_policy=_launch_policy_info(from_options),
                    process_tree=_process_tree_snapshot(),
                    error_type=type(enter_exc).__name__,
                    error_len=len(str(enter_exc)),
                    error_summary=_safe_text(enter_exc),
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, exc=enter_exc),
                )
                raise
            _camoufox_debug(
                "launch_context_enter_ok",
                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                browser_type=type(self.browser).__name__,
                persistent=bool(kwargs.get("persistent_context")),
                playwright_phase=launch_phase,
                executable=_path_info(executable_path),
                profile_dir=profile_dir or "",
                profile_locks=_profile_lock_snapshot(profile_dir),
                launch_policy=_launch_policy_info(from_options),
                runtime=_browser_runtime_snapshot(self.browser),
                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms),
            )

            persistent_context = bool(kwargs.get("persistent_context"))
            _camoufox_debug(
                "launch_context_resolve_begin",
                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                browser_type=type(self.browser).__name__,
                persistent=persistent_context,
                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                state=await self._launch_debug_snapshot("context_resolve", launch_started, launch_timeout_ms),
            )
            if persistent_context:
                ctx = self.browser
                context_source = "persistent_context"
            else:
                launch_phase = "new_context"
                self._set_launch_phase(launch_phase, launch_started, cfg.get("bridge_attempt_id"))
                _camoufox_debug(
                    "launch_new_context_begin",
                    timeout_s=context_create_timeout_s,
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    ua_policy=context_plan.get("ua_policy"),
                    ua_override=bool(context_plan.get("user_agent")),
                    state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms),
                )
                try:
                    context_create_budget_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "new_context", cap_s=context_create_timeout_s)
                    ctx, context_source = await _create_private_context(self.browser, context_plan, context_create_budget_s, "launch_new_context", launch_started)
                except asyncio.TimeoutError as exc:
                    _camoufox_debug(
                        "launch_new_context_timeout",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        timeout_s=context_create_timeout_s,
                        remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                        state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, exc=exc),
                    )
                    raise
                except Exception as exc:
                    _camoufox_debug(
                        "launch_new_context_failed",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        timeout_s=context_create_timeout_s,
                        error_type=type(exc).__name__,
                        error_kind=_launch_error_kind(exc, self._browser_connected()),
                        error_summary=_safe_text(exc),
                        remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                        state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, exc=exc),
                    )
                    raise
                _camoufox_debug(
                    "launch_new_context_ok",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    source=context_source,
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx),
                )
            self.contexts["default"] = ctx
            self._register_context("default", ctx)
            _camoufox_debug(
                "launch_context_created",
                session_id=self.session_id,
                context_id="default",
                ctx_pages=_context_page_count(ctx),
                browser_context_count=_browser_context_count(self.browser),
                registered_contexts=len(self.contexts),
                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                context_source=context_source,
            )
            self._context_plan = dict(context_plan)
            _camoufox_debug(
                "launch_context_selected",
                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                contexts=len(self.contexts),
                source=context_source,
                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                state=await self._launch_debug_snapshot("context_selected", launch_started, launch_timeout_ms, ctx=ctx),
            )

            override_script = _privacy_override_script(context_plan)
            privacy_init_script_installed = False
            if override_script:
                launch_phase = "privacy_init_script"
                privacy_script_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "privacy_init_script", cap_s=5.0)
                await _await_no_cancel_wait(ctx.add_init_script(override_script), timeout=privacy_script_timeout_s)
                privacy_init_script_installed = True
                _camoufox_debug("launch_privacy_init_script_ok", elapsed_ms=int((time.perf_counter() - launch_started) * 1000))

            if os_type != host_os:
                launch_phase = "font_fallback_script"
                from .utils.js_helpers import get_font_fallback_script
                _camoufox_debug("launch_font_fallback_begin")
                font_script_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "font_fallback_script", cap_s=5.0)
                await _await_no_cancel_wait(ctx.add_init_script(get_font_fallback_script()), timeout=font_script_timeout_s)
                _camoufox_debug("launch_font_fallback_ok", elapsed_ms=int((time.perf_counter() - launch_started) * 1000))

            launch_phase = "persistent_scripts"
            _camoufox_debug("launch_persistent_scripts_begin", count=len(self._persistent_scripts))
            for script_info in self._persistent_scripts:
                persistent_script_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "persistent_scripts", cap_s=5.0)
                await _await_no_cancel_wait(ctx.add_init_script(script=script_info["content"]), timeout=persistent_script_timeout_s)
            _camoufox_debug("launch_persistent_scripts_ok", elapsed_ms=int((time.perf_counter() - launch_started) * 1000))

            launch_phase = "page_select"
            self._set_launch_phase(launch_phase, launch_started, cfg.get("bridge_attempt_id"))
            existing_pages: list[Page] = []
            try:
                existing_pages = list(ctx.pages)
            except Exception as exc:
                _camoufox_debug(
                    "launch_existing_pages_snapshot_failed",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    error_type=type(exc).__name__,
                    error_len=len(str(exc)),
                )
            page = None
            page_reused = False
            page_fresh = False
            page_from_browser_fallback = False
            page_viewport_info: dict[str, Any] = {}
            if persistent_context:
                page = self._first_live_page(existing_pages)
                if page is not None:
                    page_reused = True
                    _camoufox_debug(
                        "launch_persistent_existing_page_reuse",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        pages_seen=len(existing_pages),
                        url=_safe_page_url(page, 160),
                        init_script_installed=privacy_init_script_installed,
                        remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                        state=await self._launch_debug_snapshot("persistent_existing_page", launch_started, launch_timeout_ms, ctx=ctx, page=page),
                    )
                else:
                    launch_phase = "persistent_page_wait"
                    page, existing_pages = await self._wait_for_existing_context_page(ctx, persistent_page_wait_s, "launch_persistent_existing_page_wait", launch_started)
                    if page is not None:
                        page_reused = True
                        _camoufox_debug(
                            "launch_persistent_existing_page_reuse",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            pages_seen=len(existing_pages),
                            url=_safe_page_url(page, 160),
                            init_script_installed=privacy_init_script_installed,
                            waited=True,
                            remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                            state=await self._launch_debug_snapshot("persistent_existing_page_wait", launch_started, launch_timeout_ms, ctx=ctx, page=page),
                        )
                if page is None:
                    launch_phase = "persistent_new_page"
                    _camoufox_debug(
                        "launch_persistent_new_page_begin",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        existing_pages=len(existing_pages),
                        timeout_s=persistent_page_create_timeout_s,
                        init_script_installed=privacy_init_script_installed,
                        persistent=True,
                        remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                        state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx),
                    )
                    self._queue_pending_page_id("default", "default")
                    pending_page_task: asyncio.Task | None = None
                    try:
                        persistent_create_budget_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "persistent_new_page", cap_s=persistent_page_create_timeout_s, reserve_ms=500)
                        page, pending_page_task = await _await_late_result(ctx.new_page(), timeout=persistent_create_budget_s)
                        if pending_page_task is not None:
                            raise asyncio.TimeoutError
                        page_fresh = True
                    except asyncio.TimeoutError as exc:
                        _camoufox_debug(
                            "launch_persistent_new_page_timeout",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            timeout_s=persistent_page_create_timeout_s,
                            init_script_installed=privacy_init_script_installed,
                            pending_page_task=_pending_page_task_state(pending_page_task),
                            process_tree=_process_tree_snapshot(),
                            remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                            state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx, exc=exc),
                        )
                        persistent_late_wait_s = min(2.0, persistent_page_wait_s, self._launch_remaining_timeout_s(launch_started, launch_timeout_ms, reserve_ms=500))
                        page = await self._wait_for_late_page(ctx, existing_pages, persistent_late_wait_s, "launch_persistent_new_page", launch_started, pending_page_task)
                        if page is None:
                            _abandon_late_task(pending_page_task)
                            raise
                        page_fresh = True
                    except Exception as exc:
                        _camoufox_debug(
                            "launch_persistent_new_page_failed",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            error_type=type(exc).__name__,
                            error_len=len(str(exc)),
                            error_summary=_safe_text(exc),
                            pending_page_task=_pending_page_task_state(pending_page_task),
                            process_tree=_process_tree_snapshot(),
                            remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                            state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx, exc=exc),
                        )
                        persistent_failed_late_wait_s = min(1.0, persistent_page_wait_s, self._launch_remaining_timeout_s(launch_started, launch_timeout_ms, reserve_ms=500))
                        page = await self._wait_for_late_page(ctx, existing_pages, persistent_failed_late_wait_s, "launch_persistent_new_page_failed", launch_started, pending_page_task)
                        if page is None:
                            _abandon_late_task(pending_page_task)
                            raise
                        page_fresh = True
                    finally:
                        self._discard_pending_page_id("default", "default")
                _camoufox_debug(
                    "launch_privacy_page_selected",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    pages_seen=len(existing_pages),
                    url=_safe_page_url(page, 160),
                    init_script_installed=privacy_init_script_installed,
                    page_reused=page_reused,
                    page_fresh=page_fresh,
                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                    state=await self._launch_debug_snapshot("privacy_page_selected", launch_started, launch_timeout_ms, ctx=ctx, page=page),
                )
                page_viewport_info = await _apply_plan_page_viewport_size(
                    page,
                    context_plan,
                    "launch_persistent_page_selected",
                    5.0,
                    window_size,
                    launch_started,
                )
                launch_phase = "privacy_page_navigate"
                try:
                    persistent_nav_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "privacy_page_navigate", cap_s=persistent_page_nav_timeout_s)
                    await _await_no_cancel_wait(page.goto(PRIVACY_VERIFY_URL, wait_until="domcontentloaded", timeout=max(1, int(persistent_nav_timeout_s * 1000))), timeout=persistent_nav_timeout_s)
                    _camoufox_debug(
                        "launch_privacy_page_navigate_ok",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        url=_safe_page_url(page, 160),
                        init_script_installed=privacy_init_script_installed,
                        remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                        state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx, page=page),
                    )
                except asyncio.TimeoutError as exc:
                    _camoufox_debug(
                        "launch_privacy_page_navigate_timeout",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        url=_safe_page_url(page, 160),
                        init_script_installed=privacy_init_script_installed,
                        remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                        state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx, page=page, exc=exc),
                    )
                    raise
                except Exception as exc:
                    _camoufox_debug(
                        "launch_privacy_page_navigate_failed",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        url=_safe_page_url(page, 160),
                        init_script_installed=privacy_init_script_installed,
                        error_type=type(exc).__name__,
                        error_len=len(str(exc)),
                        error_summary=_safe_text(exc),
                        remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                        state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx, page=page, exc=exc),
                    )
                    raise
            else:
                for candidate in existing_pages:
                    try:
                        if not candidate.is_closed():
                            page = candidate
                            page_reused = True
                            break
                    except Exception as exc:
                        _camoufox_debug(
                            "launch_existing_page_state_failed",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            error_type=type(exc).__name__,
                            error_len=len(str(exc)),
                        )
                if page is not None:
                    page_url = ""
                    try:
                        page_url = str(page.url or "")
                    except Exception:
                        pass
                    _camoufox_debug(
                        "launch_existing_page_reuse",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        pages_seen=len(existing_pages),
                        url=_safe_text(page_url, 160),
                        init_script_installed=privacy_init_script_installed,
                    )
                else:
                    launch_phase = "new_page"
                    self._set_launch_phase(launch_phase, launch_started, cfg.get("bridge_attempt_id"))
                    _camoufox_debug(
                        "launch_new_page_begin",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        timeout_s=page_create_timeout_s,
                        init_script_installed=privacy_init_script_installed,
                        existing_pages=len(existing_pages),
                        context_page_count=_context_page_count(ctx),
                        registered_pages=len(self.pages),
                        active_page_id=self.active_page_id or "",
                        remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                        state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx),
                    )
                    self._queue_pending_page_id("default", "default")
                    pending_page_task: asyncio.Task | None = None
                    try:
                        page_task_started = time.perf_counter()
                        page_create_budget_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "new_page", cap_s=page_create_timeout_s, reserve_ms=500)
                        page, pending_page_task, page_task_timed_out = await self._await_page_result_for_launch(ctx.new_page(), page_create_budget_s, "launch_new_page", launch_started)
                        _camoufox_debug(
                            "launch_new_page_task_result",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            phase_elapsed_ms=int((time.perf_counter() - page_task_started) * 1000),
                            task_id=id(pending_page_task),
                            task_timed_out=page_task_timed_out,
                            pending_page_task=_pending_page_task_state(pending_page_task),
                            page_present=page is not None,
                            page_closed=self._page_closed(page) if page is not None else True,
                            page_url=_safe_page_url(page, 160) if page is not None else "",
                            context_page_count=_context_page_count(ctx),
                            registered_pages=len(self.pages),
                            remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                            state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx, page=page),
                        )
                        self._raise_if_launch_terminal("launch_new_page")
                        if page_task_timed_out:
                            raise asyncio.TimeoutError
                        if page is None or self._page_closed(page):
                            raise RuntimeError("Camoufox page creation returned a closed page during launch")
                        page_fresh = True
                    except asyncio.TimeoutError as exc:
                        _camoufox_debug(
                            "launch_new_page_timeout",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            timeout_s=page_create_timeout_s,
                            task_id=id(pending_page_task) if pending_page_task is not None else 0,
                            pending_page_task=_pending_page_task_state(pending_page_task),
                            context_page_count=_context_page_count(ctx),
                            registered_pages=len(self.pages),
                            active_page_id=self.active_page_id or "",
                            process_tree=_process_tree_snapshot(),
                            remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                            state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx, exc=exc),
                        )
                        page_late_wait_s = min(late_page_wait_s, self._launch_remaining_timeout_s(launch_started, launch_timeout_ms, reserve_ms=500))
                        page = await self._wait_for_late_page(ctx, existing_pages, page_late_wait_s, "launch_new_page", launch_started, pending_page_task)
                        if page is None:
                            _abandon_late_task(pending_page_task)
                            if not hasattr(self.browser, "new_page"):
                                raise
                            self._discard_pending_page_id("default", "default")
                            _camoufox_debug(
                                "launch_browser_page_fallback_begin",
                                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                                old_context_pages=len(existing_pages),
                                timeout_s=page_create_timeout_s,
                                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                                state=await self._launch_debug_snapshot("browser_page_fallback", launch_started, launch_timeout_ms, ctx=ctx, exc=exc),
                            )
                            with contextlib.suppress(Exception):
                                close_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "browser_page_fallback_close", cap_s=8.0)
                                await _await_no_cancel_wait(ctx.close(), timeout=close_timeout_s)
                            self.context_ids.pop(id(ctx), None)
                            try:
                                fallback_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "browser_page_fallback", cap_s=page_create_timeout_s)
                                ctx, page, context_source = await _create_private_browser_page_context(self.browser, context_plan, fallback_timeout_s, "launch_browser_page_fallback", launch_started)
                                if page is None or self._page_closed(page):
                                    raise RuntimeError("Camoufox browser page fallback returned a closed page during launch")
                            except Exception as fallback_exc:
                                _camoufox_debug(
                                    "launch_browser_page_fallback_failed",
                                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                                    error_type=type(fallback_exc).__name__,
                                    error_len=len(str(fallback_exc)),
                                    error_summary=_safe_text(fallback_exc),
                                    process_tree=_process_tree_snapshot(),
                                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                                    state=await self._launch_debug_snapshot("browser_page_fallback", launch_started, launch_timeout_ms, ctx=ctx, exc=fallback_exc),
                                )
                                raise
                            self.contexts["default"] = ctx
                            self._register_context("default", ctx)
                            _camoufox_debug(
                                "launch_browser_page_fallback_context_created",
                                session_id=self.session_id,
                                context_id="default",
                                ctx_pages=_context_page_count(ctx),
                                browser_context_count=_browser_context_count(self.browser),
                                registered_contexts=len(self.contexts),
                                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                                context_source=context_source,
                            )
                            if os_type != host_os:
                                from .utils.js_helpers import get_font_fallback_script
                                font_script_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "browser_page_fallback_font_script", cap_s=5.0)
                                await _await_no_cancel_wait(ctx.add_init_script(get_font_fallback_script()), timeout=font_script_timeout_s)
                            for script_info in self._persistent_scripts:
                                persistent_script_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "browser_page_fallback_persistent_scripts", cap_s=5.0)
                                await _await_no_cancel_wait(ctx.add_init_script(script=script_info["content"]), timeout=persistent_script_timeout_s)
                            page_from_browser_fallback = True
                            _camoufox_debug(
                                "launch_browser_page_fallback_ok",
                                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                                page_closed=self._page_closed(page),
                                page_url=_safe_page_url(page, 160),
                                context_page_count=_context_page_count(ctx),
                                registered_pages=len(self.pages),
                                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                                state=await self._launch_debug_snapshot("browser_page_fallback", launch_started, launch_timeout_ms, ctx=ctx, page=page),
                            )
                        page_fresh = True
                    except Exception as exc:
                        _camoufox_debug(
                            "launch_new_page_failed",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            error_type=type(exc).__name__,
                            error_len=len(str(exc)),
                            error_summary=_safe_text(exc),
                            error_kind=_launch_error_kind(exc, self._browser_connected()),
                            task_id=id(pending_page_task) if pending_page_task is not None else 0,
                            pending_page_task=_pending_page_task_state(pending_page_task),
                            context_page_count=_context_page_count(ctx),
                            registered_pages=len(self.pages),
                            process_tree=_process_tree_snapshot(),
                            remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                            state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx, exc=exc),
                        )
                        failed_late_wait_s = min(3.0, late_page_wait_s, self._launch_remaining_timeout_s(launch_started, launch_timeout_ms, reserve_ms=500))
                        page = await self._wait_for_late_page(ctx, existing_pages, failed_late_wait_s, "launch_new_page_failed", launch_started, pending_page_task)
                        if page is None:
                            _abandon_late_task(pending_page_task)
                            if not hasattr(self.browser, "new_page"):
                                raise
                            self._discard_pending_page_id("default", "default")
                            _camoufox_debug(
                                "launch_browser_page_fallback_begin",
                                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                                old_context_pages=len(existing_pages),
                                timeout_s=page_create_timeout_s,
                                source_error=type(exc).__name__,
                                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                                state=await self._launch_debug_snapshot("browser_page_fallback", launch_started, launch_timeout_ms, ctx=ctx, exc=exc),
                            )
                            with contextlib.suppress(Exception):
                                close_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "browser_page_fallback_close", cap_s=8.0)
                                await _await_no_cancel_wait(ctx.close(), timeout=close_timeout_s)
                            self.context_ids.pop(id(ctx), None)
                            try:
                                fallback_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "browser_page_fallback", cap_s=page_create_timeout_s)
                                ctx, page, context_source = await _create_private_browser_page_context(self.browser, context_plan, fallback_timeout_s, "launch_browser_page_fallback", launch_started)
                                if page is None or self._page_closed(page):
                                    raise RuntimeError("Camoufox browser page fallback returned a closed page during launch")
                            except Exception as fallback_exc:
                                _camoufox_debug(
                                    "launch_browser_page_fallback_failed",
                                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                                    error_type=type(fallback_exc).__name__,
                                    error_len=len(str(fallback_exc)),
                                    error_summary=_safe_text(fallback_exc),
                                    process_tree=_process_tree_snapshot(),
                                    remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                                    state=await self._launch_debug_snapshot("browser_page_fallback", launch_started, launch_timeout_ms, ctx=ctx, exc=fallback_exc),
                                )
                                raise
                            self.contexts["default"] = ctx
                            self._register_context("default", ctx)
                            _camoufox_debug(
                                "launch_browser_page_fallback_context_created",
                                session_id=self.session_id,
                                context_id="default",
                                ctx_pages=_context_page_count(ctx),
                                browser_context_count=_browser_context_count(self.browser),
                                registered_contexts=len(self.contexts),
                                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                                context_source=context_source,
                            )
                            if os_type != host_os:
                                from .utils.js_helpers import get_font_fallback_script
                                font_script_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "browser_page_fallback_font_script", cap_s=5.0)
                                await _await_no_cancel_wait(ctx.add_init_script(get_font_fallback_script()), timeout=font_script_timeout_s)
                            for script_info in self._persistent_scripts:
                                persistent_script_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "browser_page_fallback_persistent_scripts", cap_s=5.0)
                                await _await_no_cancel_wait(ctx.add_init_script(script=script_info["content"]), timeout=persistent_script_timeout_s)
                            page_from_browser_fallback = True
                            _camoufox_debug(
                                "launch_browser_page_fallback_ok",
                                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                                page_closed=self._page_closed(page),
                                page_url=_safe_page_url(page, 160),
                                context_page_count=_context_page_count(ctx),
                                registered_pages=len(self.pages),
                                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                                state=await self._launch_debug_snapshot("browser_page_fallback", launch_started, launch_timeout_ms, ctx=ctx, page=page),
                            )
                        page_fresh = True
                    finally:
                        self._discard_pending_page_id("default", "default")
                    if page_from_browser_fallback:
                        launch_phase = "browser_page_fallback_navigate"
                        fallback_nav_timeout_s = self._launch_timeout_s(launch_started, launch_timeout_ms, "browser_page_fallback_navigate", cap_s=min(15.0, page_create_timeout_s))
                        await _await_no_cancel_wait(page.goto(PRIVACY_VERIFY_URL, wait_until="load", timeout=max(1, int(fallback_nav_timeout_s * 1000))), timeout=fallback_nav_timeout_s)
                    _camoufox_debug(
                        "launch_new_page_ok",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        init_script_installed=privacy_init_script_installed,
                        browser_fallback=page_from_browser_fallback,
                        remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                        state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx, page=page),
                    )
                self._raise_if_launch_terminal("launch_new_page_ok")
                page_viewport_info = await _apply_plan_page_viewport_size(
                    page,
                    context_plan,
                    "launch_new_page",
                    5.0,
                    window_size,
                    launch_started,
                )
            try:
                current_pages = list(ctx.pages)
                extras_seen = 0
                extras_closed = 0
                extras_kept = 0
                for extra in current_pages:
                    if extra is page:
                        continue
                    extras_seen += 1
                    extra_url = ""
                    try:
                        extra_url = str(extra.url or "")
                    except Exception:
                        pass
                    if extra_url in {"", "about:blank", "about:newtab"}:
                        await _await_no_cancel_wait(extra.close(), timeout=5.0)
                        extras_closed += 1
                    else:
                        extras_kept += 1
                if extras_seen:
                    _camoufox_debug(
                        "launch_duplicate_pages_cleanup",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        seen=extras_seen,
                        closed=extras_closed,
                        kept=extras_kept,
                        remaining=len(ctx.pages),
                    )
            except Exception as exc:
                _camoufox_debug(
                    "launch_duplicate_pages_cleanup_failed",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    error_type=type(exc).__name__,
                    error_len=len(str(exc)),
                    error_summary=_safe_text(exc),
                )
            if page is None or self._page_closed(page):
                _camoufox_debug(
                    "page_closed_during_launch",
                    phase="before_privacy_verify",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    page_present=page is not None,
                    page_closed=self._page_closed(page) if page is not None else True,
                    context_page_count=_context_page_count(ctx),
                    registered_pages=len(self.pages),
                    active_page_id=self.active_page_id or "",
                    recent_page_events=self._page_event_tail(None, 24),
                    state=await self._launch_debug_snapshot("before_privacy_verify", launch_started, launch_timeout_ms, ctx=ctx, page=page),
                )
                raise RuntimeError("Camoufox page closed during launch before privacy verification")
            self._raise_if_launch_terminal("before_privacy_verify")
            launch_phase = "privacy_verify"
            self._set_launch_phase(launch_phase, launch_started, cfg.get("bridge_attempt_id"))
            privacy_info = await _verify_page_privacy(
                page,
                context_plan,
                {
                    "page_reused": page_reused,
                    "page_fresh": page_fresh,
                    "init_script_installed": privacy_init_script_installed,
                    "persistent_context": persistent_context,
                    "context_source": context_source,
                    "page_id": self.page_id_for(page) or "",
                    "active_page_id": self.active_page_id or "",
                    "registered_pages": len(self.pages),
                    "context_page_count": _context_page_count(ctx),
                    "browser_open": self.browser is not None,
                    "browser_connected": self._browser_connected(),
                    "recent_page_events": self._page_event_tail(None, 24),
                    "launch_attempt_id": cfg.get("bridge_attempt_id") or "",
                },
            )
            self._active_launch_privacy_verified = True
            _camoufox_debug(
                "launch_privacy_verified",
                **privacy_info,
                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                state=await self._launch_debug_snapshot("privacy_verify", launch_started, launch_timeout_ms, ctx=ctx, page=page),
            )
            launch_phase = "page_ready"
            for existing_page in list(ctx.pages):
                self._register_page(existing_page, "default" if existing_page is page else None, existing_page is page, "launch_existing")
            page_id = self._register_page(page, "default", True, "launch")
            page = self.pages[page_id]
            _camoufox_debug(
                "launch_page_registered",
                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                state=await self._launch_debug_snapshot("page_ready", launch_started, launch_timeout_ms, ctx=ctx, page=page, page_id=page_id),
            )
            page_bounds = await self._page_bounds_limited(page)
            elapsed_ms = int((time.perf_counter() - launch_started) * 1000)
            visible_window_probe = _visible_window_snapshot()
            diagnostics = {
                "elapsed_ms": elapsed_ms,
                "window": window_diag,
                "page_bounds": page_bounds,
                "viewport": page.viewport_size or {},
                "contexts": len(self.contexts),
                "pages": len(self.pages),
                "active_page_id": self.active_page_id,
                "profile": profile_info,
                "ff_version": kwargs.get("ff_version"),
                "ff_version_source": ff_version_source,
                "privacy": privacy_info,
                "context_source": context_source,
                "viewport_policy": page_viewport_info,
                "browser_ready_ms": elapsed_ms,
                "camoufox_launch_ms": elapsed_ms,
                "visible_window_probe": visible_window_probe,
                "process_tree": _process_tree_snapshot(),
                "subprocesses": _subprocess_diagnostics_snapshot(),
                "launch": self._last_launch_summary,
            }
            _camoufox_debug(
                "launch_ready",
                elapsed_ms=elapsed_ms,
                browser_open=True,
                pages=len(self.pages),
                contexts=len(self.contexts),
                window=window_diag,
                page_bounds=page_bounds,
                profile=profile_info,
                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                visible_window_probe=visible_window_probe,
                launch_summary=self._last_launch_summary,
                state=await self._launch_debug_snapshot("launch_ready", launch_started, launch_timeout_ms, ctx=ctx, page=page, page_id=page_id),
            )

            self._last_successful_launch_config = dict(cfg)
            self._active_launch_phase = ""
            self._active_launch_started = 0.0
            self._active_launch_attempt_id = ""
            self._active_launch_privacy_verified = False
            self._active_launch_terminal_reason = ""
            self._active_launch_terminal_payload = {}
            self._active_launch_terminal_event = None
            if _ACTIVE_LAUNCH_MANAGER is self:
                _ACTIVE_LAUNCH_MANAGER = None
            return {
                "status": "launched",
                "headless": headless,
                "os": os_type,
                "locale": locale,
                "session_id": self.session_id,
                "active_page": self.active_page_id,
                "active_page_id": self.active_page_id,
                "page_count": len(self.pages),
                "pages": await self.list_pages(),
                "window_width": window_size[0],
                "window_height": window_size[1],
                "diagnostics": diagnostics,
                "browser_ready_ms": elapsed_ms,
                "camoufox_launch_ms": elapsed_ms,
            }
        except Exception as exc:
            elapsed_ms = int((time.perf_counter() - launch_started) * 1000)
            stderr_tail = _debug_log_tail(4000)
            failure_payload = await self._build_launch_failure_payload(
                exc,
                launch_phase,
                launch_started,
                launch_timeout_ms,
                ctx=ctx,
                page=page,
                stderr_tail=stderr_tail,
            )
            self._last_error = str(failure_payload.get("error") or _launch_error_summary(exc, launch_phase))
            _camoufox_debug(
                "launch_error",
                elapsed_ms=elapsed_ms,
                session_id=self.session_id,
                error_type=type(exc).__name__,
                error_len=len(str(exc)),
                error_kind=failure_payload.get("error_kind", ""),
                error_summary=failure_payload.get("error_summary", ""),
                phase=launch_phase,
                window=window_diag,
                persistent=bool(kwargs.get("persistent_context")),
                profile=profile_info,
                profile_locks=_profile_lock_snapshot(profile_dir),
                launch_policy=_launch_policy_info(from_options),
                process_tree=_process_tree_detailed_snapshot(),
                subprocesses=_subprocess_diagnostics_snapshot(),
                launch_summary=self._last_launch_summary,
                crash_reports=_crash_report_snapshot(self._profile_dir or self._last_launch_summary.get("profile_dir", "")),
                stderr_tail=stderr_tail,
                stderr_tail_len=len(stderr_tail),
                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                failure_payload=failure_payload,
                state=await self._launch_debug_snapshot(launch_phase, launch_started, launch_timeout_ms, ctx=ctx, page=page, exc=exc),
            )
            cleanup_tree_before = _process_tree_detailed_snapshot()
            if self._cm is not None and not (launch_phase == "context_enter" and isinstance(exc, asyncio.TimeoutError)):
                try:
                    cleanup_started = time.perf_counter()
                    _camoufox_debug("launch_error_context_exit_begin")
                    await _await_no_cancel_wait(self._cm.__aexit__(None, None, None), timeout=10)
                    _camoufox_debug("launch_error_context_exit_ok", elapsed_ms=int((time.perf_counter() - cleanup_started) * 1000))
                except Exception as cleanup_exc:
                    _camoufox_debug(
                        "launch_error_context_exit_failed",
                        error_type=type(cleanup_exc).__name__,
                        error_len=len(str(cleanup_exc)),
                        error_summary=_safe_text(cleanup_exc),
                    )
            elif self.browser is not None:
                try:
                    cleanup_started = time.perf_counter()
                    _camoufox_debug("launch_error_browser_close_begin")
                    await _await_no_cancel_wait(self.browser.close(), timeout=10)
                    _camoufox_debug("launch_error_browser_close_ok", elapsed_ms=int((time.perf_counter() - cleanup_started) * 1000))
                except Exception as cleanup_exc:
                    _camoufox_debug(
                        "launch_error_browser_close_failed",
                        error_type=type(cleanup_exc).__name__,
                        error_len=len(str(cleanup_exc)),
                        error_summary=_safe_text(cleanup_exc),
                    )
            if self._playwright is not None:
                try:
                    stop_started = time.perf_counter()
                    _camoufox_debug("launch_error_playwright_stop_begin")
                    await _await_no_cancel_wait(self._playwright.stop(), timeout=5)
                    _camoufox_debug("launch_error_playwright_stop_ok", elapsed_ms=int((time.perf_counter() - stop_started) * 1000))
                except Exception as stop_exc:
                    _camoufox_debug(
                        "launch_error_playwright_stop_failed",
                        error_type=type(stop_exc).__name__,
                        error_len=len(str(stop_exc)),
                        error_summary=_safe_text(stop_exc),
                    )
            cleanup_summary = _terminate_descendant_processes("launch_error")
            cleanup_tail = _debug_log_tail(4000)
            _camoufox_debug(
                "launch_error_cleanup_counts",
                session_id=self.session_id,
                cleanup_tree_before=cleanup_tree_before,
                cleanup_summary=cleanup_summary,
                contexts_before_clear=len(self.contexts),
                pages_before_clear=len(self.pages),
                pending_page_id_count=sum(len(v) for v in self._pending_page_ids_by_context.values()),
                stderr_tail=cleanup_tail,
                stderr_tail_len=len(cleanup_tail),
            )
            failure_payload["cleanup_summary"] = cleanup_summary
            failure_payload["cleanup_stderr_tail"] = cleanup_tail
            failure_payload["cleanup_stderr_tail_len"] = len(cleanup_tail)
            self._last_launch_failure = failure_payload
            self._browser_generation += 1
            self.browser = None
            self.contexts.clear()
            self.pages.clear()
            self.page_meta.clear()
            self.context_ids.clear()
            self._page_guid_to_id.clear()
            self._listener_page_ids.clear()
            self._browser_lifecycle_listener_ids.clear()
            self._pending_page_ids_by_context.clear()
            self._active_page_operations.clear()
            self.active_page_name = None
            self.active_page_id = None
            self._cm = None
            self._playwright = None
            self._profile_dir = None
            self._profile_generated = False
            if profile_dir and bool(profile_info.get("generated")):
                try:
                    _shutil.rmtree(profile_dir, ignore_errors=True)
                    _camoufox_debug("launch_error_profile_removed", profile_dir=profile_dir)
                except Exception:
                    _camoufox_debug("launch_error_profile_remove_failed", profile_dir=profile_dir)
            elif profile_dir:
                _camoufox_debug("launch_error_profile_preserved", profile_dir=profile_dir)
            if bundled_visible_launch and not bool(cfg.get("_aida_launch_retry")) and _launch_error_retryable(exc, launch_phase):
                remaining_global_ms = self._budget_remaining_ms(launch_started, launch_timeout_ms)
                retry_error_kind = str(failure_payload.get("error_kind") or _launch_error_kind(exc, self._browser_connected()))
                retry_error_summary = str(failure_payload.get("error_summary") or _launch_error_summary(exc, launch_phase))
                retry_launch_terminal_reason = self._active_launch_terminal_reason
                retry_terminal_failure = retry_error_kind in {"target_closed", "browser_disconnected", "browser_closed", "context_closed", "page_crash"}
                retry_terminal_failure = retry_terminal_failure or "page closed during launch" in retry_error_summary.lower()
                retry_terminal_failure = retry_terminal_failure or bool(retry_launch_terminal_reason)
                retry_budget_ms = aida_retry_launch_timeout_ms(launch_timeout_ms, remaining_global_ms)
                if launch_phase == "context_enter" and isinstance(exc, asyncio.TimeoutError):
                    _camoufox_debug(
                        "launch_retry_skipped",
                        elapsed_ms=elapsed_ms,
                        phase=launch_phase,
                        remaining_budget_ms=remaining_global_ms,
                        error_type=type(exc).__name__,
                        error_kind=retry_error_kind,
                        error_summary=retry_error_summary,
                        reason="cold_start_context_enter_timeout_retry_disabled",
                    )
                elif retry_terminal_failure:
                    _camoufox_debug(
                        "launch_retry_skipped",
                        elapsed_ms=elapsed_ms,
                        phase=launch_phase,
                        remaining_budget_ms=remaining_global_ms,
                        error_type=type(exc).__name__,
                        error_kind=retry_error_kind,
                        error_summary=retry_error_summary,
                        reason="terminal_launch_failure",
                        launch_terminal_reason=retry_launch_terminal_reason,
                        launch_terminal_payload=self._active_launch_terminal_payload,
                    )
                elif retry_budget_ms <= 0:
                    _camoufox_debug(
                        "launch_retry_skipped",
                        elapsed_ms=elapsed_ms,
                        phase=launch_phase,
                        remaining_budget_ms=remaining_global_ms,
                        error_type=type(exc).__name__,
                        error_kind=retry_error_kind,
                        error_summary=retry_error_summary,
                        reason="remaining_budget_below_floor",
                        floor_ms=AIDA_LAUNCH_FLOOR_MS,
                    )
                else:
                    retry_config = dict(cfg)
                    retry_config["_aida_launch_retry"] = True
                    retry_config.pop("profile_dir", None)
                    retry_config["launch_timeout_ms"] = retry_budget_ms
                    _camoufox_debug(
                        "launch_retry_begin",
                        elapsed_ms=elapsed_ms,
                        phase=launch_phase,
                        error_type=type(exc).__name__,
                        error_kind=retry_error_kind,
                        error_summary=retry_error_summary,
                        policy_marker=AIDA_LAUNCH_BUDGET_POLICY_MARKER,
                        next_timeout_ms=retry_config["launch_timeout_ms"],
                        remaining_budget_ms=remaining_global_ms,
                    )
                    return await self.launch(retry_config)
            self._active_launch_phase = ""
            self._active_launch_started = 0.0
            self._active_launch_attempt_id = ""
            self._active_launch_privacy_verified = False
            self._active_launch_terminal_reason = ""
            self._active_launch_terminal_payload = {}
            self._active_launch_terminal_event = None
            if _ACTIVE_LAUNCH_MANAGER is self:
                _ACTIVE_LAUNCH_MANAGER = None
            raise

    async def _ensure_browser(self) -> None:
        """Lazy-launch the browser if not already running."""
        if self.browser is None:
            await self.launch()

    async def add_persistent_script(self, name: str, content: str) -> None:
        """Register a script that persists across all navigations via context-level injection."""
        for s in self._persistent_scripts:
            if s["name"] == name:
                s["content"] = content
                break
        else:
            self._persistent_scripts.append({"name": name, "content": content})
        for ctx in self.contexts.values():
            await ctx.add_init_script(script=content)

    def remove_persistent_script(self, name: str) -> bool:
        """Remove a persistent script by name. Returns True if found."""
        before = len(self._persistent_scripts)
        self._persistent_scripts = [s for s in self._persistent_scripts if s["name"] != name]
        return len(self._persistent_scripts) < before

    def _page_guid(self, page: Page) -> str:
        impl = getattr(page, "_impl_obj", None)
        return str(getattr(page, "_guid", "") or getattr(impl, "_guid", "") or "")

    def _page_closed(self, page: Page) -> bool:
        try:
            return bool(page.is_closed())
        except Exception:
            return True

    def _budget_remaining_ms(self, started: float, budget_ms: int | float | None) -> int:
        if not budget_ms or budget_ms <= 0:
            return -1
        return max(0, int(budget_ms) - int((time.perf_counter() - started) * 1000))

    def _launch_remaining_timeout_s(self, started: float, budget_ms: int | float | None, cap_s: float | None = None, reserve_ms: int = 0) -> float:
        remaining_ms = self._budget_remaining_ms(started, budget_ms)
        if remaining_ms < 0:
            timeout_s = cap_s if cap_s is not None else 0.0
        else:
            remaining_ms = max(0, remaining_ms - max(0, int(reserve_ms)))
            timeout_s = remaining_ms / 1000.0
            if cap_s is not None:
                timeout_s = min(timeout_s, cap_s)
        return max(0.0, timeout_s)

    def _launch_timeout_s(self, started: float, budget_ms: int | float | None, phase: str, cap_s: float | None = None, reserve_ms: int = 0) -> float:
        timeout_s = self._launch_remaining_timeout_s(started, budget_ms, cap_s, reserve_ms)
        if timeout_s <= 0.0:
            _camoufox_debug(
                f"{phase}_launch_budget_exhausted",
                elapsed_ms=int((time.perf_counter() - started) * 1000),
                remaining_budget_ms=self._budget_remaining_ms(started, budget_ms),
                reserved_ms=max(0, int(reserve_ms)),
            )
            raise asyncio.TimeoutError
        return timeout_s

    def _browser_connected(self) -> bool:
        if self.browser is None:
            return False
        candidates: list[Any] = [self.browser]
        for attr in ("browser", "_browser"):
            try:
                candidate = getattr(self.browser, attr, None)
                if candidate is not None:
                    candidates.append(candidate)
            except Exception:
                pass
        try:
            candidate = getattr(getattr(self.browser, "_impl_obj", None), "_browser", None)
            if candidate is not None:
                candidates.append(candidate)
        except Exception:
            pass
        for candidate in candidates:
            try:
                connected = getattr(candidate, "is_connected", None)
                if callable(connected):
                    return bool(connected())
            except Exception:
                pass
        return self.browser is not None

    def _attach_browser_lifecycle_listeners(self) -> None:
        candidates: list[Any] = []
        if self.browser is not None:
            candidates.append(self.browser)
        for attr in ("browser", "_browser"):
            try:
                candidate = getattr(self.browser, attr, None)
                if candidate is not None:
                    candidates.append(candidate)
            except Exception:
                pass
        try:
            candidate = getattr(getattr(self.browser, "_impl_obj", None), "_browser", None)
            if candidate is not None:
                candidates.append(candidate)
        except Exception:
            pass
        for candidate in candidates:
            target_id = id(candidate)
            if target_id in self._browser_lifecycle_listener_ids:
                continue
            try:
                on_fn = getattr(candidate, "on", None)
                if callable(on_fn):
                    on_fn("disconnected", lambda *_, target_id=target_id: self._handle_browser_disconnected_event(target_id))
                    self._browser_lifecycle_listener_ids.add(target_id)
                    _camoufox_debug("browser_disconnected_listener_registered", session_id=self.session_id, target_type=type(candidate).__name__, listener_count=len(self._browser_lifecycle_listener_ids))
            except Exception as exc:
                _camoufox_debug("browser_disconnected_listener_failed", session_id=self.session_id, target_type=type(candidate).__name__, error_type=type(exc).__name__, error_len=len(str(exc)), error_summary=_safe_text(exc), process_tree=_process_tree_snapshot())

    def _handle_browser_disconnected_event(self, target_id: int) -> None:
        process_before = _process_tree_detailed_snapshot()
        pages_before = len(self.pages)
        contexts_before = len(self.contexts)
        active_before = self.active_page_id or ""
        self._record_page_event(
            "browser_disconnected",
            active_before,
            target_id=target_id,
            pages_before=pages_before,
            contexts_before=contexts_before,
            process_tree=process_before,
            launch=self._last_launch_summary,
            crash_reports=_crash_report_snapshot(self._profile_dir or self._last_launch_summary.get("profile_dir", "")),
        )
        process_after = _process_tree_detailed_snapshot()
        _camoufox_debug(
            "browser_disconnected",
            session_id=self.session_id,
            target_id=target_id,
            browser_open=self.browser is not None,
            browser_connected=self._browser_connected(),
            page_count=pages_before,
            context_count=contexts_before,
            active_page_id=active_before,
            browser_generation=self._browser_generation,
            process_before=process_before,
            process_after=process_after,
            process_delta=_process_tree_delta(process_before, process_after),
            launch=self._last_launch_summary,
            crash_reports=_crash_report_snapshot(self._profile_dir or self._last_launch_summary.get("profile_dir", "")),
        )
        self._mark_launch_terminal(
            "browser_disconnected",
            "browser_disconnected",
            target_id=target_id,
            pages_before=pages_before,
            contexts_before=contexts_before,
            active_before=active_before,
            process_before=process_before,
            process_after=process_after,
        )
        self._clear_browser_runtime_state("browser_disconnected", target_id, pages_before, contexts_before, active_before)

    def _clear_browser_runtime_state(self, reason: str, target_id: int = 0, pages_before: int | None = None, contexts_before: int | None = None, active_before: str | None = None) -> None:
        if pages_before is None:
            pages_before = len(self.pages)
        if contexts_before is None:
            contexts_before = len(self.contexts)
        if active_before is None:
            active_before = self.active_page_id or ""
        self._browser_generation += 1
        self.browser = None
        self.contexts.clear()
        self.pages.clear()
        self.page_meta.clear()
        self.context_ids.clear()
        self._page_guid_to_id.clear()
        self._listener_page_ids.clear()
        self._page_terminal_ids.clear()
        self._browser_lifecycle_listener_ids.clear()
        self._pending_page_ids_by_context.clear()
        self._active_page_operations.clear()
        self.active_page_name = None
        self.active_page_id = None
        self._cm = None
        self._capturing = False
        self._capture_body = False
        self._network_requests.clear()
        self._nav_responses.clear()
        self._nav_responses_by_page.clear()
        self._route_handlers.clear()
        _camoufox_debug(
            "browser_runtime_state_cleared",
            session_id=self.session_id,
            reason=reason,
            target_id=target_id,
            browser_generation=self._browser_generation,
            pages_before=pages_before,
            contexts_before=contexts_before,
            active_page_before=active_before,
            pages_after=len(self.pages),
            contexts_after=len(self.contexts),
            process_tree=_process_tree_snapshot(),
        )

    async def _page_debug_summary(self, page: Page | None, page_id: str | None = None, title_timeout_s: float = 0.5) -> dict[str, Any]:
        if page is None:
            return {"page_id": page_id or "", "present": False}
        out: dict[str, Any] = {
            "page_id": page_id or self.page_id_for(page) or "",
            "context_id": self._context_id_for_page(page),
            "guid": self._page_guid(page),
            "present": True,
            "closed": True,
            "url": "",
            "title": "",
        }
        try:
            closed = self._page_closed(page)
            out["closed"] = closed
            if not closed:
                out["url"] = _safe_page_url(page, 240)
                try:
                    out["title"] = _safe_text(await _await_no_cancel_wait(page.title(), timeout=title_timeout_s), 240)
                except Exception as exc:
                    out["title_error_type"] = type(exc).__name__
                    out["title_error"] = _safe_text(exc, 240)
        except Exception as exc:
            out["error_type"] = type(exc).__name__
            out["error"] = _safe_text(exc, 240)
        return out

    async def _context_debug_snapshot(self, ctx: BrowserContext | None = None, page: Page | None = None, page_id: str | None = None, title_timeout_s: float = 0.4) -> dict[str, Any]:
        out: dict[str, Any] = {
            "registered_contexts": len(self.contexts),
            "registered_pages": len(self.pages),
            "active_page_id": self.active_page_id or "",
            "active_page_name": self.active_page_name or "",
            "browser_open": self.browser is not None,
            "browser_connected": self._browser_connected(),
            "browser_type": type(self.browser).__name__ if self.browser is not None else "",
        }
        if ctx is None and page is not None:
            try:
                ctx = page.context
            except Exception as exc:
                out["context_from_page_error_type"] = type(exc).__name__
                out["context_from_page_error"] = _safe_text(exc, 240)
        context_pages: list[Page] = []
        if ctx is not None:
            try:
                context_pages = list(ctx.pages)
            except Exception as exc:
                out["context_pages_error_type"] = type(exc).__name__
                out["context_pages_error"] = _safe_text(exc, 240)
        out["context_page_count"] = len(context_pages)
        page_summaries = []
        for candidate in context_pages[:8]:
            page_summaries.append(await self._page_debug_summary(candidate, self.page_id_for(candidate), title_timeout_s))
        out["context_pages"] = page_summaries
        if page is not None:
            out["selected_page"] = await self._page_debug_summary(page, page_id, title_timeout_s)
        return out

    async def _launch_debug_snapshot(self, phase: str, started: float, budget_ms: int | float | None, ctx: BrowserContext | None = None, page: Page | None = None, page_id: str | None = None, exc: Exception | None = None) -> dict[str, Any]:
        elapsed_ms = int((time.perf_counter() - started) * 1000)
        out = await self._context_debug_snapshot(ctx, page, page_id)
        out.update({
            "phase": phase,
            "elapsed_ms": elapsed_ms,
            "remaining_budget_ms": self._budget_remaining_ms(started, budget_ms),
            "runtime": _browser_runtime_snapshot(self.browser),
            "process_tree": _process_tree_snapshot(),
        })
        if exc is not None:
            out.update({
                "error_type": type(exc).__name__,
                "error_len": len(str(exc)),
                "error_summary": _safe_text(exc),
            })
        return out

    def _set_launch_phase(self, phase: str, started: float, attempt_id: str | None = None) -> None:
        self._active_launch_phase = phase
        self._active_launch_started = started
        if attempt_id is not None:
            self._active_launch_attempt_id = str(attempt_id or "")

    def _launch_active_before_privacy(self) -> bool:
        return bool(
            self._active_launch_phase
            and self._active_launch_phase != "launch_ready"
            and not self._active_launch_privacy_verified
        )

    def _mark_launch_terminal(self, reason: str, event_name: str, **fields: Any) -> None:
        if not self._launch_active_before_privacy():
            return
        if self._active_launch_terminal_reason:
            return
        payload = {
            "reason": str(reason or "unknown"),
            "event": str(event_name or "launch_terminal"),
            "active_launch_phase": self._active_launch_phase,
            "launch_elapsed_ms": int((time.perf_counter() - self._active_launch_started) * 1000) if self._active_launch_started > 0 else -1,
            "launch_attempt_id": self._active_launch_attempt_id,
            "session_id": self.session_id,
        }
        payload.update(fields)
        self._active_launch_terminal_reason = payload["reason"]
        self._active_launch_terminal_payload = payload
        terminal_event = self._active_launch_terminal_event
        if terminal_event is not None:
            terminal_event.set()
        _camoufox_debug("launch_terminal_event", **payload)

    def _launch_terminal_exception(self, phase: str) -> RuntimeError:
        payload = dict(self._active_launch_terminal_payload or {})
        reason = str(payload.get("reason") or self._active_launch_terminal_reason or "unknown")
        event_name = str(payload.get("event") or "launch_terminal")
        active_phase = str(payload.get("active_launch_phase") or phase or self._active_launch_phase or "launch_browser")
        return RuntimeError(f"Camoufox launch terminal event during {active_phase}: {reason} ({event_name})")

    def _raise_if_launch_terminal(self, phase: str) -> None:
        if not self._active_launch_terminal_reason:
            return
        _camoufox_debug(
            "launch_terminal_raise",
            phase=phase,
            reason=self._active_launch_terminal_reason,
            active_launch_phase=self._active_launch_phase,
            launch_elapsed_ms=int((time.perf_counter() - self._active_launch_started) * 1000) if self._active_launch_started > 0 else -1,
            payload=self._active_launch_terminal_payload,
        )
        raise self._launch_terminal_exception(phase)

    async def _await_page_result_for_launch(self, awaitable: Any, timeout: float, phase: str, launch_started: float) -> tuple[Any | None, asyncio.Task, bool]:
        task = asyncio.create_task(awaitable)
        terminal_task: asyncio.Task | None = None
        terminal_event = self._active_launch_terminal_event
        wait_set: set[asyncio.Task] = {task}
        if terminal_event is not None:
            terminal_task = asyncio.create_task(terminal_event.wait())
            wait_set.add(terminal_task)
        done, _ = await asyncio.wait(wait_set, timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
        if terminal_task is not None and terminal_task in done and terminal_event is not None and terminal_event.is_set():
            if not task.done():
                task.cancel()
                _track_timeout_task(task)
            raise self._launch_terminal_exception(phase)
        if task in done:
            if terminal_task is not None and not terminal_task.done():
                terminal_task.cancel()
                _track_timeout_task(terminal_task)
            return task.result(), task, False
        if terminal_task is not None and not terminal_task.done():
            terminal_task.cancel()
            _track_timeout_task(terminal_task)
        _track_timeout_task(task)
        return None, task, True

    async def _build_launch_failure_payload(self, exc: Exception, phase: str, started: float, budget_ms: int, ctx: BrowserContext | None = None, page: Page | None = None, cleanup_summary: dict[str, Any] | None = None, stderr_tail: str = "") -> dict[str, Any]:
        elapsed_ms = int((time.perf_counter() - started) * 1000)
        browser_connected = self._browser_connected()
        recent_events = self._page_event_tail(None, 32)
        last_event = recent_events[-1] if recent_events else {}
        summary = _launch_error_summary(exc, phase)
        payload = {
            "status": "error",
            "error": summary,
            "error_type": type(exc).__name__,
            "error_kind": _launch_error_kind(exc, browser_connected),
            "error_summary": summary,
            "phase": phase or "launch_browser",
            "elapsed_ms": elapsed_ms,
            "remaining_ms": self._budget_remaining_ms(started, budget_ms),
            "launch_attempt_id": self._active_launch_attempt_id,
            "attempt_id": self._active_launch_attempt_id,
            "session_id": self.session_id,
            "browser_open": self.browser is not None,
            "browser_connected": browser_connected,
            "context_count": len(self.contexts),
            "registered_pages": len(self.pages),
            "page_count": _context_page_count(ctx),
            "active_page_id": self.active_page_id or "",
            "active_launch_phase": self._active_launch_phase,
            "launch_terminal_reason": self._active_launch_terminal_reason,
            "launch_terminal_payload": dict(self._active_launch_terminal_payload or {}),
            "last_debug_event": last_event,
            "last_debug_event_name": str(last_event.get("event") or ""),
            "recent_page_events": recent_events,
            "launch_summary": dict(self._last_launch_summary or {}),
            "process_tree": _process_tree_detailed_snapshot(),
            "subprocesses": _subprocess_diagnostics_snapshot(),
            "stderr_tail": stderr_tail,
            "stderr_tail_len": len(stderr_tail),
            "diagnostics": await self._launch_debug_snapshot(phase or "launch_browser", started, budget_ms, ctx=ctx, page=page, exc=exc),
        }
        if cleanup_summary is not None:
            payload["cleanup_summary"] = cleanup_summary
        return payload

    def last_launch_failure_payload(self, exc: Exception | None = None, fallback_phase: str = "launch_browser") -> dict[str, Any]:
        payload = dict(self._last_launch_failure or {})
        if exc is not None:
            payload.setdefault("error_type", type(exc).__name__)
            payload.setdefault("error_kind", _launch_error_kind(exc, payload.get("browser_connected") if isinstance(payload.get("browser_connected"), bool) else None))
            payload.setdefault("error_summary", _launch_error_summary(exc, str(payload.get("phase") or fallback_phase)))
            payload.setdefault("error", str(payload.get("error_summary") or _launch_error_summary(exc, fallback_phase)))
        if not payload:
            summary = _launch_error_summary(exc, fallback_phase) if exc is not None else f"launch_browser failed during {fallback_phase}"
            payload = {
                "status": "error",
                "error": summary,
                "error_type": type(exc).__name__ if exc is not None else "RuntimeError",
                "error_kind": _launch_error_kind(exc, None) if exc is not None else "error",
                "error_summary": summary,
                "phase": fallback_phase,
                "elapsed_ms": 0,
                "remaining_ms": -1,
                "launch_attempt_id": self._active_launch_attempt_id,
                "attempt_id": self._active_launch_attempt_id,
                "session_id": self.session_id,
                "browser_open": self.browser is not None,
                "browser_connected": self._browser_connected(),
                "context_count": len(self.contexts),
                "registered_pages": len(self.pages),
                "page_count": -1,
                "active_page_id": self.active_page_id or "",
                "last_debug_event": {},
                "last_debug_event_name": "",
                "recent_page_events": self._page_event_tail(None, 32),
            }
        if not str(payload.get("error") or ""):
            payload["error"] = str(payload.get("error_summary") or payload.get("error_type") or "launch_browser failed")
        return payload

    def _next_diagnostic_id(self) -> int:
        self._diagnostic_generation += 1
        return self._diagnostic_generation

    def _remember_launch_summary(
        self,
        selected_launch_path: str,
        executable_path: str | None,
        profile_info: dict[str, Any],
        prefs: dict[str, Any] | None,
        launch_policy: dict[str, Any] | None,
        context_plan: dict[str, Any],
        window_diag: dict[str, Any],
        timeout_ms: int,
        ff_version: Any,
        ff_version_source: str,
    ) -> None:
        profile_data = dict(profile_info or {})
        profile_candidates = list((launch_policy or {}).get("profile_path_candidates") or [])
        resolved_profile_dir = self._profile_dir or profile_data.get("profile_dir", "") or (str(profile_candidates[0]) if profile_candidates else "")
        self._last_launch_summary = {
            "browser_generation": self._browser_generation,
            "selected_launch_path": selected_launch_path,
            "executable": _path_info(executable_path),
            "profile": profile_data,
            "profile_dir": resolved_profile_dir,
            "profile_locks": _profile_lock_snapshot(resolved_profile_dir),
            "prefs": _prefs_summary(prefs),
            "launch_policy": dict(launch_policy or {}),
            "hashes": {
                "prefs": _hash_summary(prefs or {}),
                "launch_policy": _hash_summary(launch_policy or {}),
                "context_plan": _hash_summary(context_plan or {}),
            },
            "ua_policy": context_plan.get("ua_policy"),
            "ua_override": bool(context_plan.get("user_agent")),
            "service_workers": dict(context_plan.get("context_options") or {}).get("service_workers", ""),
            "block_webrtc": bool(context_plan.get("block_webrtc")),
            "persistent_context": bool(context_plan.get("persistent_context")),
            "diagnostic_variant": context_plan.get("diagnostic_variant", ""),
            "window": dict(window_diag or {}),
            "timeout_ms": int(timeout_ms or 0),
            "ff_version": ff_version,
            "ff_version_source": ff_version_source,
        }

    def _launch_diagnostics(self) -> dict[str, Any]:
        profile_dir = self._profile_dir or self._last_launch_summary.get("profile_dir", "")
        return {
            "browser_generation": self._browser_generation,
            "profile_dir": profile_dir,
            "profile_locks": _profile_lock_snapshot(profile_dir),
            "launch": dict(self._last_launch_summary or {}),
            "crash_reports": _crash_report_snapshot(profile_dir),
            "subprocesses": _subprocess_diagnostics_snapshot(),
        }

    def begin_page_operation(self, page_id: str | None, operation: str, target_url: str | None, generation: int, external_operation_id: Any = None) -> int:
        self._operation_counter += 1
        op_id = self._operation_counter
        pid = page_id or ""
        now_ms = int(time.time() * 1000)
        process_before = _process_tree_detailed_snapshot()
        external_id = _safe_text(external_operation_id, 120) if external_operation_id is not None else ""
        record = {
            "operation_id": op_id,
            "external_operation_id": external_id,
            "correlation_id": external_id or str(op_id),
            "operation": _safe_text(operation, 160),
            "page_id": pid,
            "context_id": self.page_meta.get(pid, {}).get("context_id", ""),
            "target_url": _safe_text(target_url or "", 700),
            "target_domain": _target_domain(target_url),
            "diagnostic_generation": generation,
            "browser_generation": self._browser_generation,
            "started_ms": now_ms,
            "started_perf": time.perf_counter(),
            "profile_dir": self._profile_dir or self._last_launch_summary.get("profile_dir", ""),
            "process_before": process_before,
            "crash_reports_before": _crash_report_snapshot(self._profile_dir or self._last_launch_summary.get("profile_dir", ""), now_ms),
        }
        if pid:
            self._active_page_operations[pid] = record
        _camoufox_debug(
            "page_operation_begin",
            session_id=self.session_id,
            operation_id=op_id,
            external_operation_id=external_id,
            correlation_id=external_id or str(op_id),
            operation=_safe_text(operation, 160),
            page_id=pid,
            context_id=record.get("context_id", ""),
            target_url=_safe_text(target_url or "", 700),
            target_domain=_target_domain(target_url),
            diagnostic_generation=generation,
            browser_generation=self._browser_generation,
            profile_dir=record.get("profile_dir", ""),
            launch=self._last_launch_summary,
            process_before=process_before,
            crash_reports_before=record.get("crash_reports_before"),
        )
        return op_id

    def finish_page_operation(self, page_id: str | None, operation_id: int | None, status: str, exc: Exception | None = None) -> dict[str, Any]:
        pid = page_id or ""
        record = self._active_page_operations.get(pid) if pid else None
        if record and operation_id and int(record.get("operation_id") or 0) != int(operation_id):
            record = None
        if record is None:
            return {}
        after = _process_tree_detailed_snapshot()
        elapsed_ms = int((time.perf_counter() - float(record.get("started_perf") or time.perf_counter())) * 1000)
        crash_reports_after = _crash_report_snapshot(str(record.get("profile_dir") or ""), int(record.get("started_ms") or 0))
        out = {
            "operation_id": int(record.get("operation_id") or 0),
            "external_operation_id": record.get("external_operation_id", ""),
            "correlation_id": record.get("correlation_id", str(record.get("operation_id") or 0)),
            "operation": record.get("operation", ""),
            "page_id": pid,
            "context_id": record.get("context_id", ""),
            "status": _safe_text(status, 120),
            "elapsed_ms": elapsed_ms,
            "target_url": record.get("target_url", ""),
            "target_domain": record.get("target_domain", ""),
            "diagnostic_generation": record.get("diagnostic_generation"),
            "browser_generation": record.get("browser_generation"),
            "profile_dir": record.get("profile_dir", ""),
            "process_before": record.get("process_before"),
            "process_after": after,
            "process_delta": _process_tree_delta(record.get("process_before"), after),
            "crash_reports_before": record.get("crash_reports_before"),
            "crash_reports_after": crash_reports_after,
            "launch": self._last_launch_summary,
        }
        if exc is not None:
            out["error_kind"] = _page_error_kind(exc)
            out["error_type"] = type(exc).__name__
            out["error_summary"] = _safe_text(exc, 900)
        if pid and self._active_page_operations.get(pid) is record:
            self._active_page_operations.pop(pid, None)
        _camoufox_debug("page_operation_end", session_id=self.session_id, **out)
        return out

    def handle_page_operation_exception(self, page_id: str | None, page: Page | None, operation: str, exc: Exception) -> dict[str, Any]:
        pid = page_id or self.page_id_for(page) or ""
        kind = _page_error_kind(exc)
        fields = {
            "operation": _safe_text(operation, 160),
            "page_id": pid,
            "error_kind": kind,
            "error_type": type(exc).__name__,
            "error_summary": _safe_text(exc, 900),
            "process_tree": _process_tree_detailed_snapshot(),
            "launch": self._last_launch_summary,
            "crash_reports": _crash_report_snapshot(self._profile_dir or self._last_launch_summary.get("profile_dir", "")),
        }
        _camoufox_debug("page_operation_exception", session_id=self.session_id, **fields)
        if pid and kind == "page_crash":
            self._on_page_crashed(pid, page, f"{operation}_exception", exc)
        elif pid and kind in {"target_closed", "context_closed", "browser_closed"}:
            self._on_page_closed(pid, page, f"{operation}_exception", exc)
        return fields

    def _record_page_event(self, event: str, page_id: str | None = None, page: Page | None = None, **fields: Any) -> dict[str, Any]:
        self._page_event_counter += 1
        resolved_page_id = page_id or self.page_id_for(page) or ""
        url = _safe_page_url(page, 360) if page is not None else _safe_text(fields.get("url") or "", 360)
        entry: dict[str, Any] = {
            "seq": self._page_event_counter,
            "event": event,
            "ts_ms": int(time.time() * 1000),
            "session_id": self.session_id,
            "page_id": resolved_page_id,
            "context_id": self.page_meta.get(resolved_page_id or "", {}).get("context_id") or (self._context_id_for_page(page) if page is not None else ""),
            "active_page_id": self.active_page_id or "",
            "url": url,
            "domain": _target_domain(url),
            "browser_connected": self._browser_connected(),
            "browser_generation": self._browser_generation,
        }
        for key, value in fields.items():
            if isinstance(value, BaseException):
                entry[key] = _safe_text(value, 500)
                entry[f"{key}_type"] = type(value).__name__
            elif isinstance(value, str):
                entry[key] = _safe_text(value, 900)
            else:
                entry[key] = value
        self._recent_page_events.append(entry)
        return entry

    def _page_event_tail(self, page_id: str | None = None, limit: int = 40) -> list[dict[str, Any]]:
        events = list(self._recent_page_events)
        if page_id:
            events = [event for event in events if event.get("page_id") in {page_id, ""}]
        return events[-max(1, min(limit, 80)):]

    async def _page_activity_snapshot(self, page: Page | None, timeout_s: float = 2.0) -> dict[str, Any]:
        if page is None:
            return {}
        try:
            return await _await_no_cancel_wait(
                page.evaluate(
                    """() => ({
                        visibilityState: String(document.visibilityState || ""),
                        hidden: !!document.hidden,
                        hasFocus: typeof document.hasFocus === "function" ? document.hasFocus() : false,
                        readyState: String(document.readyState || ""),
                        href: String(location.href || ""),
                        title: String(document.title || "")
                    })"""
                ),
                timeout=timeout_s,
            )
        except Exception as exc:
            return {"activity_error_type": type(exc).__name__, "activity_error": _safe_text(exc, 300)}

    async def _screenshot_metadata(self, page: Page | None, timeout_s: float = 3.0) -> dict[str, Any]:
        if page is None:
            return {"captured": False, "bytes": 0}
        try:
            data = await _await_no_cancel_wait(page.screenshot(full_page=False), timeout=timeout_s)
            return {"captured": True, "bytes": len(data or b""), "format": "png", "full_page": False}
        except Exception as exc:
            return {"captured": False, "bytes": 0, "error_type": type(exc).__name__, "error": _safe_text(exc, 300)}

    def _registered_page_records(self) -> list[dict[str, Any]]:
        records: list[dict[str, Any]] = []
        seen: set[str] = set()
        for page_id, meta_src in list(self.page_meta.items()):
            if len(records) >= 32:
                break
            meta = dict(meta_src or {})
            page = self.pages.get(page_id)
            record: dict[str, Any] = {
                "page_id": page_id,
                "context_id": meta.get("context_id", ""),
                "active": page_id == self.active_page_id,
                "registered": page is not None,
                "closed": bool(meta.get("closed")),
                "crashed": bool(meta.get("crashed")),
                "terminal_reason": meta.get("terminal_reason", ""),
                "terminal_source": meta.get("terminal_source", ""),
                "browser_generation": meta.get("browser_generation"),
                "created_ms": meta.get("created_ms"),
                "closed_ms": meta.get("closed_ms"),
                "last_used_ms": meta.get("last_used_ms"),
                "url": "",
            }
            if page is not None:
                record["url"] = _safe_page_url(page, 300)
                try:
                    record["closed"] = bool(self._page_closed(page))
                except Exception as exc:
                    record["closed_probe_error_type"] = type(exc).__name__
                    record["closed_probe_error"] = _safe_text(exc, 180)
            records.append(record)
            seen.add(page_id)
        for page_id, page in list(self.pages.items()):
            if len(records) >= 32:
                break
            if page_id in seen:
                continue
            try:
                closed = bool(self._page_closed(page))
            except Exception:
                closed = True
            records.append({
                "page_id": page_id,
                "context_id": self._context_id_for_page(page),
                "active": page_id == self.active_page_id,
                "registered": True,
                "closed": closed,
                "crashed": False,
                "terminal_reason": "",
                "terminal_source": "",
                "browser_generation": self._browser_generation,
                "created_ms": None,
                "closed_ms": None,
                "last_used_ms": None,
                "url": _safe_page_url(page, 300),
            })
        return records

    async def navigation_diagnostic_snapshot(
        self,
        action: str,
        page: Page | None,
        page_id: str | None,
        target_url: str | None,
        generation: int,
        started: float,
        include_screenshot_metadata: bool = False,
        last_error: str = "",
    ) -> dict[str, Any]:
        resolved_page_id = page_id or self.page_id_for(page) or ""
        current_url = _safe_page_url(page, 500) if page is not None else ""
        out: dict[str, Any] = {
            "action": action,
            "diagnostic_generation": generation,
            "browser_generation": self._browser_generation,
            "session_id": self.session_id,
            "page_id": resolved_page_id,
            "context_id": self.page_meta.get(resolved_page_id or "", {}).get("context_id") or (self._context_id_for_page(page) if page is not None else ""),
            "active_page_id": self.active_page_id or "",
            "target_url": _safe_text(target_url or "", 500),
            "target_domain": _target_domain(target_url),
            "current_url": current_url,
            "current_domain": _target_domain(current_url),
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
            "page_count": len(self.pages),
            "pages": self._registered_page_records(),
            "context_count": len(self.contexts),
            "browser_open": self.browser is not None,
            "browser_connected": self._browser_connected(),
            "process_tree": _process_tree_snapshot(),
            "process_tree_detail": _process_tree_detailed_snapshot(),
            "browser_runtime": _browser_runtime_snapshot(self.browser),
            "visible_window_probe": _visible_window_snapshot(),
            "last_error": _safe_text(last_error or self._last_error, 700),
            "recent_page_events": self._page_event_tail(resolved_page_id),
            "active_operation": dict(self._active_page_operations.get(resolved_page_id or "", {})),
            "launch_diagnostics": self._launch_diagnostics(),
            "subprocesses": _subprocess_diagnostics_snapshot(),
            "response_chain": self.nav_responses_for_page(resolved_page_id),
        }
        response_chain = out["response_chain"] if isinstance(out.get("response_chain"), list) else []
        if response_chain:
            document_status = None
            for item in reversed(response_chain):
                if isinstance(item, dict) and (item.get("resource_type") == "document" or item.get("url") == current_url):
                    document_status = item.get("status")
                    break
            out["response_chain_count"] = len(response_chain)
            out["last_response_status"] = response_chain[-1].get("status") if isinstance(response_chain[-1], dict) else None
            out["document_response_status"] = document_status
        if resolved_page_id:
            meta = dict(self.page_meta.get(resolved_page_id, {}))
            out["page_meta"] = {
                "page_id": meta.get("page_id", resolved_page_id),
                "context_id": meta.get("context_id", ""),
                "guid": meta.get("guid", ""),
                "closed": bool(meta.get("closed")),
                "crashed": bool(meta.get("crashed")),
                "terminal_reason": meta.get("terminal_reason", ""),
                "terminal_source": meta.get("terminal_source", ""),
                "browser_generation": meta.get("browser_generation"),
                "created_ms": meta.get("created_ms"),
                "closed_ms": meta.get("closed_ms"),
                "last_used_ms": meta.get("last_used_ms"),
            }
            out["page_close_reason"] = meta.get("terminal_reason", "")
            out["page_close_source"] = meta.get("terminal_source", "")
        if page is None:
            out["page_present"] = False
            out["page_closed"] = True
            return out
        out["page_present"] = True
        try:
            out["page_closed"] = self._page_closed(page)
        except Exception as exc:
            out["page_closed"] = True
            out["page_closed_probe_error_type"] = type(exc).__name__
            out["page_closed_probe_error"] = _safe_text(exc, 300)
        if not bool(out.get("page_closed")):
            try:
                out["title"] = _safe_text(await _await_no_cancel_wait(page.title(), timeout=1.5), 500)
            except Exception as exc:
                out["title_error_type"] = type(exc).__name__
                out["title_error"] = _safe_text(exc, 300)
            out["activity"] = await self._page_activity_snapshot(page)
            if include_screenshot_metadata:
                out["screenshot"] = await self._screenshot_metadata(page)
        return out

    def log_navigation_diagnostic(self, event: str, snapshot: dict[str, Any]) -> None:
        _camoufox_debug(event, **snapshot)

    def _slug(self, value: str | None) -> str:
        value = str(value or "page").strip().lower()
        value = _re.sub(r"[^a-z0-9_.:-]+", "-", value).strip("-")
        return value[:48] or "page"

    def _next_page_id(self, hint: str | None = None) -> str:
        base = self._slug(hint)
        while True:
            self._page_counter += 1
            candidate = f"{base}-{self._page_counter:04d}"
            if candidate not in self.pages and candidate not in self.page_meta:
                return candidate

    def _context_id_for_page(self, page: Page | None) -> str:
        if page is None:
            return "default"
        try:
            key = id(page.context)
            if key in self.context_ids:
                return self.context_ids[key]
        except Exception:
            pass
        return "default"

    def _register_context(self, context_id: str, ctx: BrowserContext) -> None:
        self.context_ids[id(ctx)] = context_id
        page_listener_registered = False
        close_listener_registered = False
        service_worker_listener_registered = False
        try:
            ctx.on("page", lambda page, cid=context_id: self._register_pending_context_page(page, cid))
            page_listener_registered = True
        except Exception as exc:
            _camoufox_debug("context_listener_failed", session_id=self.session_id, context_id=context_id, error_type=type(exc).__name__, error=_safe_text(exc, 300))
        try:
            ctx.on("close", lambda *_, cid=context_id, c=ctx: self._handle_context_close_event(cid, c))
            close_listener_registered = True
        except Exception as exc:
            _camoufox_debug("context_close_listener_failed", session_id=self.session_id, context_id=context_id, error_type=type(exc).__name__, error_len=len(str(exc)), error_summary=_safe_text(exc, 300))
        try:
            ctx.on("serviceworker", lambda worker, cid=context_id: self._handle_service_worker_event(worker, cid))
            service_worker_listener_registered = True
        except Exception as exc:
            _camoufox_debug("context_serviceworker_listener_failed", session_id=self.session_id, context_id=context_id, error_type=type(exc).__name__, error_len=len(str(exc)), error_summary=_safe_text(exc, 300))
        try:
            ctx_pages_count = len(getattr(ctx, "pages", []) or [])
        except Exception:
            ctx_pages_count = -1
        _camoufox_debug(
            "context_registered",
            session_id=self.session_id,
            context_id=context_id,
            pages=ctx_pages_count,
            listener_registered=page_listener_registered,
            page_listener_registered=page_listener_registered,
            close_listener_registered=close_listener_registered,
            service_worker_listener_registered=service_worker_listener_registered,
            browser_context_count=_browser_context_count(self.browser),
            registered_contexts=len(self.contexts),
            pending_page_ids=sum(len(v) for v in self._pending_page_ids_by_context.values()),
        )

    def _handle_context_close_event(self, context_id: str, ctx: BrowserContext | None) -> None:
        started = time.perf_counter()
        process_before = _process_tree_detailed_snapshot()
        pages_before = _context_page_count(ctx)
        page_ids = [
            pid for pid, meta in list(self.page_meta.items())
            if meta.get("context_id") == context_id and pid in self.pages
        ]
        for pid in page_ids:
            self._on_page_closed(pid, self.pages.get(pid), "context_close_event")
        if ctx is not None:
            self.context_ids.pop(id(ctx), None)
        if self.contexts.get(context_id) is ctx:
            self.contexts.pop(context_id, None)
        process_after = _process_tree_detailed_snapshot()
        _camoufox_debug(
            "context_close_event",
            session_id=self.session_id,
            context_id=context_id,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            pages_before=pages_before,
            pages_marked_closed=len(page_ids),
            registered_contexts=len(self.contexts),
            registered_pages=len(self.pages),
            active_page_id=self.active_page_id or "",
            browser_open=self.browser is not None,
            browser_connected=self._browser_connected(),
            browser_generation=self._browser_generation,
            active_operations=[dict(self._active_page_operations.get(pid, {})) for pid in page_ids],
            process_before=process_before,
            process_after=process_after,
            process_delta=_process_tree_delta(process_before, process_after),
            launch=self._last_launch_summary,
            crash_reports=_crash_report_snapshot(self._profile_dir or self._last_launch_summary.get("profile_dir", ""), int(time.time() * 1000) - 60000),
        )
        self._schedule_default_page_recovery("context_close_event")

    def _live_page_count(self) -> int:
        count = 0
        for page in list(self.pages.values()):
            if not self._page_closed(page):
                count += 1
        return count

    def _requested_page_id_blocker(self, page_id: str | None) -> dict[str, Any] | None:
        pid = self._slug(page_id) if page_id else ""
        if not pid:
            return None
        live = self.pages.get(pid)
        if live is not None and not self._page_closed(live):
            return {
                "success": False,
                "status": "blocked",
                "error": "browser_page_id_unavailable",
                "error_code": "page_id_already_live",
                "page_id": pid,
                "active_page_id": self.active_page_id,
                "page_count": len(self.pages),
            }
        for cid, queue in self._pending_page_ids_by_context.items():
            for entry in queue:
                pending_id = entry.get("page_id") if isinstance(entry, dict) else entry
                if str(pending_id or "") == pid:
                    return {
                        "success": False,
                        "status": "blocked",
                        "error": "browser_page_id_unavailable",
                        "error_code": "page_id_pending",
                        "page_id": pid,
                        "pending_context_id": cid,
                        "active_page_id": self.active_page_id,
                        "page_count": len(self.pages),
                    }
        return None

    def _schedule_default_page_recovery(self, reason: str) -> None:
        if self.browser is None or not self._browser_connected():
            return
        if self._live_page_count() > 0 and self.active_page_id:
            return
        if self._page_recovery_task is not None and not self._page_recovery_task.done():
            _camoufox_debug(
                "page_recovery_already_pending",
                session_id=self.session_id,
                reason=reason,
                active_page_id=self.active_page_id or "",
                page_count=len(self.pages),
            )
            return
        try:
            self._page_recovery_task = asyncio.get_running_loop().create_task(self._recover_default_page(reason))
            _track_timeout_task(self._page_recovery_task)
            _camoufox_debug(
                "page_recovery_scheduled",
                session_id=self.session_id,
                reason=reason,
                active_page_id=self.active_page_id or "",
                page_count=len(self.pages),
                browser_connected=self._browser_connected(),
            )
        except RuntimeError as exc:
            _camoufox_debug(
                "page_recovery_schedule_failed",
                session_id=self.session_id,
                reason=reason,
                error_type=type(exc).__name__,
                error_summary=_safe_text(exc, 300),
                process_tree=_process_tree_snapshot(),
            )

    async def _recover_default_page(self, reason: str) -> None:
        started = time.perf_counter()
        await asyncio.sleep(0.05)
        if self.browser is None or not self._browser_connected():
            _camoufox_debug(
                "page_recovery_skipped",
                session_id=self.session_id,
                reason=reason,
                skip_reason="browser_unavailable",
                elapsed_ms=int((time.perf_counter() - started) * 1000),
                process_tree=_process_tree_snapshot(),
            )
            return
        if self._live_page_count() > 0 and self.active_page_id:
            _camoufox_debug(
                "page_recovery_skipped",
                session_id=self.session_id,
                reason=reason,
                skip_reason="live_page_available",
                active_page_id=self.active_page_id or "",
                page_count=len(self.pages),
                elapsed_ms=int((time.perf_counter() - started) * 1000),
            )
            return
        result = await self.new_page(make_active=True)
        if isinstance(result, dict) and result.get("error"):
            _camoufox_debug(
                "page_recovery_failed",
                session_id=self.session_id,
                reason=reason,
                elapsed_ms=int((time.perf_counter() - started) * 1000),
                result_status=_safe_text(result.get("status", ""), 120),
                error_code=_safe_text(result.get("error_code", ""), 120),
                error_summary=_safe_text(result.get("error", ""), 500),
                process_tree=_process_tree_snapshot(),
            )
            return
        _camoufox_debug(
            "page_recovery_created",
            session_id=self.session_id,
            reason=reason,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            page_id=result.get("page_id", "") if isinstance(result, dict) else "",
            active_page_id=self.active_page_id or "",
            page_count=len(self.pages),
        )

    def _queue_pending_page_id(self, context_id: str, page_id: str | None, make_active: bool = True) -> None:
        pid = self._slug(page_id) if page_id else ""
        if pid:
            cid = context_id or "default"
            queue = self._pending_page_ids_by_context.setdefault(cid, [])
            queue.append({"page_id": pid, "make_active": bool(make_active)})
            _camoufox_debug(
                "pending_page_queued",
                session_id=self.session_id,
                context_id=cid,
                page_id=pid,
                make_active=bool(make_active),
                queue_len=len(queue),
            )

    def _pop_pending_page_id(self, context_id: str) -> dict[str, Any]:
        cid = context_id or "default"
        queue = self._pending_page_ids_by_context.get(cid)
        if not queue:
            return {"page_id": None, "make_active": True}
        pending = queue.pop(0)
        if not queue:
            self._pending_page_ids_by_context.pop(cid, None)
        if isinstance(pending, dict):
            return {"page_id": pending.get("page_id"), "make_active": bool(pending.get("make_active", True))}
        return {"page_id": pending, "make_active": True}

    def _discard_pending_page_id(self, context_id: str, page_id: str | None) -> None:
        pid = self._slug(page_id) if page_id else ""
        cid = context_id or "default"
        queue = self._pending_page_ids_by_context.get(cid)
        if not pid or not queue:
            return
        before = len(queue)
        queue[:] = [entry for entry in queue if str(entry.get("page_id") if isinstance(entry, dict) else entry) != pid]
        if not queue:
            self._pending_page_ids_by_context.pop(cid, None)
        if len(queue) != before:
            _camoufox_debug(
                "pending_page_discarded",
                session_id=self.session_id,
                context_id=cid,
                page_id=pid,
                removed=before - len(queue),
                queue_len=len(queue),
            )

    def _register_pending_context_page(self, page: Page, context_id: str) -> str:
        cid = context_id or "default"
        queue_before = list(self._pending_page_ids_by_context.get(cid, []))
        active_before = self.active_page_id or ""
        closed_at_entry = self._page_closed(page)
        pending = self._pop_pending_page_id(context_id)
        preferred_id = pending.get("page_id") if isinstance(pending, dict) else None
        make_active = bool(pending.get("make_active", True)) if isinstance(pending, dict) else True
        pid = self._register_page(page, preferred_id, make_active, "context_page", context_id)
        queue_after = list(self._pending_page_ids_by_context.get(cid, []))
        try:
            page_context = page.context
        except Exception:
            page_context = None
        _camoufox_debug(
            "pending_context_page_registered",
            session_id=self.session_id,
            context_id=cid,
            requested_page_id=preferred_id or "",
            page_id=pid,
            make_active=make_active,
            active_before=active_before,
            active_page_id=self.active_page_id or "",
            page_closed_at_entry=closed_at_entry,
            queue_before_len=len(queue_before),
            queue_after_len=len(queue_after),
            queue_before=queue_before,
            queue_after=queue_after,
            page_count=len(self.pages),
            context_page_count=_context_page_count(page_context),
            active_launch_phase=self._active_launch_phase,
            recent_page_events=self._page_event_tail(None, 12),
        )
        return pid

    def _first_live_page(self, pages: list[Page]) -> Page | None:
        for candidate in pages:
            if not self._page_closed(candidate):
                return candidate
        return None

    async def _wait_for_existing_context_page(self, ctx: BrowserContext, timeout_s: float, event_prefix: str, launch_started: float) -> tuple[Page | None, list[Page]]:
        started = time.perf_counter()
        deadline = started + max(0.0, timeout_s)
        polls = 0
        last_page_count = -1
        last_pages: list[Page] = []
        while True:
            pages: list[Page] = []
            try:
                pages = list(ctx.pages)
                last_pages = pages
            except Exception as exc:
                _camoufox_debug(
                    f"{event_prefix}_snapshot_failed",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    wait_ms=int((time.perf_counter() - started) * 1000),
                    error_type=type(exc).__name__,
                    error_len=len(str(exc)),
                    error_summary=_safe_text(exc),
                )
            if len(pages) != last_page_count:
                last_page_count = len(pages)
                _camoufox_debug(
                    f"{event_prefix}_poll",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    wait_ms=int((time.perf_counter() - started) * 1000),
                    pages_seen=len(pages),
                    polls=polls,
                )
            page = self._first_live_page(pages)
            if page is not None:
                _camoufox_debug(
                    f"{event_prefix}_ready",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    wait_ms=int((time.perf_counter() - started) * 1000),
                    pages_seen=len(pages),
                    polls=polls,
                    url=_safe_page_url(page, 160),
                )
                return page, pages
            now = time.perf_counter()
            if now >= deadline:
                _camoufox_debug(
                    f"{event_prefix}_timeout",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    wait_ms=int((now - started) * 1000),
                    pages_seen=len(last_pages),
                    polls=polls,
                    timeout_s=timeout_s,
                    process_tree=_process_tree_snapshot(),
                )
                return None, last_pages
            polls += 1
            await asyncio.sleep(min(0.10, max(0.01, deadline - now)))

    async def _final_late_page_recovery(self, ctx: BrowserContext, known_pages: list[Page], event_prefix: str, launch_started: float, launch_timeout_ms: int, pending_task: asyncio.Task | None = None) -> Page | None:
        remaining_ms = self._budget_remaining_ms(launch_started, launch_timeout_ms)
        if remaining_ms <= 500:
            return None
        wait_s = min(45.0, max(1.0, (remaining_ms - 500) / 1000.0))
        _camoufox_debug(
            f"{event_prefix}_late_page_final_wait_begin",
            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
            timeout_s=wait_s,
            remaining_budget_ms=remaining_ms,
            reserved_ms=500,
            known_pages=len(known_pages),
            pending_task=bool(pending_task is not None),
            pending_task_done=bool(pending_task is not None and pending_task.done()),
            process_tree=_process_tree_snapshot(),
        )
        page = await self._wait_for_late_page(ctx, known_pages, wait_s, event_prefix, launch_started, pending_task)
        if page is not None:
            _camoufox_debug(
                f"{event_prefix}_late_page_final_recovered",
                elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                remaining_budget_ms=self._budget_remaining_ms(launch_started, launch_timeout_ms),
                state=await self._launch_debug_snapshot(event_prefix, launch_started, launch_timeout_ms, ctx=ctx, page=page),
            )
        return page

    async def _wait_for_late_page(self, ctx: BrowserContext, known_pages: list[Page], timeout_s: float, event_prefix: str, launch_started: float, pending_task: asyncio.Task | None = None) -> Page | None:
        known_ids = {id(page) for page in known_pages}
        started = time.perf_counter()
        deadline = started + max(0.0, timeout_s)
        polls = 0
        last_page_count = -1
        _camoufox_debug(
            f"{event_prefix}_late_page_wait_begin",
            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
            timeout_s=timeout_s,
            known_pages=len(known_pages),
            pending_task_id=id(pending_task) if pending_task is not None else 0,
            pending_page_task=_pending_page_task_state(pending_task),
            context_page_count=_context_page_count(ctx),
            registered_pages=len(self.pages),
            active_page_id=self.active_page_id or "",
            active_launch_phase=self._active_launch_phase,
        )
        while True:
            self._raise_if_launch_terminal(event_prefix)
            if pending_task is not None and pending_task.done():
                try:
                    candidate = pending_task.result()
                except Exception as exc:
                    _camoufox_debug(
                        f"{event_prefix}_late_page_task_failed",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        wait_ms=int((time.perf_counter() - started) * 1000),
                        error_type=type(exc).__name__,
                        error_len=len(str(exc)),
                        error_summary=_safe_text(exc),
                        pending_task_id=id(pending_task),
                        pending_page_task=_pending_page_task_state(pending_task),
                    )
                    pending_task = None
                else:
                    if candidate is not None and not self._page_closed(candidate):
                        _camoufox_debug(
                            f"{event_prefix}_late_page_task_recovered",
                            elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                            wait_ms=int((time.perf_counter() - started) * 1000),
                            pages_seen=-1,
                            polls=polls,
                            url=_safe_page_url(candidate, 160),
                            pending_task_id=id(pending_task),
                            pending_page_task=_pending_page_task_state(pending_task),
                            context_page_count=_context_page_count(ctx),
                            registered_pages=len(self.pages),
                            active_launch_phase=self._active_launch_phase,
                        )
                        return candidate
                    _camoufox_debug(
                        f"{event_prefix}_late_page_task_rejected",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        wait_ms=int((time.perf_counter() - started) * 1000),
                        pending_task_id=id(pending_task),
                        pending_page_task=_pending_page_task_state(pending_task),
                        page_present=candidate is not None,
                        page_closed=self._page_closed(candidate) if candidate is not None else True,
                        url=_safe_page_url(candidate, 160) if candidate is not None else "",
                        context_page_count=_context_page_count(ctx),
                        registered_pages=len(self.pages),
                        active_launch_phase=self._active_launch_phase,
                    )
                    pending_task = None
            pages: list[Page] = []
            try:
                pages = list(ctx.pages)
            except Exception as exc:
                _camoufox_debug(
                    f"{event_prefix}_late_page_snapshot_failed",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    wait_ms=int((time.perf_counter() - started) * 1000),
                    error_type=type(exc).__name__,
                    error_len=len(str(exc)),
                    error_summary=_safe_text(exc),
                )
            registry_keys: list[str] = []
            try:
                registry_keys = [str(k) for k in self.pages.keys()]
            except Exception:
                registry_keys = []
            if len(pages) != last_page_count:
                last_page_count = len(pages)
                _camoufox_debug(
                    f"{event_prefix}_late_page_poll",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    wait_ms=int((time.perf_counter() - started) * 1000),
                    pages_seen=len(pages),
                    polls=polls,
                    registered_pages_count=len(self.pages),
                    registry_keys=registry_keys,
                    active_page_id=self.active_page_id or "",
                    pending_page_task=_pending_page_task_state(pending_task),
                )
            for candidate in pages:
                if id(candidate) in known_ids:
                    continue
                if not self._page_closed(candidate):
                    _camoufox_debug(
                        f"{event_prefix}_late_page_recovered",
                        elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                        wait_ms=int((time.perf_counter() - started) * 1000),
                        pages_seen=len(pages),
                        polls=polls,
                        url=_safe_page_url(candidate, 160),
                        context_page_count=_context_page_count(ctx),
                        registered_pages=len(self.pages),
                        active_launch_phase=self._active_launch_phase,
                        source="ctx_pages",
                    )
                    return candidate
                _camoufox_debug(
                    f"{event_prefix}_late_page_rejected_closed",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    wait_ms=int((time.perf_counter() - started) * 1000),
                    pages_seen=len(pages),
                    polls=polls,
                    url=_safe_page_url(candidate, 160),
                    context_page_count=_context_page_count(ctx),
                    registered_pages=len(self.pages),
                    active_launch_phase=self._active_launch_phase,
                )
            try:
                registry_items = list(self.pages.items())
            except Exception as registry_exc:
                registry_items = []
                _camoufox_debug(
                    f"{event_prefix}_late_page_registry_snapshot_failed",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    wait_ms=int((time.perf_counter() - started) * 1000),
                    error_type=type(registry_exc).__name__,
                    error_summary=_safe_text(registry_exc),
                )
            for pid, registered_page in registry_items:
                if registered_page is None:
                    continue
                if id(registered_page) in known_ids:
                    continue
                if registered_page in pages:
                    continue
                try:
                    if self._page_closed(registered_page):
                        continue
                except Exception:
                    continue
                _camoufox_debug(
                    f"{event_prefix}_late_page_recovered",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    wait_ms=int((time.perf_counter() - started) * 1000),
                    pages_seen=len(pages),
                    polls=polls,
                    url=_safe_page_url(registered_page, 160),
                    context_page_count=_context_page_count(ctx),
                    registered_pages=len(self.pages),
                    active_launch_phase=self._active_launch_phase,
                    source="self_pages",
                    registry_page_id=str(pid or ""),
                    active_page_id=self.active_page_id or "",
                )
                return registered_page
            now = time.perf_counter()
            if now >= deadline:
                _camoufox_debug(
                    f"{event_prefix}_late_page_unavailable",
                    elapsed_ms=int((time.perf_counter() - launch_started) * 1000),
                    wait_ms=int((now - started) * 1000),
                    pages_seen=len(pages),
                    polls=polls,
                    timeout_s=timeout_s,
                    pending_task=bool(pending_task is not None and not pending_task.done()),
                    pending_page_task=_pending_page_task_state(pending_task),
                    process_tree=_process_tree_snapshot(),
                )
                return None
            polls += 1
            await asyncio.sleep(min(0.20, max(0.01, deadline - now)))

    def _register_page(self, page: Page, preferred_id: str | None = None, make_active: bool = False, source: str = "register", context_id: str | None = None) -> str:
        guid = self._page_guid(page)
        preferred_slug = self._slug(preferred_id) if preferred_id else ""
        active_before = self.active_page_id or ""
        if guid and guid in self._page_guid_to_id:
            page_id = self._page_guid_to_id[guid]
        else:
            existing = None
            for pid, known in self.pages.items():
                if known is page:
                    existing = pid
                    break
            if existing:
                page_id = existing
            elif preferred_slug:
                preferred_page = self.pages.get(preferred_slug)
                preferred_meta = self.page_meta.get(preferred_slug, {})
                if preferred_page is None or self._page_closed(preferred_page):
                    if not preferred_meta or bool(preferred_meta.get("closed")) or preferred_slug in self._page_terminal_ids:
                        page_id = preferred_slug
                    else:
                        page_id = self._next_page_id(preferred_slug)
                else:
                    page_id = self._next_page_id(preferred_slug)
            else:
                page_id = self._next_page_id(preferred_id)
        context_id = context_id or self._context_id_for_page(page)
        created = page_id not in self.pages
        self._page_terminal_ids.discard(page_id)
        self.pages[page_id] = page
        if guid:
            self._page_guid_to_id[guid] = page_id
        meta = self.page_meta.setdefault(page_id, {})
        meta.update({
            "page_id": page_id,
            "context_id": context_id,
            "guid": guid,
            "created_ms": meta.get("created_ms") or int(time.time() * 1000),
            "last_used_ms": int(time.time() * 1000),
            "closed": False,
            "source": source,
            "browser_generation": self._browser_generation,
        })
        if page_id not in self._listener_page_ids:
            self._listener_page_ids.add(page_id)
            self._attach_listeners(page, page_id)
        if make_active or not self.active_page_id:
            self.active_page_id = page_id
            self.active_page_name = page_id
        closed_at_registration = self._page_closed(page)
        try:
            page_context = page.context
        except Exception:
            page_context = None
        event = self._record_page_event(
            "page_registered",
            page_id,
            page,
            context_id=context_id,
            guid=guid,
            created=created,
            source=source,
            preferred_id=preferred_slug,
            make_active=bool(make_active),
            active_before=active_before,
            active_after=self.active_page_id or "",
            page_closed=closed_at_registration,
            page_count=len(self.pages),
            context_page_count=_context_page_count(page_context),
            active_launch_phase=self._active_launch_phase,
        )
        _camoufox_debug(
            "page_registered",
            session_id=self.session_id,
            page_id=page_id,
            context_id=context_id,
            guid=guid,
            created=created,
            active=self.active_page_id,
            active_before=active_before,
            source=source,
            make_active=bool(make_active),
            page_closed=closed_at_registration,
            page_count=len(self.pages),
            context_page_count=event.get("context_page_count", -1),
            active_launch_phase=self._active_launch_phase,
            recent_page_events=self._page_event_tail(None, 12),
        )
        return page_id

    def _schedule_page_lifecycle_log(self, event: str, page: Page | None, page_id: str, started: float | None = None, exc: Exception | None = None) -> None:
        async def emit() -> None:
            event_started = started or time.perf_counter()
            try:
                state = await self._launch_debug_snapshot(event, event_started, None, page=page, page_id=page_id, exc=exc)
                _camoufox_debug(
                    event,
                    session_id=self.session_id,
                    page_id=page_id,
                    elapsed_ms=int((time.perf_counter() - event_started) * 1000),
                    state=state,
                )
            except Exception as log_exc:
                _camoufox_debug(
                    f"{event}_log_failed",
                    session_id=self.session_id,
                    page_id=page_id,
                    error_type=type(log_exc).__name__,
                    error_len=len(str(log_exc)),
                    error_summary=_safe_text(log_exc),
                )
        try:
            asyncio.get_running_loop().create_task(emit())
        except RuntimeError as loop_exc:
            _camoufox_debug(
                f"{event}_log_unavailable",
                session_id=self.session_id,
                page_id=page_id,
                error_type=type(loop_exc).__name__,
                error_len=len(str(loop_exc)),
                error_summary=_safe_text(loop_exc),
                process_tree=_process_tree_snapshot(),
            )

    def _handle_page_close_event(self, page: Page | None, page_id: str) -> None:
        started = time.perf_counter()
        self._schedule_page_lifecycle_log("page_close_event", page, page_id, started)
        self._on_page_closed(page_id, page, "close_event")

    def _handle_page_crash_event(self, page: Page | None, page_id: str) -> None:
        self._schedule_page_lifecycle_log("page_crash_event", page, page_id)
        self._on_page_crashed(page_id, page, "crash_event")

    def _handle_page_error_event(self, exc: Any, page: Page | None, page_id: str) -> None:
        entry = self._record_page_event(
            "pageerror",
            page_id,
            page,
            exception_type=type(exc).__name__,
            exception_summary=_safe_text(exc, 900),
            process_tree=_process_tree_detailed_snapshot(),
            launch=self._last_launch_summary,
        )
        _camoufox_debug("pageerror_event", **entry)

    def _request_redirect_chain(self, req: Any) -> list[dict[str, Any]]:
        chain: list[dict[str, Any]] = []
        seen: set[int] = set()
        current = req
        while current is not None and id(current) not in seen and len(chain) < 16:
            seen.add(id(current))
            try:
                chain.append({
                    "url": _safe_text(getattr(current, "url", ""), 700),
                    "method": _safe_text(getattr(current, "method", ""), 40),
                    "resource_type": _safe_text(getattr(current, "resource_type", ""), 80),
                })
                current = getattr(current, "redirected_from", None)
                if callable(current):
                    current = current()
            except Exception:
                break
        chain.reverse()
        return chain

    def _request_timing_snapshot(self, req: Any) -> dict[str, Any]:
        try:
            timing = getattr(req, "timing", None)
            if callable(timing):
                timing = timing()
            return dict(timing or {}) if isinstance(timing, dict) else {}
        except Exception as exc:
            return {"timing_error_type": type(exc).__name__, "timing_error": _safe_text(exc, 300)}

    def _request_initiator_snapshot(self, req: Any) -> dict[str, Any]:
        try:
            frame = getattr(req, "frame", None)
            return {
                "frame_url": _safe_text(getattr(frame, "url", ""), 700) if frame is not None else "",
                "is_navigation_request": bool(req.is_navigation_request()) if hasattr(req, "is_navigation_request") else False,
            }
        except Exception as exc:
            return {"initiator_error_type": type(exc).__name__, "initiator_error": _safe_text(exc, 300)}

    def _find_network_entry_for_request(self, req: Any, page_id: str | None = None) -> dict[str, Any] | None:
        try:
            url = str(getattr(req, "url", "") or "")
            method = str(getattr(req, "method", "") or "")
            resource_type = str(getattr(req, "resource_type", "") or "")
        except Exception:
            return None
        for entry in reversed(self._network_requests):
            if page_id is not None and entry.get("page_id") not in {page_id, None}:
                continue
            if entry.get("url") == url and entry.get("method") == method and entry.get("resource_type") == resource_type:
                return entry
        return None

    async def _update_request_sizes(self, req: Any, entry: dict[str, Any]) -> None:
        try:
            sizes = req.sizes()
            if hasattr(sizes, "__await__"):
                sizes = await sizes
            if isinstance(sizes, dict):
                entry["sizes"] = dict(sizes)
                entry["request_body_length"] = int(sizes.get("requestBodySize", 0) or sizes.get("request_body_size", 0) or 0)
                entry["response_body_length"] = int(sizes.get("responseBodySize", 0) or sizes.get("response_body_size", 0) or 0)
                entry["body_length"] = entry.get("response_body_length", 0)
        except Exception as exc:
            entry["sizes_error_type"] = type(exc).__name__
            entry["sizes_error"] = _safe_text(exc, 300)

    def _handle_request_failed_event(self, req: Any, page_id: str | None = None) -> None:
        failure_text = ""
        try:
            failure = getattr(req, "failure", "")
            if callable(failure):
                failure = failure()
            failure_text = _safe_text(failure, 500)
        except Exception as exc:
            failure_text = f"<failure_probe:{type(exc).__name__}>"
        try:
            request_url = str(getattr(req, "url", "") or "")
        except Exception:
            request_url = ""
        entry = self._record_page_event(
            "requestfailed",
            page_id,
            None,
            url=request_url,
            domain=_target_domain(request_url),
            method=_safe_text(getattr(req, "method", ""), 40),
            resource_type=_safe_text(getattr(req, "resource_type", ""), 80),
            failure=failure_text,
            process_tree=_process_tree_detailed_snapshot(),
            launch=self._last_launch_summary,
        )
        if self._capturing:
            capture_entry = self._find_network_entry_for_request(req, page_id)
            if capture_entry is None:
                self._request_id_counter += 1
                capture_entry = {
                    "id": self._request_id_counter,
                    "request_id": self._request_id_counter,
                    "network_request_id": self._request_id_counter,
                    "page_id": page_id,
                    "context_id": self.page_meta.get(page_id or "", {}).get("context_id"),
                    "url": request_url,
                    "method": _safe_text(getattr(req, "method", ""), 40),
                    "resource_type": _safe_text(getattr(req, "resource_type", ""), 80),
                    "type": _safe_text(getattr(req, "resource_type", ""), 80),
                    "request_headers": dict(getattr(req, "headers", {}) or {}),
                    "timestamp": int(time.time() * 1000),
                    "timestamp_ms": int(time.time() * 1000),
                    "status": None,
                    "response_headers": None,
                    "response_body": None,
                    "duration": None,
                    "duration_ms": None,
                    "redirect_chain": self._request_redirect_chain(req),
                    "initiator": self._request_initiator_snapshot(req),
                    "timing": self._request_timing_snapshot(req),
                }
                self._network_requests.append(capture_entry)
            capture_entry["failed"] = True
            capture_entry["failure"] = failure_text
            capture_entry["error_text"] = failure_text
            capture_entry["finished_ms"] = int(time.time() * 1000)
            if capture_entry.get("timestamp"):
                capture_entry["duration"] = capture_entry["finished_ms"] - int(capture_entry.get("timestamp") or capture_entry["finished_ms"])
                capture_entry["duration_ms"] = capture_entry["duration"]
        _camoufox_debug("requestfailed_event", **entry)

    def _handle_dialog_event(self, dialog: Any, page_id: str | None = None) -> None:
        try:
            asyncio.get_running_loop().create_task(self._dismiss_dialog_event(dialog, page_id))
        except RuntimeError as exc:
            entry = self._record_page_event(
                "dialog",
                page_id,
                None,
                dialog_type=_safe_text(getattr(dialog, "type", ""), 80),
                message=_safe_text(getattr(dialog, "message", ""), 700),
                default_value=_safe_text(getattr(dialog, "default_value", ""), 300),
                dismiss_status="loop_unavailable",
                dismiss_error_type=type(exc).__name__,
                dismiss_error=_safe_text(exc, 300),
            )
            _camoufox_debug("dialog_event", **entry)

    async def _dismiss_dialog_event(self, dialog: Any, page_id: str | None = None) -> None:
        fields = {
            "dialog_type": _safe_text(getattr(dialog, "type", ""), 80),
            "message": _safe_text(getattr(dialog, "message", ""), 700),
            "default_value": _safe_text(getattr(dialog, "default_value", ""), 300),
            "dismiss_status": "pending",
        }
        try:
            await _await_no_cancel_wait(dialog.dismiss(), timeout=2.0)
            fields["dismiss_status"] = "dismissed"
        except Exception as exc:
            fields["dismiss_status"] = "failed"
            fields["dismiss_error_type"] = type(exc).__name__
            fields["dismiss_error"] = _safe_text(exc, 300)
            self._last_error = _safe_text(exc, 700)
        entry = self._record_page_event("dialog", page_id, None, **fields)
        _camoufox_debug("dialog_event", **entry)

    def _next_live_page_id(self, excluded_page_id: str) -> str | None:
        for candidate_id, candidate in list(self.pages.items()):
            if candidate_id == excluded_page_id:
                continue
            if not self._page_closed(candidate):
                return candidate_id
        return None

    def _mark_page_terminal(self, page_id: str, page: Page | None, reason: str, source: str, exc: Exception | None = None) -> bool:
        process_after = _process_tree_detailed_snapshot()
        active_operation = dict(self._active_page_operations.get(page_id, {}))
        process_before = active_operation.get("process_before") if isinstance(active_operation, dict) else None
        already_terminal = page_id in self._page_terminal_ids
        registered_before = len(self.pages)
        context_before = len(self.contexts)
        active_before = self.active_page_id or ""
        stored_page = self.pages.pop(page_id, None)
        page = page or stored_page
        terminal_url = _safe_page_url(page, 700) if page is not None else ""
        meta = self.page_meta.setdefault(page_id, {"page_id": page_id})
        if bool(meta.get("closed")) and stored_page is None:
            already_terminal = True
        now_ms = int(time.time() * 1000)
        meta["closed"] = True
        meta["closed_ms"] = meta.get("closed_ms") or now_ms
        meta["terminal_reason"] = reason
        meta["terminal_source"] = source
        meta["terminal_browser_generation"] = self._browser_generation
        if terminal_url:
            meta["terminal_url"] = terminal_url
            meta["last_url"] = terminal_url
            meta["last_url_ms"] = now_ms
        if reason == "crashed":
            meta["crashed"] = True
            meta["crashed_ms"] = meta.get("crashed_ms") or now_ms
        guid = meta.get("guid") or (self._page_guid(page) if page else "")
        if guid:
            self._page_guid_to_id.pop(str(guid), None)
        self._listener_page_ids.discard(page_id)
        self._page_terminal_ids.add(page_id)
        if self.active_page_id == page_id:
            self.active_page_id = self._next_live_page_id(page_id)
            self.active_page_name = self.active_page_id
        launch_elapsed_ms = int((time.perf_counter() - self._active_launch_started) * 1000) if self._active_launch_started > 0 else -1
        closed_during_launch = bool(
            self._active_launch_phase
            and not self._active_launch_privacy_verified
            and self._active_launch_phase not in {"", "launch_ready"}
        )
        event_name = "page_crashed" if reason == "crashed" else "page_closed"
        self._record_page_event(
            event_name,
            page_id,
            page,
            duplicate=already_terminal,
            reason=reason,
            source=source,
            active_before=active_before,
            active_after=self.active_page_id or "",
            registered_pages_before=registered_before,
            registered_pages_after=len(self.pages),
            context_count_before=context_before,
            context_count_after=len(self.contexts),
            active_launch_phase=self._active_launch_phase,
            launch_elapsed_ms=launch_elapsed_ms,
            during_launch_before_privacy=closed_during_launch,
            browser_generation=self._browser_generation,
            terminal_url_len=len(terminal_url),
            terminal_domain=_target_domain(terminal_url),
            active_operation=active_operation,
            process_tree=process_after,
            process_delta=_process_tree_delta(process_before, process_after),
            launch=self._last_launch_summary,
            crash_reports=_crash_report_snapshot(self._profile_dir or self._last_launch_summary.get("profile_dir", ""), int(active_operation.get("started_ms") or 0) if active_operation else now_ms - 60000),
        )
        _camoufox_debug(
            event_name,
            session_id=self.session_id,
            page_id=page_id,
            active=self.active_page_id or "",
            active_before=active_before,
            page_count=len(self.pages),
            registered_pages_before=registered_before,
            context_count_before=context_before,
            context_count=len(self.contexts),
            browser_open=self.browser is not None,
            browser_connected=self._browser_connected(),
            guid=str(guid or ""),
            meta_context_id=meta.get("context_id", ""),
            duplicate=already_terminal,
            reason=reason,
            source=source,
            error_type=type(exc).__name__ if exc else "",
            error_summary=_safe_text(exc, 300) if exc else "",
            active_launch_phase=self._active_launch_phase,
            launch_elapsed_ms=launch_elapsed_ms,
            during_launch_before_privacy=closed_during_launch,
            browser_generation=self._browser_generation,
            terminal_url_len=len(terminal_url),
            terminal_domain=_target_domain(terminal_url),
            active_operation=active_operation,
            process_before=process_before or {},
            process_after=process_after,
            process_delta=_process_tree_delta(process_before, process_after),
            launch=self._last_launch_summary,
            crash_reports=_crash_report_snapshot(self._profile_dir or self._last_launch_summary.get("profile_dir", ""), int(active_operation.get("started_ms") or 0) if active_operation else now_ms - 60000),
        )
        if closed_during_launch:
            self._mark_launch_terminal(
                "page_crashed" if reason == "crashed" else "page_closed",
                "page_closed_during_launch",
                page_id=page_id,
                source=source,
                terminal_reason=reason,
                active_before=active_before,
                active_after=self.active_page_id or "",
                registered_pages_before=registered_before,
                registered_pages_after=len(self.pages),
                context_count_before=context_before,
                context_count_after=len(self.contexts),
                browser_connected=self._browser_connected(),
                process_after=process_after,
            )
            _camoufox_debug(
                "page_closed_during_launch",
                session_id=self.session_id,
                page_id=page_id,
                reason=reason,
                source=source,
                active_launch_phase=self._active_launch_phase,
                launch_elapsed_ms=launch_elapsed_ms,
                active_before=active_before,
                active_after=self.active_page_id or "",
                registered_pages_before=registered_before,
                registered_pages_after=len(self.pages),
                context_count_before=context_before,
                context_count_after=len(self.contexts),
                browser_open=self.browser is not None,
                browser_connected=self._browser_connected(),
                process_after=process_after,
                recent_page_events=self._page_event_tail(None, 24),
                launch=self._last_launch_summary,
            )
        if not closed_during_launch and self.browser is not None and self._browser_connected():
            self._schedule_default_page_recovery(source)
        return not already_terminal

    def _on_page_closed(self, page_id: str, page: Page | None = None, source: str = "manual", exc: Exception | None = None) -> bool:
        return self._mark_page_terminal(page_id, page, "closed", source, exc)

    def _on_page_crashed(self, page_id: str, page: Page | None = None, source: str = "manual", exc: Exception | None = None) -> bool:
        return self._mark_page_terminal(page_id, page, "crashed", source, exc)

    def page_id_for(self, page: Page | None) -> str | None:
        if page is None:
            return None
        guid = self._page_guid(page)
        if guid and guid in self._page_guid_to_id:
            return self._page_guid_to_id[guid]
        for pid, known in self.pages.items():
            if known is page:
                return pid
        return None

    async def page_summary(self, page: Page | None = None, page_id: str | None = None) -> dict[str, Any]:
        if page is None:
            page = await self.resolve_page(page_id)
        page_id = page_id or self.page_id_for(page) or self._register_page(page, None, False, "summary")
        meta = dict(self.page_meta.get(page_id, {}))
        url = ""
        title = ""
        closed = True
        title_error = None
        try:
            closed = self._page_closed(page)
            if not closed:
                url = str(page.url or "")
                if url:
                    live_meta = self.page_meta.setdefault(page_id, {"page_id": page_id})
                    live_meta["last_url"] = url
                    live_meta["last_url_ms"] = int(time.time() * 1000)
                try:
                    title = await asyncio.wait_for(page.title(), timeout=3)
                except Exception as exc:
                    title_error = _safe_text(exc, 300)
        except Exception as exc:
            title_error = _safe_text(exc, 300)
        out = {
            "session_id": self.session_id,
            "page_id": page_id,
            "context_id": meta.get("context_id") or self._context_id_for_page(page),
            "active": page_id == self.active_page_id,
            "closed": closed,
            "url": url,
            "title": title,
            "guid": meta.get("guid") or self._page_guid(page),
            "created_ms": meta.get("created_ms"),
            "last_used_ms": meta.get("last_used_ms"),
        }
        if title_error:
            out["title_error"] = title_error
        return out

    async def page_envelope(self, page: Page | None = None, page_id: str | None = None) -> dict[str, Any]:
        if page is None:
            page = await self.resolve_page(page_id)
        pid = self.page_id_for(page) or page_id or self._register_page(page, None, False, "envelope")
        summary = await self.page_summary(page, pid)
        return {
            "session_id": self.session_id,
            "page_id": pid,
            "active_page_id": self.active_page_id,
            "page_count": len(self.pages),
            "url": summary.get("url", ""),
            "title": summary.get("title", ""),
        }

    async def list_pages(self) -> list[dict[str, Any]]:
        await self._ensure_browser()
        out = []
        for page_id, page in list(self.pages.items()):
            out.append(await self.page_summary(page, page_id))
        _camoufox_debug("pages_listed", session_id=self.session_id, active_page_id=self.active_page_id or "", page_count=len(out))
        return out

    async def new_page(self, url: str | None = None, page_id: str | None = None, make_active: bool = True, context_id: str | None = None) -> dict[str, Any]:
        await self._ensure_browser()
        requested_context_id = context_id or "default"
        ctx = self.contexts.get(requested_context_id) or await self.get_active_context()
        requested_context_id = context_id or self.context_ids.get(id(ctx), "default")
        requested_slug = self._slug(page_id) if page_id else ""
        blocked = self._requested_page_id_blocker(page_id)
        if blocked is not None:
            _camoufox_debug(
                "new_page_page_id_blocked",
                session_id=self.session_id,
                requested_context_id=requested_context_id,
                requested_page_id=requested_slug,
                error_code=blocked.get("error_code", ""),
                page_count=len(self.pages),
            )
            return blocked
        existing_pages: list[Page] = []
        started = time.perf_counter()
        try:
            existing_pages = list(ctx.pages)
        except Exception as exc:
            _camoufox_debug(
                "new_page_existing_pages_snapshot_failed",
                session_id=self.session_id,
                context_id=requested_context_id,
                error_type=type(exc).__name__,
                error_len=len(str(exc)),
            )
        _camoufox_debug(
            "new_page_begin",
            session_id=self.session_id,
            requested_context_id=requested_context_id,
            requested_page_id=page_id or "",
            make_active=bool(make_active),
            existing_pages=len(existing_pages),
            state=await self._launch_debug_snapshot("new_page_begin", started, 15000, ctx=ctx),
        )
        self._queue_pending_page_id(requested_context_id, page_id, make_active)
        page_from_browser_fallback = False
        pending_page_task: asyncio.Task | None = None
        try:
            page, pending_page_task = await _await_late_result(ctx.new_page(), timeout=15.0)
            if pending_page_task is not None:
                raise asyncio.TimeoutError
        except Exception as exc:
            page = await self._wait_for_late_page(ctx, existing_pages, 8.0 if isinstance(exc, asyncio.TimeoutError) else 3.0, "new_page", started, pending_page_task)
            if page is not None:
                _camoufox_debug(
                    "new_page_late_page_recovered",
                    session_id=self.session_id,
                    requested_context_id=requested_context_id,
                    requested_page_id=page_id or "",
                    error_type=type(exc).__name__,
                    error_len=len(str(exc)),
                    error_summary=_safe_text(exc),
                    url=_safe_page_url(page, 160),
                    state=await self._launch_debug_snapshot("new_page_late_recovered", started, 15000, ctx=ctx, page=page, exc=exc),
                )
            else:
                _abandon_late_task(pending_page_task)
                self._discard_pending_page_id(requested_context_id, page_id)
                if isinstance(self.browser, BrowserContext) or not hasattr(self.browser, "new_page") or not self._context_plan:
                    raise
                live_pages = []
                with contextlib.suppress(Exception):
                    live_pages = [candidate for candidate in list(ctx.pages) if not self._page_closed(candidate)]
                fallback_context_id = requested_context_id if not live_pages else self._slug(f"{requested_context_id}-fallback-{self._page_counter + 1}")
                _camoufox_debug(
                    "new_page_browser_fallback_begin",
                    session_id=self.session_id,
                    requested_context_id=requested_context_id,
                    fallback_context_id=fallback_context_id,
                    live_pages=len(live_pages),
                    error_type=type(exc).__name__,
                    error_len=len(str(exc)),
                    error_summary=_safe_text(exc),
                    state=await self._launch_debug_snapshot("new_page_browser_fallback", started, 15000, ctx=ctx, exc=exc),
                )
                if not live_pages:
                    with contextlib.suppress(Exception):
                        await _await_no_cancel_wait(ctx.close(), timeout=8.0)
                    self.context_ids.pop(id(ctx), None)
                fallback_ctx, page, _ = await _create_private_browser_page_context(self.browser, self._context_plan, 20.0, "new_page_browser_fallback", started)
                if self._context_plan.get("os") and self._context_plan.get("os") != detect_host_os():
                    from .utils.js_helpers import get_font_fallback_script
                    await fallback_ctx.add_init_script(get_font_fallback_script())
                for script_info in self._persistent_scripts:
                    await fallback_ctx.add_init_script(script=script_info["content"])
                self.contexts[fallback_context_id] = fallback_ctx
                self._register_context(fallback_context_id, fallback_ctx)
                requested_context_id = fallback_context_id
                page_from_browser_fallback = True
                await _await_no_cancel_wait(page.goto(PRIVACY_VERIFY_URL, wait_until="load", timeout=15000), timeout=15.0)
        await _apply_plan_page_viewport_size(page, self._context_plan, "new_page", 5.0, None, started)
        try:
            privacy_info = await _verify_page_privacy(
                page,
                self._context_plan,
                {
                    "page_reused": False,
                    "page_fresh": True,
                    "init_script_installed": bool(_privacy_override_script(self._context_plan)),
                    "persistent_context": bool(self._context_plan.get("persistent_context")) and not page_from_browser_fallback,
                    "context_source": requested_context_id,
                },
            )
            privacy_page_id = self.page_id_for(page) or (page_id or "")
            _camoufox_debug("page_privacy_verified", session_id=self.session_id, page_id=privacy_page_id, **privacy_info)
            if not privacy_info.get("webrtc_blocked") or not privacy_info.get("ice_probe_ok") or privacy_info.get("ice_candidate_leak_detected"):
                with contextlib.suppress(Exception):
                    await page.close()
                raise RuntimeError("Camoufox privacy verification failed")
            pid = self._register_page(page, page_id, make_active, "new_page", requested_context_id)
            if requested_slug and pid != requested_slug:
                self._on_page_closed(pid, page, "new_page_page_id_mismatch")
                with contextlib.suppress(Exception):
                    await _await_no_cancel_wait(page.close(), timeout=5.0)
                return {
                    "success": False,
                    "status": "blocked",
                    "error": "browser_page_id_registration_failed",
                    "error_code": "requested_page_id_not_registered",
                    "requested_page_id": requested_slug,
                    "actual_page_id": pid,
                    "active_page_id": self.active_page_id,
                    "page_count": len(self.pages),
                }
        finally:
            self._discard_pending_page_id(requested_context_id, page_id)
        if url:
            await page.goto(url, wait_until="load", timeout=30000)
        summary = await self.page_summary(page, pid)
        _camoufox_debug(
            "page_created",
            session_id=self.session_id,
            page_id=pid,
            active_page_id=self.active_page_id or "",
            url_len=len(summary.get("url", "")),
            page_count=len(self.pages),
            state=await self._launch_debug_snapshot("page_created", started, 15000, page=page, page_id=pid),
        )
        return {"status": "created", "page": summary, "page_id": pid, "active_page_id": self.active_page_id, "page_count": len(self.pages)}

    async def new_diagnostic_page(self, page_id: str, context_id: str) -> dict[str, Any]:
        started = time.perf_counter()
        await self._ensure_browser()
        requested_page_id = self._slug(page_id)
        requested_context_id = self._slug(context_id)
        if self.browser is not None and hasattr(self.browser, "new_page") and not isinstance(self.browser, BrowserContext) and self._context_plan:
            ctx, page, mode = await _create_private_browser_page_context(self.browser, self._context_plan, 20.0, "new_diagnostic_page", started)
            if self._context_plan.get("os") and self._context_plan.get("os") != detect_host_os():
                from .utils.js_helpers import get_font_fallback_script
                await ctx.add_init_script(get_font_fallback_script())
            for script_info in self._persistent_scripts:
                await ctx.add_init_script(script=script_info["content"])
            self.contexts[requested_context_id] = ctx
            self._register_context(requested_context_id, ctx)
            await _apply_plan_page_viewport_size(page, self._context_plan, "new_diagnostic_page", 5.0, None, started)
            try:
                privacy_info = await _verify_page_privacy(
                    page,
                    self._context_plan,
                    {
                        "page_reused": False,
                        "page_fresh": True,
                        "init_script_installed": bool(_privacy_override_script(self._context_plan)),
                        "persistent_context": False,
                        "context_source": requested_context_id,
                    },
                )
            except Exception as exc:
                _camoufox_debug(
                    "diagnostic_page_privacy_failed",
                    session_id=self.session_id,
                    page_id=requested_page_id,
                    context_id=requested_context_id,
                    error_type=type(exc).__name__,
                    error_summary=_safe_text(exc, 700),
                    process_tree=_process_tree_detailed_snapshot(),
                )
                with contextlib.suppress(Exception):
                    await ctx.close()
                if self.contexts.get(requested_context_id) is ctx:
                    self.contexts.pop(requested_context_id, None)
                self.context_ids.pop(id(ctx), None)
                raise
            pid = self._register_page(page, requested_page_id, False, "diagnostic_page", requested_context_id)
            _camoufox_debug(
                "diagnostic_page_created",
                session_id=self.session_id,
                page_id=pid,
                context_id=requested_context_id,
                isolated_context=True,
                mode=mode,
                privacy=privacy_info,
                elapsed_ms=int((time.perf_counter() - started) * 1000),
                state=await self._launch_debug_snapshot("diagnostic_page_created", started, 20000, ctx=ctx, page=page, page_id=pid),
            )
            return {"page": page, "page_id": pid, "context_id": requested_context_id, "isolated_context": True, "mode": mode}
        result = await self.new_page(page_id=requested_page_id, make_active=False)
        if isinstance(result, dict) and result.get("error"):
            return result
        pid = str(result.get("page_id") or requested_page_id) if isinstance(result, dict) else requested_page_id
        page = self.pages.get(pid)
        if page is None:
            return {"error": "diagnostic page was not registered", "page_id": pid}
        _camoufox_debug(
            "diagnostic_page_created",
            session_id=self.session_id,
            page_id=pid,
            context_id=self.page_meta.get(pid, {}).get("context_id", ""),
            isolated_context=False,
            mode="temporary_page",
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            state=await self._launch_debug_snapshot("diagnostic_page_created", started, 20000, page=page, page_id=pid),
        )
        return {"page": page, "page_id": pid, "context_id": self.page_meta.get(pid, {}).get("context_id", ""), "isolated_context": False, "mode": "temporary_page"}

    async def select_page(self, page_id: str) -> dict[str, Any]:
        page = await self.resolve_page(page_id)
        pid = self.page_id_for(page) or page_id
        self.active_page_id = pid
        self.active_page_name = pid
        meta = self.page_meta.setdefault(pid, {"page_id": pid})
        meta["last_used_ms"] = int(time.time() * 1000)
        summary = await self.page_summary(page, pid)
        _camoufox_debug("page_selected", session_id=self.session_id, page_id=pid, url_len=len(summary.get("url", "")), page_count=len(self.pages))
        return {"status": "selected", "page": summary, "page_id": pid, "active_page_id": self.active_page_id, "page_count": len(self.pages)}

    async def close_page(self, page_id: str) -> dict[str, Any]:
        started = time.perf_counter()
        page = await self.resolve_page(page_id)
        pid = self.page_id_for(page) or page_id
        _camoufox_debug(
            "close_page_begin",
            session_id=self.session_id,
            page_id=pid,
            state=await self._launch_debug_snapshot("close_page_begin", started, 15000, page=page, page_id=pid),
        )
        try:
            await _await_no_cancel_wait(page.close(), timeout=15.0)
        except Exception as exc:
            if _target_closed_error(exc):
                self._on_page_closed(pid, page, "close_page_target_closed", exc)
                _camoufox_debug(
                    "close_page_target_closed",
                    session_id=self.session_id,
                    page_id=pid,
                    elapsed_ms=int((time.perf_counter() - started) * 1000),
                    state=await self._launch_debug_snapshot("close_page_target_closed", started, 15000, page=page, page_id=pid, exc=exc),
                )
                return {"status": "closed", "page_id": pid, "active_page_id": self.active_page_id, "page_count": len(self.pages)}
            _camoufox_debug(
                "close_page_failed",
                session_id=self.session_id,
                page_id=pid,
                elapsed_ms=int((time.perf_counter() - started) * 1000),
                state=await self._launch_debug_snapshot("close_page_failed", started, 15000, page=page, page_id=pid, exc=exc),
            )
            raise
        self._on_page_closed(pid, page, "close_page")
        _camoufox_debug(
            "close_page_done",
            session_id=self.session_id,
            page_id=pid,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            state=await self._launch_debug_snapshot("close_page_done", started, 15000, page=page, page_id=pid),
        )
        return {"status": "closed", "page_id": pid, "active_page_id": self.active_page_id, "page_count": len(self.pages)}

    async def resolve_page(self, page_id: str | None = None) -> Page:
        await self._ensure_browser()
        pid = str(page_id or self.active_page_id or self.active_page_name or "default")
        page = self.pages.get(pid)
        if page is None and pid == "active" and self.active_page_id:
            page = self.pages.get(self.active_page_id)
            pid = self.active_page_id
        if page is None and not page_id and self.pages:
            for candidate_id, candidate in list(self.pages.items()):
                if not self._page_closed(candidate):
                    pid, page = candidate_id, candidate
                    break
        if page is None and (not page_id or pid in {"default", "active"}):
            _camoufox_debug(
                "resolve_page_default_recovery_begin",
                session_id=self.session_id,
                requested_page_id=pid,
                active_page_id=self.active_page_id or "",
                page_count=len(self.pages),
                browser_connected=self._browser_connected(),
            )
            recovery = await self.new_page(page_id="default" if pid == "default" else None, make_active=True)
            if isinstance(recovery, dict) and recovery.get("error"):
                _camoufox_debug(
                    "resolve_page_default_recovery_failed",
                    session_id=self.session_id,
                    requested_page_id=pid,
                    error_code=_safe_text(recovery.get("error_code", ""), 120),
                    error_summary=_safe_text(recovery.get("error", ""), 500),
                    page_count=len(self.pages),
                    process_tree=_process_tree_snapshot(),
                )
            else:
                recovered_id = str(recovery.get("page_id") or self.active_page_id or "") if isinstance(recovery, dict) else ""
                page = self.pages.get(recovered_id)
                pid = recovered_id or pid
                _camoufox_debug(
                    "resolve_page_default_recovered",
                    session_id=self.session_id,
                    requested_page_id=pid,
                    recovered_page_id=recovered_id,
                    active_page_id=self.active_page_id or "",
                    page_count=len(self.pages),
                )
        if page is None:
            raise RuntimeError(f"No page available for page_id={pid!r}. Call launch_browser or new_page first.")
        if self._page_closed(page):
            self._on_page_closed(pid, page, "resolve_closed")
            raise RuntimeError(f"Page is closed: {pid}")
        if not page_id:
            self.active_page_id = pid
            self.active_page_name = pid
        self.page_meta.setdefault(pid, {"page_id": pid})["last_used_ms"] = int(time.time() * 1000)
        return page

    async def resolve_page_for_operation(self, page_id: str | None = None, operation: str = "operation", restore_last_url: bool = True, aida_operation_id: Any = None) -> Page:
        try:
            return await self.resolve_page(page_id)
        except Exception as first_exc:
            requested_pid = str(page_id or self.active_page_id or self.active_page_name or "default")
            requested_slug = self._slug(requested_pid if requested_pid != "active" else (self.active_page_id or "default"))
            meta = dict(self.page_meta.get(requested_slug, {}))
            restore_url = str(meta.get("last_url") or meta.get("terminal_url") or "")
            external_id = _safe_text(aida_operation_id, 120) if aida_operation_id is not None else ""
            _camoufox_debug(
                "page_operation_recovery_begin",
                session_id=self.session_id,
                operation=_safe_text(operation, 160),
                external_operation_id=external_id,
                requested_page_id=requested_pid,
                recovery_page_id=requested_slug,
                restore_url_len=len(restore_url),
                restore_domain=_target_domain(restore_url),
                first_error_type=type(first_exc).__name__,
                first_error_kind=_page_error_kind(first_exc),
                first_error_summary=_safe_text(first_exc, 700),
                browser_open=self.browser is not None,
                browser_connected=self._browser_connected(),
                browser_generation=self._browser_generation,
                page_count=len(self.pages),
                context_count=len(self.contexts),
                process_tree=_process_tree_detailed_snapshot(),
                launch=self._last_launch_summary,
            )
            await self._ensure_browser()
            try:
                recovery = await self.new_page(page_id=requested_slug, make_active=True)
            except Exception as new_page_exc:
                if self.browser is None or not hasattr(self.browser, "new_page") or not self._context_plan:
                    _camoufox_debug(
                        "page_operation_recovery_failed",
                        session_id=self.session_id,
                        operation=_safe_text(operation, 160),
                        external_operation_id=external_id,
                        requested_page_id=requested_pid,
                        recovery_page_id=requested_slug,
                        error_type=type(new_page_exc).__name__,
                        error_kind=_page_error_kind(new_page_exc),
                        error_summary=_safe_text(new_page_exc, 700),
                        process_tree=_process_tree_detailed_snapshot(),
                    )
                    raise first_exc
                context_id = self._slug(f"recovery-{requested_slug}-{self._browser_generation}-{self._page_counter + 1}")
                ctx, page, mode = await _create_private_browser_page_context(self.browser, self._context_plan, 20.0, "page_operation_recovery_fallback", time.perf_counter())
                for script_info in self._persistent_scripts:
                    await ctx.add_init_script(script=script_info["content"])
                self.contexts[context_id] = ctx
                self._register_context(context_id, ctx)
                recovered_page_id = self._register_page(page, requested_slug, True, "navigation_recovery_fallback", context_id)
                _camoufox_debug(
                    "page_operation_recovery_fallback_created",
                    session_id=self.session_id,
                    operation=_safe_text(operation, 160),
                    external_operation_id=external_id,
                    requested_page_id=requested_pid,
                    page_id=recovered_page_id,
                    context_id=context_id,
                    mode=mode,
                    source_error_type=type(new_page_exc).__name__,
                    browser_generation=self._browser_generation,
                    state=await self._launch_debug_snapshot("navigation_page_recovery_fallback", time.perf_counter(), 20000, ctx=ctx, page=page, page_id=recovered_page_id),
                )
                return page
            if isinstance(recovery, dict) and recovery.get("error"):
                _camoufox_debug(
                    "page_operation_recovery_failed",
                    session_id=self.session_id,
                    operation=_safe_text(operation, 160),
                    external_operation_id=external_id,
                    requested_page_id=requested_pid,
                    recovery_page_id=requested_slug,
                    error_code=_safe_text(recovery.get("error_code", ""), 120),
                    error_summary=_safe_text(recovery.get("error", ""), 700),
                    process_tree=_process_tree_detailed_snapshot(),
                )
                raise first_exc
            recovered_id = str(recovery.get("page_id") or self.active_page_id or requested_slug) if isinstance(recovery, dict) else requested_slug
            page = self.pages.get(recovered_id)
            if page is None:
                raise first_exc
            restored = False
            restore_error = ""
            if restore_last_url and restore_url and restore_url != "about:blank" and not restore_url.startswith("data:"):
                try:
                    await _await_no_cancel_wait(page.goto(restore_url, wait_until="load", timeout=15000), timeout=18.0)
                    restored = True
                    live_meta = self.page_meta.setdefault(recovered_id, {"page_id": recovered_id})
                    live_meta["last_url"] = restore_url
                    live_meta["last_url_ms"] = int(time.time() * 1000)
                except Exception as restore_exc:
                    restore_error = _safe_text(restore_exc, 500)
                    _camoufox_debug(
                        "page_operation_recovery_restore_failed",
                        session_id=self.session_id,
                        operation=_safe_text(operation, 160),
                        external_operation_id=external_id,
                        requested_page_id=requested_pid,
                        page_id=recovered_id,
                        restore_url_len=len(restore_url),
                        restore_domain=_target_domain(restore_url),
                        error_type=type(restore_exc).__name__,
                        error_summary=restore_error,
                    )
            _camoufox_debug(
                "page_operation_recovery_created",
                session_id=self.session_id,
                operation=_safe_text(operation, 160),
                external_operation_id=external_id,
                requested_page_id=requested_pid,
                page_id=recovered_id,
                restored_last_url=restored,
                restore_error=restore_error,
                active_page_id=self.active_page_id or "",
                browser_generation=self._browser_generation,
                page_count=len(self.pages),
                context_count=len(self.contexts),
                state=await self._launch_debug_snapshot("navigation_page_recovery_created", time.perf_counter(), 15000, page=page, page_id=recovered_id),
            )
            return page

    async def resolve_page_for_navigation(self, page_id: str | None = None, operation: str = "navigate") -> Page:
        return await self.resolve_page_for_operation(page_id, operation, True, None)

    def _attach_listeners(self, page: Page, page_id: str | None = None) -> None:
        page_id = page_id or self.page_id_for(page) or self._register_page(page, None, False, "listener_attach")
        page.on("console", lambda msg, pid=page_id: self._on_console(msg, pid))
        page.on("request", lambda req, pid=page_id: self._on_request(req, pid))
        page.on("requestfailed", lambda req, pid=page_id: self._handle_request_failed_event(req, pid))
        page.on("requestfinished", lambda req, pid=page_id: self._handle_request_finished_event(req, pid))
        page.on("response", lambda resp, pid=page_id: self._on_response_async(resp, pid))
        page.on("response", lambda resp, pid=page_id: self._on_response_for_nav(resp, pid))
        page.on("pageerror", lambda exc, pid=page_id, p=page: self._handle_page_error_event(exc, p, pid))
        page.on("dialog", lambda dialog, pid=page_id: self._handle_dialog_event(dialog, pid))
        page.on("websocket", lambda ws, pid=page_id: self._handle_websocket_event(ws, pid))
        page.on("close", lambda *_, pid=page_id, p=page: self._handle_page_close_event(p, pid))
        with contextlib.suppress(Exception):
            page.on("crash", lambda *_, pid=page_id, p=page: self._handle_page_crash_event(p, pid))

    async def _page_bounds(self, page: Page | None) -> dict[str, Any]:
        if page is None:
            return {}
        try:
            return await page.evaluate("""() => ({
                innerWidth: window.innerWidth,
                innerHeight: window.innerHeight,
                outerWidth: window.outerWidth,
                outerHeight: window.outerHeight,
                screenX: window.screenX,
                screenY: window.screenY,
                devicePixelRatio: window.devicePixelRatio,
                screenWidth: window.screen ? window.screen.width : null,
                screenHeight: window.screen ? window.screen.height : null,
                availWidth: window.screen ? window.screen.availWidth : null,
                availHeight: window.screen ? window.screen.availHeight : null,
                availLeft: window.screen ? window.screen.availLeft : null,
                availTop: window.screen ? window.screen.availTop : null
            })""")
        except Exception as exc:
            return {"error_type": type(exc).__name__, "error_len": len(str(exc))}

    async def _page_bounds_limited(self, page: Page | None, timeout: float = 3.0) -> dict[str, Any]:
        try:
            return await _await_no_cancel_wait(self._page_bounds(page), timeout=timeout)
        except Exception as exc:
            return {"error_type": type(exc).__name__, "error_len": len(str(exc)), "timeout_sec": timeout}

    def _on_console(self, msg, page_id: str | None = None) -> None:
        text = msg.text
        if text and text.startswith("__MCP_TRACE__:"):
            try:
                import json
                payload = json.loads(text[len("__MCP_TRACE__:"):])
                path = payload.pop("__path__", "unknown")
                self._persistent_traces.setdefault(path, []).append(payload)
            except Exception:
                pass
            return

        self._console_logs.append({
            "level": msg.type,
            "text": text,
            "timestamp": int(time.time() * 1000),
            "page_id": page_id,
            "context_id": self.page_meta.get(page_id or "", {}).get("context_id"),
            "location": str(msg.location) if hasattr(msg, "location") else None,
        })
        entry = self._record_page_event(
            "console",
            page_id,
            None,
            level=_safe_text(getattr(msg, "type", ""), 80),
            text=_safe_text(text, 900),
            location=_safe_text(getattr(msg, "location", ""), 300),
        )
        _camoufox_debug("console_event", **entry)
        if str(getattr(msg, "type", "") or "").lower() == "error":
            _camoufox_debug("console_error_event", **entry)

    def _on_request(self, req, page_id: str | None = None) -> None:
        if not self._capturing:
            return
        import fnmatch
        if not fnmatch.fnmatch(req.url, self._capture_pattern):
            return
        self._request_id_counter += 1
        now_stamp = int(time.time() * 1000)
        redirect_chain = self._request_redirect_chain(req)
        redirected_from = redirect_chain[-2]["url"] if len(redirect_chain) > 1 else ""
        entry = {
            "id": self._request_id_counter,
            "request_id": self._request_id_counter,
            "network_request_id": self._request_id_counter,
            "page_id": page_id,
            "context_id": self.page_meta.get(page_id or "", {}).get("context_id"),
            "url": req.url,
            "method": req.method,
            "resource_type": req.resource_type,
            "type": req.resource_type,
            "request_headers": dict(req.headers),
            "request_post_data": req.post_data,
            "post_data": req.post_data,
            "request_body_length": len(req.post_data or ""),
            "timestamp": now_stamp,
            "timestamp_ms": now_stamp,
            "started_ms": now_stamp,
            "status": None,
            "status_code": None,
            "response_headers": None,
            "response_body": None,
            "duration": None,
            "duration_ms": None,
            "failed": False,
            "failure": "",
            "redirected_from": redirected_from,
            "redirect_chain": redirect_chain,
            "initiator": self._request_initiator_snapshot(req),
            "timing": self._request_timing_snapshot(req),
            "body_length": 0,
        }
        self._network_requests.append(entry)
        _camoufox_debug(
            "request_event",
            session_id=self.session_id,
            request_id=entry["request_id"],
            page_id=page_id or "",
            method=_safe_text(req.method, 40),
            resource_type=_safe_text(req.resource_type, 80),
            url_len=len(req.url or ""),
            domain=_target_domain(req.url),
            redirected=bool(redirected_from),
            redirect_count=max(0, len(redirect_chain) - 1),
        )

    def _on_response_async(self, resp, page_id: str | None = None) -> None:
        if not self._capturing:
            return
        for entry in reversed(self._network_requests):
            if entry["url"] == resp.url and entry["status"] is None and (page_id is None or entry.get("page_id") == page_id):
                entry["status"] = resp.status
                entry["status_code"] = resp.status
                entry["response_headers"] = dict(resp.headers)
                entry["finished_ms"] = int(time.time() * 1000)
                entry["duration"] = entry["finished_ms"] - entry["timestamp"]
                entry["duration_ms"] = entry["duration"]
                entry["timing"] = self._request_timing_snapshot(resp.request)
                if self._capture_body:
                    entry["response_body_task"] = asyncio.ensure_future(self._fetch_response_body(resp, entry))
                asyncio.ensure_future(self._update_request_sizes(resp.request, entry))
                _camoufox_debug(
                    "response_event",
                    session_id=self.session_id,
                    request_id=entry.get("request_id"),
                    page_id=entry.get("page_id") or "",
                    status=resp.status,
                    resource_type=_safe_text(entry.get("resource_type", ""), 80),
                    method=_safe_text(entry.get("method", ""), 40),
                    url_len=len(resp.url or ""),
                    domain=_target_domain(resp.url),
                    duration_ms=entry.get("duration_ms"),
                    redirect_count=max(0, len(entry.get("redirect_chain") or []) - 1),
                )
                break

    def _handle_request_finished_event(self, req: Any, page_id: str | None = None) -> None:
        if not self._capturing:
            return
        entry = self._find_network_entry_for_request(req, page_id)
        if entry is None:
            return
        now_stamp = int(time.time() * 1000)
        entry["finished"] = True
        entry["finished_ms"] = now_stamp
        if entry.get("timestamp"):
            entry["duration"] = now_stamp - int(entry.get("timestamp") or now_stamp)
            entry["duration_ms"] = entry["duration"]
        asyncio.ensure_future(self._update_request_sizes(req, entry))
        _camoufox_debug(
            "requestfinished_event",
            session_id=self.session_id,
            request_id=entry.get("request_id"),
            page_id=entry.get("page_id") or "",
            status=entry.get("status"),
            resource_type=_safe_text(entry.get("resource_type", ""), 80),
            method=_safe_text(entry.get("method", ""), 40),
            url_len=len(entry.get("url", "") or ""),
            duration_ms=entry.get("duration_ms"),
        )

    def _handle_websocket_event(self, ws: Any, page_id: str | None = None) -> None:
        url = ""
        try:
            url = str(getattr(ws, "url", "") or "")
        except Exception:
            url = ""
        request_id = 0
        if self._capturing:
            self._request_id_counter += 1
            request_id = self._request_id_counter
            now_stamp = int(time.time() * 1000)
            self._network_requests.append({
                "id": request_id,
                "request_id": request_id,
                "network_request_id": request_id,
                "page_id": page_id,
                "context_id": self.page_meta.get(page_id or "", {}).get("context_id"),
                "url": url,
                "method": "GET",
                "resource_type": "websocket",
                "type": "websocket",
                "request_headers": {},
                "response_headers": None,
                "status": None,
                "status_code": None,
                "timestamp": now_stamp,
                "timestamp_ms": now_stamp,
                "started_ms": now_stamp,
                "duration": None,
                "duration_ms": None,
                "failed": False,
                "failure": "",
                "websocket": True,
                "initiator": {"source": "playwright_websocket_event"},
                "timing": {},
                "body_length": 0,
            })
        event = self._record_page_event(
            "websocket",
            page_id,
            None,
            request_id=request_id,
            url=url,
            domain=_target_domain(url),
            resource_type="websocket",
            process_tree=_process_tree_detailed_snapshot(),
            launch=self._last_launch_summary,
        )
        _camoufox_debug("websocket_event", **event)
        with contextlib.suppress(Exception):
            ws.on("close", lambda *_, pid=page_id, rid=request_id, u=url: _camoufox_debug("websocket_close_event", session_id=self.session_id, page_id=pid or "", request_id=rid, url_len=len(u or ""), domain=_target_domain(u)))

    def _handle_service_worker_event(self, worker: Any, context_id: str | None = None) -> None:
        url = ""
        try:
            url = str(getattr(worker, "url", "") or "")
        except Exception:
            url = ""
        event = self._record_page_event(
            "serviceworker",
            "",
            None,
            context_id=context_id or "",
            url=url,
            domain=_target_domain(url),
            process_tree=_process_tree_detailed_snapshot(),
        )
        _camoufox_debug("serviceworker_event", **event)
        with contextlib.suppress(Exception):
            worker.on("close", lambda *_, cid=context_id, u=url: _camoufox_debug("serviceworker_close_event", session_id=self.session_id, context_id=cid or "", url_len=len(u or ""), domain=_target_domain(u), process_tree=_process_tree_detailed_snapshot()))

    async def _fetch_response_body(self, resp, entry: dict) -> None:
        """Asynchronously fetch and store the response body."""
        try:
            body_bytes = await resp.body()
            try:
                body_text = body_bytes.decode("utf-8")
            except UnicodeDecodeError:
                body_text = body_bytes.decode("latin-1")
            if len(body_text) > MAX_BODY_SIZE:
                entry["response_body"] = body_text[:MAX_BODY_SIZE]
                entry["response_body_truncated"] = True
                entry["response_body_total_size"] = len(body_text)
            else:
                entry["response_body"] = body_text
            entry["response_body_length"] = len(body_bytes or b"")
            entry["body_length"] = entry["response_body_length"]
        except Exception:
            entry["response_body"] = None

    def _on_response_for_nav(self, resp, page_id: str | None = None) -> None:
        try:
            entry = {
                "url": resp.url,
                "status": resp.status,
                "status_code": resp.status,
                "resource_type": getattr(resp.request, "resource_type", None) if resp.request else None,
                "method": getattr(resp.request, "method", None) if resp.request else None,
                "page_id": page_id,
                "ts": int(time.time() * 1000),
                "timestamp_ms": int(time.time() * 1000),
                "redirect_chain": self._request_redirect_chain(resp.request) if resp.request else [],
            }
            self._nav_responses.append(entry)
            if page_id:
                page_chain = self._nav_responses_by_page.setdefault(page_id, [])
                page_chain.append(entry)
                if len(page_chain) > 100:
                    self._nav_responses_by_page[page_id] = page_chain[-100:]
            if len(self._nav_responses) > 100:
                self._nav_responses = self._nav_responses[-100:]
            _camoufox_debug(
                "navigation_response_event",
                session_id=self.session_id,
                page_id=page_id or "",
                status=resp.status,
                resource_type=_safe_text(entry.get("resource_type", ""), 80),
                method=_safe_text(entry.get("method", ""), 40),
                url_len=len(resp.url or ""),
                domain=_target_domain(resp.url),
                redirect_count=max(0, len(entry.get("redirect_chain") or []) - 1),
            )
        except Exception:
            pass

    def reset_nav_responses(self, page_id: str | None = None) -> None:
        if page_id:
            self._nav_responses_by_page[page_id] = []
            self._nav_responses = [r for r in self._nav_responses if r.get("page_id") != page_id]
        else:
            self._nav_responses = []
            self._nav_responses_by_page.clear()

    def nav_responses_for_page(self, page_id: str | None = None) -> list[dict]:
        if page_id:
            return list(self._nav_responses_by_page.get(page_id, []))
        return list(self._nav_responses)

    async def create_context(self, name: str, cookies: list[dict] | None = None) -> dict:
        """Create a new isolated browser context with optional cookies."""
        started = time.perf_counter()
        await self._ensure_browser()
        mode = "isolated_context"
        if hasattr(self.browser, "new_context"):
            plan = dict(self._context_plan or {})
            if plan:
                ctx, mode = await _create_private_context(self.browser, plan, 30.0, "create_context", started)
            else:
                ctx, _, _ = await _create_camoufox_safe_context(self.browser, {}, 30.0, "create_context_empty_plan", None, started)
        else:
            ctx = await self.get_active_context()
            mode = "persistent_context_page"
        if cookies:
            await ctx.add_cookies(cookies)
        if mode == "isolated_context":
            for script_info in self._persistent_scripts:
                await ctx.add_init_script(script=script_info["content"])
        self.contexts[name] = ctx
        self._register_context(name, ctx)
        page_from_browser_fallback = False
        existing_pages: list[Page] = []
        with contextlib.suppress(Exception):
            existing_pages = list(ctx.pages)
        _camoufox_debug(
            "create_context_page_begin",
            session_id=self.session_id,
            context_id=name,
            mode=mode,
            cookies=len(cookies or []),
            existing_pages=len(existing_pages),
            state=await self._launch_debug_snapshot("create_context_page_begin", started, 15000, ctx=ctx),
        )
        pending_page_task: asyncio.Task | None = None
        try:
            page, pending_page_task = await _await_late_result(ctx.new_page(), timeout=15.0)
            if pending_page_task is not None:
                raise asyncio.TimeoutError
        except Exception as exc:
            page = await self._wait_for_late_page(ctx, existing_pages, 8.0 if isinstance(exc, asyncio.TimeoutError) else 3.0, "create_context_page", started, pending_page_task)
            if page is not None:
                _camoufox_debug(
                    "create_context_late_page_recovered",
                    session_id=self.session_id,
                    context_id=name,
                    elapsed_ms=int((time.perf_counter() - started) * 1000),
                    error_type=type(exc).__name__,
                    error_len=len(str(exc)),
                    error_summary=_safe_text(exc),
                    state=await self._launch_debug_snapshot("create_context_late_recovered", started, 15000, ctx=ctx, page=page, exc=exc),
                )
            elif isinstance(self.browser, BrowserContext) or not hasattr(self.browser, "new_page") or not self._context_plan:
                _abandon_late_task(pending_page_task)
                _camoufox_debug(
                    "create_context_page_failed",
                    session_id=self.session_id,
                    context_id=name,
                    elapsed_ms=int((time.perf_counter() - started) * 1000),
                    state=await self._launch_debug_snapshot("create_context_page_failed", started, 15000, ctx=ctx, exc=exc),
                )
                raise
            else:
                _abandon_late_task(pending_page_task)
                _camoufox_debug(
                    "create_context_browser_fallback_begin",
                    session_id=self.session_id,
                    context_id=name,
                    error_type=type(exc).__name__,
                    error_len=len(str(exc)),
                    error_summary=_safe_text(exc),
                    state=await self._launch_debug_snapshot("create_context_browser_fallback", started, 15000, ctx=ctx, exc=exc),
                )
                with contextlib.suppress(Exception):
                    await _await_no_cancel_wait(ctx.close(), timeout=8.0)
                self.context_ids.pop(id(ctx), None)
                fallback_ctx, page, mode = await _create_private_browser_page_context(self.browser, self._context_plan, 20.0, "create_context_browser_fallback", started)
                if cookies:
                    await fallback_ctx.add_cookies(cookies)
                if self._context_plan.get("os") and self._context_plan.get("os") != detect_host_os():
                    from .utils.js_helpers import get_font_fallback_script
                    await fallback_ctx.add_init_script(get_font_fallback_script())
                for script_info in self._persistent_scripts:
                    await fallback_ctx.add_init_script(script=script_info["content"])
                ctx = fallback_ctx
                self.contexts[name] = ctx
                self._register_context(name, ctx)
                page_from_browser_fallback = True
                await _await_no_cancel_wait(page.goto(PRIVACY_VERIFY_URL, wait_until="load", timeout=15000), timeout=15.0)
        await _apply_plan_page_viewport_size(page, self._context_plan, "create_context_page", 5.0, None, started)
        if self._context_plan:
            await _verify_page_privacy(
                page,
                self._context_plan,
                {
                    "page_reused": False,
                    "page_fresh": True,
                    "init_script_installed": bool(_privacy_override_script(self._context_plan)),
                    "persistent_context": mode == "persistent_context_page" and not page_from_browser_fallback,
                    "context_source": name,
                },
            )
        page_id = self._register_page(page, name, True, "create_context", name)
        _camoufox_debug(
            "create_context_page_done",
            session_id=self.session_id,
            context_id=name,
            page_id=page_id,
            mode=mode,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            state=await self._launch_debug_snapshot("create_context_page_done", started, 15000, ctx=ctx, page=page, page_id=page_id),
        )
        return {"status": "created", "context": name, "mode": mode, "page_id": page_id, "active_page_id": self.active_page_id, "page_count": len(self.pages)}

    async def get_active_context(self) -> BrowserContext:
        await self._ensure_browser()
        if self.active_page_id and self.active_page_id in self.pages:
            return self.pages[self.active_page_id].context
        if self.contexts:
            return next(iter(self.contexts.values()))
        if isinstance(self.browser, BrowserContext):
            return self.browser
        raise RuntimeError("No active context available. Call launch_browser first.")

    async def get_active_page(self) -> Page:
        """Get the currently active page, launching the browser if needed."""
        return await self.resolve_page(None)

    async def close(self) -> dict:
        """Close the browser and clean up all resources."""
        close_started = time.perf_counter()
        profile_dir = self._profile_dir
        profile_generated = self._profile_generated
        _camoufox_debug(
            "close_begin",
            browser_open=self.browser is not None,
            contexts=len(self.contexts),
            pages=len(self.pages),
            profile_dir=profile_dir or "",
            profile_generated=profile_generated,
        )
        if self._cm is not None:
            try:
                exit_started = time.perf_counter()
                await _await_no_cancel_wait(self._cm.__aexit__(None, None, None), timeout=10)
                _camoufox_debug("close_context_exit_ok", elapsed_ms=int((time.perf_counter() - exit_started) * 1000))
            except Exception as exc:
                _camoufox_debug(
                    "close_context_exit_failed",
                    elapsed_ms=int((time.perf_counter() - exit_started) * 1000),
                    error_type=type(exc).__name__,
                    error_len=len(str(exc)),
                    error_summary=_safe_text(exc),
                )
        elif self.browser is not None:
            try:
                exit_started = time.perf_counter()
                await _await_no_cancel_wait(self.browser.close(), timeout=10)
                _camoufox_debug("close_browser_close_ok", elapsed_ms=int((time.perf_counter() - exit_started) * 1000))
            except Exception as exc:
                _camoufox_debug(
                    "close_browser_close_failed",
                    elapsed_ms=int((time.perf_counter() - exit_started) * 1000),
                    error_type=type(exc).__name__,
                    error_len=len(str(exc)),
                    error_summary=_safe_text(exc),
                )
        if self._playwright is not None:
            try:
                stop_started = time.perf_counter()
                await _await_no_cancel_wait(self._playwright.stop(), timeout=5)
                _camoufox_debug("close_playwright_stop_ok", elapsed_ms=int((time.perf_counter() - stop_started) * 1000))
            except Exception as exc:
                _camoufox_debug(
                    "close_playwright_stop_failed",
                    error_type=type(exc).__name__,
                    error_len=len(str(exc)),
                    error_summary=_safe_text(exc),
                )
        _terminate_descendant_processes("close")
        self._browser_generation += 1
        self.browser = None
        self.contexts.clear()
        self.pages.clear()
        self.page_meta.clear()
        self.context_ids.clear()
        self._page_guid_to_id.clear()
        self._listener_page_ids.clear()
        self._page_terminal_ids.clear()
        self._browser_lifecycle_listener_ids.clear()
        self._pending_page_ids_by_context.clear()
        self._active_page_operations.clear()
        self.active_page_name = None
        self.active_page_id = None
        self._cm = None
        self._playwright = None
        self._console_logs.clear()
        self._network_requests.clear()
        self._request_id_counter = 0
        self._capturing = False
        self._capture_body = False
        self._init_scripts.clear()
        self._persistent_scripts.clear()
        self._persistent_traces.clear()
        self._nav_responses.clear()
        self._nav_responses_by_page.clear()
        self._route_handlers.clear()
        self._profile_dir = None
        self._profile_generated = False
        self._context_plan.clear()
        if profile_dir and profile_generated:
            try:
                _shutil.rmtree(profile_dir, ignore_errors=True)
                _camoufox_debug("close_profile_removed", profile_dir=profile_dir)
            except Exception:
                _camoufox_debug("close_profile_remove_failed", profile_dir=profile_dir)
        elif profile_dir:
            _camoufox_debug("close_profile_preserved", profile_dir=profile_dir)
        _camoufox_debug("close_done", elapsed_ms=int((time.perf_counter() - close_started) * 1000))
        return {"status": "closed"}
