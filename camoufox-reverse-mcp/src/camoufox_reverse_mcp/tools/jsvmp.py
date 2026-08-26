from __future__ import annotations

import json
import os
import asyncio
import time

from ..browser import _await_no_cancel_wait, _camoufox_debug
from ..server import mcp, browser_manager


_COMPARE_ENV_PAGE_TIMEOUT_S = 5.0
_COMPARE_ENV_TITLE_TIMEOUT_S = 0.5
_COMPARE_ENV_EVALUATE_TIMEOUT_S = 34.0
_COMPARE_ENV_CHUNK_TIMEOUT_S = 4.0
_JSVMP_ACTIVE_EVAL_TIMEOUT_S = 8.0
_JSVMP_PROOF_PAGE_TIMEOUT_S = 8.0
_JSVMP_PROOF_EVAL_TIMEOUT_S = 6.0
_JSVMP_RELOAD_TIMEOUT_S = 10.0
_JSVMP_PROOF_READ_TIMEOUT_S = 2.0


def _compare_env_requested_count(properties: list[str] | None) -> int:
    return len(properties) if isinstance(properties, list) else 0


def _compare_env_result_count(result: object) -> int:
    if not isinstance(result, dict):
        return 0
    total = 0
    for value in result.values():
        if isinstance(value, dict):
            total += len(value)
    return total


def _compare_env_phase_timeout_ms(phase: str) -> int:
    if phase == "get_active_page":
        return int(_COMPARE_ENV_PAGE_TIMEOUT_S * 1000)
    if phase == "page_title":
        return int(_COMPARE_ENV_TITLE_TIMEOUT_S * 1000)
    if phase == "evaluate":
        return int(_COMPARE_ENV_EVALUATE_TIMEOUT_S * 1000)
    return 0


def _safe_page_url_len(page) -> int:
    try:
        return len(page.url or "")
    except Exception:
        return -1


def _jsvmp_proof_expression() -> str:
    return """() => ({
        installed: !!(window.__mcp_jsvmp_installed || window.__mcp_jsvmp_transparent_installed),
        proxy_installed: !!window.__mcp_jsvmp_installed,
        transparent_installed: !!window.__mcp_jsvmp_transparent_installed,
        log_ready: Array.isArray(window.__mcp_jsvmp_log),
        log_count: Array.isArray(window.__mcp_jsvmp_log) ? window.__mcp_jsvmp_log.length : -1
    })"""


async def _read_jsvmp_proof(page) -> dict:
    proof = await _await_no_cancel_wait(
        page.evaluate(_jsvmp_proof_expression()),
        timeout=_JSVMP_PROOF_READ_TIMEOUT_S,
    )
    return proof if isinstance(proof, dict) else {"proof_type": type(proof).__name__}


async def _fresh_jsvmp_proof(page, hook_js: str, requested_mode: str, effective_mode: str, started: float) -> dict:
    proof_page = None
    proof_started = time.perf_counter()
    try:
        ctx = page.context
        proof_page = await _await_no_cancel_wait(ctx.new_page(), timeout=_JSVMP_PROOF_PAGE_TIMEOUT_S)
        await _await_no_cancel_wait(
            proof_page.goto("about:blank", wait_until="domcontentloaded", timeout=int(_JSVMP_PROOF_PAGE_TIMEOUT_S * 1000)),
            timeout=_JSVMP_PROOF_PAGE_TIMEOUT_S,
        )
        await _await_no_cancel_wait(proof_page.evaluate(hook_js), timeout=_JSVMP_PROOF_EVAL_TIMEOUT_S)
        proof = await _read_jsvmp_proof(proof_page)
        proof_ok = bool(proof.get("installed") and proof.get("log_ready"))
        _camoufox_debug(
            "hook_jsvmp_interpreter_fresh_proof",
            requested_mode=requested_mode,
            effective_mode=effective_mode,
            success=proof_ok,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            phase_elapsed_ms=int((time.perf_counter() - proof_started) * 1000),
            proof=proof,
        )
        return {
            "proof_status": "fresh_page_instrumented" if proof_ok else "fresh_page_unverified",
            "proof_ok": proof_ok,
            "proof": proof,
            "proof_elapsed_ms": int((time.perf_counter() - proof_started) * 1000),
        }
    except asyncio.TimeoutError:
        _camoufox_debug(
            "hook_jsvmp_interpreter_fresh_proof_timeout",
            requested_mode=requested_mode,
            effective_mode=effective_mode,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            timeout_ms=int(max(_JSVMP_PROOF_PAGE_TIMEOUT_S, _JSVMP_PROOF_EVAL_TIMEOUT_S) * 1000),
        )
        return {
            "proof_status": "fresh_page_timeout",
            "proof_ok": False,
            "proof_timeout_ms": int(max(_JSVMP_PROOF_PAGE_TIMEOUT_S, _JSVMP_PROOF_EVAL_TIMEOUT_S) * 1000),
        }
    except Exception as exc:
        _camoufox_debug(
            "hook_jsvmp_interpreter_fresh_proof_failed",
            requested_mode=requested_mode,
            effective_mode=effective_mode,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            error_type=type(exc).__name__,
            error_len=len(str(exc)),
        )
        return {
            "proof_status": "fresh_page_error",
            "proof_ok": False,
            "proof_error_type": type(exc).__name__,
        }
    finally:
        if proof_page is not None:
            try:
                await _await_no_cancel_wait(proof_page.close(), timeout=2.0)
            except Exception:
                pass


async def _persistent_reload_jsvmp_proof(page, requested_mode: str, effective_mode: str, started: float) -> dict:
    reload_started = time.perf_counter()
    try:
        await _await_no_cancel_wait(
            page.reload(wait_until="domcontentloaded", timeout=int(_JSVMP_RELOAD_TIMEOUT_S * 1000)),
            timeout=_JSVMP_RELOAD_TIMEOUT_S,
        )
        proof = await _read_jsvmp_proof(page)
        proof_ok = bool(proof.get("installed") and proof.get("log_ready"))
        _camoufox_debug(
            "hook_jsvmp_interpreter_reload_proof",
            requested_mode=requested_mode,
            effective_mode=effective_mode,
            success=proof_ok,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            phase_elapsed_ms=int((time.perf_counter() - reload_started) * 1000),
            page_url_len=_safe_page_url_len(page),
            proof=proof,
        )
        return {
            "reload_status": "active_page_reloaded_instrumented" if proof_ok else "active_page_reload_unverified",
            "reload_ok": proof_ok,
            "reload_proof": proof,
            "reload_elapsed_ms": int((time.perf_counter() - reload_started) * 1000),
        }
    except asyncio.TimeoutError:
        _camoufox_debug(
            "hook_jsvmp_interpreter_reload_proof_timeout",
            requested_mode=requested_mode,
            effective_mode=effective_mode,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            timeout_ms=int(_JSVMP_RELOAD_TIMEOUT_S * 1000),
            page_url_len=_safe_page_url_len(page),
        )
        return {
            "reload_status": "active_page_reload_timeout",
            "reload_ok": False,
            "reload_timeout_ms": int(_JSVMP_RELOAD_TIMEOUT_S * 1000),
        }
    except Exception as exc:
        _camoufox_debug(
            "hook_jsvmp_interpreter_reload_proof_failed",
            requested_mode=requested_mode,
            effective_mode=effective_mode,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            error_type=type(exc).__name__,
            error_len=len(str(exc)),
            page_url_len=_safe_page_url_len(page),
        )
        return {
            "reload_status": "active_page_reload_error",
            "reload_ok": False,
            "reload_error_type": type(exc).__name__,
        }


async def _evaluate_jsvmp_hook_with_proof(page, hook_js: str, requested_mode: str, effective_mode: str, persistent: bool, started: float) -> dict:
    eval_started = time.perf_counter()
    try:
        _camoufox_debug(
            "hook_jsvmp_interpreter_evaluate_begin",
            requested_mode=requested_mode,
            effective_mode=effective_mode,
            timeout_ms=int(_JSVMP_ACTIVE_EVAL_TIMEOUT_S * 1000),
            page_url_len=_safe_page_url_len(page),
        )
        await _await_no_cancel_wait(page.evaluate(hook_js), timeout=_JSVMP_ACTIVE_EVAL_TIMEOUT_S)
        readiness_started = time.perf_counter()
        readiness_ok = False
        readiness_error_type = ""
        try:
            await _await_no_cancel_wait(
                page.wait_for_function(
                    "() => (window.__mcp_jsvmp_installed === true || window.__mcp_jsvmp_transparent_installed === true) && Array.isArray(window.__mcp_jsvmp_log)",
                    timeout=2000,
                ),
                timeout=2.0,
            )
            readiness_ok = True
        except Exception as readiness_exc:
            readiness_error_type = type(readiness_exc).__name__
            _camoufox_debug(
                "hook_jsvmp_interpreter_readiness_wait_failed",
                requested_mode=requested_mode,
                effective_mode=effective_mode,
                error_type=readiness_error_type,
                error_len=len(str(readiness_exc)),
                elapsed_ms=int((time.perf_counter() - readiness_started) * 1000),
            )
        _camoufox_debug(
            "hook_jsvmp_interpreter_readiness_wait",
            requested_mode=requested_mode,
            effective_mode=effective_mode,
            readiness_ok=readiness_ok,
            readiness_error_type=readiness_error_type,
            elapsed_ms=int((time.perf_counter() - readiness_started) * 1000),
        )
        proof = await _read_jsvmp_proof(page)
        proof_ok = bool(proof.get("installed") and proof.get("log_ready"))
        _camoufox_debug(
            "hook_jsvmp_interpreter_evaluate_ok",
            requested_mode=requested_mode,
            effective_mode=effective_mode,
            elapsed_ms=int((time.perf_counter() - eval_started) * 1000),
            page_url_len=_safe_page_url_len(page),
            proof=proof,
        )
        return {
            "active_page_instrumented": proof_ok,
            "active_proof": proof,
            "active_eval_elapsed_ms": int((time.perf_counter() - eval_started) * 1000),
        }
    except asyncio.TimeoutError:
        _camoufox_debug(
            "hook_jsvmp_interpreter_evaluate_timeout",
            requested_mode=requested_mode,
            effective_mode=effective_mode,
            elapsed_ms=int((time.perf_counter() - eval_started) * 1000),
            timeout_ms=int(_JSVMP_ACTIVE_EVAL_TIMEOUT_S * 1000),
            page_url_len=_safe_page_url_len(page),
        )
        proof = await _fresh_jsvmp_proof(page, hook_js, requested_mode, effective_mode, started)
        reload_proof = {}
        if persistent:
            reload_proof = await _persistent_reload_jsvmp_proof(page, requested_mode, effective_mode, started)
        active_recovered = bool(reload_proof.get("reload_ok"))
        return {
            "active_page_instrumented": active_recovered,
            "active_eval_timeout": True,
            "active_timeout_ms": int(_JSVMP_ACTIVE_EVAL_TIMEOUT_S * 1000),
            **proof,
            **reload_proof,
        }
    except Exception as exc:
        _camoufox_debug(
            "hook_jsvmp_interpreter_evaluate_failed",
            requested_mode=requested_mode,
            effective_mode=effective_mode,
            elapsed_ms=int((time.perf_counter() - eval_started) * 1000),
            error_type=type(exc).__name__,
            error_len=len(str(exc)),
            page_url_len=_safe_page_url_len(page),
        )
        proof = await _fresh_jsvmp_proof(page, hook_js, requested_mode, effective_mode, started)
        return {
            "active_page_instrumented": False,
            "active_eval_error_type": type(exc).__name__,
            "active_eval_error": str(exc),
            **proof,
        }


async def _compare_env_eval_chunk(page, chunk_name: str, script: str, arg, timeout_s: float, started: float) -> tuple[str, dict]:
    chunk_started = time.perf_counter()
    _camoufox_debug(
        "compare_env_chunk_begin",
        chunk=chunk_name,
        timeout_ms=int(timeout_s * 1000),
        elapsed_ms=int((time.perf_counter() - started) * 1000),
    )
    result = await _await_no_cancel_wait(page.evaluate(script, arg), timeout=timeout_s)
    if not isinstance(result, dict):
        result = {"value": str(result), "type": type(result).__name__}
    _camoufox_debug(
        "compare_env_chunk_ok",
        chunk=chunk_name,
        elapsed_ms=int((time.perf_counter() - started) * 1000),
        phase_elapsed_ms=int((time.perf_counter() - chunk_started) * 1000),
        property_count=len(result),
    )
    return chunk_name, result


@mcp.tool()
async def hook_jsvmp_interpreter(
    script_url: str = "",
    persistent: bool = True,
    mode: str = "proxy",
    track_calls: bool = True,
    track_props: bool = True,
    track_reflect: bool = True,
    proxy_objects: list[str] | None = None,
    max_entries: int = 10000,
) -> dict:
    """Install a JSVMP runtime probe.

    Multi-path instrumentation for JSVMP interpreters. Wraps Reflect.get/apply,
    installs Proxies on globals (navigator, screen, etc.), intercepts timing APIs.

    LIMITATIONS: "proxy" mode is DETECTABLE by RS/AK-style signature-based anti-bot.
    For those, use instrumentation(action='install') (source-level rewrite) or
    mode='transparent' instead.

    IMPORTANT — timing for sync-loaded SDKs (e.g. webmssdk):
        JSVMP interpreters capture native references at startup via closures.
        If you install hooks AFTER the SDK has loaded, the SDK's closures
        already hold the original (un-hooked) references — your hooks will
        never fire. You MUST install hooks BEFORE navigate():
          1. launch_browser()
          2. hook_jsvmp_interpreter(mode='transparent', persistent=True)
          3. navigate("https://www.douyin.com/...")
        If already navigated, call instrumentation(action='reload') after
        installing hooks to force a page reload with hooks active.

    Args:
        script_url: Target script URL substring for stack filtering.
        persistent: Survive navigation (default True).
        mode: "proxy" (full coverage, detectable) or "transparent" (safe, lower coverage).
        track_calls, track_props, track_reflect: Only for mode="proxy".
        proxy_objects: Objects to proxy (default: navigator, screen, etc.).
        max_entries: Log buffer cap (default 10000).

    Returns:
        dict with status, mode, coverage summary.
    """
    started = time.perf_counter()
    requested_mode = mode
    _camoufox_debug(
        "hook_jsvmp_interpreter_begin",
        requested_mode=requested_mode,
        persistent=bool(persistent),
        track_calls=bool(track_calls),
        track_props=bool(track_props),
        track_reflect=bool(track_reflect),
        proxy_objects=len(proxy_objects or []),
        max_entries=int(max_entries),
    )
    try:
        hooks_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "hooks")
        page = await browser_manager.get_active_page()
        mode = (mode or "proxy").lower()
        effective_mode = "proxy" if mode == "trace" else mode

        if effective_mode == "transparent":
            hook_path = os.path.join(hooks_dir, "jsvmp_transparent_hook.js")
            if not os.path.exists(hook_path):
                return {"error": "jsvmp_transparent_hook.js not found"}
            with open(hook_path, "r", encoding="utf-8") as f:
                template = f.read()
            hook_js = (template
                .replace("{{SCRIPT_URL}}", script_url.replace('"', '\\"').replace("'", "\\'"))
                .replace("{{MAX_ENTRIES}}", str(max_entries)))
            if persistent:
                await browser_manager.add_persistent_script(
                    f"jsvmp_transparent:{script_url or 'all'}", hook_js)
            page_already_loaded = page.url and page.url != "about:blank"
            proof = await _evaluate_jsvmp_hook_with_proof(page, hook_js, requested_mode, "transparent", bool(persistent), started)
            if proof.get("active_eval_timeout") or proof.get("active_eval_error_type"):
                status = "instrumented" if proof.get("active_page_instrumented") else "degraded"
                instrumentation_status = (
                    "active_reload_instrumented" if proof.get("active_page_instrumented") else
                    ("active_timeout_fresh_proofed" if proof.get("proof_ok") else "active_timeout_unverified")
                )
                result = {
                    "status": status,
                    "mode": "transparent",
                    "requested_mode": requested_mode,
                    "effective_mode": "transparent",
                    "instrumentation_status": instrumentation_status,
                    "timeout_status": "controlled_timeout" if proof.get("active_eval_timeout") else None,
                    "warning": "Active page hook evaluation timed out" if proof.get("active_eval_timeout") else "Active page hook evaluation failed",
                    "timeout_ms": proof.get("active_timeout_ms"),
                    "persistent": persistent,
                    "active_page_instrumented": bool(proof.get("active_page_instrumented")),
                    "proof_status": proof.get("proof_status", ""),
                    "proof_ok": bool(proof.get("proof_ok")),
                    "reload_status": proof.get("reload_status", ""),
                    "reload_ok": bool(proof.get("reload_ok")),
                    "data_location": "window.__mcp_jsvmp_log",
                    "evidence": proof,
                }
                return {k: v for k, v in result.items() if v is not None}
            result = {
                "status": "instrumented", "mode": "transparent",
                "instrumentation_status": "instrumented",
                "requested_mode": requested_mode,
                "effective_mode": "transparent",
                "script_url": script_url or "(all)", "persistent": persistent,
                "data_location": "window.__mcp_jsvmp_log",
                "active_page_instrumented": bool(proof.get("active_page_instrumented")),
                "evidence": proof,
            }
            if page_already_loaded:
                result["warnings"] = [
                    "Hooks installed on already-loaded page. If the target "
                    "SDK (e.g. webmssdk) was loaded BEFORE this call, it "
                    "likely captured native references at startup (closure "
                    "capture) and won't trigger your hooks. Call "
                    "instrumentation(action='reload') or re-navigate to "
                    "force SDK re-init with hooks in place."
                ]
            return result

        elif effective_mode == "proxy":
            if proxy_objects is None:
                proxy_objects = ["navigator", "screen", "history",
                                 "localStorage", "sessionStorage", "performance"]
            with open(os.path.join(hooks_dir, "jsvmp_hook.js"), "r", encoding="utf-8") as f:
                template = f.read()
            hook_js = (template
                .replace("{{SCRIPT_URL}}", script_url.replace('"', '\\"').replace("'", "\\'"))
                .replace("{{MAX_ENTRIES}}", str(max_entries))
                .replace("{{TRACK_CALLS}}", "true" if track_calls else "false")
                .replace("{{TRACK_PROPS}}", "true" if track_props else "false")
                .replace("{{TRACK_REFLECT}}", "true" if track_reflect else "false")
                .replace("'{{PROXY_OBJECTS}}'", json.dumps(json.dumps(proxy_objects))))
            if persistent:
                await browser_manager.add_persistent_script(
                    f"jsvmp_probe:{script_url or 'all'}", hook_js)
            page_already_loaded = page.url and page.url != "about:blank"
            proof = await _evaluate_jsvmp_hook_with_proof(page, hook_js, requested_mode, "proxy", bool(persistent), started)
            if proof.get("active_eval_timeout") or proof.get("active_eval_error_type"):
                status = "instrumented" if proof.get("active_page_instrumented") else "degraded"
                instrumentation_status = (
                    "active_reload_instrumented" if proof.get("active_page_instrumented") else
                    ("active_timeout_fresh_proofed" if proof.get("proof_ok") else "active_timeout_unverified")
                )
                result = {
                    "status": status,
                    "mode": mode,
                    "requested_mode": requested_mode,
                    "effective_mode": "proxy",
                    "instrumentation_status": instrumentation_status,
                    "timeout_status": "controlled_timeout" if proof.get("active_eval_timeout") else None,
                    "warning": "Active page hook evaluation timed out" if proof.get("active_eval_timeout") else "Active page hook evaluation failed",
                    "timeout_ms": proof.get("active_timeout_ms"),
                    "persistent": persistent,
                    "active_page_instrumented": bool(proof.get("active_page_instrumented")),
                    "proof_status": proof.get("proof_status", ""),
                    "proof_ok": bool(proof.get("proof_ok")),
                    "reload_status": proof.get("reload_status", ""),
                    "reload_ok": bool(proof.get("reload_ok")),
                    "data_location": "window.__mcp_jsvmp_log",
                    "evidence": proof,
                }
                return {k: v for k, v in result.items() if v is not None}
            result = {
                "status": "instrumented", "mode": mode,
                "instrumentation_status": "instrumented",
                "requested_mode": requested_mode,
                "effective_mode": "proxy",
                "script_url": script_url or "(all)", "persistent": persistent,
                "data_location": "window.__mcp_jsvmp_log",
                "active_page_instrumented": bool(proof.get("active_page_instrumented")),
                "evidence": proof,
                "warning": "proxy mode is detectable by RS/AK-style anti-bot.",
            }
            if page_already_loaded:
                result.setdefault("warnings", [])
                if isinstance(result.get("warning"), str):
                    result["warnings"].append(result.pop("warning"))
                result["warnings"].append(
                    "Hooks installed on already-loaded page. If the target "
                    "SDK was loaded BEFORE this call, it likely captured "
                    "native references at startup (closure capture) and "
                    "won't trigger your hooks. Call "
                    "instrumentation(action='reload') to re-trigger."
                )
            return result
        else:
            return {"error": f"unknown mode '{mode}', use 'proxy', 'trace', or 'transparent'"}
    except Exception as e:
        _camoufox_debug(
            "hook_jsvmp_interpreter_exit",
            success=False,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            requested_mode=requested_mode,
            error_type=type(e).__name__,
            error_len=len(str(e)),
        )
        return {"error": str(e)}


@mcp.tool()
async def compare_env(properties: list[str] | None = None) -> dict:
    """Collect browser environment fingerprint data for comparison with Node.js/jsdom.

    Args:
        properties: Optional list of specific properties to check.
            If omitted, checks navigator, screen, canvas, WebGL, audio, timing.

    Returns:
        dict with categorized environment data and their values.
    """
    started = time.perf_counter()
    requested_property_count = _compare_env_requested_count(properties)
    timeout_phase = "entry"
    page_url_len = -1
    page_url_error = ""
    page_title_len = -1
    page_title_error = ""
    _camoufox_debug(
        "compare_env_begin",
        requested_property_count=requested_property_count,
        page_timeout_ms=int(_COMPARE_ENV_PAGE_TIMEOUT_S * 1000),
        title_timeout_ms=int(_COMPARE_ENV_TITLE_TIMEOUT_S * 1000),
        evaluate_timeout_ms=int(_COMPARE_ENV_EVALUATE_TIMEOUT_S * 1000),
    )
    try:
        timeout_phase = "get_active_page"
        page = await _await_no_cancel_wait(browser_manager.get_active_page(), timeout=_COMPARE_ENV_PAGE_TIMEOUT_S)
        timeout_phase = "page_metadata"
        try:
            page_url_len = len(page.url or "")
        except Exception as exc:
            page_url_error = type(exc).__name__
        timeout_phase = "page_title"
        try:
            page_title = await _await_no_cancel_wait(page.title(), timeout=_COMPARE_ENV_TITLE_TIMEOUT_S)
            page_title_len = len(page_title or "")
        except asyncio.TimeoutError:
            page_title_error = "TimeoutError"
        except Exception as exc:
            page_title_error = type(exc).__name__
        _camoufox_debug(
            "compare_env_page",
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            requested_property_count=requested_property_count,
            page_url_len=page_url_len,
            page_url_error=page_url_error,
            page_title_len=page_title_len,
            page_title_error=page_title_error,
        )
        timeout_phase = "evaluate"
        _camoufox_debug(
            "compare_env_evaluate_begin",
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            requested_property_count=requested_property_count,
            timeout_ms=int(_COMPARE_ENV_EVALUATE_TIMEOUT_S * 1000),
            chunk_timeout_ms=int(_COMPARE_ENV_CHUNK_TIMEOUT_S * 1000),
            page_url_len=page_url_len,
            page_title_len=page_title_len,
        )
        evaluate_started = time.perf_counter()
        result = {
            "navigator": {}, "screen": {}, "canvas": {}, "webgl": {},
            "audio": {}, "timing": {}, "misc": {}, "custom": {},
        }
        chunk_failures: list[dict] = []
        chunk_timeouts: list[dict] = []
        chunks: list[tuple[str, str, object]] = [
            ("navigator", """(props) => {
                const out = {};
                for (const p of props) {
                    try { out[p] = { value: String(navigator[p]), type: typeof navigator[p] }; }
                    catch(e) { out[p] = { value: null, error: e.message }; }
                }
                return out;
            }""", ['userAgent', 'platform', 'language', 'languages',
                'hardwareConcurrency', 'deviceMemory', 'maxTouchPoints',
                'vendor', 'cookieEnabled', 'webdriver']),
            ("screen", """(props) => {
                const out = {};
                for (const p of props) {
                    try { out[p] = { value: screen[p], type: typeof screen[p] }; }
                    catch(e) { out[p] = { value: null, error: e.message }; }
                }
                try { out.devicePixelRatio = { value: window.devicePixelRatio, type: 'number' }; }
                catch(e) { out.devicePixelRatio = { value: null, error: e.message }; }
                return out;
            }""", ['width', 'height', 'availWidth', 'availHeight', 'colorDepth']),
            ("timing", """() => {
                const out = {};
                try { out.timezoneOffset = { value: new Date().getTimezoneOffset(), type: 'number' }; }
                catch(e) { out.timezoneOffset = { value: null, error: e.message }; }
                try { out.timezone = { value: Intl.DateTimeFormat().resolvedOptions().timeZone, type: 'string' }; }
                catch(e) { out.timezone = { value: null, error: e.message }; }
                try { out.now = { value: Math.round(performance.now()), type: 'number' }; }
                catch(e) { out.now = { value: null, error: e.message }; }
                return out;
            }""", None),
        ]
        if properties:
            for i in range(0, len(properties), 8):
                chunks.append((f"custom_{i // 8}", """(props) => {
                    const out = {};
                    for (const prop of props) {
                        try {
                            const val = eval(prop);
                            out[prop] = {
                                value: typeof val === 'object' ? JSON.stringify(val).substring(0, 500) : String(val),
                                type: typeof val
                            };
                        } catch(e) {
                            out[prop] = { value: null, error: e.message };
                        }
                    }
                    return out;
                }""", properties[i:i + 8]))
        deadline = evaluate_started + _COMPARE_ENV_EVALUATE_TIMEOUT_S
        for chunk_name, script, arg in chunks:
            remaining_s = deadline - time.perf_counter()
            if remaining_s <= 0:
                chunk_timeouts.append({"chunk": chunk_name, "timeout_ms": 0, "phase": "budget_exhausted"})
                _camoufox_debug(
                    "compare_env_chunk_budget_exhausted",
                    chunk=chunk_name,
                    elapsed_ms=int((time.perf_counter() - started) * 1000),
                    result_property_count=_compare_env_result_count(result),
                )
                break
            timeout_s = min(_COMPARE_ENV_CHUNK_TIMEOUT_S, remaining_s)
            try:
                name, chunk_result = await _compare_env_eval_chunk(page, chunk_name, script, arg, timeout_s, started)
                if name.startswith("custom_"):
                    result["custom"].update(chunk_result)
                else:
                    result[name] = chunk_result
            except asyncio.TimeoutError:
                chunk_timeouts.append({"chunk": chunk_name, "timeout_ms": int(timeout_s * 1000), "phase": "chunk_eval"})
                _camoufox_debug(
                    "compare_env_chunk_timeout",
                    chunk=chunk_name,
                    timeout_ms=int(timeout_s * 1000),
                    elapsed_ms=int((time.perf_counter() - started) * 1000),
                    result_property_count=_compare_env_result_count(result),
                )
                break
            except Exception as chunk_exc:
                chunk_failures.append({
                    "chunk": chunk_name,
                    "error_type": type(chunk_exc).__name__,
                    "error": str(chunk_exc)[:300],
                })
                _camoufox_debug(
                    "compare_env_chunk_failed",
                    chunk=chunk_name,
                    error_type=type(chunk_exc).__name__,
                    error_len=len(str(chunk_exc)),
                    elapsed_ms=int((time.perf_counter() - started) * 1000),
                    result_property_count=_compare_env_result_count(result),
                )
        degraded = bool(chunk_timeouts or chunk_failures)
        result["status"] = "degraded" if degraded else "ok"
        result["instrumentation_status"] = "partial" if degraded else "complete"
        if chunk_timeouts:
            result["timeout_status"] = "controlled_timeout"
        result["requested_property_count"] = requested_property_count
        result["evaluation_timeout_ms"] = int(_COMPARE_ENV_EVALUATE_TIMEOUT_S * 1000)
        result["evaluation_chunk_timeout_ms"] = int(_COMPARE_ENV_CHUNK_TIMEOUT_S * 1000)
        result["evaluation_elapsed_ms"] = int((time.perf_counter() - evaluate_started) * 1000)
        result["result_property_count"] = _compare_env_result_count(result)
        result["chunk_timeouts"] = chunk_timeouts
        result["chunk_failures"] = chunk_failures
        result["partial_result"] = degraded
        _camoufox_debug(
            "compare_env_exit",
            success=not degraded,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            requested_property_count=requested_property_count,
            result_property_count=_compare_env_result_count(result),
            chunk_timeout_count=len(chunk_timeouts),
            chunk_failure_count=len(chunk_failures),
            page_url_len=page_url_len,
            page_url_error=page_url_error,
            page_title_len=page_title_len,
            page_title_error=page_title_error,
            timeout_phase="partial" if degraded else "complete",
        )
        return result
    except asyncio.TimeoutError:
        elapsed_ms = int((time.perf_counter() - started) * 1000)
        timeout_ms = _compare_env_phase_timeout_ms(timeout_phase)
        _camoufox_debug(
            "compare_env_timeout",
            elapsed_ms=elapsed_ms,
            requested_property_count=requested_property_count,
            page_url_len=page_url_len,
            page_url_error=page_url_error,
            page_title_len=page_title_len,
            page_title_error=page_title_error,
            timeout_phase=timeout_phase,
            timeout_ms=timeout_ms,
        )
        _camoufox_debug(
            "compare_env_exit",
            success=False,
            elapsed_ms=elapsed_ms,
            requested_property_count=requested_property_count,
            result_property_count=0,
            page_url_len=page_url_len,
            page_url_error=page_url_error,
            page_title_len=page_title_len,
            page_title_error=page_title_error,
            timeout_phase=timeout_phase,
            error_type="TimeoutError",
        )
        return {
            "status": "degraded",
            "instrumentation_status": "timeout",
            "timeout_status": "controlled_timeout",
            "error": "compare_env timed out",
            "phase": timeout_phase,
            "timeout_ms": timeout_ms,
            "elapsed_ms": elapsed_ms,
            "requested_property_count": requested_property_count,
            "result_property_count": 0,
            "page_url_len": page_url_len,
            "page_url_error": page_url_error,
            "page_title_len": page_title_len,
            "page_title_error": page_title_error,
        }
    except Exception as e:
        _camoufox_debug(
            "compare_env_exit",
            success=False,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            requested_property_count=requested_property_count,
            result_property_count=0,
            page_url_len=page_url_len,
            page_url_error=page_url_error,
            page_title_len=page_title_len,
            page_title_error=page_title_error,
            timeout_phase=timeout_phase,
            error_type=type(e).__name__,
        )
        return {"error": str(e)}
