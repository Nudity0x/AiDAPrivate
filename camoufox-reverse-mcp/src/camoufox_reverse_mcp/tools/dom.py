from __future__ import annotations

import asyncio
import hashlib
import re
import time
from typing import Any

from ..browser import _await_no_cancel_wait, _camoufox_debug, _safe_text
from ..server import mcp, browser_manager


_MAX_SELECTOR_LEN = 2048
_MAX_TEXT_LEN = 262144
_MAX_HTML_LEN = 262144
_SENSITIVE_PARTS = (
    "authorization",
    "bearer",
    "credential",
    "passwd",
    "password",
    "private",
    "secret",
    "session",
    "token",
    "x-api-key",
    "apikey",
    "api_key",
)
_JWT_RE = re.compile(r"\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b")
_BEARER_RE = re.compile(r"(?i)\bbearer\s+[A-Za-z0-9._~+/=-]{12,}")
_ASSIGN_SECRET_RE = re.compile(r"(?i)\b(password|passwd|pwd|secret|token|session|api[_-]?key|authorization)\s*[:=]\s*['\"]?[^'\"\s&;,)}]{4,}")


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


def _bounded_timeout_ms(value: Any) -> int:
    return _bounded_int(value, 30000, 250, 120000)


def _require_selector(selector: Any) -> str:
    text = str(selector or "").strip()
    if not text:
        raise ValueError("selector is required")
    if len(text) > _MAX_SELECTOR_LEN:
        raise ValueError(f"selector exceeds {_MAX_SELECTOR_LEN} characters")
    return text


def _hash_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8", "replace")).hexdigest()


def _sensitive_key(name: Any) -> bool:
    low = str(name or "").strip().lower().replace("_", "-")
    return any(part in low for part in _SENSITIVE_PARTS)


def _redact_text(value: Any, limit: int = 4096, key: str = "") -> dict[str, Any]:
    text = str(value or "")
    sensitive = _sensitive_key(key)
    redacted = _JWT_RE.sub("<redacted:jwt>", text)
    redacted = _BEARER_RE.sub("Bearer <redacted>", redacted)
    redacted = _ASSIGN_SECRET_RE.sub(lambda m: f"{m.group(1)}=<redacted>", redacted)
    if sensitive:
        return {
            "redacted": True,
            "length": len(text),
            "sha256": _hash_text(text),
        }
    truncated = len(redacted) > limit
    return {
        "value": redacted[:limit] if truncated else redacted,
        "length": len(text),
        "sha256": _hash_text(text),
        "truncated": truncated,
        "redacted": redacted != text,
    }


def _sanitize_attrs(attrs: Any) -> dict[str, Any]:
    if not isinstance(attrs, dict):
        return {}
    out: dict[str, Any] = {}
    for key, value in attrs.items():
        out[str(key)] = _redact_text(value, 1024, str(key))
    return out


def _sanitize_node(node: Any) -> Any:
    if isinstance(node, list):
        return [_sanitize_node(item) for item in node]
    if not isinstance(node, dict):
        return node
    out: dict[str, Any] = {}
    for key, value in node.items():
        if key == "attributes":
            out[key] = _sanitize_attrs(value)
        elif key in {"text", "value"}:
            out[key] = _redact_text(value, 2048, str(key))
        elif key == "children" and isinstance(value, list):
            out[key] = [_sanitize_node(child) for child in value]
        else:
            out[key] = value
    return out


async def _page_eval(page, script: str, arg: Any, timeout_ms: int) -> Any:
    return await _await_no_cancel_wait(page.evaluate(script, arg), timeout=timeout_ms / 1000.0)


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


async def _dom_tree(page, selector: str | None, max_depth: int, max_children: int, max_nodes: int, include_text: bool, include_attributes: bool, timeout_ms: int) -> dict[str, Any]:
    result = await _page_eval(page, """args => {
        const rootSelector = String(args.selector || '').trim();
        const maxDepth = Number(args.maxDepth || 4);
        const maxChildren = Number(args.maxChildren || 64);
        const maxNodes = Number(args.maxNodes || 500);
        const includeText = !!args.includeText;
        const includeAttributes = !!args.includeAttributes;
        const root = rootSelector ? document.querySelector(rootSelector) : (document.body || document.documentElement);
        const counters = {seen: 0, truncated: false};
        function ownText(node) {
            const parts = [];
            for (const child of Array.from(node.childNodes || [])) {
                if (child.nodeType === Node.TEXT_NODE) {
                    const text = String(child.textContent || '').replace(/\\s+/g, ' ').trim();
                    if (text) parts.push(text);
                }
            }
            return parts.join(' ').slice(0, 4096);
        }
        function cssPath(node) {
            if (!node || node.nodeType !== Node.ELEMENT_NODE) return '';
            const parts = [];
            let current = node;
            while (current && current.nodeType === Node.ELEMENT_NODE && parts.length < 12) {
                let part = current.tagName.toLowerCase();
                if (current.id) {
                    part += '#' + CSS.escape(current.id);
                    parts.unshift(part);
                    break;
                }
                const cls = Array.from(current.classList || []).slice(0, 3).map(x => CSS.escape(x));
                if (cls.length) part += '.' + cls.join('.');
                const parent = current.parentElement;
                if (parent) {
                    const siblings = Array.from(parent.children).filter(x => x.tagName === current.tagName);
                    if (siblings.length > 1) part += ':nth-of-type(' + (siblings.indexOf(current) + 1) + ')';
                }
                parts.unshift(part);
                current = parent;
            }
            return parts.join(' > ');
        }
        function attrs(node) {
            const out = {};
            if (!includeAttributes) return out;
            for (const attr of Array.from(node.attributes || [])) out[attr.name] = String(attr.value || '').slice(0, 4096);
            return out;
        }
        function walk(node, depth) {
            if (!node || node.nodeType !== Node.ELEMENT_NODE) return null;
            if (counters.seen >= maxNodes) {
                counters.truncated = true;
                return null;
            }
            counters.seen += 1;
            const item = {
                tag: node.tagName.toLowerCase(),
                selector: cssPath(node),
                id: node.id || '',
                classes: Array.from(node.classList || []).slice(0, 16),
                attributes: attrs(node)
            };
            if (includeText) item.text = ownText(node);
            if (node instanceof HTMLInputElement || node instanceof HTMLTextAreaElement || node instanceof HTMLSelectElement) {
                item.input_type = node.getAttribute('type') || node.tagName.toLowerCase();
                item.value = String(node.value || '').slice(0, 4096);
            }
            if (depth < maxDepth) {
                const children = [];
                const rawChildren = Array.from(node.children || []);
                for (const child of rawChildren.slice(0, maxChildren)) {
                    const next = walk(child, depth + 1);
                    if (next) children.push(next);
                }
                if (rawChildren.length > maxChildren) {
                    item.children_truncated = rawChildren.length - maxChildren;
                    counters.truncated = true;
                }
                if (children.length) item.children = children;
            } else if ((node.children || []).length) {
                item.children_truncated = node.children.length;
                counters.truncated = true;
            }
            return item;
        }
        if (!root) return {found: false, selector: rootSelector, node_count: 0, truncated: false, tree: null};
        return {
            found: true,
            selector: rootSelector || 'body',
            node_count: counters.seen,
            truncated: counters.truncated,
            tree: walk(root, 0)
        };
    }""", {
        "selector": selector or "",
        "maxDepth": max_depth,
        "maxChildren": max_children,
        "maxNodes": max_nodes,
        "includeText": include_text,
        "includeAttributes": include_attributes,
    }, timeout_ms)
    result["tree"] = _sanitize_node(result.get("tree"))
    return result


async def _dom_styles(page, selector: str, properties: list[str] | None, timeout_ms: int) -> dict[str, Any]:
    props = [str(item).strip() for item in (properties or []) if str(item).strip()]
    if len(props) > 128:
        props = props[:128]
    return await _page_eval(page, """args => {
        const node = document.querySelector(String(args.selector || ''));
        if (!node) return {found: false, selector: args.selector, styles: {}};
        const computed = getComputedStyle(node);
        const wanted = Array.isArray(args.properties) && args.properties.length
            ? args.properties
            : ['display','position','visibility','opacity','z-index','width','height','color','background-color','font-family','font-size','font-weight','line-height','margin-top','margin-right','margin-bottom','margin-left','padding-top','padding-right','padding-bottom','padding-left','border-top-width','border-right-width','border-bottom-width','border-left-width','overflow','pointer-events'];
        const styles = {};
        for (const name of wanted) styles[name] = computed.getPropertyValue(name);
        return {found: true, selector: args.selector, styles};
    }""", {"selector": selector, "properties": props}, timeout_ms)


async def _dom_listeners(page, selector: str, limit: int, timeout_ms: int) -> dict[str, Any]:
    return await _page_eval(page, """args => {
        const selector = String(args.selector || '');
        const limit = Math.max(1, Math.min(Number(args.limit || 200), 1000));
        const commonEvents = ['click','dblclick','mousedown','mouseup','mousemove','mouseenter','mouseleave','mouseover','mouseout','keydown','keyup','keypress','input','change','submit','focus','blur','load','error','touchstart','touchmove','touchend','pointerdown','pointerup','pointermove','dragstart','drag','dragend','drop'];
        function cssPath(node) {
            if (!node || node.nodeType !== Node.ELEMENT_NODE) return '';
            const parts = [];
            let current = node;
            while (current && current.nodeType === Node.ELEMENT_NODE && parts.length < 12) {
                let part = current.tagName.toLowerCase();
                if (current.id) {
                    part += '#' + CSS.escape(current.id);
                    parts.unshift(part);
                    break;
                }
                const parent = current.parentElement;
                if (parent) {
                    const siblings = Array.from(parent.children).filter(x => x.tagName === current.tagName);
                    if (siblings.length > 1) part += ':nth-of-type(' + (siblings.indexOf(current) + 1) + ')';
                }
                parts.unshift(part);
                current = parent;
            }
            return parts.join(' > ');
        }
        function ensureTracker() {
            if (window.__aida_listener_tracker && window.__aida_listener_tracker.installed) return window.__aida_listener_tracker;
            const state = {
                installed: false,
                items: [],
                nextId: 1,
                originalAdd: EventTarget.prototype.addEventListener,
                originalRemove: EventTarget.prototype.removeEventListener
            };
            function targetId(target) {
                if (!target) return 0;
                try {
                    if (!Object.prototype.hasOwnProperty.call(target, '__aida_listener_target_id')) {
                        Object.defineProperty(target, '__aida_listener_target_id', {value: state.nextId++, enumerable: false});
                    }
                    return target.__aida_listener_target_id || 0;
                } catch(e) {
                    return 0;
                }
            }
            function describe(target) {
                if (!target) return {target_id: 0, target_type: ''};
                const id = targetId(target);
                const out = {target_id: id, target_type: target.constructor ? target.constructor.name : typeof target};
                if (target.nodeType === Node.ELEMENT_NODE) {
                    out.tag = target.tagName.toLowerCase();
                    out.selector = cssPath(target);
                    out.id = target.id || '';
                    out.classes = Array.from(target.classList || []).slice(0, 8);
                } else if (target === window) {
                    out.selector = 'window';
                } else if (target === document) {
                    out.selector = 'document';
                }
                return out;
            }
            EventTarget.prototype.addEventListener = function(type, listener, options) {
                const detail = describe(this);
                const capture = typeof options === 'boolean' ? options : !!(options && options.capture);
                const passive = typeof options === 'object' && options ? !!options.passive : false;
                const once = typeof options === 'object' && options ? !!options.once : false;
                state.items.push({
                    event_type: String(type || ''),
                    listener_type: listener && listener.constructor ? listener.constructor.name : typeof listener,
                    capture,
                    passive,
                    once,
                    ts_ms: Date.now(),
                    stack: String((new Error()).stack || '').split('\\n').slice(2, 10).join('\\n').slice(0, 2000),
                    ...detail
                });
                while (state.items.length > 2000) state.items.shift();
                return state.originalAdd.apply(this, arguments);
            };
            EventTarget.prototype.removeEventListener = function(type, listener, options) {
                const detail = describe(this);
                state.items.push({
                    event_type: String(type || ''),
                    listener_type: listener && listener.constructor ? listener.constructor.name : typeof listener,
                    removed: true,
                    capture: typeof options === 'boolean' ? options : !!(options && options.capture),
                    ts_ms: Date.now(),
                    ...detail
                });
                while (state.items.length > 2000) state.items.shift();
                return state.originalRemove.apply(this, arguments);
            };
            state.installed = true;
            window.__aida_listener_tracker = state;
            return state;
        }
        const tracker = ensureTracker();
        const node = document.querySelector(selector);
        if (!node) return {found: false, selector, instrumentation_installed: !!tracker.installed, captured: [], inline: [], properties: []};
        const nodeId = (() => {
            try {
                if (!Object.prototype.hasOwnProperty.call(node, '__aida_listener_target_id')) {
                    Object.defineProperty(node, '__aida_listener_target_id', {value: tracker.nextId++, enumerable: false});
                }
                return node.__aida_listener_target_id || 0;
            } catch(e) {
                return 0;
            }
        })();
        const captured = tracker.items.filter(item => item.target_id === nodeId).slice(-limit);
        const inline = [];
        for (const attr of Array.from(node.attributes || [])) {
            if (String(attr.name || '').toLowerCase().startsWith('on')) {
                inline.push({event_type: attr.name.slice(2), source_length: String(attr.value || '').length, source_preview: String(attr.value || '').slice(0, 300)});
            }
        }
        const properties = [];
        for (const type of commonEvents) {
            const key = 'on' + type;
            if (typeof node[key] === 'function') properties.push({event_type: type, handler_type: node[key].constructor ? node[key].constructor.name : 'function'});
        }
        return {found: true, selector, instrumentation_installed: !!tracker.installed, target_id: nodeId, captured, inline, properties, captured_count: captured.length};
    }""", {"selector": selector, "limit": limit}, timeout_ms)


async def _dom_box_model(page, selector: str, timeout_ms: int) -> dict[str, Any]:
    return await _page_eval(page, """args => {
        const node = document.querySelector(String(args.selector || ''));
        if (!node) return {found: false, selector: args.selector};
        const rect = node.getBoundingClientRect();
        const cs = getComputedStyle(node);
        function box(prefix) {
            return {
                top: parseFloat(cs.getPropertyValue(prefix + '-top-width') || cs.getPropertyValue(prefix + '-top') || '0') || 0,
                right: parseFloat(cs.getPropertyValue(prefix + '-right-width') || cs.getPropertyValue(prefix + '-right') || '0') || 0,
                bottom: parseFloat(cs.getPropertyValue(prefix + '-bottom-width') || cs.getPropertyValue(prefix + '-bottom') || '0') || 0,
                left: parseFloat(cs.getPropertyValue(prefix + '-left-width') || cs.getPropertyValue(prefix + '-left') || '0') || 0
            };
        }
        return {
            found: true,
            selector: args.selector,
            rect: {x: rect.x, y: rect.y, width: rect.width, height: rect.height, top: rect.top, right: rect.right, bottom: rect.bottom, left: rect.left},
            margin: box('margin'),
            border: box('border'),
            padding: box('padding'),
            scroll: {width: node.scrollWidth, height: node.scrollHeight, top: node.scrollTop, left: node.scrollLeft},
            client: {width: node.clientWidth, height: node.clientHeight, top: node.clientTop, left: node.clientLeft},
            offset: {width: node.offsetWidth, height: node.offsetHeight, top: node.offsetTop, left: node.offsetLeft}
        };
    }""", {"selector": selector}, timeout_ms)


async def _dom_attributes(page, action: str, selector: str, attribute_name: str | None, attribute_value: str | None, timeout_ms: int) -> dict[str, Any]:
    if action != "list_attributes" and not str(attribute_name or "").strip():
        raise ValueError("attribute_name is required")
    if attribute_value is not None and len(str(attribute_value)) > _MAX_TEXT_LEN:
        raise ValueError(f"attribute_value exceeds {_MAX_TEXT_LEN} characters")
    result = await _page_eval(page, """args => {
        const node = document.querySelector(String(args.selector || ''));
        if (!node) return {found: false, selector: args.selector, attributes: {}};
        const name = String(args.attributeName || '').trim();
        const action = String(args.action || '');
        if (action === 'set_attributes') {
            node.setAttribute(name, String(args.attributeValue ?? ''));
        } else if (action === 'remove_attributes') {
            node.removeAttribute(name);
        }
        const attrs = {};
        for (const attr of Array.from(node.attributes || [])) attrs[attr.name] = String(attr.value || '');
        return {found: true, selector: args.selector, attribute_name: name, attributes: attrs};
    }""", {
        "selector": selector,
        "action": action,
        "attributeName": attribute_name or "",
        "attributeValue": attribute_value if attribute_value is not None else "",
    }, timeout_ms)
    result["attributes"] = _sanitize_attrs(result.get("attributes"))
    if attribute_value is not None:
        result["attribute_value"] = _redact_text(attribute_value, 1024, attribute_name or "")
    return result


async def _dom_text(page, action: str, selector: str, text: str | None, timeout_ms: int) -> dict[str, Any]:
    if action == "set_text" and text is None:
        raise ValueError("text is required")
    if text is not None and len(str(text)) > _MAX_TEXT_LEN:
        raise ValueError(f"text exceeds {_MAX_TEXT_LEN} characters")
    result = await _page_eval(page, """args => {
        const node = document.querySelector(String(args.selector || ''));
        if (!node) return {found: false, selector: args.selector};
        if (args.action === 'set_text') node.textContent = String(args.text ?? '');
        const value = String(node.textContent || '');
        return {found: true, selector: args.selector, text: value};
    }""", {"selector": selector, "action": action, "text": text if text is not None else ""}, timeout_ms)
    result["text"] = _redact_text(result.get("text", ""), 8192, "text")
    return result


async def _dom_insert_html(page, selector: str, html: str | None, position: str, timeout_ms: int) -> dict[str, Any]:
    if html is None:
        raise ValueError("html is required")
    html_text = str(html)
    if len(html_text) > _MAX_HTML_LEN:
        raise ValueError(f"html exceeds {_MAX_HTML_LEN} characters")
    pos = str(position or "beforeend").strip().lower()
    if pos not in {"beforebegin", "afterbegin", "beforeend", "afterend"}:
        raise ValueError("position must be beforebegin, afterbegin, beforeend, or afterend")
    result = await _page_eval(page, """args => {
        const node = document.querySelector(String(args.selector || ''));
        if (!node) return {found: false, selector: args.selector, inserted: false};
        node.insertAdjacentHTML(args.position, String(args.html || ''));
        return {found: true, selector: args.selector, inserted: true, html_length: String(args.html || '').length, position: args.position};
    }""", {"selector": selector, "html": html_text, "position": pos}, timeout_ms)
    result["html_sha256"] = _hash_text(html_text)
    return result


async def _dom_remove_node(page, selector: str, timeout_ms: int) -> dict[str, Any]:
    result = await _page_eval(page, """args => {
        const node = document.querySelector(String(args.selector || ''));
        if (!node) return {found: false, selector: args.selector, removed: false};
        const summary = {
            tag: node.tagName ? node.tagName.toLowerCase() : '',
            id: node.id || '',
            classes: Array.from(node.classList || []).slice(0, 16),
            text_length: String(node.textContent || '').length,
            child_count: node.children ? node.children.length : 0
        };
        node.remove();
        return {found: true, selector: args.selector, removed: true, removed_node: summary};
    }""", {"selector": selector}, timeout_ms)
    return result


@mcp.tool()
async def browser_dom(
    action: str = "tree",
    selector: str | None = None,
    attribute: str | None = None,
    attribute_name: str | None = None,
    value: Any = None,
    attribute_value: str | None = None,
    text: str | None = None,
    html: str | None = None,
    position: str = "beforeend",
    properties: list[str] | None = None,
    include_text: bool = True,
    include_attributes: bool = True,
    max_depth: int = 4,
    max_children: int = 64,
    max_nodes: int = 500,
    limit: int = 200,
    timeout_ms: int = 30000,
    payload: dict | None = None,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    selected_action = str(_payload_value(payload, "action", action) or "tree").strip().lower()
    aliases = {
        "attr_list": "list_attributes",
        "attribute_list": "list_attributes",
        "attributes": "list_attributes",
        "list_attribute": "list_attributes",
        "list_attributes": "list_attributes",
        "attr_set": "set_attributes",
        "attribute_set": "set_attributes",
        "set_attr": "set_attributes",
        "set_attribute": "set_attributes",
        "set_attributes": "set_attributes",
        "attr_remove": "remove_attributes",
        "attribute_remove": "remove_attributes",
        "remove_attr": "remove_attributes",
        "remove_attribute": "remove_attributes",
        "remove_attributes": "remove_attributes",
        "get_text": "get_text",
        "text_get": "get_text",
        "set_text": "set_text",
        "text_set": "set_text",
        "box": "box_model",
        "box_model": "box_model",
        "insert": "insert_html",
        "insert_html": "insert_html",
        "remove": "remove_node",
        "remove_node": "remove_node",
        "listeners": "listeners",
        "styles": "styles",
        "tree": "tree",
    }
    selected_action = aliases.get(selected_action, selected_action)
    effective_page_id = _payload_value(payload, "page_id", page_id)
    timeout = _bounded_timeout_ms(_payload_value(payload, "timeout_ms", timeout_ms))
    try:
        page = await browser_manager.resolve_page_for_operation(effective_page_id, f"browser_dom:{selected_action}", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or effective_page_id or ""
        effective_selector = _payload_value(payload, "selector", selector)
        if selected_action == "tree":
            result = await _dom_tree(
                page,
                str(effective_selector or "").strip() or None,
                _bounded_int(_payload_value(payload, "max_depth", max_depth), 4, 0, 12),
                _bounded_int(_payload_value(payload, "max_children", max_children), 64, 1, 256),
                _bounded_int(_payload_value(payload, "max_nodes", max_nodes), 500, 1, 5000),
                bool(_payload_value(payload, "include_text", include_text)),
                bool(_payload_value(payload, "include_attributes", include_attributes)),
                timeout,
            )
        elif selected_action == "styles":
            result = await _dom_styles(page, _require_selector(effective_selector), _payload_value(payload, "properties", properties), timeout)
        elif selected_action == "listeners":
            result = await _dom_listeners(page, _require_selector(effective_selector), _bounded_int(_payload_value(payload, "limit", limit), 200, 1, 1000), timeout)
        elif selected_action == "box_model":
            result = await _dom_box_model(page, _require_selector(effective_selector), timeout)
        elif selected_action in {"list_attributes", "set_attributes", "remove_attributes"}:
            effective_attribute_name = _payload_value(payload, "attribute_name", attribute_name)
            if effective_attribute_name is None:
                effective_attribute_name = _payload_value(payload, "attribute", attribute)
            effective_attribute_value = _payload_value(payload, "attribute_value", attribute_value)
            if effective_attribute_value is None:
                effective_attribute_value = _payload_value(payload, "value", value)
            result = await _dom_attributes(
                page,
                selected_action,
                _require_selector(effective_selector),
                effective_attribute_name,
                effective_attribute_value,
                timeout,
            )
        elif selected_action in {"get_text", "set_text"}:
            effective_text = _payload_value(payload, "text", text)
            if effective_text is None:
                effective_text = _payload_value(payload, "value", value)
            result = await _dom_text(page, selected_action, _require_selector(effective_selector), effective_text, timeout)
        elif selected_action == "insert_html":
            result = await _dom_insert_html(page, _require_selector(effective_selector), _payload_value(payload, "html", html), _payload_value(payload, "position", position), timeout)
        elif selected_action == "remove_node":
            result = await _dom_remove_node(page, _require_selector(effective_selector), timeout)
        else:
            return _error(selected_action, f"unknown action: {selected_action}", str(effective_page_id or ""))
        out = await _base(selected_action, page, "ok" if result.get("found", True) is not False else "not_found", result.get("found", True) is not False)
        out.update(result)
        out["elapsed_ms"] = int((time.perf_counter() - started) * 1000)
        out["page_id"] = resolved_page_id
        return out
    except asyncio.TimeoutError:
        return _error(selected_action, f"browser_dom timed out after {timeout}ms", str(effective_page_id or ""))
    except Exception as exc:
        _camoufox_debug("browser_dom_error", action=selected_action, page_id=str(effective_page_id or ""), error_type=type(exc).__name__, error_summary=_safe_text(exc, 700))
        return _error(selected_action, exc, str(effective_page_id or ""))
