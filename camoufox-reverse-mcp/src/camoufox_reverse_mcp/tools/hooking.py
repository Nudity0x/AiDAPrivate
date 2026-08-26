from __future__ import annotations

import json
import os
import hashlib

from ..browser import _camoufox_debug
from ..server import mcp, browser_manager
from ..utils.js_helpers import render_trace_template, render_persistent_trace_template


@mcp.tool()
async def hook_function(
    function_path: str,
    mode: str = "intercept",
    hook_code: str = "",
    position: str = "before",
    non_overridable: bool = False,
    persistent: bool = False,
    log_args: bool = True,
    log_return: bool = True,
    log_stack: bool = False,
    max_captures: int = 50,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    """Hook or trace a function (v0.9.0 unified).

    Replaces hook_function + trace_function.

    Args:
        function_path: Full path like "window.encrypt",
            "XMLHttpRequest.prototype.open", "JSON.stringify".
        mode:
          "intercept" — inject custom JS before/after/replace the function.
                        Requires hook_code. (was: hook_function)
          "trace"     — non-invasive trace logging args, return values,
                        and optionally call stacks. (was: trace_function)
        hook_code: JS code for "intercept" mode. Context vars:
            - arguments: original args
            - __this: the 'this' context
            - __result: return value (only in position="after")
        position: For "intercept": "before", "after", or "replace".
        non_overridable: For "intercept": use Object.defineProperty to lock.
        persistent: If True, survives page navigation.
        log_args: For "trace": record arguments (default True).
        log_return: For "trace": record return values (default True).
        log_stack: For "trace": record call stacks (default False).
        max_captures: For "trace": max calls to record (default 50).

    Returns:
        dict with status, target, mode.
    """
    if mode == "trace":
        return await _trace_function(
            function_path, persistent, log_args, log_return, log_stack, max_captures, page_id, aida_operation_id
        )
    elif mode == "intercept":
        return await _hook_function(
            function_path, hook_code, position, non_overridable, page_id, aida_operation_id
        )
    else:
        return {"error": f"unknown mode: {mode}. Use 'intercept' or 'trace'"}


async def _trace_function(
    function_path: str, persistent: bool,
    log_args: bool, log_return: bool, log_stack: bool, max_captures: int,
    page_id: str | None = None, aida_operation_id=None,
) -> dict:
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, "hook_function_trace", True, aida_operation_id)
        if persistent:
            trace_js = render_persistent_trace_template(
                function_path=function_path, max_captures=max_captures,
                log_args=log_args, log_return=log_return, log_stack=log_stack,
            )
            trace_name = f"trace:{function_path}"
            await browser_manager.add_persistent_script(trace_name, trace_js)
            await page.evaluate(trace_js)
            return {"status": "tracing", "target": function_path, "persistent": True, "page_id": browser_manager.page_id_for(page) or page_id}
        else:
            trace_js = render_trace_template(
                function_path=function_path, max_captures=max_captures,
                log_args=log_args, log_return=log_return, log_stack=log_stack,
            )
            await page.evaluate(trace_js)
            return {"status": "tracing", "target": function_path, "persistent": False, "page_id": browser_manager.page_id_for(page) or page_id}
    except Exception as e:
        return {"error": str(e)}


async def _hook_function(
    function_path: str, hook_code: str, position: str, non_overridable: bool,
    page_id: str | None = None, aida_operation_id=None,
) -> dict:
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, "hook_function", True, aida_operation_id)
        escaped_hook = hook_code.replace("\\", "\\\\").replace("`", "\\`").replace("$", "\\$")

        freeze_code = ""
        if non_overridable:
            freeze_code = """
    try {
        Object.defineProperty(parent, fn, {
            value: parent[fn], writable: false, configurable: false
        });
    } catch(e) {}"""

        if position == "before":
            js = f"""(() => {{
    const path = {repr(function_path)};
    const parts = path.split('.');
    let parent = window;
    for (let i = 0; i < parts.length - 1; i++) {{ parent = parent[parts[i]]; if(!parent) return; }}
    const fn = parts[parts.length - 1];
    const _orig = parent[fn];
    if (typeof _orig !== 'function') return;
    const wrapper = function(...args) {{
        const __this = this;
        (function() {{ {escaped_hook} }}).call(__this);
        return _orig.apply(this, args);
    }};
    wrapper.toString = function() {{ return _orig.toString(); }};
    parent[fn] = wrapper;{freeze_code}
}})();"""
        elif position == "after":
            js = f"""(() => {{
    const path = {repr(function_path)};
    const parts = path.split('.');
    let parent = window;
    for (let i = 0; i < parts.length - 1; i++) {{ parent = parent[parts[i]]; if(!parent) return; }}
    const fn = parts[parts.length - 1];
    const _orig = parent[fn];
    if (typeof _orig !== 'function') return;
    const wrapper = function(...args) {{
        const __this = this;
        const __result = _orig.apply(this, args);
        (function() {{ {escaped_hook} }}).call(__this);
        return __result;
    }};
    wrapper.toString = function() {{ return _orig.toString(); }};
    parent[fn] = wrapper;{freeze_code}
}})();"""
        elif position == "replace":
            js = f"""(() => {{
    const path = {repr(function_path)};
    const parts = path.split('.');
    let parent = window;
    for (let i = 0; i < parts.length - 1; i++) {{ parent = parent[parts[i]]; if(!parent) return; }}
    const fn = parts[parts.length - 1];
    const wrapper = function(...args) {{
        const __this = this;
        {escaped_hook}
    }};
    parent[fn] = wrapper;{freeze_code}
}})();"""
        else:
            return {"error": f"Invalid position: {position}. Use 'before', 'after', or 'replace'."}

        await page.evaluate(js)
        return {"status": "hooked", "target": function_path, "position": position,
                "non_overridable": non_overridable, "page_id": browser_manager.page_id_for(page) or page_id}
    except Exception as e:
        return {"error": str(e)}


@mcp.tool()
async def add_init_script(
    script: str,
    name: str = "",
    persistent: bool = True,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    try:
        if not isinstance(script, str) or not script.strip():
            return {"error": "script is required"}
        script_name = name.strip() if isinstance(name, str) and name.strip() else f"inline:{hashlib.sha256(script.encode('utf-8')).hexdigest()[:16]}"
        page = await browser_manager.resolve_page_for_operation(page_id, "add_init_script", True, aida_operation_id)
        if persistent:
            await browser_manager.add_persistent_script(script_name, script)
        else:
            await page.add_init_script(script=script)
        if script_name not in browser_manager._init_scripts:
            browser_manager._init_scripts.append(script_name)
        warning = None
        try:
            await page.evaluate(script)
        except Exception as e:
            warning = str(e)
        out = {
            "status": "injected",
            "name": script_name,
            "persistent": bool(persistent),
            "context_init": bool(persistent),
            "page_init": not bool(persistent),
            "page_id": browser_manager.page_id_for(page) if hasattr(browser_manager, "page_id_for") else page_id,
            "applied_to_current_page": warning is None,
            "contexts": len(browser_manager.contexts),
            "pages": len(browser_manager.pages),
        }
        if warning:
            out["warning"] = f"current page evaluate failed: {warning}"
        return out
    except Exception as e:
        return {"error": str(e)}


@mcp.tool()
async def inject_hook_preset(preset: str, persistent: bool = True, page_id: str | None = None, aida_operation_id=None) -> dict:
    """Inject a pre-built hook template for common reverse engineering tasks.

    Available presets:
        - "xhr": Hook XMLHttpRequest to log all XHR requests.
        - "fetch": Hook window.fetch to log all fetch requests.
        - "crypto": Hook btoa/atob/JSON.stringify to capture encryption I/O.
        - "websocket": Hook WebSocket to log all WS messages.
        - "debugger_bypass": Bypass anti-debugging traps.
        - "cookie": Hook document.cookie writes.
        - "runtime_probe": Full runtime probe.

    Args:
        preset: One of the above preset names.
        persistent: If True (default), survives page navigation.

    Returns:
        dict with status and the preset name.
    """
    preset_map = {
        "xhr": "xhr_hook.js",
        "fetch": "fetch_hook.js",
        "crypto": "crypto_hook.js",
        "websocket": "websocket_hook.js",
        "debugger_bypass": "debugger_trap.js",
        "cookie": "cookie_hook.js",
        "runtime_probe": "runtime_probe.js",
        "mutation_observer": "mutation_observer_hook.js",
        "event_target": "event_target_hook.js",
        "intersection_observer": "intersection_observer_hook.js",
        "resize_observer": "resize_observer_hook.js",
        "performance_observer": "performance_observer_hook.js",
        "crypto_subtle": "crypto_subtle_hook.js",
        "canvas_spoof": "canvas_spoof.js",
        "webgl_spoof": "webgl_spoof.js",
        "audio_spoof": "audio_spoof.js",
        "font_fallback": "font_fallback.js",
    }
    if preset not in preset_map:
        return {"error": f"Unknown preset: {preset}. Available: {list(preset_map.keys())}"}
    try:
        _camoufox_debug("inject_hook_preset_begin", preset=preset, persistent=bool(persistent))
        hooks_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "hooks")
        hook_file = os.path.join(hooks_dir, preset_map[preset])
        with open(hook_file, "r", encoding="utf-8") as f:
            hook_js = f.read()
        page = await browser_manager.resolve_page_for_operation(page_id, f"inject_hook_preset:{preset}", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
        if persistent:
            script_name = f"preset:{preset}"
            await browser_manager.add_persistent_script(script_name, hook_js)
            await page.evaluate(hook_js)
        else:
            await page.add_init_script(script=hook_js)
            await page.evaluate(hook_js)
        browser_manager._init_scripts.append(f"preset:{preset}")
        _camoufox_debug(
            "inject_hook_preset_exit",
            preset=preset,
            persistent=bool(persistent),
            applied_to_current_page=True,
            requested_page_id=page_id or "",
            page_id=resolved_page_id,
            page_url_len=len(page.url or ""),
        )
        return {
            "status": "injected",
            "preset": preset,
            "persistent": persistent,
            "page_init": True,
            "applied_to_current_page": True,
            "page_id": resolved_page_id,
        }
    except Exception as e:
        _camoufox_debug(
            "inject_hook_preset_exit",
            preset=preset,
            persistent=bool(persistent),
            applied_to_current_page=False,
            error_type=type(e).__name__,
            error_len=len(str(e)),
        )
        return {"error": str(e)}


@mcp.tool()
async def remove_hooks(keep_persistent: bool = False, page_id: str | None = None, aida_operation_id=None) -> dict:
    """Remove installed hooks and restore original objects in-place.

    Args:
        keep_persistent: If True, keep persistent init_scripts registered.

    Returns:
        dict with status, restored_objects, cleared counts.
    """
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, "remove_hooks", True, aida_operation_id)
        warnings: list[str] = []
        restored: list[str] = []

        uninstall_js = r"""
        (function() {
          var out = { uninstalled: [], errors: [] };
          if (typeof window.__mcp_jsvmp_uninstall === 'function') {
            try {
              var r = window.__mcp_jsvmp_uninstall();
              out.uninstalled.push({ hook: 'jsvmp_proxy',
                                     restored: (r && r.restored) || [] });
            } catch (e) { out.errors.push('jsvmp_uninstall: ' + e.message); }
          }
          if (typeof window.__mcp_transparent_uninstall === 'function') {
            try {
              var r = window.__mcp_transparent_uninstall();
              out.uninstalled.push({ hook: 'jsvmp_transparent',
                                     restored: (r && r.restored) || [] });
            } catch (e) { out.errors.push('transparent_uninstall: ' + e.message); }
          }
          return out;
        })();
        """
        try:
            in_page = await page.evaluate(uninstall_js)
            for item in (in_page.get("uninstalled") or []):
                hook = item.get("hook")
                items = item.get("restored") or []
                if items:
                    restored.extend([f"{hook}:{n}" for n in items])
                else:
                    restored.append(hook)
            for err in (in_page.get("errors") or []):
                warnings.append(f"in-page uninstall: {err}")
        except Exception as e:
            warnings.append(f"in-page uninstall eval failed: {e}")

        cleared_init = len(browser_manager._init_scripts)
        browser_manager._init_scripts.clear()
        cleared_persistent = 0
        if not keep_persistent:
            cleared_persistent = len(browser_manager._persistent_scripts)
            browser_manager._persistent_scripts.clear()

        return {
            "status": "hooks_removed",
            "restored_objects": restored,
            "cleared_init_scripts": cleared_init,
            "cleared_persistent_scripts": cleared_persistent if not keep_persistent else 0,
            "persistent_kept": keep_persistent,
            "warnings": warnings if warnings else None,
        }
    except Exception as e:
        return {"error": str(e)}


@mcp.tool()
async def get_console_logs(
    level: str | None = None,
    keyword: str | None = None,
    clear: bool = False,
    page_id: str | None = None,
    aida_operation_id=None,
) -> list[dict]:
    try:
        _ = aida_operation_id
        logs = list(browser_manager._console_logs)
        if page_id:
            target = str(page_id)
            if target == "active":
                target = browser_manager.active_page_id or target
            logs = [l for l in logs if str(l.get("page_id") or "") == target]
        if level:
            logs = [l for l in logs if l["level"] == level]
        if keyword:
            logs = [l for l in logs if keyword in (l.get("text") or "")]
        if clear:
            if page_id:
                returned_ids = {id(item) for item in logs}
                remaining = [item for item in browser_manager._console_logs if id(item) not in returned_ids]
                browser_manager._console_logs.clear()
                browser_manager._console_logs.extend(remaining)
            else:
                browser_manager._console_logs.clear()
        return logs
    except Exception as e:
        return [{"error": str(e)}]
