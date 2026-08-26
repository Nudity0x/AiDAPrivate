"""Environment self-check tool (v1.0.0: session fields removed)."""
from __future__ import annotations

import asyncio
import importlib
import time
from typing import Any

from ..browser import _await_no_cancel_wait, _camoufox_debug, _verify_page_privacy
from ..server import mcp, browser_manager


@mcp.tool()
async def check_environment() -> dict:
    """One-stop self-check of MCP environment, dependencies, and browser state.

    v1.0.0: session-related checks removed (session mechanism removed).
    Checks MCP version, critical dependencies (esprima, playwright),
    browser state (residuals, captures).

    Returns:
        dict with sections: mcp, deps, browser, overall_ok, recommendations.
    """
    started = time.perf_counter()
    recommendations: list[str] = []
    _camoufox_debug("check_environment_begin")

    # MCP version
    try:
        mod = importlib.import_module("camoufox_reverse_mcp")
        version = getattr(mod, "__version__", "unknown")
        parts = tuple(int(x) for x in version.split(".") if x.isdigit())
        version_ok = parts >= (1, 0, 0)
    except Exception:
        version = "unknown"
        version_ok = False
    if not version_ok:
        recommendations.append(f"MCP version is {version}, need >= 1.0.0.")

    # Dependencies
    deps: dict[str, dict] = {}
    for dep in ("esprima", "playwright"):
        try:
            m = importlib.import_module(dep)
            deps[dep] = {"installed": True, "version": getattr(m, "__version__", "unknown"), "ok": True}
        except ImportError:
            deps[dep] = {"installed": False, "version": None, "ok": False}

    # Browser state
    browser_state: dict[str, Any] = {"running": False}
    privacy_ok = True
    timeout_status = ""
    try:
        if browser_manager.browser is not None:
            browser_state["running"] = True
            ctx = browser_manager.contexts.get("default")
            pages = ctx.pages if ctx else []
            browser_state["page_count"] = len(pages)
            browser_state["persistent_scripts_count"] = len(browser_manager._persistent_scripts)
            browser_state["active_captures"] = browser_manager._capturing
            browser_state["captured_requests_count"] = len(browser_manager._network_requests)
            browser_state["privacy"] = {}
            if getattr(browser_manager, "_context_plan", None):
                try:
                    page = await _await_no_cancel_wait(browser_manager.get_active_page(), timeout=4.0)
                    browser_state["active_url"] = page.url or ""
                    browser_state["active_url_len"] = len(browser_state["active_url"])
                    _camoufox_debug(
                        "check_environment_privacy_begin",
                        elapsed_ms=int((time.perf_counter() - started) * 1000),
                        active_url_len=browser_state["active_url_len"],
                        page_count=len(pages),
                        verify_timeout_ms=10000,
                    )
                    privacy = await _await_no_cancel_wait(_verify_page_privacy(page, browser_manager._context_plan), timeout=10.0)
                    browser_state["privacy"] = privacy
                    privacy_ok = bool(
                        privacy.get("webrtc_blocked")
                        and privacy.get("webdriver_ok")
                        and privacy.get("platform_ok", True)
                        and privacy.get("oscpu_ok", True)
                        and privacy.get("ice_probe_ok")
                        and not privacy.get("ice_candidate_leak_detected")
                    )
                    _camoufox_debug(
                        "check_environment_privacy_exit",
                        success=True,
                        elapsed_ms=int((time.perf_counter() - started) * 1000),
                        privacy_ok=bool(privacy_ok),
                        active_url_len=browser_state["active_url_len"],
                    )
                except asyncio.TimeoutError:
                    timeout_status = "controlled_timeout"
                    privacy_ok = False
                    browser_state["privacy"] = {
                        "ok": False,
                        "status": "degraded",
                        "timeout_status": timeout_status,
                        "error": "privacy verification timed out",
                        "timeout_ms": 10000,
                    }
                    _camoufox_debug(
                        "check_environment_privacy_timeout",
                        elapsed_ms=int((time.perf_counter() - started) * 1000),
                        timeout_ms=10000,
                    )
                except Exception as e:
                    privacy_ok = False
                    browser_state["privacy"] = {
                        "ok": False,
                        "error": str(e),
                        "error_type": type(e).__name__,
                    }
            has_residuals = (
                browser_state["persistent_scripts_count"] > 0
                or browser_state["captured_requests_count"] > 0
            )
            browser_state["has_residuals"] = has_residuals
            if has_residuals:
                recommendations.append("Browser has residual state. Consider reset_browser_state().")
            if not privacy_ok:
                recommendations.append("Browser privacy verification failed. Close and relaunch Camoufox.")
    except Exception as e:
        browser_state["error"] = str(e)
        privacy_ok = False

    overall_ok = version_ok and all(d["ok"] for d in deps.values() if d.get("installed")) and privacy_ok

    # camoufox-reverse custom browser detection
    from ..property_trace import CACHE_DIR, CONTROL_DIR, TRACES_DIR
    custom_browser: dict[str, Any] = {"installed": False}
    try:
        # Check if trace control files exist (= custom browser running with trace)
        ctrl_files = list(CONTROL_DIR.glob("control-*.cmd")) if CONTROL_DIR.exists() else []
        trace_files = list(TRACES_DIR.glob("*.jsonl")) if TRACES_DIR.exists() else []
        if ctrl_files:
            custom_browser = {
                "installed": True,
                "trace_active": True,
                "control_files": len(ctrl_files),
                "trace_files": len(trace_files),
                "cache_dir": str(CACHE_DIR),
            }
        else:
            custom_browser = {
                "installed": False,
                "install_hint": (
                    "Download camoufox-reverse from "
                    "https://github.com/WhiteNightShadow/camoufox-reverse/releases "
                    "and launch with enable_trace=True"
                ),
            }
    except Exception:
        pass

    result = {
        "status": "ok" if overall_ok else "degraded",
        "mcp": {"version": version, "version_ok": version_ok},
        "deps": deps,
        "browser": browser_state,
        "camoufox_reverse": custom_browser,
        "overall_ok": overall_ok,
        "recommendations": recommendations,
        "elapsed_ms": int((time.perf_counter() - started) * 1000),
    }
    if timeout_status:
        result["timeout_status"] = timeout_status
        result["instrumentation_status"] = "timeout"
    _camoufox_debug(
        "check_environment_exit",
        success=bool(overall_ok),
        status=result["status"],
        elapsed_ms=result["elapsed_ms"],
        browser_running=bool(browser_state.get("running")),
        page_count=int(browser_state.get("page_count") or 0),
        privacy_ok=bool(privacy_ok),
        timeout_status=timeout_status,
    )
    return result
