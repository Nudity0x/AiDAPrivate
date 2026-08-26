from __future__ import annotations

import base64
import binascii
import time
from typing import Any

from ..browser import _await_no_cancel_wait, _camoufox_debug, _safe_text
from ..server import browser_manager, mcp


_WASM_HOOK_NAME = "aida:wasm"
_WASM_MAX_BYTES = 2 * 1024 * 1024
_WASM_HOOK = r"""
(() => {
  const existing = window.__aida_wasm_debug;
  if (existing && existing.version === 1 && existing.installed) return existing.snapshot();
  const Native = window.WebAssembly;
  const state = existing || {
    version: 1,
    installed: false,
    installedAt: Date.now(),
    nextModuleId: 1,
    nextInstanceId: 1,
    nextMemoryId: 1,
    modules: {},
    instances: {},
    memories: {},
    traces: {},
    moduleIds: new WeakMap(),
    instanceIds: new WeakMap(),
    memoryIds: new WeakMap(),
    install_errors: [],
    instanceObjects: [],
    memoryObjects: []
  };
  state.version = 1;
  state.maxBytes = 2097152;
  function bytesFrom(source) {
    try {
      if (source instanceof ArrayBuffer) return new Uint8Array(source);
      if (ArrayBuffer.isView(source)) return new Uint8Array(source.buffer, source.byteOffset, source.byteLength);
    } catch (e) {}
    return null;
  }
  function b64(bytes) {
    if (!bytes || bytes.byteLength > state.maxBytes) return "";
    let binary = "";
    const chunk = 0x8000;
    for (let i = 0; i < bytes.length; i += chunk) {
      binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
    }
    return btoa(binary);
  }
  function registerMemory(memory, label) {
    if (!memory || !memory.buffer) return "";
    let id = state.memoryIds.get(memory);
    if (id) return id;
    id = "memory-" + state.nextMemoryId++;
    state.memoryIds.set(memory, id);
    state.memoryObjects.push({ id, object: memory });
    let bytes = 0;
    try { bytes = memory.buffer.byteLength || 0; } catch (e) {}
    state.memories[id] = { id, label: String(label || ""), byteLength: bytes, pages: Math.floor(bytes / 65536), createdAt: Date.now() };
    return id;
  }
  function registerModule(module, source, label) {
    if (!module) return "";
    let id = state.moduleIds.get(module);
    if (id) return id;
    id = "module-" + state.nextModuleId++;
    state.moduleIds.set(module, id);
    const bytes = bytesFrom(source);
    let imports = [];
    let exports = [];
    try { imports = Native.Module.imports(module).map(item => ({ module: item.module || "", name: item.name || "", kind: item.kind || "" })); } catch (e) {}
    try { exports = Native.Module.exports(module).map(item => ({ name: item.name || "", kind: item.kind || "" })); } catch (e) {}
    state.modules[id] = {
      id,
      name: String(label || id),
      imports,
      exports,
      import_count: imports.length,
      export_count: exports.length,
      byteLength: bytes ? bytes.byteLength : 0,
      bytesCaptured: !!(bytes && bytes.byteLength <= state.maxBytes),
      bytesBase64: bytes ? b64(bytes) : "",
      truncated: !!(bytes && bytes.byteLength > state.maxBytes),
      createdAt: Date.now()
    };
    return id;
  }
  function registerInstance(instance, moduleId, label) {
    if (!instance) return "";
    let id = state.instanceIds.get(instance);
    if (id) return id;
    id = "instance-" + state.nextInstanceId++;
    state.instanceIds.set(instance, id);
    state.instanceObjects.push({ id, object: instance });
    const exports = [];
    const memoryIds = [];
    try {
      for (const [name, value] of Object.entries(instance.exports || {})) {
        let kind = typeof value;
        let detail = {};
        if (typeof Native.Memory !== "undefined" && value instanceof Native.Memory) {
          kind = "memory";
          const memoryId = registerMemory(value, id + ":" + name);
          memoryIds.push(memoryId);
          detail.memory_id = memoryId;
          detail.byteLength = value.buffer.byteLength || 0;
          detail.pages = Math.floor((value.buffer.byteLength || 0) / 65536);
        } else if (typeof Native.Table !== "undefined" && value instanceof Native.Table) {
          kind = "table";
          try { detail.length = value.length; } catch (e) {}
        }
        exports.push({ name, kind, detail });
      }
    } catch (e) {}
    state.instances[id] = {
      id,
      module_id: moduleId || "",
      name: String(label || id),
      exports,
      export_count: exports.length,
      memory_ids: memoryIds,
      createdAt: Date.now()
    };
    return id;
  }
  function wrapImportObject(importObject, label) {
    if (!importObject || typeof importObject !== "object") return importObject;
    for (const [moduleName, members] of Object.entries(importObject)) {
      if (!members || typeof members !== "object") continue;
      for (const [name, value] of Object.entries(members)) {
        if (typeof value === "function" && !value.__aida_wasm_import_wrapper) {
          const original = value;
          const traceKey = "import:" + moduleName + "." + name;
          const wrapped = function(...args) {
            const rows = state.traces[traceKey] || (state.traces[traceKey] = []);
            if (rows.length < 500) rows.push({ kind: "import", name: moduleName + "." + name, args: args.map(v => String(v).slice(0, 500)), timestamp: Date.now() });
            return original.apply(this, args);
          };
          try { Object.defineProperty(wrapped, "__aida_wasm_import_wrapper", { value: true }); } catch (e) {}
          members[name] = wrapped;
        } else if (typeof Native.Memory !== "undefined" && value instanceof Native.Memory) {
          registerMemory(value, "import:" + moduleName + "." + name);
        }
      }
    }
    return importObject;
  }
  function recordResult(result, source, label) {
    if (!result) return result;
    if (result.module && result.instance) {
      const moduleId = registerModule(result.module, source, label);
      const instanceId = registerInstance(result.instance, moduleId, label);
      state.last = { module_id: moduleId, instance_id: instanceId, at: Date.now() };
      return result;
    }
    if (result instanceof Native.Instance) {
      const instanceId = registerInstance(result, "", label);
      state.last = { module_id: "", instance_id: instanceId, at: Date.now() };
      return result;
    }
    return result;
  }
  function wrapExport(instanceId, functionName, maxCaptures) {
    let obj = null;
    try {
      for (const ref of state.instanceObjects || []) {
        if (ref && ref.id === instanceId) {
          obj = ref.object;
          break;
        }
      }
    } catch (e) {}
    if (!obj) {
      for (const [id, info] of Object.entries(state.instances)) {
        if ((instanceId && id !== instanceId) || !info) continue;
      }
      return { ok: false, error: "instance_object_not_available" };
    }
    const exports = obj.exports || {};
    const original = exports[functionName];
    if (typeof original !== "function") return { ok: false, error: "export_function_not_found" };
    if (original.__aida_wasm_export_wrapper) return { ok: true, already_wrapped: true, instance_id: instanceId, function_name: functionName };
    const key = "export:" + instanceId + ":" + functionName;
    const wrapped = function(...args) {
      const rows = state.traces[key] || (state.traces[key] = []);
      const started = performance.now();
      try {
        const result = original.apply(this, args);
        if (rows.length < Math.max(1, Math.min(Number(maxCaptures || 100), 1000))) {
          rows.push({ kind: "export", instance_id: instanceId, name: functionName, args: args.map(v => String(v).slice(0, 500)), result: String(result).slice(0, 500), duration_ms: performance.now() - started, timestamp: Date.now() });
        }
        return result;
      } catch (e) {
        if (rows.length < Math.max(1, Math.min(Number(maxCaptures || 100), 1000))) {
          rows.push({ kind: "export", instance_id: instanceId, name: functionName, args: args.map(v => String(v).slice(0, 500)), error: String(e && (e.message || e)).slice(0, 500), duration_ms: performance.now() - started, timestamp: Date.now() });
        }
        throw e;
      }
    };
    try { Object.defineProperty(wrapped, "__aida_wasm_export_wrapper", { value: true }); } catch (e) {}
    try {
      exports[functionName] = wrapped;
      return { ok: true, instance_id: instanceId, function_name: functionName, method: "assignment" };
    } catch (assignError) {
      try {
        Object.defineProperty(exports, functionName, { value: wrapped, configurable: true });
        return { ok: true, instance_id: instanceId, function_name: functionName, method: "defineProperty" };
      } catch (defineError) {
        return { ok: false, error: "export_property_not_writable", assign_error: String(assignError && (assignError.message || assignError)).slice(0, 300), define_error: String(defineError && (defineError.message || defineError)).slice(0, 300) };
      }
    }
  }
  try {
    const NativeModule = Native.Module;
    const NativeInstance = Native.Instance;
    const nativeInstantiate = Native.instantiate.bind(Native);
    const nativeInstantiateStreaming = Native.instantiateStreaming ? Native.instantiateStreaming.bind(Native) : null;
    const nativeCompile = Native.compile ? Native.compile.bind(Native) : null;
    const nativeCompileStreaming = Native.compileStreaming ? Native.compileStreaming.bind(Native) : null;
    function WrappedModule(source) {
      const module = new NativeModule(source);
      registerModule(module, source, "new WebAssembly.Module");
      return module;
    }
    Object.setPrototypeOf(WrappedModule, NativeModule);
    WrappedModule.prototype = NativeModule.prototype;
    function WrappedInstance(module, importObject) {
      const moduleId = registerModule(module, null, "new WebAssembly.Instance");
      const instance = new NativeInstance(module, wrapImportObject(importObject, moduleId));
      registerInstance(instance, moduleId, "new WebAssembly.Instance");
      return instance;
    }
    Object.setPrototypeOf(WrappedInstance, NativeInstance);
    WrappedInstance.prototype = NativeInstance.prototype;
    Native.Module = WrappedModule;
    Native.Instance = WrappedInstance;
    Native.instantiate = async function(source, importObject) {
      const isModule = source instanceof NativeModule || source instanceof WrappedModule;
      const moduleId = isModule ? registerModule(source, null, "instantiate(Module)") : "";
      const result = await nativeInstantiate(source, wrapImportObject(importObject, moduleId || "instantiate"));
      if (result && result.instance && result.module) return recordResult(result, source, "instantiate");
      if (result instanceof NativeInstance || result instanceof WrappedInstance) {
        const instanceId = registerInstance(result, moduleId, "instantiate(Module)");
        state.last = { module_id: moduleId, instance_id: instanceId, at: Date.now() };
      }
      return result;
    };
    if (nativeInstantiateStreaming) {
      Native.instantiateStreaming = async function(source, importObject) {
        const result = await nativeInstantiateStreaming(source, wrapImportObject(importObject, "instantiateStreaming"));
        return recordResult(result, null, "instantiateStreaming");
      };
    }
    if (nativeCompile) {
      Native.compile = async function(source) {
        const module = await nativeCompile(source);
        registerModule(module, source, "compile");
        return module;
      };
    }
    if (nativeCompileStreaming) {
      Native.compileStreaming = async function(source) {
        const module = await nativeCompileStreaming(source);
        registerModule(module, null, "compileStreaming");
        return module;
      };
    }
    state.installed = true;
  } catch (e) {
    state.install_errors.push(String(e && (e.message || e)).slice(0, 500));
  }
  state.snapshot = function() {
    for (const [id, memory] of Object.entries(state.memories)) {
      try {
        const obj = Array.from(state.memoryIds.entries ? [] : []);
        memory.updatedAt = Date.now();
      } catch (e) {}
    }
    return {
      installed: state.installed,
      supported: !!Native,
      installedAt: state.installedAt,
      modules: Object.values(state.modules).map(item => ({
        id: item.id,
        name: item.name,
        imports: item.imports,
        exports: item.exports,
        import_count: item.import_count,
        export_count: item.export_count,
        byteLength: item.byteLength,
        bytesCaptured: item.bytesCaptured,
        truncated: item.truncated,
        createdAt: item.createdAt
      })),
      instances: Object.values(state.instances),
      memories: Object.values(state.memories),
      module_count: Object.keys(state.modules).length,
      instance_count: Object.keys(state.instances).length,
      memory_count: Object.keys(state.memories).length,
      last: state.last || null,
      install_errors: state.install_errors.slice(-5)
    };
  };
  state.dumpMemory = function(target, offset, length) {
    let memory = null;
    if (target && state.memories[target]) {
      try {
        for (const ref of state.memoryObjects || []) {
          if (ref && ref.id === target) {
            memory = ref.object;
            break;
          }
        }
      } catch (e) {}
    }
    if (!memory && target && state.instances[target]) {
      try {
        for (const ref of state.instanceObjects || []) {
          if (ref && ref.id === target) {
            for (const value of Object.values(ref.object.exports || {})) {
              if (typeof Native.Memory !== "undefined" && value instanceof Native.Memory) {
                memory = value;
                break;
              }
            }
          }
        }
      } catch (e) {}
    }
    if (!memory) return { ok: false, error: "memory_not_found" };
    const start = Math.max(0, Number(offset || 0));
    const count = Math.max(0, Math.min(Number(length || 256), 65536));
    const bytes = new Uint8Array(memory.buffer).slice(start, start + count);
    return { ok: true, offset: start, length: bytes.length, bytes: Array.from(bytes), totalLength: memory.buffer.byteLength || 0 };
  };
  state.traceExport = wrapExport;
  window.__aida_wasm_debug = state;
  return state.snapshot();
})()
"""


_SECTION_NAMES = {
    0: "custom",
    1: "type",
    2: "import",
    3: "function",
    4: "table",
    5: "memory",
    6: "global",
    7: "export",
    8: "start",
    9: "element",
    10: "code",
    11: "data",
    12: "data_count",
}


def _read_u32(data: bytes, offset: int) -> tuple[int, int]:
    result = 0
    shift = 0
    pos = offset
    while pos < len(data):
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if (byte & 0x80) == 0:
            return result, pos
        shift += 7
        if shift > 35:
            break
    raise ValueError("invalid unsigned LEB128")


def _read_name(data: bytes, offset: int) -> tuple[str, int]:
    length, pos = _read_u32(data, offset)
    end = min(len(data), pos + length)
    return data[pos:end].decode("utf-8", "replace"), end


def _skip_limits(data: bytes, offset: int) -> int:
    flags, pos = _read_u32(data, offset)
    _, pos = _read_u32(data, pos)
    if flags & 1:
        _, pos = _read_u32(data, pos)
    return pos


def _parse_import_section(payload: bytes) -> list[dict]:
    imports: list[dict] = []
    pos = 0
    count, pos = _read_u32(payload, pos)
    for _ in range(count):
        module, pos = _read_name(payload, pos)
        name, pos = _read_name(payload, pos)
        kind = payload[pos] if pos < len(payload) else 255
        pos += 1
        kind_name = {0: "function", 1: "table", 2: "memory", 3: "global"}.get(kind, f"kind_{kind}")
        if kind == 0:
            type_index, pos = _read_u32(payload, pos)
            detail = {"type_index": type_index}
        elif kind in (1, 2):
            if kind == 1 and pos < len(payload):
                pos += 1
            next_pos = _skip_limits(payload, pos)
            detail = {"raw_length": next_pos - pos}
            pos = next_pos
        elif kind == 3:
            value_type = payload[pos] if pos < len(payload) else None
            mutable = payload[pos + 1] if pos + 1 < len(payload) else None
            pos += 2
            detail = {"value_type": value_type, "mutable": mutable}
        else:
            detail = {}
        imports.append({"module": module, "name": name, "kind": kind_name, "detail": detail})
    return imports


def _parse_export_section(payload: bytes) -> list[dict]:
    exports: list[dict] = []
    pos = 0
    count, pos = _read_u32(payload, pos)
    for _ in range(count):
        name, pos = _read_name(payload, pos)
        kind = payload[pos] if pos < len(payload) else 255
        pos += 1
        index, pos = _read_u32(payload, pos)
        exports.append({"name": name, "kind": {0: "function", 1: "table", 2: "memory", 3: "global"}.get(kind, f"kind_{kind}"), "index": index})
    return exports


def _parse_code_section(payload: bytes) -> list[dict]:
    functions: list[dict] = []
    pos = 0
    count, pos = _read_u32(payload, pos)
    for index in range(count):
        body_size, pos = _read_u32(payload, pos)
        body_start = pos
        body_end = min(len(payload), body_start + body_size)
        local_count = 0
        code_start = body_start
        try:
            local_count, code_start = _read_u32(payload, body_start)
            for _ in range(local_count):
                _, code_start = _read_u32(payload, code_start)
                code_start += 1
        except Exception:
            code_start = body_start
        code = payload[code_start:body_end]
        functions.append({
            "index": index,
            "body_size": body_size,
            "local_decl_count": local_count,
            "opcode_preview_hex": binascii.hexlify(code[:64]).decode("ascii"),
            "ends_with_end_opcode": bool(code[-1:] == b"\x0b"),
        })
        pos = body_end
    return functions


def _section_count(payload: bytes) -> int | None:
    try:
        count, _ = _read_u32(payload, 0)
        return count
    except Exception:
        return None


def _parse_wasm(data: bytes) -> dict:
    if len(data) < 8 or data[:4] != b"\x00asm":
        return {"valid": False, "error": "invalid wasm magic", "byte_length": len(data)}
    version = int.from_bytes(data[4:8], "little")
    sections: list[dict] = []
    imports: list[dict] = []
    exports: list[dict] = []
    functions: list[dict] = []
    pos = 8
    while pos < len(data):
        section_start = pos
        section_id = data[pos]
        pos += 1
        size, pos = _read_u32(data, pos)
        payload_start = pos
        payload_end = min(len(data), payload_start + size)
        payload = data[payload_start:payload_end]
        entry = {
            "id": section_id,
            "name": _SECTION_NAMES.get(section_id, f"section_{section_id}"),
            "offset": section_start,
            "payload_offset": payload_start,
            "size": size,
        }
        if section_id == 0:
            try:
                custom_name, _ = _read_name(payload, 0)
                entry["custom_name"] = custom_name
            except Exception as exc:
                entry["custom_name_error"] = _safe_text(exc, 120)
        elif section_id in {1, 3, 4, 5, 6, 9, 11, 12}:
            count = _section_count(payload)
            if count is not None:
                entry["count"] = count
        elif section_id == 2:
            try:
                imports = _parse_import_section(payload)
                entry["count"] = len(imports)
            except Exception as exc:
                entry["parse_error"] = _safe_text(exc, 300)
        elif section_id == 7:
            try:
                exports = _parse_export_section(payload)
                entry["count"] = len(exports)
            except Exception as exc:
                entry["parse_error"] = _safe_text(exc, 300)
        elif section_id == 10:
            try:
                functions = _parse_code_section(payload)
                entry["count"] = len(functions)
            except Exception as exc:
                entry["parse_error"] = _safe_text(exc, 300)
        sections.append(entry)
        pos = payload_end
    return {
        "valid": True,
        "version": version,
        "byte_length": len(data),
        "sections": sections,
        "imports": imports,
        "exports": exports,
        "functions": functions,
        "section_count": len(sections),
        "function_count": len(functions),
    }


def _hex_ascii(values: list[int]) -> dict:
    bytes_value = bytes(max(0, min(int(v), 255)) for v in values)
    return {
        "hexDump": binascii.hexlify(bytes_value).decode("ascii"),
        "asciiDump": "".join(chr(b) if 32 <= b < 127 else "." for b in bytes_value),
    }


async def _page_context(page: Any, page_id: str | None) -> dict:
    try:
        return await browser_manager.page_envelope(page, page_id)
    except Exception:
        return {"page_id": browser_manager.page_id_for(page) or page_id or "", "active_page_id": browser_manager.active_page_id, "session_id": browser_manager.session_id}


async def _install(page: Any, page_id: str | None) -> dict:
    await browser_manager.add_persistent_script(_WASM_HOOK_NAME, _WASM_HOOK)
    result = await _await_no_cancel_wait(page.evaluate(_WASM_HOOK), timeout=5.0)
    out = result if isinstance(result, dict) else {"installed": False, "result_type": type(result).__name__}
    out.update(await _page_context(page, page_id))
    return out


@mcp.tool()
async def browser_wasm(
    action: str = "list",
    module_id: str | None = None,
    instance_id: str | None = None,
    memory_id: str | None = None,
    function_name: str | None = None,
    offset: int = 0,
    length: int = 256,
    max_captures: int = 100,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    action_name = str(action or "list").strip().lower()
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, f"browser_wasm:{action_name}", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
        snapshot = await _install(page, resolved_page_id)
        if action_name == "list":
            snapshot.update({"success": True, "status": "ok", "action": action_name, "elapsed_ms": int((time.perf_counter() - started) * 1000)})
            return snapshot
        if action_name == "disassemble":
            target = module_id or (snapshot.get("last") or {}).get("module_id") if isinstance(snapshot.get("last"), dict) else module_id
            if not target:
                return {"success": False, "status": "failed", "action": action_name, "error": "module_id is required and no last module is available", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
            module = await _await_no_cancel_wait(page.evaluate("""moduleId => {
              const state = window.__aida_wasm_debug;
              if (!state || !state.modules || !state.modules[moduleId]) return null;
              const item = state.modules[moduleId];
              return { id: item.id, name: item.name, imports: item.imports, exports: item.exports, byteLength: item.byteLength, bytesCaptured: item.bytesCaptured, bytesBase64: item.bytesBase64 || "", truncated: item.truncated };
            }""", target), timeout=3.0)
            if not isinstance(module, dict):
                return {"success": False, "status": "failed", "action": action_name, "error": "module_not_found", "module_id": target, "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
            if not module.get("bytesBase64"):
                return {
                    "success": True,
                    "status": "metadata_only",
                    "action": action_name,
                    "module_id": target,
                    "module": {k: v for k, v in module.items() if k != "bytesBase64"},
                    "imports": module.get("imports") or [],
                    "exports": module.get("exports") or [],
                    "sections": [],
                    "functions": [],
                    "limitation": "Raw module bytes were not available; returning WebAssembly.Module imports/exports metadata captured by the browser API.",
                    "page_id": resolved_page_id,
                    "active_page_id": browser_manager.active_page_id,
                    "elapsed_ms": int((time.perf_counter() - started) * 1000),
                }
            parsed = _parse_wasm(base64.b64decode(str(module.get("bytesBase64") or "")))
            return {
                "success": bool(parsed.get("valid")),
                "status": "ok" if parsed.get("valid") else "failed",
                "action": action_name,
                "module_id": target,
                "module": {k: v for k, v in module.items() if k != "bytesBase64"},
                "imports": parsed.get("imports") or module.get("imports") or [],
                "exports": parsed.get("exports") or module.get("exports") or [],
                "sections": parsed.get("sections") or [],
                "functions": parsed.get("functions") or [],
                "parse": {k: v for k, v in parsed.items() if k not in {"sections", "imports", "exports", "functions"}},
                "page_id": resolved_page_id,
                "active_page_id": browser_manager.active_page_id,
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
        if action_name == "dump_memory":
            target = memory_id or instance_id or ""
            if not target:
                return {"success": False, "status": "failed", "action": action_name, "error": "instance_id or memory_id is required", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
            result = await _await_no_cancel_wait(page.evaluate("""arg => {
              const state = window.__aida_wasm_debug;
              if (!state || !state.dumpMemory) return { ok: false, error: "wasm_hook_not_installed" };
              return state.dumpMemory(arg.target, arg.offset, arg.length);
            }""", {"target": target, "offset": max(0, int(offset or 0)), "length": max(1, min(int(length or 256), 65536))}), timeout=4.0)
            payload = result if isinstance(result, dict) else {"ok": False, "error": "unexpected_dump_result"}
            dump = _hex_ascii(payload.get("bytes") if isinstance(payload.get("bytes"), list) else []) if payload.get("ok") else {}
            return {
                "success": bool(payload.get("ok")),
                "status": "ok" if payload.get("ok") else "failed",
                "action": action_name,
                "target": target,
                "offset": payload.get("offset", offset),
                "length": payload.get("length", 0),
                "totalLength": payload.get("totalLength", 0),
                **dump,
                "error": payload.get("error"),
                "page_id": resolved_page_id,
                "active_page_id": browser_manager.active_page_id,
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
        if action_name == "trace_calls":
            if not instance_id:
                return {"success": False, "status": "failed", "action": action_name, "error": "instance_id is required", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
            if function_name:
                installed = await _await_no_cancel_wait(page.evaluate("""arg => {
                  const state = window.__aida_wasm_debug;
                  if (!state || !state.traceExport) return { ok: false, error: "wasm_hook_not_installed" };
                  return state.traceExport(arg.instanceId, arg.functionName, arg.maxCaptures);
                }""", {"instanceId": instance_id, "functionName": function_name, "maxCaptures": max(1, min(int(max_captures or 100), 1000))}), timeout=4.0)
            else:
                installed = {"ok": True, "mode": "read_existing"}
            traces = await _await_no_cancel_wait(page.evaluate("""arg => {
              const state = window.__aida_wasm_debug;
              if (!state || !state.traces) return [];
              const prefix = "export:" + arg.instanceId + ":";
              const rows = [];
              for (const [key, values] of Object.entries(state.traces)) {
                if (key.startsWith(prefix) && (!arg.functionName || key.endsWith(":" + arg.functionName))) rows.push(...values);
              }
              return rows.slice(Math.max(0, rows.length - arg.limit));
            }""", {"instanceId": instance_id, "functionName": function_name or "", "limit": max(1, min(int(max_captures or 100), 1000))}), timeout=3.0)
            install_payload = installed if isinstance(installed, dict) else {"ok": False, "error": "unexpected_trace_result"}
            out = {
                "success": bool(install_payload.get("ok")),
                "status": "ok" if install_payload.get("ok") else "failed",
                "action": action_name,
                "trace": install_payload,
                "traces": traces if isinstance(traces, list) else [],
                "instance_id": instance_id,
                "function_name": function_name or "",
                "page_id": resolved_page_id,
                "active_page_id": browser_manager.active_page_id,
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
            if not install_payload.get("ok"):
                out["limitation"] = "Some WebAssembly export objects are not writable/configurable in the browser and cannot be wrapped after instantiation."
            return out
        return {"success": False, "status": "failed", "action": action_name, "error": "unknown action. Use list, disassemble, dump_memory, trace_calls", "page_id": resolved_page_id, "active_page_id": browser_manager.active_page_id}
    except Exception as exc:
        _camoufox_debug("browser_wasm_exception", action=action_name, error_type=type(exc).__name__, error_summary=_safe_text(exc, 500))
        return {"success": False, "status": "failed", "action": action_name, "error": str(exc), "error_type": type(exc).__name__, "active_page_id": browser_manager.active_page_id}
