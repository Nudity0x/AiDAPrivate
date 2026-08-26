from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sys
import threading
import time
from typing import Any


AIDA_PLAYWRIGHT_PAGEERROR_PATCH_ID = "aida_playwright_pageerror_location_patch_20260620_1"

_CORE_BUNDLE_PARTS = ("driver", "package", "lib", "coreBundle.js")
_REPLACEMENTS = (
    (b"pageError.location.url", b"pageError.location?.url ?? ''"),
    (b"pageError.location.lineNumber", b"pageError.location?.lineNumber ?? 0"),
    (b"pageError.location.columnNumber", b"pageError.location?.columnNumber ?? 0"),
)
_PATCHED_MARKERS = tuple(replacement for _, replacement in _REPLACEMENTS)


def _safe_text(value: Any, limit: int = 500) -> str:
    try:
        text = str(value)
    except Exception:
        text = repr(value)
    if len(text) > limit:
        return text[:limit] + f"...({len(text)} chars)"
    return text


def _emit(event_name: str, **fields: Any) -> None:
    payload = {
        "event": event_name,
        "patch_id": AIDA_PLAYWRIGHT_PAGEERROR_PATCH_ID,
        "pid": os.getpid(),
        "tid": threading.get_ident(),
        "ts_ms": int(time.time() * 1000),
    }
    payload.update(fields)
    try:
        line = "AIDA_CAMOUFOX " + json.dumps(payload, sort_keys=True, separators=(",", ":"))
        stderr_mode = os.environ.get("AIDA_CAMOUFOX_DEBUG_STDERR", "1").strip().lower()
        if stderr_mode not in {"0", "false", "no", "off"}:
            print(line, file=sys.stderr, flush=True)
        log_path = os.environ.get("AIDA_CAMOUFOX_DEBUG_LOG", "")
        if log_path:
            with open(log_path, "a", encoding="utf-8") as handle:
                handle.write(line + "\n")
    except Exception:
        pass


def _join_core_bundle(root: Path) -> Path:
    return root.joinpath(*_CORE_BUNDLE_PARTS)


def _candidate_core_bundles() -> tuple[list[Path], dict[str, Any]]:
    paths: list[Path] = []
    details: dict[str, Any] = {}
    try:
        import playwright

        playwright_file = Path(playwright.__file__).resolve()
        details["playwright_file"] = str(playwright_file)
        paths.append(_join_core_bundle(playwright_file.parent))
    except Exception as exc:
        details["playwright_import_error_type"] = type(exc).__name__
        details["playwright_import_error"] = _safe_text(exc)

    meipass = getattr(sys, "_MEIPASS", "")
    if meipass:
        paths.append(_join_core_bundle(Path(meipass) / "playwright"))

    try:
        exe_dir = Path(sys.executable).resolve().parent
        paths.append(_join_core_bundle(exe_dir / "_internal" / "playwright"))
        paths.append(_join_core_bundle(exe_dir / "playwright"))
    except Exception as exc:
        details["executable_probe_error_type"] = type(exc).__name__
        details["executable_probe_error"] = _safe_text(exc)

    unique: list[Path] = []
    seen: set[str] = set()
    for path in paths:
        try:
            key = str(path.resolve()).lower()
        except Exception:
            key = str(path).lower()
        if key not in seen:
            seen.add(key)
            unique.append(path)
    return unique, details


def _is_core_bundle_path(path: Path) -> bool:
    parts = [part.lower() for part in path.parts]
    required = [part.lower() for part in _CORE_BUNDLE_PARTS]
    return len(parts) >= len(required) and parts[-len(required):] == required


def _short_hash(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:24]


def _patch_core_bundle(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {
        "path": str(path),
        "ok": False,
        "changed": False,
    }
    if not _is_core_bundle_path(path):
        result["status"] = "rejected_path"
        return result
    try:
        original = path.read_bytes()
    except Exception as exc:
        result.update({
            "status": "read_failed",
            "error_type": type(exc).__name__,
            "error": _safe_text(exc),
        })
        return result

    unsafe_counts = {token.decode("ascii"): original.count(token) for token, _ in _REPLACEMENTS}
    patched_counts = {marker.decode("ascii"): original.count(marker) for marker in _PATCHED_MARKERS}
    result.update({
        "size": len(original),
        "sha256_before": _short_hash(original),
        "unsafe_counts": unsafe_counts,
        "patched_counts": patched_counts,
    })

    if not any(unsafe_counts.values()):
        result["ok"] = True
        result["status"] = "already_patched" if any(patched_counts.values()) else "not_vulnerable"
        return result

    updated = original
    for token, replacement in _REPLACEMENTS:
        updated = updated.replace(token, replacement)

    remaining_counts = {token.decode("ascii"): updated.count(token) for token, _ in _REPLACEMENTS}
    if any(remaining_counts.values()):
        result.update({
            "status": "unsafe_tokens_remaining",
            "remaining_counts": remaining_counts,
        })
        return result

    try:
        path.write_bytes(updated)
    except Exception as exc:
        result.update({
            "status": "write_failed",
            "error_type": type(exc).__name__,
            "error": _safe_text(exc),
        })
        return result

    result.update({
        "ok": True,
        "changed": True,
        "status": "patched",
        "sha256_after": _short_hash(updated),
        "patched_counts_after": {marker.decode("ascii"): updated.count(marker) for marker in _PATCHED_MARKERS},
    })
    return result


def patch_playwright_pageerror() -> dict[str, Any]:
    started = time.perf_counter()
    try:
        candidates, lookup = _candidate_core_bundles()
        existing = [path for path in candidates if path.is_file()]
        results = [_patch_core_bundle(path) for path in existing]
        payload: dict[str, Any] = {
            "ok": bool(existing) and all(bool(result.get("ok")) for result in results),
            "changed": any(bool(result.get("changed")) for result in results),
            "candidate_count": len(candidates),
            "existing_count": len(existing),
            "candidates": [str(path) for path in candidates],
            "results": results,
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
            "lookup": lookup,
        }
        if not existing:
            payload["status"] = "core_bundle_missing"
        elif any(bool(result.get("changed")) for result in results):
            payload["status"] = "patched"
        elif all(result.get("status") == "already_patched" for result in results):
            payload["status"] = "already_patched"
        elif all(result.get("status") in {"already_patched", "not_vulnerable"} for result in results):
            payload["status"] = "not_vulnerable"
        else:
            payload["status"] = "failed"
        _emit("playwright_pageerror_patch", **payload)
        return {"patch_id": AIDA_PLAYWRIGHT_PAGEERROR_PATCH_ID, **payload}
    except Exception as exc:
        payload = {
            "patch_id": AIDA_PLAYWRIGHT_PAGEERROR_PATCH_ID,
            "ok": False,
            "changed": False,
            "status": "exception",
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
            "error_type": type(exc).__name__,
            "error": _safe_text(exc),
        }
        _emit("playwright_pageerror_patch_failed", **payload)
        return payload
