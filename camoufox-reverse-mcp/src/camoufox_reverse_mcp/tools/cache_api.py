from __future__ import annotations

import hashlib
import re
import time
from typing import Any

from ..browser import _await_no_cancel_wait, _camoufox_debug, _safe_text
from ..server import mcp, browser_manager


_MAX_CACHE_NAME_LEN = 512
_MAX_URL_LEN = 8192
_MAX_BODY_LIMIT = 1048576
_SENSITIVE_HEADER_PARTS = (
    "authorization",
    "cookie",
    "set-cookie",
    "token",
    "secret",
    "x-api-key",
    "apikey",
    "api-key",
    "session",
)
_JWT_RE = re.compile(r"\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b")
_BEARER_RE = re.compile(r"(?i)\bbearer\s+[A-Za-z0-9._~+/=-]{12,}")
_ASSIGN_SECRET_RE = re.compile(r"(?i)\b(password|passwd|pwd|secret|token|session|api[_-]?key|authorization)\s*[:=]\s*['\"]?[^'\"\s&;,)}]{4,}")


_CACHE_JS = r"""
async args => {
    const action = String(args.action || 'list_caches');
    const cacheName = String(args.cacheName || '');
    const url = String(args.url || '');
    const limit = Math.max(1, Math.min(Number(args.limit || 100), 1000));
    const bodyLimit = Math.max(0, Math.min(Number(args.bodySizeLimit || 65536), 1048576));
    const includeBody = !!args.includeBody;
    function headersObject(headers) {
        const out = {};
        try {
            headers.forEach((value, key) => out[key] = value);
        } catch(e) {}
        return out;
    }
    function hashText(value) {
        const text = String(value ?? '');
        let h = 2166136261;
        for (let i = 0; i < text.length; i++) {
            h ^= text.charCodeAt(i);
            h = Math.imul(h, 16777619) >>> 0;
        }
        return 'fnv32:' + h.toString(16).padStart(8, '0');
    }
    async function readBody(response) {
        if (!response) return {body: '', bytes_read: 0, truncated: false, hash: ''};
        const clone = response.clone();
        const reader = clone.body && clone.body.getReader ? clone.body.getReader() : null;
        if (!reader) {
            const text = await clone.text();
            return {body: text.slice(0, bodyLimit), bytes_read: text.length, truncated: text.length > bodyLimit, hash: hashText(text)};
        }
        const chunks = [];
        let total = 0;
        let truncated = false;
        while (true) {
            const item = await reader.read();
            if (item.done) break;
            const chunk = item.value || new Uint8Array();
            const remaining = bodyLimit - total;
            if (remaining > 0) chunks.push(chunk.slice(0, remaining));
            total += chunk.byteLength;
            if (total > bodyLimit) {
                truncated = true;
                try { await reader.cancel(); } catch(e) {}
                break;
            }
        }
        let kept = 0;
        for (const chunk of chunks) kept += chunk.byteLength;
        const merged = new Uint8Array(kept);
        let offset = 0;
        for (const chunk of chunks) {
            merged.set(chunk, offset);
            offset += chunk.byteLength;
        }
        const text = new TextDecoder('utf-8', {fatal: false}).decode(merged);
        return {body: text, bytes_read: total, bytes_returned: kept, truncated, hash: hashText(text)};
    }
    if (!('caches' in window)) return {supported: false, error: 'Cache Storage API is not available'};
    if (action === 'list_caches') {
        const names = await caches.keys();
        return {supported: true, caches: names, count: names.length};
    }
    if (!cacheName) return {supported: true, error: 'cache_name is required'};
    const cache = await caches.open(cacheName);
    if (action === 'list_entries') {
        const requests = await cache.keys();
        const entries = [];
        for (const request of requests.slice(0, limit)) {
            const response = await cache.match(request);
            const headers = response ? headersObject(response.headers) : {};
            const contentLength = headers['content-length'] || headers['Content-Length'] || '';
            const entry = {
                url: request.url,
                method: request.method,
                request_headers: headersObject(request.headers),
                status: response ? response.status : null,
                statusText: response ? response.statusText : '',
                response_headers: headers,
                content_length: contentLength,
                body_size: contentLength && !Number.isNaN(Number(contentLength)) ? Number(contentLength) : null
            };
            if (includeBody && response) entry.body = await readBody(response);
            entries.push(entry);
        }
        return {supported: true, cache_name: cacheName, entries, count: entries.length, total_count: requests.length, truncated: requests.length > entries.length};
    }
    if (action === 'read_entry') {
        if (!url) return {supported: true, error: 'url is required'};
        const response = await cache.match(url);
        if (!response) return {supported: true, found: false, cache_name: cacheName, url};
        const out = {
            supported: true,
            found: true,
            cache_name: cacheName,
            url,
            status: response.status,
            statusText: response.statusText,
            headers: headersObject(response.headers)
        };
        if (includeBody || !Object.prototype.hasOwnProperty.call(args, 'includeBody')) out.body = await readBody(response);
        return out;
    }
    if (action === 'delete_entry') {
        if (!url) return {supported: true, error: 'url is required'};
        const deleted = await cache.delete(url);
        return {supported: true, cache_name: cacheName, url, deleted};
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


def _require_name(value: Any, name: str, max_len: int) -> str:
    text = str(value or "").strip()
    if not text:
        raise ValueError(f"{name} is required")
    if len(text) > max_len:
        raise ValueError(f"{name} exceeds {max_len} characters")
    return text


def _hash_text(value: Any) -> str:
    text = str(value if value is not None else "")
    return hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()


def _sensitive_header(name: Any) -> bool:
    low = str(name or "").strip().lower()
    return any(part in low for part in _SENSITIVE_HEADER_PARTS)


def _header_value(value: Any, name: Any) -> Any:
    text = str(value if value is not None else "")
    if _sensitive_header(name):
        return {"redacted": True, "length": len(text), "sha256": _hash_text(text)}
    return text if len(text) <= 2048 else {"length": len(text), "sha256": _hash_text(text), "preview": text[:2048], "truncated": True}


def _sanitize_headers(headers: Any) -> dict[str, Any]:
    if not isinstance(headers, dict):
        return {}
    return {str(key): _header_value(value, key) for key, value in headers.items()}


def _sanitize_url(value: Any) -> str:
    text = str(value or "")
    text = re.sub(r"(?i)([?&](?:token|session|secret|api[_-]?key|password|authorization)=)[^&#]+", r"\1<redacted>", text)
    text = _JWT_RE.sub("<redacted:jwt>", text)
    return text[:_MAX_URL_LEN]


def _sanitize_body(body: Any) -> dict[str, Any]:
    data = body if isinstance(body, dict) else {}
    text = str(data.get("body") or "")
    redacted = _JWT_RE.sub("<redacted:jwt>", text)
    redacted = _BEARER_RE.sub("Bearer <redacted>", redacted)
    redacted = _ASSIGN_SECRET_RE.sub(lambda m: f"{m.group(1)}=<redacted>", redacted)
    return {
        "body": redacted,
        "body_length": int(data.get("bytes_read") or len(text)),
        "body_returned": len(redacted),
        "body_sha256": _hash_text(text),
        "truncated": bool(data.get("truncated")),
        "redacted": redacted != text,
        "hash": data.get("hash", ""),
    }


def _sanitize_result(result: dict[str, Any]) -> dict[str, Any]:
    if "entries" in result and isinstance(result.get("entries"), list):
        entries = []
        for entry in result["entries"]:
            if not isinstance(entry, dict):
                continue
            item = dict(entry)
            item["url"] = _sanitize_url(item.get("url"))
            item["request_headers"] = _sanitize_headers(item.get("request_headers"))
            item["response_headers"] = _sanitize_headers(item.get("response_headers"))
            entries.append(item)
        result["entries"] = entries
    if "url" in result:
        result["url"] = _sanitize_url(result.get("url"))
    if "headers" in result:
        result["headers"] = _sanitize_headers(result.get("headers"))
    if "body" in result:
        result.update(_sanitize_body(result.pop("body")))
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
async def browser_cache(
    action: str = "list_caches",
    cache_name: str | None = None,
    url: str | None = None,
    include_body: bool | None = None,
    limit: int = 100,
    body_size_limit: int = 65536,
    timeout_ms: int = 30000,
    payload: dict | None = None,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    selected_action = str(_payload_value(payload, "action", action) or "list_caches").strip().lower()
    aliases = {
        "list": "list_caches",
        "caches": "list_caches",
        "entries": "list_entries",
        "read": "read_entry",
        "delete": "delete_entry",
        "remove": "delete_entry",
    }
    selected_action = aliases.get(selected_action, selected_action)
    effective_page_id = _payload_value(payload, "page_id", page_id)
    timeout = _bounded_int(_payload_value(payload, "timeout_ms", timeout_ms), 30000, 250, 120000)
    try:
        effective_cache_name = ""
        effective_url = ""
        if selected_action != "list_caches":
            effective_cache_name = _require_name(_payload_value(payload, "cache_name", cache_name), "cache_name", _MAX_CACHE_NAME_LEN)
        if selected_action in {"read_entry", "delete_entry"}:
            effective_url = _require_name(_payload_value(payload, "url", url), "url", _MAX_URL_LEN)
        page = await browser_manager.resolve_page_for_operation(effective_page_id, f"browser_cache:{selected_action}", True, aida_operation_id)
        result = await _await_no_cancel_wait(page.evaluate(_CACHE_JS, {
            "action": selected_action,
            "cacheName": effective_cache_name,
            "url": effective_url,
            "limit": _bounded_int(_payload_value(payload, "limit", limit), 100, 1, 1000),
            "includeBody": bool(_payload_value(payload, "include_body", include_body)) if _payload_value(payload, "include_body", include_body) is not None else selected_action == "read_entry",
            "bodySizeLimit": _bounded_int(_payload_value(payload, "body_size_limit", body_size_limit), 65536, 0, _MAX_BODY_LIMIT),
        }), timeout=timeout / 1000.0)
        result = _sanitize_result(result if isinstance(result, dict) else {"error": f"unexpected result type: {type(result).__name__}"})
        if result.get("error"):
            out = await _base(selected_action, page, "error", False)
        elif result.get("supported", True) is False:
            out = await _base(selected_action, page, "unavailable", False)
        elif selected_action == "read_entry" and result.get("found") is False:
            out = await _base(selected_action, page, "not_found", False)
        else:
            out = await _base(selected_action, page, "ok", True)
        out.update(result)
        out["elapsed_ms"] = int((time.perf_counter() - started) * 1000)
        return out
    except Exception as exc:
        _camoufox_debug("browser_cache_error", action=selected_action, page_id=str(effective_page_id or ""), error_type=type(exc).__name__, error_summary=_safe_text(exc, 700))
        return _error(selected_action, exc, str(effective_page_id or ""))
