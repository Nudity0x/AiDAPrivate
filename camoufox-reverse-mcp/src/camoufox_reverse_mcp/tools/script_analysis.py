from __future__ import annotations

import json as _json
import hashlib as _hashlib
import os
import time
from urllib.parse import urldefrag as _urldefrag
from urllib.parse import urljoin as _urljoin

from ..browser import _camoufox_debug, _safe_text, _target_domain
from ..server import mcp, browser_manager


@mcp.tool()
async def scripts(
    action: str,
    url: str | None = None,
    save_path: str | None = None,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    """Script inspection (v0.9.0 unified).

    Replaces list_scripts / get_script_source / save_script.

    Args:
        action:
          "list" — list all loaded scripts (src, type, inline preview)
          "get"  — get full source of one script (requires url;
                   use "inline:<index>" for inline scripts)
          "save" — save script source to local file (requires url + save_path)
        url: Script URL or "inline:<index>" (required for "get" and "save").
        save_path: Local file path (required for "save").

    Returns:
        For "list": list of script info dicts.
        For "get": dict with source string.
        For "save": dict with status, path, size.
    """
    started = time.perf_counter()
    normalized_action = str(action or "").strip().lower()
    if normalized_action == "list":
        return await _list_scripts(page_id, started, aida_operation_id)
    elif normalized_action == "get":
        if not url:
            return await _script_error("get", "url is required for action='get'", page_id, started, "missing_url")
        try:
            page = await browser_manager.resolve_page_for_operation(page_id, "scripts_get", True, aida_operation_id)
            src = await _get_script_source(page, url)
            out = {
                "status": "ok",
                "action": "get",
                "source": src,
                "url": url,
                "length": len(src) if isinstance(src, str) else 0,
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
            out.update(await _script_page_context(page, page_id))
            return out
        except Exception as exc:
            return await _script_error("get", exc, page_id, started, "get_failed")
    elif normalized_action == "save":
        if not url:
            return await _script_error("save", "url is required for action='save'", page_id, started, "missing_url")
        if not save_path:
            return await _script_error("save", "save_path is required for action='save'", page_id, started, "missing_save_path")
        try:
            page = await browser_manager.resolve_page_for_operation(page_id, "scripts_save", True, aida_operation_id)
            out = await _save_script(page, url, save_path, started)
            out.update(await _script_page_context(page, page_id))
            return out
        except Exception as exc:
            return await _script_error("save", exc, page_id, started, "save_failed")
    else:
        return await _script_error(normalized_action or "unknown", f"unknown action: {action}. Use list/get/save", page_id, started, "unknown_action")


@mcp.tool()
async def search_code(
    keyword: str,
    script_url: str | None = None,
    context_chars: int = 200,
    context_lines: int = 3,
    max_results: int = 200,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    """Search keyword in loaded scripts (v0.9.0 unified).

    Replaces search_code (all scripts) + search_code_in_script (single script).

    Args:
        keyword: The keyword to search for (case-sensitive substring match).
        script_url: If None, search across ALL loaded scripts.
            If given, search within that one script only (supports
            "inline:<index>" for inline scripts). Single-script mode
            auto-detects minified files and uses character-based context.
        context_chars: Context window in char mode (default 200 = +/-200 chars).
            Used when searching single minified scripts.
        context_lines: Context window in line mode (default 3).
        max_results: Maximum matches to return (default 200).

    Returns:
        dict with matches, total_matches, mode ("line" | "char"), etc.
    """
    if script_url is None:
        return await _search_code_all(keyword, max_results, context_lines, page_id, aida_operation_id)
    else:
        return await _search_code_in_script(
            script_url, keyword, context_lines, context_chars, max_results, page_id, aida_operation_id
        )


# ---- internal implementations (not registered as MCP tools) ----

async def _script_page_context(page, page_id: str | None) -> dict:
    out: dict = {
        "session_id": browser_manager.session_id,
        "requested_page_id": page_id or "",
        "active_page_id": browser_manager.active_page_id or "",
        "page_count": len(browser_manager.pages),
        "pages": [],
    }
    try:
        out.update(await browser_manager.page_envelope(page, page_id))
    except Exception as exc:
        out["page_envelope_error"] = _safe_text(exc, 300)
    try:
        records_fn = getattr(browser_manager, "_registered_page_records", None)
        if callable(records_fn):
            out["pages"] = records_fn()
    except Exception as exc:
        out["pages_error"] = _safe_text(exc, 300)
    return out


async def _script_error(action: str, exc_or_message, page_id: str | None, started: float, error_code: str) -> dict:
    error_text = _safe_text(exc_or_message, 900)
    out = {
        "status": "error",
        "action": action,
        "error": error_text,
        "error_code": error_code,
        "requested_page_id": page_id or "",
        "session_id": browser_manager.session_id,
        "active_page_id": browser_manager.active_page_id or "",
        "page_count": len(browser_manager.pages),
        "elapsed_ms": int((time.perf_counter() - started) * 1000),
        "pages": [],
    }
    try:
        records_fn = getattr(browser_manager, "_registered_page_records", None)
        if callable(records_fn):
            out["pages"] = records_fn()
    except Exception as records_exc:
        out["pages_error"] = _safe_text(records_exc, 300)
    _camoufox_debug(
        "scripts_error",
        action=action,
        error_code=error_code,
        error=error_text,
        requested_page_id=page_id or "",
        active_page_id=browser_manager.active_page_id or "",
        page_count=len(browser_manager.pages),
    )
    return out


async def _list_scripts(page_id: str | None, started: float, aida_operation_id=None) -> dict:
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, "scripts_list", True, aida_operation_id)
        scripts_list = await page.evaluate("""() => {
            const scripts = document.querySelectorAll('script');
            return Array.from(scripts).map((s, i) => ({
                index: i,
                src: s.src || null,
                normalized_src: s.src ? (() => { try { const u = new URL(s.src, location.href); u.hash = ''; return u.href; } catch(e) { return s.src || null; } })() : null,
                type: s.type || 'text/javascript',
                is_module: s.type === 'module',
                inline_length: s.src ? 0 : (s.textContent || '').length,
                preview: s.src ? null : (s.textContent || '').substring(0, 200)
            }));
        }""")
        if not isinstance(scripts_list, list):
            raise RuntimeError(f"script enumeration returned {type(scripts_list).__name__}")
        # v1.0.1: add hint for large scripts
        for s in scripts_list:
            size = s.get("inline_length", 0)
            src = s.get("src")
            if src and size == 0:
                # For external scripts, we don't know size yet from DOM alone
                # but we can hint based on common patterns
                pass
            elif size > 100_000:
                s["hint"] = (
                    f"Large script ({size} bytes). Consider saving for "
                    f"offline analysis: scripts(action='save', "
                    f"url='inline:{s['index']}', save_path='./script_{s['index']}.js')"
                )
        out = {
            "status": "ok",
            "action": "list",
            "scripts": scripts_list,
            "count": len(scripts_list),
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
        }
        out.update(await _script_page_context(page, page_id))
        _camoufox_debug(
            "scripts_list",
            status="ok",
            count=len(scripts_list),
            page_id=out.get("page_id", ""),
            active_page_id=out.get("active_page_id", ""),
            page_domain=_target_domain(out.get("url", "")),
        )
        return out
    except Exception as e:
        return await _script_error("list", e, page_id, started, "list_failed")


async def _get_script_source(page, url: str) -> str:
    if url.startswith("inline:"):
        idx = int(url.split(":", 1)[1])
        source = await page.evaluate("""(idx) => {
            const scripts = document.querySelectorAll('script');
            return scripts[idx] ? scripts[idx].textContent : null;
        }""", idx)
        if source is None:
            raise RuntimeError(f"inline script at index {idx} not found")
        return str(source)
    result = await page.evaluate("""async (scriptUrl) => {
        try {
            const resp = await fetch(scriptUrl);
            const source = await resp.text();
            return { ok: resp.ok, status: resp.status, statusText: resp.statusText || "", source };
        } catch(e) {
            return { ok: false, status: 0, statusText: "", error: String(e && (e.message || e)) };
        }
    }""", url)
    if not isinstance(result, dict):
        raise RuntimeError(f"fetch returned {type(result).__name__}")
    if not result.get("ok"):
        raise RuntimeError(f"fetch failed status={result.get('status', 0)} error={result.get('error') or result.get('statusText') or 'unknown'}")
    return str(result.get("source") or "")


async def _save_script(page, url: str, save_path: str, started: float) -> dict:
    source = await _get_script_source(page, url)
    os.makedirs(os.path.dirname(os.path.abspath(save_path)), exist_ok=True)
    with open(save_path, "w", encoding="utf-8") as f:
        f.write(source)
    return {
        "status": "saved",
        "action": "save",
        "url": url,
        "path": save_path,
        "size": len(source),
        "elapsed_ms": int((time.perf_counter() - started) * 1000),
    }


def _normalize_script_url(page_url: str, script_url: str) -> str:
    if script_url.startswith("inline:"):
        return script_url
    try:
        return _urldefrag(_urljoin(page_url or "", script_url or ""))[0]
    except Exception:
        return script_url or ""


async def _search_candidate_pages(page_id: str | None, operation: str, aida_operation_id=None) -> list[tuple[str, object]]:
    if page_id:
        page = await browser_manager.resolve_page_for_operation(page_id, operation, True, aida_operation_id)
        return [(browser_manager.page_id_for(page) or page_id, page)]
    out: list[tuple[str, object]] = []
    seen: set[int] = set()
    active_id = browser_manager.active_page_id or browser_manager.active_page_name
    if active_id and active_id in browser_manager.pages:
        page = browser_manager.pages.get(active_id)
        if page is not None and not browser_manager._page_closed(page):
            out.append((active_id, page))
            seen.add(id(page))
    for pid, page in list(browser_manager.pages.items()):
        if id(page) in seen:
            continue
        if not browser_manager._page_closed(page):
            out.append((pid, page))
            seen.add(id(page))
    if out:
        return out
    page = await browser_manager.resolve_page_for_operation(None, operation, True, aida_operation_id)
    return [(browser_manager.page_id_for(page) or "", page)]


async def _search_code_all(keyword: str, max_results: int = 50, context_lines: int = 3, page_id: str | None = None, aida_operation_id=None) -> dict:
    try:
        if max_results > 200:
            max_results = 200
        keyword_text = str(keyword or "")
        pages = await _search_candidate_pages(page_id, "search_code", aida_operation_id)
        merged_matches: list[dict] = []
        merged_scripts_with_matches: list[dict] = []
        merged_script_diagnostics: list[dict] = []
        total_matches = 0
        scripts_searched = 0
        page_summaries: list[dict] = []
        for pid, page in pages:
            page_url = str(getattr(page, "url", "") or "")
            result = await page.evaluate("""async (args) => {
            const keyword = String(args.keyword || '');
            const maxResults = Number(args.maxResults || 0);
            const contextLines = Math.max(0, Math.min(Number(args.contextLines || 3), 20));
            async function sha256(text) {
                try {
                    const bytes = new TextEncoder().encode(String(text || ''));
                    const digest = await crypto.subtle.digest('SHA-256', bytes);
                    return Array.from(new Uint8Array(digest)).map(b => b.toString(16).padStart(2, '0')).join('');
                } catch(e) {
                    let h = 2166136261;
                    const s = String(text || '');
                    for (let i = 0; i < s.length; i++) {
                        h ^= s.charCodeAt(i);
                        h = Math.imul(h, 16777619) >>> 0;
                    }
                    return 'fnv32:' + h.toString(16).padStart(8, '0');
                }
            }
            function normalizeUrl(value) {
                try {
                    if (String(value || '').indexOf('inline:') === 0) return String(value || '');
                    const u = new URL(String(value || ''), location.href);
                    u.hash = '';
                    return u.href;
                } catch(e) {
                    return String(value || '');
                }
            }
            const scripts = document.querySelectorAll('script');
            const matches = [];
            let totalMatches = 0;
            let scriptsSearched = 0;
            const scriptsWithMatches = [];
            const scriptDiagnostics = [];
            for (const s of scripts) {
                let source = '';
                let scriptUrl = '';
                let fetchStatus = 0;
                let fetchOk = false;
                let fetchError = '';
                const domSrc = String(s.src || '');
                if (s.src) {
                    scriptUrl = s.src;
                    try {
                        const resp = await fetch(s.src, {cache: 'force-cache'});
                        fetchStatus = resp.status || 0;
                        fetchOk = !!resp.ok;
                        source = await resp.text();
                    } catch(e) {
                        fetchError = String(e && (e.message || e) || '');
                    }
                } else {
                    scriptUrl = 'inline:' + scriptsSearched;
                    source = s.textContent || '';
                    fetchOk = true;
                }
                scriptsSearched++;
                const normalizedUrl = normalizeUrl(scriptUrl);
                const markerPresent = keyword ? source.indexOf(keyword) >= 0 : false;
                const sourceHash = await sha256(source);
                scriptDiagnostics.push({
                    index: scriptsSearched - 1,
                    script_url: scriptUrl,
                    normalized_url: normalizedUrl,
                    dom_src: domSrc,
                    source_length: source.length,
                    source_sha256: sourceHash,
                    marker_present: markerPresent,
                    fetch_status: fetchStatus,
                    fetch_ok: fetchOk,
                    fetch_error: fetchError ? fetchError.slice(0, 300) : ''
                });
                if (!source) continue;
                const lines = source.split('\\n');
                let scriptMatchCount = 0;
                for (let i = 0; i < lines.length; i++) {
                    if (lines[i].includes(keyword)) {
                        totalMatches++;
                        scriptMatchCount++;
                        if (matches.length < maxResults) {
                            const start = Math.max(0, i - contextLines);
                            const end = Math.min(lines.length, i + contextLines + 1);
                            const contextSlice = lines.slice(start, end);
                            const contextStr = contextSlice.join('\\n');
                            matches.push({
                                script_url: scriptUrl,
                                normalized_url: normalizedUrl,
                                source_sha256: sourceHash,
                                line_number: i + 1,
                                match: lines[i].trim().substring(0, 500),
                                context: contextStr.length > 2000
                                    ? contextStr.substring(0, 2000) + '...(truncated)'
                                    : contextStr
                            });
                        }
                    }
                }
                if (scriptMatchCount > 0) {
                    scriptsWithMatches.push({
                        url: scriptUrl,
                        normalized_url: normalizedUrl,
                        match_count: scriptMatchCount,
                        source_length: source.length,
                        source_sha256: sourceHash
                    });
                }
            }
            return {
                matches: matches,
                total_matches: totalMatches,
                returned_matches: matches.length,
                scripts_searched: scriptsSearched,
                scripts_with_matches: scriptsWithMatches,
                script_diagnostics: scriptDiagnostics,
                truncated: totalMatches > matches.length
            };
        }""", {"keyword": keyword_text, "maxResults": max(0, int(max_results) - len(merged_matches)), "contextLines": context_lines})
            if not isinstance(result, dict):
                continue
            scripts_searched += int(result.get("scripts_searched") or 0)
            total_matches += int(result.get("total_matches") or 0)
            for diag in result.get("script_diagnostics") or []:
                if isinstance(diag, dict):
                    diag["page_id"] = pid
                    diag["page_url"] = page_url
                    diag["python_normalized_url"] = _normalize_script_url(page_url, str(diag.get("script_url") or ""))
                    merged_script_diagnostics.append(diag)
            for item in result.get("scripts_with_matches") or []:
                if isinstance(item, dict):
                    item["page_id"] = pid
                    merged_scripts_with_matches.append(item)
            for item in result.get("matches") or []:
                if isinstance(item, dict) and len(merged_matches) < max_results:
                    item["page_id"] = pid
                    merged_matches.append(item)
            page_summaries.append({
                "page_id": pid,
                "url": page_url,
                "scripts_searched": int(result.get("scripts_searched") or 0),
                "total_matches": int(result.get("total_matches") or 0),
            })
        marker_diag = [d for d in merged_script_diagnostics if d.get("marker_present")]
        out = {
            "matches": merged_matches,
            "total_matches": total_matches,
            "returned_matches": len(merged_matches),
            "scripts_searched": scripts_searched,
            "scripts_with_matches": merged_scripts_with_matches,
            "truncated": total_matches > len(merged_matches),
            "pages_searched": len(pages),
            "page_id": page_id or "",
            "active_page_id": browser_manager.active_page_id or "",
            "script_diagnostics": merged_script_diagnostics[:80],
            "marker_proof": {
                "keyword": keyword_text,
                "marker_present": bool(marker_diag),
                "script_count": len(merged_script_diagnostics),
                "marker_script_count": len(marker_diag),
                "source_hashes": [str(d.get("source_sha256") or "") for d in marker_diag[:16]],
                "normalized_urls": [str(d.get("normalized_url") or d.get("python_normalized_url") or "") for d in marker_diag[:16]],
            },
            "page_summaries": page_summaries,
        }
        _camoufox_debug(
            "search_code_exit",
            keyword_len=len(keyword_text),
            requested_page_id=page_id or "",
            active_page_id=browser_manager.active_page_id or "",
            pages_searched=len(pages),
            scripts_searched=scripts_searched,
            total_matches=total_matches,
            marker_present=bool(marker_diag),
            marker_script_count=len(marker_diag),
        )
        return out
    except Exception as e:
        _camoufox_debug(
            "search_code_exit",
            success=False,
            keyword_len=len(str(keyword or "")),
            requested_page_id=page_id or "",
            error_type=type(e).__name__,
            error_summary=_safe_text(e, 700),
        )
        return {"error": str(e)}


async def _search_code_in_script(
    script_url: str, keyword: str,
    context_lines: int = 3, context_chars: int = 200,
    max_results: int = 200,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, "search_code_in_script", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
        if script_url.startswith("inline:"):
            idx = int(script_url.split(":")[1])
            src = await page.evaluate(f"""() => {{
                const scripts = document.querySelectorAll('script');
                return scripts[{idx}] ? (scripts[{idx}].textContent || '') : null;
            }}""")
            if src is None:
                return {"error": f"Inline script not found at index {idx}"}
        else:
            src = await page.evaluate(
                f"fetch({_json.dumps(script_url)}, {{cache: 'force-cache'}}).then(r => r.text())"
            )
        if not isinstance(src, str):
            return {"error": f"script not fetchable: got {type(src).__name__}"}
        normalized_url = _normalize_script_url(str(getattr(page, "url", "") or ""), script_url)
        source_hash = _hashlib.sha256(src.encode("utf-8", "replace")).hexdigest()

        lines = src.split("\n")
        max_line_len = max((len(l) for l in lines), default=0)
        use_char_mode = len(lines) < 10 or max_line_len > 5000

        results: list[dict] = []
        total = 0

        if use_char_mode:
            i = 0
            while True:
                pos = src.find(keyword, i)
                if pos == -1:
                    break
                total += 1
                if len(results) < max_results:
                    start = max(0, pos - context_chars)
                    end = min(len(src), pos + len(keyword) + context_chars)
                    results.append({
                        "position": pos,
                        "context_start": start,
                        "context_end": end,
                        "context": src[start:end],
                        "match_highlight_range": [pos - start, pos - start + len(keyword)],
                    })
                i = pos + len(keyword)
            return {
                "total_matches": total, "returned": len(results),
                "script_url": script_url, "mode": "char",
                "page_id": resolved_page_id,
                "normalized_url": normalized_url,
                "source_sha256": source_hash,
                "marker_proof": {
                    "keyword": keyword,
                    "marker_present": total > 0,
                    "source_hashes": [source_hash] if total > 0 else [],
                    "normalized_urls": [normalized_url] if total > 0 else [],
                },
                "source_size": len(src), "total_lines": len(lines),
                "max_line_length": max_line_len,
                "context_chars": context_chars, "results": results,
            }

        for idx, line in enumerate(lines):
            if keyword in line:
                total += 1
                if len(results) < max_results:
                    start = max(0, idx - context_lines)
                    end = min(len(lines), idx + context_lines + 1)
                    ctx = "\n".join(lines[start:end])
                    results.append({
                        "line": idx + 1,
                        "context": ctx[:3000] + ("...(truncated)" if len(ctx) > 3000 else ""),
                        "context_range": [start + 1, end],
                    })
        return {
            "total_matches": total, "returned": len(results),
            "script_url": script_url, "mode": "line",
            "page_id": resolved_page_id,
            "normalized_url": normalized_url,
            "source_sha256": source_hash,
            "marker_proof": {
                "keyword": keyword,
                "marker_present": total > 0,
                "source_hashes": [source_hash] if total > 0 else [],
                "normalized_urls": [normalized_url] if total > 0 else [],
            },
            "total_lines": len(lines), "results": results,
        }
    except Exception as e:
        return {"error": str(e)}
