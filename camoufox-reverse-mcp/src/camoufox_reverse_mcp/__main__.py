import argparse
import ast
import importlib
import importlib.util
import inspect
import json
import marshal
import os
import sys
import time
import types


AIDA_INITIATOR_CONTRACT_V2 = "aida_initiator_contract_v2_page_marker"
AIDA_DEFAULT_ADDON_POLICY_V1 = "aida_default_addon_policy_v1"
AIDA_FAST_VISIBLE_POLICY_V1 = "aida_fast_visible_policy_v1"
AIDA_CONTEXT_VIEWPORT_SANITIZER_V1 = "aida_context_viewport_sanitizer_v1"
AIDA_LAUNCH_BUDGET_POLICY_V1 = "aida_launch_budget_policy_v1"


def _aida_apply_playwright_pageerror_patch():
    try:
        from ._playwright_patch import patch_playwright_pageerror

        return patch_playwright_pageerror()
    except Exception as exc:
        payload = {
            "event": "playwright_pageerror_patch_import_failed",
            "ok": False,
            "status": "import_failed",
            "error_type": type(exc).__name__,
            "error": str(exc)[:500],
            "pid": os.getpid(),
            "ts_ms": int(time.time() * 1000),
        }
        try:
            line = "AIDA_CAMOUFOX " + json.dumps(payload, sort_keys=True, separators=(",", ":"))
            print(line, file=sys.stderr, flush=True)
            log_path = os.environ.get("AIDA_CAMOUFOX_DEBUG_LOG", "")
            if log_path:
                with open(log_path, "a", encoding="utf-8") as handle:
                    handle.write(line + "\n")
        except Exception:
            pass
        return payload


def _aida_network_source_path():
    try:
        spec = importlib.util.find_spec("camoufox_reverse_mcp.tools.network")
        if spec is not None and spec.origin and os.path.isfile(spec.origin):
            return spec.origin
    except Exception:
        pass
    return os.path.join(os.path.dirname(__file__), "tools", "network.py")


def _aida_contract_probe_from_source():
    path = _aida_network_source_path()
    if path.endswith((".pyc", ".pyo")):
        return _aida_contract_probe_from_pyc(path)
    with open(path, "r", encoding="utf-8") as handle:
        tree = ast.parse(handle.read(), filename=path)
    target = None
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == "get_request_initiator":
            target = node
            break
    if target is None:
        return {"source": "ast", "ok": False, "params": [], "has_marker_constant": False, "error": "get_request_initiator_missing"}
    params = [arg.arg for arg in target.args.args]
    strings = {node.value for node in ast.walk(target) if isinstance(node, ast.Constant) and isinstance(node.value, str)}
    has_marker_constant = AIDA_INITIATOR_CONTRACT_V2 in strings
    ok = all(name in params for name in ("request_id", "page_id", "marker")) and has_marker_constant
    return {
        "source": "ast",
        "ok": ok,
        "params": params,
        "has_marker_constant": has_marker_constant,
    }


def _aida_contract_probe_from_code(code, source):
    for value in code.co_consts:
        if isinstance(value, types.CodeType) and value.co_name == "get_request_initiator":
            arg_count = value.co_argcount + value.co_kwonlyargcount
            params = list(value.co_varnames[:arg_count])
            consts = repr(value.co_consts)
            has_marker_constant = AIDA_INITIATOR_CONTRACT_V2 in consts
            ok = all(name in params for name in ("request_id", "page_id", "marker")) and has_marker_constant
            return {
                "source": source,
                "ok": ok,
                "params": params,
                "has_marker_constant": has_marker_constant,
            }
    return {"source": source, "ok": False, "params": [], "has_marker_constant": False, "error": "get_request_initiator_missing"}


def _aida_contract_probe_from_pyc(path):
    with open(path, "rb") as handle:
        handle.read(16)
        code = marshal.load(handle)
    return _aida_contract_probe_from_code(code, "pyc")


def _aida_contract_probe_from_import():
    from .tools import network as aida_contract_network
    fn = aida_contract_network.get_request_initiator
    params = list(inspect.signature(fn).parameters)
    consts = repr(getattr(getattr(fn, "__code__", None), "co_consts", ()))
    has_marker_constant = AIDA_INITIATOR_CONTRACT_V2 in consts
    ok = all(name in params for name in ("request_id", "page_id", "marker")) and has_marker_constant
    return {
        "source": "import",
        "ok": ok,
        "params": params,
        "has_marker_constant": has_marker_constant,
    }


def _aida_walk_code_objects(code):
    yield code
    for value in getattr(code, "co_consts", ()) or ():
        if isinstance(value, types.CodeType):
            yield from _aida_walk_code_objects(value)


def _aida_iter_module_code_objects(module):
    seen = set()
    for value in vars(module).values():
        candidates = [value]
        if isinstance(value, type):
            candidates.extend(vars(value).values())
        for candidate in candidates:
            if isinstance(candidate, (staticmethod, classmethod)):
                candidate = candidate.__func__
            code = getattr(candidate, "__code__", None)
            if not isinstance(code, types.CodeType):
                continue
            for nested in _aida_walk_code_objects(code):
                marker = id(nested)
                if marker in seen:
                    continue
                seen.add(marker)
                yield nested


def _aida_module_consts_repr(module_name, function_names):
    out = {
        "available": False,
        "consts_repr": "",
        "per_function": {},
        "missing_functions": [],
        "errors": [],
    }
    try:
        module = importlib.import_module(module_name)
        out["available"] = True
        snippets = []
        for name in function_names:
            obj = module
            for piece in name.split("."):
                obj = getattr(obj, piece, None)
                if obj is None:
                    break
            if obj is None:
                out["missing_functions"].append(name)
                continue
            code = getattr(obj, "__code__", None)
            if code is None:
                out["missing_functions"].append(name)
                continue
            per_fn_snippets = []
            for sub_code in _aida_walk_code_objects(code):
                per_fn_snippets.append(repr(getattr(sub_code, "co_consts", ())))
                per_fn_snippets.append(repr(getattr(sub_code, "co_names", ())))
                per_fn_snippets.append(repr(getattr(sub_code, "co_varnames", ())))
                per_fn_snippets.append(repr(getattr(sub_code, "co_cellvars", ())))
                per_fn_snippets.append(repr(getattr(sub_code, "co_freevars", ())))
            per_fn_repr = "".join(per_fn_snippets)
            out["per_function"][name] = per_fn_repr
            snippets.append(per_fn_repr)
        out["consts_repr"] = "".join(snippets)
    except Exception as exc:
        out["errors"].append({"module": module_name, "error_type": type(exc).__name__, "error": str(exc)[:300]})
    return out


def _aida_contract_probe_diag(message, **fields):
    payload = {
        "event": "launch_budget_contract_probe",
        "stage": message,
        "pid": os.getpid(),
        "ts_ms": int(time.time() * 1000),
    }
    payload.update(fields)
    try:
        line = "AIDA_CAMOUFOX " + json.dumps(payload, sort_keys=True, separators=(",", ":"), default=str)
        print(line, file=sys.stderr, flush=True)
        log_path = os.environ.get("AIDA_CAMOUFOX_DEBUG_LOG", "")
        if log_path:
            with open(log_path, "a", encoding="utf-8") as handle:
                handle.write(line + "\n")
    except Exception:
        pass


def _aida_launch_budget_policy_namespace_from_source(source_text, filename):
    tree = ast.parse(source_text, filename=filename)
    selected = []
    needed_names = {
        "AIDA_VISIBLE_READINESS_MAX_MS",
        "AIDA_LAUNCH_BUDGET_POLICY_MARKER",
        "AIDA_LAUNCH_MAX_TIMEOUT_MS",
        "AIDA_LAUNCH_FLOOR_MS",
        "AIDA_LAUNCH_DEFAULT_BUNDLED_VISIBLE_MS",
        "AIDA_LAUNCH_DEFAULT_FAST_PROBE_MS",
        "AIDA_LAUNCH_DEFAULT_NORMAL_MS",
        "AIDA_NAVIGATION_MIN_TIMEOUT_MS",
        "AIDA_NAVIGATION_DEFAULT_TIMEOUT_MS",
        "AIDA_NAVIGATION_MAX_TIMEOUT_MS",
        "AIDA_LAUNCH_PHASE_POLICY",
        "_int_config",
        "_aida_launch_mode",
        "_aida_default_launch_timeout_ms",
        "_aida_clamp_int",
        "_aida_phase_budget_seconds",
        "aida_launch_budget_policy_snapshot",
        "aida_resolve_launch_budget_policy",
        "aida_retry_launch_timeout_ms",
        "aida_clamp_navigation_timeout_ms",
        "aida_validate_launch_budget_policy",
    }
    for node in tree.body:
        if isinstance(node, ast.ImportFrom) and node.module == "__future__":
            selected.append(node)
            break
    for node in tree.body:
        name = None
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            name = node.targets[0].id
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            name = node.target.id
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            name = node.name
        if name in needed_names:
            selected.append(node)
    module = ast.Module(body=selected, type_ignores=[])
    ast.fix_missing_locations(module)
    namespace = {"Any": object}
    exec(compile(module, filename, "exec"), namespace)
    return namespace, tree


def _aida_addon_policy_contract_probe():
    diagnostics = {
        "browser_module": False,
        "browser_origin": "",
        "browser_probe_mode": "",
        "addon_policy_marker_present": False,
        "addon_policy_log_present": False,
        "addon_invalid_diagnostic_present": False,
        "exclude_addons_marker_present": False,
        "default_ubo_exclusion_present": False,
        "addon_all_launch_scope_present": False,
        "explicit_addons_validated_present": False,
        "addon_policy_contract_ok": False,
        "errors": [],
    }
    try:
        spec = importlib.util.find_spec("camoufox_reverse_mcp.browser")
        if spec and spec.origin and os.path.isfile(spec.origin) and spec.origin.endswith(".py"):
            diagnostics["browser_module"] = True
            diagnostics["browser_origin"] = spec.origin
            diagnostics["browser_probe_mode"] = "source"
            with open(spec.origin, "r", encoding="utf-8") as handle:
                browser_repr = handle.read()
        else:
            diagnostics["browser_probe_mode"] = "code_consts"
            diagnostics["browser_origin"] = spec.origin if spec else ""
            browser_module = importlib.import_module("camoufox_reverse_mcp.browser")
            diagnostics["browser_module"] = True
            snippets = [repr(getattr(browser_module, "AIDA_DEFAULT_ADDON_POLICY_MARKER", ""))]
            for obj in (
                getattr(browser_module, "_apply_default_addon_policy", None),
                getattr(getattr(browser_module, "BrowserManager", None), "launch", None),
            ):
                code = getattr(obj, "__code__", None)
                if code is None:
                    continue
                for sub_code in _aida_walk_code_objects(code):
                    snippets.append(repr(getattr(sub_code, "co_consts", ())))
                    snippets.append(repr(getattr(sub_code, "co_names", ())))
                    snippets.append(repr(getattr(sub_code, "co_varnames", ())))
            browser_repr = "".join(snippets)
        diagnostics["addon_policy_marker_present"] = AIDA_DEFAULT_ADDON_POLICY_V1 in browser_repr
        diagnostics["addon_policy_log_present"] = "launch_options_addon_policy" in browser_repr
        diagnostics["addon_invalid_diagnostic_present"] = "launch_options_addon_invalid" in browser_repr
        diagnostics["exclude_addons_marker_present"] = "exclude_addons" in browser_repr
        diagnostics["default_ubo_exclusion_present"] = (
            "DefaultAddons.UBO" in browser_repr or
            ("DefaultAddons" in browser_repr and "UBO" in browser_repr)
        )
        diagnostics["addon_all_launch_scope_present"] = "all_launches" in browser_repr
        diagnostics["explicit_addons_validated_present"] = "explicit_addons_validated" in browser_repr
    except Exception as exc:
        diagnostics["errors"].append({"module": "browser", "error_type": type(exc).__name__, "error": str(exc)[:300]})
        _aida_contract_probe_diag(
            "addon_policy_contract_probe_exception",
            error_type=type(exc).__name__,
            error=str(exc)[:300],
        )
    diagnostics["addon_policy_contract_ok"] = (
        diagnostics["addon_policy_marker_present"]
        and diagnostics["addon_policy_log_present"]
        and diagnostics["addon_invalid_diagnostic_present"]
        and diagnostics["exclude_addons_marker_present"]
        and diagnostics["default_ubo_exclusion_present"]
        and diagnostics["addon_all_launch_scope_present"]
        and diagnostics["explicit_addons_validated_present"]
        and not diagnostics["errors"]
    )
    if not diagnostics["addon_policy_contract_ok"]:
        _aida_contract_probe_diag(
            "addon_policy_contract_probe_failed",
            browser_probe_mode=diagnostics["browser_probe_mode"],
            browser_origin=diagnostics["browser_origin"],
            addon_policy_marker_present=diagnostics["addon_policy_marker_present"],
            addon_policy_log_present=diagnostics["addon_policy_log_present"],
            addon_invalid_diagnostic_present=diagnostics["addon_invalid_diagnostic_present"],
            exclude_addons_marker_present=diagnostics["exclude_addons_marker_present"],
            default_ubo_exclusion_present=diagnostics["default_ubo_exclusion_present"],
            addon_all_launch_scope_present=diagnostics["addon_all_launch_scope_present"],
            explicit_addons_validated_present=diagnostics["explicit_addons_validated_present"],
            errors=list(diagnostics["errors"]),
        )
    return diagnostics


def _aida_launch_policy_contract_probe():
    diagnostics = {
        "browser_module": False,
        "browser_origin": "",
        "browser_probe_mode": "",
        "fast_visible_policy_marker_present": False,
        "fast_visible_disabled_return_present": False,
        "fast_visible_fallback_ignored_present": False,
        "fast_visible_forbidden_return_absent": False,
        "fast_visible_compat_path_absent": False,
        "fast_visible_selected_async_present": False,
        "context_viewport_sanitizer_marker_present": False,
        "context_options_sanitizer_present": False,
        "context_no_viewport_forced_present": False,
        "context_viewport_strip_keys_present": False,
        "page_viewport_set_present": False,
        "protocol_schema_viewport_classification_present": False,
        "direct_context_viewport_emulation_absent": False,
        "direct_context_viewport_emulation_violations": [],
        "safe_expansion_context_creation_present": False,
        "safe_expansion_context_creation_seen": [],
        "fast_visible_policy_contract_ok": False,
        "errors": [],
    }
    try:
        spec = importlib.util.find_spec("camoufox_reverse_mcp.browser")
        if spec and spec.origin and os.path.isfile(spec.origin) and spec.origin.endswith(".py"):
            diagnostics["browser_module"] = True
            diagnostics["browser_origin"] = spec.origin
            diagnostics["browser_probe_mode"] = "source"
            with open(spec.origin, "r", encoding="utf-8") as handle:
                browser_repr = handle.read()
            diagnostics["fast_visible_disabled_return_present"] = (
                "def _use_fast_visible_launch" in browser_repr and
                "return False" in browser_repr
            )
            diagnostics["fast_visible_selected_async_present"] = (
                'selected_launch_path = "async_camoufox"' in browser_repr or
                'selected_launch_path="async_camoufox"' in browser_repr
            )
            diagnostics["context_viewport_sanitizer_marker_present"] = AIDA_CONTEXT_VIEWPORT_SANITIZER_V1 in browser_repr
            diagnostics["context_options_sanitizer_present"] = "def _sanitize_camoufox_context_options" in browser_repr
            diagnostics["context_no_viewport_forced_present"] = 'sanitized["no_viewport"] = True' in browser_repr
            diagnostics["context_viewport_strip_keys_present"] = all(marker in browser_repr for marker in ("_CONTEXT_VIEWPORT_DEVICE_KEYS", '"viewport"', '"screen"', '"device_scale_factor"', '"is_mobile"', '"isMobile"'))
            diagnostics["page_viewport_set_present"] = "page.set_viewport_size(target)" in browser_repr and "page_viewport_set_ok" in browser_repr
            diagnostics["protocol_schema_viewport_classification_present"] = "protocol_schema_viewport" in browser_repr and "browser.setdefaultviewport" in browser_repr.lower()
            try:
                tree = ast.parse(browser_repr, filename=spec.origin)
                forbidden = {"viewport", "screen", "device_scale_factor", "deviceScaleFactor", "is_mobile", "isMobile"}
                safe_expansion_calls = {
                    ("new_context", "_create_camoufox_safe_context"),
                    ("new_context", "_create_private_context"),
                    ("new_page", "_create_private_browser_page_context"),
                }
                safe_expansion_seen = set()
                violations = []
                function_stack = []

                class ContextViewportVisitor(ast.NodeVisitor):
                    def visit_FunctionDef(self, node):
                        function_stack.append(node.name)
                        self.generic_visit(node)
                        function_stack.pop()

                    def visit_AsyncFunctionDef(self, node):
                        function_stack.append(node.name)
                        self.generic_visit(node)
                        function_stack.pop()

                    def visit_Call(self, node):
                        func = node.func
                        if isinstance(func, ast.Attribute) and func.attr in {"new_context", "new_page"}:
                            owner = function_stack[-1] if function_stack else ""
                            if func.attr == "new_context" and not node.keywords and (func.attr, owner) not in safe_expansion_calls:
                                violations.append({"call": func.attr, "keyword": "direct", "line": getattr(node, "lineno", 0), "owner": owner})
                            for keyword in node.keywords:
                                if keyword.arg in forbidden:
                                    violations.append({"call": func.attr, "keyword": keyword.arg, "line": getattr(node, "lineno", 0), "owner": owner})
                                elif keyword.arg is None:
                                    pair = (func.attr, owner)
                                    if pair in safe_expansion_calls:
                                        safe_expansion_seen.add(pair)
                                    else:
                                        violations.append({"call": func.attr, "keyword": "kwargs", "line": getattr(node, "lineno", 0), "owner": owner})
                        self.generic_visit(node)

                ContextViewportVisitor().visit(tree)
                diagnostics["direct_context_viewport_emulation_violations"] = violations
                diagnostics["direct_context_viewport_emulation_absent"] = not violations
                diagnostics["safe_expansion_context_creation_present"] = all(pair in safe_expansion_seen for pair in safe_expansion_calls)
                diagnostics["safe_expansion_context_creation_seen"] = sorted(":".join(pair) for pair in safe_expansion_seen)
            except Exception as parse_exc:
                diagnostics["errors"].append({"module": "browser", "error_type": type(parse_exc).__name__, "error": str(parse_exc)[:300]})
        else:
            diagnostics["browser_probe_mode"] = "code_consts"
            diagnostics["browser_origin"] = spec.origin if spec else ""
            browser_module = importlib.import_module("camoufox_reverse_mcp.browser")
            diagnostics["browser_module"] = True
            snippets = [
                repr(getattr(browser_module, "AIDA_FAST_VISIBLE_POLICY_MARKER", "")),
                repr(getattr(browser_module, "AIDA_CONTEXT_VIEWPORT_SANITIZER_MARKER", "")),
            ]
            probe = _aida_module_consts_repr(
                "camoufox_reverse_mcp.browser",
                ["_use_fast_visible_launch", "_preflight_context_plan", "_sanitize_camoufox_context_options", "_apply_page_viewport_size", "_launch_error_kind", "BrowserManager.launch"],
            )
            snippets.append(probe.get("consts_repr", ""))
            diagnostics["errors"].extend(probe.get("errors", []))
            browser_repr = "".join(snippets)
            use_repr = probe.get("per_function", {}).get("_use_fast_visible_launch", "")
            launch_repr = probe.get("per_function", {}).get("BrowserManager.launch", "")
            diagnostics["fast_visible_disabled_return_present"] = (
                "False" in use_repr and
                "_flag_enabled" not in use_repr and
                "requested" not in use_repr
            )
            diagnostics["fast_visible_selected_async_present"] = (
                "async_camoufox" in launch_repr and
                "selected_launch_path" in launch_repr
            )
            sanitizer_repr = probe.get("per_function", {}).get("_sanitize_camoufox_context_options", "")
            page_viewport_repr = probe.get("per_function", {}).get("_apply_page_viewport_size", "")
            launch_error_repr = probe.get("per_function", {}).get("_launch_error_kind", "")
            diagnostics["context_viewport_sanitizer_marker_present"] = AIDA_CONTEXT_VIEWPORT_SANITIZER_V1 in browser_repr
            diagnostics["context_options_sanitizer_present"] = "_sanitize_camoufox_context_options" in browser_repr
            diagnostics["context_no_viewport_forced_present"] = "no_viewport" in sanitizer_repr and "True" in sanitizer_repr
            diagnostics["context_viewport_strip_keys_present"] = all(marker in browser_repr for marker in ("viewport", "screen", "device_scale_factor", "is_mobile", "isMobile"))
            diagnostics["page_viewport_set_present"] = "set_viewport_size" in page_viewport_repr and "page_viewport_set_ok" in page_viewport_repr
            diagnostics["protocol_schema_viewport_classification_present"] = "protocol_schema_viewport" in launch_error_repr
            safe_expansion_calls = {
                ("new_context", "_create_camoufox_safe_context"),
                ("new_context", "_create_private_context"),
                ("new_page", "_create_private_browser_page_context"),
            }
            safe_expansion_seen = set()
            violations = []
            for code in _aida_iter_module_code_objects(browser_module):
                names = set(getattr(code, "co_names", ()) or ())
                owner = getattr(code, "co_name", "")
                if "new_context" in names:
                    pair = ("new_context", owner)
                    if pair in safe_expansion_calls:
                        safe_expansion_seen.add(pair)
                    else:
                        violations.append({"call": "new_context", "keyword": "direct_or_context_options", "line": getattr(code, "co_firstlineno", 0), "owner": owner})
                if "new_page" in names:
                    pair = ("new_page", owner)
                    if pair in safe_expansion_calls:
                        safe_expansion_seen.add(pair)
            diagnostics["direct_context_viewport_emulation_violations"] = violations
            diagnostics["direct_context_viewport_emulation_absent"] = not violations
            diagnostics["safe_expansion_context_creation_present"] = all(pair in safe_expansion_seen for pair in safe_expansion_calls)
            diagnostics["safe_expansion_context_creation_seen"] = sorted(":".join(pair) for pair in safe_expansion_seen)
            for missing in probe.get("missing_functions", []):
                diagnostics["errors"].append({"module": "browser", "error_type": "missing_function", "error": missing})
        diagnostics["fast_visible_policy_marker_present"] = AIDA_FAST_VISIBLE_POLICY_V1 in browser_repr
        diagnostics["fast_visible_fallback_ignored_present"] = "aida_fast_visible_fallback_ignored" in browser_repr
        diagnostics["fast_visible_forbidden_return_absent"] = (
            "return _flag_enabled(requested)" not in browser_repr and
            'return bool(cfg.get("aida_fast_visible_launch", True))' not in browser_repr
        )
        diagnostics["fast_visible_compat_path_absent"] = (
            "fast_visible_firefox_compat" not in browser_repr and
            "launch_fast_visible_compat_selected" not in browser_repr
        )
    except Exception as exc:
        diagnostics["errors"].append({"module": "browser", "error_type": type(exc).__name__, "error": str(exc)[:300]})
        _aida_contract_probe_diag(
            "launch_policy_contract_probe_exception",
            error_type=type(exc).__name__,
            error=str(exc)[:300],
        )
    diagnostics["fast_visible_policy_contract_ok"] = (
        diagnostics["fast_visible_policy_marker_present"]
        and diagnostics["fast_visible_disabled_return_present"]
        and diagnostics["fast_visible_fallback_ignored_present"]
        and diagnostics["fast_visible_forbidden_return_absent"]
        and diagnostics["fast_visible_compat_path_absent"]
        and diagnostics["fast_visible_selected_async_present"]
        and diagnostics["context_viewport_sanitizer_marker_present"]
        and diagnostics["context_options_sanitizer_present"]
        and diagnostics["context_no_viewport_forced_present"]
        and diagnostics["context_viewport_strip_keys_present"]
        and diagnostics["page_viewport_set_present"]
        and diagnostics["protocol_schema_viewport_classification_present"]
        and diagnostics["direct_context_viewport_emulation_absent"]
        and diagnostics["safe_expansion_context_creation_present"]
        and not diagnostics["errors"]
    )
    if not diagnostics["fast_visible_policy_contract_ok"]:
        _aida_contract_probe_diag(
            "launch_policy_contract_probe_failed",
            browser_probe_mode=diagnostics["browser_probe_mode"],
            browser_origin=diagnostics["browser_origin"],
            fast_visible_policy_marker_present=diagnostics["fast_visible_policy_marker_present"],
            fast_visible_disabled_return_present=diagnostics["fast_visible_disabled_return_present"],
            fast_visible_fallback_ignored_present=diagnostics["fast_visible_fallback_ignored_present"],
            fast_visible_forbidden_return_absent=diagnostics["fast_visible_forbidden_return_absent"],
            fast_visible_compat_path_absent=diagnostics["fast_visible_compat_path_absent"],
            fast_visible_selected_async_present=diagnostics["fast_visible_selected_async_present"],
            context_viewport_sanitizer_marker_present=diagnostics["context_viewport_sanitizer_marker_present"],
            context_options_sanitizer_present=diagnostics["context_options_sanitizer_present"],
            context_no_viewport_forced_present=diagnostics["context_no_viewport_forced_present"],
            context_viewport_strip_keys_present=diagnostics["context_viewport_strip_keys_present"],
            page_viewport_set_present=diagnostics["page_viewport_set_present"],
            protocol_schema_viewport_classification_present=diagnostics["protocol_schema_viewport_classification_present"],
            direct_context_viewport_emulation_absent=diagnostics["direct_context_viewport_emulation_absent"],
            direct_context_viewport_emulation_violations=list(diagnostics["direct_context_viewport_emulation_violations"]),
            safe_expansion_context_creation_present=diagnostics["safe_expansion_context_creation_present"],
            safe_expansion_context_creation_seen=list(diagnostics["safe_expansion_context_creation_seen"]),
            errors=list(diagnostics["errors"]),
        )
    return diagnostics


def _aida_launch_budget_contract_probe():
    diagnostics = {
        "browser_module": False,
        "navigation_module": False,
        "browser_origin": "",
        "navigation_origin": "",
        "page_create_floor_present": False,
        "page_create_ceiling_present": False,
        "page_create_ratio_present": False,
        "policy_marker_present": False,
        "policy_max_ok": False,
        "policy_floor_defaults_ok": False,
        "requested_timeout_clamp_ok": False,
        "phase_budget_bounds_ok": False,
        "bundled_visible_page_budget_lower_ok": False,
        "retry_budget_contract_ok": False,
        "primary_wait_until_downgrade_present": True,
        "primary_wait_until_positive_present": False,
        "wait_for_late_page_self_pages_present": False,
        "late_page_wait_floor_present": False,
        "browser_probe_mode": "",
        "navigation_probe_mode": "",
        "policy_validation": {},
        "browser_failed_markers": [],
        "navigation_failed_markers": [],
        "errors": [],
    }
    try:
        spec = importlib.util.find_spec("camoufox_reverse_mcp.browser")
        diagnostics["browser_origin"] = (spec.origin if spec else "")
        diagnostics["browser_probe_mode"] = "source" if spec and spec.origin and spec.origin.endswith(".py") else "code_consts"
        text = ""
        tree = None
        if spec and spec.origin and os.path.isfile(spec.origin) and spec.origin.endswith(".py"):
            with open(spec.origin, "r", encoding="utf-8") as handle:
                text = handle.read()
            policy_owner, tree = _aida_launch_budget_policy_namespace_from_source(text, spec.origin)
        else:
            policy_owner = importlib.import_module("camoufox_reverse_mcp.browser")
        diagnostics["browser_module"] = True
        marker = str(policy_owner.get("AIDA_LAUNCH_BUDGET_POLICY_MARKER", "") if isinstance(policy_owner, dict) else getattr(policy_owner, "AIDA_LAUNCH_BUDGET_POLICY_MARKER", ""))
        resolver = policy_owner.get("aida_resolve_launch_budget_policy") if isinstance(policy_owner, dict) else getattr(policy_owner, "aida_resolve_launch_budget_policy", None)
        validator = policy_owner.get("aida_validate_launch_budget_policy") if isinstance(policy_owner, dict) else getattr(policy_owner, "aida_validate_launch_budget_policy", None)
        retry_resolver = policy_owner.get("aida_retry_launch_timeout_ms") if isinstance(policy_owner, dict) else getattr(policy_owner, "aida_retry_launch_timeout_ms", None)
        diagnostics["policy_marker_present"] = marker == AIDA_LAUNCH_BUDGET_POLICY_V1
        if not diagnostics["policy_marker_present"]:
            diagnostics["browser_failed_markers"].append("policy_marker:aida_launch_budget_policy_v1")
        if not callable(resolver):
            diagnostics["browser_failed_markers"].append("helper:aida_resolve_launch_budget_policy")
        if not callable(validator):
            diagnostics["browser_failed_markers"].append("helper:aida_validate_launch_budget_policy")
        if not callable(retry_resolver):
            diagnostics["browser_failed_markers"].append("helper:aida_retry_launch_timeout_ms")
        policy_validation = validator() if callable(validator) else {}
        diagnostics["policy_validation"] = policy_validation
        invariants = policy_validation.get("invariants", {}) if isinstance(policy_validation, dict) else {}
        diagnostics["policy_max_ok"] = bool(invariants.get("policy_max_not_above_40000"))
        diagnostics["policy_floor_defaults_ok"] = bool(
            invariants.get("policy_floor_positive")
            and invariants.get("default_bundled_visible_within_bounds")
            and invariants.get("default_fast_probe_within_bounds")
            and invariants.get("default_normal_within_bounds")
        )
        diagnostics["requested_timeout_clamp_ok"] = bool(
            invariants.get("requested_timeout_clamps_to_policy_max")
            and invariants.get("requested_timeout_clamps_to_policy_floor")
        )
        diagnostics["phase_budget_bounds_ok"] = bool(
            invariants.get("fast_probe_phase_budgets_nonnegative")
            and invariants.get("fast_probe_phase_budgets_within_launch_timeout")
            and invariants.get("bundled_visible_phase_budgets_nonnegative")
            and invariants.get("bundled_visible_phase_budgets_within_launch_timeout")
            and invariants.get("normal_phase_budgets_nonnegative")
            and invariants.get("normal_phase_budgets_within_launch_timeout")
            and invariants.get("oversized_phase_budgets_nonnegative")
            and invariants.get("oversized_phase_budgets_within_launch_timeout")
            and invariants.get("undersized_phase_budgets_nonnegative")
            and invariants.get("undersized_phase_budgets_within_launch_timeout")
        )
        diagnostics["bundled_visible_page_budget_lower_ok"] = bool(invariants.get("bundled_visible_page_budget_lower_than_normal"))
        diagnostics["retry_budget_contract_ok"] = bool(
            invariants.get("retry_never_exceeds_original_timeout")
            and invariants.get("retry_never_exceeds_remaining_budget")
            and invariants.get("retry_rejects_budget_below_floor")
        )
        diagnostics["page_create_floor_present"] = bool(diagnostics["policy_floor_defaults_ok"])
        diagnostics["page_create_ceiling_present"] = bool(diagnostics["policy_max_ok"] and diagnostics["requested_timeout_clamp_ok"])
        diagnostics["page_create_ratio_present"] = bool(diagnostics["phase_budget_bounds_ok"] and diagnostics["bundled_visible_page_budget_lower_ok"])
        diagnostics["late_page_wait_floor_present"] = bool(diagnostics["phase_budget_bounds_ok"])
        if callable(resolver):
            oversized = resolver(999999, bundled_visible_launch=True, fast_probe=False)
            normal = resolver(None, bundled_visible_launch=False, fast_probe=False)
            bundled = resolver(None, bundled_visible_launch=True, fast_probe=False)
            diagnostics["requested_timeout_clamp_ok"] = bool(
                diagnostics["requested_timeout_clamp_ok"]
                and int(oversized.get("launch_timeout_ms", 0)) <= 40000
                and int(oversized.get("launch_timeout_ms", 0)) == int(oversized.get("max_ms", 0))
            )
            diagnostics["bundled_visible_page_budget_lower_ok"] = bool(
                diagnostics["bundled_visible_page_budget_lower_ok"]
                and float(bundled.get("page_create_timeout_s", 0.0)) < float(normal.get("page_create_timeout_s", 0.0))
            )
            diagnostics["page_create_ceiling_present"] = bool(diagnostics["policy_max_ok"] and diagnostics["requested_timeout_clamp_ok"])
            diagnostics["page_create_ratio_present"] = bool(diagnostics["phase_budget_bounds_ok"] and diagnostics["bundled_visible_page_budget_lower_ok"])
        if callable(retry_resolver):
            retry_timeout = int(retry_resolver(40000, 16000))
            diagnostics["retry_budget_contract_ok"] = bool(diagnostics["retry_budget_contract_ok"] and 0 <= retry_timeout <= 15500)
        if spec and spec.origin and os.path.isfile(spec.origin) and spec.origin.endswith(".py"):
            for marker_text in (
                "AIDA_LAUNCH_BUDGET_POLICY_MARKER",
                "aida_resolve_launch_budget_policy",
                "aida_validate_launch_budget_policy",
                "aida_retry_launch_timeout_ms",
                "launch_budget_policy",
                "launch_budget_allocation",
            ):
                if marker_text not in text:
                    diagnostics["browser_failed_markers"].append(f"source_marker:{marker_text}")
            if tree is None:
                tree = ast.parse(text, filename=spec.origin)
            for node in ast.walk(tree):
                if isinstance(node, ast.AsyncFunctionDef) and node.name == "_wait_for_late_page":
                    references = [
                        child for child in ast.walk(node)
                        if isinstance(child, ast.Attribute) and isinstance(child.value, ast.Name)
                        and child.value.id == "self" and child.attr == "pages"
                    ]
                    diagnostics["wait_for_late_page_self_pages_present"] = bool(references)
                    break
        else:
            probe = _aida_module_consts_repr(
                "camoufox_reverse_mcp.browser",
                [
                    "aida_resolve_launch_budget_policy",
                    "aida_validate_launch_budget_policy",
                    "aida_retry_launch_timeout_ms",
                    "BrowserManager.launch",
                    "BrowserManager._wait_for_late_page",
                ],
            )
            launch_repr = probe["per_function"].get("BrowserManager.launch", "") + probe["per_function"].get("aida_resolve_launch_budget_policy", "") + probe["per_function"].get("aida_validate_launch_budget_policy", "") + probe["per_function"].get("aida_retry_launch_timeout_ms", "")
            late_repr = probe["per_function"].get("BrowserManager._wait_for_late_page", "")
            late_self_pages_co_located = (
                "'pages'" in late_repr
                and ("'self'" in late_repr or "'self_pages'" in late_repr)
            )
            diagnostics["wait_for_late_page_self_pages_present"] = late_self_pages_co_located
            for marker_text in (
                "aida_resolve_launch_budget_policy",
                "aida_validate_launch_budget_policy",
                "aida_retry_launch_timeout_ms",
                "launch_budget_policy",
                "launch_budget_allocation",
            ):
                if marker_text not in launch_repr:
                    diagnostics["browser_failed_markers"].append("code_marker:" + marker_text)
            if not late_self_pages_co_located:
                diagnostics["browser_failed_markers"].append("_wait_for_late_page:self+pages")
            for missing in probe.get("missing_functions", []):
                diagnostics["browser_failed_markers"].append("missing_function:" + missing)
            diagnostics["errors"].extend(probe["errors"])
            if diagnostics["browser_failed_markers"]:
                _aida_contract_probe_diag(
                    "browser_code_consts_markers_missing",
                    module="camoufox_reverse_mcp.browser",
                    origin=diagnostics["browser_origin"],
                    failed_markers=list(diagnostics["browser_failed_markers"]),
                    launch_repr_len=len(launch_repr),
                    late_repr_len=len(late_repr),
                )
        for name in ("policy_max_ok", "policy_floor_defaults_ok", "requested_timeout_clamp_ok", "phase_budget_bounds_ok", "bundled_visible_page_budget_lower_ok", "retry_budget_contract_ok"):
            if not diagnostics.get(name):
                diagnostics["browser_failed_markers"].append("policy_invariant:" + name)
    except Exception as exc:
        diagnostics["errors"].append({"module": "browser", "error_type": type(exc).__name__, "error": str(exc)[:300]})
        _aida_contract_probe_diag(
            "browser_probe_exception",
            error_type=type(exc).__name__,
            error=str(exc)[:300],
        )
    try:
        spec = importlib.util.find_spec("camoufox_reverse_mcp.tools.navigation")
        if spec and spec.origin and os.path.isfile(spec.origin) and spec.origin.endswith(".py"):
            diagnostics["navigation_module"] = True
            diagnostics["navigation_origin"] = spec.origin
            diagnostics["navigation_probe_mode"] = "source"
            with open(spec.origin, "r", encoding="utf-8") as handle:
                navigation_text = handle.read()
            primary_resolved_present = (
                'primary_wait_until = wait_until if wait_until in ("domcontentloaded", "networkidle", "commit", "load") else "load"'
                in navigation_text
            )
            diagnostics["primary_wait_until_positive_present"] = primary_resolved_present
            diagnostics["primary_wait_until_downgrade_present"] = not primary_resolved_present
            if not primary_resolved_present:
                diagnostics["navigation_failed_markers"].append(
                    "source:primary_wait_until_resolution_expression"
                )
                _aida_contract_probe_diag(
                    "navigation_source_marker_missing",
                    module="camoufox_reverse_mcp.tools.navigation",
                    origin=spec.origin,
                    failed_markers=list(diagnostics["navigation_failed_markers"]),
                )
        else:
            diagnostics["navigation_probe_mode"] = "code_consts"
            probe = _aida_module_consts_repr("camoufox_reverse_mcp.tools.navigation", ["navigate"])
            diagnostics["navigation_module"] = probe["available"]
            diagnostics["navigation_origin"] = (spec.origin if spec else "")
            navigate_repr = probe["per_function"].get("navigate", "")
            primary_marker = "'primary_wait_until'" in navigate_repr
            domcontent_marker = "'domcontentloaded'" in navigate_repr
            load_marker = "'load'" in navigate_repr
            resolved_event_marker = "'navigate_wait_until_resolved'" in navigate_repr
            positive_co_located = primary_marker and domcontent_marker and load_marker and resolved_event_marker
            diagnostics["primary_wait_until_positive_present"] = positive_co_located
            diagnostics["primary_wait_until_downgrade_present"] = not positive_co_located
            if not positive_co_located:
                if not primary_marker:
                    diagnostics["navigation_failed_markers"].append("primary_wait_until")
                if not domcontent_marker:
                    diagnostics["navigation_failed_markers"].append("domcontentloaded")
                if not load_marker:
                    diagnostics["navigation_failed_markers"].append("load")
                if not resolved_event_marker:
                    diagnostics["navigation_failed_markers"].append("navigate_wait_until_resolved")
            for missing in probe.get("missing_functions", []):
                diagnostics["navigation_failed_markers"].append("missing_function:" + missing)
            diagnostics["errors"].extend(probe["errors"])
            if diagnostics["navigation_failed_markers"]:
                _aida_contract_probe_diag(
                    "navigation_code_consts_markers_missing",
                    module="camoufox_reverse_mcp.tools.navigation",
                    origin=diagnostics["navigation_origin"],
                    failed_markers=list(diagnostics["navigation_failed_markers"]),
                    navigate_repr_len=len(navigate_repr),
                )
    except Exception as exc:
        diagnostics["errors"].append({"module": "navigation", "error_type": type(exc).__name__, "error": str(exc)[:300]})
        _aida_contract_probe_diag(
            "navigation_probe_exception",
            error_type=type(exc).__name__,
            error=str(exc)[:300],
        )
    ok = (
        diagnostics["page_create_floor_present"]
        and diagnostics["page_create_ceiling_present"]
        and diagnostics["page_create_ratio_present"]
        and diagnostics["policy_marker_present"]
        and diagnostics["policy_max_ok"]
        and diagnostics["policy_floor_defaults_ok"]
        and diagnostics["requested_timeout_clamp_ok"]
        and diagnostics["phase_budget_bounds_ok"]
        and diagnostics["bundled_visible_page_budget_lower_ok"]
        and diagnostics["retry_budget_contract_ok"]
        and diagnostics["late_page_wait_floor_present"]
        and diagnostics["wait_for_late_page_self_pages_present"]
        and diagnostics["primary_wait_until_positive_present"]
        and not diagnostics["primary_wait_until_downgrade_present"]
        and not diagnostics["errors"]
    )
    diagnostics["ok"] = ok
    if not ok:
        _aida_contract_probe_diag(
            "launch_budget_contract_probe_failed",
            browser_probe_mode=diagnostics["browser_probe_mode"],
            navigation_probe_mode=diagnostics["navigation_probe_mode"],
            browser_origin=diagnostics["browser_origin"],
            navigation_origin=diagnostics["navigation_origin"],
            page_create_floor_present=diagnostics["page_create_floor_present"],
            page_create_ceiling_present=diagnostics["page_create_ceiling_present"],
            page_create_ratio_present=diagnostics["page_create_ratio_present"],
            policy_marker_present=diagnostics["policy_marker_present"],
            policy_max_ok=diagnostics["policy_max_ok"],
            policy_floor_defaults_ok=diagnostics["policy_floor_defaults_ok"],
            requested_timeout_clamp_ok=diagnostics["requested_timeout_clamp_ok"],
            phase_budget_bounds_ok=diagnostics["phase_budget_bounds_ok"],
            bundled_visible_page_budget_lower_ok=diagnostics["bundled_visible_page_budget_lower_ok"],
            retry_budget_contract_ok=diagnostics["retry_budget_contract_ok"],
            late_page_wait_floor_present=diagnostics["late_page_wait_floor_present"],
            wait_for_late_page_self_pages_present=diagnostics["wait_for_late_page_self_pages_present"],
            primary_wait_until_positive_present=diagnostics["primary_wait_until_positive_present"],
            primary_wait_until_downgrade_present=diagnostics["primary_wait_until_downgrade_present"],
            browser_failed_markers=list(diagnostics["browser_failed_markers"]),
            navigation_failed_markers=list(diagnostics["navigation_failed_markers"]),
            errors=list(diagnostics["errors"]),
        )
    return diagnostics


def _aida_contract_probe():
    try:
        payload = _aida_contract_probe_from_source()
    except Exception as source_exc:
        try:
            payload = _aida_contract_probe_from_import()
            payload["source_fallback_error_type"] = type(source_exc).__name__
        except Exception as import_exc:
            payload = {
                "source": "failed",
                "ok": False,
                "params": [],
                "has_marker_constant": False,
                "error_type": type(import_exc).__name__,
                "error": str(import_exc)[:500],
                "source_error_type": type(source_exc).__name__,
                "source_error": str(source_exc)[:500],
            }
    payload["contract"] = AIDA_INITIATOR_CONTRACT_V2
    payload["required_params"] = ["request_id", "page_id", "marker"]
    launch_budget = _aida_launch_budget_contract_probe()
    addon_policy = _aida_addon_policy_contract_probe()
    launch_policy = _aida_launch_policy_contract_probe()
    payload["launch_budget_contract"] = launch_budget
    payload["addon_policy_contract"] = addon_policy
    payload["launch_policy_contract"] = launch_policy
    payload["launch_budget_contract_ok"] = bool(launch_budget.get("ok"))
    payload["addon_policy_contract_ok"] = bool(addon_policy.get("addon_policy_contract_ok"))
    payload["fast_visible_policy_contract_ok"] = bool(launch_policy.get("fast_visible_policy_contract_ok"))
    payload["fast_visible_policy_marker"] = AIDA_FAST_VISIBLE_POLICY_V1
    payload["fast_visible_policy_marker_present"] = bool(launch_policy.get("fast_visible_policy_marker_present"))
    payload["fast_visible_disabled_return_present"] = bool(launch_policy.get("fast_visible_disabled_return_present"))
    payload["fast_visible_fallback_ignored_present"] = bool(launch_policy.get("fast_visible_fallback_ignored_present"))
    payload["fast_visible_forbidden_return_absent"] = bool(launch_policy.get("fast_visible_forbidden_return_absent"))
    payload["fast_visible_compat_path_absent"] = bool(launch_policy.get("fast_visible_compat_path_absent"))
    payload["fast_visible_selected_async_present"] = bool(launch_policy.get("fast_visible_selected_async_present"))
    payload["context_viewport_sanitizer_marker"] = AIDA_CONTEXT_VIEWPORT_SANITIZER_V1
    payload["context_viewport_sanitizer_marker_present"] = bool(launch_policy.get("context_viewport_sanitizer_marker_present"))
    payload["context_options_sanitizer_present"] = bool(launch_policy.get("context_options_sanitizer_present"))
    payload["context_no_viewport_forced_present"] = bool(launch_policy.get("context_no_viewport_forced_present"))
    payload["context_viewport_strip_keys_present"] = bool(launch_policy.get("context_viewport_strip_keys_present"))
    payload["page_viewport_set_present"] = bool(launch_policy.get("page_viewport_set_present"))
    payload["protocol_schema_viewport_classification_present"] = bool(launch_policy.get("protocol_schema_viewport_classification_present"))
    payload["direct_context_viewport_emulation_absent"] = bool(launch_policy.get("direct_context_viewport_emulation_absent"))
    payload["direct_context_viewport_emulation_violations"] = list(launch_policy.get("direct_context_viewport_emulation_violations", []))
    payload["safe_expansion_context_creation_present"] = bool(launch_policy.get("safe_expansion_context_creation_present"))
    payload["safe_expansion_context_creation_seen"] = list(launch_policy.get("safe_expansion_context_creation_seen", []))
    payload["addon_policy_marker_present"] = bool(addon_policy.get("addon_policy_marker_present"))
    payload["addon_policy_marker"] = AIDA_DEFAULT_ADDON_POLICY_V1
    payload["addon_policy_log_present"] = bool(addon_policy.get("addon_policy_log_present"))
    payload["addon_invalid_diagnostic_present"] = bool(addon_policy.get("addon_invalid_diagnostic_present"))
    payload["exclude_addons_marker_present"] = bool(addon_policy.get("exclude_addons_marker_present"))
    payload["default_ubo_exclusion_present"] = bool(addon_policy.get("default_ubo_exclusion_present"))
    payload["addon_all_launch_scope_present"] = bool(addon_policy.get("addon_all_launch_scope_present"))
    payload["explicit_addons_validated_present"] = bool(addon_policy.get("explicit_addons_validated_present"))
    payload["page_create_floor_present"] = bool(launch_budget.get("page_create_floor_present"))
    payload["page_create_ceiling_present"] = bool(launch_budget.get("page_create_ceiling_present"))
    payload["page_create_ratio_present"] = bool(launch_budget.get("page_create_ratio_present"))
    payload["late_page_wait_floor_present"] = bool(launch_budget.get("late_page_wait_floor_present"))
    payload["launch_budget_policy_marker"] = AIDA_LAUNCH_BUDGET_POLICY_V1
    payload["launch_budget_policy_marker_present"] = bool(launch_budget.get("policy_marker_present"))
    payload["launch_budget_policy_max_ok"] = bool(launch_budget.get("policy_max_ok"))
    payload["launch_budget_policy_floor_defaults_ok"] = bool(launch_budget.get("policy_floor_defaults_ok"))
    payload["launch_budget_requested_timeout_clamp_ok"] = bool(launch_budget.get("requested_timeout_clamp_ok"))
    payload["launch_budget_phase_bounds_ok"] = bool(launch_budget.get("phase_budget_bounds_ok"))
    payload["launch_budget_bundled_visible_page_budget_lower_ok"] = bool(launch_budget.get("bundled_visible_page_budget_lower_ok"))
    payload["launch_budget_retry_contract_ok"] = bool(launch_budget.get("retry_budget_contract_ok"))
    payload["primary_wait_until_downgrade_absent"] = not bool(launch_budget.get("primary_wait_until_downgrade_present", True))
    payload["primary_wait_until_positive_present"] = bool(launch_budget.get("primary_wait_until_positive_present"))
    payload["wait_for_late_page_self_pages_present"] = bool(launch_budget.get("wait_for_late_page_self_pages_present"))
    payload["launch_budget_browser_probe_mode"] = launch_budget.get("browser_probe_mode", "")
    payload["launch_budget_navigation_probe_mode"] = launch_budget.get("navigation_probe_mode", "")
    payload["launch_budget_browser_failed_markers"] = list(launch_budget.get("browser_failed_markers", []))
    payload["launch_budget_navigation_failed_markers"] = list(launch_budget.get("navigation_failed_markers", []))
    if (
        not launch_budget.get("ok") or
        not addon_policy.get("addon_policy_contract_ok") or
        not launch_policy.get("fast_visible_policy_contract_ok")
    ):
        payload["ok"] = False
    return payload


def main():
    if "--aida-contract-check" in sys.argv:
        payload = _aida_contract_probe()
        print(json.dumps(payload, sort_keys=True, separators=(",", ":")), flush=True)
        if payload.get("ok"):
            raise SystemExit(0)
        launch_budget = payload.get("launch_budget_contract", {}) or {}
        browser_mode = launch_budget.get("browser_probe_mode", "")
        navigation_mode = launch_budget.get("navigation_probe_mode", "")
        browser_failed = list(launch_budget.get("browser_failed_markers", []))
        navigation_failed = list(launch_budget.get("navigation_failed_markers", []))
        frozen_mode = (
            browser_mode == "code_consts" or navigation_mode == "code_consts"
        )
        markers_absent = bool(browser_failed) or bool(navigation_failed)
        _aida_contract_probe_diag(
            "contract_check_hard_fail",
            ok=False,
            browser_probe_mode=browser_mode,
            navigation_probe_mode=navigation_mode,
            browser_failed_markers=browser_failed,
            navigation_failed_markers=navigation_failed,
            errors=list(launch_budget.get("errors", [])),
            launch_budget_contract_ok=bool(payload.get("launch_budget_contract_ok")),
            launch_budget_policy_marker_present=bool(payload.get("launch_budget_policy_marker_present")),
            launch_budget_policy_max_ok=bool(payload.get("launch_budget_policy_max_ok")),
            launch_budget_requested_timeout_clamp_ok=bool(payload.get("launch_budget_requested_timeout_clamp_ok")),
            launch_budget_phase_bounds_ok=bool(payload.get("launch_budget_phase_bounds_ok")),
            launch_budget_retry_contract_ok=bool(payload.get("launch_budget_retry_contract_ok")),
            addon_policy_contract_ok=bool(payload.get("addon_policy_contract_ok")),
            addon_policy_marker_present=bool(payload.get("addon_policy_marker_present")),
            default_ubo_exclusion_present=bool(payload.get("default_ubo_exclusion_present")),
            exclude_addons_marker_present=bool(payload.get("exclude_addons_marker_present")),
            addon_invalid_diagnostic_present=bool(payload.get("addon_invalid_diagnostic_present")),
            addon_all_launch_scope_present=bool(payload.get("addon_all_launch_scope_present")),
            fast_visible_policy_contract_ok=bool(payload.get("fast_visible_policy_contract_ok")),
            fast_visible_policy_marker_present=bool(payload.get("fast_visible_policy_marker_present")),
            fast_visible_disabled_return_present=bool(payload.get("fast_visible_disabled_return_present")),
            fast_visible_fallback_ignored_present=bool(payload.get("fast_visible_fallback_ignored_present")),
            fast_visible_forbidden_return_absent=bool(payload.get("fast_visible_forbidden_return_absent")),
            fast_visible_compat_path_absent=bool(payload.get("fast_visible_compat_path_absent")),
            fast_visible_selected_async_present=bool(payload.get("fast_visible_selected_async_present")),
            base_contract_ok=bool(payload.get("has_marker_constant"))
            and all(name in payload.get("params", []) for name in ("request_id", "page_id", "marker")),
        )
        if frozen_mode and markers_absent:
            raise SystemExit(3)
        raise SystemExit(2)

    playwright_patch = _aida_apply_playwright_pageerror_patch()
    started = time.perf_counter()
    parser = argparse.ArgumentParser(description="Camoufox Reverse Engineering MCP Server")
    parser.add_argument("--proxy", type=str, help="Proxy server URL (e.g. http://127.0.0.1:7890)")
    parser.add_argument("--headless", action="store_true", help="Run in headless mode")
    parser.add_argument("--os", type=str, default="auto",
                        choices=["auto", "windows", "macos", "linux"],
                        help="OS fingerprint to emulate (default: auto-detect host OS)")
    parser.add_argument("--locale", type=str, default="auto",
                        help="Browser locale, e.g. zh-CN, en-US (default: auto-detect)")
    parser.add_argument("--geoip", action="store_true", help="Enable GeoIP inference from proxy")
    parser.add_argument("--humanize", action="store_true", help="Enable humanized mouse movement")
    parser.add_argument("--block-images", action="store_true", help="Block image loading")
    parser.add_argument("--block-webrtc", action="store_true", help="Block WebRTC")
    parser.add_argument("--user-agent", type=str, default=None)
    parser.add_argument("--ua-policy", type=str, default="camoufox_native")
    args = parser.parse_args()

    browser_import_started = time.perf_counter()
    from .browser import BrowserManager, _camoufox_debug
    _camoufox_debug(
        "server_browser_import_ok",
        elapsed_ms=int((time.perf_counter() - browser_import_started) * 1000),
        total_ms=int((time.perf_counter() - started) * 1000),
        cwd=os.getcwd(),
        python=sys.executable,
        argv_len=len(sys.argv),
        has_proxy=bool(args.proxy),
        headless=bool(args.headless),
        os=args.os,
        locale=args.locale,
        env_browser=bool(os.environ.get("AIDA_CAMOUFOX_EXECUTABLE")),
        env_debug_log=bool(os.environ.get("AIDA_CAMOUFOX_DEBUG_LOG")),
        playwright_patch=playwright_patch,
    )

    preload_started = time.perf_counter()
    _camoufox_debug("preload_launch_options_import_begin")
    try:
        from camoufox.addons import (  # noqa: F401
            ADDONS_DIR as _PRELOAD_ADDONS_DIR,
            DefaultAddons as _PRELOAD_DEFAULT_ADDONS,
            INSTALL_DIR as _PRELOAD_INSTALL_DIR,
        )
        from camoufox.utils import launch_options as _PRELOAD_LAUNCH_OPTIONS  # noqa: F401

        _camoufox_debug(
            "preload_launch_options_import_ok",
            loaded=True,
            elapsed_ms=int((time.perf_counter() - preload_started) * 1000),
            total_ms=int((time.perf_counter() - started) * 1000),
        )
    except Exception as exc:
        import traceback as _preload_traceback

        _camoufox_debug(
            "preload_launch_options_import_fail",
            loaded=False,
            elapsed_ms=int((time.perf_counter() - preload_started) * 1000),
            total_ms=int((time.perf_counter() - started) * 1000),
            error_type=type(exc).__name__,
            error_len=len(str(exc)),
            error_summary=str(exc)[:500],
            error_traceback="".join(
                _preload_traceback.format_exception(type(exc), exc, exc.__traceback__)
            )[:4000],
        )

    BrowserManager.default_config = {
        "proxy": {"server": args.proxy} if args.proxy else None,
        "headless": args.headless,
        "os": args.os,
        "locale": args.locale,
        "geoip": args.geoip,
        "humanize": args.humanize,
        "block_images": args.block_images,
        "block_webrtc": True,
        "user_agent": args.user_agent,
        "ua_policy": args.ua_policy,
    }

    server_import_started = time.perf_counter()
    _camoufox_debug("server_import_begin")
    from .server import mcp
    _camoufox_debug(
        "server_import_ok",
        elapsed_ms=int((time.perf_counter() - server_import_started) * 1000),
        total_ms=int((time.perf_counter() - started) * 1000),
    )
    _camoufox_debug("server_run_begin", transport="stdio")
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
