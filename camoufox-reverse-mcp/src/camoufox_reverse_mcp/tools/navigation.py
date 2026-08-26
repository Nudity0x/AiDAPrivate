from __future__ import annotations

import asyncio
import base64
import contextlib
import json as _json
import os
import time
import traceback as _traceback

from ..browser import (
    AIDA_LAUNCH_DEFAULT_BUNDLED_VISIBLE_MS,
    AIDA_LAUNCH_FLOOR_MS,
    AIDA_NAVIGATION_DEFAULT_TIMEOUT_MS,
    aida_clamp_navigation_timeout_ms,
    aida_resolve_launch_budget_policy,
    _await_no_cancel_wait,
    _camoufox_debug,
    _safe_text,
    _target_domain,
)
from ..server import mcp, browser_manager

_PRE_INJECT_REGISTER_TIMEOUT = 10.0
_NAVIGATION_CAPTURE_INLINE_LIMIT = 80


def _is_bloxflip_url(url: str | None) -> bool:
    domain = _target_domain(url).lower()
    return domain == "bloxflip.com" or domain.endswith(".bloxflip.com")


async def _log_navigation_phase(event_name: str, phase: str, page, page_id: str | None, url: str, generation: int, started: float, last_error: str = "") -> dict:
    try:
        snapshot = await browser_manager.navigation_diagnostic_snapshot(
            phase,
            page,
            page_id,
            url,
            generation,
            started,
            False,
            last_error,
        )
        _camoufox_debug(event_name, phase=phase, bloxflip=_is_bloxflip_url(url), **snapshot)
        return snapshot
    except Exception as exc:
        payload = {
            "phase": phase,
            "page_id": page_id or "",
            "target_url": _safe_text(url, 500),
            "target_domain": _target_domain(url),
            "error_type": type(exc).__name__,
            "error_summary": _safe_text(exc, 500),
        }
        _camoufox_debug(f"{event_name}_failed", **payload)
        return payload


class _NavigationLifecycleError(RuntimeError):
    def __init__(self, phase: str, reason: str, snapshot: dict):
        self.phase = phase
        self.reason = reason
        self.snapshot = snapshot
        super().__init__(f"navigation lifecycle degraded at {phase}: {reason}")


def _browser_process_count(snapshot: dict) -> int:
    detail = snapshot.get("process_tree_detail") if isinstance(snapshot, dict) else None
    records = detail.get("records") if isinstance(detail, dict) else None
    if not isinstance(records, list) or not records:
        return -1
    count = 0
    for record in records:
        if not isinstance(record, dict):
            continue
        exe = str(record.get("exe") or "").lower()
        if "camoufox" in exe and record.get("alive") is not False:
            count += 1
    return count


def _recent_terminal_event(snapshot: dict) -> str:
    events = snapshot.get("recent_page_events") if isinstance(snapshot, dict) else None
    if not isinstance(events, list):
        return ""
    for event in reversed(events[-16:]):
        if not isinstance(event, dict):
            continue
        event_type = str(event.get("event") or event.get("type") or event.get("reason") or "")
        event_low = event_type.lower()
        if "browser_disconnected" in event_low:
            return "browser_disconnected_event"
        if "page_crashed" in event_low or "crash" in event_low:
            return "page_crashed_event"
        if "page_closed" in event_low or "context_close" in event_low or "closed" in event_low:
            return "page_closed_event"
    return ""


def _navigation_snapshot_failure_reason(snapshot: dict) -> str:
    if not isinstance(snapshot, dict) or not snapshot:
        return "missing_navigation_snapshot"
    if snapshot.get("browser_open") is False:
        return "browser_not_open"
    if snapshot.get("browser_connected") is False:
        return "browser_disconnected"
    if snapshot.get("page_present") is False:
        return "page_missing"
    page_meta = snapshot.get("page_meta") if isinstance(snapshot.get("page_meta"), dict) else {}
    if page_meta.get("crashed"):
        return "page_crashed"
    if snapshot.get("page_closed") is True or page_meta.get("closed"):
        terminal = str(page_meta.get("terminal_reason") or snapshot.get("page_close_reason") or "page_closed")
        source = str(page_meta.get("terminal_source") or snapshot.get("page_close_source") or "")
        return f"{terminal}:{source}" if source else terminal
    terminal_event = _recent_terminal_event(snapshot)
    if terminal_event:
        return terminal_event
    process_count = _browser_process_count(snapshot)
    detail = snapshot.get("process_tree_detail") if isinstance(snapshot.get("process_tree_detail"), dict) else {}
    if process_count >= 0 and process_count < 2:
        return f"browser_process_tree_degraded browser_processes={process_count} record_count={detail.get('record_count', 0)}"
    return ""


def _navigation_request_summary(entry: dict) -> dict:
    response_body = entry.get("response_body")
    response_body_length = int(entry.get("response_body_length") or entry.get("body_length") or (len(response_body) if response_body else 0) or 0)
    request_body = entry.get("request_body") or entry.get("request_post_data") or entry.get("post_data") or ""
    request_body_length = int(entry.get("request_body_length") or len(request_body or "") or 0)
    request_id = entry.get("request_id", entry.get("id"))
    url = str(entry.get("url") or "")
    return {
        "id": entry.get("id"),
        "request_id": request_id,
        "network_request_id": entry.get("network_request_id", request_id),
        "page_id": entry.get("page_id"),
        "context_id": entry.get("context_id"),
        "url": url[:240],
        "url_len": len(url),
        "method": entry.get("method"),
        "status": entry.get("status"),
        "status_code": entry.get("status_code", entry.get("status")),
        "type": entry.get("resource_type"),
        "resource_type": entry.get("resource_type"),
        "ms": entry.get("duration"),
        "duration_ms": entry.get("duration_ms", entry.get("duration")),
        "size": response_body_length,
        "body_length": response_body_length,
        "request_body_length": request_body_length,
        "response_body_length": response_body_length,
        "response_body_available": response_body is not None,
        "failed": bool(entry.get("failed")),
        "failure": entry.get("failure", ""),
        "websocket": bool(entry.get("websocket") or entry.get("resource_type") == "websocket"),
        "redirected_from": entry.get("redirected_from", ""),
        "redirect_chain_count": len(entry.get("redirect_chain") or []) if isinstance(entry.get("redirect_chain"), list) else 0,
    }


def _navigation_capture_summary(page_id: str | None, limit: int = _NAVIGATION_CAPTURE_INLINE_LIMIT) -> dict:
    all_requests = []
    try:
        for entry in list(browser_manager._network_requests):
            if page_id and entry.get("page_id") not in {page_id, None}:
                continue
            all_requests.append(entry)
    except Exception as exc:
        return {
            "status": "capture_summary_failed",
            "error": _safe_text(exc, 500),
            "requests": [],
            "count": 0,
            "returned_count": 0,
            "total_count": 0,
            "truncated": False,
            "capture_compacted": True,
        }
    safe_limit = max(0, int(limit or 0))
    summaries = [_navigation_request_summary(entry) for entry in all_requests[:safe_limit]]
    return {
        "status": "captured" if summaries else "empty",
        "requests": summaries,
        "count": len(summaries),
        "returned_count": len(summaries),
        "total_count": len(all_requests),
        "truncated": len(all_requests) > len(summaries),
        "limit": safe_limit,
        "page_id": page_id,
        "active": browser_manager._capturing,
        "capture_compacted": True,
        "body_access": "browser_network.get_request",
    }


def _process_records(snapshot: dict) -> dict[int, dict]:
    detail = snapshot.get("process_tree_detail") if isinstance(snapshot, dict) else None
    records = detail.get("records") if isinstance(detail, dict) else None
    out: dict[int, dict] = {}
    if not isinstance(records, list):
        return out
    for record in records:
        if not isinstance(record, dict):
            continue
        try:
            pid = int(record.get("pid") or 0)
        except Exception:
            pid = 0
        if pid > 0:
            out[pid] = dict(record)
    return out


def _process_delta_evidence(before: dict, after: dict) -> dict:
    before_map = _process_records(before)
    after_map = _process_records(after)
    before_pids = set(before_map)
    after_pids = set(after_map)
    exited = sorted(before_pids - after_pids)
    started = sorted(after_pids - before_pids)
    observed_ms = int(time.time() * 1000)
    exited_records = []
    for pid in exited[:48]:
        record = dict(before_map.get(pid) or {})
        record["observed_exit_ts_ms"] = observed_ms
        record["exit_observed_by"] = "navigation_snapshot_delta"
        exited_records.append(record)
    return {
        "before_count": len(before_pids),
        "after_count": len(after_pids),
        "exited_count": len(exited),
        "started_count": len(started),
        "survived_count": len(before_pids & after_pids),
        "observed_ts_ms": observed_ms,
        "exited_pids": exited[:96],
        "started_pids": started[:96],
        "exited_records": exited_records,
        "started_records": [after_map[pid] for pid in started[:48]],
    }


def _camoufox_exit_records_from_delta(delta: dict) -> list[dict]:
    out = []
    records = delta.get("exited_records") if isinstance(delta, dict) else None
    if not isinstance(records, list):
        return out
    for record in records:
        if not isinstance(record, dict):
            continue
        exe = str(record.get("exe") or "").lower()
        if "camoufox" in exe:
            out.append(record)
    return out


def _latest_driver_evidence(snapshot: dict) -> dict:
    candidates: list[dict] = []
    for source in (
        snapshot.get("subprocesses") if isinstance(snapshot, dict) else None,
        (snapshot.get("launch_diagnostics") or {}).get("subprocesses") if isinstance(snapshot, dict) and isinstance(snapshot.get("launch_diagnostics"), dict) else None,
    ):
        if not isinstance(source, dict):
            continue
        latest = source.get("latest_driver")
        if isinstance(latest, dict) and latest:
            candidates.append(latest)
        records = source.get("records")
        if isinstance(records, list):
            candidates.extend(record for record in records if isinstance(record, dict) and record.get("driver_like"))
    if not candidates:
        return {}
    latest = candidates[-1]
    return {
        "id": latest.get("id"),
        "pid": latest.get("pid"),
        "exited": bool(latest.get("exited")),
        "exit_code": latest.get("exit_code"),
        "exit_ts_ms": latest.get("exit_ts_ms"),
        "exit_elapsed_ms": latest.get("exit_elapsed_ms"),
        "stdout_capture": latest.get("stdout_capture", ""),
        "stderr_capture": latest.get("stderr_capture", {}),
        "argv": latest.get("argv", {}),
        "env": latest.get("env", {}),
    }


def _cloudflare_challenge_markers(response_chain: list[dict], before: dict, after: dict) -> dict:
    markers: set[str] = set()
    samples: list[dict] = []
    def scan_url(value: str, source: str, status: object = None) -> None:
        text = str(value or "")
        low = text.lower()
        found = []
        for marker in ("cdn-cgi", "cf-chl", "challenge-platform", "turnstile", "cloudflare", "__cf_bm", "cf_clearance"):
            if marker in low:
                markers.add(marker)
                found.append(marker)
        if found and len(samples) < 24:
            samples.append({"source": source, "url_len": len(text), "domain": _target_domain(text), "status": status, "markers": found})
    for item in response_chain if isinstance(response_chain, list) else []:
        if isinstance(item, dict):
            scan_url(str(item.get("url") or ""), "response_chain", item.get("status"))
            for redirect in item.get("redirect_chain") or []:
                if isinstance(redirect, dict):
                    scan_url(str(redirect.get("url") or ""), "redirect_chain", redirect.get("status"))
    for source_name, snapshot in (("before", before), ("after", after)):
        if not isinstance(snapshot, dict):
            continue
        scan_url(str(snapshot.get("current_url") or ""), f"{source_name}_current", snapshot.get("document_response_status"))
        scan_url(str(snapshot.get("target_url") or ""), f"{source_name}_target", snapshot.get("document_response_status"))
        for item in snapshot.get("response_chain") or []:
            if isinstance(item, dict):
                scan_url(str(item.get("url") or ""), f"{source_name}_snapshot_chain", item.get("status"))
    return {
        "present": bool(markers),
        "markers": sorted(markers),
        "samples": samples,
    }


def _navigation_evidence_bundle(variant: str, before: dict, after: dict, phases: list[dict], response_chain: list[dict]) -> dict:
    process_delta = _process_delta_evidence(before or {}, after or {})
    camoufox_exits = _camoufox_exit_records_from_delta(process_delta)
    previous_snapshot = before if isinstance(before, dict) else {}
    phase_deltas = []
    for phase in phases if isinstance(phases, list) else []:
        snapshot = phase.get("snapshot") if isinstance(phase, dict) else None
        if isinstance(snapshot, dict) and previous_snapshot:
            delta = _process_delta_evidence(previous_snapshot, snapshot)
            phase_deltas.append({"phase": phase.get("phase", ""), "delta": delta})
            camoufox_exits.extend(_camoufox_exit_records_from_delta(delta))
            previous_snapshot = snapshot
    deduped_exits: dict[int, dict] = {}
    for record in camoufox_exits:
        try:
            pid = int(record.get("pid") or 0)
        except Exception:
            pid = 0
        if pid > 0:
            deduped_exits[pid] = record
    driver = _latest_driver_evidence(after or {}) or _latest_driver_evidence(before or {})
    page_meta = after.get("page_meta") if isinstance(after.get("page_meta"), dict) else {}
    return {
        "variant": _safe_text(variant or "current_launch", 120),
        "node_driver": driver,
        "node_exit_code": driver.get("exit_code") if isinstance(driver, dict) else None,
        "node_exit_ts_ms": driver.get("exit_ts_ms") if isinstance(driver, dict) else None,
        "camoufox_child_exit_count": len(deduped_exits),
        "camoufox_child_exits": list(deduped_exits.values())[:48],
        "browser_disconnected": after.get("browser_connected") is False if isinstance(after, dict) else None,
        "page_present": after.get("page_present") if isinstance(after, dict) else None,
        "page_closed": (after.get("page_closed") is True or page_meta.get("closed") is True) if isinstance(after, dict) else None,
        "page_crashed": page_meta.get("crashed") is True,
        "process_before": before.get("process_tree_detail") or before.get("process_tree") if isinstance(before, dict) else {},
        "process_after": after.get("process_tree_detail") or after.get("process_tree") if isinstance(after, dict) else {},
        "process_delta": process_delta,
        "phase_process_deltas": phase_deltas[-16:],
        "cloudflare": _cloudflare_challenge_markers(response_chain or [], before or {}, after or {}),
    }


async def _capture_navigation_phase(
    event_name: str,
    phase: str,
    page,
    page_id: str | None,
    url: str,
    generation: int,
    started: float,
    phase_snapshots: list[dict],
    last_error: str = "",
    enforce_lifecycle: bool = False,
) -> dict:
    snapshot = await _log_navigation_phase(event_name, phase, page, page_id, url, generation, started, last_error)
    reason = _navigation_snapshot_failure_reason(snapshot) if enforce_lifecycle else ""
    phase_snapshots.append({
        "phase": phase,
        "elapsed_ms": snapshot.get("elapsed_ms", int((time.perf_counter() - started) * 1000)) if isinstance(snapshot, dict) else int((time.perf_counter() - started) * 1000),
        "failure_reason": reason,
        "snapshot": snapshot,
    })
    if reason:
        raise _NavigationLifecycleError(phase, reason, snapshot)
    return snapshot


async def _begin_page_tool_operation(page_id: str | None, operation: str, target: str = "", aida_operation_id=None) -> dict:
    started = time.perf_counter()
    generation = browser_manager._next_diagnostic_id()
    page = await browser_manager.resolve_page_for_operation(page_id, operation, True, aida_operation_id)
    resolved_page_id = browser_manager.page_id_for(page) or page_id
    operation_id = browser_manager.begin_page_operation(resolved_page_id, operation, target, generation, aida_operation_id)
    before = await browser_manager.navigation_diagnostic_snapshot(
        f"{operation}_before",
        page,
        resolved_page_id,
        target,
        generation,
        started,
        False,
    )
    browser_manager.log_navigation_diagnostic(f"{operation}_page_state_before", before)
    return {
        "started": started,
        "generation": generation,
        "page": page,
        "page_id": resolved_page_id,
        "operation_id": operation_id,
        "external_operation_id": _safe_text(aida_operation_id, 120) if aida_operation_id is not None else "",
        "before": before,
        "target": target,
        "operation": operation,
    }


async def _finish_page_tool_operation(state: dict, status: str, exc: Exception | None = None) -> dict:
    page = state.get("page")
    page_id = state.get("page_id")
    operation = str(state.get("operation") or "operation")
    started = float(state.get("started") or time.perf_counter())
    generation = int(state.get("generation") or 0)
    target = str(state.get("target") or "")
    after = {}
    try:
        after = await browser_manager.navigation_diagnostic_snapshot(
            f"{operation}_after",
            page,
            page_id,
            target,
            generation,
            started,
            False,
            _safe_text(exc, 700) if exc else "",
        )
        browser_manager.log_navigation_diagnostic(f"{operation}_page_state_after", after)
    except Exception as snapshot_exc:
        after = {
            "snapshot_error_type": type(snapshot_exc).__name__,
            "snapshot_error": _safe_text(snapshot_exc, 500),
            "page_id": page_id or "",
            "operation": operation,
        }
    if exc is not None:
        with contextlib.suppress(Exception):
            browser_manager.handle_page_operation_exception(page_id, page, operation, exc)
    finish = browser_manager.finish_page_operation(page_id, state.get("operation_id"), status, exc)
    return {
        "operation_id": state.get("operation_id"),
        "external_operation_id": state.get("external_operation_id", ""),
        "generation": generation,
        "before": state.get("before", {}),
        "after": after,
        "finish": finish,
    }


@mcp.tool()
async def launch_browser(
    headless: bool = False,
    os_type: str = "auto",
    locale: str = "auto",
    proxy: str | None = None,
    humanize: bool = False,
    geoip: bool = False,
    block_images: bool = False,
    block_webrtc: bool = True,
    user_agent: str | None = None,
    userAgent: str | None = None,
    ua_policy: str = "camoufox_native",
    user_agent_profile: str | None = None,
    user_agent_mode: str | None = None,
    enable_trace: bool = False,
    window_width: int = 1280,
    window_height: int = 900,
    executable_path: str | None = None,
    ff_version: int | None = None,
    launch_timeout_ms: int = AIDA_LAUNCH_DEFAULT_BUNDLED_VISIBLE_MS,
    persistent_context: bool = False,
    profile_dir: str | None = None,
    user_data_dir: str | None = None,
    privacy_fail_closed: bool = True,
    service_workers: str | None = None,
    block_service_workers: bool = False,
    aida_fast_visible_launch: bool | None = None,
    aida_launch_policy_marker: str | None = None,
    bridge_generation: int | str | None = None,
    bridge_session_id: str | None = None,
    bridge_attempt_id: str | None = None,
) -> dict:
    """Launch the Camoufox anti-detection browser.

    Args:
        headless: Run in headless mode (default False).
        os_type: OS fingerprint - "auto", "windows", "macos", or "linux".
        locale: Browser locale (e.g. "zh-CN"). "auto" detects system locale.
        proxy: Proxy server URL (e.g. "http://127.0.0.1:7890").
        humanize: Enable humanized mouse movement.
        geoip: Auto-infer geolocation from proxy IP.
        block_images: Block image loading.
        block_webrtc: Block WebRTC to prevent IP leaks.
        enable_trace: Enable engine-level property access tracing.
            Requires camoufox-reverse custom browser build.
            When enabled, use trace_property_access() to capture DOM access.

    Returns:
        dict with status, config, and page list.
    """
    try:
        bundled_visible_launch = bool(executable_path or os.environ.get("AIDA_CAMOUFOX_EXECUTABLE")) and not headless
        launch_policy = aida_resolve_launch_budget_policy(
            launch_timeout_ms,
            bundled_visible_launch=bundled_visible_launch,
            fast_probe=False,
        )
        config = {
            "headless": headless, "os": os_type, "locale": locale,
            "humanize": humanize, "geoip": geoip,
            "block_images": block_images, "block_webrtc": block_webrtc,
            "ua_policy": user_agent_profile or user_agent_mode or ua_policy,
            "enable_trace": enable_trace,
            "window_width": window_width,
            "window_height": window_height,
            "launch_timeout_ms": int(launch_policy["launch_timeout_ms"]),
            "aida_launch_budget_policy": launch_policy,
            "privacy_fail_closed": privacy_fail_closed,
        }
        selected_user_agent = user_agent or userAgent
        if selected_user_agent:
            config["user_agent"] = selected_user_agent
        if executable_path:
            config["executable_path"] = executable_path
        if ff_version is not None:
            try:
                config["ff_version"] = int(ff_version)
            except (TypeError, ValueError):
                pass
        if persistent_context or profile_dir or user_data_dir:
            config["persistent_context"] = True
        if profile_dir:
            config["profile_dir"] = profile_dir
        if user_data_dir:
            config["user_data_dir"] = user_data_dir
        if service_workers is not None:
            config["service_workers"] = str(service_workers)
        if block_service_workers:
            config["block_service_workers"] = True
        if aida_fast_visible_launch is not None:
            config["aida_fast_visible_launch"] = bool(aida_fast_visible_launch)
        if aida_launch_policy_marker:
            config["aida_launch_policy_marker"] = str(aida_launch_policy_marker)
        if bridge_generation is not None:
            config["bridge_generation"] = bridge_generation
        if bridge_session_id:
            config["bridge_session_id"] = bridge_session_id
        if bridge_attempt_id:
            config["bridge_attempt_id"] = bridge_attempt_id
        if proxy:
            config["proxy"] = {"server": proxy}
        result = await browser_manager.launch(config)

        if result.get("status") == "already_running":
            result["persistent_scripts_count"] = len(browser_manager._persistent_scripts)
            result["active_captures"] = browser_manager._capturing
            result["captured_requests_count"] = len(browser_manager._network_requests)
            from .instrumentation import _active_routes
            result["active_routes"] = len(_active_routes)
            has_residuals = (
                len(browser_manager._persistent_scripts) > 0
                or len(browser_manager._network_requests) > 0
                or len(_active_routes) > 0
            )
            if has_residuals:
                result.setdefault("warnings", []).append(
                    "browser already running with residual state. "
                    "Call reset_browser_state() or close_browser() + launch_browser()."
                )

        return result
    except Exception as e:
        payload = browser_manager.last_launch_failure_payload(e, "launch_browser")
        payload["status"] = payload.get("status") or "error"
        payload["error_type"] = payload.get("error_type") or type(e).__name__
        payload["phase"] = payload.get("phase") or "launch_browser"
        payload["error_summary"] = payload.get("error_summary") or f"{type(e).__name__} during {payload['phase']}"
        payload["error"] = payload.get("error") or payload["error_summary"]
        if bridge_attempt_id and not payload.get("launch_attempt_id"):
            payload["launch_attempt_id"] = str(bridge_attempt_id)
        if bridge_attempt_id and not payload.get("attempt_id"):
            payload["attempt_id"] = str(bridge_attempt_id)
        if bridge_generation is not None and not payload.get("generation"):
            payload["generation"] = bridge_generation
        if bridge_session_id and not payload.get("session_id"):
            payload["session_id"] = str(bridge_session_id)
        _camoufox_debug(
            "launch_browser_tool_exception",
            error_type=payload["error_type"],
            error_kind=payload.get("error_kind", ""),
            error_summary=payload["error_summary"],
            phase=payload["phase"],
            elapsed_ms=payload.get("elapsed_ms", 0),
            last_debug_event_name=payload.get("last_debug_event_name", ""),
            browser_open=payload.get("browser_open", False),
            browser_connected=payload.get("browser_connected", False),
            registered_pages=payload.get("registered_pages", 0),
            context_count=payload.get("context_count", 0),
            page_count=payload.get("page_count", -1),
        )
        return payload


@mcp.tool()
async def close_browser() -> dict:
    """Close the Camoufox browser and release all resources."""
    try:
        return await browser_manager.close()
    except Exception as e:
        return {"error": str(e)}


@mcp.tool()
async def list_pages() -> dict:
    try:
        pages = await browser_manager.list_pages()
        return {
            "session_id": browser_manager.session_id,
            "active_page_id": browser_manager.active_page_id,
            "page_count": len(pages),
            "pages": pages,
        }
    except Exception as e:
        return {"error": str(e)}


@mcp.tool()
async def new_page(url: str | None = None, page_id: str | None = None, make_active: bool = True) -> dict:
    try:
        return await browser_manager.new_page(url=url, page_id=page_id, make_active=make_active)
    except Exception as e:
        return {"error": str(e)}


@mcp.tool()
async def select_page(page_id: str) -> dict:
    try:
        return await browser_manager.select_page(page_id)
    except Exception as e:
        return {"error": str(e)}


@mcp.tool()
async def close_page(page_id: str) -> dict:
    try:
        return await browser_manager.close_page(page_id)
    except Exception as e:
        return {"error": str(e)}


@mcp.tool()
async def navigate(
    url: str,
    page_id: str | None = None,
    capture_from_start: bool = False,
    capture_body: bool = False,
    capture_url_pattern: str = "**/*",
    wait_until: str = "load",
    timeout: int = 30000,
    pre_inject_hooks: list[str] | None = None,
    collect_response_chain: bool = True,
    clear_network_capture: bool = True,
    include_title: bool = True,
    diagnostic_variant: str = "current_launch",
    aida_operation_id=None,
) -> dict:
    """Navigate to a URL, with optional hook pre-injection and redirect tracing.

    Args:
        url: Target URL.
        wait_until: "load", "domcontentloaded", or "networkidle".
        pre_inject_hooks: Hook preset names to register before navigation.
        collect_response_chain: Record responses for final_status resolution.
        clear_network_capture: Clear stale network buffer before navigating.

    Returns:
        dict with url, title, initial_status, final_status, redirect_chain,
        hooks_injected, reloaded, warnings.
    """
    diagnostic_started = time.perf_counter()
    diagnostic_generation = browser_manager._next_diagnostic_id()
    page = None
    resolved_page_id = page_id
    operation_id: int | None = None
    operation_finished = False
    before_diagnostics: dict = {}
    phase_snapshots: list[dict] = []
    first_failure_phase = ""
    first_failure_reason = ""
    timeout_source = ""
    operation_failure_status = "exception"
    try:
        page = await browser_manager.resolve_page_for_operation(page_id, "navigate", True, aida_operation_id)
        resolved_page_id = browser_manager.page_id_for(page) or page_id
        operation_id = browser_manager.begin_page_operation(resolved_page_id, "navigate", url, diagnostic_generation, aida_operation_id)
        before_diagnostics = await browser_manager.navigation_diagnostic_snapshot(
            "navigate_before",
            page,
            resolved_page_id,
            url,
            diagnostic_generation,
            diagnostic_started,
            False,
        )
        browser_manager.log_navigation_diagnostic("diagnostic_navigation_before", before_diagnostics)
        warnings: list[str] = []
        hooks_injected: list[str] = []
        try:
            nav_timeout_ms = aida_clamp_navigation_timeout_ms(timeout)
        except Exception:
            nav_timeout_ms = AIDA_NAVIGATION_DEFAULT_TIMEOUT_MS

        if clear_network_capture:
            try:
                cleared_count = len(browser_manager._network_requests)
                if cleared_count > 0:
                    browser_manager._network_requests.clear()
                    browser_manager._request_id_counter = 0
                    warnings.append(f"cleared {cleared_count} stale network requests")
            except Exception:
                pass

        if capture_from_start:
            browser_manager._capturing = True
            browser_manager._capture_pattern = capture_url_pattern or "**/*"
            browser_manager._capture_body = capture_body
            warnings.append("capture_from_start_enabled")

        if collect_response_chain:
            browser_manager.reset_nav_responses(resolved_page_id)

        if pre_inject_hooks:
            for name in pre_inject_hooks:
                ok, msg = await _inject_hook_by_name(name)
                if ok:
                    hooks_injected.append(name)
                else:
                    warnings.append(f"hook '{name}' failed: {msg}")

        navigation_timed_out = False
        primary_wait_until = wait_until if wait_until in ("domcontentloaded", "networkidle", "commit", "load") else "load"
        _camoufox_debug(
            "navigate_wait_until_resolved",
            requested=str(wait_until or ""),
            primary=primary_wait_until,
            downgraded=primary_wait_until != (wait_until or ""),
            page_id=resolved_page_id or "",
            target_domain=_target_domain(url),
            nav_timeout_ms=nav_timeout_ms,
        )
        navigate_phase_observed = ""
        networkidle_elapsed_ms = -1
        fallback_attempted = False
        try:
            await _capture_navigation_phase(
                "diagnostic_navigation_goto_begin",
                "navigate_goto_begin",
                page,
                resolved_page_id,
                url,
                diagnostic_generation,
                diagnostic_started,
                phase_snapshots,
            )
            if _is_bloxflip_url(url):
                await _capture_navigation_phase(
                    "bloxflip_navigation_state",
                    "before_goto",
                    page,
                    resolved_page_id,
                    url,
                    diagnostic_generation,
                    diagnostic_started,
                    phase_snapshots,
                )
            goto_started_ts = time.perf_counter()
            navigate_phase_observed = f"goto_{primary_wait_until}"
            resp = await page.goto(url, wait_until=primary_wait_until, timeout=nav_timeout_ms)
            _camoufox_debug(
                "navigate_phase",
                phase=navigate_phase_observed,
                elapsed_ms=int((time.perf_counter() - goto_started_ts) * 1000),
                page_id=resolved_page_id or "",
                target_domain=_target_domain(url),
                wait_until_requested=wait_until,
                wait_until_effective=primary_wait_until,
            )
            if primary_wait_until != "networkidle":
                networkidle_budget_ms = max(500, min(nav_timeout_ms // 2, 8000))
                networkidle_started_ts = time.perf_counter()
                navigate_phase_observed = "wait_networkidle"
                try:
                    await page.wait_for_load_state("networkidle", timeout=networkidle_budget_ms)
                    networkidle_elapsed_ms = int((time.perf_counter() - networkidle_started_ts) * 1000)
                    _camoufox_debug(
                        "navigate_phase",
                        phase=navigate_phase_observed,
                        elapsed_ms=networkidle_elapsed_ms,
                        page_id=resolved_page_id or "",
                        target_domain=_target_domain(url),
                    )
                except Exception as networkidle_exc:
                    networkidle_elapsed_ms = int((time.perf_counter() - networkidle_started_ts) * 1000)
                    networkidle_err = _safe_text(networkidle_exc, 400)
                    warnings.append(f"networkidle wait skipped: {networkidle_err}")
                    _camoufox_debug(
                        "call_tool_navigate_timeout",
                        phase=navigate_phase_observed,
                        elapsed_ms=networkidle_elapsed_ms,
                        page_id=resolved_page_id or "",
                        target_domain=_target_domain(url),
                        error_type=type(networkidle_exc).__name__,
                        error_summary=networkidle_err,
                    )
        except Exception as e:
            goto_trace = _safe_text(_traceback.format_exc(), 2000)
            browser_manager.handle_page_operation_exception(resolved_page_id, page, "navigate_goto", e)
            browser_manager.log_navigation_diagnostic(
                "diagnostic_navigation_goto_exception",
                {
                    "action": "navigate_goto_exception",
                    "diagnostic_generation": diagnostic_generation,
                    "session_id": browser_manager.session_id,
                    "page_id": resolved_page_id or "",
                    "target_url": _safe_text(url, 500),
                    "target_domain": _target_domain(url),
                    "elapsed_ms": int((time.perf_counter() - diagnostic_started) * 1000),
                    "error_type": type(e).__name__,
                    "error_summary": _safe_text(e, 900),
                    "traceback": goto_trace,
                    "navigate_phase_observed": navigate_phase_observed,
                    "wait_until_requested": wait_until,
                    "wait_until_effective": primary_wait_until,
                    "networkidle_elapsed_ms": networkidle_elapsed_ms,
                },
            )
            _camoufox_debug(
                "call_tool_navigate_timeout",
                phase=navigate_phase_observed or "goto",
                elapsed_ms=int((time.perf_counter() - diagnostic_started) * 1000),
                page_id=resolved_page_id or "",
                target_domain=_target_domain(url),
                error_type=type(e).__name__,
                error_summary=_safe_text(e, 500),
                wait_until_requested=wait_until,
                wait_until_effective=primary_wait_until,
                networkidle_elapsed_ms=networkidle_elapsed_ms,
            )
            msg = str(e).lower()
            if "timeout" in msg or "exceeded" in msg or "waiting" in msg:
                timeout_source = "goto"
                child_alive = True
                browser_open = browser_manager.browser is not None
                page_verified = False
                try:
                    page_verified = bool(page) and not browser_manager._page_closed(page)
                except Exception:
                    page_verified = False
                if (not fallback_attempted) and child_alive and browser_open and page_verified and primary_wait_until != "domcontentloaded":
                    fallback_attempted = True
                    warnings.append(f"goto timeout for '{primary_wait_until}'; retrying once with domcontentloaded")
                    retry_started_ts = time.perf_counter()
                    try:
                        navigate_phase_observed = "goto_domcontentloaded_retry"
                        resp = await page.goto(url, wait_until="domcontentloaded", timeout=nav_timeout_ms)
                        _camoufox_debug(
                            "navigate_phase",
                            phase=navigate_phase_observed,
                            elapsed_ms=int((time.perf_counter() - retry_started_ts) * 1000),
                            page_id=resolved_page_id or "",
                            target_domain=_target_domain(url),
                        )
                    except Exception as retry_exc:
                        warnings.append(f"domcontentloaded retry failed: {_safe_text(retry_exc, 300)}")
                        raise
                else:
                    warnings.append(f"goto timeout for '{primary_wait_until}'; checking usability")
                    try:
                        dom_ready = await _await_no_cancel_wait(page.evaluate("document.readyState"), timeout=3.0)
                        current_url = page.url
                        if dom_ready in ("interactive", "complete") and current_url != "about:blank":
                            warnings.append(f"page usable (readyState={dom_ready})")
                            resp = None
                            navigation_timed_out = True
                        else:
                            raise
                    except Exception:
                        raise
            else:
                raise
        initial_status = resp.status if resp else None
        await _capture_navigation_phase(
            "diagnostic_navigation_goto_end",
            "navigate_goto_end",
            page,
            resolved_page_id,
            url,
            diagnostic_generation,
            diagnostic_started,
            phase_snapshots,
            "",
            True,
        )
        if _is_bloxflip_url(url):
            await _capture_navigation_phase(
                "bloxflip_navigation_state",
                "after_goto",
                page,
                resolved_page_id,
                url,
                diagnostic_generation,
                diagnostic_started,
                phase_snapshots,
                "",
                True,
            )
            for state_name in ("domcontentloaded", "load"):
                state_error = ""
                state_timeout_ms = max(250, min(5000, nav_timeout_ms))
                try:
                    await page.wait_for_load_state(state_name, timeout=state_timeout_ms)
                except Exception as state_exc:
                    state_error = _safe_text(state_exc, 700)
                    timeout_source = state_name if not timeout_source and ("timeout" in state_error.lower() or "exceeded" in state_error.lower()) else timeout_source
                    warnings.append(f"{state_name} state unavailable: {state_error}")
                await _capture_navigation_phase(
                    "bloxflip_navigation_state",
                    state_name,
                    page,
                    resolved_page_id,
                    url,
                    diagnostic_generation,
                    diagnostic_started,
                    phase_snapshots,
                    state_error,
                    True,
                )
            await asyncio.sleep(1.0)
            await _capture_navigation_phase(
                "bloxflip_navigation_state",
                "settle",
                page,
                resolved_page_id,
                url,
                diagnostic_generation,
                diagnostic_started,
                phase_snapshots,
                "",
                True,
            )

        reloaded = False
        if hooks_injected:
            try:
                if collect_response_chain:
                    browser_manager.reset_nav_responses(resolved_page_id)
                resp2 = await page.reload(wait_until=wait_until, timeout=nav_timeout_ms)
                reloaded = True
                if resp2:
                    initial_status = resp2.status
            except Exception as e:
                warnings.append(f"auto-reload failed: {e}")

        final_status = None
        chain = []
        if collect_response_chain:
            chain = browser_manager.nav_responses_for_page(resolved_page_id)
            for r in reversed(chain):
                if r["url"] == page.url or r.get("resource_type") == "document":
                    final_status = r["status"]
                    break

        title = ""
        title_error = None
        if include_title:
            try:
                title = await page.title()
            except Exception as e:
                title_error = str(e)
                warnings.append(f"title unavailable: {title_error}")

        out = {
            "url": page.url, "title": title,
            "diagnostic_variant": _safe_text(diagnostic_variant, 120),
            "initial_status": initial_status,
            "final_status": final_status if final_status is not None else initial_status,
            "redirect_chain": chain if collect_response_chain else None,
            "hooks_injected": hooks_injected, "reloaded": reloaded,
            "navigation_timed_out": navigation_timed_out,
            "timeout_ms": nav_timeout_ms,
            "warnings": warnings if warnings else None,
        }
        if title_error:
            out["title_error"] = title_error
        capture_summary = _navigation_capture_summary(resolved_page_id)
        if capture_summary.get("returned_count", 0) > 0:
            out["network_requests"] = capture_summary["requests"]
            out["network_capture"] = capture_summary
        after_diagnostics = await browser_manager.navigation_diagnostic_snapshot(
            "navigate_after",
            page,
            resolved_page_id,
            url,
            diagnostic_generation,
            diagnostic_started,
            False,
        )
        browser_manager.log_navigation_diagnostic("diagnostic_navigation_after", after_diagnostics)
        final_failure_reason = _navigation_snapshot_failure_reason(after_diagnostics)
        if final_failure_reason:
            raise _NavigationLifecycleError("navigate_after", final_failure_reason, after_diagnostics)
        out["diagnostics"] = {
            "generation": diagnostic_generation,
            "elapsed_ms": int((time.perf_counter() - diagnostic_started) * 1000),
            "before": before_diagnostics,
            "after": after_diagnostics,
            "phases": phase_snapshots,
            "first_failure_phase": "",
            "first_failure_reason": "",
            "timeout_source": timeout_source,
            "last_error": "",
        }
        out["evidence"] = _navigation_evidence_bundle(diagnostic_variant, before_diagnostics, after_diagnostics, phase_snapshots, chain)
        browser_manager.finish_page_operation(
            resolved_page_id,
            operation_id,
            "timeout_usable" if navigation_timed_out else "success",
        )
        operation_finished = True
        out.update(await browser_manager.page_envelope(page))
        return out

    except Exception as e:
        last_error = _safe_text(e, 900)
        if isinstance(e, _NavigationLifecycleError):
            first_failure_phase = e.phase
            first_failure_reason = e.reason
            operation_failure_status = f"failed_{first_failure_phase}"[:120]
        else:
            operation_failure_status = "timeout" if "timeout" in last_error.lower() or "exceeded" in last_error.lower() else "exception"
            if not timeout_source and operation_failure_status == "timeout":
                timeout_source = "navigate"
        browser_manager._last_error = last_error
        try:
            browser_manager.handle_page_operation_exception(resolved_page_id, page, "navigate", e)
        except Exception:
            pass
        after_diagnostics: dict = {}
        try:
            after_diagnostics = await browser_manager.navigation_diagnostic_snapshot(
                "navigate_failure",
                page,
                resolved_page_id,
                url,
                diagnostic_generation,
                diagnostic_started,
                True,
                last_error,
            )
            browser_manager.log_navigation_diagnostic("diagnostic_navigation_failure", after_diagnostics)
        except Exception:
            pass
        return {
            "status": "error",
            "error": str(e),
            "error_code": "navigation_lifecycle_degraded" if isinstance(e, _NavigationLifecycleError) else "navigation_failed",
            "diagnostic_variant": _safe_text(diagnostic_variant, 120),
            "failure_phase": first_failure_phase or "exception",
            "failure_reason": first_failure_reason or last_error,
            "timeout_source": timeout_source,
            "evidence": _navigation_evidence_bundle(diagnostic_variant, before_diagnostics, after_diagnostics, phase_snapshots, response_chain if "response_chain" in locals() else []),
            "diagnostics": {
                "generation": diagnostic_generation,
                "elapsed_ms": int((time.perf_counter() - diagnostic_started) * 1000),
                "before": before_diagnostics,
                "after": after_diagnostics,
                "phases": phase_snapshots,
                "first_failure_phase": first_failure_phase or "exception",
                "first_failure_reason": first_failure_reason or last_error,
                "timeout_source": timeout_source,
                "last_error": last_error,
            },
        }
    finally:
        if operation_id is not None and not operation_finished:
            with contextlib.suppress(Exception):
                browser_manager.finish_page_operation(resolved_page_id, operation_id, operation_failure_status)


@mcp.tool()
async def diagnose_navigation(
    url: str = "https://bloxflip.com",
    page_id: str | None = None,
    wait_until: str = "domcontentloaded",
    timeout: int = 30000,
    include_screenshot_metadata: bool = True,
    diagnostic_label: str = "bloxflip",
    isolated_context: bool = True,
    cleanup_diagnostic_page: bool = True,
    diagnostic_variant: str = "current_launch",
) -> dict:
    diagnostic_started = time.perf_counter()
    diagnostic_generation = browser_manager._next_diagnostic_id()
    page = None
    resolved_page_id = page_id
    before_diagnostics: dict = {}
    after_diagnostics: dict = {}
    last_error = ""
    navigation_completed = False
    navigation_timeout_observed = False
    initial_status = None
    final_status = None
    title = ""
    response_chain: list[dict] = []
    phase_snapshots: list[dict] = []
    phase_timings: list[dict] = []
    first_failure_phase = ""
    first_failure_reason = ""
    timeout_source = ""
    operation_failure_status = "exception"
    operation_id: int | None = None
    operation_finished = False
    diagnostic_page_owned = False
    diagnostic_context_id = ""
    diagnostic_isolated = False
    diagnostic_mode = ""
    original_active_page_id = browser_manager.active_page_id
    cleanup_result: dict = {"attempted": False}
    result: dict | None = None
    def record_phase_timing(phase: str, started: float, status: str = "ok", error: str = "") -> None:
        phase_timings.append({
            "phase": phase,
            "status": status,
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
            "error": _safe_text(error, 700),
        })
    try:
        if isolated_context and page_id is None and _is_bloxflip_url(url):
            diagnostic_page_id = f"aida_diag_{diagnostic_generation}"
            diagnostic_context_name = f"aida_diag_ctx_{diagnostic_generation}"
            created = await browser_manager.new_diagnostic_page(diagnostic_page_id, diagnostic_context_name)
            if isinstance(created, dict) and created.get("error"):
                raise RuntimeError(str(created.get("error") or "diagnostic page creation failed"))
            page = created.get("page") if isinstance(created, dict) else None
            if page is None:
                raise RuntimeError("diagnostic page creation returned no page")
            resolved_page_id = str(created.get("page_id") or browser_manager.page_id_for(page) or diagnostic_page_id)
            diagnostic_context_id = str(created.get("context_id") or "")
            diagnostic_isolated = bool(created.get("isolated_context"))
            diagnostic_mode = str(created.get("mode") or "")
            diagnostic_page_owned = True
        else:
            page = await browser_manager.resolve_page_for_navigation(page_id, "diagnose_navigation")
        resolved_page_id = browser_manager.page_id_for(page) or page_id
        operation_id = browser_manager.begin_page_operation(resolved_page_id, "diagnose_navigation", url, diagnostic_generation)
        browser_manager.reset_nav_responses(resolved_page_id)
        before_diagnostics = await browser_manager.navigation_diagnostic_snapshot(
            "diagnose_navigation_before",
            page,
            resolved_page_id,
            url,
            diagnostic_generation,
            diagnostic_started,
            False,
        )
        browser_manager.log_navigation_diagnostic("diagnostic_navigation_before", before_diagnostics)
        goto_started = time.perf_counter()
        try:
            if _is_bloxflip_url(url):
                await _capture_navigation_phase(
                    "bloxflip_navigation_state",
                    "diagnose_before_goto",
                    page,
                    resolved_page_id,
                    url,
                    diagnostic_generation,
                    diagnostic_started,
                    phase_snapshots,
                )
            resp = await page.goto(url, wait_until=wait_until, timeout=max(1000, int(timeout)))
            navigation_completed = True
            initial_status = resp.status if resp else None
            record_phase_timing("goto", goto_started, "completed")
        except Exception as exc:
            last_error = _safe_text(exc, 900)
            record_phase_timing("goto", goto_started, "error", last_error)
            browser_manager._last_error = last_error
            browser_manager.handle_page_operation_exception(resolved_page_id, page, "diagnose_navigation_goto", exc)
            browser_manager.log_navigation_diagnostic(
                "diagnostic_navigation_goto_exception",
                {
                    "action": "diagnose_navigation_goto_exception",
                    "diagnostic_generation": diagnostic_generation,
                    "session_id": browser_manager.session_id,
                    "page_id": resolved_page_id or "",
                    "target_url": _safe_text(url, 500),
                    "target_domain": _target_domain(url),
                    "elapsed_ms": int((time.perf_counter() - diagnostic_started) * 1000),
                    "error_type": type(exc).__name__,
                    "error_summary": last_error,
                    "traceback": _safe_text(_traceback.format_exc(), 2000),
                },
            )
            msg = str(exc).lower()
            navigation_timeout_observed = "timeout" in msg or "exceeded" in msg or "waiting" in msg
            if navigation_timeout_observed:
                timeout_source = "diagnose_goto"
        if _is_bloxflip_url(url):
            await _capture_navigation_phase(
                "bloxflip_navigation_state",
                "diagnose_after_goto",
                page,
                resolved_page_id,
                url,
                diagnostic_generation,
                diagnostic_started,
                phase_snapshots,
                last_error,
                True,
            )
            usable_document = navigation_completed and initial_status is not None and 200 <= int(initial_status) < 400
            diagnostic_deadline = diagnostic_started + max(1.0, int(timeout) / 1000.0)
            for state_name in ("domcontentloaded", "load"):
                phase_started = time.perf_counter()
                state_error = ""
                state_status = "completed"
                remaining_ms = int((diagnostic_deadline - time.perf_counter()) * 1000)
                if navigation_completed and state_name == wait_until:
                    state_status = "already_satisfied"
                elif usable_document and remaining_ms <= 750:
                    state_error = "skipped_timeout_budget_after_usable_document"
                    state_status = "skipped"
                    if not timeout_source:
                        timeout_source = f"diagnose_{state_name}_budget"
                else:
                    state_timeout = max(250, min(3000, remaining_ms - 500 if remaining_ms > 500 else 250))
                    try:
                        await page.wait_for_load_state(state_name, timeout=state_timeout)
                    except Exception as state_exc:
                        state_error = _safe_text(state_exc, 700)
                        state_status = "timeout" if "timeout" in state_error.lower() or "exceeded" in state_error.lower() else "error"
                        if not timeout_source and state_status == "timeout":
                            timeout_source = f"diagnose_{state_name}"
                record_phase_timing(f"wait_{state_name}", phase_started, state_status, state_error)
                await _capture_navigation_phase(
                    "bloxflip_navigation_state",
                    f"diagnose_{state_name}",
                    page,
                    resolved_page_id,
                    url,
                    diagnostic_generation,
                    diagnostic_started,
                    phase_snapshots,
                    state_error,
                    True,
                )
            settle_started = time.perf_counter()
            settle_remaining_ms = int((diagnostic_deadline - time.perf_counter()) * 1000)
            if usable_document and settle_remaining_ms <= 500:
                record_phase_timing("settle", settle_started, "skipped", "skipped_timeout_budget_after_usable_document")
            else:
                settle_sleep = 1.0 if settle_remaining_ms > 1500 else max(0.05, settle_remaining_ms / 1000.0)
                await asyncio.sleep(settle_sleep)
                record_phase_timing("settle", settle_started, "completed")
            await _capture_navigation_phase(
                "bloxflip_navigation_state",
                "diagnose_settle",
                page,
                resolved_page_id,
                url,
                diagnostic_generation,
                diagnostic_started,
                phase_snapshots,
                last_error,
                True,
            )
        response_chain = browser_manager.nav_responses_for_page(resolved_page_id)
        current_url_for_chain = ""
        try:
            current_url_for_chain = str(page.url or "")
        except Exception:
            current_url_for_chain = ""
        for response in reversed(response_chain):
            if response.get("resource_type") == "document" or response.get("url") == current_url_for_chain:
                final_status = response.get("status")
                break
        if final_status is None:
            final_status = initial_status
        try:
            if not browser_manager._page_closed(page):
                title = await asyncio.wait_for(page.title(), timeout=3)
        except Exception as exc:
            if not last_error:
                last_error = _safe_text(exc, 700)
        after_diagnostics = await browser_manager.navigation_diagnostic_snapshot(
            "diagnose_navigation_after",
            page,
            resolved_page_id,
            url,
            diagnostic_generation,
            diagnostic_started,
            include_screenshot_metadata,
            last_error,
        )
        browser_manager.log_navigation_diagnostic("diagnostic_navigation_after", after_diagnostics)
        final_failure_reason = _navigation_snapshot_failure_reason(after_diagnostics)
        if final_failure_reason:
            raise _NavigationLifecycleError("diagnose_after", final_failure_reason, after_diagnostics)
        operation_status = "success" if navigation_completed else ("timeout" if navigation_timeout_observed else "goto_error")
        browser_manager.finish_page_operation(resolved_page_id, operation_id, operation_status)
        operation_finished = True
        result = {
            "status": "diagnostic_complete",
            "diagnostic_label": _safe_text(diagnostic_label, 120),
            "diagnostic_variant": _safe_text(diagnostic_variant, 120),
            "diagnostic_generation": diagnostic_generation,
            "target_url": _safe_text(url, 500),
            "target_domain": _target_domain(url),
            "diagnostic_page_id": resolved_page_id or "",
            "diagnostic_context_id": diagnostic_context_id,
            "diagnostic_isolated_context": diagnostic_isolated,
            "diagnostic_mode": diagnostic_mode,
            "original_active_page_id": original_active_page_id or "",
            "navigation_completed": navigation_completed,
            "navigation_timeout_observed": navigation_timeout_observed,
            "initial_status": initial_status,
            "final_status": final_status,
            "final_url": after_diagnostics.get("current_url", ""),
            "final_domain": after_diagnostics.get("current_domain", ""),
            "title": _safe_text(title, 500),
            "last_error": last_error,
            "failure_phase": "",
            "failure_reason": "",
            "timeout_source": timeout_source,
            "elapsed_ms": int((time.perf_counter() - diagnostic_started) * 1000),
            "phase_timings": phase_timings,
            "response_chain": response_chain,
            "evidence": _navigation_evidence_bundle(diagnostic_variant, before_diagnostics, after_diagnostics, phase_snapshots, response_chain),
            "diagnostics": {
                "before": before_diagnostics,
                "after": after_diagnostics,
                "phases": phase_snapshots,
                "phase_timings": phase_timings,
                "first_failure_phase": "",
                "first_failure_reason": "",
                "timeout_source": timeout_source,
                "recent_page_events": after_diagnostics.get("recent_page_events", []),
            },
        }
    except Exception as exc:
        last_error = _safe_text(exc, 900)
        if isinstance(exc, _NavigationLifecycleError):
            first_failure_phase = exc.phase
            first_failure_reason = exc.reason
            operation_failure_status = f"failed_{first_failure_phase}"[:120]
        else:
            first_failure_phase = "exception"
            first_failure_reason = last_error
            operation_failure_status = "timeout" if "timeout" in last_error.lower() or "exceeded" in last_error.lower() else "exception"
            if not timeout_source and operation_failure_status == "timeout":
                timeout_source = "diagnose_navigation"
        browser_manager._last_error = last_error
        with contextlib.suppress(Exception):
            browser_manager.handle_page_operation_exception(resolved_page_id, page, "diagnose_navigation", exc)
        try:
            after_diagnostics = await browser_manager.navigation_diagnostic_snapshot(
                "diagnose_navigation_unavailable",
                page,
                resolved_page_id,
                url,
                diagnostic_generation,
                diagnostic_started,
                include_screenshot_metadata,
                last_error,
            )
            browser_manager.log_navigation_diagnostic("diagnostic_navigation_unavailable", after_diagnostics)
        except Exception:
            pass
        result = {
            "status": "error",
            "error": last_error,
            "error_code": "navigation_lifecycle_degraded" if isinstance(exc, _NavigationLifecycleError) else "diagnose_navigation_failed",
            "diagnostic_label": _safe_text(diagnostic_label, 120),
            "diagnostic_variant": _safe_text(diagnostic_variant, 120),
            "diagnostic_generation": diagnostic_generation,
            "target_url": _safe_text(url, 500),
            "target_domain": _target_domain(url),
            "diagnostic_page_id": resolved_page_id or "",
            "diagnostic_context_id": diagnostic_context_id,
            "diagnostic_isolated_context": diagnostic_isolated,
            "diagnostic_mode": diagnostic_mode,
            "original_active_page_id": original_active_page_id or "",
            "navigation_completed": navigation_completed,
            "navigation_timeout_observed": navigation_timeout_observed,
            "initial_status": initial_status,
            "final_status": final_status,
            "final_url": after_diagnostics.get("current_url", ""),
            "final_domain": after_diagnostics.get("current_domain", ""),
            "title": "",
            "last_error": last_error,
            "failure_phase": first_failure_phase,
            "failure_reason": first_failure_reason,
            "timeout_source": timeout_source,
            "elapsed_ms": int((time.perf_counter() - diagnostic_started) * 1000),
            "phase_timings": phase_timings,
            "response_chain": response_chain,
            "evidence": _navigation_evidence_bundle(diagnostic_variant, before_diagnostics, after_diagnostics, phase_snapshots, response_chain),
            "diagnostics": {
                "before": before_diagnostics,
                "after": after_diagnostics,
                "phases": phase_snapshots,
                "phase_timings": phase_timings,
                "first_failure_phase": first_failure_phase,
                "first_failure_reason": first_failure_reason,
                "timeout_source": timeout_source,
                "recent_page_events": after_diagnostics.get("recent_page_events", []),
            },
        }
    finally:
        if operation_id is not None and not operation_finished:
            with contextlib.suppress(Exception):
                browser_manager.finish_page_operation(resolved_page_id, operation_id, operation_failure_status)
        if diagnostic_page_owned and cleanup_diagnostic_page:
            cleanup_result = {
                "attempted": True,
                "diagnostic_page_id": resolved_page_id or "",
                "diagnostic_context_id": diagnostic_context_id,
                "isolated_context": diagnostic_isolated,
                "mode": diagnostic_mode,
            }
            cleanup_started = time.perf_counter()
            try:
                cleanup_before = await browser_manager.navigation_diagnostic_snapshot(
                    "diagnose_navigation_cleanup_before",
                    page,
                    resolved_page_id,
                    url,
                    diagnostic_generation,
                    diagnostic_started,
                    False,
                    last_error,
                )
                cleanup_result["before"] = cleanup_before
                if diagnostic_isolated and diagnostic_context_id:
                    ctx = browser_manager.contexts.get(diagnostic_context_id)
                    if ctx is not None:
                        await ctx.close()
                        cleanup_result["context_closed"] = True
                    else:
                        cleanup_result["context_missing"] = True
                elif resolved_page_id:
                    close_result = await browser_manager.close_page(str(resolved_page_id))
                    cleanup_result["page_closed"] = True
                    cleanup_result["close_status"] = close_result.get("status") if isinstance(close_result, dict) else ""
                cleanup_after = await browser_manager.navigation_diagnostic_snapshot(
                    "diagnose_navigation_cleanup_after",
                    page,
                    resolved_page_id,
                    url,
                    diagnostic_generation,
                    diagnostic_started,
                    False,
                    last_error,
                )
                cleanup_result["after"] = cleanup_after
            except Exception as cleanup_exc:
                cleanup_result["error_type"] = type(cleanup_exc).__name__
                cleanup_result["error"] = _safe_text(cleanup_exc, 700)
                if not first_failure_phase:
                    first_failure_phase = "cleanup"
                    first_failure_reason = cleanup_result["error"]
                    if result is not None:
                        result["status"] = "error"
                        result["error"] = first_failure_reason
                        result["error_code"] = "diagnose_navigation_cleanup_failed"
                        result["failure_phase"] = first_failure_phase
                        result["failure_reason"] = first_failure_reason
            if original_active_page_id and original_active_page_id in browser_manager.pages:
                with contextlib.suppress(Exception):
                    await browser_manager.select_page(original_active_page_id)
                    cleanup_result["restored_active_page_id"] = original_active_page_id
            cleanup_result["elapsed_ms"] = int((time.perf_counter() - cleanup_started) * 1000)
            _camoufox_debug(
                "diagnose_navigation_cleanup",
                diagnostic_generation=diagnostic_generation,
                target_url=_safe_text(url, 500),
                target_domain=_target_domain(url),
                **cleanup_result,
            )
        if result is not None:
            result["diagnostic_cleanup"] = cleanup_result
    return result or {
        "status": "error",
        "error": last_error or "diagnose_navigation did not produce a result",
        "error_code": "diagnose_navigation_no_result",
        "diagnostic_label": _safe_text(diagnostic_label, 120),
        "diagnostic_variant": _safe_text(diagnostic_variant, 120),
        "diagnostic_generation": diagnostic_generation,
        "target_url": _safe_text(url, 500),
        "target_domain": _target_domain(url),
        "navigation_completed": False,
        "navigation_timeout_observed": False,
        "initial_status": None,
        "final_status": None,
        "final_url": "",
        "final_domain": "",
        "title": "",
        "last_error": last_error,
        "failure_phase": first_failure_phase or "no_result",
        "failure_reason": first_failure_reason or last_error or "no_result",
        "timeout_source": timeout_source,
        "elapsed_ms": int((time.perf_counter() - diagnostic_started) * 1000),
        "phase_timings": phase_timings,
        "response_chain": response_chain,
        "evidence": _navigation_evidence_bundle(diagnostic_variant, before_diagnostics, after_diagnostics, phase_snapshots, response_chain),
        "diagnostics": {"before": before_diagnostics, "after": after_diagnostics, "phases": phase_snapshots, "phase_timings": phase_timings, "first_failure_phase": first_failure_phase or "no_result", "first_failure_reason": first_failure_reason or last_error or "no_result", "timeout_source": timeout_source, "recent_page_events": []},
        "diagnostic_cleanup": cleanup_result,
    }


def _matrix_profile_root() -> str:
    root = os.environ.get("AIDA_CAMOUFOX_PROFILE_ROOT", "")
    if root:
        return os.path.abspath(os.path.expandvars(os.path.expanduser(root)))
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("TEMP") or os.getcwd()
    return os.path.abspath(os.path.join(base, "AiDA", "camoufox-profiles"))


def _matrix_base_launch_config() -> dict:
    cfg = dict(getattr(browser_manager, "_last_successful_launch_config", {}) or {})
    cfg.pop("_aida_launch_retry", None)
    cfg["headless"] = False
    cfg["block_webrtc"] = True
    cfg["privacy_fail_closed"] = True
    executable = os.environ.get("AIDA_CAMOUFOX_EXECUTABLE", "")
    if executable and not cfg.get("executable_path"):
        cfg["executable_path"] = executable
        cfg.setdefault("ff_version", 135)
    cfg["launch_timeout_ms"] = int(aida_resolve_launch_budget_policy(
        cfg.get("launch_timeout_ms"),
        bundled_visible_launch=bool(cfg.get("executable_path") or executable),
        fast_probe=bool(cfg.get("aida_testlab_fast_probe") or cfg.get("testlab_fast_probe")),
    )["launch_timeout_ms"])
    return cfg


async def _matrix_snapshot(label: str, url: str, started: float) -> dict:
    try:
        page = None
        page_id = getattr(browser_manager, "active_page_id", None)
        if page_id:
            page = browser_manager.pages.get(page_id)
        return await browser_manager.navigation_diagnostic_snapshot(
            label,
            page,
            page_id,
            url,
            browser_manager._next_diagnostic_id(),
            started,
            False,
        )
    except Exception as exc:
        return {"action": label, "error_type": type(exc).__name__, "error": _safe_text(exc, 700)}


async def _run_bloxflip_matrix_variant(
    name: str,
    url: str,
    wait_until: str,
    timeout: int,
    include_screenshot_metadata: bool,
    launch_config: dict,
    reuse_current: bool,
) -> dict:
    started = time.perf_counter()
    launch_result: dict = {}
    phase_timings: list[dict] = []
    before = await _matrix_snapshot(f"{name}_before", url, started)
    try:
        launch_started = time.perf_counter()
        if not reuse_current:
            with contextlib.suppress(Exception):
                await browser_manager.close()
            launch_result = await browser_manager.launch(launch_config)
        elif browser_manager.browser is None:
            launch_result = await browser_manager.launch(launch_config)
        else:
            launch_result = {"status": "already_running", "launch": getattr(browser_manager, "_last_launch_summary", {})}
        phase_timings.append({"phase": "launch", "status": str(launch_result.get("status") or "ok"), "elapsed_ms": int((time.perf_counter() - launch_started) * 1000), "error": _safe_text(launch_result.get("error") or "", 700)})
        browser_manager._capturing = False
        browser_manager._capture_body = False
        nav_started = time.perf_counter()
        nav = await diagnose_navigation(
            url=url,
            wait_until=wait_until,
            timeout=timeout,
            include_screenshot_metadata=include_screenshot_metadata,
            diagnostic_label=f"bloxflip_matrix_{name}",
            isolated_context=True,
            cleanup_diagnostic_page=True,
            diagnostic_variant=name,
        )
        phase_timings.append({"phase": "diagnose_navigation", "status": str(nav.get("status") if isinstance(nav, dict) else "unknown"), "elapsed_ms": int((time.perf_counter() - nav_started) * 1000), "error": _safe_text(nav.get("error") if isinstance(nav, dict) else "", 700)})
        after = await _matrix_snapshot(f"{name}_after", url, started)
        evidence = nav.get("evidence") if isinstance(nav, dict) and isinstance(nav.get("evidence"), dict) else _navigation_evidence_bundle(name, before, after, [], [])
        return {
            "variant": name,
            "status": nav.get("status", "unknown") if isinstance(nav, dict) else "unknown",
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
            "launch": launch_result,
            "navigation": nav,
            "process_before": before.get("process_tree_detail") or before.get("process_tree") if isinstance(before, dict) else {},
            "process_after": after.get("process_tree_detail") or after.get("process_tree") if isinstance(after, dict) else {},
            "evidence": evidence,
            "phase_timings": phase_timings,
            "cpp_network_capture_wrapper": False,
            "response_body_collection": False,
        }
    except Exception as exc:
        after = await _matrix_snapshot(f"{name}_exception", url, started)
        return {
            "variant": name,
            "status": "error",
            "error": _safe_text(exc, 900),
            "error_type": type(exc).__name__,
            "elapsed_ms": int((time.perf_counter() - started) * 1000),
            "launch": launch_result,
            "process_before": before.get("process_tree_detail") or before.get("process_tree") if isinstance(before, dict) else {},
            "process_after": after.get("process_tree_detail") or after.get("process_tree") if isinstance(after, dict) else {},
            "evidence": _navigation_evidence_bundle(name, before, after, [], []),
            "phase_timings": phase_timings + [{"phase": "exception", "status": "error", "elapsed_ms": int((time.perf_counter() - started) * 1000), "error": _safe_text(exc, 700)}],
            "cpp_network_capture_wrapper": False,
            "response_body_collection": False,
        }


@mcp.tool()
async def diagnose_bloxflip_matrix(
    url: str = "https://bloxflip.com",
    wait_until: str = "domcontentloaded",
    timeout: int = 30000,
    include_screenshot_metadata: bool = True,
    restore_browser: bool = True,
    matrix_timeout_ms: int = 28000,
) -> dict:
    matrix_started = time.perf_counter()
    if not _is_bloxflip_url(url):
        return {
            "status": "error",
            "error": "diagnose_bloxflip_matrix only accepts bloxflip.com targets",
            "error_code": "bloxflip_matrix_target_required",
            "target_url": _safe_text(url, 500),
            "target_domain": _target_domain(url),
        }
    original_running = browser_manager.browser is not None
    original_active_page_id = getattr(browser_manager, "active_page_id", None)
    original_config = dict(getattr(browser_manager, "_last_successful_launch_config", {}) or {})
    base = _matrix_base_launch_config()
    profile_root = _matrix_profile_root()
    os.makedirs(profile_root, exist_ok=True)
    variants: list[dict] = []
    variants.append({"name": "current_launch", "reuse_current": True, "config": dict(base)})
    persistent_cfg = dict(base)
    persistent_cfg.update({
        "persistent_context": True,
        "profile_dir": os.path.join(profile_root, "bloxflip-diagnostic-persistent"),
        "aida_diagnostic_variant": "persistent_app_local_profile",
    })
    variants.append({"name": "persistent_app_local_profile", "reuse_current": False, "config": persistent_cfg})
    minimal_cfg = dict(base)
    minimal_cfg["aida_diagnostic_variant"] = "minimal_prefs"
    variants.append({"name": "minimal_prefs", "reuse_current": False, "config": minimal_cfg})
    no_cpp_cfg = dict(base)
    no_cpp_cfg["aida_diagnostic_variant"] = "no_cpp_network_capture_wrapper"
    variants.append({"name": "no_cpp_network_capture_wrapper", "reuse_current": False, "config": no_cpp_cfg})
    no_body_cfg = dict(base)
    no_body_cfg["aida_diagnostic_variant"] = "no_response_body_collection"
    variants.append({"name": "no_response_body_collection", "reuse_current": False, "config": no_body_cfg})
    original_style_cfg = dict(base)
    original_style_cfg["aida_diagnostic_variant"] = "original_style_bundled"
    original_style_cfg["aida_fast_visible_launch"] = False
    variants.append({"name": "original_style_bundled", "reuse_current": False, "config": original_style_cfg})
    results = []
    phase_timings: list[dict] = []
    budget_ms = max(8000, min(max(1000, int(matrix_timeout_ms)), 28000))
    budget_exhausted = False
    restore_result: dict = {"attempted": False}
    try:
        for variant in variants:
            variant_started = time.perf_counter()
            elapsed_ms = int((time.perf_counter() - matrix_started) * 1000)
            remaining_ms = budget_ms - elapsed_ms
            if remaining_ms < 3500:
                budget_exhausted = True
                skipped = {
                    "variant": variant["name"],
                    "status": "skipped",
                    "reason": "matrix_budget_exhausted",
                    "elapsed_ms": 0,
                    "remaining_budget_ms": max(0, remaining_ms),
                    "cpp_network_capture_wrapper": False,
                    "response_body_collection": False,
                }
                results.append(skipped)
                phase_timings.append({"phase": f"variant:{variant['name']}", "status": "skipped_budget", "elapsed_ms": int((time.perf_counter() - variant_started) * 1000), "error": "matrix_budget_exhausted"})
                continue
            executable_missing = variant["name"] == "original_style_bundled" and not (variant["config"].get("executable_path") or os.environ.get("AIDA_CAMOUFOX_EXECUTABLE"))
            if executable_missing:
                results.append({
                    "variant": variant["name"],
                    "status": "skipped",
                    "reason": "bundled Camoufox executable unavailable",
                    "cpp_network_capture_wrapper": False,
                    "response_body_collection": False,
                })
                phase_timings.append({"phase": f"variant:{variant['name']}", "status": "skipped", "elapsed_ms": int((time.perf_counter() - variant_started) * 1000), "error": "bundled Camoufox executable unavailable"})
                continue
            per_variant_timeout = max(2500, min(int(timeout), remaining_ms - 2000, 6000))
            variant_config = dict(variant["config"])
            variant_config["launch_timeout_ms"] = int(aida_resolve_launch_budget_policy(
                min(int(variant_config.get("launch_timeout_ms", AIDA_LAUNCH_DEFAULT_BUNDLED_VISIBLE_MS)), max(AIDA_LAUNCH_FLOOR_MS, per_variant_timeout)),
                bundled_visible_launch=bool(variant_config.get("executable_path") or os.environ.get("AIDA_CAMOUFOX_EXECUTABLE")) and not bool(variant_config.get("headless")),
                fast_probe=bool(variant_config.get("aida_testlab_fast_probe") or variant_config.get("testlab_fast_probe")),
            )["launch_timeout_ms"])
            results.append(await _run_bloxflip_matrix_variant(
                variant["name"],
                url,
                wait_until,
                per_variant_timeout,
                include_screenshot_metadata,
                variant_config,
                bool(variant["reuse_current"]),
            ))
            phase_timings.append({"phase": f"variant:{variant['name']}", "status": str(results[-1].get("status") if isinstance(results[-1], dict) else "unknown"), "elapsed_ms": int((time.perf_counter() - variant_started) * 1000), "error": _safe_text(results[-1].get("error") if isinstance(results[-1], dict) else "", 700)})
    finally:
        if restore_browser:
            restore_started = time.perf_counter()
            remaining_ms = budget_ms - int((time.perf_counter() - matrix_started) * 1000)
            if remaining_ms < 1500:
                restore_result = {"attempted": False, "skipped_due_to_budget": True, "was_running": original_running, "original_active_page_id": original_active_page_id or "", "remaining_budget_ms": max(0, remaining_ms)}
                phase_timings.append({"phase": "restore", "status": "skipped_budget", "elapsed_ms": int((time.perf_counter() - restore_started) * 1000), "error": "matrix_budget_exhausted"})
            else:
                restore_result = {"attempted": True, "was_running": original_running, "original_active_page_id": original_active_page_id or ""}
                with contextlib.suppress(Exception):
                    await browser_manager.close()
                if original_running:
                    try:
                        restore_cfg = dict(original_config or base)
                        restore_cfg.pop("aida_diagnostic_variant", None)
                        restore_cfg["launch_timeout_ms"] = int(aida_resolve_launch_budget_policy(
                            min(int(restore_cfg.get("launch_timeout_ms", AIDA_LAUNCH_DEFAULT_BUNDLED_VISIBLE_MS)), max(AIDA_LAUNCH_FLOOR_MS, remaining_ms - 500)),
                            bundled_visible_launch=bool(restore_cfg.get("executable_path") or os.environ.get("AIDA_CAMOUFOX_EXECUTABLE")) and not bool(restore_cfg.get("headless")),
                            fast_probe=bool(restore_cfg.get("aida_testlab_fast_probe") or restore_cfg.get("testlab_fast_probe")),
                        )["launch_timeout_ms"])
                        restore_result["launch"] = await browser_manager.launch(restore_cfg)
                        if original_active_page_id and original_active_page_id in browser_manager.pages:
                            with contextlib.suppress(Exception):
                                await browser_manager.select_page(original_active_page_id)
                                restore_result["restored_active_page_id"] = original_active_page_id
                    except Exception as restore_exc:
                        restore_result["error_type"] = type(restore_exc).__name__
                        restore_result["error"] = _safe_text(restore_exc, 900)
                phase_timings.append({"phase": "restore", "status": "completed" if not restore_result.get("error") else "error", "elapsed_ms": int((time.perf_counter() - restore_started) * 1000), "error": _safe_text(restore_result.get("error") or "", 700)})
    succeeded = [item for item in results if isinstance(item, dict) and str(item.get("status") or "").lower() in {"diagnostic_complete", "success", "ok"}]
    degraded = [item for item in results if isinstance(item, dict) and str(item.get("status") or "").lower() == "error"]
    return {
        "status": "matrix_complete",
        "diagnostic_only": True,
        "target_url": _safe_text(url, 500),
        "target_domain": _target_domain(url),
        "elapsed_ms": int((time.perf_counter() - matrix_started) * 1000),
        "budget_ms": budget_ms,
        "budget_exhausted": budget_exhausted,
        "variant_count": len(results),
        "success_count": len(succeeded),
        "degraded_count": len(degraded),
        "variants": results,
        "phase_timings": phase_timings,
        "restore": restore_result,
    }


async def _inject_hook_by_name(name: str) -> tuple[bool, str]:
    """Register a hook as a persistent context-level script."""
    hooks_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "hooks")
    preset_files = {
        "xhr": "xhr_hook.js", "fetch": "fetch_hook.js",
        "crypto": "crypto_hook.js", "websocket": "websocket_hook.js",
        "debugger_bypass": "debugger_trap.js",
        "cookie_hook": "cookie_hook.js", "runtime_probe": "runtime_probe.js",
    }
    try:
        if name == "jsvmp_probe":
            with open(os.path.join(hooks_dir, "jsvmp_hook.js"), "r", encoding="utf-8") as f:
                tpl = f.read()
            default_proxy = ["navigator", "screen", "history", "localStorage",
                             "sessionStorage", "performance"]
            js = (tpl.replace("{{SCRIPT_URL}}", "").replace("{{MAX_ENTRIES}}", "10000")
                .replace("{{TRACK_CALLS}}", "true").replace("{{TRACK_PROPS}}", "true")
                .replace("{{TRACK_REFLECT}}", "true")
                .replace("'{{PROXY_OBJECTS}}'", _json.dumps(_json.dumps(default_proxy))))
            persist_name = "pre_inject:jsvmp_probe"
        elif name == "jsvmp_probe_transparent":
            hook_path = os.path.join(hooks_dir, "jsvmp_transparent_hook.js")
            if not os.path.exists(hook_path):
                return False, "jsvmp_transparent_hook.js not found"
            with open(hook_path, "r", encoding="utf-8") as f:
                tpl = f.read()
            js = tpl.replace("{{SCRIPT_URL}}", "").replace("{{MAX_ENTRIES}}", "10000")
            persist_name = "pre_inject:jsvmp_probe_transparent"
        elif name in preset_files:
            fpath = os.path.join(hooks_dir, preset_files[name])
            if not os.path.exists(fpath):
                return False, f"hook file not found: {preset_files[name]}"
            with open(fpath, "r", encoding="utf-8") as f:
                js = f.read()
            persist_name = f"pre_inject:{name}"
        else:
            return False, f"unknown hook name: {name}"
    except Exception as e:
        return False, f"prepare failed: {e}"
    try:
        await asyncio.wait_for(
            browser_manager.add_persistent_script(persist_name, js),
            timeout=_PRE_INJECT_REGISTER_TIMEOUT,
        )
        return True, "ok"
    except asyncio.TimeoutError:
        return False, "add_persistent_script timed out (10s)"
    except Exception as e:
        return False, f"add_persistent_script failed: {e}"


@mcp.tool()
async def reload(wait_until: str = "load", page_id: str | None = None, aida_operation_id=None) -> dict:
    """Reload the current page, preserving any init scripts."""
    state: dict = {}
    try:
        state = await _begin_page_tool_operation(page_id, "reload", wait_until, aida_operation_id)
        page = state["page"]
        current_url = page.url
        if not current_url or current_url == "about:blank":
            diag = await _finish_page_tool_operation(state, "no_page_loaded")
            return {"error": "No page loaded to reload", "diagnostics": diag}
        await page.goto(current_url, wait_until=wait_until)
        title = ""
        title_error = None
        try:
            title = await page.title()
        except Exception as e:
            title_error = str(e)
        out = {"url": page.url, "title": title}
        if title_error:
            out["title_error"] = title_error
        out.update(await browser_manager.page_envelope(page))
        out["diagnostics"] = await _finish_page_tool_operation(state, "success")
        return out
    except Exception as e:
        diag = await _finish_page_tool_operation(state, "exception", e) if state else {}
        return {"error": str(e), "diagnostics": diag}


@mcp.tool()
async def take_screenshot(full_page: bool = False, selector: str | None = None, page_id: str | None = None, aida_operation_id=None) -> dict:
    """Take a screenshot of the current page or a specific element.

    Args:
        full_page: Capture the entire scrollable page.
        selector: CSS selector of a specific element to capture.
    """
    state: dict = {}
    try:
        state = await _begin_page_tool_operation(page_id, "take_screenshot", selector or "", aida_operation_id)
        page = state["page"]
        if selector:
            elem = await page.query_selector(selector)
            if not elem:
                return {"error": f"Element not found: {selector}"}
            data = await elem.screenshot()
        else:
            data = await page.screenshot(full_page=full_page)
        out = {"screenshot_base64": base64.b64encode(data).decode(), "format": "png"}
        out.update(await browser_manager.page_envelope(page))
        out["diagnostics"] = await _finish_page_tool_operation(state, "success")
        return out
    except Exception as e:
        diag = await _finish_page_tool_operation(state, "exception", e) if state else {}
        return {"error": str(e), "diagnostics": diag}


@mcp.tool()
async def take_snapshot(page_id: str | None = None, aida_operation_id=None) -> dict:
    """Get the accessibility tree of the current page (token-efficient)."""
    state: dict = {}
    try:
        state = await _begin_page_tool_operation(page_id, "take_snapshot", "", aida_operation_id)
        page = state["page"]
        try:
            snapshot = await page.accessibility.snapshot()
        except AttributeError:
            snapshot = await page.evaluate("""() => {
                function walk(node) {
                    if (!node) return null;
                    const item = {};
                    const tag = node.tagName ? node.tagName.toLowerCase() : '';
                    const role = node.getAttribute ? (node.getAttribute('role') || tag) : '';
                    if (role) item.role = role;
                    const name = node.getAttribute ? (node.getAttribute('aria-label')
                        || node.getAttribute('alt') || node.getAttribute('title')
                        || (node.tagName === 'INPUT' ? node.getAttribute('placeholder') : '')
                        || '') : '';
                    if (name) item.name = name;
                    if (['INPUT','TEXTAREA','SELECT'].includes(node.tagName)) item.value = node.value || '';
                    const text = [], children = [];
                    for (const child of (node.childNodes || [])) {
                        if (child.nodeType === 3) { const t = child.textContent.trim(); if (t) text.push(t); }
                        else if (child.nodeType === 1) { const c = walk(child); if (c) children.push(c); }
                    }
                    if (text.length && !children.length) item.text = text.join(' ');
                    if (children.length) item.children = children;
                    if (!item.role && !item.name && !item.text && !children.length) return null;
                    return item;
                }
                return walk(document.body);
            }""")
        out = {"snapshot": snapshot}
        out.update(await browser_manager.page_envelope(page))
        out["diagnostics"] = await _finish_page_tool_operation(state, "success")
        return out
    except Exception as e:
        diag = await _finish_page_tool_operation(state, "exception", e) if state else {}
        return {"error": str(e), "diagnostics": diag}


@mcp.tool()
async def click(selector: str, page_id: str | None = None, aida_operation_id=None) -> dict:
    """Click on a page element."""
    state: dict = {}
    try:
        state = await _begin_page_tool_operation(page_id, "click", selector, aida_operation_id)
        page = state["page"]
        await page.click(selector)
        out = {"status": "clicked", "selector": selector}
        out.update(await browser_manager.page_envelope(page))
        out["diagnostics"] = await _finish_page_tool_operation(state, "success")
        return out
    except Exception as e:
        diag = await _finish_page_tool_operation(state, "exception", e) if state else {}
        return {"error": str(e), "diagnostics": diag}


@mcp.tool()
async def type_text(selector: str, text: str, delay: int = 50, page_id: str | None = None, aida_operation_id=None) -> dict:
    """Type text into an input field with realistic keystroke delays."""
    state: dict = {}
    try:
        state = await _begin_page_tool_operation(page_id, "type_text", selector, aida_operation_id)
        page = state["page"]
        await page.type(selector, text, delay=delay)
        out = {"status": "typed", "selector": selector, "text": text}
        out.update(await browser_manager.page_envelope(page))
        out["diagnostics"] = await _finish_page_tool_operation(state, "success")
        return out
    except Exception as e:
        diag = await _finish_page_tool_operation(state, "exception", e) if state else {}
        return {"error": str(e), "diagnostics": diag}


@mcp.tool()
async def wait_for(
    selector: str | None = None,
    url_pattern: str | None = None,
    timeout: int = 30000,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    """Wait for an element to appear or a network request matching a URL pattern."""
    state: dict = {}
    try:
        state = await _begin_page_tool_operation(page_id, "wait_for", selector or url_pattern or "", aida_operation_id)
        page = state["page"]
        if selector:
            await page.wait_for_selector(selector, timeout=timeout)
            out = {"status": "found", "selector": selector}
            out.update(await browser_manager.page_envelope(page))
            out["diagnostics"] = await _finish_page_tool_operation(state, "success")
            return out
        elif url_pattern:
            await page.wait_for_url(url_pattern, timeout=timeout)
            out = {"status": "matched", "url_pattern": url_pattern}
            out.update(await browser_manager.page_envelope(page))
            out["diagnostics"] = await _finish_page_tool_operation(state, "success")
            return out
        else:
            diag = await _finish_page_tool_operation(state, "invalid_target")
            return {"error": "Provide either selector or url_pattern", "diagnostics": diag}
    except Exception as e:
        diag = await _finish_page_tool_operation(state, "exception", e) if state else {}
        return {"error": str(e), "diagnostics": diag}


@mcp.tool()
async def get_page_info(page_id: str | None = None, aida_operation_id=None) -> dict:
    """Get current page URL, title, and viewport size."""
    state: dict = {}
    try:
        state = await _begin_page_tool_operation(page_id, "get_page_info", "", aida_operation_id)
        page = state["page"]
        viewport = page.viewport_size or {}
        bounds = await browser_manager._page_bounds(page)
        title = ""
        title_error = None
        try:
            title = await page.title()
        except Exception as e:
            title_error = str(e)
        out = {
            "url": page.url, "title": title,
            "viewport_width": viewport.get("width"),
            "viewport_height": viewport.get("height"),
            "window_bounds": bounds,
        }
        if title_error:
            out["title_error"] = title_error
        out.update(await browser_manager.page_envelope(page))
        out["diagnostics"] = await _finish_page_tool_operation(state, "success")
        return out
    except Exception as e:
        diag = await _finish_page_tool_operation(state, "exception", e) if state else {}
        return {"error": str(e), "diagnostics": diag}


@mcp.tool()
async def reset_browser_state(
    clear_persistent_hooks: bool = True,
    clear_network_capture: bool = True,
    clear_active_routes: bool = True,
    clear_cookies: bool = False,
    clear_storage: bool = False,
    close_page_prefix: str | None = None,
    restore_page_id: str | None = None,
    close_empty_contexts: bool = True,
) -> dict:
    """Reset MCP-side browser residual state without closing the browser.

    Args:
        clear_persistent_hooks: Remove all persistent init scripts.
        clear_network_capture: Clear network request buffer and stop captures.
        clear_active_routes: Clear instrumentation routes.
        clear_cookies: ALSO clear browser cookies (destructive; default False).
        clear_storage: ALSO clear localStorage/sessionStorage (default False).
    """
    from typing import Any
    result: dict[str, Any] = {"status": "reset"}
    try:
        result["active_page_before"] = getattr(browser_manager, "active_page_id", None)
        result["page_count_before"] = len(getattr(browser_manager, "pages", {}) or {})
        result["context_count_before"] = len(getattr(browser_manager, "contexts", {}) or {})
        if clear_persistent_hooks:
            try:
                from .hooking import remove_hooks
                r = await remove_hooks(keep_persistent=False)
                result["hooks_removed"] = r
            except Exception as e:
                result["hooks_remove_error"] = str(e)
        if clear_network_capture:
            count = len(browser_manager._network_requests)
            browser_manager._network_requests.clear()
            browser_manager._request_id_counter = 0
            browser_manager._capturing = False
            browser_manager._capture_body = False
            result["network_requests_cleared"] = count
        if clear_active_routes:
            try:
                from .instrumentation import _active_routes, _stop
                count = len(_active_routes)
                await _stop(None)
                result["instrumentation_routes_cleared"] = count
            except Exception as e:
                result["instrumentation_clear_error"] = str(e)
        if clear_cookies:
            try:
                ctx = browser_manager.contexts.get("default")
                if ctx:
                    await ctx.clear_cookies()
                    result["cookies_cleared"] = True
            except Exception as e:
                result["cookies_clear_error"] = str(e)
        if clear_storage:
            try:
                page = await browser_manager.get_active_page()
                await page.evaluate(
                    "() => { try { localStorage.clear(); } catch(e) {} "
                    "try { sessionStorage.clear(); } catch(e) {} }"
                )
                result["storage_cleared"] = True
            except Exception as e:
                result["storage_clear_error"] = str(e)
        if close_page_prefix:
            prefix = str(close_page_prefix)
            closed_pages: list[dict[str, Any]] = []
            close_errors: list[dict[str, Any]] = []
            page_ids = [
                pid for pid in list(getattr(browser_manager, "pages", {}) or {})
                if str(pid).startswith(prefix)
            ]
            for pid in page_ids:
                try:
                    close_result = await browser_manager.close_page(str(pid))
                    closed_pages.append({
                        "page_id": str(pid),
                        "status": close_result.get("status") if isinstance(close_result, dict) else "",
                        "active_page_id": close_result.get("active_page_id") if isinstance(close_result, dict) else None,
                    })
                except Exception as e:
                    close_errors.append({
                        "page_id": str(pid),
                        "error_type": type(e).__name__,
                        "error": _safe_text(e, 500),
                    })
            result["closed_pages"] = closed_pages
            result["close_page_prefix"] = prefix
            if close_errors:
                result["close_page_errors"] = close_errors
        if close_empty_contexts:
            closed_contexts: list[str] = []
            context_errors: list[dict[str, Any]] = []
            for cid, ctx in list((getattr(browser_manager, "contexts", {}) or {}).items()):
                if str(cid) == "default":
                    continue
                try:
                    live_pages = [
                        page for page in list(getattr(ctx, "pages", []) or [])
                        if not browser_manager._page_closed(page)
                    ]
                    if live_pages:
                        continue
                    await ctx.close()
                    browser_manager._handle_context_close_event(str(cid), ctx)
                    closed_contexts.append(str(cid))
                except Exception as e:
                    context_errors.append({
                        "context_id": str(cid),
                        "error_type": type(e).__name__,
                        "error": _safe_text(e, 500),
                    })
            result["closed_contexts"] = closed_contexts
            if context_errors:
                result["close_context_errors"] = context_errors
        if restore_page_id:
            try:
                restore_result = await browser_manager.select_page(str(restore_page_id))
                result["restored_page_id"] = restore_result.get("page_id", restore_page_id) if isinstance(restore_result, dict) else restore_page_id
            except Exception as e:
                result["restore_page_error"] = {
                    "page_id": str(restore_page_id),
                    "error_type": type(e).__name__,
                    "error": _safe_text(e, 500),
                }
        result["active_page_after"] = getattr(browser_manager, "active_page_id", None)
        result["page_count_after"] = len(getattr(browser_manager, "pages", {}) or {})
        result["context_count_after"] = len(getattr(browser_manager, "contexts", {}) or {})
        _camoufox_debug(
            "reset_browser_state_complete",
            close_page_prefix=str(close_page_prefix or ""),
            restore_page_id=str(restore_page_id or ""),
            active_page_before=str(result.get("active_page_before") or ""),
            active_page_after=str(result.get("active_page_after") or ""),
            page_count_before=int(result.get("page_count_before") or 0),
            page_count_after=int(result.get("page_count_after") or 0),
            context_count_before=int(result.get("context_count_before") or 0),
            context_count_after=int(result.get("context_count_after") or 0),
            closed_pages=len(result.get("closed_pages") or []),
            closed_contexts=len(result.get("closed_contexts") or []),
            close_page_errors=len(result.get("close_page_errors") or []),
            close_context_errors=len(result.get("close_context_errors") or []),
            restore_failed=bool(result.get("restore_page_error")),
        )
        return result
    except Exception as e:
        _camoufox_debug("reset_browser_state_exception", error_type=type(e).__name__, error_summary=_safe_text(e, 800))
        return {"error": str(e)}
