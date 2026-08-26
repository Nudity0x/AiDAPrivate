from __future__ import annotations

import hashlib
import os
import time
from typing import Any
from urllib.parse import parse_qsl, urlparse

from ..browser import _await_no_cancel_wait, _camoufox_debug, _safe_text
from ..server import browser_manager, mcp


_TOKEN_KEYWORDS = ("token", "jwt", "bearer", "auth", "session", "csrf", "xsrf", "access", "refresh", "id_token")


def _redact(value: Any) -> dict:
    text = "" if value is None else str(value)
    digest = hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()
    if len(text) <= 8:
        preview = "*" * len(text)
    else:
        preview = f"{text[:4]}...{text[-4:]}"
    return {"redacted": preview, "length": len(text), "sha256": digest}


def _url_evidence(value: str) -> dict:
    parsed = urlparse(value or "")
    path = parsed.path or ""
    safe = f"{parsed.scheme}://{parsed.netloc}{path}" if parsed.scheme and parsed.netloc else path
    return {"url_redacted": safe[:1000], "url_length": len(value or ""), "url_sha256": hashlib.sha256((value or "").encode("utf-8", "replace")).hexdigest(), "has_query": bool(parsed.query), "has_fragment": bool(parsed.fragment)}


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
        envelope = await browser_manager.page_envelope(page, page_id)
    except Exception:
        envelope = {"page_id": browser_manager.page_id_for(page) or page_id or "", "active_page_id": browser_manager.active_page_id, "session_id": browser_manager.session_id}
    if "url" in envelope:
        envelope["url_evidence"] = _url_evidence(str(envelope.pop("url") or ""))
    return envelope


def _token_candidate_name(name: str | None) -> bool:
    lowered = str(name or "").lower()
    return any(keyword in lowered for keyword in _TOKEN_KEYWORDS)


async def _storage_tokens(page: Any, storage_type: str, name: str | None) -> list[dict]:
    result = await _await_no_cancel_wait(page.evaluate("""arg => {
      const storage = arg.storageType === "sessionstorage" ? sessionStorage : localStorage;
      const rows = [];
      if (arg.name) {
        const value = storage.getItem(arg.name);
        if (value !== null) rows.push({ name: arg.name, value, storage_type: arg.storageType });
        return rows;
      }
      for (let i = 0; i < storage.length; i++) {
        const key = storage.key(i);
        const lowered = String(key || "").toLowerCase();
        if (!/(token|jwt|bearer|auth|session|csrf|xsrf|access|refresh|id_token)/.test(lowered)) continue;
        rows.push({ name: key, value: storage.getItem(key), storage_type: arg.storageType });
      }
      return rows;
    }""", {"storageType": storage_type.lower(), "name": name or ""}), timeout=4.0)
    rows = result if isinstance(result, list) else []
    return [{"source": storage_type, "name": str(item.get("name") or ""), "token": _redact(item.get("value"))} for item in rows if isinstance(item, dict)]


async def _click_or_wait_after_submit(page: Any, submit_selector: str | None, wait_ms: int) -> dict:
    outcome: dict[str, Any] = {"submitted": False}
    if submit_selector:
        await page.click(submit_selector, timeout=max(1000, min(wait_ms, 30000)))
        outcome["submitted"] = True
        outcome["submit_selector_supplied"] = True
    if wait_ms > 0:
        try:
            await _await_no_cancel_wait(page.wait_for_load_state("networkidle", timeout=wait_ms), timeout=(wait_ms / 1000.0) + 1.0)
            outcome["post_submit_wait"] = "networkidle"
        except Exception:
            try:
                await _await_no_cancel_wait(page.wait_for_load_state("load", timeout=max(1000, min(wait_ms, 10000))), timeout=(max(1000, min(wait_ms, 10000)) / 1000.0) + 1.0)
                outcome["post_submit_wait"] = "load"
            except Exception as exc:
                outcome["post_submit_wait"] = "timeout"
                outcome["post_submit_wait_error"] = _safe_text(exc, 300)
    return outcome


async def _run_login_form(
    page: Any,
    url: str | None,
    username_selector: str | None,
    password_selector: str | None,
    submit_selector: str | None,
    username: str | None,
    password: str | None,
    wait_ms: int,
) -> dict:
    if url:
        await page.goto(url, wait_until="domcontentloaded", timeout=max(1000, min(wait_ms, 60000)))
    if not username_selector or not password_selector:
        return {"success": False, "status": "failed", "error": "username_selector and password_selector are required"}
    if username is None or password is None:
        return {"success": False, "status": "failed", "error": "username and password are required"}
    await page.fill(username_selector, username, timeout=max(1000, min(wait_ms, 30000)))
    await page.fill(password_selector, password, timeout=max(1000, min(wait_ms, 30000)))
    submit = await _click_or_wait_after_submit(page, submit_selector, wait_ms)
    return {
        "success": True,
        "status": "ok",
        "username_length": len(str(username)),
        "password_supplied": True,
        "selectors": {"username": bool(username_selector), "password": bool(password_selector), "submit": bool(submit_selector)},
        **submit,
    }


def _credential_value(step: dict, credentials: dict | None) -> tuple[str, str]:
    if "value" in step:
        return str(step.get("value") or ""), "inline_value"
    key = str(step.get("credential") or step.get("credential_key") or "")
    if key and isinstance(credentials, dict) and key in credentials:
        return str(credentials.get(key) or ""), f"credential:{key}"
    return "", ""


async def _run_oauth_steps(page: Any, steps: list[dict], credentials: dict | None, timeout_ms: int) -> list[dict]:
    results: list[dict] = []
    for index, raw_step in enumerate(steps[:30]):
        step = raw_step if isinstance(raw_step, dict) else {}
        step_type = str(step.get("type") or step.get("action") or "").strip().lower()
        selector = str(step.get("selector") or "")
        entry = {"index": index, "type": step_type, "selector_supplied": bool(selector)}
        try:
            if step_type == "navigate":
                target_url = str(step.get("url") or "")
                if not target_url:
                    raise ValueError("url is required")
                await page.goto(target_url, wait_until=str(step.get("wait_until") or "domcontentloaded"), timeout=timeout_ms)
                entry["status"] = "ok"
                entry["url_evidence"] = _url_evidence(target_url)
            elif step_type == "fill":
                if not selector:
                    raise ValueError("selector is required")
                value, source = _credential_value(step, credentials)
                await page.fill(selector, value, timeout=timeout_ms)
                entry["status"] = "ok"
                entry["value_source"] = source
                entry["value_length"] = len(value)
            elif step_type == "click":
                if not selector:
                    raise ValueError("selector is required")
                await page.click(selector, timeout=timeout_ms)
                entry["status"] = "ok"
            elif step_type == "press":
                if not selector:
                    raise ValueError("selector is required")
                await page.press(selector, str(step.get("key") or "Enter"), timeout=timeout_ms)
                entry["status"] = "ok"
                entry["key"] = str(step.get("key") or "Enter")
            elif step_type == "wait_for_selector":
                if not selector:
                    raise ValueError("selector is required")
                await page.wait_for_selector(selector, timeout=timeout_ms)
                entry["status"] = "ok"
            elif step_type == "wait_for_url":
                pattern = str(step.get("url") or step.get("pattern") or "")
                if not pattern:
                    raise ValueError("url or pattern is required")
                await page.wait_for_url(pattern, timeout=timeout_ms)
                entry["status"] = "ok"
            elif step_type == "check":
                if not selector:
                    raise ValueError("selector is required")
                await page.check(selector, timeout=timeout_ms)
                entry["status"] = "ok"
            else:
                raise ValueError("unsupported step type")
        except Exception as exc:
            entry["status"] = "failed"
            entry["error"] = _safe_text(exc, 500)
            results.append(entry)
            break
        results.append(entry)
    return results


@mcp.tool()
async def browser_auth_flow(
    action: str = "login_form",
    url: str | None = None,
    start_url: str | None = None,
    username_selector: str | None = None,
    password_selector: str | None = None,
    submit_selector: str | None = None,
    username: str | None = None,
    password: str | None = None,
    credentials: dict | None = None,
    steps: list[dict] | None = None,
    source: str = "localStorage",
    name: str | None = None,
    save_path: str | None = None,
    state_path: str | None = None,
    overwrite: bool = False,
    wait_ms: int = 5000,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    action_name = str(action or "login_form").strip().lower()
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, f"browser_auth_flow:{action_name}", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
        bounded_wait = max(1000, min(int(wait_ms or 5000), 60000))
        effective_url = url or start_url
        if action_name == "login_form":
            result = await _run_login_form(page, effective_url, username_selector, password_selector, submit_selector, username, password, bounded_wait)
            result.update({"action": action_name, "elapsed_ms": int((time.perf_counter() - started) * 1000), "raw_secret_returned": False})
            result.update(await _page_context(page, resolved_page_id))
            return result
        if action_name == "oauth_flow":
            flow_steps = steps if isinstance(steps, list) else []
            if not flow_steps and (effective_url or username_selector or password_selector or submit_selector):
                flow_steps = []
                if effective_url:
                    flow_steps.append({"type": "navigate", "url": effective_url})
                if username_selector:
                    flow_steps.append({"type": "fill", "selector": username_selector, "credential": "username"})
                if password_selector:
                    flow_steps.append({"type": "fill", "selector": password_selector, "credential": "password"})
                if submit_selector:
                    flow_steps.append({"type": "click", "selector": submit_selector})
            merged_credentials = dict(credentials or {})
            if username is not None and "username" not in merged_credentials:
                merged_credentials["username"] = username
            if password is not None and "password" not in merged_credentials:
                merged_credentials["password"] = password
            if not flow_steps:
                return {"success": False, "status": "failed", "action": action_name, "error": "steps or selectors are required", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
            results = await _run_oauth_steps(page, flow_steps, merged_credentials, bounded_wait)
            success = bool(results) and all(item.get("status") == "ok" for item in results)
            out = {"success": success, "status": "ok" if success else "failed", "action": action_name, "steps": results, "step_count": len(results), "credentials_supplied": sorted(str(k) for k in merged_credentials.keys()), "raw_secret_returned": False, "elapsed_ms": int((time.perf_counter() - started) * 1000)}
            out.update(await _page_context(page, resolved_page_id))
            return out
        if action_name == "extract_token":
            source_name = str(source or "localStorage").strip()
            source_key = source_name.lower()
            tokens: list[dict] = []
            if source_key in {"localstorage", "local"}:
                tokens.extend(await _storage_tokens(page, "localStorage", name))
            elif source_key in {"sessionstorage", "session"}:
                tokens.extend(await _storage_tokens(page, "sessionStorage", name))
            elif source_key == "cookie":
                cookies = await page.context.cookies()
                for cookie in cookies:
                    if not isinstance(cookie, dict):
                        continue
                    cookie_name = str(cookie.get("name") or "")
                    if name and cookie_name != name:
                        continue
                    if not name and not _token_candidate_name(cookie_name):
                        continue
                    tokens.append({"source": "cookie", "name": cookie_name, "domain": cookie.get("domain", ""), "path": cookie.get("path", ""), "httpOnly": bool(cookie.get("httpOnly")), "secure": bool(cookie.get("secure")), "token": _redact(cookie.get("value"))})
            elif source_key == "header":
                target_name = str(name or "").lower()
                for entry in list(getattr(browser_manager, "_network_requests", [])):
                    for side in ("request_headers", "response_headers"):
                        headers = entry.get(side) if isinstance(entry.get(side), dict) else {}
                        for header_name, header_value in headers.items():
                            header_text = str(header_name or "")
                            if target_name:
                                if header_text.lower() != target_name:
                                    continue
                            elif not _token_candidate_name(header_text):
                                continue
                            tokens.append({"source": "header", "name": header_text, "side": side, "request_id": entry.get("request_id") or entry.get("id"), "url_evidence": _url_evidence(str(entry.get("url") or "")), "token": _redact(header_value)})
            elif source_key == "url":
                current_url = str(page.url or "")
                parsed = urlparse(current_url)
                pairs = parse_qsl(parsed.query, keep_blank_values=True) + parse_qsl(parsed.fragment, keep_blank_values=True)
                for key, value in pairs:
                    if name and key != name:
                        continue
                    if not name and not _token_candidate_name(key):
                        continue
                    tokens.append({"source": "url", "name": key, "url_evidence": _url_evidence(current_url), "token": _redact(value)})
            else:
                return {"success": False, "status": "failed", "action": action_name, "error": "source must be header, cookie, localStorage, sessionStorage, or url", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
            out = {"success": True, "status": "ok", "action": action_name, "source": source_name, "name": name or "", "tokens": tokens, "count": len(tokens), "raw_token_returned": False, "elapsed_ms": int((time.perf_counter() - started) * 1000)}
            out.update(await _page_context(page, resolved_page_id))
            return out
        if action_name == "persist_session":
            target = _safe_write_path(save_path or "", overwrite)
            state = await page.context.storage_state(path=target)
            cookies = state.get("cookies") if isinstance(state, dict) else []
            origins = state.get("origins") if isinstance(state, dict) else []
            return {"success": True, "status": "ok", "action": action_name, "path": target, "cookie_count": len(cookies) if isinstance(cookies, list) else 0, "origin_count": len(origins) if isinstance(origins, list) else 0, "raw_secret_returned": False, "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id, "elapsed_ms": int((time.perf_counter() - started) * 1000)}
        if action_name == "restore_session":
            target = state_path or save_path or ""
            if not target:
                return {"success": False, "status": "failed", "action": action_name, "error": "state_path is required", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
            from .storage import import_state
            result = await import_state(target)
            status = "ok" if isinstance(result, dict) and not result.get("error") else "failed"
            out = {"success": status == "ok", "status": status, "action": action_name, "restore": result, "raw_secret_returned": False, "elapsed_ms": int((time.perf_counter() - started) * 1000)}
            out.update(await _page_context(page, resolved_page_id))
            return out
        return {"success": False, "status": "failed", "action": action_name, "error": "unknown action. Use login_form, oauth_flow, extract_token, persist_session, restore_session", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
    except Exception as exc:
        _camoufox_debug("browser_auth_flow_exception", action=action_name, error_type=type(exc).__name__, error_summary=_safe_text(exc, 500))
        return {"success": False, "status": "failed", "action": action_name, "error": str(exc), "error_type": type(exc).__name__, "active_page_id": browser_manager.active_page_id, "raw_secret_returned": False}
