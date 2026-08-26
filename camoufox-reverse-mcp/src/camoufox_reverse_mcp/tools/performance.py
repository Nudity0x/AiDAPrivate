from __future__ import annotations

import builtins
import re
import time
from typing import Any

from ..browser import _await_no_cancel_wait, _camoufox_debug, _safe_text
from ..server import mcp, browser_manager


_MAX_FILTER_LEN = 2048
_JWT_RE = re.compile(r"\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b")


_PERFORMANCE_JS = r"""
async args => {
    const action = String(args.action || 'navigation_timing');
    const limit = Math.max(1, Math.min(Number(args.limit || 200), 1000));
    const urlFilter = String(args.urlFilter || '').toLowerCase();
    const initiatorType = String(args.initiatorType || '').toLowerCase();
    function cleanEntry(entry) {
        if (!entry) return {};
        const json = typeof entry.toJSON === 'function' ? entry.toJSON() : {};
        const out = {...json};
        for (const key of ['name','entryType','initiatorType','startTime','duration','nextHopProtocol','workerStart','redirectStart','redirectEnd','fetchStart','domainLookupStart','domainLookupEnd','connectStart','connectEnd','secureConnectionStart','requestStart','responseStart','responseEnd','transferSize','encodedBodySize','decodedBodySize','renderBlockingStatus']) {
            if (entry[key] !== undefined && out[key] === undefined) out[key] = entry[key];
        }
        return out;
    }
    function supported(type) {
        try {
            return Array.isArray(PerformanceObserver.supportedEntryTypes) && PerformanceObserver.supportedEntryTypes.includes(type);
        } catch(e) {
            return false;
        }
    }
    if (!('performance' in window)) return {supported: false, error: 'Performance API is not available'};
    if (action === 'navigation_timing') {
        const nav = performance.getEntriesByType('navigation')[0];
        if (nav) return {supported: true, timing: cleanEntry(nav), source: 'performance_navigation'};
        const t = performance.timing;
        if (!t) return {supported: false, error: 'navigation timing is not available'};
        return {supported: true, source: 'performance_timing', timing: {
            navigationStart: t.navigationStart,
            unloadEventStart: t.unloadEventStart,
            unloadEventEnd: t.unloadEventEnd,
            redirectStart: t.redirectStart,
            redirectEnd: t.redirectEnd,
            fetchStart: t.fetchStart,
            domainLookupStart: t.domainLookupStart,
            domainLookupEnd: t.domainLookupEnd,
            connectStart: t.connectStart,
            connectEnd: t.connectEnd,
            secureConnectionStart: t.secureConnectionStart,
            requestStart: t.requestStart,
            responseStart: t.responseStart,
            responseEnd: t.responseEnd,
            domLoading: t.domLoading,
            domInteractive: t.domInteractive,
            domContentLoadedEventStart: t.domContentLoadedEventStart,
            domContentLoadedEventEnd: t.domContentLoadedEventEnd,
            domComplete: t.domComplete,
            loadEventStart: t.loadEventStart,
            loadEventEnd: t.loadEventEnd
        }};
    }
    if (action === 'resource_timing') {
        let entries = performance.getEntriesByType('resource').map(cleanEntry);
        if (urlFilter) entries = entries.filter(entry => String(entry.name || '').toLowerCase().includes(urlFilter));
        if (initiatorType) entries = entries.filter(entry => String(entry.initiatorType || '').toLowerCase() === initiatorType);
        const total = entries.length;
        entries = entries.slice(0, limit);
        return {supported: true, entries, count: entries.length, total_count: total, truncated: total > entries.length};
    }
    if (action === 'paint_timing') {
        const entries = performance.getEntriesByType('paint').map(cleanEntry);
        const paints = {};
        for (const entry of entries) paints[entry.name] = entry.startTime;
        return {supported: true, paints, entries, count: entries.length};
    }
    if (action === 'long_tasks') {
        const entries = performance.getEntriesByType('longtask').map(cleanEntry).slice(0, limit);
        return {supported: supported('longtask') || entries.length > 0, entries, count: entries.length};
    }
    if (action === 'memory') {
        const memory = performance.memory || null;
        if (memory) {
            return {supported: true, source: 'performance.memory', memory: {
                usedJSHeapSize: memory.usedJSHeapSize,
                totalJSHeapSize: memory.totalJSHeapSize,
                jsHeapSizeLimit: memory.jsHeapSizeLimit
            }};
        }
        if (typeof performance.measureUserAgentSpecificMemory === 'function') {
            try {
                const result = await performance.measureUserAgentSpecificMemory();
                return {supported: true, source: 'measureUserAgentSpecificMemory', memory: result};
            } catch(e) {
                return {supported: false, source: 'measureUserAgentSpecificMemory', error: String(e && e.message || e)};
            }
        }
        return {supported: false, error: 'browser memory API is not available'};
    }
    if (action === 'observe') {
        const type = String(args.type || args.entryType || '').trim();
        const durationMs = Math.max(100, Math.min(Number(args.durationMs || 1000), 30000));
        if (!type) return {supported: false, error: 'type is required'};
        if (typeof PerformanceObserver !== 'function') return {supported: false, error: 'PerformanceObserver is not available', type};
        if (!supported(type)) return {supported: false, error: 'entry type is not supported', type, supported_types: PerformanceObserver.supportedEntryTypes || []};
        return await new Promise(resolve => {
            const entries = [];
            let observer = null;
            const finish = () => {
                try { if (observer) observer.disconnect(); } catch(e) {}
                resolve({supported: true, type, entries: entries.slice(0, limit), count: Math.min(entries.length, limit), total_count: entries.length, truncated: entries.length > limit, duration_ms: durationMs});
            };
            try {
                observer = new PerformanceObserver(list => {
                    for (const entry of list.getEntries()) {
                        if (entries.length < limit + 1) entries.push(cleanEntry(entry));
                    }
                });
                observer.observe({type, buffered: true});
                setTimeout(finish, durationMs);
            } catch(e) {
                resolve({supported: false, type, error: String(e && e.message || e)});
            }
        });
    }
    return {supported: true, error: 'unknown action: ' + action};
}
"""


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


def _sanitize_url(value: Any) -> str:
    text = str(value or "")
    text = re.sub(r"(?i)([?&](?:token|session|secret|api[_-]?key|password|authorization)=)[^&#]+", r"\1<redacted>", text)
    text = _JWT_RE.sub("<redacted:jwt>", text)
    return text[:4096]


def _sanitize_entries(entries: Any) -> list[dict[str, Any]]:
    out = []
    if not isinstance(entries, list):
        return out
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        item = dict(entry)
        if "name" in item:
            item["name"] = _sanitize_url(item.get("name"))
        out.append(item)
    return out


def _sanitize_result(result: dict[str, Any]) -> dict[str, Any]:
    if "entries" in result:
        result["entries"] = _sanitize_entries(result.get("entries"))
    timing = result.get("timing")
    if isinstance(timing, dict) and "name" in timing:
        timing["name"] = _sanitize_url(timing.get("name"))
    return result


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


@mcp.tool()
async def browser_performance(
    action: str = "navigation_timing",
    url_filter: str | None = None,
    initiator_type: str | None = None,
    type: str | None = None,
    entry_type: str | None = None,
    duration: int | float | None = None,
    duration_ms: int = 1000,
    limit: int = 200,
    timeout_ms: int = 30000,
    payload: dict | None = None,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    selected_action = str(_payload_value(payload, "action", action) or "navigation_timing").strip().lower()
    aliases = {
        "navigation": "navigation_timing",
        "resources": "resource_timing",
        "resource": "resource_timing",
        "paint": "paint_timing",
        "long_task": "long_tasks",
        "longtask": "long_tasks",
        "observe_entries": "observe",
    }
    selected_action = aliases.get(selected_action, selected_action)
    effective_page_id = _payload_value(payload, "page_id", page_id)
    timeout = _bounded_int(_payload_value(payload, "timeout_ms", timeout_ms), 30000, 250, 120000)
    try:
        effective_filter = str(_payload_value(payload, "url_filter", url_filter) or "")
        if len(effective_filter) > _MAX_FILTER_LEN:
            raise ValueError(f"url_filter exceeds {_MAX_FILTER_LEN} characters")
        effective_type = str(_payload_value(payload, "type", type) or _payload_value(payload, "entry_type", entry_type) or "")
        if len(effective_type) > 128:
            raise ValueError("type exceeds 128 characters")
        duration_value = _payload_value(payload, "duration_ms")
        if duration_value is None:
            duration_seconds = _payload_value(payload, "duration", duration)
            if duration_seconds is not None:
                try:
                    duration_value = int(float(duration_seconds) * 1000.0)
                except Exception:
                    duration_value = duration_ms
            else:
                duration_value = duration_ms
        effective_duration = _bounded_int(duration_value, 1000, 100, 30000)
        page = await browser_manager.resolve_page_for_operation(effective_page_id, f"browser_performance:{selected_action}", True, aida_operation_id)
        eval_timeout = max(timeout, effective_duration + 5000) if selected_action == "observe" else timeout
        result = await _await_no_cancel_wait(page.evaluate(_PERFORMANCE_JS, {
            "action": selected_action,
            "urlFilter": effective_filter,
            "initiatorType": str(_payload_value(payload, "initiator_type", initiator_type) or ""),
            "type": effective_type,
            "entryType": effective_type,
            "durationMs": effective_duration,
            "limit": _bounded_int(_payload_value(payload, "limit", limit), 200, 1, 1000),
        }), timeout=eval_timeout / 1000.0)
        result = _sanitize_result(result if isinstance(result, dict) else {"error": f"unexpected result type: {builtins.type(result).__name__}"})
        if result.get("error"):
            out = await _base(selected_action, page, "error", False)
        elif result.get("supported", True) is False:
            out = await _base(selected_action, page, "unavailable", False)
        else:
            out = await _base(selected_action, page, "ok", True)
        out.update(result)
        out["elapsed_ms"] = int((time.perf_counter() - started) * 1000)
        return out
    except Exception as exc:
        _camoufox_debug("browser_performance_error", action=selected_action, page_id=str(effective_page_id or ""), error_type=builtins.type(exc).__name__, error_summary=_safe_text(exc, 700))
        return _error(selected_action, exc, str(effective_page_id or ""))
