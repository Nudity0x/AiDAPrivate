"""Offline signer verification tool (v1.0.0).

Replaces verify_against_session. Fully stateless — user provides samples.
"""
from __future__ import annotations

import asyncio
import time

from ..browser import _await_no_cancel_wait, _camoufox_debug
from ..server import mcp, browser_manager


_VERIFY_PAGE_TIMEOUT_S = 5.0
_VERIFY_SIGNER_EVAL_TIMEOUT_S = 8.0
_VERIFY_SAMPLE_TIMEOUT_S = 4.0


@mcp.tool()
async def verify_signer_offline(
    signer_code: str,
    samples: list[dict],
    compare_params: list[str] | None = None,
) -> dict:
    """Offline verify a signing function against user-provided samples.

    Typical workflow:
      1. Capture real signed requests via network_capture + list_network_requests
      2. Extract samples into a list
      3. Write candidate signing code
      4. Call this tool -> get pass_rate + first_divergence
      5. Iterate

    Args:
        signer_code: JS evaluating to a function: (sample) => {param: computed_value}.
            Runs in current page context.
        samples: List of sample dicts, each with:
            - id: user-defined identifier
            - input: dict passed to signer function
            - expected: dict of {param_name: expected_value_str}
        compare_params: Which params to compare. If None, compare all keys
            in each sample's expected.

    Returns:
        dict with total_samples, passed, failed, pass_rate, first_divergence, details.
    """
    started = time.perf_counter()
    sample_count = len(samples) if isinstance(samples, list) else 0
    timeout_phase = "entry"
    _camoufox_debug(
        "verify_signer_offline_begin",
        signer_code_len=len(signer_code or ""),
        sample_count=sample_count,
        compare_param_count=len(compare_params or []),
        page_timeout_ms=int(_VERIFY_PAGE_TIMEOUT_S * 1000),
        signer_eval_timeout_ms=int(_VERIFY_SIGNER_EVAL_TIMEOUT_S * 1000),
        sample_timeout_ms=int(_VERIFY_SAMPLE_TIMEOUT_S * 1000),
    )
    try:
        if not isinstance(samples, list) or not samples:
            return {"error": "samples must be a non-empty list"}

        timeout_phase = "get_active_page"
        page = await _await_no_cancel_wait(browser_manager.get_active_page(), timeout=_VERIFY_PAGE_TIMEOUT_S)
        page_url_len = -1
        try:
            page_url_len = len(page.url or "")
        except Exception:
            pass
        _camoufox_debug(
            "verify_signer_offline_page",
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            page_url_len=page_url_len,
            sample_count=len(samples),
        )
        try:
            timeout_phase = "signer_eval"
            eval_started = time.perf_counter()
            await _await_no_cancel_wait(
                page.evaluate(f"window.__mcp_signer_fn = {signer_code};"),
                timeout=_VERIFY_SIGNER_EVAL_TIMEOUT_S,
            )
            _camoufox_debug(
                "verify_signer_offline_signer_eval_ok",
                elapsed_ms=int((time.perf_counter() - started) * 1000),
                phase_elapsed_ms=int((time.perf_counter() - eval_started) * 1000),
                page_url_len=page_url_len,
            )
        except asyncio.TimeoutError:
            elapsed_ms = int((time.perf_counter() - started) * 1000)
            _camoufox_debug(
                "verify_signer_offline_signer_eval_timeout",
                elapsed_ms=elapsed_ms,
                timeout_ms=int(_VERIFY_SIGNER_EVAL_TIMEOUT_S * 1000),
                page_url_len=page_url_len,
            )
            return {
                "status": "degraded",
                "instrumentation_status": "timeout",
                "timeout_status": "controlled_timeout",
                "error": "signer_code evaluation timed out",
                "phase": "signer_eval",
                "timeout_ms": int(_VERIFY_SIGNER_EVAL_TIMEOUT_S * 1000),
                "elapsed_ms": elapsed_ms,
                "total_samples": len(samples),
                "passed": 0,
                "failed": len(samples),
                "pass_rate": 0,
                "first_divergence": None,
                "details": [],
                "page_url_len": page_url_len,
            }
        except Exception as e:
            _camoufox_debug(
                "verify_signer_offline_signer_eval_failed",
                elapsed_ms=int((time.perf_counter() - started) * 1000),
                error_type=type(e).__name__,
                error_len=len(str(e)),
                page_url_len=page_url_len,
            )
            return {
                "status": "error",
                "instrumentation_status": "error",
                "error": f"signer_code failed to evaluate: {e}",
                "phase": "signer_eval",
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
                "page_url_len": page_url_len,
            }

        details = []
        passed = failed = 0
        first_divergence = None
        sample_timeouts = 0
        sample_errors = 0

        for s in samples:
            sid = s.get("id", f"sample_{len(details)}")
            sample_input = s.get("input", {})
            expected = s.get("expected", {})

            try:
                timeout_phase = "sample_eval"
                sample_started = time.perf_counter()
                computed = await _await_no_cancel_wait(
                    page.evaluate("(sample) => window.__mcp_signer_fn(sample)", sample_input),
                    timeout=_VERIFY_SAMPLE_TIMEOUT_S,
                )
            except asyncio.TimeoutError:
                sample_timeouts += 1
                failed += 1
                detail = {
                    "sample_id": sid,
                    "passed": False,
                    "error": "signer sample evaluation timed out",
                    "timeout_ms": int(_VERIFY_SAMPLE_TIMEOUT_S * 1000),
                }
                details.append(detail)
                if first_divergence is None:
                    first_divergence = {"sample_id": sid, "error": detail["error"], "input": sample_input}
                _camoufox_debug(
                    "verify_signer_offline_sample_timeout",
                    elapsed_ms=int((time.perf_counter() - started) * 1000),
                    sample_id=str(sid),
                    sample_index=len(details) - 1,
                    timeout_ms=int(_VERIFY_SAMPLE_TIMEOUT_S * 1000),
                )
                continue
            except Exception as e:
                sample_errors += 1
                details.append({"sample_id": sid, "passed": False, "error": f"signer threw: {e}"})
                failed += 1
                if first_divergence is None:
                    first_divergence = {"sample_id": sid, "error": f"signer threw: {type(e).__name__}", "input": sample_input}
                _camoufox_debug(
                    "verify_signer_offline_sample_failed",
                    elapsed_ms=int((time.perf_counter() - started) * 1000),
                    sample_id=str(sid),
                    sample_index=len(details) - 1,
                    error_type=type(e).__name__,
                    error_len=len(str(e)),
                )
                continue

            diffs = _compare_params(expected, computed, compare_params)
            if not diffs:
                passed += 1
                details.append({
                    "sample_id": sid,
                    "passed": True,
                    "elapsed_ms": int((time.perf_counter() - sample_started) * 1000),
                })
            else:
                failed += 1
                details.append({
                    "sample_id": sid,
                    "passed": False,
                    "diffs": diffs,
                    "elapsed_ms": int((time.perf_counter() - sample_started) * 1000),
                })
                if first_divergence is None:
                    first_divergence = {"sample_id": sid, "diffs": diffs, "input": sample_input}

        elapsed_ms = int((time.perf_counter() - started) * 1000)
        status = "ok" if sample_timeouts == 0 and sample_errors == 0 else "degraded"
        result = {
            "status": status,
            "instrumentation_status": "complete" if status == "ok" else "partial",
            "total_samples": len(samples), "passed": passed, "failed": failed,
            "pass_rate": round(passed / len(samples), 3) if samples else 0,
            "first_divergence": first_divergence, "details": details,
            "sample_timeout_count": sample_timeouts,
            "sample_error_count": sample_errors,
            "sample_timeout_ms": int(_VERIFY_SAMPLE_TIMEOUT_S * 1000),
            "elapsed_ms": elapsed_ms,
            "page_url_len": page_url_len,
        }
        if sample_timeouts:
            result["timeout_status"] = "controlled_timeout"
        _camoufox_debug(
            "verify_signer_offline_exit",
            success=True,
            status=status,
            elapsed_ms=elapsed_ms,
            total_samples=len(samples),
            passed=passed,
            failed=failed,
            sample_timeout_count=sample_timeouts,
            sample_error_count=sample_errors,
            page_url_len=page_url_len,
        )
        return result
    except asyncio.TimeoutError:
        elapsed_ms = int((time.perf_counter() - started) * 1000)
        timeout_ms = int((_VERIFY_PAGE_TIMEOUT_S if timeout_phase == "get_active_page" else _VERIFY_SAMPLE_TIMEOUT_S) * 1000)
        _camoufox_debug(
            "verify_signer_offline_timeout",
            elapsed_ms=elapsed_ms,
            timeout_phase=timeout_phase,
            timeout_ms=timeout_ms,
            sample_count=sample_count,
        )
        return {
            "status": "degraded",
            "instrumentation_status": "timeout",
            "timeout_status": "controlled_timeout",
            "error": "verify_signer_offline timed out",
            "phase": timeout_phase,
            "timeout_ms": timeout_ms,
            "elapsed_ms": elapsed_ms,
            "total_samples": sample_count,
            "passed": 0,
            "failed": sample_count,
            "pass_rate": 0,
            "first_divergence": None,
            "details": [],
        }
    except Exception as e:
        _camoufox_debug(
            "verify_signer_offline_exit",
            success=False,
            elapsed_ms=int((time.perf_counter() - started) * 1000),
            timeout_phase=timeout_phase,
            error_type=type(e).__name__,
            error_len=len(str(e)),
        )
        return {"status": "error", "instrumentation_status": "error", "error": str(e), "phase": timeout_phase}


def _compare_params(expected: dict, computed: dict, focus: list[str] | None) -> list[dict]:
    diffs = []
    keys = focus if focus else list(expected.keys())
    for k in keys:
        exp = expected.get(k)
        act = (computed or {}).get(k)
        if exp == act:
            continue
        if isinstance(exp, str) and isinstance(act, str):
            first_diff = -1
            for i in range(min(len(exp), len(act))):
                if exp[i] != act[i]:
                    first_diff = i
                    break
            if first_diff == -1 and len(exp) != len(act):
                first_diff = min(len(exp), len(act))
            diffs.append({"param": k, "expected": exp, "actual": act,
                          "first_diff_char": first_diff,
                          "expected_length": len(exp), "actual_length": len(act)})
        else:
            diffs.append({"param": k, "expected": exp, "actual": act})
    return diffs
