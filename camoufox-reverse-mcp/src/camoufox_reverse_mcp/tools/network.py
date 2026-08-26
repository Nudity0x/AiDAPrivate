from __future__ import annotations

import asyncio
import hashlib
import json
import re
import time

from ..browser import _await_no_cancel_wait, _camoufox_debug
from ..server import mcp, browser_manager


AIDA_INITIATOR_CONTRACT_V2 = "aida_initiator_contract_v2_page_marker"
_NETWORK_EVAL_TIMEOUT_S = 3.0
_NETWORK_INITIATOR_TIMEOUT_S = 4.0
_NETWORK_DEFAULT_LIMIT = 200
_NETWORK_MAX_LIMIT = 1000
_NETWORK_BODY_SEARCH_MAX_BYTES = 1_000_000
_NETWORK_BODY_SNIPPET_MAX_CHARS = 512
_NETWORK_MAX_DELAY_MS = 15_000
_SECRET_REDACTIONS = (
    re.compile(r"(?i)(authorization\s*[:=]\s*)(bearer\s+)?[^\s&,\"']+"),
    re.compile(r"(?i)((?:api[_-]?key|access[_-]?token|refresh[_-]?token|session[_-]?token|password|secret|license[_-]?key)\s*[:=]\s*)[^\s&,\"']+"),
    re.compile(r"(?i)((?:cookie|set-cookie)\s*:\s*)[^\r\n]+"),
)


def _url_hash(value: str | None) -> str:
    return hashlib.sha256((value or "").encode("utf-8", errors="ignore")).hexdigest()[:16]


def _sample_url_diagnostics(values: object) -> dict:
    urls = values if isinstance(values, list) else []
    return {
        "sample_urls": urls[:5],
        "sample_url_lengths": [len(str(v or "")) for v in urls[:5]],
        "sample_url_hashes": [_url_hash(str(v or "")) for v in urls[:5]],
    }


def _safe_text(value: object, limit: int = 500) -> str:
    text = str(value or "")
    if len(text) <= limit:
        return text
    return text[:limit]


def _bounded_int(value: object, fallback: int, minimum: int, maximum: int) -> int:
    try:
        parsed = int(value)
    except Exception:
        return fallback
    if parsed < minimum:
        return minimum
    if parsed > maximum:
        return maximum
    return parsed


def _request_matches_text_filter(entry: dict, filter_text: str | None) -> bool:
    needle = str(filter_text or "").strip().lower()
    if not needle:
        return True
    values = [
        entry.get("url"),
        entry.get("method"),
        entry.get("resource_type"),
        entry.get("type"),
        entry.get("status"),
        entry.get("status_code"),
        entry.get("page_id"),
        entry.get("context_id"),
        entry.get("post_data"),
        entry.get("request_post_data"),
        entry.get("request_body"),
        entry.get("response_body"),
        entry.get("failure"),
    ]
    for key in ("request_headers", "response_headers", "headers", "initiator", "timing", "redirect_chain"):
        value = entry.get(key)
        if value is not None:
            try:
                values.append(json.dumps(value, sort_keys=True, default=str))
            except Exception:
                values.append(str(value))
    return any(needle in str(value or "").lower() for value in values)


def _request_marker_matches(entry: dict, marker: str | None) -> bool:
    needle = str(marker or "")
    if not needle:
        return True
    values = [
        entry.get("url"),
        entry.get("post_data"),
        entry.get("response_body"),
        entry.get("request_body"),
    ]
    for key in ("request_headers", "response_headers", "headers"):
        value = entry.get(key)
        if value is not None:
            try:
                values.append(json.dumps(value, sort_keys=True, default=str))
            except Exception:
                values.append(str(value))
    return any(needle in str(value or "") for value in values)


def _request_matches_scope(entry: dict, page_id: str | None = None, marker: str | None = None) -> bool:
    if page_id and str(entry.get("page_id") or "") != str(page_id):
        return False
    return _request_marker_matches(entry, marker)


def _lower_header_map(headers: object) -> dict[str, str]:
    if not isinstance(headers, dict):
        return {}
    out: dict[str, str] = {}
    for key, value in headers.items():
        out[str(key or "").lower()] = str(value or "")
    return out


def _current_session_id() -> str:
    return str(getattr(browser_manager, "session_id", "") or "")


def _session_matches(expected_session_id: str | None) -> bool:
    expected = str(expected_session_id or "").strip()
    return not expected or expected == _current_session_id()


def _request_frame_url(request) -> str:
    try:
        frame = getattr(request, "frame", None)
        if callable(frame):
            frame = frame()
        value = getattr(frame, "url", "") if frame is not None else ""
        if callable(value):
            value = value()
        return str(value or "")
    except Exception:
        return ""


def _bounded_body_text(value: object, max_body_size: int) -> dict:
    limit = _bounded_int(max_body_size, _NETWORK_BODY_SEARCH_MAX_BYTES, 0, _NETWORK_BODY_SEARCH_MAX_BYTES)
    if value is None:
        return {"text": "", "original_bytes": 0, "searched_bytes": 0, "truncated": False, "sha256": hashlib.sha256(b"").hexdigest()}
    if isinstance(value, bytes):
        original_bytes = len(value)
        body = value[:limit]
        text = body.decode("utf-8", errors="replace")
        return {
            "text": text,
            "original_bytes": original_bytes,
            "searched_bytes": len(body),
            "truncated": original_bytes > limit,
            "sha256": hashlib.sha256(body).hexdigest(),
        }
    raw = str(value)
    encoded = raw.encode("utf-8", errors="replace")
    original_bytes = len(encoded)
    body = encoded[:limit]
    text = body.decode("utf-8", errors="replace")
    return {
        "text": text,
        "original_bytes": original_bytes,
        "searched_bytes": len(body),
        "truncated": original_bytes > limit,
        "sha256": hashlib.sha256(body).hexdigest(),
    }


def _redact_text(text: str) -> str:
    out = str(text or "")
    for pattern in _SECRET_REDACTIONS:
        out = pattern.sub(lambda m: f"{m.group(1)}<redacted>", out)
    return out


def _body_snippet(text: str, index: int, snippet_chars: int) -> str:
    limit = _bounded_int(snippet_chars, 240, 32, _NETWORK_BODY_SNIPPET_MAX_CHARS)
    start = max(0, index - limit // 2)
    end = min(len(text), start + limit)
    return _redact_text(text[start:end])


def _body_match(text: str, needle: str, case_sensitive: bool) -> int:
    if not needle:
        return -1
    if case_sensitive:
        return text.find(needle)
    return text.lower().find(needle.lower())


def _request_body_value(entry: dict) -> object:
    for key in ("request_body", "request_post_data", "post_data", "body"):
        if entry.get(key) is not None:
            return entry.get(key)
    return ""


def _response_body_value(entry: dict) -> object:
    for key in ("response_body", "response_text", "body_text", "body"):
        if entry.get(key) is not None:
            return entry.get(key)
    return ""


def _condition_present(conditions: dict, *keys: str) -> bool:
    return any(key in conditions and conditions.get(key) is not None for key in keys)


def _normal_conditions(conditions: object | None, extra: dict | None = None) -> dict:
    out = dict(conditions) if isinstance(conditions, dict) else {}
    if isinstance(extra, dict):
        for key, value in extra.items():
            if value is not None and key not in out:
                out[key] = value
    return out


def _scope_includes(scope: object, target: str) -> bool:
    text = str(scope or "both").strip().lower()
    if text in ("", "all", "both", "request_response"):
        return True
    return target in {part.strip() for part in re.split(r"[,| ]+", text) if part.strip()}


def _headers_contain(headers: dict[str, str], expected: object, case_sensitive: bool) -> bool:
    if expected is None:
        return True
    haystack = dict(headers) if case_sensitive else {k.lower(): v.lower() for k, v in headers.items()}
    if isinstance(expected, dict):
        for key, value in expected.items():
            header_name = str(key or "")
            header_value = str(value or "")
            lookup_key = header_name.lower()
            actual = haystack.get(lookup_key, "")
            needle = header_value if case_sensitive else header_value.lower()
            if needle not in actual:
                return False
        return True
    needle = str(expected or "")
    if not case_sensitive:
        needle = needle.lower()
    return any(needle in value for value in haystack.values())


def _intercept_needs_response(conditions: dict) -> bool:
    if _condition_present(conditions, "status_code", "response_status", "response_header_contains", "response_headers_contain", "response_body_contains", "response_body_sha256"):
        return True
    if _condition_present(conditions, "body_search", "search_query", "query", "body_sha256"):
        return _scope_includes(conditions.get("search_scope", conditions.get("scope")), "response")
    return False


def _route_request_matches(request, conditions: dict, max_body_size: int, case_sensitive: bool, current_page_id: str | None = None, current_session_id: str | None = None) -> dict:
    url = str(getattr(request, "url", "") or "")
    method = str(getattr(request, "method", "") or "")
    resource_type = str(getattr(request, "resource_type", "") or "")
    frame_url = _request_frame_url(request)
    headers = _lower_header_map(getattr(request, "headers", {}))
    reasons: list[str] = []
    expected_page_id = str(conditions.get("page_id") or conditions.get("target_page_id") or "").strip()
    if expected_page_id and str(current_page_id or "") != expected_page_id:
        reasons.append("page_id")
    expected_session_id = str(conditions.get("session_id") or "").strip()
    if expected_session_id and str(current_session_id or "") != expected_session_id:
        reasons.append("session_id")
    expected_method = str(conditions.get("method") or "").strip()
    if expected_method and method.upper() != expected_method.upper():
        reasons.append("method")
    expected_resource = str(conditions.get("resource_type") or conditions.get("type") or "").strip()
    if expected_resource and resource_type != expected_resource:
        reasons.append("resource_type")
    url_contains = str(conditions.get("url_contains") or conditions.get("url_filter") or "").strip()
    if url_contains and url_contains not in url:
        reasons.append("url_contains")
    url_prefix = str(conditions.get("url_prefix") or "").strip()
    if url_prefix and not url.startswith(url_prefix):
        reasons.append("url_prefix")
    domain_contains = str(conditions.get("url_contains_domain") or conditions.get("domain_contains") or "").strip()
    if domain_contains and domain_contains not in url:
        reasons.append("url_contains_domain")
    initiator_contains = str(conditions.get("initiator_contains") or conditions.get("initiator_url_contains") or conditions.get("frame_url_contains") or "").strip()
    if initiator_contains and initiator_contains not in frame_url:
        reasons.append("initiator_url_contains")
    frame_url_prefix = str(conditions.get("frame_url_prefix") or "").strip()
    if frame_url_prefix and not frame_url.startswith(frame_url_prefix):
        reasons.append("frame_url_prefix")
    if not _headers_contain(headers, conditions.get("header_contains", conditions.get("request_header_contains")), case_sensitive):
        reasons.append("request_header_contains")
    body = _bounded_body_text(getattr(request, "post_data", "") or "", max_body_size)
    body_scope = conditions.get("search_scope", conditions.get("scope"))
    scope_request = _scope_includes(body_scope, "request")
    scope_response = _scope_includes(body_scope, "response")
    request_body_contains = str(conditions.get("request_body_contains") or "").strip()
    if request_body_contains and _body_match(body["text"], request_body_contains, case_sensitive) < 0:
        reasons.append("request_body_contains")
    body_sha256 = str(conditions.get("request_body_sha256") or "").strip().lower()
    if body_sha256 and body["sha256"].lower() != body_sha256:
        reasons.append("request_body_sha256")
    generic_sha256 = str(conditions.get("body_sha256") or "").strip().lower()
    generic_condition = bool(generic_sha256)
    generic_matched = bool(generic_sha256 and scope_request and body["sha256"].lower() == generic_sha256)
    if generic_sha256 and scope_request and not scope_response and not generic_matched:
        reasons.append("body_sha256")
    query = str(conditions.get("body_search") or conditions.get("search_query") or conditions.get("query") or "").strip()
    if query:
        generic_condition = True
    query_matched = bool(query and scope_request and _body_match(body["text"], query, case_sensitive) >= 0)
    generic_matched = generic_matched or query_matched
    if query and scope_request and not scope_response and not query_matched:
        reasons.append("request_body_search")
    return {
        "matched": not reasons,
        "miss_reasons": reasons,
        "request_body_sha256": body["sha256"],
        "request_body_truncated": body["truncated"],
        "request_body_searched_bytes": body["searched_bytes"],
        "generic_body_condition": generic_condition,
        "generic_body_matched": generic_matched,
        "page_id": str(current_page_id or ""),
        "session_id": str(current_session_id or ""),
        "initiator_url": frame_url[:240],
        "initiator_url_hash": _url_hash(frame_url),
    }


def _route_response_matches(response, response_body: object, conditions: dict, max_body_size: int, case_sensitive: bool, request_generic_body_matched: bool = False) -> dict:
    reasons: list[str] = []
    status = int(getattr(response, "status", 0) or 0)
    status_condition = conditions.get("status_code", conditions.get("response_status"))
    if status_condition is not None and status != _bounded_int(status_condition, -1, 0, 999):
        reasons.append("response_status")
    headers = _lower_header_map(getattr(response, "headers", {}))
    if not _headers_contain(headers, conditions.get("response_header_contains", conditions.get("response_headers_contain")), case_sensitive):
        reasons.append("response_header_contains")
    body = _bounded_body_text(response_body, max_body_size)
    body_scope = conditions.get("search_scope", conditions.get("scope"))
    scope_response = _scope_includes(body_scope, "response")
    response_body_contains = str(conditions.get("response_body_contains") or "").strip()
    if response_body_contains and _body_match(body["text"], response_body_contains, case_sensitive) < 0:
        reasons.append("response_body_contains")
    body_sha256 = str(conditions.get("response_body_sha256") or "").strip().lower()
    if body_sha256 and body["sha256"].lower() != body_sha256:
        reasons.append("response_body_sha256")
    generic_sha256 = str(conditions.get("body_sha256") or "").strip().lower()
    response_generic_matched = bool(generic_sha256 and scope_response and body["sha256"].lower() == generic_sha256)
    query = str(conditions.get("body_search") or conditions.get("search_query") or conditions.get("query") or "").strip()
    if query and scope_response and _body_match(body["text"], query, case_sensitive) >= 0:
        response_generic_matched = True
    if (generic_sha256 or query) and scope_response and not request_generic_body_matched and not response_generic_matched:
        reasons.append("body_search")
    return {
        "matched": not reasons,
        "miss_reasons": reasons,
        "response_body_sha256": body["sha256"],
        "response_body_truncated": body["truncated"],
        "response_body_searched_bytes": body["searched_bytes"],
        "generic_body_matched": bool(request_generic_body_matched or response_generic_matched),
    }


async def _fulfill_original(route, response, body: bytes | None = None) -> None:
    if body is None:
        await route.fulfill(response=response)
    else:
        await route.fulfill(response=response, body=body)


async def _bounded_delay(delay_ms: int, max_delay_ms: int) -> int:
    maximum = _bounded_int(max_delay_ms, 5000, 0, _NETWORK_MAX_DELAY_MS)
    delay = _bounded_int(delay_ms, 0, 0, maximum)
    if delay > 0:
        await asyncio.sleep(delay / 1000.0)
    return delay


def _normal_hook_counts(payload: object, marker: str | None = None) -> dict:
    data = payload if isinstance(payload, dict) else {}
    out = {
        "marker": str(marker or ""),
        "xhr_hook_active": bool(data.get("xhr_hook_active", False)),
        "fetch_hook_active": bool(data.get("fetch_hook_active", False)),
        "xhr_log_count": int(data.get("xhr_log_count", 0) or 0),
        "fetch_log_count": int(data.get("fetch_log_count", 0) or 0),
        "fetch_initiator_log_count": int(data.get("fetch_initiator_log_count", 0) or 0),
        "marker_xhr_log_count": int(data.get("marker_xhr_log_count", 0) or 0),
        "marker_fetch_log_count": int(data.get("marker_fetch_log_count", 0) or 0),
        "marker_fetch_initiator_log_count": int(data.get("marker_fetch_initiator_log_count", 0) or 0),
    }
    for name in ("xhr_sample_urls", "fetch_sample_urls", "fetch_initiator_sample_urls"):
        value = data.get(name)
        if isinstance(value, list):
            out[name] = value[:5]
    if data.get("error"):
        out["error"] = _safe_text(data.get("error"), 300)
    return out


async def _aida_network_hook_counts(page, marker: str | None = None, include_samples: bool = False) -> dict:
    if page is None:
        return _normal_hook_counts({"error": "page_unavailable"}, marker)
    escaped_marker = json.dumps(str(marker or ""))
    include_samples_text = "true" if include_samples else "false"
    try:
        result = await _await_no_cancel_wait(page.evaluate(f"""() => {{
            const marker = {escaped_marker};
            const includeSamples = {include_samples_text};
            const xhrLog = Array.isArray(window.__mcp_xhr_log) ? window.__mcp_xhr_log : [];
            const fetchLog = Array.isArray(window.__mcp_fetch_log) ? window.__mcp_fetch_log : [];
            const fetchInitLog = Array.isArray(window.__mcp_fetch_initiator_log) ? window.__mcp_fetch_initiator_log : [];
            function textValues(entry) {{
                const values = [
                    entry && entry.url,
                    entry && entry.body,
                    entry && entry.postData,
                    entry && entry.stack,
                    entry && entry.marker
                ];
                try {{ values.push(JSON.stringify(entry && entry.headers || {{}})); }} catch(e) {{}}
                return values;
            }}
            function matches(entry) {{
                if (!marker) return true;
                return textValues(entry).some(value => String(value || '').includes(marker));
            }}
            function countMatches(logs) {{
                if (!marker) return logs.length;
                let count = 0;
                for (const entry of logs) {{
                    if (matches(entry)) count++;
                }}
                return count;
            }}
            function sampleUrls(logs) {{
                if (!includeSamples) return [];
                const matched = logs.filter(matches);
                return matched.slice(Math.max(0, matched.length - 5)).map(entry => String((entry && entry.url) || '').slice(0, 240));
            }}
            return {{
                marker: marker || '',
                xhr_hook_active: !!window.__mcp_xhr_hooked,
                fetch_hook_active: !!window.__mcp_fetch_hooked,
                xhr_log_count: xhrLog.length,
                fetch_log_count: fetchLog.length,
                fetch_initiator_log_count: fetchInitLog.length,
                marker_xhr_log_count: countMatches(xhrLog),
                marker_fetch_log_count: countMatches(fetchLog),
                marker_fetch_initiator_log_count: countMatches(fetchInitLog),
                xhr_sample_urls: sampleUrls(xhrLog),
                fetch_sample_urls: sampleUrls(fetchLog),
                fetch_initiator_sample_urls: sampleUrls(fetchInitLog)
            }};
        }}"""), timeout=_NETWORK_EVAL_TIMEOUT_S)
        return _normal_hook_counts(result, marker)
    except Exception as exc:
        return _normal_hook_counts({"error": f"{type(exc).__name__}: {_safe_text(exc, 240)}"}, marker)


def _initiator_snapshot_source(snapshot: object) -> str:
    data = snapshot if isinstance(snapshot, dict) else {}
    return str(data.get("source") or data.get("type") or data.get("initiator_type") or "unknown")


def _initiator_snapshot_valid(snapshot: object, marker: str | None = None) -> bool:
    data = snapshot if isinstance(snapshot, dict) else {}
    source = _initiator_snapshot_source(data).lower()
    if source in ("", "unknown", "null", "none"):
        return False
    if not data.get("stack"):
        return False
    needle = str(marker or "")
    if needle and needle not in str(data.get("url") or data.get("request_url") or ""):
        return False
    return True


async def _aida_find_request_initiator_on_page(page, req_url: str, marker: str | None = None) -> dict:
    if page is None:
        return {"url": req_url, "stack": None, "type": "unknown", "diagnostics": {"error": "page_unavailable"}}
    escaped_url = json.dumps(req_url or "")
    escaped_marker = json.dumps(str(marker or ""))
    try:
        result = await _await_no_cancel_wait(page.evaluate(f"""() => {{
            const reqUrl = {escaped_url};
            const marker = {escaped_marker};
            const xhrLog = Array.isArray(window.__mcp_xhr_log) ? window.__mcp_xhr_log : [];
            const fetchLog = Array.isArray(window.__mcp_fetch_log) ? window.__mcp_fetch_log : [];
            const fetchInitLog = Array.isArray(window.__mcp_fetch_initiator_log) ? window.__mcp_fetch_initiator_log : [];
            function sampleUrls(logs) {{
                if (!logs || !logs.length) return [];
                return logs.slice(Math.max(0, logs.length - 5)).map(entry => String((entry && entry.url) || '').slice(0, 240));
            }}
            function values(entry) {{
                const out = [entry && entry.url, entry && entry.body, entry && entry.postData, entry && entry.stack, entry && entry.marker];
                try {{ out.push(JSON.stringify(entry && entry.headers || {{}})); }} catch(e) {{}}
                return out;
            }}
            function markerMatches(logUrl, entry) {{
                if (!marker) return true;
                const out = values(entry);
                out.push(logUrl || '');
                return out.some(value => String(value || '').includes(marker));
            }}
            function urlMatches(logUrl, entry) {{
                if (!markerMatches(logUrl, entry)) return false;
                if (reqUrl === logUrl || reqUrl.includes(logUrl) || logUrl.includes(reqUrl)) return true;
                try {{
                    const u1 = new URL(reqUrl, location.origin);
                    const u2 = new URL(logUrl, location.origin);
                    return u1.pathname === u2.pathname && u1.host === u2.host;
                }} catch(e) {{
                    return false;
                }}
            }}
            function counts() {{
                function countMarker(logs) {{
                    if (!marker) return logs.length;
                    let count = 0;
                    for (const entry of logs) {{
                        if (markerMatches((entry && entry.url) || '', entry)) count++;
                    }}
                    return count;
                }}
                return {{
                    xhr_log_count: xhrLog.length,
                    fetch_log_count: fetchLog.length,
                    fetch_initiator_log_count: fetchInitLog.length,
                    marker_xhr_log_count: countMarker(xhrLog),
                    marker_fetch_log_count: countMarker(fetchLog),
                    marker_fetch_initiator_log_count: countMarker(fetchInitLog)
                }};
            }}
            function diagnostics() {{
                const c = counts();
                return {{
                    marker: marker || '',
                    xhr_hook_active: !!window.__mcp_xhr_hooked,
                    fetch_hook_active: !!window.__mcp_fetch_hooked,
                    xhr_log_count: c.xhr_log_count,
                    fetch_log_count: c.fetch_log_count,
                    fetch_initiator_log_count: c.fetch_initiator_log_count,
                    marker_xhr_log_count: c.marker_xhr_log_count,
                    marker_fetch_log_count: c.marker_fetch_log_count,
                    marker_fetch_initiator_log_count: c.marker_fetch_initiator_log_count,
                    xhr_sample_urls: sampleUrls(xhrLog),
                    fetch_sample_urls: sampleUrls(fetchLog),
                    fetch_initiator_sample_urls: sampleUrls(fetchInitLog),
                    location_href: String(location.href || '').slice(0, 240),
                    hint: !window.__mcp_xhr_hooked && !window.__mcp_fetch_hooked
                        ? 'No hooks detected. Call inject_hook_preset("xhr"/"fetch") BEFORE navigating.'
                        : 'Hooks active but no matching URL found in logs.'
                }};
            }}
            function fromLog(log, type) {{
                const logUrl = String((log && log.url) || '');
                return {{
                    url: logUrl,
                    stack: log && log.stack || null,
                    type: type,
                    source: type,
                    method: log && log.method,
                    headers: log && log.headers,
                    body: log && log.body ? String(log.body).substring(0, 2000) : null,
                    timestamp: log && (log.timestamp || log.ts),
                    diagnostics: diagnostics()
                }};
            }}
            function searchLogs(logs, type) {{
                for (let i = logs.length - 1; i >= 0; i--) {{
                    const log = logs[i];
                    const logUrl = String((log && log.url) || '');
                    if (urlMatches(logUrl, log)) return fromLog(log, type);
                }}
                return null;
            }}
            const xhrResult = searchLogs(xhrLog, 'xhr');
            if (xhrResult) return xhrResult;
            const fetchResult = searchLogs(fetchLog, 'fetch');
            if (fetchResult) return fetchResult;
            const fetchInitResult = searchLogs(fetchInitLog, 'fetch_hook');
            if (fetchInitResult) return fetchInitResult;
            return {{ url: reqUrl, stack: null, type: 'unknown', source: 'unknown', diagnostics: diagnostics() }};
        }}"""), timeout=_NETWORK_INITIATOR_TIMEOUT_S)
    except Exception as exc:
        return {
            "url": req_url,
            "stack": None,
            "type": "unknown",
            "source": "unknown",
            "diagnostics": {"error": f"{type(exc).__name__}: {_safe_text(exc, 240)}"},
        }
    if not isinstance(result, dict):
        return {"url": req_url, "stack": None, "type": "unknown", "source": "unknown", "diagnostics": {"result_type": type(result).__name__}}
    if "source" not in result:
        result["source"] = result.get("type", "unknown")
    return result


async def _persist_initiator_snapshots(reqs: list[dict], marker: str | None = None, max_attempts: int = 50) -> dict:
    request_marker = str(marker or "")
    counts: dict[str, dict] = {}
    persisted = 0
    attempted = 0
    skipped = 0
    for entry in reqs:
        if not isinstance(entry, dict):
            continue
        if _initiator_snapshot_valid(entry.get("initiator_snapshot"), request_marker):
            persisted += 1
            continue
        if attempted >= max(0, int(max_attempts)):
            skipped += 1
            continue
        page_id = str(entry.get("page_id") or "")
        req_url = str(entry.get("url") or "")
        try:
            page = await browser_manager.resolve_page(page_id or None)
        except Exception:
            continue
        if page_id not in counts:
            counts[page_id] = await _aida_network_hook_counts(page, request_marker, True)
        attempted += 1
        snapshot = await _aida_find_request_initiator_on_page(page, req_url, request_marker)
        snapshot["captured_page_id"] = page_id
        snapshot["request_url"] = req_url
        snapshot["request_marker"] = request_marker
        snapshot["snapshot_ms"] = int(time.time() * 1000)
        if _initiator_snapshot_valid(snapshot, request_marker):
            entry["initiator_snapshot"] = snapshot
            persisted += 1
    merged_counts = next(iter(counts.values()), _normal_hook_counts({}, request_marker))
    merged_counts["snapshot_attempted"] = attempted
    merged_counts["snapshot_persisted"] = persisted
    merged_counts["snapshot_skipped"] = skipped
    return merged_counts


@mcp.tool()
async def network_capture(
    action: str = "status",
    url_pattern: str = "**/*",
    capture_body: bool = False,
    session_id: str | None = None,
    page_id: str | None = None,
    marker: str | None = None,
    aida_operation_id=None,
) -> dict:
    """Unified network capture control (v0.9.0).

    Replaces start_network_capture / stop_network_capture.

    Args:
        action:
          "start"  — begin capturing network events
          "stop"   — stop capturing (buffer retained)
          "clear"  — clear the capture buffer
          "status" — return current capture state
        url_pattern: Glob pattern for "start" (default "**/*" captures all).
        capture_body: For "start" only; capture response bodies (more memory).

    Returns:
        dict with action result + current status snapshot.
    """
    request_marker = str(marker or "")
    action_name = str(action or "status").strip().lower()
    if not _session_matches(session_id):
        return {"success": False, "status": "failed", "error": "session_id_mismatch", "session_id": _current_session_id(), "requested_session_id": str(session_id or "")}
    if action_name in ("", "capture", "state"):
        action_name = "status"
    hook_counts = {}
    if page_id:
        try:
            page = await browser_manager.resolve_page(page_id)
            hook_counts = await _aida_network_hook_counts(page, request_marker, False)
        except Exception as exc:
            return {
                "success": False,
                "status": "failed",
                "error": "browser_network_capture_page_not_found",
                "error_type": type(exc).__name__,
                "error_summary": _safe_text(exc, 500),
                "page_id": page_id,
                "active_page_id": getattr(browser_manager, "active_page_id", None),
                "marker": request_marker,
            }
    if action_name == "start":
        browser_manager._capturing = True
        browser_manager._capture_pattern = url_pattern
        browser_manager._capture_body = capture_body
        return {"status": "capturing", "pattern": url_pattern,
                "capture_body": capture_body, "page_id": page_id,
                "active_page_id": getattr(browser_manager, "active_page_id", None),
                "marker": request_marker, "hook_counts": hook_counts}
    elif action_name == "stop":
        browser_manager._capturing = False
        return {"status": "stopped",
                "total_requests": len(browser_manager._network_requests),
                "page_id": page_id,
                "active_page_id": getattr(browser_manager, "active_page_id", None),
                "marker": request_marker}
    elif action_name == "clear":
        if page_id or request_marker:
            before = len(browser_manager._network_requests)
            retained = [r for r in browser_manager._network_requests if not _request_matches_scope(r, page_id, request_marker)]
            browser_manager._network_requests.clear()
            browser_manager._network_requests.extend(retained)
            count = before - len(retained)
        else:
            count = len(browser_manager._network_requests)
            browser_manager._network_requests.clear()
            browser_manager._request_id_counter = 0
        return {"status": "cleared", "cleared_count": count,
                "page_id": page_id,
                "active_page_id": getattr(browser_manager, "active_page_id", None),
                "marker": request_marker, "hook_counts": hook_counts}
    elif action_name == "status":
        return {
            "active": browser_manager._capturing,
            "pattern": browser_manager._capture_pattern,
            "capture_body": browser_manager._capture_body,
            "buffer_size": len(browser_manager._network_requests),
            "page_id": page_id,
            "active_page_id": getattr(browser_manager, "active_page_id", None),
            "marker": request_marker,
            "hook_counts": hook_counts,
        }
    else:
        return {"error": f"unknown action: {action}. Use start/stop/clear/status"}


@mcp.tool()
async def list_network_requests(
    session_id: str | None = None,
    page_id: str | None = None,
    marker: str | None = None,
    filter: str | None = None,
    url_filter: str | None = None,
    url_prefix: str | None = None,
    url_contains_domain: str | None = None,
    method: str | None = None,
    resource_type: str | None = None,
    status_code: int | None = None,
    limit: int = _NETWORK_DEFAULT_LIMIT,
    offset: int = 0,
    include_initiator_snapshots: bool = True,
    include_body: bool = False,
    max_body_size: int = 5000,
    aida_operation_id=None,
) -> dict:
    """List captured network requests with optional filters.

    Args:
        url_filter: Substring filter for request URLs.
        url_contains_domain: Convenience domain filter (e.g. 'nmpa.gov.cn').
        method: HTTP method filter (e.g. "GET", "POST").
        resource_type: Resource type filter (e.g. "xhr", "fetch", "script", "document").
        status_code: HTTP status code filter.

    Returns:
        List of request summaries with id, url, method, status, type, ms, size.
    """
    try:
        if not _session_matches(session_id):
            return {"status": "ok", "requests": [], "count": 0, "returned_count": 0, "filtered_count": 0, "total_count": len(browser_manager._network_requests), "offset": 0, "limit": 0, "has_more": False, "session_id": _current_session_id(), "requested_session_id": str(session_id or "")}
        all_reqs = list(browser_manager._network_requests)
        reqs = list(all_reqs)
        if page_id:
            reqs = [r for r in reqs if str(r.get("page_id") or "") == str(page_id)]
        if marker:
            reqs = [r for r in reqs if _request_marker_matches(r, marker)]
        if filter:
            reqs = [r for r in reqs if _request_matches_text_filter(r, filter)]
        if url_filter:
            reqs = [r for r in reqs if url_filter in r.get("url", "")]
        if url_prefix:
            reqs = [r for r in reqs if str(r.get("url") or "").startswith(url_prefix)]
        if url_contains_domain:
            reqs = [r for r in reqs if url_contains_domain in r.get("url", "")]
        if method:
            reqs = [r for r in reqs if str(r.get("method") or "").upper() == method.upper()]
        if resource_type:
            reqs = [r for r in reqs if r.get("resource_type") == resource_type]
        if status_code is not None:
            reqs = [r for r in reqs if r.get("status") == status_code]

        filtered_count = len(reqs)
        safe_offset = _bounded_int(offset, 0, 0, filtered_count)
        safe_limit = _bounded_int(limit, _NETWORK_DEFAULT_LIMIT, 0, _NETWORK_MAX_LIMIT)
        paged_reqs = reqs[safe_offset:safe_offset + safe_limit] if safe_limit > 0 else []
        if include_body:
            body_tasks = []
            for r in paged_reqs:
                task = r.get("response_body_task")
                if isinstance(task, asyncio.Future) and not task.done():
                    body_tasks.append(task)
            if body_tasks:
                await asyncio.wait(body_tasks, timeout=2.0)

        effective_marker = str(marker or filter or url_filter or url_prefix or "")
        hook_counts = await _persist_initiator_snapshots(
            paged_reqs,
            effective_marker,
            min(50, len(paged_reqs)),
        ) if effective_marker and include_initiator_snapshots else {}
        summaries = []
        for r in paged_reqs:
            body_size = len(r.get("response_body")) if r.get("response_body") else 0
            snapshot = r.get("initiator_snapshot") if isinstance(r.get("initiator_snapshot"), dict) else {}
            request_id = r.get("request_id", r.get("id"))
            response_body_length = int(r.get("response_body_length") or r.get("body_length") or body_size or 0)
            request_body = r.get("request_body") or r.get("request_post_data") or r.get("post_data") or ""
            request_body_length = int(r.get("request_body_length") or len(request_body or ""))
            summary = {
                "id": r.get("id"), "request_id": request_id, "network_request_id": r.get("network_request_id", request_id),
                "page_id": r.get("page_id"), "context_id": r.get("context_id"),
                "url": str(r.get("url") or "")[:240], "method": r.get("method"),
                "status": r.get("status"), "status_code": r.get("status_code", r.get("status")),
                "type": r.get("resource_type"), "resource_type": r.get("resource_type"),
                "ms": r.get("duration"), "duration_ms": r.get("duration_ms", r.get("duration")),
                "size": response_body_length, "body_length": response_body_length,
                "request_body_length": request_body_length, "response_body_length": response_body_length,
                "request_headers": r.get("request_headers") or {},
                "response_headers": r.get("response_headers") or {},
                "timing": r.get("timing") if isinstance(r.get("timing"), dict) else {},
                "initiator": r.get("initiator") if isinstance(r.get("initiator"), dict) else {},
                "redirect_chain": r.get("redirect_chain") if isinstance(r.get("redirect_chain"), list) else [],
                "redirected_from": r.get("redirected_from", ""),
                "failed": bool(r.get("failed")),
                "failure": r.get("failure", ""),
                "websocket": bool(r.get("websocket") or r.get("resource_type") == "websocket"),
                "has_body": body_size > 0, "marker_matches": _request_marker_matches(r, effective_marker),
                "url_len": len(r.get("url", "")), "url_hash": _url_hash(r.get("url")),
                "initiator_snapshot": _initiator_snapshot_valid(snapshot, effective_marker),
                "initiator_source": _initiator_snapshot_source(snapshot),
                "initiator_stack_len": len(str(snapshot.get("stack") or "")),
                "response_body_available": r.get("response_body") is not None,
            }
            if include_body:
                response_body = r.get("response_body")
                bounded_body_size = _bounded_int(max_body_size, 5000, -1, 10_000_000)
                if request_body:
                    summary["request_body"] = request_body
                    summary["post_data"] = request_body
                if response_body is not None:
                    if bounded_body_size >= 0 and len(response_body) > bounded_body_size:
                        summary["response_body"] = response_body[:bounded_body_size]
                        summary["response_body_truncated"] = True
                        summary["response_body_original_size"] = len(response_body)
                        summary["response_body_size_returned"] = bounded_body_size
                    else:
                        summary["response_body"] = response_body
                        summary["response_body_truncated"] = False
                        summary["response_body_original_size"] = len(response_body)
                        summary["response_body_size_returned"] = len(response_body)
            summaries.append(summary)
        return {
            "status": "ok",
            "requests": summaries,
            "count": len(summaries),
            "returned_count": len(summaries),
            "filtered_count": filtered_count,
            "total_count": len(all_reqs),
            "offset": safe_offset,
            "limit": safe_limit,
            "has_more": safe_offset + len(summaries) < filtered_count,
            "page_id": page_id,
            "active_page_id": browser_manager.active_page_id,
            "page_count": len(browser_manager.pages),
            "marker": str(marker or ""),
            "filter": str(filter or ""),
            "url_prefix": str(url_prefix or ""),
            "effective_marker": effective_marker,
            "hook_counts": hook_counts,
            "capturing": browser_manager._capturing,
        }
    except Exception as e:
        return {"error": str(e)}


@mcp.tool()
async def get_network_request(
    request_id: int,
    session_id: str | None = None,
    page_id: str | None = None,
    marker: str | None = None,
    include_body: bool = False,
    include_headers: bool = True,
    max_body_size: int = 5000,
    aida_operation_id=None,
) -> dict:
    """Get full details of a specific captured network request.

    Args:
        request_id: The ID of the request (from list_network_requests).
        include_body: Include response body (default False).
        include_headers: Include request/response headers (default True).
        max_body_size: Max chars of body when include_body=True. Pass -1 for unlimited.

    Returns:
        dict with request and response details.
    """
    try:
        if not _session_matches(session_id):
            return {"error": "session_id_mismatch", "session_id": _current_session_id(), "requested_session_id": str(session_id or "")}
        for r in browser_manager._network_requests:
            if r["id"] == request_id and _request_matches_scope(r, page_id, marker):
                result = dict(r)
                result.pop("response_body_task", None)
                if not include_body:
                    body = result.pop("response_body", None)
                    result["response_body_available"] = body is not None
                    if body:
                        result["response_body_size"] = len(body)
                else:
                    body = result.get("response_body")
                    if body is not None and max_body_size >= 0 and len(body) > max_body_size:
                        result["response_body"] = body[:max_body_size]
                        result["response_body_truncated"] = True
                        result["response_body_original_size"] = len(body)
                        result["response_body_size_returned"] = max_body_size
                    elif body is not None:
                        result["response_body_truncated"] = False
                        result["response_body_original_size"] = len(body)
                        result["response_body_size_returned"] = len(body)
                if not include_headers:
                    result.pop("request_headers", None)
                    result.pop("response_headers", None)
                result["request_page_id"] = r.get("page_id")
                result["active_page_id"] = browser_manager.active_page_id
                result["request_marker"] = str(marker or "")
                return result
        return {"error": f"Request ID {request_id} not found"}
    except Exception as e:
        return {"error": str(e)}


@mcp.tool()
async def get_request_initiator(request_id: int, session_id: str | None = None, page_id: str | None = None, marker: str | None = None, aida_operation_id=None) -> dict:
    started = time.perf_counter()
    contract = "aida_initiator_contract_v2_page_marker"
    request_marker = str(marker or "")
    active_page_id = getattr(browser_manager, "active_page_id", None) or ""
    try:
        if not _session_matches(session_id):
            return {
                "initiator_contract": contract,
                "success": False,
                "status": "failed",
                "error": "session_id_mismatch",
                "request_id": request_id,
                "session_id": _current_session_id(),
                "requested_session_id": str(session_id or ""),
                "active_page_id": active_page_id,
            }
        target_entry = None
        id_match = None
        for entry in browser_manager._network_requests:
            if entry.get("id") == request_id:
                if id_match is None:
                    id_match = entry
                if _request_matches_scope(entry, page_id, request_marker):
                    target_entry = entry
                    break
        if target_entry is None:
            requested_page_id = page_id or (id_match.get("page_id") if isinstance(id_match, dict) else None)
            _camoufox_debug(
                "get_request_initiator_missing_request",
                request_id=int(request_id),
                requested_page_id=str(page_id or ""),
                request_marker_len=len(request_marker),
                id_seen=bool(id_match),
                captured_request_count=len(browser_manager._network_requests),
            )
            return {
                "initiator_contract": contract,
                "success": False,
                "status": "failed",
                "error": "browser_network_initiator_request_not_found",
                "request_id": request_id,
                "request_page_id": requested_page_id,
                "resolved_page_id": requested_page_id,
                "active_page_id": active_page_id,
                "request_marker": request_marker,
                "hook_counts_before": {},
                "hook_counts_after": {},
                "fetch_initiator_log_count": 0,
                "diagnostics": {
                    "requested_page_id": page_id,
                    "marker": request_marker,
                    "id_match_page_id": id_match.get("page_id") if isinstance(id_match, dict) else None,
                    "buffer_size": len(browser_manager._network_requests),
                },
            }

        req_url = target_entry.get("url", "")
        target_page_id = str(page_id or target_entry.get("page_id") or "")
        _camoufox_debug(
            "get_request_initiator_begin",
            request_id=int(request_id),
            target_page_id=target_page_id,
            active_page_id=str(active_page_id),
            request_marker_len=len(request_marker),
            request_url_len=len(req_url or ""),
            captured_request_count=len(browser_manager._network_requests),
        )
        persisted_snapshot = target_entry.get("initiator_snapshot") if isinstance(target_entry.get("initiator_snapshot"), dict) else None
        used_persisted_snapshot = _initiator_snapshot_valid(persisted_snapshot, request_marker)
        try:
            page = await browser_manager.resolve_page(target_page_id or None)
        except Exception as resolve_exc:
            if page_id:
                if used_persisted_snapshot:
                    result = dict(persisted_snapshot)
                    diagnostics = result.get("diagnostics") if isinstance(result.get("diagnostics"), dict) else {}
                    diagnostics = dict(diagnostics)
                    diagnostics["persisted_snapshot_used"] = True
                    diagnostics["page_resolve_failed"] = True
                    source = _initiator_snapshot_source(result)
                    return {
                        "initiator_contract": contract,
                        "success": True,
                        "status": "ok",
                        "request_id": request_id,
                        "matched_request_id": request_id,
                        "request_page_id": target_page_id,
                        "captured_page_id": target_page_id,
                        "resolved_page_id": target_page_id,
                        "active_page_id": active_page_id,
                        "hook_page_id": target_page_id,
                        "request_marker": request_marker,
                        "hook_counts_before": diagnostics,
                        "hook_counts_after": diagnostics,
                        "persisted_snapshot_used": True,
                        "fetch_log_count": int(diagnostics.get("fetch_log_count", 0) or 0),
                        "fetch_initiator_log_count": int(diagnostics.get("fetch_initiator_log_count", 0) or 0),
                        "xhr_log_count": int(diagnostics.get("xhr_log_count", 0) or 0),
                        "marker_fetch_log_count": int(diagnostics.get("marker_fetch_log_count", 0) or 0),
                        "marker_fetch_initiator_log_count": int(diagnostics.get("marker_fetch_initiator_log_count", 0) or 0),
                        "url": result.get("url") or req_url,
                        "request_url": req_url,
                        "initiator_stack": result.get("stack"),
                        "initiator_type": source,
                        "source": source,
                        "method": result.get("method"),
                        "request_headers": result.get("headers"),
                        "request_body": result.get("body"),
                        "diagnostics": diagnostics,
                        "request_url_len": len(req_url or ""),
                        "request_url_hash": _url_hash(req_url),
                        "unknown_source_diagnostics": None,
                    }
                _camoufox_debug(
                    "get_request_initiator_resolve_page_failed",
                    request_id=int(request_id),
                    target_page_id=target_page_id,
                    active_page_id=str(active_page_id),
                    error_type=type(resolve_exc).__name__,
                    error_len=len(str(resolve_exc)),
                )
                return {
                    "initiator_contract": contract,
                    "success": False,
                    "status": "failed",
                    "error": "browser_network_initiator_page_not_found",
                    "error_type": type(resolve_exc).__name__,
                    "error_summary": _safe_text(resolve_exc, 500),
                    "request_id": request_id,
                    "request_page_id": target_page_id,
                    "resolved_page_id": target_page_id,
                    "active_page_id": active_page_id,
                    "request_marker": request_marker,
                    "hook_counts_before": {},
                    "hook_counts_after": {},
                    "fetch_initiator_log_count": 0,
                }
            page = await browser_manager.get_active_page()
        resolved_page_id = browser_manager.page_id_for(page) or target_page_id
        page_url = ""
        try:
            page_url = page.url or ""
        except Exception:
            page_url = ""
        hook_counts_before = await _aida_network_hook_counts(page, request_marker, False)
        if used_persisted_snapshot:
            result = dict(persisted_snapshot)
            diagnostics_snapshot = result.get("diagnostics") if isinstance(result.get("diagnostics"), dict) else {}
            diagnostics_snapshot = dict(diagnostics_snapshot)
            diagnostics_snapshot["persisted_snapshot_used"] = True
            result["diagnostics"] = diagnostics_snapshot
        else:
            result = await _aida_find_request_initiator_on_page(page, req_url, request_marker)
            if _initiator_snapshot_valid(result, request_marker):
                snapshot = dict(result)
                snapshot["captured_page_id"] = target_page_id
                snapshot["request_url"] = req_url
                snapshot["request_marker"] = request_marker
                snapshot["snapshot_ms"] = int(time.time() * 1000)
                target_entry["initiator_snapshot"] = snapshot

        if not isinstance(result, dict):
            result = {"url": req_url, "stack": None, "type": "unknown", "diagnostics": {"result_type": type(result).__name__}}
        source = result.get("type") or result.get("source") or "unknown"
        diagnostics = result.get("diagnostics") if isinstance(result.get("diagnostics"), dict) else {}

        def diagnostic_count(name: str) -> int:
            try:
                return int(diagnostics.get(name, -1))
            except (TypeError, ValueError):
                return -1

        hook_counts_after = await _aida_network_hook_counts(page, request_marker, False)
        unknown_diagnostics = None
        if source in ("unknown", None):
            xhr_samples = _sample_url_diagnostics(diagnostics.get("xhr_sample_urls"))
            fetch_samples = _sample_url_diagnostics(diagnostics.get("fetch_sample_urls"))
            fetch_init_samples = _sample_url_diagnostics(diagnostics.get("fetch_initiator_sample_urls"))
            unknown_diagnostics = {
                "active_page_id": active_page_id,
                "matched_request_id": int(request_id),
                "hook_page_id": resolved_page_id,
                "captured_page_id": target_page_id,
                "request_marker": request_marker,
                "request_url_len": len(req_url or ""),
                "request_url_hash": _url_hash(req_url),
                "page_url_len": len(page_url),
                "page_url_hash": _url_hash(page_url),
                "xhr_hook_active": bool(diagnostics.get("xhr_hook_active", False)),
                "fetch_hook_active": bool(diagnostics.get("fetch_hook_active", False)),
                "xhr_log_count": diagnostic_count("xhr_log_count"),
                "fetch_log_count": diagnostic_count("fetch_log_count"),
                "fetch_initiator_log_count": diagnostic_count("fetch_initiator_log_count"),
                "marker_xhr_log_count": diagnostic_count("marker_xhr_log_count"),
                "marker_fetch_log_count": diagnostic_count("marker_fetch_log_count"),
                "marker_fetch_initiator_log_count": diagnostic_count("marker_fetch_initiator_log_count"),
                "xhr_sample_urls": xhr_samples["sample_urls"],
                "xhr_sample_url_lengths": xhr_samples["sample_url_lengths"],
                "xhr_sample_url_hashes": xhr_samples["sample_url_hashes"],
                "fetch_sample_urls": fetch_samples["sample_urls"],
                "fetch_sample_url_lengths": fetch_samples["sample_url_lengths"],
                "fetch_sample_url_hashes": fetch_samples["sample_url_hashes"],
                "fetch_initiator_sample_urls": fetch_init_samples["sample_urls"],
                "fetch_initiator_sample_url_lengths": fetch_init_samples["sample_url_lengths"],
                "fetch_initiator_sample_url_hashes": fetch_init_samples["sample_url_hashes"],
            }
            diagnostics["unknown_source"] = unknown_diagnostics
        success = source not in ("unknown", None) and bool(result.get("stack"))
        _camoufox_debug(
            "get_request_initiator_exit",
            request_id=int(request_id),
            target_page_id=str(target_page_id or ""),
            resolved_page_id=str(resolved_page_id),
            active_page_id=str(active_page_id),
            request_marker_len=len(request_marker),
            matched_request_id=int(request_id),
            hook_page_id=str(resolved_page_id),
            request_url_len=len(req_url or ""),
            request_url_hash=_url_hash(req_url),
            source=str(source or ""),
            stack_len=len(result.get("stack") or ""),
            page_url_len=len(page_url),
            page_url_hash=_url_hash(page_url),
            xhr_hook_active=bool(diagnostics.get("xhr_hook_active", False)),
            fetch_hook_active=bool(diagnostics.get("fetch_hook_active", False)),
            xhr_log_count=diagnostic_count("xhr_log_count"),
            fetch_log_count=diagnostic_count("fetch_log_count"),
            fetch_initiator_log_count=diagnostic_count("fetch_initiator_log_count"),
            elapsed_ms=int((time.perf_counter() - started) * 1000),
        )
        out = {
            "initiator_contract": contract,
            "success": success,
            "status": "ok" if success else "failed",
            "request_id": request_id,
            "matched_request_id": request_id,
            "request_page_id": target_page_id,
            "captured_page_id": target_page_id,
            "resolved_page_id": resolved_page_id,
            "active_page_id": active_page_id,
            "hook_page_id": resolved_page_id,
            "request_marker": request_marker,
            "hook_counts_before": hook_counts_before,
            "hook_counts_after": hook_counts_after,
            "persisted_snapshot_used": used_persisted_snapshot,
            "fetch_log_count": diagnostic_count("fetch_log_count") if diagnostic_count("fetch_log_count") >= 0 else hook_counts_after.get("fetch_log_count", 0),
            "fetch_initiator_log_count": diagnostic_count("fetch_initiator_log_count") if diagnostic_count("fetch_initiator_log_count") >= 0 else hook_counts_after.get("fetch_initiator_log_count", 0),
            "xhr_log_count": diagnostic_count("xhr_log_count") if diagnostic_count("xhr_log_count") >= 0 else hook_counts_after.get("xhr_log_count", 0),
            "marker_fetch_log_count": diagnostic_count("marker_fetch_log_count") if diagnostic_count("marker_fetch_log_count") >= 0 else hook_counts_after.get("marker_fetch_log_count", 0),
            "marker_fetch_initiator_log_count": diagnostic_count("marker_fetch_initiator_log_count") if diagnostic_count("marker_fetch_initiator_log_count") >= 0 else hook_counts_after.get("marker_fetch_initiator_log_count", 0),
            "url": result.get("url") or req_url,
            "request_url": req_url,
            "initiator_stack": result.get("stack"),
            "initiator_type": source,
            "source": source,
            "method": result.get("method"),
            "request_headers": result.get("headers"),
            "request_body": result.get("body"),
            "diagnostics": diagnostics,
            "request_url_len": len(req_url or ""),
            "request_url_hash": _url_hash(req_url),
            "unknown_source_diagnostics": unknown_diagnostics,
        }
        if not success:
            out["error"] = "browser_network_initiator_unknown_source" if source in ("unknown", None) else "browser_network_initiator_stack_missing"
            out["diagnostic"] = {
                "likely_causes": [
                    "hook registered after SDK",
                    "request made inside a sync-loaded SDK interceptor",
                    "fetch hook was not injected before the request",
                    "request marker was not present in hook logs",
                ],
                "recommended_action": "Use reload_with_hooks or inject hooks before navigate.",
            }
        return out
    except Exception as e:
        _camoufox_debug(
            "get_request_initiator_exit",
            request_id=int(request_id),
            success=False,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            error_type=type(e).__name__,
            error_len=len(str(e)),
        )
        return {
            "initiator_contract": contract,
            "success": False,
            "status": "failed",
            "error": "browser_network_initiator_exception",
            "error_type": type(e).__name__,
            "error_summary": _safe_text(e, 500),
            "request_id": request_id,
            "request_page_id": page_id,
            "resolved_page_id": page_id,
            "active_page_id": active_page_id,
            "request_marker": request_marker,
            "hook_counts_before": {},
            "hook_counts_after": {},
            "fetch_initiator_log_count": 0,
        }


@mcp.tool()
async def search_network_bodies(
    query: str | None = None,
    search_query: str | None = None,
    body_search: str | None = None,
    body_sha256: str | None = None,
    scope: str = "both",
    case_sensitive: bool = False,
    session_id: str | None = None,
    page_id: str | None = None,
    marker: str | None = None,
    url_filter: str | None = None,
    url_prefix: str | None = None,
    url_contains_domain: str | None = None,
    method: str | None = None,
    resource_type: str | None = None,
    status_code: int | None = None,
    header_contains: dict | str | None = None,
    max_body_size: int = _NETWORK_BODY_SEARCH_MAX_BYTES,
    max_matches: int = 100,
    offset: int = 0,
    snippet_chars: int = 240,
    hash_only: bool = False,
    wait_for_bodies_ms: int = 2000,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    needle = str(body_search or search_query or query or "").strip()
    expected_sha256 = str(body_sha256 or "").strip().lower()
    if not needle and not expected_sha256:
        return {"status": "failed", "error": "query, search_query, body_search, or body_sha256 is required"}
    if not _session_matches(session_id):
        return {"status": "ok", "query_present": bool(needle), "body_sha256_present": bool(expected_sha256), "matched_count": 0, "returned_count": 0, "matches": [], "session_id": _current_session_id(), "requested_session_id": str(session_id or ""), "redacted": True}
    safe_max_body = _bounded_int(max_body_size, _NETWORK_BODY_SEARCH_MAX_BYTES, 0, _NETWORK_BODY_SEARCH_MAX_BYTES)
    safe_limit = _bounded_int(max_matches, 100, 0, 500)
    safe_offset = _bounded_int(offset, 0, 0, 1_000_000)
    wait_ms = _bounded_int(wait_for_bodies_ms, 2000, 0, 5000)
    candidates = [entry for entry in list(browser_manager._network_requests) if isinstance(entry, dict)]
    body_tasks = []
    for entry in candidates:
        task = entry.get("response_body_task")
        if isinstance(task, asyncio.Future) and not task.done():
            body_tasks.append(task)
    if body_tasks and wait_ms > 0:
        await asyncio.wait(body_tasks, timeout=wait_ms / 1000.0)
    filtered: list[dict] = []
    lower_method = str(method or "").upper()
    for entry in candidates:
        if not _request_matches_scope(entry, page_id, marker):
            continue
        url = str(entry.get("url") or "")
        if url_filter and url_filter not in url:
            continue
        if url_prefix and not url.startswith(url_prefix):
            continue
        if url_contains_domain and url_contains_domain not in url:
            continue
        if lower_method and str(entry.get("method") or "").upper() != lower_method:
            continue
        if resource_type and str(entry.get("resource_type") or entry.get("type") or "") != str(resource_type):
            continue
        if status_code is not None and _bounded_int(entry.get("status"), -1, -1, 999) != int(status_code):
            continue
        headers = _lower_header_map(entry.get("request_headers"))
        response_headers = _lower_header_map(entry.get("response_headers"))
        if header_contains is not None and not (_headers_contain(headers, header_contains, case_sensitive) or _headers_contain(response_headers, header_contains, case_sensitive)):
            continue
        filtered.append(entry)
    matches: list[dict] = []
    matched_total = 0
    considered_bodies = 0
    for entry in filtered:
        body_hits: list[dict] = []
        for body_scope, value in (("request", _request_body_value(entry)), ("response", _response_body_value(entry))):
            if not _scope_includes(scope, body_scope):
                continue
            body = _bounded_body_text(value, safe_max_body)
            considered_bodies += 1
            match_index = _body_match(body["text"], needle, case_sensitive) if needle else -1
            hash_match = bool(expected_sha256 and body["sha256"].lower() == expected_sha256)
            if match_index < 0 and not hash_match:
                continue
            hit = {
                "scope": body_scope,
                "body_sha256": body["sha256"],
                "searched_bytes": body["searched_bytes"],
                "original_bytes": body["original_bytes"],
                "truncated": body["truncated"],
                "match_index": match_index,
                "hash_match": hash_match,
            }
            if not hash_only and match_index >= 0:
                hit["snippet"] = _body_snippet(body["text"], match_index, snippet_chars)
            body_hits.append(hit)
        if not body_hits:
            continue
        matched_total += 1
        if matched_total <= safe_offset:
            continue
        if len(matches) >= safe_limit:
            continue
        request_id = entry.get("request_id", entry.get("id"))
        matches.append({
            "id": entry.get("id"),
            "request_id": request_id,
            "network_request_id": entry.get("network_request_id", request_id),
            "page_id": entry.get("page_id"),
            "context_id": entry.get("context_id"),
            "method": entry.get("method"),
            "resource_type": entry.get("resource_type", entry.get("type")),
            "status": entry.get("status"),
            "status_code": entry.get("status_code", entry.get("status")),
            "url": str(entry.get("url") or "")[:240],
            "url_len": len(str(entry.get("url") or "")),
            "url_hash": _url_hash(entry.get("url")),
            "body_matches": body_hits,
        })
    return {
        "status": "ok",
        "query_present": bool(needle),
        "body_sha256_present": bool(expected_sha256),
        "scope": scope,
        "case_sensitive": bool(case_sensitive),
        "redacted": True,
        "hash_only": bool(hash_only),
        "max_body_size": safe_max_body,
        "considered_requests": len(filtered),
        "considered_bodies": considered_bodies,
        "matched_count": matched_total,
        "returned_count": len(matches),
        "offset": safe_offset,
        "limit": safe_limit,
        "has_more": matched_total > safe_offset + len(matches),
        "matches": matches,
        "elapsed_ms": int((time.perf_counter() - started) * 1000),
    }


@mcp.tool()
async def intercept_request(
    url_pattern: str,
    action: str = "log",
    modify_headers: dict | None = None,
    modify_body: str | None = None,
    mock_response: dict | None = None,
    conditions: dict | None = None,
    intercept_action: str | None = None,
    delay_ms: int = 0,
    max_delay_ms: int = 5000,
    search_query: str | None = None,
    body_search: str | None = None,
    body_sha256: str | None = None,
    header_contains: dict | str | None = None,
    response_header_contains: dict | str | None = None,
    request_body_contains: str | None = None,
    response_body_contains: str | None = None,
    url_contains: str | None = None,
    url_prefix: str | None = None,
    url_contains_domain: str | None = None,
    method: str | None = None,
    resource_type: str | None = None,
    status_code: int | None = None,
    search_scope: str = "both",
    case_sensitive: bool = False,
    max_body_size: int = _NETWORK_BODY_SEARCH_MAX_BYTES,
    session_id: str | None = None,
    page_id: str | None = None,
    initiator_contains: str | None = None,
    initiator_url_contains: str | None = None,
    frame_url_contains: str | None = None,
    frame_url_prefix: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    try:
        if not _session_matches(session_id):
            return {"status": "failed", "error": "session_id_mismatch", "session_id": _current_session_id(), "requested_session_id": str(session_id or "")}
        page = await browser_manager.resolve_page_for_operation(page_id, "intercept_request", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
        active_session_id = _current_session_id()
        selected_action = str(intercept_action or action or "log").strip().lower()
        if selected_action in ("", "intercept", "conditional_intercept"):
            selected_action = "log"
        if selected_action == "stop":
            pattern = url_pattern or "**/*"
            if url_pattern:
                await page.unroute(pattern)
                browser_manager._route_handlers.pop(f"network:{resolved_page_id}:{pattern}", None)
                return {"status": "stopped", "pattern": pattern, "page_id": resolved_page_id}
            await page.unroute("**/*")
            keys = [key for key in browser_manager._route_handlers if str(key).startswith(f"network:{resolved_page_id}:")]
            for key in keys:
                browser_manager._route_handlers.pop(key, None)
            return {"status": "stopped_all", "page_id": resolved_page_id}

        allowed = {"log", "delay", "block", "modify", "mock"}
        if selected_action not in allowed:
            return {"status": "failed", "error": f"unsupported intercept action: {selected_action}", "available_actions": sorted(allowed | {"stop"})}
        if not url_pattern:
            return {"status": "failed", "error": "url_pattern is required"}

        predicate = _normal_conditions(conditions, {
            "search_query": search_query,
            "body_search": body_search,
            "body_sha256": body_sha256,
            "header_contains": header_contains,
            "response_header_contains": response_header_contains,
            "request_body_contains": request_body_contains,
            "response_body_contains": response_body_contains,
            "url_contains": url_contains,
            "url_prefix": url_prefix,
            "url_contains_domain": url_contains_domain,
            "method": method,
            "resource_type": resource_type,
            "status_code": status_code,
            "search_scope": search_scope,
            "page_id": page_id,
            "session_id": session_id,
            "initiator_contains": initiator_contains,
            "initiator_url_contains": initiator_url_contains,
            "frame_url_contains": frame_url_contains,
            "frame_url_prefix": frame_url_prefix,
        })
        safe_max_body = _bounded_int(max_body_size, _NETWORK_BODY_SEARCH_MAX_BYTES, 0, _NETWORK_BODY_SEARCH_MAX_BYTES)
        safe_max_delay = _bounded_int(max_delay_ms, 5000, 0, _NETWORK_MAX_DELAY_MS)
        safe_delay = _bounded_int(delay_ms, 0, 0, safe_max_delay)
        needs_response = _intercept_needs_response(predicate)
        if needs_response and selected_action == "modify":
            return {"status": "failed", "error": "response predicates cannot be combined with request-body/header modification"}

        async def handler(route):
            try:
                response = None
                response_body_bytes = None
                matched = _route_request_matches(route.request, predicate, safe_max_body, bool(case_sensitive), resolved_page_id, active_session_id)
                if not matched["matched"]:
                    await route.continue_()
                    return
                if needs_response:
                    response = await route.fetch()
                    if _condition_present(predicate, "response_body_contains", "response_body_sha256", "body_search", "search_query", "query", "body_sha256"):
                        response_body_bytes = await response.body()
                    response_match = _route_response_matches(response, response_body_bytes or b"", predicate, safe_max_body, bool(case_sensitive), bool(matched.get("generic_body_matched")))
                    if not response_match["matched"]:
                        await _fulfill_original(route, response, response_body_bytes)
                        return
                    matched.update(response_match)
                applied_delay = await _bounded_delay(safe_delay, safe_max_delay)
                if selected_action == "log":
                    browser_manager._console_logs.append({
                        "level": "info",
                        "text": f"[INTERCEPT:log] {route.request.method} {route.request.url}",
                        "timestamp": time.time() * 1000,
                        "location": None,
                        "page_id": resolved_page_id,
                        "predicate": matched,
                    })
                    if response is not None:
                        await _fulfill_original(route, response, response_body_bytes)
                    else:
                        await route.continue_()
                elif selected_action == "delay":
                    browser_manager._console_logs.append({
                        "level": "info",
                        "text": f"[INTERCEPT:delay:{applied_delay}ms] {route.request.method} {route.request.url}",
                        "timestamp": time.time() * 1000,
                        "location": None,
                        "page_id": resolved_page_id,
                        "predicate": matched,
                    })
                    if response is not None:
                        await _fulfill_original(route, response, response_body_bytes)
                    else:
                        await route.continue_()
                elif selected_action == "block":
                    if response is not None:
                        await route.fulfill(status=451, headers={"content-type": "text/plain; charset=utf-8"}, body="blocked by AiDA conditional network intercept")
                        return
                    await route.abort()
                elif selected_action == "modify":
                    overrides = {}
                    if modify_headers:
                        overrides["headers"] = {**dict(route.request.headers), **modify_headers}
                    if modify_body is not None:
                        overrides["post_data"] = modify_body
                    await route.continue_(**overrides)
                elif selected_action == "mock":
                    resp = mock_response or {}
                    await route.fulfill(
                        status=int(resp.get("status", 200)),
                        headers=dict(resp.get("headers") or {"content-type": "application/json"}),
                        body=str(resp.get("body", "{}")),
                    )
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                browser_manager._console_logs.append({
                    "level": "error",
                    "text": f"[INTERCEPT:error] {type(exc).__name__}: {_safe_text(exc, 300)}",
                    "timestamp": time.time() * 1000,
                    "location": None,
                    "page_id": resolved_page_id,
                })
                try:
                    await route.continue_()
                except Exception:
                    try:
                        await route.abort()
                    except Exception:
                        return

        await page.route(url_pattern, handler)
        browser_manager._route_handlers[f"network:{resolved_page_id}:{url_pattern}"] = handler
        return {
            "status": "intercepting",
            "pattern": url_pattern,
            "action": selected_action,
            "page_id": resolved_page_id,
            "session_id": active_session_id,
            "predicate_keys": sorted(predicate.keys()),
            "needs_response": needs_response,
            "delay_ms": safe_delay,
            "max_delay_ms": safe_max_delay,
            "max_body_size": safe_max_body,
            "case_sensitive": bool(case_sensitive),
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
        }
    except Exception as e:
        return {"error": str(e), "error_type": type(e).__name__}
