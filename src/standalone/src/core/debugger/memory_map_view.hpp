#pragma once

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

namespace memory_map_view {

// Backend store + refresh worker for the memory-map pane. The Qt pane
// (qt/debugger/memory_map_pane.*) observes this store through the snapshot
// accessors; it never touches these internals directly.

struct ui_state_t {
	std::shared_ptr<const std::vector<debugger_engine::memory_region_t>> regions =
		std::make_shared<const std::vector<debugger_engine::memory_region_t>>();
	std::string                                last_error;
	std::mutex                                 regions_mutex;
	std::atomic<bool>                          refreshing{false};
	std::atomic<std::uint64_t>                 regions_generation{1};
};

inline ui_state_t g_ui;

inline void register_background_task(const aida::infra::executor::submit_result_t& submitted,
	const char* action, const char* label) {
	if (!submitted.submitted || submitted.task_id == 0) return;
	aida::ui::task_center::task_registration_t registration;
	registration.owner = "debugger";
	registration.owner_view = "view.debug.memory_map";
	registration.owner_action = action;
	registration.label = label;
	registration.stage = "Queued";
	registration.progress = -1.f;
	registration.target = driver_bridge::attached_pid() == 0 ? std::string{} :
		"PID " + std::to_string(driver_bridge::attached_pid());
	registration.cancellation_is_safe = false;
	static_cast<void>(aida::ui::task_center::register_executor_job(
		submitted.task_id, std::move(registration)));
}

inline void refresh()
{
	if (!driver_bridge::is_loaded() || driver_bridge::attached_pid() == 0) {
		diag::log_tagged_fmt("memmap",
			"memmap_refresh_skipped driver_loaded=%d attached_pid=%u",
			driver_bridge::is_loaded() ? 1 : 0,
			static_cast<unsigned>(driver_bridge::attached_pid()));
		return;
	}

	bool expected = false;
	if (!g_ui.refreshing.compare_exchange_strong(expected, true)) {
		diag::log_tagged_fmt("memmap",
			"memmap_refresh_already_in_flight");
		return;
	}

	diag::log_tagged_fmt("memmap",
		"memmap_refresh_request attached_pid=%u",
		static_cast<unsigned>(driver_bridge::attached_pid()));
	aida::infra::executor::submission_t sub;
	sub.owner_subsystem = "debugger";
	sub.label = "debugger.memory_map_refresh";
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
			auto map = debugger_engine::get_memory_map();
			if (map.empty()) {
				const std::string detail = debugger_engine::last_error();
				if (!detail.empty()) throw std::runtime_error(detail);
			}
			size_t n = map.size();
			if (driver_bridge::attached_pid() == target_pid &&
				debugger_interaction::current_stop_generation() == target_generation) {
				std::lock_guard<std::mutex> lk(g_ui.regions_mutex);
				g_ui.regions = std::make_shared<const std::vector<debugger_engine::memory_region_t>>(
					std::move(map));
				g_ui.last_error.clear();
				g_ui.regions_generation.fetch_add(1, std::memory_order_release);
			}
			diag::log_tagged_fmt("memmap",
				"memmap_refresh_done regions=%zu", n);
			g_ui.refreshing.store(false);
		} catch (const std::exception& exception) {
			{
				std::lock_guard<std::mutex> lk(g_ui.regions_mutex);
				g_ui.last_error = std::string("Memory-map refresh failed: ") + exception.what();
			}
			g_ui.refreshing.store(false);
			throw;
		} catch (...) {
			{
				std::lock_guard<std::mutex> lk(g_ui.regions_mutex);
				g_ui.last_error = "Memory-map refresh failed with an unknown error.";
			}
			g_ui.refreshing.store(false);
			throw;
		}
	};
	const auto submitted = aida::infra::executor::submit(std::move(sub));
	if (!submitted.submitted) {
		diag::log_tagged("memmap", "memmap_refresh_post_failed");
		std::unique_lock<std::mutex> lock(g_ui.regions_mutex, std::try_to_lock);
		if (lock.owns_lock())
			g_ui.last_error = "Memory-map refresh could not be queued: " + submitted.reject_reason;
		g_ui.refreshing.store(false);
	} else register_background_task(submitted, "debugger.memory_map_refresh",
		"Refresh memory map");
}

inline bool refresh_in_flight() {
	return g_ui.refreshing.load(std::memory_order_acquire);
}

namespace detail {

inline std::string format_size(uint64_t bytes)
{
	char buf[32];
	if (bytes >= 1073741824ULL)
		std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / 1073741824.0);
	else if (bytes >= 1048576)
		std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / 1048576.0);
	else if (bytes >= 1024)
		std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
	else
		std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
	return buf;
}

inline std::string format_state(uint32_t state)
{
	if (state == 0x1000) return "COMMIT";
	if (state == 0x2000) return "RESERVE";
	if (state == 0x10000) return "FREE";
	char buf[16];
	std::snprintf(buf, sizeof(buf), "0x%X", state);
	return buf;
}

inline std::string format_type(uint32_t type)
{
	if (type == 0x1000000) return "IMAGE";
	if (type == 0x20000) return "PRIVATE";
	if (type == 0x40000) return "MAPPED";
	if (type == 0) return "";
	char buf[16];
	std::snprintf(buf, sizeof(buf), "0x%X", type);
	return buf;
}

inline bool match_filter(const debugger_engine::memory_region_t& r, const char* filter)
{
	if (!filter || filter[0] == 0) return true;
	std::string lower_filter;
	for (const char* p = filter; *p; ++p)
		lower_filter.push_back(static_cast<char>((*p >= 'A' && *p <= 'Z') ? (*p + 32) : *p));
	std::string lower_mod;
	for (auto& c : r.module_name)
		lower_mod.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? (c + 32) : c));
	std::string lower_info;
	for (auto& c : r.info)
		lower_info.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? (c + 32) : c));
	if (lower_mod.find(lower_filter) != std::string::npos) return true;
	if (lower_info.find(lower_filter) != std::string::npos) return true;
	return false;
}

}

// GUI-safe snapshot accessors (try-lock; on busy the pane keeps its last
// applied snapshot).
inline bool snapshot_state(
	std::shared_ptr<const std::vector<debugger_engine::memory_region_t>>& out,
	std::uint64_t& generation, std::string& error)
{
	std::unique_lock<std::mutex> lock(g_ui.regions_mutex, std::try_to_lock);
	if (!lock.owns_lock())
		return false;
	out = g_ui.regions;
	generation = g_ui.regions_generation.load(std::memory_order_acquire);
	error = g_ui.last_error;
	return true;
}

inline std::shared_ptr<const std::vector<debugger_engine::memory_region_t>>
regions_snapshot()
{
	std::unique_lock<std::mutex> lock(g_ui.regions_mutex, std::try_to_lock);
	if (!lock.owns_lock())
		return nullptr;
	return g_ui.regions;
}

inline bool find_region_by_base(uint64_t base, debugger_engine::memory_region_t& out)
{
	std::unique_lock<std::mutex> lk(g_ui.regions_mutex, std::try_to_lock);
	if (!lk.owns_lock()) return false;
	const auto regions = g_ui.regions;
	lk.unlock();
	if (!regions) return false;
	for (const auto& r : *regions) {
		if (r.base == base) {
			out = r;
			return true;
		}
	}
	return false;
}

}
