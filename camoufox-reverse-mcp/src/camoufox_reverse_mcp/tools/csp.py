from __future__ import annotations

import hashlib
import html as html_mod
import re
import time
from typing import Any

from ..browser import _await_no_cancel_wait, _camoufox_debug, _safe_text
from ..server import browser_manager, mcp


_CSP_HOOK_NAME = "aida:csp"
_CSP_HOOK = r"""
(() => {
  const existing = window.__aida_csp_debug;
  if (existing && existing.version === 1 && existing.installed) return existing.snapshot();
  const state = existing || { version: 1, installed: false, violations: [], maxViolations: 300, installedAt: Date.now() };
  state.version = 1;
  function trim() {
    while (state.violations.length > state.maxViolations) state.violations.shift();
  }
  function short(value, limit) {
    const text = String(value == null ? "" : value);
    return text.slice(0, limit || 1000);
  }
  window.addEventListener("securitypolicyviolation", event => {
    state.violations.push({
      timestamp: Date.now(),
      documentURI: short(event.documentURI, 1000),
      referrer: short(event.referrer, 1000),
      blockedURI: short(event.blockedURI, 1000),
      violatedDirective: short(event.violatedDirective, 300),
      effectiveDirective: short(event.effectiveDirective, 300),
      originalPolicy: short(event.originalPolicy, 2000),
      disposition: short(event.disposition, 80),
      sourceFile: short(event.sourceFile, 1000),
      statusCode: event.statusCode || 0,
      lineNumber: event.lineNumber || 0,
      columnNumber: event.columnNumber || 0,
      sample: short(event.sample, 500)
    });
    trim();
  }, true);
  state.installed = true;
  state.snapshot = function() {
    return { installed: true, installedAt: state.installedAt, violation_count: state.violations.length, violations: state.violations.slice() };
  };
  window.__aida_csp_debug = state;
  return state.snapshot();
})()
"""


_META_TAG_RE = re.compile(r"<meta\b[^>]*>", re.IGNORECASE)
_ATTR_RE = re.compile(r"([A-Za-z_:][A-Za-z0-9_:.-]*)\s*=\s*(\"[^\"]*\"|'[^']*'|[^\s>]+)", re.IGNORECASE)
_NONCE_TOKEN_RE = re.compile(r"(?i)'nonce-[^'\s;]+'|nonce-[^\s;]+")


def _sanitize_csp_token(token: str) -> str:
    text = str(token or "")
    low = text.lower().strip("'")
    if not low.startswith("nonce-"):
        return text
    digest = hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()[:16]
    return f"'nonce-<redacted:{len(text)}:{digest}>'"


def _sanitize_csp_policy_text(value: str) -> str:
    return _NONCE_TOKEN_RE.sub(lambda match: _sanitize_csp_token(match.group(0)), str(value or ""))


def _parse_csp_value(value: str) -> dict:
    directives: dict[str, list[str]] = {}
    for raw_part in str(value or "").split(";"):
        part = raw_part.strip()
        if not part:
            continue
        pieces = part.split()
        name = pieces[0].lower()
        tokens = [_sanitize_csp_token(token) for token in pieces[1:]]
        directives[name] = tokens
    def values(*names: str) -> list[str]:
        for name in names:
            if name in directives:
                return directives[name]
        return []
    return {
        "directives": directives,
        "scriptSources": values("script-src", "default-src"),
        "styleSources": values("style-src", "default-src"),
        "imgSources": values("img-src", "default-src"),
        "connectSources": values("connect-src", "default-src"),
        "defaultSources": directives.get("default-src", []),
        "reportUris": directives.get("report-uri", []) + directives.get("report-to", []),
    }


def _header_iter(headers: Any) -> list[tuple[str, str]]:
    if isinstance(headers, dict):
        return [(str(name or ""), str(value or "")) for name, value in headers.items()]
    if isinstance(headers, list):
        rows: list[tuple[str, str]] = []
        for item in headers:
            if isinstance(item, dict):
                rows.append((str(item.get("name") or item.get("key") or ""), str(item.get("value") or "")))
            elif isinstance(item, (list, tuple)) and len(item) >= 2:
                rows.append((str(item[0] or ""), str(item[1] or "")))
        return rows
    return []


def _meta_attrs(tag: str) -> dict[str, str]:
    attrs: dict[str, str] = {}
    for name, raw_value in _ATTR_RE.findall(tag):
        value = str(raw_value or "")
        if len(value) >= 2 and value[0] in {"'", '"'} and value[-1] == value[0]:
            value = value[1:-1]
        attrs[name.lower()] = html_mod.unescape(value)
    return attrs


def _direct_policy_rows(headers: Any, html_text: str | None, url: str | None) -> list[dict]:
    rows: list[dict] = []
    source_url = str(url or "")
    for name, value in _header_iter(headers):
        lowered = name.lower()
        if lowered in {"content-security-policy", "content-security-policy-report-only"}:
            text = str(value or "")
            rows.append({
                "source": "provided_header",
                "header": lowered,
                "url": source_url,
                "policy": _sanitize_csp_policy_text(text),
                "report_only": lowered.endswith("report-only"),
                **_parse_csp_value(text),
            })
    for index, tag in enumerate(_META_TAG_RE.findall(str(html_text or ""))):
        attrs = _meta_attrs(tag)
        if attrs.get("http-equiv", "").lower() != "content-security-policy":
            continue
        text = attrs.get("content", "")
        rows.append({"source": "provided_html_meta", "index": index, "url": source_url, "policy": _sanitize_csp_policy_text(text), "report_only": False, **_parse_csp_value(text)})
    return rows


def _collect_header_policies(page_id: str | None) -> list[dict]:
    rows: list[dict] = []
    for entry in list(getattr(browser_manager, "_network_requests", [])):
        if page_id and str(entry.get("page_id") or "") != str(page_id):
            continue
        headers = entry.get("response_headers") if isinstance(entry.get("response_headers"), dict) else {}
        for name, value in headers.items():
            lowered = str(name or "").lower()
            if lowered in {"content-security-policy", "content-security-policy-report-only"}:
                text = str(value or "")
                parsed = _parse_csp_value(text)
                rows.append({
                    "source": "network_header",
                    "header": lowered,
                    "url": entry.get("url", ""),
                    "request_id": entry.get("request_id") or entry.get("id"),
                    "policy": _sanitize_csp_policy_text(text),
                    "report_only": lowered.endswith("report-only"),
                    **parsed,
                })
    return rows


async def _meta_policies(page: Any) -> list[dict]:
    rows = await _await_no_cancel_wait(page.evaluate("""() => {
      return Array.from(document.querySelectorAll("meta[http-equiv]")).map((node, index) => ({
        index,
        httpEquiv: node.getAttribute("http-equiv") || "",
        content: node.getAttribute("content") || ""
      })).filter(item => item.httpEquiv.toLowerCase() === "content-security-policy");
    }"""), timeout=3.0)
    out: list[dict] = []
    for item in rows if isinstance(rows, list) else []:
        text = str((item or {}).get("content") or "")
        parsed = _parse_csp_value(text)
        out.append({"source": "meta", "index": (item or {}).get("index"), "policy": _sanitize_csp_policy_text(text), "report_only": False, **parsed})
    return out


def _merge_policies(rows: list[dict]) -> dict:
    merged: dict[str, list[str]] = {}
    for row in rows:
        directives = row.get("directives") if isinstance(row.get("directives"), dict) else {}
        for name, values in directives.items():
            current = merged.setdefault(name, [])
            for token in values if isinstance(values, list) else []:
                if token not in current:
                    current.append(token)
    return {
        "directives": merged,
        "scriptSources": merged.get("script-src", merged.get("default-src", [])),
        "styleSources": merged.get("style-src", merged.get("default-src", [])),
        "imgSources": merged.get("img-src", merged.get("default-src", [])),
        "connectSources": merged.get("connect-src", merged.get("default-src", [])),
    }


def _redact(value: str) -> dict:
    text = str(value or "")
    digest = hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()
    if len(text) <= 8:
        preview = "*" * len(text)
    else:
        preview = f"{text[:4]}...{text[-4:]}"
    return {"redacted": preview, "length": len(text), "sha256": digest}


async def _page_context(page: Any, page_id: str | None) -> dict:
    try:
        return await browser_manager.page_envelope(page, page_id)
    except Exception:
        return {"page_id": browser_manager.page_id_for(page) or page_id or "", "active_page_id": browser_manager.active_page_id, "session_id": browser_manager.session_id}


async def _install(page: Any) -> dict:
    await browser_manager.add_persistent_script(_CSP_HOOK_NAME, _CSP_HOOK)
    result = await _await_no_cancel_wait(page.evaluate(_CSP_HOOK), timeout=3.0)
    return result if isinstance(result, dict) else {"installed": False}


@mcp.tool()
async def browser_csp(
    action: str = "parse",
    url: str | None = None,
    headers: dict | list | None = None,
    html: str | None = None,
    limit: int = 300,
    clear: bool = False,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    action_name = str(action or "parse").strip().lower()
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, f"browser_csp:{action_name}", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
        hook_state = await _install(page)
        max_items = max(1, min(int(limit or 300), 1000))
        if action_name == "parse":
            rows = _direct_policy_rows(headers, html, url)
            rows.extend(_collect_header_policies(resolved_page_id))
            rows.extend(await _meta_policies(page))
            merged = _merge_policies(rows)
            out = {
                "success": True,
                "status": "ok",
                "action": action_name,
                "policies": rows[:max_items],
                "policy_count": len(rows),
                "truncated": len(rows) > max_items,
                **merged,
                "hook": {"installed": bool(hook_state.get("installed")), "violation_count": hook_state.get("violation_count", 0)},
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
            out.update(await _page_context(page, resolved_page_id))
            return out
        if action_name == "violations":
            result = await _await_no_cancel_wait(page.evaluate("""clear => {
              const state = window.__aida_csp_debug;
              if (!state || !Array.isArray(state.violations)) return { violations: [], count: 0, installed: false };
              const rows = state.violations.slice();
              if (clear) state.violations.length = 0;
              return { violations: rows, count: rows.length, installed: true, cleared: !!clear };
            }""", bool(clear)), timeout=3.0)
            out = result if isinstance(result, dict) else {"violations": [], "count": 0, "installed": False}
            if isinstance(out.get("violations"), list) and len(out["violations"]) > max_items:
                out["violations"] = out["violations"][-max_items:]
                out["truncated"] = True
            out.update({"success": True, "status": "ok", "action": action_name, "elapsed_ms": int((time.perf_counter() - started) * 1000)})
            out.update(await _page_context(page, resolved_page_id))
            return out
        if action_name == "nonce_extract":
            rows = await _await_no_cancel_wait(page.evaluate("""() => {
              const nodes = Array.from(document.querySelectorAll("script[nonce],style[nonce],[nonce]"));
              return nodes.map((node, index) => ({
                index,
                tagName: node.tagName || "",
                nonce: node.nonce || node.getAttribute("nonce") || "",
                src: node.src || "",
                href: node.href || "",
                id: node.id || "",
                className: String(node.className || "").slice(0, 200)
              })).filter(item => item.nonce);
            }"""), timeout=3.0)
            nonces = []
            for item in rows if isinstance(rows, list) else []:
                nonce = str((item or {}).get("nonce") or "")
                if not nonce:
                    continue
                redacted = _redact(nonce)
                nonces.append({
                    "index": (item or {}).get("index"),
                    "tagName": (item or {}).get("tagName"),
                    "nonce": redacted,
                    "source": {"src": (item or {}).get("src", ""), "href": (item or {}).get("href", ""), "id": (item or {}).get("id", ""), "className": (item or {}).get("className", "")},
                })
            if len(nonces) > max_items:
                nonces = nonces[-max_items:]
            out = {"success": True, "status": "ok", "action": action_name, "nonces": nonces, "count": len(nonces), "raw_nonce_returned": False, "elapsed_ms": int((time.perf_counter() - started) * 1000)}
            out.update(await _page_context(page, resolved_page_id))
            return out
        if action_name == "bypass_eval":
            rows = _direct_policy_rows(headers, html, url)
            rows.extend(_collect_header_policies(resolved_page_id))
            rows.extend(await _meta_policies(page))
            merged = _merge_policies(rows)
            script_sources = [str(token) for token in merged.get("scriptSources", [])]
            lower = {token.lower().strip("'") for token in script_sources}
            eval_allowed = "unsafe-eval" in lower
            wasm_eval_allowed = "wasm-unsafe-eval" in lower or eval_allowed
            inline_allowed = "unsafe-inline" in lower
            has_nonce_or_hash = any(token.startswith("'nonce-") or token.startswith("'sha") for token in script_sources)
            methods = []
            if eval_allowed:
                methods.append({"method": "native_eval_allowed_by_policy", "allowed": True, "weakens_browser_protections": False})
            if wasm_eval_allowed:
                methods.append({"method": "wasm_eval_allowed_by_policy", "allowed": True, "weakens_browser_protections": False})
            if inline_allowed:
                methods.append({"method": "inline_script_allowed_by_policy", "allowed": True, "weakens_browser_protections": False})
            if has_nonce_or_hash:
                methods.append({"method": "application_owned_nonce_or_hash_required", "allowed": False, "weakens_browser_protections": False})
            if not methods:
                methods.append({"method": "no_safe_eval_path_visible", "allowed": False, "weakens_browser_protections": False})
            out = {
                "success": True,
                "status": "ok",
                "action": action_name,
                "cspBypassPossible": False,
                "evalAllowedByPolicy": eval_allowed,
                "wasmEvalAllowedByPolicy": wasm_eval_allowed,
                "inlineScriptAllowedByPolicy": inline_allowed,
                "methods": methods,
                "policy_count": len(rows),
                "scriptSources": script_sources,
                "note": "No bypass is performed. This action reports policy-visible feasibility without disabling or weakening browser CSP enforcement.",
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
            out.update(await _page_context(page, resolved_page_id))
            return out
        return {"success": False, "status": "failed", "action": action_name, "error": "unknown action. Use parse, violations, nonce_extract, bypass_eval", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
    except Exception as exc:
        _camoufox_debug("browser_csp_exception", action=action_name, error_type=type(exc).__name__, error_summary=_safe_text(exc, 500))
        return {"success": False, "status": "failed", "action": action_name, "error": str(exc), "error_type": type(exc).__name__, "active_page_id": browser_manager.active_page_id}
