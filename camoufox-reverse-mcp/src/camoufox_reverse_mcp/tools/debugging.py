from __future__ import annotations

import asyncio
import hashlib
import time
import traceback

from ..browser import _await_no_cancel_wait, _camoufox_debug, _safe_text
from ..server import mcp, browser_manager


def _build_error_response(error_msg: str) -> dict:
    """Build error response with friendly hint for common failure modes (Bug 6)."""
    hint = None

    # Playwright expression mode rejects top-level statements
    if ("expected expression" in error_msg) and ("keyword" in error_msg):
        hint = (
            "Playwright page.evaluate() expects a single expression, not statements. "
            "Wrap in IIFE if you need var/let/const/function declarations: "
            "(() => { var x = 1; return x; })()"
        )
    # JSON.parse errors (pre-v1.0.1 undefined trigger, or non-serializable values)
    elif "JSON.parse" in error_msg and "unexpected character" in error_msg:
        hint = (
            "The expression likely returned a non-JSON-serializable value "
            "(undefined, Symbol, DOM node, circular reference, etc). "
            "Wrap the result in a plain object with only primitive/string/array fields: "
            "(() => ({ field: <serializable_value> }))()"
        )
    # Timeout
    elif "timeout" in error_msg.lower() or "exceeded" in error_msg.lower():
        hint = (
            "evaluate_js timed out. If your expression returns a Promise, "
            "set await_promise=True. Otherwise simplify the expression or check "
            "if page is responsive."
        )
    # Page closed
    elif "target closed" in error_msg.lower() or "page closed" in error_msg.lower():
        hint = (
            "The page is closed. Call launch_browser() + navigate() to establish a "
            "new session before running evaluate_js."
        )

    return {
        "type": "error",
        "error": error_msg,
        "hint": hint,
    }


async def _evaluate_operation_finish(state: dict, status: str, exc: Exception | None = None) -> dict:
    if not state:
        return {}
    page = state.get("page")
    page_id = state.get("page_id")
    started = float(state.get("started") or time.perf_counter())
    generation = int(state.get("generation") or 0)
    target = f"sha256:{state.get('expr_hash', '')}"
    after = {}
    try:
        after = await browser_manager.navigation_diagnostic_snapshot(
            "evaluate_js_after",
            page,
            page_id,
            target,
            generation,
            started,
            False,
            _safe_text(exc, 700) if exc else "",
        )
        browser_manager.log_navigation_diagnostic("evaluate_js_page_state_after", after)
    except Exception as snapshot_exc:
        after = {
            "snapshot_error_type": type(snapshot_exc).__name__,
            "snapshot_error": _safe_text(snapshot_exc, 500),
            "page_id": page_id or "",
        }
    if exc is not None:
        stack = _safe_text(traceback.format_exc(), 2000)
        _camoufox_debug(
            "evaluate_js_exception_detail",
            page_id=page_id or "",
            operation_id=state.get("operation_id"),
            external_operation_id=state.get("external_operation_id", ""),
            error_type=type(exc).__name__,
            error_summary=_safe_text(exc, 900),
            stack=stack,
        )
        try:
            browser_manager.handle_page_operation_exception(page_id, page, "evaluate_js", exc)
        except Exception:
            pass
    finish = browser_manager.finish_page_operation(page_id, state.get("operation_id"), status, exc)
    return {
        "operation_id": state.get("operation_id"),
        "external_operation_id": state.get("external_operation_id", ""),
        "generation": generation,
        "expression_hash": state.get("expr_hash", ""),
        "before": state.get("before", {}),
        "after": after,
        "finish": finish,
    }


@mcp.tool()
async def evaluate_js(
    expression: str,
    await_promise: bool = True,
    page_id: str | None = None,
    timeout_ms: int = 30000,
    aida_operation_id=None,
) -> dict:
    """Execute an arbitrary JavaScript expression in the page context and return the result.

    v1.0.1 fix: correctly handles undefined/null/void/Symbol return values
    without triggering JSON.parse crashes.

    Return value is aggressively cleaned (strips BOM, fixes lone surrogates,
    trims whitespace, auto-parses JSON strings). If direct evaluate fails
    with serialization error, automatically falls back to evaluate_handle.

    Args:
        expression: JavaScript expression. Must be a single expression, not
            top-level var/let/const/function declarations (Playwright limitation).
            Wrap in IIFE if needed: (() => { var x = 1; return x; })()
        await_promise: If True, awaits Promise results (default True).

    Returns:
        dict with keys:
          value       - cleaned value (parsed JSON if applicable)
          value_raw   - raw string before cleaning (only when cleaning applied)
          type        - "primitive" | "json" | "handle_fallback" | "error"
          warnings    - list of applied cleanups, if any
          hint        - (error only) friendly fix suggestion or None
    """
    import json as _json
    import re as _re

    try:
        timeout_budget_ms = max(1000, min(int(timeout_ms), 120000))
    except Exception:
        timeout_budget_ms = 30000
    loop = asyncio.get_running_loop()
    deadline = loop.time() + (timeout_budget_ms / 1000.0)

    def _remaining_timeout() -> float:
        return max(0.001, deadline - loop.time())

    async def _eval_with_budget(awaitable):
        return await _await_no_cancel_wait(awaitable, timeout=_remaining_timeout())

    def _clean_str(s: str) -> tuple[str, list[str]]:
        warns: list[str] = []
        if not isinstance(s, str):
            return s, warns
        if s.startswith("\ufeff"):
            s = s.lstrip("\ufeff")
            warns.append("stripped BOM")
        try:
            s.encode("utf-8")
        except UnicodeEncodeError:
            s = s.encode("utf-8", "replace").decode("utf-8")
            warns.append("replaced invalid unicode")
        stripped = s.strip()
        if stripped != s and stripped:
            s = stripped
            warns.append("trimmed whitespace")
        return s, warns

    def _parse_smart(s: str, warns: list[str]) -> tuple:
        if not isinstance(s, str) or not s.strip():
            return s, None
        first_char = s.lstrip()[:1]
        if first_char not in '[{"':
            return s, None
        e1_msg = ""
        try:
            return _json.loads(s), None
        except Exception as e1:
            e1_msg = str(e1)[:100]
        cleaned = _re.sub(r'[\x00-\x08\x0b\x0c\x0e-\x1f]', '', s)
        if cleaned != s:
            try:
                val = _json.loads(cleaned)
                warns.append("stripped control chars")
                return val, None
            except Exception:
                pass
        if s.startswith('"') and s.endswith('"'):
            try:
                unwrapped = _json.loads(s)
                if isinstance(unwrapped, str) and unwrapped.lstrip()[:1] in '[{"':
                    try:
                        val = _json.loads(unwrapped)
                        warns.append("unwrapped double-encoded JSON")
                        return val, None
                    except Exception:
                        pass
            except Exception:
                pass
        return s, f"all JSON parse strategies failed: {e1_msg}"

    operation_state: dict = {}
    try:
        started = time.perf_counter()
        generation = browser_manager._next_diagnostic_id()
        expr_hash = hashlib.sha256(str(expression or "").encode("utf-8", "replace")).hexdigest()[:16]
        page = await browser_manager.resolve_page_for_operation(page_id, "evaluate_js", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id
        operation_id = browser_manager.begin_page_operation(resolved_page_id, "evaluate_js", f"sha256:{expr_hash}", generation, aida_operation_id)
        before = await browser_manager.navigation_diagnostic_snapshot(
            "evaluate_js_before",
            page,
            resolved_page_id,
            f"sha256:{expr_hash}",
            generation,
            started,
            False,
        )
        browser_manager.log_navigation_diagnostic("evaluate_js_page_state_before", before)
        operation_state = {
            "started": started,
            "generation": generation,
            "page": page,
            "page_id": resolved_page_id,
            "operation_id": operation_id,
            "external_operation_id": _safe_text(aida_operation_id, 120) if aida_operation_id is not None else "",
            "expr_hash": expr_hash,
            "before": before,
        }
        try:
            # v1.0.1 fix (Bug 5): Handle undefined/null/Symbol without JSON.parse crash.
            # Previous code did JSON.parse(JSON.stringify(r)) inside JS, which throws
            # when r is undefined/Symbol (JSON.stringify returns undefined, not a string).
            # New approach: check typeof first, only JSON-roundtrip for object/array.
            if await_promise:
                raw = await _eval_with_budget(page.evaluate(f"""async () => {{
                    try {{
                        const r = await (async () => {{ return {expression}; }})();
                        const t = typeof r;
                        if (r === undefined || r === null) {{
                            return {{ result: null, type: t, is_undefined: r === undefined }};
                        }}
                        if (t === 'symbol') {{
                            return {{ result: null, type: 'symbol', symbol_desc: r.toString() }};
                        }}
                        if (t === 'object' || t === 'function') {{
                            try {{
                                return {{ result: JSON.parse(JSON.stringify(r)), type: t }};
                            }} catch(e) {{
                                return {{ result: String(r), type: t, serialization_warning: e.message }};
                            }}
                        }}
                        return {{ result: r, type: t }};
                    }} catch(e) {{
                        return {{ error: e.message, type: 'error' }};
                    }}
                }}"""))
            else:
                raw = await _eval_with_budget(page.evaluate(f"""() => {{
                    try {{
                        const r = (() => {{ return {expression}; }})();
                        const t = typeof r;
                        if (r === undefined || r === null) {{
                            return {{ result: null, type: t, is_undefined: r === undefined }};
                        }}
                        if (t === 'symbol') {{
                            return {{ result: null, type: 'symbol', symbol_desc: r.toString() }};
                        }}
                        if (t === 'object' || t === 'function') {{
                            try {{
                                return {{ result: JSON.parse(JSON.stringify(r)), type: t }};
                            }} catch(e) {{
                                return {{ result: String(r), type: t, serialization_warning: e.message }};
                            }}
                        }}
                        return {{ result: r, type: t }};
                    }} catch(e) {{
                        return {{ error: e.message, type: 'error' }};
                    }}
                }}"""))
        except Exception as e:
            msg = str(e)
            low = msg.lower()
            if any(kw in low for kw in ("unexpected", "serialize", "cloneable", "circular", "cyclic")):
                try:
                    handle = await _eval_with_budget(page.evaluate_handle(expression))
                    descr = await _eval_with_budget(handle.evaluate(
                        "obj => ({"
                        "  type: typeof obj,"
                        "  ctor: obj && obj.constructor ? obj.constructor.name : null,"
                        "  keys: obj && typeof obj === 'object' ? "
                        "        Object.keys(obj).slice(0, 40) : null,"
                        "  preview: (function(){"
                        "    try { var s = JSON.stringify(obj); "
                        "          return s ? s.substring(0, 500) : String(obj).substring(0, 500); }"
                        "    catch(e) { return String(obj).substring(0, 500); }"
                        "  })()"
                        "})"
                    ))
                    try:
                        await handle.dispose()
                    except Exception:
                        pass
                    out = {
                        "type": "handle_fallback",
                        "value": descr,
                        "warnings": [f"direct evaluate failed, used handle fallback: {msg[:200]}"],
                    }
                    out["diagnostics"] = await _evaluate_operation_finish(operation_state, "success")
                    return out
                except Exception as e2:
                    out = _build_error_response(f"both paths failed: {msg[:200]} / {e2}")
                    out["diagnostics"] = await _evaluate_operation_finish(operation_state, "exception", e2)
                    return out
            raise

        if isinstance(raw, dict) and "error" in raw:
            out = _build_error_response(raw["error"])
            out["diagnostics"] = await _evaluate_operation_finish(operation_state, "js_error")
            return out

        # ★ Bug 5 core fix: handle None (undefined/null) from JS side ★
        # The JS wrapper now explicitly returns {result: null, type: "undefined"/"object"}
        # for undefined/null values instead of crashing on JSON.parse(JSON.stringify(undefined))
        result_val = raw.get("result") if isinstance(raw, dict) else raw
        js_type = raw.get("type") if isinstance(raw, dict) else None
        warnings_list: list[str] = []

        # Check serialization warning from JS side
        ser_warn = raw.get("serialization_warning") if isinstance(raw, dict) else None
        if ser_warn:
            warnings_list.append(f"JS serialization fallback: {ser_warn}")

        # Handle None result (undefined/null/Symbol from JS)
        if result_val is None:
            is_undef = raw.get("is_undefined") if isinstance(raw, dict) else False
            symbol_desc = raw.get("symbol_desc") if isinstance(raw, dict) else None
            if symbol_desc:
                out = {
                    "type": "primitive",
                    "value": None,
                    "value_raw": symbol_desc,
                    "warnings": [
                        f"Expression returned a Symbol ({symbol_desc}). "
                        "Symbols are not JSON-serializable; value is None."
                    ],
                }
                out["diagnostics"] = await _evaluate_operation_finish(operation_state, "success")
                return out
            if is_undef or js_type == "undefined":
                out = {
                    "type": "primitive",
                    "value": None,
                    "value_raw": "undefined",
                    "warnings": [
                        "Expression returned undefined. If unintended, "
                        "wrap logic in IIFE with explicit return: "
                        "(() => { /* logic */; return <your_value>; })()"
                    ],
                }
                out["diagnostics"] = await _evaluate_operation_finish(operation_state, "success")
                return out
            out = {
                "type": "primitive",
                "value": None,
                "value_raw": None,
                "warnings": None,
            }
            out["diagnostics"] = await _evaluate_operation_finish(operation_state, "success")
            return out

        if isinstance(result_val, str):
            cleaned, w = _clean_str(result_val)
            warnings_list.extend(w)
            parsed, parse_err = _parse_smart(cleaned, warnings_list)
            if parse_err is None and parsed is not cleaned:
                out = {
                    "type": "json", "value": parsed,
                    "value_raw": result_val if warnings_list else None,
                    "warnings": warnings_list if warnings_list else None,
                }
                out["diagnostics"] = await _evaluate_operation_finish(operation_state, "success")
                return out
            if parse_err is not None:
                warnings_list.append(parse_err)
            out = {
                "type": "primitive", "value": cleaned,
                "value_raw": result_val if warnings_list else None,
                "warnings": warnings_list if warnings_list else None,
            }
            out["diagnostics"] = await _evaluate_operation_finish(operation_state, "success")
            return out

        out = {
            "type": "primitive" if not isinstance(result_val, (dict, list)) else "json",
            "value": result_val,
            "warnings": warnings_list if warnings_list else None,
        }
        out["diagnostics"] = await _evaluate_operation_finish(operation_state, "success")
        return out
    except asyncio.TimeoutError:
        out = _build_error_response(f"evaluate_js timed out after {timeout_budget_ms}ms")
        out["diagnostics"] = await _evaluate_operation_finish(operation_state, "timeout") if operation_state else {}
        return out
    except Exception as e:
        out = _build_error_response(str(e))
        out["diagnostics"] = await _evaluate_operation_finish(operation_state, "exception", e) if operation_state else {}
        return out
