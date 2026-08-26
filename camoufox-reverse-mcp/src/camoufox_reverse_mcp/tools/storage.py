from __future__ import annotations

import asyncio
import hashlib
import json
import os
import time
from typing import Any

from ..browser import _apply_plan_page_viewport_size, _await_no_cancel_wait, _camoufox_debug, _create_camoufox_safe_context, _create_private_context, _safe_text, _verify_page_privacy
from ..server import mcp, browser_manager


async def _storage_unavailable(page, storage_type: str, exc: Exception) -> dict:
    return {
        "storage_type": storage_type,
        "data": {},
        "count": 0,
        "available": False,
        "url": getattr(page, "url", ""),
        "warning": str(exc),
    }


_STORAGE_SENSITIVE_PARTS = (
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


def _storage_payload_value(payload: dict | None, name: str, current: Any = None) -> Any:
    if isinstance(payload, dict) and name in payload:
        return payload.get(name)
    return current


def _storage_type_name(value: Any) -> str:
    text = str(value or "local").strip().lower()
    if text in {"local", "localstorage", "local_storage"}:
        return "local"
    if text in {"session", "sessionstorage", "session_storage"}:
        return "session"
    raise ValueError("storage_type must be local or session")


def _storage_hash_text(value: Any) -> str:
    text = str(value if value is not None else "")
    return hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()


def _storage_sensitive_key(name: Any) -> bool:
    low = str(name or "").strip().lower().replace("_", "-")
    return any(part in low for part in _STORAGE_SENSITIVE_PARTS)


def _storage_value_summary(value: Any, key: Any = "") -> dict[str, Any]:
    text = str(value if value is not None else "")
    out: dict[str, Any] = {
        "type": type(value).__name__,
        "length": len(text),
        "sha256": _storage_hash_text(text),
    }
    if _storage_sensitive_key(key):
        out["redacted"] = True
    else:
        out["preview"] = text[:240]
        out["truncated"] = len(text) > 240
    return out


def _storage_snapshot_summary(data: Any) -> dict[str, dict[str, Any]]:
    if not isinstance(data, dict):
        return {}
    return {str(key): _storage_value_summary(value, key) for key, value in data.items()}


def _storage_bounded_int(value: Any, fallback: int, minimum: int, maximum: int) -> int:
    try:
        parsed = int(value)
    except Exception:
        return fallback
    return max(minimum, min(maximum, parsed))


async def _storage_read_map(page, storage_type: str, timeout_ms: int = 5000) -> dict[str, str]:
    data = await _await_no_cancel_wait(page.evaluate("""args => {
        const store = args.storageType === 'session' ? sessionStorage : localStorage;
        const out = {};
        for (let i = 0; i < store.length; i++) {
            const key = store.key(i);
            out[key] = store.getItem(key);
        }
        return out;
    }""", {"storageType": storage_type}), timeout=timeout_ms / 1000.0)
    return data if isinstance(data, dict) else {}


async def _storage_set_key(page, storage_type: str, key: str, value: Any, timeout_ms: int) -> dict[str, Any]:
    if not key:
        raise ValueError("key is required")
    await _await_no_cancel_wait(page.evaluate("""args => {
        const store = args.storageType === 'session' ? sessionStorage : localStorage;
        store.setItem(String(args.key), String(args.value ?? ''));
        return true;
    }""", {"storageType": storage_type, "key": key, "value": value}), timeout=timeout_ms / 1000.0)
    return {"key": key, "value": _storage_value_summary(value, key)}


async def _storage_delete_key(page, storage_type: str, key: str, timeout_ms: int) -> dict[str, Any]:
    if not key:
        raise ValueError("key is required")
    result = await _await_no_cancel_wait(page.evaluate("""args => {
        const store = args.storageType === 'session' ? sessionStorage : localStorage;
        const existed = store.getItem(String(args.key)) !== null;
        store.removeItem(String(args.key));
        return {key: String(args.key), existed};
    }""", {"storageType": storage_type, "key": key}), timeout=timeout_ms / 1000.0)
    return result if isinstance(result, dict) else {"key": key, "existed": False}


async def _storage_monitor(page, storage_type: str, duration_ms: int, limit: int, timeout_ms: int) -> dict[str, Any]:
    monitor_id = f"aida-storage-{int(time.time() * 1000)}"
    installed = await _await_no_cancel_wait(page.evaluate("""args => {
        const id = String(args.id);
        const storageType = String(args.storageType || 'local');
        const limit = Math.max(1, Math.min(Number(args.limit || 200), 1000));
        if (window.__aida_storage_monitor && window.__aida_storage_monitor.active) {
            return {installed: false, error: 'storage_monitor_already_active'};
        }
        function hashText(value) {
            const text = String(value ?? '');
            let h = 2166136261;
            for (let i = 0; i < text.length; i++) {
                h ^= text.charCodeAt(i);
                h = Math.imul(h, 16777619) >>> 0;
            }
            return 'fnv32:' + h.toString(16).padStart(8, '0');
        }
        function summary(value) {
            const text = String(value ?? '');
            return {length: text.length, sha256: hashText(text)};
        }
        function areaName(area) {
            try {
                if (area === localStorage) return 'local';
                if (area === sessionStorage) return 'session';
            } catch(e) {}
            return '';
        }
        const state = {
            active: true,
            id,
            storageType,
            limit,
            events: [],
            originalSetItem: Storage.prototype.setItem,
            originalRemoveItem: Storage.prototype.removeItem,
            originalClear: Storage.prototype.clear
        };
        function push(event) {
            if (event.storage_type && event.storage_type !== storageType) return;
            event.ts_ms = Date.now();
            state.events.push(event);
            while (state.events.length > limit) state.events.shift();
        }
        state.listener = function(event) {
            push({
                source: 'storage_event',
                storage_type: areaName(event.storageArea),
                key: event.key,
                old_value: summary(event.oldValue),
                new_value: summary(event.newValue),
                url: String(event.url || '')
            });
        };
        Storage.prototype.setItem = function(key, value) {
            const type = areaName(this);
            const oldValue = this.getItem(String(key));
            const result = state.originalSetItem.apply(this, arguments);
            push({source: 'same_page_setItem', storage_type: type, key: String(key), old_value: summary(oldValue), new_value: summary(value)});
            return result;
        };
        Storage.prototype.removeItem = function(key) {
            const type = areaName(this);
            const oldValue = this.getItem(String(key));
            const result = state.originalRemoveItem.apply(this, arguments);
            push({source: 'same_page_removeItem', storage_type: type, key: String(key), old_value: summary(oldValue), new_value: summary(null)});
            return result;
        };
        Storage.prototype.clear = function() {
            const type = areaName(this);
            const count = this.length;
            const result = state.originalClear.apply(this, arguments);
            push({source: 'same_page_clear', storage_type: type, key: null, cleared_count: count});
            return result;
        };
        window.addEventListener('storage', state.listener);
        window.__aida_storage_monitor = state;
        return {installed: true, id, storage_type: storageType};
    }""", {"id": monitor_id, "storageType": storage_type, "limit": limit}), timeout=timeout_ms / 1000.0)
    if not isinstance(installed, dict) or not installed.get("installed"):
        return installed if isinstance(installed, dict) else {"installed": False, "error": "monitor_install_failed"}
    await asyncio.sleep(duration_ms / 1000.0)
    result = await _await_no_cancel_wait(page.evaluate("""args => {
        const state = window.__aida_storage_monitor;
        if (!state || state.id !== String(args.id)) return {status: 'missing', events: []};
        try { Storage.prototype.setItem = state.originalSetItem; } catch(e) {}
        try { Storage.prototype.removeItem = state.originalRemoveItem; } catch(e) {}
        try { Storage.prototype.clear = state.originalClear; } catch(e) {}
        try { window.removeEventListener('storage', state.listener); } catch(e) {}
        const events = Array.isArray(state.events) ? state.events.slice() : [];
        state.active = false;
        delete window.__aida_storage_monitor;
        return {status: 'observed', id: state.id, storage_type: state.storageType, events};
    }""", {"id": monitor_id}), timeout=timeout_ms / 1000.0)
    return result if isinstance(result, dict) else {"status": "observed", "events": []}


def _storage_diff(before: Any, after: dict[str, str]) -> dict[str, Any]:
    baseline = before if isinstance(before, dict) else {}
    added: dict[str, Any] = {}
    removed: dict[str, Any] = {}
    changed: dict[str, Any] = {}
    before_keys = {str(key) for key in baseline.keys()}
    after_keys = {str(key) for key in after.keys()}
    for key in sorted(after_keys - before_keys):
        added[key] = _storage_value_summary(after.get(key), key)
    for key in sorted(before_keys - after_keys):
        removed[key] = _storage_value_summary(baseline.get(key), key)
    for key in sorted(before_keys & after_keys):
        old_value = baseline.get(key)
        new_value = after.get(key)
        if old_value != new_value:
            changed[key] = {
                "before": _storage_value_summary(old_value, key),
                "after": _storage_value_summary(new_value, key),
            }
    return {
        "added": added,
        "removed": removed,
        "changed": changed,
        "added_count": len(added),
        "removed_count": len(removed),
        "changed_count": len(changed),
    }


@mcp.tool()
async def cookies(
    action: str = "get",
    domain: str | None = None,
    cookies_list: list[dict] | None = None,
    name: str | None = None,
    payload: dict | None = None,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict | list:
    """Cookie management (v0.9.0 unified).

    Replaces get_cookies / set_cookies / delete_cookies.

    Args:
        action:
          "get"   — return cookies (optionally filtered by domain)
          "set"   — set cookies (requires cookies_list: [{name, value, domain, ...}])
          "delete" — delete cookies (filter by name and/or domain; no filter = clear all)
        domain: Domain filter for "get" and "delete" (e.g. ".example.com").
        cookies_list: List of cookie dicts for "set".
        name: Cookie name filter for "delete".

    Returns:
        For "get": list of cookie dicts.
        For "set"/"delete": dict with status and count.
    """
    try:
        params = payload if isinstance(payload, dict) else {}
        cookie_action = str(params.get("action") or action or "get").strip().lower()
        if cookie_action in ("", "cookies", "list", "read"):
            cookie_action = "get"
        elif cookie_action in ("remove", "clear"):
            cookie_action = "delete"
        if domain is None and params.get("domain") is not None:
            domain = str(params.get("domain"))
        if name is None and params.get("name") is not None:
            name = str(params.get("name"))
        if cookies_list is None and isinstance(params.get("cookies_list"), list):
            cookies_list = params.get("cookies_list")

        page = await browser_manager.resolve_page_for_operation(page_id, "cookies", True, aida_operation_id)
        ctx = page.context

        if cookie_action == "get":
            all_cookies = await ctx.cookies()
            if domain:
                all_cookies = [c for c in all_cookies if domain in c.get("domain", "")]
            deduped: list[dict] = []
            seen_keys: set[tuple] = set()
            for c in all_cookies:
                if not isinstance(c, dict):
                    deduped.append(c)
                    continue
                key = (
                    str(c.get("name", "")),
                    str(c.get("domain", "")),
                    str(c.get("path", "")),
                    bool(c.get("secure", False)),
                    bool(c.get("httpOnly", False)),
                    str(c.get("sameSite", "")),
                )
                if key in seen_keys:
                    continue
                seen_keys.add(key)
                deduped.append(c)
            cookie_names = [c.get("name", "") for c in deduped if isinstance(c, dict)]
            return {
                "cookies": deduped,
                "count": len(deduped),
                "names": cookie_names,
                "domain_filter": domain or "",
                "total_unfiltered_count": len(all_cookies),
            }

        elif cookie_action == "set":
            if not cookies_list:
                return {"error": "cookies_list is required for action='set'"}
            await ctx.add_cookies(cookies_list)
            return {"status": "set", "count": len(cookies_list)}

        elif cookie_action == "delete":
            all_cookies = await ctx.cookies()
            to_keep = []
            deleted = 0
            for c in all_cookies:
                should_delete = False
                if name and c["name"] == name:
                    should_delete = True
                if domain and domain in c.get("domain", ""):
                    should_delete = True
                if not name and not domain:
                    should_delete = True
                if should_delete:
                    deleted += 1
                else:
                    to_keep.append(c)
            await ctx.clear_cookies()
            if to_keep:
                await ctx.add_cookies(to_keep)
            return {"status": "deleted", "count": deleted}

        else:
            return {"error": f"unknown action: {action}. Use get/set/delete"}
    except Exception as e:
        return {"error": str(e)}


@mcp.tool()
async def get_storage(storage_type: str = "local", page_id: str | None = None, aida_operation_id=None) -> dict:
    """Get the contents of localStorage or sessionStorage.

    Args:
        storage_type: "local" for localStorage, "session" for sessionStorage.

    Returns:
        dict with all key-value pairs in the storage.
    """
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, "get_storage", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
        _camoufox_debug(
            "get_storage_begin",
            storage_type=storage_type,
            requested_page_id=page_id or "",
            page_id=resolved_page_id,
            active_page_id=browser_manager.active_page_id or "",
            external_operation_id=_safe_text(aida_operation_id, 120) if aida_operation_id is not None else "",
        )
        if storage_type == "local":
            data = await page.evaluate("""() => {
                const obj = {};
                for (let i = 0; i < localStorage.length; i++) {
                    const key = localStorage.key(i);
                    obj[key] = localStorage.getItem(key);
                }
                return obj;
            }""")
        elif storage_type == "session":
            data = await page.evaluate("""() => {
                const obj = {};
                for (let i = 0; i < sessionStorage.length; i++) {
                    const key = sessionStorage.key(i);
                    obj[key] = sessionStorage.getItem(key);
                }
                return obj;
            }""")
        else:
            return {"error": f"Invalid storage_type: {storage_type}. Use 'local' or 'session'."}
        count = len(data) if isinstance(data, dict) else 0
        out = {"storage_type": storage_type, "data": data, "count": count, "available": True, "url": page.url, "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id or ""}
        _camoufox_debug(
            "get_storage_exit",
            storage_type=storage_type,
            page_id=resolved_page_id,
            count=count,
            available=True,
            url_len=len(page.url or ""),
        )
        return out
    except Exception as e:
        try:
            page = await browser_manager.resolve_page_for_operation(page_id, "get_storage_error_probe", True, aida_operation_id)
            msg = str(e).lower()
            if "operation is insecure" in msg or "securityerror" in msg or "access is denied" in msg:
                out = await _storage_unavailable(page, storage_type, e)
                out["page_id"] = browser_manager.page_id_for(page) or page_id or ""
                return out
        except Exception:
            pass
        _camoufox_debug(
            "get_storage_exit",
            storage_type=storage_type,
            requested_page_id=page_id or "",
            available=False,
            error_type=type(e).__name__,
            error_summary=_safe_text(e, 500),
        )
        return {"error": str(e)}


@mcp.tool()
async def browser_storage_ops(
    action: str = "diff",
    storage_type: str = "local",
    key: str | None = None,
    value: Any = None,
    snapshot_before: dict | None = None,
    duration: int | float | None = None,
    duration_ms: int = 3000,
    limit: int = 200,
    timeout_ms: int = 10000,
    payload: dict | None = None,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    selected_action = str(_storage_payload_value(payload, "action", action) or "diff").strip().lower()
    aliases = {
        "set": "set_key",
        "delete": "delete_key",
        "remove": "delete_key",
        "watch": "monitor",
        "compare": "diff",
    }
    selected_action = aliases.get(selected_action, selected_action)
    effective_page_id = _storage_payload_value(payload, "page_id", page_id)
    timeout = _storage_bounded_int(_storage_payload_value(payload, "timeout_ms", timeout_ms), 10000, 250, 120000)
    try:
        effective_storage_type = _storage_type_name(_storage_payload_value(payload, "storage_type", storage_type))
        page = await browser_manager.resolve_page_for_operation(effective_page_id, f"browser_storage_ops:{selected_action}", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or effective_page_id or ""
        if selected_action == "set_key":
            effective_key = str(_storage_payload_value(payload, "key", key) or "")
            result = await _storage_set_key(page, effective_storage_type, effective_key, _storage_payload_value(payload, "value", value), timeout)
            status = "set"
        elif selected_action == "delete_key":
            effective_key = str(_storage_payload_value(payload, "key", key) or "")
            result = await _storage_delete_key(page, effective_storage_type, effective_key, timeout)
            status = "deleted" if result.get("existed") else "missing"
        elif selected_action == "monitor":
            duration_value = _storage_payload_value(payload, "duration_ms")
            if duration_value is None:
                duration_seconds = _storage_payload_value(payload, "duration", duration)
                if duration_seconds is not None:
                    try:
                        duration_value = int(float(duration_seconds) * 1000.0)
                    except Exception:
                        duration_value = duration_ms
                else:
                    duration_value = duration_ms
            effective_duration = _storage_bounded_int(duration_value, 3000, 100, 30000)
            effective_limit = _storage_bounded_int(_storage_payload_value(payload, "limit", limit), 200, 1, 1000)
            result = await _storage_monitor(page, effective_storage_type, effective_duration, effective_limit, timeout)
            status = str(result.get("status") or ("observed" if result.get("events") is not None else "monitor_failed"))
        elif selected_action == "diff":
            before = _storage_payload_value(payload, "snapshot_before", snapshot_before)
            current = await _storage_read_map(page, effective_storage_type, timeout)
            result = _storage_diff(before, current)
            result["current_snapshot"] = _storage_snapshot_summary(current)
            status = "diffed"
        else:
            return {
                "success": False,
                "status": "error",
                "action": selected_action,
                "storage_type": _storage_payload_value(payload, "storage_type", storage_type),
                "error": f"unknown action: {selected_action}",
                "page_id": str(effective_page_id or ""),
                "active_page_id": browser_manager.active_page_id or "",
            }
        out: dict[str, Any] = {
            "success": status not in {"monitor_failed"},
            "status": status,
            "action": selected_action,
            "storage_type": effective_storage_type,
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
        }
        out.update(await browser_manager.page_envelope(page))
        out["page_id"] = resolved_page_id
        out.update(result)
        return out
    except Exception as exc:
        _camoufox_debug("browser_storage_ops_error", action=selected_action, page_id=str(effective_page_id or ""), error_type=type(exc).__name__, error_summary=_safe_text(exc, 700))
        return {
            "success": False,
            "status": "error",
            "action": selected_action,
            "storage_type": str(_storage_payload_value(payload, "storage_type", storage_type) or ""),
            "error": _safe_text(exc, 700),
            "page_id": str(effective_page_id or ""),
            "active_page_id": browser_manager.active_page_id or "",
        }


@mcp.tool()
async def export_state(save_path: str) -> dict:
    """Export the complete browser state (cookies + storage) to a JSON file.

    Args:
        save_path: Local file path to save the state JSON.

    Returns:
        dict with status and the save path.
    """
    try:
        page = await browser_manager.get_active_page()
        ctx = page.context
        os.makedirs(os.path.dirname(os.path.abspath(save_path)), exist_ok=True)
        await ctx.storage_state(path=save_path)
        return {"status": "exported", "path": save_path}
    except Exception as e:
        return {"error": str(e)}


@mcp.tool()
async def import_state(state_path: str) -> dict:
    """Import browser state from a JSON file by creating a new context.

    Args:
        state_path: Path to the state JSON file (exported by export_state).

    Returns:
        dict with status and the new context name.
    """
    try:
        await browser_manager._ensure_browser()
        browser = browser_manager.browser
        if hasattr(browser, "new_context"):
            plan = dict(browser_manager._context_plan or {})
            if plan:
                context_options = dict(plan.get("context_options") or {})
                context_options["storage_state"] = state_path
                plan["context_options"] = context_options
                ctx, mode = await _create_private_context(browser, plan, 30.0, "storage_import_context")
            else:
                ctx, _, _ = await _create_camoufox_safe_context(browser, {"storage_state": state_path, "service_workers": "block"}, 30.0, "storage_import_context")
                mode = "isolated_context"
            ctx_name = f"imported_{len(browser_manager.contexts)}"
            browser_manager.contexts[ctx_name] = ctx
            page = await _await_no_cancel_wait(ctx.new_page(), timeout=30.0)
            await _apply_plan_page_viewport_size(page, plan or browser_manager._context_plan, "storage_import_page", 5.0)
            if plan:
                await _verify_page_privacy(page, plan)
            browser_manager._attach_listeners(page)
            browser_manager.pages[ctx_name] = page
            browser_manager.active_page_name = ctx_name
            return {"status": "imported", "context": ctx_name, "path": state_path, "mode": mode}

        with open(state_path, "r", encoding="utf-8") as f:
            state = json.load(f)
        page = await browser_manager.get_active_page()
        ctx = page.context
        cookies_list = state.get("cookies") if isinstance(state, dict) else []
        origins = state.get("origins") if isinstance(state, dict) else []
        if not isinstance(cookies_list, list):
            cookies_list = []
        if not isinstance(origins, list):
            origins = []
        await ctx.clear_cookies()
        if cookies_list:
            await ctx.add_cookies(cookies_list)
        current_origin = ""
        warnings: list[str] = []
        try:
            current_origin = await page.evaluate("() => location.origin")
        except Exception as e:
            warnings.append(f"origin unavailable: {e}")
        applied_storage = 0
        skipped_origins: list[str] = []
        for origin_state in origins:
            if not isinstance(origin_state, dict):
                continue
            origin = str(origin_state.get("origin") or "")
            entries = origin_state.get("localStorage") or []
            if origin != current_origin:
                if origin:
                    skipped_origins.append(origin)
                continue
            if not isinstance(entries, list):
                continue
            values = {
                str(item.get("name")): str(item.get("value", ""))
                for item in entries
                if isinstance(item, dict) and item.get("name") is not None
            }
            try:
                applied_storage += int(await page.evaluate("""values => {
                    localStorage.clear();
                    for (const [k, v] of Object.entries(values)) localStorage.setItem(k, String(v));
                    return Object.keys(values).length;
                }""", values))
            except Exception as e:
                warnings.append(f"localStorage import failed for {origin}: {e}")
        ctx_name = browser_manager.active_page_name or "default"
        return {
            "status": "imported",
            "context": ctx_name,
            "path": state_path,
            "mode": "persistent_context",
            "cookies": len(cookies_list),
            "local_storage_entries": applied_storage,
            "skipped_origins": skipped_origins,
            "warnings": warnings,
        }
    except Exception as e:
        return {"error": str(e)}
