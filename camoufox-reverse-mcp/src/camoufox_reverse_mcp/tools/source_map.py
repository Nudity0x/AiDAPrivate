from __future__ import annotations

import base64
import json
import os
import re
import time
from typing import Any
from urllib.parse import unquote_to_bytes, urljoin

from ..browser import _await_no_cancel_wait, _camoufox_debug, _safe_text
from ..server import browser_manager, mcp


_VLQ_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
_VLQ_VALUES = {ch: index for index, ch in enumerate(_VLQ_CHARS)}
_MAP_REF_RE = re.compile(r"(?:\/\/[#@]\s*sourceMappingURL=([^\s]+)|\/\*[#@]\s*sourceMappingURL=([^*]+)\*\/)")


def _decode_data_url(value: str) -> str | None:
    text = str(value or "")
    if not text.startswith("data:"):
        return None
    header, _, payload = text.partition(",")
    if not payload:
        return ""
    raw = unquote_to_bytes(payload)
    if ";base64" in header.lower():
        raw = base64.b64decode(raw)
    return raw.decode("utf-8", "replace")


def _extract_map_ref(source: str) -> str:
    matches = list(_MAP_REF_RE.finditer(source or ""))
    if not matches:
        return ""
    match = matches[-1]
    return str(match.group(1) or match.group(2) or "").strip()


def _safe_write_path(path: str, overwrite: bool) -> str:
    if not isinstance(path, str) or not path.strip():
        raise ValueError("save_path is required")
    resolved = os.path.abspath(os.path.expanduser(os.path.expandvars(path)))
    if os.path.isdir(resolved):
        raise ValueError("save_path must be a file path")
    parent = os.path.dirname(resolved)
    if parent:
        os.makedirs(parent, exist_ok=True)
    if os.path.exists(resolved) and not overwrite:
        raise FileExistsError(f"file already exists: {resolved}")
    return resolved


async def _page_context(page: Any, page_id: str | None) -> dict:
    try:
        return await browser_manager.page_envelope(page, page_id)
    except Exception:
        return {"page_id": browser_manager.page_id_for(page) or page_id or "", "active_page_id": browser_manager.active_page_id, "session_id": browser_manager.session_id}


async def _detect(page: Any, limit: int) -> list[dict]:
    result = await _await_no_cancel_wait(page.evaluate("""async maxScripts => {
      const scripts = Array.from(document.scripts || []).slice(0, Math.max(1, Math.min(Number(maxScripts || 200), 500)));
      const out = [];
      const re = /(?:\/\/[#@]\s*sourceMappingURL=([^\s]+)|\/\*[#@]\s*sourceMappingURL=([^*]+)\*\/)/g;
      for (let i = 0; i < scripts.length; i++) {
        const script = scripts[i];
        const scriptUrl = script.src || ("inline:" + i);
        let source = script.src ? "" : (script.textContent || "");
        let fetchStatus = script.src ? "not_attempted" : "inline";
        if (script.src) {
          try {
            const response = await fetch(script.src, { credentials: "include", cache: "no-store" });
            source = await response.text();
            fetchStatus = response.ok ? "ok" : ("http_" + response.status);
          } catch (e) {
            fetchStatus = "failed:" + String(e && (e.message || e)).slice(0, 160);
          }
        }
        let match = null;
        let found = null;
        while ((match = re.exec(source)) !== null) found = match;
        if (!found) continue;
        const ref = String(found[1] || found[2] || "").trim();
        let resolved = ref;
        let inline = false;
        try {
          inline = ref.startsWith("data:");
          resolved = inline ? ref : new URL(ref, script.src || location.href).href;
        } catch (e) {}
        out.push({
          scriptUrl,
          index: i,
          sourceMapUrl: resolved,
          sourceMapRef: ref,
          inline,
          fetchStatus,
          scriptLength: source.length
        });
      }
      return out;
    }""", max(1, min(int(limit or 200), 500))), timeout=20.0)
    return result if isinstance(result, list) else []


async def _fetch_text(page: Any, source_map_url: str) -> dict:
    inline = _decode_data_url(source_map_url)
    if inline is not None:
        return {"ok": True, "url": source_map_url, "text": inline, "status": 200, "inline": True}
    result = await _await_no_cancel_wait(page.evaluate("""async url => {
      try {
        const response = await fetch(url, { credentials: "include", cache: "no-store" });
        const text = await response.text();
        return { ok: response.ok, status: response.status, statusText: response.statusText || "", url: response.url || url, text, inline: false };
      } catch (e) {
        return { ok: false, status: 0, statusText: "", url, text: "", error: String(e && (e.message || e)), inline: false };
      }
    }""", source_map_url), timeout=20.0)
    return result if isinstance(result, dict) else {"ok": False, "url": source_map_url, "text": "", "error": "unexpected_fetch_result", "inline": False}


async def _load_map(page: Any, source_map_url: str | None, script_url: str | None, limit: int) -> tuple[dict, str, str]:
    target = str(source_map_url or "").strip()
    generated = str(script_url or "").strip()
    if not target:
        detected = await _detect(page, limit)
        if script_url:
            filtered = [item for item in detected if str(item.get("scriptUrl") or "") == script_url or script_url in str(item.get("scriptUrl") or "")]
            detected = filtered or detected
        if not detected:
            raise ValueError("source map not detected")
        target = str(detected[0].get("sourceMapUrl") or "")
        generated = generated or str(detected[0].get("scriptUrl") or "")
    fetched = await _fetch_text(page, target)
    if not fetched.get("ok") and not fetched.get("text"):
        raise RuntimeError(f"source map fetch failed: {fetched.get('error') or fetched.get('statusText') or fetched.get('status')}")
    text = str(fetched.get("text") or "")
    data = json.loads(text)
    if not isinstance(data, dict):
        raise ValueError("source map JSON root is not an object")
    return data, target, generated


def _decode_vlq_segment(segment: str) -> list[int]:
    values: list[int] = []
    value = 0
    shift = 0
    for ch in segment:
        digit = _VLQ_VALUES.get(ch)
        if digit is None:
            raise ValueError("invalid source map VLQ character")
        continuation = digit & 32
        digit &= 31
        value += digit << shift
        if continuation:
            shift += 5
            continue
        negative = value & 1
        decoded = value >> 1
        values.append(-decoded if negative else decoded)
        value = 0
        shift = 0
    return values


def _mapping_segments(source_map: dict) -> list[dict]:
    mappings = str(source_map.get("mappings") or "")
    rows: list[dict] = []
    source_index = 0
    original_line = 0
    original_column = 0
    name_index = 0
    for generated_line, line in enumerate(mappings.split(";")):
        generated_column = 0
        if not line:
            continue
        for segment in line.split(","):
            if not segment:
                continue
            values = _decode_vlq_segment(segment)
            if not values:
                continue
            generated_column += values[0]
            row = {"generatedLine": generated_line, "generatedColumn": generated_column}
            if len(values) >= 4:
                source_index += values[1]
                original_line += values[2]
                original_column += values[3]
                row.update({"sourceIndex": source_index, "originalLine": original_line, "originalColumn": original_column})
                if len(values) >= 5:
                    name_index += values[4]
                    row["nameIndex"] = name_index
            rows.append(row)
    return rows


def _map_position(source_map: dict, line: int, column: int) -> dict:
    generated_line = max(0, int(line) - 1)
    generated_column = max(0, int(column))
    candidates = [row for row in _mapping_segments(source_map) if row.get("generatedLine") == generated_line and "sourceIndex" in row]
    if not candidates:
        return {"matched": False, "generatedLine": line, "generatedColumn": column}
    best = candidates[0]
    for row in candidates:
        if int(row.get("generatedColumn", 0)) <= generated_column:
            best = row
        else:
            break
    sources = source_map.get("sources") if isinstance(source_map.get("sources"), list) else []
    names = source_map.get("names") if isinstance(source_map.get("names"), list) else []
    source_index = int(best.get("sourceIndex", -1))
    name_index = int(best.get("nameIndex", -1)) if "nameIndex" in best else -1
    return {
        "matched": source_index >= 0 and source_index < len(sources),
        "generatedLine": line,
        "generatedColumn": column,
        "mappedGeneratedLine": int(best.get("generatedLine", 0)) + 1,
        "mappedGeneratedColumn": int(best.get("generatedColumn", 0)),
        "sourceIndex": source_index,
        "source": sources[source_index] if 0 <= source_index < len(sources) else "",
        "line": int(best.get("originalLine", 0)) + 1,
        "column": int(best.get("originalColumn", 0)),
        "name": names[name_index] if 0 <= name_index < len(names) else "",
    }


async def _source_content(page: Any, source_map: dict, source_map_url: str, source_name: str, source_index: int) -> dict:
    contents = source_map.get("sourcesContent") if isinstance(source_map.get("sourcesContent"), list) else []
    if 0 <= source_index < len(contents) and contents[source_index] is not None:
        text = str(contents[source_index])
        return {"available": True, "source": text, "length": len(text), "source_method": "sourcesContent"}
    root = str(source_map.get("sourceRoot") or "")
    candidate = urljoin(source_map_url, urljoin(root, source_name))
    fetched = await _fetch_text(page, candidate)
    if fetched.get("ok"):
        text = str(fetched.get("text") or "")
        return {"available": True, "source": text, "length": len(text), "source_method": "fetch", "source_url": fetched.get("url") or candidate}
    return {"available": False, "source": "", "length": 0, "source_method": "unavailable", "source_url": candidate, "error": fetched.get("error") or fetched.get("statusText") or fetched.get("status")}


@mcp.tool()
async def browser_source_map(
    action: str = "detect",
    source_map_url: str | None = None,
    script_url: str | None = None,
    save_path: str | None = None,
    source_file: str | None = None,
    line: int = 1,
    column: int = 0,
    overwrite: bool = False,
    limit: int = 200,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    action_name = str(action or "detect").strip().lower()
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, f"browser_source_map:{action_name}", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
        if action_name == "detect":
            maps = await _detect(page, limit)
            out = {"success": True, "status": "ok", "action": action_name, "source_maps": maps, "count": len(maps), "elapsed_ms": int((time.perf_counter() - started) * 1000)}
            out.update(await _page_context(page, resolved_page_id))
            return out
        if action_name == "download":
            data, target, generated = await _load_map(page, source_map_url, script_url, limit)
            if save_path:
                resolved = _safe_write_path(save_path, overwrite)
                text = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
                with open(resolved, "w", encoding="utf-8") as handle:
                    handle.write(text)
                written_path = resolved
                size = os.path.getsize(resolved)
            else:
                written_path = ""
                size = len(json.dumps(data, ensure_ascii=False))
            out = {
                "success": True,
                "status": "ok",
                "action": action_name,
                "source_map_url": target,
                "generated": generated,
                "path": written_path,
                "size": size,
                "version": data.get("version"),
                "file": data.get("file", ""),
                "sources": data.get("sources") if isinstance(data.get("sources"), list) else [],
                "source_count": len(data.get("sources") if isinstance(data.get("sources"), list) else []),
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
            out.update(await _page_context(page, resolved_page_id))
            return out
        if action_name == "list_original_sources":
            data, target, generated = await _load_map(page, source_map_url, script_url, limit)
            sources = data.get("sources") if isinstance(data.get("sources"), list) else []
            contents = data.get("sourcesContent") if isinstance(data.get("sourcesContent"), list) else []
            rows = []
            for index, source in enumerate(sources):
                content = contents[index] if index < len(contents) else None
                rows.append({"index": index, "originalPath": str(source), "generatedPath": generated or str(data.get("file") or ""), "hasContent": content is not None, "sourceLength": len(str(content)) if content is not None else 0})
            out = {"success": True, "status": "ok", "action": action_name, "source_map_url": target, "sources": rows, "count": len(rows), "elapsed_ms": int((time.perf_counter() - started) * 1000)}
            out.update(await _page_context(page, resolved_page_id))
            return out
        if action_name == "reconstruct":
            data, target, generated = await _load_map(page, source_map_url, script_url, limit)
            sources = data.get("sources") if isinstance(data.get("sources"), list) else []
            mapping = _map_position(data, int(line), int(column))
            selected_index = -1
            selected_source = ""
            if source_file:
                for index, source in enumerate(sources):
                    if str(source_file) == str(source) or str(source_file) in str(source):
                        selected_index = index
                        selected_source = str(source)
                        break
            if selected_index < 0 and mapping.get("matched"):
                selected_index = int(mapping.get("sourceIndex", -1))
                selected_source = str(mapping.get("source") or "")
            if selected_index < 0:
                return {"success": False, "status": "failed", "action": action_name, "error": "original source not found for requested mapping", "mapping": mapping, "source_map_url": target, "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
            content = await _source_content(page, data, target, selected_source, selected_index)
            out = {
                "success": bool(content.get("available")),
                "status": "ok" if content.get("available") else "metadata_only",
                "action": action_name,
                "source_map_url": target,
                "generated": generated,
                "mapping": mapping,
                "source_file": selected_source,
                "source_index": selected_index,
                "source": content.get("source", ""),
                "length": content.get("length", 0),
                "source_method": content.get("source_method"),
                "source_url": content.get("source_url", ""),
                "error": content.get("error"),
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
            out.update(await _page_context(page, resolved_page_id))
            return out
        return {"success": False, "status": "failed", "action": action_name, "error": "unknown action. Use detect, download, reconstruct, list_original_sources", "active_page_id": browser_manager.active_page_id}
    except Exception as exc:
        _camoufox_debug("browser_source_map_exception", action=action_name, error_type=type(exc).__name__, error_summary=_safe_text(exc, 500))
        return {"success": False, "status": "failed", "action": action_name, "error": str(exc), "error_type": type(exc).__name__, "active_page_id": browser_manager.active_page_id}
