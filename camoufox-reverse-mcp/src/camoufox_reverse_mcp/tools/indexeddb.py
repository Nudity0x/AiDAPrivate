from __future__ import annotations

import hashlib
import json
import os
import tempfile
import time
from typing import Any

from ..browser import _await_no_cancel_wait, _camoufox_debug, _safe_text
from ..server import mcp, browser_manager


_MAX_DB_NAME_LEN = 512
_MAX_STORE_NAME_LEN = 512
_MAX_RECORD_LIMIT = 5000
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


_INDEXEDDB_JS = r"""
async args => {
    const action = String(args.action || 'list_dbs');
    const maxString = Math.max(64, Math.min(Number(args.maxValueSize || 4096), 65536));
    const maxRecords = Math.max(1, Math.min(Number(args.limit || 100), 5000));
    const offset = Math.max(0, Number(args.offset || 0));
    const direction = String(args.direction || 'next');
    const dbName = String(args.dbName || '');
    const storeName = String(args.storeName || '');
    const indexName = String(args.indexName || '');
    const sensitiveParts = ['authorization','bearer','credential','passwd','password','private','secret','session','token','x-api-key','apikey','api_key'];
    function hashText(value) {
        const text = String(value ?? '');
        let h = 2166136261;
        for (let i = 0; i < text.length; i++) {
            h ^= text.charCodeAt(i);
            h = Math.imul(h, 16777619) >>> 0;
        }
        return 'fnv32:' + h.toString(16).padStart(8, '0');
    }
    function sensitiveKey(key) {
        const low = String(key || '').toLowerCase().replace(/_/g, '-');
        return sensitiveParts.some(part => low.includes(part));
    }
    function redactString(text, key) {
        let value = String(text ?? '');
        value = value.replace(/\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b/g, '<redacted:jwt>');
        value = value.replace(/\bbearer\s+[A-Za-z0-9._~+/=-]{12,}/gi, 'Bearer <redacted>');
        const redacted = value !== String(text ?? '') || sensitiveKey(key);
        const out = {type: 'string', length: String(text ?? '').length, hash: hashText(text), redacted};
        if (sensitiveKey(key)) return out;
        out.value = value.length > maxString ? value.slice(0, maxString) : value;
        out.truncated = value.length > maxString;
        return out;
    }
    function safeValue(value, key, depth, seen) {
        const t = typeof value;
        if (value === null || t === 'undefined') return value === null ? null : {type: 'undefined'};
        if (t === 'string') return redactString(value, key);
        if (t === 'number' || t === 'boolean') return value;
        if (t === 'bigint') return {type: 'bigint', value: value.toString()};
        if (t === 'function' || t === 'symbol') return {type: t, value: String(value).slice(0, 256)};
        if (value instanceof Date) return {type: 'Date', value: value.toISOString()};
        if (value instanceof ArrayBuffer) return {type: 'ArrayBuffer', byteLength: value.byteLength};
        if (ArrayBuffer.isView(value)) return {type: value.constructor ? value.constructor.name : 'TypedArray', byteLength: value.byteLength, length: value.length};
        if (typeof Blob !== 'undefined' && value instanceof Blob) return {type: 'Blob', size: value.size, contentType: value.type || ''};
        if (seen.has(value)) return {type: 'circular'};
        if (depth <= 0) return {type: value && value.constructor ? value.constructor.name : 'object', depth_limited: true};
        seen.add(value);
        if (Array.isArray(value)) {
            const items = value.slice(0, 128).map((item, index) => safeValue(item, String(index), depth - 1, seen));
            const out = {type: 'array', length: value.length, items};
            if (value.length > items.length) out.truncated = true;
            seen.delete(value);
            return out;
        }
        const keys = Object.keys(value);
        const out = {type: value && value.constructor ? value.constructor.name : 'object', fields: {}};
        for (const keyName of keys.slice(0, 128)) out.fields[keyName] = safeValue(value[keyName], keyName, depth - 1, seen);
        if (keys.length > 128) out.truncated_keys = keys.length - 128;
        seen.delete(value);
        return out;
    }
    function openDb(name) {
        return new Promise((resolve, reject) => {
            const req = indexedDB.open(name);
            req.onerror = () => reject(req.error || new Error('indexedDB open failed'));
            req.onblocked = () => reject(new Error('indexedDB open blocked'));
            req.onsuccess = () => resolve(req.result);
        });
    }
    function dbExists(name) {
        if (typeof indexedDB.databases !== 'function') return Promise.resolve({checked: false, exists: true});
        return indexedDB.databases().then(dbs => ({checked: true, exists: dbs.some(db => String(db.name || '') === name)}));
    }
    function requestToPromise(req) {
        return new Promise((resolve, reject) => {
            req.onerror = () => reject(req.error || new Error('indexedDB request failed'));
            req.onsuccess = () => resolve(req.result);
        });
    }
    function keyRange(spec) {
        if (!spec || typeof spec !== 'object') return null;
        if (Object.prototype.hasOwnProperty.call(spec, 'only')) return IDBKeyRange.only(spec.only);
        const hasLower = Object.prototype.hasOwnProperty.call(spec, 'lower');
        const hasUpper = Object.prototype.hasOwnProperty.call(spec, 'upper');
        const lowerOpen = !!spec.lowerOpen;
        const upperOpen = !!spec.upperOpen;
        if (hasLower && hasUpper) return IDBKeyRange.bound(spec.lower, spec.upper, lowerOpen, upperOpen);
        if (hasLower) return IDBKeyRange.lowerBound(spec.lower, lowerOpen);
        if (hasUpper) return IDBKeyRange.upperBound(spec.upper, upperOpen);
        return null;
    }
    async function listStores(name) {
        const exists = await dbExists(name);
        if (exists.checked && !exists.exists) return {exists: false, stores: []};
        const db = await openDb(name);
        try {
            const names = Array.from(db.objectStoreNames || []);
            if (!names.length) return {exists: true, version: db.version, stores: []};
            const tx = db.transaction(names, 'readonly');
            const stores = [];
            for (const name of names) {
                const store = tx.objectStore(name);
                stores.push({
                    name,
                    keyPath: store.keyPath,
                    autoIncrement: !!store.autoIncrement,
                    indexNames: Array.from(store.indexNames || []).slice(0, 256)
                });
            }
            return {exists: true, version: db.version, stores};
        } finally {
            db.close();
        }
    }
    async function queryStore(name, store) {
        const exists = await dbExists(name);
        if (exists.checked && !exists.exists) return {exists: false, records: []};
        const db = await openDb(name);
        try {
            if (!Array.from(db.objectStoreNames || []).includes(store)) {
                return {exists: true, store_exists: false, records: []};
            }
            const tx = db.transaction(store, 'readonly');
            const objectStore = tx.objectStore(store);
            if (indexName && !Array.from(objectStore.indexNames || []).includes(indexName)) {
                return {exists: true, store_exists: true, index_exists: false, index_name: indexName, records: []};
            }
            const cursorSource = indexName ? objectStore.index(indexName) : objectStore;
            const range = keyRange(args.keyRange);
            const records = [];
            let skipped = 0;
            await new Promise((resolve, reject) => {
                const req = cursorSource.openCursor(range, direction);
                req.onerror = () => reject(req.error || new Error('openCursor failed'));
                req.onsuccess = event => {
                    const cursor = event.target.result;
                    if (!cursor || records.length >= maxRecords) {
                        resolve();
                        return;
                    }
                    if (skipped < offset) {
                        skipped += 1;
                        cursor.continue();
                        return;
                    }
                    records.push({
                        key: safeValue(cursor.key, 'key', 4, new WeakSet()),
                        primary_key: safeValue(cursor.primaryKey, 'primary_key', 4, new WeakSet()),
                        value: safeValue(cursor.value, '', 6, new WeakSet())
                    });
                    cursor.continue();
                };
            });
            return {exists: true, store_exists: true, index_exists: indexName ? true : null, index_name: indexName, version: db.version, records, returned_count: records.length, offset, limit: maxRecords};
        } finally {
            db.close();
        }
    }
    async function exportDb(name, selectedStore) {
        const meta = await listStores(name);
        if (!meta.exists) return {exists: false, stores: []};
        const stores = selectedStore ? meta.stores.filter(store => store.name === selectedStore) : meta.stores;
        const out = {exists: true, version: meta.version, stores: []};
        for (const store of stores) {
            const result = await queryStore(name, store.name);
            out.stores.push({metadata: store, records: result.records || [], returned_count: result.returned_count || 0, limit: maxRecords});
        }
        return out;
    }
    if (!('indexedDB' in window)) return {supported: false, error: 'indexedDB is not available'};
    if (action === 'list_dbs') {
        if (typeof indexedDB.databases !== 'function') return {supported: false, error: 'indexedDB.databases is not available'};
        const dbs = await indexedDB.databases();
        return {supported: true, databases: dbs.map(db => ({name: String(db.name || ''), version: db.version || 0})), count: dbs.length};
    }
    if (!dbName) return {supported: true, error: 'db_name is required'};
    if (action === 'list_stores') return {supported: true, ...(await listStores(dbName))};
    if (action === 'query') {
        if (!storeName) return {supported: true, error: 'store_name is required'};
        return {supported: true, ...(await queryStore(dbName, storeName))};
    }
    if (action === 'export') return {supported: true, ...(await exportDb(dbName, storeName))};
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


def _timeout_ms(value: Any) -> int:
    return _bounded_int(value, 30000, 250, 120000)


def _require_name(value: Any, name: str, max_len: int) -> str:
    text = str(value or "").strip()
    if not text:
        raise ValueError(f"{name} is required")
    if len(text) > max_len:
        raise ValueError(f"{name} exceeds {max_len} characters")
    return text


def _direction(value: Any) -> str:
    text = str(value or "next").strip().lower()
    if text not in {"next", "nextunique", "prev", "prevunique"}:
        raise ValueError("direction must be next, nextunique, prev, or prevunique")
    return text


def _hash_text(value: Any) -> str:
    text = str(value if value is not None else "")
    return hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()


def _default_export_path(db_name: str) -> str:
    root = os.path.join(tempfile.gettempdir(), "AiDA", "camoufox-indexeddb")
    os.makedirs(root, exist_ok=True)
    safe_name = "".join(ch if ch.isalnum() or ch in {"-", "_"} else "_" for ch in db_name)[:80] or "indexeddb"
    return os.path.join(root, f"{safe_name}_{int(time.time() * 1000)}.json")


def _resolve_export_path(save_path: Any, db_name: str) -> str:
    if not save_path:
        return _default_export_path(db_name)
    expanded = os.path.abspath(os.path.expandvars(os.path.expanduser(str(save_path))))
    if expanded.endswith(os.sep) or (os.path.exists(expanded) and os.path.isdir(expanded)):
        os.makedirs(expanded, exist_ok=True)
        safe_name = "".join(ch if ch.isalnum() or ch in {"-", "_"} else "_" for ch in db_name)[:80] or "indexeddb"
        return os.path.join(expanded, f"{safe_name}_{int(time.time() * 1000)}.json")
    os.makedirs(os.path.dirname(expanded), exist_ok=True)
    return expanded


async def _run_indexeddb(page, args: dict[str, Any], timeout_ms: int) -> dict[str, Any]:
    result = await _await_no_cancel_wait(page.evaluate(_INDEXEDDB_JS, args), timeout=timeout_ms / 1000.0)
    return result if isinstance(result, dict) else {"error": f"unexpected result type: {type(result).__name__}"}


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
async def browser_indexeddb(
    action: str = "list_dbs",
    database: str | None = None,
    store: str | None = None,
    db_name: str | None = None,
    store_name: str | None = None,
    index: str | None = None,
    key_range: dict | None = None,
    limit: int = 100,
    offset: int = 0,
    direction: str = "next",
    save_path: str | None = None,
    max_records: int | None = None,
    max_value_size: int = 4096,
    timeout_ms: int = 30000,
    payload: dict | None = None,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    selected_action = str(_payload_value(payload, "action", action) or "list_dbs").strip().lower()
    aliases = {
        "list_databases": "list_dbs",
        "databases": "list_dbs",
        "list": "list_dbs",
        "stores": "list_stores",
        "list_store": "list_stores",
        "dump": "export",
    }
    selected_action = aliases.get(selected_action, selected_action)
    effective_page_id = _payload_value(payload, "page_id", page_id)
    timeout = _timeout_ms(_payload_value(payload, "timeout_ms", timeout_ms))
    try:
        page = await browser_manager.resolve_page_for_operation(effective_page_id, f"browser_indexeddb:{selected_action}", True, aida_operation_id)
        effective_db_name = ""
        effective_store_name = ""
        if selected_action != "list_dbs":
            db_value = _payload_value(payload, "db_name", db_name)
            if db_value is None:
                db_value = _payload_value(payload, "database", database)
            effective_db_name = _require_name(db_value, "db_name", _MAX_DB_NAME_LEN)
        if selected_action in {"query"}:
            store_value = _payload_value(payload, "store_name", store_name)
            if store_value is None:
                store_value = _payload_value(payload, "store", store)
            effective_store_name = _require_name(store_value, "store_name", _MAX_STORE_NAME_LEN)
        elif selected_action == "export":
            store_value = _payload_value(payload, "store_name", store_name)
            if store_value is None:
                store_value = _payload_value(payload, "store", store)
            if store_value:
                effective_store_name = _require_name(store_value, "store_name", _MAX_STORE_NAME_LEN)
        effective_limit = _bounded_int(_payload_value(payload, "max_records", max_records) if max_records is not None else _payload_value(payload, "limit", limit), 100, 1, _MAX_RECORD_LIMIT)
        args = {
            "action": selected_action,
            "dbName": effective_db_name,
            "storeName": effective_store_name,
            "indexName": _payload_value(payload, "index", index) or "",
            "keyRange": _payload_value(payload, "key_range", key_range) or {},
            "limit": effective_limit,
            "offset": _bounded_int(_payload_value(payload, "offset", offset), 0, 0, 1000000),
            "direction": _direction(_payload_value(payload, "direction", direction)),
            "maxValueSize": _bounded_int(_payload_value(payload, "max_value_size", max_value_size), 4096, 64, 65536),
        }
        result = await _run_indexeddb(page, args, timeout)
        if result.get("error"):
            out = await _base(selected_action, page, "error", False)
            out.update(result)
            out["elapsed_ms"] = int((time.perf_counter() - started) * 1000)
            return out
        if selected_action == "export":
            path = _resolve_export_path(_payload_value(payload, "save_path", save_path), effective_db_name)
            document = {
                "exported_ms": int(time.time() * 1000),
                "db_name": effective_db_name,
                "store_name": effective_store_name,
                "record_limit_per_store": effective_limit,
                "data": result,
            }
            with open(path, "w", encoding="utf-8") as fp:
                json.dump(document, fp, ensure_ascii=False, indent=2)
            result = {
                "path": path,
                "size": os.path.getsize(path),
                "db_name": effective_db_name,
                "store_name": effective_store_name,
                "store_count": len(result.get("stores") or []),
                "record_count": sum(int(store.get("returned_count") or 0) for store in (result.get("stores") or []) if isinstance(store, dict)),
                "sha256": _hash_text(json.dumps(document, sort_keys=True, default=str)),
            }
        status = "ok" if result.get("supported", True) is not False and result.get("exists", True) is not False else "unavailable"
        out = await _base(selected_action, page, status, status == "ok")
        out.update(result)
        out["elapsed_ms"] = int((time.perf_counter() - started) * 1000)
        return out
    except Exception as exc:
        _camoufox_debug("browser_indexeddb_error", action=selected_action, page_id=str(effective_page_id or ""), error_type=type(exc).__name__, error_summary=_safe_text(exc, 700))
        return _error(selected_action, exc, str(effective_page_id or ""))
