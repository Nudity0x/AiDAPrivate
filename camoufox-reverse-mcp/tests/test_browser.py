import ast
import asyncio
import inspect
import time

import pytest
import camoufox_reverse_mcp.browser as browser_module
from camoufox_reverse_mcp.browser import (
    AIDA_CONTEXT_VIEWPORT_SANITIZER_MARKER,
    BrowserManager,
    DEFAULT_WINDOW_SIZE,
    _apply_page_viewport_size,
    _derive_ff_version,
    _launch_error_kind,
    _path_info,
    _prepare_profile_dir,
    _resolve_window_size,
    _sanitize_camoufox_context_options,
)


def test_browser_manager_init():
    mgr = BrowserManager()
    assert mgr.browser is None
    assert mgr.active_page_name is None
    assert len(mgr.contexts) == 0
    assert len(mgr.pages) == 0
    assert mgr._capturing is False


def test_default_config():
    assert isinstance(BrowserManager.default_config, dict)


def test_console_logs_maxlen():
    mgr = BrowserManager()
    assert mgr._console_logs.maxlen == 2000


def test_network_requests_maxlen():
    mgr = BrowserManager()
    assert mgr._network_requests.maxlen == 2000


def test_persistent_scripts_init():
    mgr = BrowserManager()
    assert isinstance(mgr._persistent_scripts, list)
    assert len(mgr._persistent_scripts) == 0


def test_persistent_traces_init():
    mgr = BrowserManager()
    assert isinstance(mgr._persistent_traces, dict)
    assert len(mgr._persistent_traces) == 0


def test_capture_body_default():
    mgr = BrowserManager()
    assert mgr._capture_body is False


def test_init_scripts_list():
    mgr = BrowserManager()
    assert isinstance(mgr._init_scripts, list)
    assert len(mgr._init_scripts) == 0


def test_default_window_size_is_fixed():
    window, diag = _resolve_window_size({})
    assert window[0] <= DEFAULT_WINDOW_SIZE[0]
    assert window[1] <= DEFAULT_WINDOW_SIZE[1]
    assert diag["width"] == window[0]
    assert diag["height"] == window[1]


def test_window_size_request_is_clamped_to_minimum():
    window, diag = _resolve_window_size({"window_width": 100, "window_height": 100})
    assert window[0] >= 480
    assert window[1] >= 480
    assert diag["requested_width"] == 100
    assert diag["requested_height"] == 100


def test_derive_ff_version_from_packaged_path():
    assert _derive_ff_version(r"C:\deps\camoufox-135.0.1-beta.24-win.x86_64\camoufox.exe") == 135


def test_path_info_missing_file(tmp_path):
    info = _path_info(str(tmp_path / "missing.exe"))
    assert info["exists"] is False
    assert info["is_file"] is False


def test_prepare_profile_dir_creates_unique_directory(tmp_path):
    profile = tmp_path / "profile-1"
    resolved, info = _prepare_profile_dir(str(profile), generated=True)
    assert profile.exists()
    assert resolved == str(profile)
    assert info["generated"] is True
    assert info["locks"] == 0


def test_launch_browser_tool_timeout_error_is_structured(monkeypatch):
    from camoufox_reverse_mcp.tools import navigation

    async def fail_launch(_config):
        raise asyncio.TimeoutError()

    monkeypatch.setattr(navigation.browser_manager, "launch", fail_launch)
    result = asyncio.run(navigation.launch_browser(launch_timeout_ms=5000, bridge_attempt_id="unit-timeout"))
    assert result["error"]
    assert result["error_type"] == "TimeoutError"
    assert result["phase"] == "launch_browser"
    assert result["launch_attempt_id"] == "unit-timeout"
    assert "TimeoutError" in result["error_summary"]


def test_launch_browser_tool_target_closed_error_is_structured(monkeypatch):
    from camoufox_reverse_mcp.tools import navigation

    class TargetClosedError(Exception):
        pass

    async def fail_launch(_config):
        raise TargetClosedError("Target page, context or browser has been closed")

    monkeypatch.setattr(navigation.browser_manager, "launch", fail_launch)
    result = asyncio.run(navigation.launch_browser(launch_timeout_ms=5000, bridge_attempt_id="unit-target-closed"))
    assert result["error"]
    assert result["error_type"] == "TargetClosedError"
    assert result["error_kind"] == "target_closed"
    assert result["launch_attempt_id"] == "unit-target-closed"


def test_context_option_sanitizer_strips_context_viewport_device_emulation():
    sanitized, page_viewport, diagnostics = _sanitize_camoufox_context_options(
        {
            "viewport": {"width": 1920, "height": 1080, "isMobile": False, "deviceScaleFactor": 1},
            "screen": {"width": 1920, "height": 1080},
            "device_scale_factor": 2,
            "deviceScaleFactor": 2,
            "is_mobile": False,
            "isMobile": False,
            "proxy": {"server": "http://127.0.0.1:8080"},
            "service_workers": "allow",
            "storage_state": "state.json",
            "locale": "en-US",
        }
    )
    forbidden = {"viewport", "screen", "device_scale_factor", "deviceScaleFactor", "is_mobile", "isMobile"}
    assert not forbidden.intersection(sanitized)
    assert sanitized["no_viewport"] is True
    assert sanitized["proxy"] == {"server": "http://127.0.0.1:8080"}
    assert sanitized["service_workers"] == "allow"
    assert sanitized["storage_state"] == "state.json"
    assert sanitized["locale"] == "en-US"
    assert page_viewport == {"width": 1920, "height": 1080}
    assert diagnostics["sanitizer_marker"] == AIDA_CONTEXT_VIEWPORT_SANITIZER_MARKER
    assert diagnostics["viewport_policy"] == "context_no_viewport_page_set"
    assert "viewport" in diagnostics["stripped_keys"]
    assert "viewport.isMobile" in diagnostics["stripped_keys"]
    assert "viewport.deviceScaleFactor" in diagnostics["stripped_keys"]


def test_protocol_schema_viewport_error_is_classified():
    exc = RuntimeError(
        'Browser.new_context: Protocol error (Browser.setDefaultViewport): ERROR: failed to call method '
        'Found property "<root>.viewport.isMobile" - false which is not described in this scheme'
    )
    assert _launch_error_kind(exc, True) == "protocol_schema_viewport"
    page_exc = RuntimeError(
        'Page.set_viewport_size: Protocol error (Page.setViewportSize): ERROR: failed to call method '
        'Found property "<root>.screenSize" - {"width": 1280, "height": 744} which is not described in this scheme'
    )
    assert _launch_error_kind(page_exc, True) == "protocol_schema_viewport"


def test_page_viewport_size_is_applied_after_page_creation():
    class FakePage:
        viewport_size = {}

        async def set_viewport_size(self, viewport):
            self.viewport_size = dict(viewport)

    page = FakePage()
    result = asyncio.run(_apply_page_viewport_size(page, {"width": 1600, "height": 900}, "unit_viewport"))
    assert result["applied"] is True
    assert result["target"] == {"width": 1600, "height": 900}
    assert page.viewport_size == {"width": 1600, "height": 900}


def test_page_viewport_protocol_schema_error_is_nonfatal():
    class FakePage:
        viewport_size = {}

        async def set_viewport_size(self, _viewport):
            raise RuntimeError(
                'Page.set_viewport_size: Protocol error (Page.setViewportSize): ERROR: failed to call method '
                'Found property "<root>.screenSize" - {"width": 1280, "height": 744} which is not described in this scheme'
            )

    result = asyncio.run(_apply_page_viewport_size(FakePage(), {"width": 1280, "height": 744}, "unit_viewport"))
    assert result["applied"] is False
    assert result["ignored"] is True
    assert result["error_kind"] == "protocol_schema_viewport"


def test_context_creation_kwargs_are_sanitizer_gated():
    tree = ast.parse(inspect.getsource(browser_module))
    forbidden = {"viewport", "screen", "device_scale_factor", "deviceScaleFactor", "is_mobile", "isMobile"}
    safe_expansion_calls = {
        ("new_context", "_create_camoufox_safe_context"),
        ("new_context", "_create_private_context"),
        ("new_page", "_create_private_browser_page_context"),
    }
    function_stack = []
    violations = []
    seen = set()

    class Visitor(ast.NodeVisitor):
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
                    violations.append((func.attr, "direct", owner))
                for keyword in node.keywords:
                    if keyword.arg in forbidden:
                        violations.append((func.attr, keyword.arg, owner))
                    elif keyword.arg is None:
                        pair = (func.attr, owner)
                        if pair in safe_expansion_calls:
                            seen.add(pair)
                        else:
                            violations.append((func.attr, "kwargs", owner))
            self.generic_visit(node)

    Visitor().visit(tree)
    assert violations == []
    assert seen == safe_expansion_calls


def test_page_closed_during_launch_records_lifecycle_event():
    class FakeContext:
        pages = []

    class FakePage:
        _guid = "fake-guid"
        context = FakeContext()

        def is_closed(self):
            return True

        @property
        def url(self):
            return "about:blank"

    mgr = BrowserManager()
    page = FakePage()
    mgr.browser = object()
    mgr.pages["default"] = page
    mgr.page_meta["default"] = {"page_id": "default", "context_id": "default"}
    mgr.active_page_id = "default"
    mgr.active_page_name = "default"
    mgr._set_launch_phase("privacy_verify", time.perf_counter(), "unit-close")
    mgr._mark_page_terminal("default", page, "closed", "close_event")
    events = mgr._page_event_tail(None, 8)
    assert any(event.get("event") == "page_closed" and event.get("during_launch_before_privacy") is True for event in events)
    assert mgr._active_launch_terminal_reason == "page_closed"
    assert mgr._active_launch_terminal_payload.get("event") == "page_closed_during_launch"
