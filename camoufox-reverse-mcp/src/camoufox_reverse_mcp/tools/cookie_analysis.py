"""
cookie_analysis.py - Attribute every cookie to its source.

Cookies can enter the jar via:
  1. HTTP response Set-Cookie header (server-side)
  2. JS document.cookie = "..." (client-side)
  3. navigator.cookieStore API (modern, rare)

This module correlates captured network responses with client-side
cookie writes to explain where each cookie came from. Critical for
understanding RS/AK-style signature-based anti-bot cookie flows where the JS computes
a token but the HTTP layer is where the cookie actually gets set.
"""
from __future__ import annotations
import re
import time

from ..browser import _camoufox_debug, _safe_text
from ..server import mcp, browser_manager


_COOKIE_NAME_RE = re.compile(r'^\s*([^=;\s]+)\s*=')


def _parse_cookie_name(header_value: str) -> str | None:
    m = _COOKIE_NAME_RE.match(header_value or "")
    return m.group(1) if m else None


@mcp.tool()
async def analyze_cookie_sources(
    name_filter: str | None = None,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    """Attribute every observed cookie to its source (HTTP header vs JS).

    Combines:
      - Captured network responses' Set-Cookie headers
        (requires start_network_capture was active)
      - document.cookie write log from cookie_hook
        (requires cookie_hook.js was injected)
      - Currently-present cookies via page.context.cookies()

    Args:
        name_filter: Only return cookies with this substring in name.

    Returns:
        dict mapping cookie_name -> {
            sources: ["http_set_cookie" | "js_document_cookie"],
            first_set_ts: ms timestamp of first observation,
            http_response_urls: [urls that sent Set-Cookie for this name],
            js_stacks: [JS stack traces that wrote this cookie],
            current_value: present value in the cookie jar,
        }
    """
    started = time.perf_counter()
    _camoufox_debug(
        "analyze_cookie_sources_begin",
        name_filter=bool(name_filter),
        requested_page_id=page_id or "",
        active_page_id=browser_manager.active_page_id or "",
        page_count=len(browser_manager.pages),
        captured_request_count=len(browser_manager._network_requests),
    )
    try:
        pages: list[tuple[str, object]] = []
        if page_id:
            page = await browser_manager.resolve_page_for_operation(page_id, "analyze_cookie_sources", True, aida_operation_id)
            pages.append((browser_manager.page_id_for(page) or page_id, page))
        else:
            seen: set[int] = set()
            active_id = browser_manager.active_page_id or browser_manager.active_page_name
            if active_id and active_id in browser_manager.pages:
                page = browser_manager.pages.get(active_id)
                if page is not None and not browser_manager._page_closed(page):
                    pages.append((active_id, page))
                    seen.add(id(page))
            for pid, page in list(browser_manager.pages.items()):
                if id(page) in seen:
                    continue
                if not browser_manager._page_closed(page):
                    pages.append((pid, page))
                    seen.add(id(page))
            if not pages:
                page = await browser_manager.resolve_page_for_operation(None, "analyze_cookie_sources", True, aida_operation_id)
                pages.append((browser_manager.page_id_for(page) or "", page))
        input_snapshot = {
            "requested_page_id": page_id or "",
            "active_page_id": browser_manager.active_page_id or "",
            "pages": [{"page_id": pid, "url": str(getattr(page, "url", "") or "")} for pid, page in pages],
            "captured_request_count": len(browser_manager._network_requests),
            "name_filter_len": len(name_filter or ""),
        }
        http_sources: dict[str, list[dict]] = {}
        network_set_cookie_count = 0
        for req in browser_manager._network_requests:
            headers = req.get("response_headers") or {}
            sc = None
            for k, v in headers.items():
                if k.lower() == "set-cookie":
                    sc = v
                    break
            if not sc:
                continue
            lines = sc.split("\n") if "\n" in sc else [sc]
            for line in lines:
                name = _parse_cookie_name(line)
                if not name:
                    continue
                network_set_cookie_count += 1
                http_sources.setdefault(name, []).append({
                    "url": req.get("url"),
                    "ts": req.get("timestamp"),
                    "header": line.strip()[:300],
                })

        js_sources: dict[str, list[dict]] = {}
        js_cookie_log_count = 0
        active_url = ""
        active_url_error = ""
        page_cookie_logs: list[dict] = []
        for pid, page in pages:
            try:
                if not active_url:
                    active_url = page.url or ""
                log = await page.evaluate("window.__mcp_cookie_log || []")
                page_log_count = len(log) if isinstance(log, list) else 0
                js_cookie_log_count += page_log_count
                page_cookie_logs.append({"page_id": pid, "url": page.url or "", "js_cookie_log_count": page_log_count})
                if not isinstance(log, list):
                    continue
                for entry in log:
                    if entry.get("op") != "set":
                        continue
                    name = _parse_cookie_name(entry.get("value", ""))
                    if not name:
                        continue
                    js_sources.setdefault(name, []).append({
                        "value": (entry.get("value") or "")[:300],
                        "stack": (entry.get("stack") or "")[:800],
                        "ts": entry.get("ts"),
                        "page_id": pid,
                        "page_url": page.url or "",
                    })
            except Exception as exc:
                active_url_error = type(exc).__name__
                page_cookie_logs.append({"page_id": pid, "error_type": type(exc).__name__, "error": _safe_text(exc, 300)})

        current: dict[str, str] = {}
        cookie_jar_count = 0
        seen_contexts: set[int] = set()
        for pid, page in pages:
            try:
                if not active_url:
                    active_url = page.url or ""
                ctx = page.context
                if id(ctx) in seen_contexts:
                    continue
                seen_contexts.add(id(ctx))
                jar = await ctx.cookies()
                cookie_jar_count += len(jar)
                for c in jar:
                    current[c["name"]] = c.get("value", "")
            except Exception as exc:
                if not active_url_error:
                    active_url_error = type(exc).__name__

        all_names = set(http_sources) | set(js_sources) | set(current)
        if name_filter:
            all_names = {n for n in all_names if name_filter in n}

        result = {}
        for name in sorted(all_names):
            sources = []
            if name in http_sources:
                sources.append("http_set_cookie")
            if name in js_sources:
                sources.append("js_document_cookie")
            ts_candidates = []
            ts_candidates.extend(h.get("ts") for h in http_sources.get(name, []) if h.get("ts"))
            ts_candidates.extend(j.get("ts") for j in js_sources.get(name, []) if j.get("ts"))
            result[name] = {
                "sources": sources or ["unknown_or_preexisting"],
                "first_set_ts": min(ts_candidates) if ts_candidates else None,
                "http_responses": http_sources.get(name, []),
                "js_writes": js_sources.get(name, []),
                "current_value": (current.get(name) or "")[:300],
            }

        empty_reason = ""
        if not result:
            if name_filter and (set(http_sources) | set(js_sources) | set(current)):
                empty_reason = "name_filter_matched_no_cookies"
            elif active_url_error and cookie_jar_count == 0 and js_cookie_log_count == 0:
                empty_reason = "active_page_or_cookie_jar_unavailable"
            elif len(browser_manager._network_requests) == 0 and cookie_jar_count == 0 and js_cookie_log_count == 0:
                empty_reason = "no_active_cookie_or_request_evidence"
            elif network_set_cookie_count == 0 and js_cookie_log_count == 0 and cookie_jar_count == 0:
                empty_reason = "no_cookie_sources_observed"
            else:
                empty_reason = "observed_sources_filtered_to_empty"

        diagnostics = {
            "active_url": active_url,
            "active_url_len": len(active_url),
            "active_url_error": active_url_error,
            "cookie_jar_count": cookie_jar_count,
            "cookie_jar_unique_names": len(current),
            "network_set_cookie_count": network_set_cookie_count,
            "network_set_cookie_unique_names": len(http_sources),
            "js_cookie_log_count": js_cookie_log_count,
            "js_cookie_unique_names": len(js_sources),
            "captured_request_count": len(browser_manager._network_requests),
            "page_cookie_logs": page_cookie_logs,
            "input_snapshot": input_snapshot,
            "empty_reason": empty_reason,
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
        }
        _camoufox_debug(
            "analyze_cookie_sources_exit",
            success=True,
            total_cookies=len(result),
            **diagnostics,
        )
        return {
            "cookies": result,
            "total_cookies": len(result),
            "empty_reason": empty_reason,
            "active_url": active_url,
            "active_url_len": len(active_url),
            "active_url_error": active_url_error,
            "cookie_jar_count": cookie_jar_count,
            "captured_request_count": len(browser_manager._network_requests),
            "network_set_cookie_count": network_set_cookie_count,
            "js_cookie_log_count": js_cookie_log_count,
            "page_id": page_id or "",
            "active_page_id": browser_manager.active_page_id or "",
            "diagnostics": diagnostics,
            "hint": (
                "If a cookie appears only in 'http_set_cookie' (e.g. acw_tc, "
                "NfBCSins2Oyw), it was set by the server via a response header, "
                "typically AFTER the JS challenge computed some token. Look at "
                "the http_responses[].url to find the server endpoint that sets it, "
                "and work backwards from there."
            ),
        }
    except Exception as e:
        _camoufox_debug(
            "analyze_cookie_sources_exit",
            success=False,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            error_type=type(e).__name__,
            error_len=len(str(e)),
        )
        return {"error": str(e)}
