from __future__ import annotations

import asyncio
import time
from typing import Any

from ..browser import _await_no_cancel_wait, _camoufox_debug, _safe_text
from ..server import browser_manager, mcp


_WS_HOOK_NAME = "aida:ws_debug"
_WS_LIMIT = 500
_WS_HOOK = r"""
(() => {
  const existing = window.__aida_ws_debug;
  if (existing && existing.version === 1 && existing.installed) return existing.snapshot();
  const NativeWebSocket = window.WebSocket;
  const state = existing || { version: 1, nextId: 1, connections: {}, objects: {}, frames: [], installedAt: Date.now() };
  state.version = 1;
  state.installed = true;
  state.maxFrames = 500;
  state.maxFrameText = 8192;
  function trim() {
    while (state.frames.length > state.maxFrames) state.frames.shift();
    for (const conn of Object.values(state.connections)) {
      while (conn.frames.length > state.maxFrames) conn.frames.shift();
    }
  }
  function encode(value) {
    const out = { type: Object.prototype.toString.call(value).slice(8, -1), text: "", length: 0, binary: false, truncated: false };
    try {
      if (typeof value === "string") {
        out.type = "string";
        out.length = value.length;
        out.text = value.slice(0, state.maxFrameText);
        out.truncated = value.length > state.maxFrameText;
        return out;
      }
      if (value instanceof ArrayBuffer) {
        const view = new Uint8Array(value);
        out.type = "ArrayBuffer";
        out.length = view.byteLength;
        out.binary = true;
        out.text = Array.from(view.slice(0, Math.min(256, view.length))).map(b => b.toString(16).padStart(2, "0")).join("");
        out.truncated = view.length > 256;
        return out;
      }
      if (ArrayBuffer.isView(value)) {
        const view = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
        out.type = value.constructor && value.constructor.name || "TypedArray";
        out.length = view.byteLength;
        out.binary = true;
        out.text = Array.from(view.slice(0, Math.min(256, view.length))).map(b => b.toString(16).padStart(2, "0")).join("");
        out.truncated = view.length > 256;
        return out;
      }
      if (typeof Blob !== "undefined" && value instanceof Blob) {
        out.type = "Blob";
        out.length = value.size || 0;
        out.binary = true;
        out.text = "";
        return out;
      }
      const text = String(value);
      out.length = text.length;
      out.text = text.slice(0, state.maxFrameText);
      out.truncated = text.length > state.maxFrameText;
      return out;
    } catch (e) {
      out.type = "unserializable";
      out.text = String(e && (e.message || e));
      return out;
    }
  }
  function record(conn, direction, data, eventName) {
    const encoded = encode(data);
    const item = {
      id: conn.id + ":" + (conn.frames.length + 1),
      connection_id: conn.id,
      url: conn.url,
      direction,
      event: eventName || "",
      timestamp: Date.now(),
      opcode: encoded.binary ? "binary" : "text",
      data: encoded.text,
      data_type: encoded.type,
      data_length: encoded.length,
      truncated: encoded.truncated
    };
    conn.frames.push(item);
    state.frames.push(item);
    trim();
    return item;
  }
  function connectionFromTarget(target) {
    for (const [id, obj] of Object.entries(state.objects)) {
      if (obj === target) return state.connections[id];
    }
    return null;
  }
  function attach(ws, url, protocols) {
    if (ws.__aida_ws_id && state.connections[ws.__aida_ws_id]) return ws;
    const id = "ws-" + state.nextId++;
    const conn = {
      id,
      url: String(url || ""),
      protocols: Array.isArray(protocols) ? protocols.map(String) : (protocols == null ? [] : [String(protocols)]),
      createdAt: Date.now(),
      readyState: ws.readyState,
      protocol: ws.protocol || "",
      extensions: ws.extensions || "",
      frames: [],
      sent: 0,
      received: 0,
      closed: false
    };
    try { Object.defineProperty(ws, "__aida_ws_id", { value: id, configurable: false }); } catch (e) { ws.__aida_ws_id = id; }
    state.connections[id] = conn;
    state.objects[id] = ws;
    const nativeSend = ws.send;
    ws.send = function(data) {
      conn.sent += 1;
      conn.readyState = ws.readyState;
      record(conn, "send", data, "send");
      return nativeSend.call(this, data);
    };
    ws.addEventListener("message", event => {
      conn.received += 1;
      conn.readyState = ws.readyState;
      record(conn, "recv", event && event.data, "message");
    });
    ws.addEventListener("close", event => {
      conn.readyState = ws.readyState;
      conn.closed = true;
      conn.closedAt = Date.now();
      conn.closeCode = event && event.code;
      conn.closeReason = event && event.reason || "";
    });
    ws.addEventListener("error", () => {
      conn.readyState = ws.readyState;
      conn.errorAt = Date.now();
    });
    return ws;
  }
  function WrappedWebSocket(url, protocols) {
    const ws = protocols === undefined ? new NativeWebSocket(url) : new NativeWebSocket(url, protocols);
    return attach(ws, url, protocols);
  }
  Object.setPrototypeOf(WrappedWebSocket, NativeWebSocket);
  WrappedWebSocket.prototype = NativeWebSocket.prototype;
  for (const key of ["CONNECTING", "OPEN", "CLOSING", "CLOSED"]) {
    try { Object.defineProperty(WrappedWebSocket, key, { value: NativeWebSocket[key], enumerable: true }); } catch (e) {}
  }
  state.snapshot = function() {
    for (const [id, ws] of Object.entries(state.objects)) {
      const conn = state.connections[id];
      if (!conn) continue;
      try {
        conn.readyState = ws.readyState;
        conn.protocol = ws.protocol || conn.protocol || "";
        conn.extensions = ws.extensions || conn.extensions || "";
        conn.bufferedAmount = ws.bufferedAmount || 0;
      } catch (e) {}
    }
    return {
      installed: true,
      installedAt: state.installedAt,
      connection_count: Object.keys(state.connections).length,
      frame_count: state.frames.length,
      connections: Object.values(state.connections).map(conn => ({
        id: conn.id,
        url: conn.url,
        readyState: conn.readyState,
        protocol: conn.protocol || "",
        extensions: conn.extensions || "",
        bufferedAmount: conn.bufferedAmount || 0,
        createdAt: conn.createdAt,
        closedAt: conn.closedAt || null,
        closeCode: conn.closeCode || null,
        closeReason: conn.closeReason || "",
        sent: conn.sent || 0,
        received: conn.received || 0,
        frame_count: conn.frames.length
      }))
    };
  };
  state.framesFor = function(target, limit) {
    const max = Math.max(1, Math.min(Number(limit || 200), 500));
    let frames = state.frames;
    if (target) {
      frames = frames.filter(item => item.connection_id === target || item.url === target || item.url.indexOf(target) >= 0);
    }
    return frames.slice(Math.max(0, frames.length - max));
  };
  state.inject = function(target, data, isBinary) {
    let selected = null;
    if (target && state.objects[target]) selected = state.objects[target];
    if (!selected && target) {
      for (const [id, conn] of Object.entries(state.connections)) {
        if (conn.url === target || conn.url.indexOf(target) >= 0) {
          selected = state.objects[id];
          break;
        }
      }
    }
    if (!selected) {
      const open = Object.entries(state.objects).find(([, ws]) => ws && ws.readyState === NativeWebSocket.OPEN);
      if (open) selected = open[1];
    }
    if (!selected) return { ok: false, error: "no_hooked_websocket_available" };
    if (selected.readyState !== NativeWebSocket.OPEN) return { ok: false, error: "websocket_not_open", readyState: selected.readyState };
    const conn = connectionFromTarget(selected);
    let payload = data;
    if (isBinary) {
      if (Array.isArray(data)) payload = new Uint8Array(data);
      else if (typeof data === "string") payload = new TextEncoder().encode(data);
      else return { ok: false, error: "binary_data_must_be_string_or_byte_array" };
    }
    selected.send(payload);
    return { ok: true, connection_id: conn && conn.id || "", url: conn && conn.url || selected.url || "", bytes_or_chars: isBinary && payload && payload.byteLength !== undefined ? payload.byteLength : String(data == null ? "" : data).length };
  };
  window.WebSocket = WrappedWebSocket;
  window.__aida_ws_debug = state;
  return state.snapshot();
})()
"""


def _limit(value: int | None, fallback: int = 200) -> int:
    try:
        parsed = int(value if value is not None else fallback)
    except Exception:
        parsed = fallback
    return max(1, min(parsed, _WS_LIMIT))


async def _page_context(page: Any, page_id: str | None) -> dict:
    try:
        envelope = await browser_manager.page_envelope(page, page_id)
    except Exception:
        envelope = {
            "page_id": browser_manager.page_id_for(page) or page_id or "",
            "active_page_id": browser_manager.active_page_id,
            "session_id": browser_manager.session_id,
        }
    return envelope


async def _install(page: Any, page_id: str | None) -> dict:
    await browser_manager.add_persistent_script(_WS_HOOK_NAME, _WS_HOOK)
    result = await _await_no_cancel_wait(page.evaluate(_WS_HOOK), timeout=5.0)
    out = result if isinstance(result, dict) else {"installed": False, "result_type": type(result).__name__}
    out.update(await _page_context(page, page_id))
    return out


async def _legacy_websocket_log(page: Any, limit: int) -> list[dict]:
    try:
        rows = await _await_no_cancel_wait(page.evaluate("""limit => {
          const log = Array.isArray(window.__mcp_ws_log) ? window.__mcp_ws_log : [];
          return log.slice(Math.max(0, log.length - limit)).map((entry, index) => ({
            id: "legacy-" + index,
            url: String(entry && entry.url || ""),
            createdAt: entry && entry.timestamp || null,
            frame_count: Array.isArray(entry && entry.messages) ? entry.messages.length : 0,
            frames: Array.isArray(entry && entry.messages) ? entry.messages.slice(-limit).map((msg, frameIndex) => ({
              id: "legacy-" + index + ":" + frameIndex,
              connection_id: "legacy-" + index,
              url: String(entry && entry.url || ""),
              direction: String(msg && msg.direction || ""),
              timestamp: msg && msg.timestamp || null,
              opcode: "text",
              data: String(msg && msg.data || ""),
              data_type: "string",
              data_length: String(msg && msg.data || "").length,
              truncated: false
            })) : []
          }));
        }""", limit), timeout=3.0)
        return rows if isinstance(rows, list) else []
    except Exception:
        return []


@mcp.tool()
async def browser_ws_debug(
    action: str = "list_connections",
    connection_url: str | None = None,
    connection_id: str | None = None,
    data: str | list[int] | None = None,
    is_binary: bool = False,
    isBinary: bool | None = None,
    duration: int | float | None = None,
    duration_ms: int = 5000,
    limit: int = 200,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    action_name = str(action or "list_connections").strip().lower()
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, f"browser_ws_debug:{action_name}", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
        max_items = _limit(limit)
        await _install(page, resolved_page_id)
        target = connection_id or connection_url or ""
        if action_name == "list_connections":
            snapshot = await _await_no_cancel_wait(page.evaluate("() => window.__aida_ws_debug && window.__aida_ws_debug.snapshot ? window.__aida_ws_debug.snapshot() : null"), timeout=3.0)
            legacy = await _legacy_websocket_log(page, max_items)
            out = snapshot if isinstance(snapshot, dict) else {"installed": False, "connections": [], "connection_count": 0}
            if legacy:
                out["legacy_connections"] = [{"id": item.get("id"), "url": item.get("url"), "createdAt": item.get("createdAt"), "frame_count": item.get("frame_count")} for item in legacy]
            out.update(await _page_context(page, resolved_page_id))
            out.update({"success": True, "status": "ok", "action": action_name, "elapsed_ms": int((time.perf_counter() - started) * 1000)})
            return out
        if action_name == "inspect_frames":
            frames = await _await_no_cancel_wait(page.evaluate("""arg => {
              const state = window.__aida_ws_debug;
              if (!state || !state.framesFor) return { frames: [], installed: false };
              return { frames: state.framesFor(arg.target, arg.limit), installed: true };
            }""", {"target": target, "limit": max_items}), timeout=3.0)
            legacy = await _legacy_websocket_log(page, max_items)
            legacy_frames: list[dict] = []
            if target and legacy:
                for item in legacy:
                    if target in str(item.get("id") or "") or target in str(item.get("url") or ""):
                        legacy_frames.extend(item.get("frames") or [])
            elif legacy:
                for item in legacy:
                    legacy_frames.extend(item.get("frames") or [])
            out = frames if isinstance(frames, dict) else {"frames": [], "installed": False}
            if legacy_frames:
                out["legacy_frames"] = legacy_frames[-max_items:]
            out.update(await _page_context(page, resolved_page_id))
            out.update({"success": True, "status": "ok", "action": action_name, "target": target, "elapsed_ms": int((time.perf_counter() - started) * 1000)})
            return out
        if action_name == "inject_message":
            if data is None:
                return {"success": False, "status": "failed", "action": action_name, "error": "data is required", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
            binary_flag = bool(is_binary if isBinary is None else isBinary)
            result = await _await_no_cancel_wait(page.evaluate("""arg => {
              const state = window.__aida_ws_debug;
              if (!state || !state.inject) return { ok: false, error: "websocket_hook_not_installed" };
              return state.inject(arg.target, arg.data, arg.isBinary);
            }""", {"target": target, "data": data, "isBinary": binary_flag}), timeout=3.0)
            payload = result if isinstance(result, dict) else {"ok": False, "error": "unexpected_injection_result"}
            out = {
                "success": bool(payload.get("ok")),
                "status": "ok" if payload.get("ok") else "failed",
                "action": action_name,
                "target": target,
                "injection": payload,
                "page_id": resolved_page_id,
                "active_page_id": browser_manager.active_page_id,
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
            if not payload.get("ok"):
                out["limitation"] = "Only WebSocket objects created after the persistent page hook was installed can be targeted."
            return out
        if action_name == "monitor":
            before = await _await_no_cancel_wait(page.evaluate("() => window.__aida_ws_debug && window.__aida_ws_debug.frames ? window.__aida_ws_debug.frames.length : 0"), timeout=2.0)
            effective_duration_ms = duration_ms
            if duration is not None:
                try:
                    effective_duration_ms = int(float(duration) * 1000.0)
                except Exception:
                    effective_duration_ms = duration_ms
            wait_s = max(0.1, min(int(effective_duration_ms or 5000), 30000) / 1000.0)
            await asyncio.sleep(wait_s)
            after = await _await_no_cancel_wait(page.evaluate("""arg => {
              const state = window.__aida_ws_debug;
              if (!state || !Array.isArray(state.frames)) return [];
              return state.frames.slice(Math.max(Number(arg.before || 0), state.frames.length - Number(arg.limit || 200)));
            }""", {"before": int(before or 0), "limit": max_items}), timeout=3.0)
            out = {
                "success": True,
                "status": "ok",
                "action": action_name,
                "frames": after if isinstance(after, list) else [],
                "duration_ms": int(wait_s * 1000),
                "page_id": resolved_page_id,
                "active_page_id": browser_manager.active_page_id,
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
            return out
        return {"success": False, "status": "failed", "action": action_name, "error": "unknown action. Use list_connections, inspect_frames, inject_message, monitor", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
    except Exception as exc:
        _camoufox_debug("browser_ws_debug_exception", action=action_name, error_type=type(exc).__name__, error_summary=_safe_text(exc, 500))
        return {"success": False, "status": "failed", "action": action_name, "error": str(exc), "error_type": type(exc).__name__, "active_page_id": browser_manager.active_page_id}
