from __future__ import annotations

import time
from typing import Any

from ..browser import _await_no_cancel_wait, _camoufox_debug, _safe_text
from ..server import browser_manager, mcp


def _normal_action(value: str | None) -> str:
    action = str(value or "").strip().lower()
    return action or "list"


def _diagnostic_contract(action_name: str) -> dict:
    return {
        "action": action_name,
        "diagnostic_only": True,
        "trusted_service_worker_interception": False,
        "trusted_push_event": False,
        "worker_script_rewritten": False,
        "protocol_boundary": "page_context_playwright_route_or_worker_postmessage",
    }


async def _page_context(page: Any, page_id: str | None) -> dict:
    try:
        return await browser_manager.page_envelope(page, page_id)
    except Exception:
        return {"page_id": browser_manager.page_id_for(page) or page_id or "", "active_page_id": browser_manager.active_page_id, "session_id": browser_manager.session_id}


async def _list_in_page(page: Any) -> dict:
    result = await _await_no_cancel_wait(page.evaluate("""async () => {
      if (!("serviceWorker" in navigator)) return { supported: false, registrations: [] };
      const regs = await navigator.serviceWorker.getRegistrations();
      return {
        supported: true,
        controller: navigator.serviceWorker.controller ? {
          scriptURL: navigator.serviceWorker.controller.scriptURL || "",
          state: navigator.serviceWorker.controller.state || ""
        } : null,
        registrations: regs.map(reg => {
          const worker = reg.active || reg.waiting || reg.installing;
          return {
            scope: reg.scope || "",
            scriptURL: worker ? worker.scriptURL || "" : "",
            state: worker ? worker.state || "" : "",
            activeScriptURL: reg.active ? reg.active.scriptURL || "" : "",
            waitingScriptURL: reg.waiting ? reg.waiting.scriptURL || "" : "",
            installingScriptURL: reg.installing ? reg.installing.scriptURL || "" : "",
            updateViaCache: reg.updateViaCache || "",
            type: worker && worker.type || ""
          };
        })
      };
    }"""), timeout=6.0)
    return result if isinstance(result, dict) else {"supported": False, "registrations": []}


def _matches_worker(registration: dict, script_url: str | None) -> bool:
    target = str(script_url or "").strip()
    if not target:
        return True
    values = [
        registration.get("scriptURL"),
        registration.get("activeScriptURL"),
        registration.get("waitingScriptURL"),
        registration.get("installingScriptURL"),
        registration.get("scope"),
    ]
    return any(target == str(value or "") or target in str(value or "") for value in values)


@mcp.tool()
async def browser_service_worker(
    action: str = "list",
    script_url: str | None = None,
    url_pattern: str | None = None,
    intercept_action: str = "log",
    modify_headers: dict | None = None,
    modify_body: str | None = None,
    mock_response: dict | None = None,
    payload: dict | str | None = None,
    payload_json: dict | str | None = None,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    action_name = _normal_action(action)
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, f"browser_service_worker:{action_name}", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
        if action_name in {"intercept", "push_event"}:
            replacement = "diagnostic_route" if action_name == "intercept" else "diagnostic_post_message"
            out = {
                "success": False,
                "status": "failed",
                "action": action_name,
                "error": "action_renamed_to_explicit_diagnostic_contract",
                "replacement_action": replacement,
                "available_actions": ["list", "get_source", "unregister", "diagnostic_route", "diagnostic_post_message"],
                "page_id": resolved_page_id,
                "active_page_id": browser_manager.active_page_id,
            }
            out.update(_diagnostic_contract(replacement))
            out["action"] = action_name
            return out
        if action_name == "list":
            data = await _list_in_page(page)
            out = {
                "success": True,
                "status": "ok",
                "action": action_name,
                "supported": bool(data.get("supported")),
                "controller": data.get("controller"),
                "service_workers": data.get("registrations") or [],
                "count": len(data.get("registrations") or []),
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
            out.update(await _page_context(page, resolved_page_id))
            return out
        if action_name == "get_source":
            if not script_url:
                return {"success": False, "status": "failed", "action": action_name, "error": "script_url is required", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
            result = await _await_no_cancel_wait(page.evaluate("""async scriptUrl => {
              try {
                const response = await fetch(scriptUrl, { credentials: "include", cache: "no-store" });
                const source = await response.text();
                return { ok: response.ok, status: response.status, statusText: response.statusText || "", source, length: source.length, url: response.url || scriptUrl };
              } catch (e) {
                return { ok: false, status: 0, statusText: "", error: String(e && (e.message || e)), source: "", length: 0, url: scriptUrl };
              }
            }""", script_url), timeout=15.0)
            out = result if isinstance(result, dict) else {"ok": False, "error": "unexpected_source_result", "source": "", "length": 0}
            out.update({"success": bool(out.get("ok")), "status": "ok" if out.get("ok") else "failed", "action": action_name, "elapsed_ms": int((time.perf_counter() - started) * 1000)})
            out.update(await _page_context(page, resolved_page_id))
            if not out.get("ok"):
                out["limitation"] = "Service worker source can only be fetched when same-origin/CORS browser policy permits it."
            return out
        if action_name == "unregister":
            data = await _await_no_cancel_wait(page.evaluate("""async target => {
              if (!("serviceWorker" in navigator)) return { supported: false, matched: 0, unregistered: 0, results: [] };
              const regs = await navigator.serviceWorker.getRegistrations();
              const results = [];
              let matched = 0;
              let unregistered = 0;
              for (const reg of regs) {
                const worker = reg.active || reg.waiting || reg.installing;
                const values = [reg.scope || "", worker && worker.scriptURL || "", reg.active && reg.active.scriptURL || "", reg.waiting && reg.waiting.scriptURL || "", reg.installing && reg.installing.scriptURL || ""];
                const isMatch = !target || values.some(value => String(value || "").includes(target));
                if (!isMatch) continue;
                matched += 1;
                const ok = await reg.unregister();
                if (ok) unregistered += 1;
                results.push({ scope: reg.scope || "", scriptURL: worker && worker.scriptURL || "", unregistered: !!ok });
              }
              return { supported: true, matched, unregistered, results };
            }""", script_url or ""), timeout=8.0)
            out = data if isinstance(data, dict) else {"supported": False, "matched": 0, "unregistered": 0, "results": []}
            out.update({"success": bool(out.get("supported")), "status": "ok" if out.get("supported") else "failed", "action": action_name, "elapsed_ms": int((time.perf_counter() - started) * 1000)})
            out.update(await _page_context(page, resolved_page_id))
            return out
        if action_name == "diagnostic_post_message":
            data = await _await_no_cancel_wait(page.evaluate("""async arg => {
              if (!("serviceWorker" in navigator)) return { ok: false, error: "service_worker_api_unavailable" };
              const regs = await navigator.serviceWorker.getRegistrations();
              for (const reg of regs) {
                const worker = reg.active || reg.waiting || reg.installing;
                const values = [reg.scope || "", worker && worker.scriptURL || ""];
                if (arg.target && !values.some(value => String(value || "").includes(arg.target))) continue;
                if (!worker || typeof worker.postMessage !== "function") return { ok: false, error: "matched_worker_cannot_receive_postmessage", scope: reg.scope || "" };
                worker.postMessage({ type: "aida.synthetic_push_payload", payload: arg.payload });
                return { ok: true, scope: reg.scope || "", scriptURL: worker.scriptURL || "", method: "postMessage" };
              }
              return { ok: false, error: "matching_service_worker_not_found" };
            }""", {"target": script_url or "", "payload": payload if payload is not None else payload_json}), timeout=6.0)
            out = data if isinstance(data, dict) else {"ok": False, "error": "unexpected_push_result"}
            out.update(_diagnostic_contract(action_name))
            out.update({"success": bool(out.get("ok")), "status": "diagnostic_message_posted" if out.get("ok") else "failed", "action": action_name, "elapsed_ms": int((time.perf_counter() - started) * 1000)})
            out.update(await _page_context(page, resolved_page_id))
            out["semantic_note"] = "This posts a controlled diagnostic message to the matched worker when available; it is not a browser-trusted PushEvent."
            return out
        if action_name == "diagnostic_route":
            pattern = url_pattern or script_url or "**/*"
            selected_action = str(intercept_action or "log").strip().lower()
            if selected_action == "stop":
                await page.unroute(pattern)
                browser_manager._route_handlers.pop(f"service_worker_diagnostic:{resolved_page_id}:{pattern}", None)
                out = {"success": True, "status": "diagnostic_route_stopped", "action": action_name, "pattern": pattern, "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
                out.update(_diagnostic_contract(action_name))
                return out
            async def handler(route):
                if selected_action == "log":
                    browser_manager._console_logs.append({"level": "info", "text": f"[SERVICE_WORKER_INTERCEPT] {route.request.method} {route.request.url}", "timestamp": int(time.time() * 1000), "page_id": resolved_page_id})
                    await route.continue_()
                elif selected_action == "block":
                    await route.abort()
                elif selected_action == "modify":
                    overrides = {}
                    if modify_headers:
                        overrides["headers"] = {**dict(route.request.headers), **modify_headers}
                    if modify_body is not None:
                        overrides["post_data"] = modify_body
                    await route.continue_(**overrides)
                elif selected_action == "mock":
                    response = mock_response or {}
                    await route.fulfill(status=int(response.get("status", 200)), headers=dict(response.get("headers") or {"content-type": "application/json"}), body=str(response.get("body", "{}")))
                else:
                    await route.continue_()
            await page.route(pattern, handler)
            browser_manager._route_handlers[f"service_worker_diagnostic:{resolved_page_id}:{pattern}"] = handler
            workers = await _list_in_page(page)
            matched = [item for item in workers.get("registrations", []) if isinstance(item, dict) and _matches_worker(item, script_url)]
            out = {
                "success": True,
                "status": "diagnostic_route_active",
                "action": action_name,
                "pattern": pattern,
                "intercept_action": selected_action,
                "matched_service_workers": matched,
                "page_id": resolved_page_id,
                "active_page_id": browser_manager.active_page_id,
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
            out.update(_diagnostic_contract(action_name))
            out["semantic_note"] = "This installs a page/context Playwright route for diagnostics. It does not rewrite registered service worker scripts and does not guarantee interception of requests already handled inside a service worker."
            return out
        return {"success": False, "status": "failed", "action": action_name, "error": "unknown action. Use list, get_source, unregister, diagnostic_route, diagnostic_post_message", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
    except Exception as exc:
        _camoufox_debug("browser_service_worker_exception", action=action_name, error_type=type(exc).__name__, error_summary=_safe_text(exc, 500))
        return {"success": False, "status": "failed", "action": action_name, "error": str(exc), "error_type": type(exc).__name__, "active_page_id": browser_manager.active_page_id}
