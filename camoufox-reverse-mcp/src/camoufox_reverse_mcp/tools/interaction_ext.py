from __future__ import annotations

import hashlib
import os
import time
from typing import Any

from ..browser import _camoufox_debug, _safe_text
from ..server import mcp, browser_manager


_MAX_SELECTOR_LEN = 2048
_MAX_KEY_LEN = 96
_SENSITIVE_PARTS = (
    "authorization",
    "bearer",
    "credential",
    "passwd",
    "password",
    "private",
    "secret",
    "session",
    "token",
    "x-api-key",
    "apikey",
    "api_key",
)


def _payload_value(payload: dict | None, name: str, current: Any = None) -> Any:
    if isinstance(payload, dict) and name in payload:
        return payload.get(name)
    return current


def _bounded_int(value: Any, fallback: int, minimum: int, maximum: int) -> int:
    try:
        parsed = int(value)
    except Exception:
        return fallback
    return max(minimum, min(maximum, parsed))


def _timeout_ms(value: Any) -> int:
    return _bounded_int(value, 30000, 250, 120000)


def _require_selector(selector: Any, name: str = "selector") -> str:
    text = str(selector or "").strip()
    if not text:
        raise ValueError(f"{name} is required")
    if len(text) > _MAX_SELECTOR_LEN:
        raise ValueError(f"{name} exceeds {_MAX_SELECTOR_LEN} characters")
    return text


def _require_key(key: Any) -> str:
    text = str(key or "").strip()
    if not text:
        raise ValueError("key is required")
    if len(text) > _MAX_KEY_LEN:
        raise ValueError(f"key exceeds {_MAX_KEY_LEN} characters")
    return text


def _hash_text(value: Any) -> str:
    text = str(value or "")
    return hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()


def _sensitive_name(name: Any) -> bool:
    low = str(name or "").strip().lower().replace("_", "-")
    return any(part in low for part in _SENSITIVE_PARTS)


def _value_summary(value: Any, selector: str = "") -> dict[str, Any]:
    text = str(value if value is not None else "")
    out: dict[str, Any] = {
        "type": type(value).__name__,
        "length": len(text),
        "sha256": _hash_text(text),
    }
    if _sensitive_name(selector):
        out["redacted"] = True
    else:
        out["preview"] = text[:160]
        out["truncated"] = len(text) > 160
    return out


def _truthy(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    return str(value or "").strip().lower() in {"1", "true", "yes", "on", "checked", "selected"}


def _normalize_values(value: Any) -> Any:
    if value is None:
        raise ValueError("values is required")
    if isinstance(value, list):
        if not value:
            raise ValueError("values must not be empty")
        return [str(item) for item in value]
    return str(value)


def _normalize_paths(paths: Any) -> list[str]:
    if isinstance(paths, str):
        candidates = [paths]
    elif isinstance(paths, list):
        candidates = [str(item) for item in paths]
    else:
        candidates = []
    out = []
    for path in candidates:
        text = str(path or "").strip()
        if text:
            out.append(text)
    if not out:
        raise ValueError("paths must not be empty")
    missing = [path for path in out if not os.path.exists(path)]
    if missing:
        raise ValueError(f"file path does not exist: {os.path.basename(missing[0])}")
    return out


def _path_summaries(paths: list[str]) -> list[dict[str, Any]]:
    out = []
    for path in paths:
        try:
            size = os.path.getsize(path)
        except Exception:
            size = None
        out.append({
            "basename": os.path.basename(path),
            "path_sha256": _hash_text(os.path.abspath(path)),
            "size": size,
        })
    return out


async def _base(action: str, page, status: str, success: bool = True) -> dict[str, Any]:
    out: dict[str, Any] = {"success": success, "status": status, "action": action}
    out.update(await browser_manager.page_envelope(page))
    return out


def _error(action: str, message: Any, page_id: str | None = None) -> dict[str, Any]:
    return {
        "success": False,
        "status": "error",
        "action": action,
        "error": _safe_text(message, 700),
        "page_id": page_id or "",
        "active_page_id": browser_manager.active_page_id or "",
    }


async def _fill_field(page, field: dict[str, Any], timeout: int) -> dict[str, Any]:
    selector = _require_selector(field.get("selector"))
    field_type = str(field.get("type") or field.get("kind") or "text").strip().lower()
    value = field.get("value")
    locator = page.locator(selector)
    started = time.perf_counter()
    if field_type in {"checkbox", "toggle"}:
        await locator.set_checked(_truthy(value), timeout=timeout)
        status = "checked" if _truthy(value) else "unchecked"
    elif field_type == "radio":
        if _truthy(value):
            await locator.check(timeout=timeout)
            status = "checked"
        else:
            status = "skipped_unchecked_radio"
    elif field_type in {"select", "select_option"}:
        selected = await locator.select_option(_normalize_values(value), timeout=timeout)
        return {
            "selector": selector,
            "type": field_type,
            "status": "selected",
            "selected": selected,
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
        }
    elif field_type in {"file", "file_upload"}:
        paths = _normalize_paths(value)
        await locator.set_input_files(paths, timeout=timeout)
        return {
            "selector": selector,
            "type": field_type,
            "status": "uploaded",
            "files": _path_summaries(paths),
            "file_count": len(paths),
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
        }
    elif field_type in {"press", "key"}:
        key = _require_key(value)
        await locator.press(key, timeout=timeout)
        return {
            "selector": selector,
            "type": field_type,
            "status": "pressed",
            "key": key,
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
        }
    else:
        await locator.fill(str(value if value is not None else ""), timeout=timeout)
        status = "filled"
    return {
        "selector": selector,
        "type": field_type,
        "status": status,
        "value": _value_summary(value, selector),
        "elapsed_ms": int((time.perf_counter() - started) * 1000),
    }


@mcp.tool()
async def browser_interaction_ext(
    action: str = "hover",
    selector: str | None = None,
    start_selector: str | None = None,
    end_selector: str | None = None,
    values: Any = None,
    fields: list[dict] | None = None,
    paths: Any = None,
    key: str | None = None,
    text: str | None = None,
    textGone: bool = False,
    text_gone: bool | None = None,
    exact: bool = False,
    timeout_ms: int = 30000,
    payload: dict | None = None,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    selected_action = str(_payload_value(payload, "action", action) or "hover").strip().lower()
    aliases = {
        "upload": "file_upload",
        "set_files": "file_upload",
        "select": "select_option",
        "press": "press_key",
        "wait_text": "wait_for_text",
    }
    selected_action = aliases.get(selected_action, selected_action)
    effective_page_id = _payload_value(payload, "page_id", page_id)
    timeout = _timeout_ms(_payload_value(payload, "timeout_ms", timeout_ms))
    try:
        page = await browser_manager.resolve_page_for_operation(effective_page_id, f"browser_interaction_ext:{selected_action}", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or effective_page_id or ""
        if selected_action == "hover":
            effective_selector = _require_selector(_payload_value(payload, "selector", selector))
            await page.locator(effective_selector).hover(timeout=timeout)
            result = {"selector": effective_selector}
            status = "hovered"
        elif selected_action == "drag":
            start = _require_selector(_payload_value(payload, "start_selector", start_selector), "start_selector")
            end = _require_selector(_payload_value(payload, "end_selector", end_selector), "end_selector")
            await page.locator(start).drag_to(page.locator(end), timeout=timeout)
            result = {"start_selector": start, "end_selector": end}
            status = "dragged"
        elif selected_action == "select_option":
            effective_selector = _require_selector(_payload_value(payload, "selector", selector))
            effective_values = _normalize_values(_payload_value(payload, "values", values))
            selected = await page.locator(effective_selector).select_option(effective_values, timeout=timeout)
            result = {"selector": effective_selector, "selected": selected, "requested_count": len(effective_values) if isinstance(effective_values, list) else 1}
            status = "selected"
        elif selected_action == "fill_form":
            effective_fields = _payload_value(payload, "fields", fields)
            if not isinstance(effective_fields, list) or not effective_fields:
                raise ValueError("fields must be a non-empty list")
            if len(effective_fields) > 128:
                raise ValueError("fields exceeds 128 entries")
            results = []
            for field in effective_fields:
                if not isinstance(field, dict):
                    raise ValueError("each field must be an object")
                results.append(await _fill_field(page, field, timeout))
            result = {"fields": results, "field_count": len(results)}
            status = "filled"
        elif selected_action == "file_upload":
            effective_selector = _require_selector(_payload_value(payload, "selector", selector))
            effective_paths = _normalize_paths(_payload_value(payload, "paths", paths))
            await page.locator(effective_selector).set_input_files(effective_paths, timeout=timeout)
            result = {"selector": effective_selector, "files": _path_summaries(effective_paths), "file_count": len(effective_paths)}
            status = "uploaded"
        elif selected_action == "press_key":
            effective_key = _require_key(_payload_value(payload, "key", key))
            effective_selector = str(_payload_value(payload, "selector", selector) or "").strip()
            if effective_selector:
                if len(effective_selector) > _MAX_SELECTOR_LEN:
                    raise ValueError(f"selector exceeds {_MAX_SELECTOR_LEN} characters")
                await page.locator(effective_selector).press(effective_key, timeout=timeout)
                result = {"selector": effective_selector, "key": effective_key}
            else:
                await page.keyboard.press(effective_key)
                result = {"key": effective_key}
            status = "pressed"
        elif selected_action == "wait_for_text":
            effective_text = str(_payload_value(payload, "text", text) or "")
            if not effective_text:
                raise ValueError("text is required")
            if len(effective_text) > 4096:
                raise ValueError("text exceeds 4096 characters")
            gone_value = _payload_value(payload, "textGone", textGone)
            gone_value = _payload_value(payload, "text_gone", text_gone if text_gone is not None else gone_value)
            gone = bool(gone_value)
            await page.get_by_text(effective_text, exact=bool(_payload_value(payload, "exact", exact))).wait_for(state="hidden" if gone else "visible", timeout=timeout)
            result = {"text": _value_summary(effective_text, "text"), "text_gone": gone}
            status = "text_gone" if gone else "text_visible"
        else:
            return _error(selected_action, f"unknown action: {selected_action}", str(effective_page_id or ""))
        out = await _base(selected_action, page, status, True)
        out.update(result)
        out["page_id"] = resolved_page_id
        out["elapsed_ms"] = int((time.perf_counter() - started) * 1000)
        return out
    except Exception as exc:
        _camoufox_debug("browser_interaction_ext_error", action=selected_action, page_id=str(effective_page_id or ""), error_type=type(exc).__name__, error_summary=_safe_text(exc, 700))
        return _error(selected_action, exc, str(effective_page_id or ""))
