from __future__ import annotations

import asyncio
import time
from typing import Any

from ..browser import _await_no_cancel_wait, _camoufox_debug, _safe_text
from ..server import browser_manager, mcp


_SSE_HOOK_NAME = "aida:sse"
_SSE_LIMIT = 500
_SSE_HOOK = r"""
(() => {
  const existing = window.__aida_sse_debug;
  if (existing && existing.version === 1 && existing.installed) return existing.snapshot();
  const NativeEventSource = window.EventSource;
  const state = existing || { version: 1, nextId: 1, sources: {}, objects: {}, events: [], installedAt: Date.now() };
  state.version = 1;
  state.installed = !!NativeEventSource;
  state.maxEvents = 500;
  state.maxText = 8192;
  function trim() {
    while (state.events.length > state.maxEvents) state.events.shift();
    for (const item of Object.values(state.sources)) {
      while (item.events.length > state.maxEvents) item.events.shift();
    }
  }
  function record(source, event) {
    const data = String(event && event.data == null ? "" : event.data);
    const entry = {
      id: source.id + ":" + (source.events.length + 1),
      source_id: source.id,
      url: source.url,
      event: String(event && event.type || "message"),
      data: data.slice(0, state.maxText),
      data_length: data.length,
      truncated: data.length > state.maxText,
      last_event_id: String(event && event.lastEventId || ""),
      timestamp: Date.now()
    };
    source.events.push(entry);
    state.events.push(entry);
    if (!source.eventTypes.includes(entry.event)) source.eventTypes.push(entry.event);
    trim();
  }
  function attach(es, url, options) {
    if (es.__aida_sse_id && state.sources[es.__aida_sse_id]) return es;
    const id = "sse-" + state.nextId++;
    const source = {
      id,
      url: String(url || ""),
      withCredentials: !!(options && options.withCredentials),
      createdAt: Date.now(),
      readyState: es.readyState,
      eventTypes: [],
      events: [],
      closed: false
    };
    try { Object.defineProperty(es, "__aida_sse_id", { value: id, configurable: false }); } catch (e) { es.__aida_sse_id = id; }
    state.sources[id] = source;
    state.objects[id] = es;
    const nativeAdd = es.addEventListener;
    es.addEventListener = function(type, listener, options) {
      const eventType = String(type || "");
      if (eventType && !source.eventTypes.includes(eventType)) source.eventTypes.push(eventType);
      if (typeof listener === "function") {
        return nativeAdd.call(this, type, function(event) {
          record(source, event);
          return listener.call(this, event);
        }, options);
      }
      return nativeAdd.call(this, type, listener, options);
    };
    nativeAdd.call(es, "message", event => record(source, event));
    nativeAdd.call(es, "open", event => {
      source.readyState = es.readyState;
      if (!source.eventTypes.includes("open")) source.eventTypes.push("open");
    });
    nativeAdd.call(es, "error", event => {
      source.readyState = es.readyState;
      source.errorAt = Date.now();
      if (!source.eventTypes.includes("error")) source.eventTypes.push("error");
      if (es.readyState === NativeEventSource.CLOSED) {
        source.closed = true;
        source.closedAt = Date.now();
      }
    });
    const nativeClose = es.close;
    es.close = function() {
      source.closed = true;
      source.closedAt = Date.now();
      source.readyState = NativeEventSource.CLOSED;
      return nativeClose.call(this);
    };
    return es;
  }
  if (NativeEventSource) {
    function WrappedEventSource(url, options) {
      const es = new NativeEventSource(url, options);
      return attach(es, url, options || {});
    }
    Object.setPrototypeOf(WrappedEventSource, NativeEventSource);
    WrappedEventSource.prototype = NativeEventSource.prototype;
    for (const key of ["CONNECTING", "OPEN", "CLOSED"]) {
      try { Object.defineProperty(WrappedEventSource, key, { value: NativeEventSource[key], enumerable: true }); } catch (e) {}
    }
    window.EventSource = WrappedEventSource;
  }
  state.snapshot = function() {
    for (const [id, es] of Object.entries(state.objects)) {
      const source = state.sources[id];
      if (!source) continue;
      try { source.readyState = es.readyState; } catch (e) {}
    }
    return {
      installed: !!NativeEventSource,
      supported: !!NativeEventSource,
      installedAt: state.installedAt,
      source_count: Object.keys(state.sources).length,
      event_count: state.events.length,
      sources: Object.values(state.sources).map(source => ({
        id: source.id,
        url: source.url,
        readyState: source.readyState,
        withCredentials: source.withCredentials,
        eventTypes: source.eventTypes.slice(),
        createdAt: source.createdAt,
        closedAt: source.closedAt || null,
        event_count: source.events.length
      }))
    };
  };
  state.eventsFor = function(target, limit) {
    const max = Math.max(1, Math.min(Number(limit || 200), 500));
    let rows = state.events;
    if (target) rows = rows.filter(item => item.source_id === target || item.url === target || item.url.indexOf(target) >= 0);
    return rows.slice(Math.max(0, rows.length - max));
  };
  window.__aida_sse_debug = state;
  return state.snapshot();
})()
"""


def _limit(value: int | None, fallback: int = 200) -> int:
    try:
        parsed = int(value if value is not None else fallback)
    except Exception:
        parsed = fallback
    return max(1, min(parsed, _SSE_LIMIT))


async def _page_context(page: Any, page_id: str | None) -> dict:
    try:
        return await browser_manager.page_envelope(page, page_id)
    except Exception:
        return {"page_id": browser_manager.page_id_for(page) or page_id or "", "active_page_id": browser_manager.active_page_id, "session_id": browser_manager.session_id}


async def _install(page: Any, page_id: str | None) -> dict:
    await browser_manager.add_persistent_script(_SSE_HOOK_NAME, _SSE_HOOK)
    result = await _await_no_cancel_wait(page.evaluate(_SSE_HOOK), timeout=5.0)
    out = result if isinstance(result, dict) else {"installed": False, "result_type": type(result).__name__}
    out.update(await _page_context(page, page_id))
    return out


@mcp.tool()
async def browser_sse(
    action: str = "list_sources",
    source_url: str | None = None,
    source_id: str | None = None,
    duration: int | float | None = None,
    duration_ms: int = 5000,
    limit: int = 200,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    action_name = str(action or "list_sources").strip().lower()
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, f"browser_sse:{action_name}", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
        max_items = _limit(limit)
        installed = await _install(page, resolved_page_id)
        if action_name == "list_sources":
            snapshot = await _await_no_cancel_wait(page.evaluate("() => window.__aida_sse_debug && window.__aida_sse_debug.snapshot ? window.__aida_sse_debug.snapshot() : null"), timeout=3.0)
            out = snapshot if isinstance(snapshot, dict) else installed
            out.update(await _page_context(page, resolved_page_id))
            out.update({"success": True, "status": "ok", "action": action_name, "elapsed_ms": int((time.perf_counter() - started) * 1000)})
            return out
        if action_name == "inspect_events":
            target = source_id or source_url or ""
            events = await _await_no_cancel_wait(page.evaluate("""arg => {
              const state = window.__aida_sse_debug;
              if (!state || !state.eventsFor) return { events: [], installed: false };
              return { events: state.eventsFor(arg.target, arg.limit), installed: true };
            }""", {"target": target, "limit": max_items}), timeout=3.0)
            out = events if isinstance(events, dict) else {"events": [], "installed": False}
            out.update(await _page_context(page, resolved_page_id))
            out.update({"success": True, "status": "ok", "action": action_name, "target": target, "elapsed_ms": int((time.perf_counter() - started) * 1000)})
            return out
        if action_name == "monitor":
            before = await _await_no_cancel_wait(page.evaluate("() => window.__aida_sse_debug && Array.isArray(window.__aida_sse_debug.events) ? window.__aida_sse_debug.events.length : 0"), timeout=2.0)
            effective_duration_ms = duration_ms
            if duration is not None:
                try:
                    effective_duration_ms = int(float(duration) * 1000.0)
                except Exception:
                    effective_duration_ms = duration_ms
            wait_s = max(0.1, min(int(effective_duration_ms or 5000), 30000) / 1000.0)
            await asyncio.sleep(wait_s)
            events = await _await_no_cancel_wait(page.evaluate("""arg => {
              const state = window.__aida_sse_debug;
              if (!state || !Array.isArray(state.events)) return [];
              return state.events.slice(Math.max(Number(arg.before || 0), state.events.length - Number(arg.limit || 200)));
            }""", {"before": int(before or 0), "limit": max_items}), timeout=3.0)
            return {
                "success": True,
                "status": "ok",
                "action": action_name,
                "events": events if isinstance(events, list) else [],
                "duration_ms": int(wait_s * 1000),
                "page_id": resolved_page_id,
                "active_page_id": browser_manager.active_page_id,
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
        return {"success": False, "status": "failed", "action": action_name, "error": "unknown action. Use list_sources, inspect_events, monitor", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
    except Exception as exc:
        _camoufox_debug("browser_sse_exception", action=action_name, error_type=type(exc).__name__, error_summary=_safe_text(exc, 500))
        return {"success": False, "status": "failed", "action": action_name, "error": str(exc), "error_type": type(exc).__name__, "active_page_id": browser_manager.active_page_id}
