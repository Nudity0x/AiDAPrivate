#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "standalone_driver.hpp"
#include "debugger_engine.hpp"
#include "debugger_interaction_context.hpp"
#include "../ui/task_center.hpp"
#include "../infra/executor.hpp"
#include "../helpers/diag_log.hpp"

namespace seh_view {

// Backend store + refresh worker for the SEH pane (TEB query + chain walk).
// The Qt pane (qt/debugger/seh_pane.*) observes through snapshot_state; the
// test lab and MCP tools drive refresh() and read g_ui directly.

struct seh_entry_t {
	uint64_t    handler_addr = 0;
	uint64_t    filter_addr = 0;
	uint64_t    frame_addr = 0;
	std::string module_name;
	std::string handler_name;
	int         index = 0;
};

struct seh_diagnostics_t {
	uint32_t target_pid = 0;
	uint32_t active_tid = 0;
	uint32_t teb_query_returned = 0;
	uint32_t stack_scan_candidates = 0;
	uint32_t chain_entries = 0;
	uint64_t teb_va = 0;
	uint64_t raw_exception_list = 0;
	uint64_t rsp = 0;
	uint64_t stack_scan_start = 0;
	uint64_t stack_scan_size = 0;
	uint64_t stack_scan_bytes = 0;
	uint64_t stack_scan_candidate_frame = 0;
	uint64_t stack_scan_candidate_handler = 0;
	bool teb_query_attempted = false;
	bool teb_query_ok = false;
	bool teb_read_ok = false;
	bool teb_read_succeeded = false;
	bool exception_list_read_ok = false;
	bool sentinel_reached = false;
	bool x64_empty_chain_proven = false;
	bool stack_scan_attempted = false;
	bool stack_scan_read_ok = false;
	bool stack_scan_candidate_found = false;
	std::string empty_reason;
	std::string stack_scan_reason;
	std::string chain_stop_reason;
};

struct ui_state_t {
	std::vector<seh_entry_t> entries;
	seh_diagnostics_t        diagnostics;
	std::string              last_error;
	std::mutex               mutex;
	std::atomic<bool>        refreshing{false};
	std::atomic<uint64_t>    entries_generation{1};
};

inline ui_state_t g_ui;

inline void register_background_task(const aida::infra::executor::submit_result_t& submitted) {
	if (!submitted.submitted || submitted.task_id == 0) return;
	aida::ui::task_center::task_registration_t registration;
	registration.owner = "debugger";
	registration.owner_view = "view.debug.seh";
	registration.owner_action = "debugger.seh_refresh";
	registration.label = "Refresh SEH chain";
	registration.stage = "Queued";
	registration.progress = -1.f;
	registration.target = driver_bridge::attached_pid() == 0 ? std::string{} :
		"PID " + std::to_string(driver_bridge::attached_pid());
	registration.cancellation_is_safe = false;
	static_cast<void>(aida::ui::task_center::register_executor_job(
		submitted.task_id, std::move(registration)));
}

inline uint64_t resolve_thread_teb(uint32_t tid)
{
	if (tid == 0)
		return 0;
	struct teb_basic_t {
		long      exit_status;
		void*     teb_base;
		void*     unique_process;
		void*     unique_thread;
		uintptr_t affinity_mask;
		long      priority;
		long      base_priority;
	};
	teb_basic_t tbi{};
	uint32_t returned = 0;
	if (!driver_bridge::query_thread_information(tid, 0, &tbi, sizeof(tbi), &returned))
		return 0;
	return reinterpret_cast<uint64_t>(tbi.teb_base);
}

inline void refresh()
{
	bool expected = false;
	if (!g_ui.refreshing.compare_exchange_strong(expected, true))
		return;
	diag::log_tagged_fmt("seh",
		"seh_refresh_request attached_pid=%u active_tid=%u",
		static_cast<unsigned>(driver_bridge::attached_pid()),
		static_cast<unsigned>(debugger_engine::g_state.active_tid));
	try {
		aida::infra::executor::submission_t sub;
		sub.owner_subsystem = "debugger";
		sub.label = "debugger.seh_refresh";
		sub.thread_class = "debugger_refresh";
		sub.domain = aida::infra::executor::domain_t::feature_worker;
		sub.priority = 3;
		const std::uint32_t target_pid = driver_bridge::attached_pid();
		const std::uint64_t target_generation = debugger_interaction::current_stop_generation();
		sub.target_pid = target_pid;
		sub.generation = target_generation;
		sub.body = [target_pid, target_generation]() {
		try {
		if (driver_bridge::attached_pid() != target_pid ||
			debugger_interaction::current_stop_generation() != target_generation) {
			g_ui.refreshing.store(false);
			return;
		}
		std::vector<seh_entry_t> entries;
		seh_diagnostics_t diag_state{};
		diag_state.target_pid = driver_bridge::attached_pid();
		diag_state.active_tid = debugger_engine::g_state.active_tid;

		auto modules = driver_bridge::enumerate_modules();
		auto regs = debugger_engine::get_registers();
		diag_state.rsp = regs.rsp;

		diag_state.teb_query_attempted = diag_state.active_tid != 0;
		uint64_t teb_addr = 0;
		if (diag_state.active_tid != 0) {
			struct teb_basic_t {
				long      exit_status;
				void*     teb_base;
				void*     unique_process;
				void*     unique_thread;
				uintptr_t affinity_mask;
				long      priority;
				long      base_priority;
			};
			teb_basic_t tbi{};
			uint32_t returned = 0;
			const bool query_ok = driver_bridge::query_thread_information(diag_state.active_tid, 0, &tbi, sizeof(tbi), &returned);
			diag_state.teb_query_ok = query_ok;
			diag_state.teb_query_returned = returned;
			if (query_ok)
				teb_addr = reinterpret_cast<uint64_t>(tbi.teb_base);
		}
		diag_state.teb_va = teb_addr;
		uint64_t nt_tib_seh = 0;
		bool found_seh = false;

		if (teb_addr != 0) {
			std::vector<uint8_t> teb_buf;
			diag_state.teb_read_ok = driver_bridge::read_memory(teb_addr, 8, teb_buf);
			diag_state.teb_read_succeeded = diag_state.teb_read_ok && teb_buf.size() >= 8;
			diag_state.exception_list_read_ok = diag_state.teb_read_succeeded;
			if (diag_state.teb_read_succeeded) {
				std::memcpy(&nt_tib_seh, teb_buf.data(), 8);
				diag_state.raw_exception_list = nt_tib_seh;
				diag_state.sentinel_reached = nt_tib_seh == 0xFFFFFFFFFFFFFFFFULL;
				found_seh = (nt_tib_seh != 0 && nt_tib_seh != 0xFFFFFFFFFFFFFFFFULL);
			}
		}

		if (!found_seh) {
			diag_state.stack_scan_attempted = regs.rsp != 0;
			if (regs.rsp != 0) {
				std::vector<uint8_t> stack_buf;
				size_t scan_size = 4096;
				diag_state.stack_scan_start = regs.rsp;
				diag_state.stack_scan_size = scan_size;
				diag_state.stack_scan_read_ok = driver_bridge::read_memory(regs.rsp, scan_size, stack_buf);
				diag_state.stack_scan_bytes = stack_buf.size();
				if (diag_state.stack_scan_read_ok && stack_buf.size() >= 16) {
					for (size_t i = 8; i + 8 <= stack_buf.size(); i += 8) {
						uint64_t candidate = 0;
						std::memcpy(&candidate, stack_buf.data() + i, 8);
						if (candidate > 0x10000 && candidate < 0x7FFFFFFFFFFF) {
							++diag_state.stack_scan_candidates;
							for (auto& m : modules) {
								if (candidate >= m.base && candidate < m.base + m.size) {
									uint64_t potential_next = 0;
									std::memcpy(&potential_next, stack_buf.data() + i - 8, 8);
									if (potential_next > regs.rsp && potential_next < regs.rsp + 0x100000) {
										nt_tib_seh = regs.rsp + i - 8;
										found_seh = true;
										diag_state.stack_scan_candidate_found = true;
										diag_state.stack_scan_candidate_frame = nt_tib_seh;
										diag_state.stack_scan_candidate_handler = candidate;
										diag_state.stack_scan_reason = "candidate_frame_selected";
									} else {
										diag_state.stack_scan_reason = "candidate_next_out_of_stack_window";
									}
									break;
								}
							}
						}
						if (found_seh) break;
					}
					if (!found_seh && diag_state.stack_scan_reason.empty())
						diag_state.stack_scan_reason = diag_state.stack_scan_candidates != 0 ? "no_valid_stack_frame_candidate" : "no_module_handler_candidate";
				} else {
					diag_state.stack_scan_reason = diag_state.stack_scan_read_ok ? "stack_read_too_short" : "stack_read_failed";
				}
			} else {
				diag_state.stack_scan_reason = "rsp_zero";
			}
		}

		if (found_seh && nt_tib_seh != 0 && nt_tib_seh != 0xFFFFFFFFFFFFFFFFULL) {
			uint64_t current = nt_tib_seh;
			int idx = 0;
			const int max_chain = 256;

			while (current != 0 && current != 0xFFFFFFFFFFFFFFFFULL && idx < max_chain) {
				std::vector<uint8_t> rec_buf;
				bool read_ok = driver_bridge::read_memory(current, 16, rec_buf);
				if (!read_ok || rec_buf.size() < 16) {
					diag_state.chain_stop_reason = read_ok ? "record_read_too_short" : "record_read_failed";
					break;
				}

				seh_entry_t entry;
				entry.frame_addr = current;
				entry.index = idx;

				uint64_t next = 0;
				std::memcpy(&next, rec_buf.data(), 8);
				std::memcpy(&entry.handler_addr, rec_buf.data() + 8, 8);

				for (auto& m : modules) {
					if (entry.handler_addr >= m.base && entry.handler_addr < m.base + m.size) {
						entry.module_name = m.name;
						char off_buf[32];
						snprintf(off_buf, sizeof(off_buf), "+0x%llX",
								 static_cast<unsigned long long>(entry.handler_addr - m.base));
						entry.handler_name = m.name + off_buf;
						break;
					}
				}

				entries.push_back(std::move(entry));
				if (next == current) {
					diag_state.chain_stop_reason = "self_link";
					break;
				}
				if (next == 0) {
					diag_state.chain_stop_reason = "null_next";
					break;
				}
				if (next == 0xFFFFFFFFFFFFFFFFULL) {
					diag_state.chain_stop_reason = "sentinel_next";
					diag_state.sentinel_reached = true;
					break;
				}
				current = next;
				++idx;
			}
			if (idx >= max_chain && diag_state.chain_stop_reason.empty())
				diag_state.chain_stop_reason = "max_chain_reached";
		}

		diag_state.chain_entries = static_cast<uint32_t>(entries.size());
		if (entries.empty()) {
			if (diag_state.teb_read_succeeded && diag_state.sentinel_reached)
				diag_state.empty_reason = "teb_exception_list_sentinel";
			else if (diag_state.teb_read_succeeded && diag_state.raw_exception_list == 0)
				diag_state.empty_reason = "teb_exception_list_null";
			else if (diag_state.teb_va == 0)
				diag_state.empty_reason = "teb_unresolved";
			else if (!diag_state.teb_read_succeeded)
				diag_state.empty_reason = "teb_exception_list_read_failed";
			else
				diag_state.empty_reason = "no_valid_chain_after_stack_scan";
			diag_state.x64_empty_chain_proven = diag_state.teb_read_succeeded &&
				(diag_state.sentinel_reached || diag_state.raw_exception_list == 0) &&
				!diag_state.stack_scan_candidate_found;
		} else if (diag_state.chain_stop_reason.empty()) {
			diag_state.chain_stop_reason = "chain_entries_collected";
		}

		size_t n = entries.size();
		if (driver_bridge::attached_pid() == target_pid &&
			debugger_interaction::current_stop_generation() == target_generation) {
			std::lock_guard<std::mutex> lk(g_ui.mutex);
			g_ui.entries = std::move(entries);
			g_ui.diagnostics = diag_state;
			g_ui.last_error.clear();
			g_ui.entries_generation.fetch_add(1, std::memory_order_release);
		}
		diag::log_tagged_fmt("seh",
			"seh_refresh_done pid=%u tid=%u chain_depth=%zu teb_query_ok=%d teb_va=0x%llX teb_read_ok=%d raw_exception_list=0x%llX sentinel=%d x64_empty_chain_proven=%d empty_reason=%s stack_attempted=%d stack_read_ok=%d stack_candidates=%u stack_found=%d stack_reason=%s chain_stop=%s",
			diag_state.target_pid,
			diag_state.active_tid,
			n,
			diag_state.teb_query_ok ? 1 : 0,
			static_cast<unsigned long long>(diag_state.teb_va),
			diag_state.teb_read_succeeded ? 1 : 0,
			static_cast<unsigned long long>(diag_state.raw_exception_list),
			diag_state.sentinel_reached ? 1 : 0,
			diag_state.x64_empty_chain_proven ? 1 : 0,
			diag_state.empty_reason.c_str(),
			diag_state.stack_scan_attempted ? 1 : 0,
			diag_state.stack_scan_read_ok ? 1 : 0,
			diag_state.stack_scan_candidates,
			diag_state.stack_scan_candidate_found ? 1 : 0,
			diag_state.stack_scan_reason.c_str(),
			diag_state.chain_stop_reason.c_str());
		g_ui.refreshing.store(false);
		} catch (const std::exception& ex) {
			diag::log_tagged_fmt("seh", "seh_refresh_worker_exception err='%s'", ex.what());
			{
				std::lock_guard<std::mutex> lk(g_ui.mutex);
				g_ui.last_error = std::string("SEH refresh failed: ") + ex.what();
			}
			g_ui.refreshing.store(false);
			throw;
		} catch (...) {
			diag::log_tagged("seh", "seh_refresh_worker_exception err='<unknown>'");
			{
				std::lock_guard<std::mutex> lk(g_ui.mutex);
				g_ui.last_error = "SEH refresh failed with an unknown error.";
			}
			g_ui.refreshing.store(false);
			throw;
		}
	};
		const auto submitted = aida::infra::executor::submit(std::move(sub));
		if (!submitted.submitted) {
			diag::log_tagged("seh", "seh_refresh_worker_post_failed");
			std::unique_lock<std::mutex> lock(g_ui.mutex, std::try_to_lock);
			if (lock.owns_lock())
				g_ui.last_error = "SEH refresh could not be queued: " + submitted.reject_reason;
			g_ui.refreshing.store(false);
		} else register_background_task(submitted);
	} catch (const std::exception& ex) {
		diag::log_tagged_fmt("seh", "seh_refresh_worker_create_failed err='%s'", ex.what());
		std::unique_lock<std::mutex> lock(g_ui.mutex, std::try_to_lock);
		if (lock.owns_lock()) g_ui.last_error = std::string("SEH refresh setup failed: ") + ex.what();
		g_ui.refreshing.store(false);
	} catch (...) {
		diag::log_tagged("seh", "seh_refresh_worker_create_failed err='<unknown>'");
		std::unique_lock<std::mutex> lock(g_ui.mutex, std::try_to_lock);
		if (lock.owns_lock()) g_ui.last_error = "SEH refresh setup failed with an unknown error.";
		g_ui.refreshing.store(false);
	}
}

// GUI-safe snapshot accessor (try-lock; on busy the pane keeps its last
// applied snapshot).
inline bool snapshot_state(
	std::shared_ptr<const std::vector<seh_entry_t>>& entries_out,
	seh_diagnostics_t& diagnostics_out, std::uint64_t& generation_out,
	std::string& error_out)
{
	std::unique_lock<std::mutex> lock(g_ui.mutex, std::try_to_lock);
	if (!lock.owns_lock())
		return false;
	entries_out = std::make_shared<const std::vector<seh_entry_t>>(g_ui.entries);
	diagnostics_out = g_ui.diagnostics;
	generation_out = g_ui.entries_generation.load(std::memory_order_acquire);
	error_out = g_ui.last_error;
	return true;
}

}
