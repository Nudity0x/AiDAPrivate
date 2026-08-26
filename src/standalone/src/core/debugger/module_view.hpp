#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "standalone_driver.hpp"
#include "debugger_interaction_context.hpp"
#include "../analysis/pe_parser.hpp"
#include "../infra/executor.hpp"
#include "../infra/event_bus.hpp"
#include "../ui/task_center.hpp"
#include "../ui/ui_thread_dispatcher.hpp"
#include "../helpers/diag_log.hpp"

namespace module_view {

// Backend store + refresh/details workers for the modules pane. The Qt pane
// (qt/debugger/modules_pane.*) observes this store through the snapshot
// accessors; the test lab and MCP tools drive refresh() directly.

struct ui_state_t {
	std::vector<driver_bridge::module_info_t> modules;
	std::string                               last_error;
	int                                       selected_module = -1;
	uint64_t                                  selected_module_base = 0;
	std::string                               selected_module_name;
	std::vector<pe_parser::export_entry_t>    exports;
	std::vector<pe_parser::import_entry_t>    imports;
	std::mutex                                modules_mutex;
	std::atomic<uint64_t>                     data_generation{1};
	std::atomic<bool>                         loading{false};
	std::atomic<uint64_t>                     last_auto_refresh_ms{0};
	std::atomic<uint64_t>                     last_event_refresh_ms{0};
	std::atomic<bool>                         subscriptions_initialized{false};
	aida::events::subscription_handle_t       dll_loaded_sub;
	aida::events::subscription_handle_t       process_created_sub;
};

inline ui_state_t g_ui;

inline void register_background_task(const aida::infra::executor::submit_result_t& submitted,
	const char* action, const char* label) {
	if (!submitted.submitted || submitted.task_id == 0) return;
	aida::ui::task_center::task_registration_t registration;
	registration.owner = "debugger";
	registration.owner_view = "view.debug.modules";
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

struct selected_module_snapshot_t {
	uint64_t    base = 0;
	uint64_t    size = 0;
	std::string name;
	std::string path;
	bool        present = false;
};

inline void sync_selected_module_locked()
{
	g_ui.selected_module = -1;
	if (g_ui.selected_module_base == 0) {
		g_ui.selected_module_name.clear();
		return;
	}
	for (size_t i = 0; i < g_ui.modules.size(); ++i) {
		const auto& m = g_ui.modules[i];
		if (m.base == g_ui.selected_module_base) {
			g_ui.selected_module = static_cast<int>(i);
			g_ui.selected_module_name = m.name;
			return;
		}
	}
}

inline void select_module_by_base(uint64_t base, const std::string& fallback_name = std::string())
{
	std::unique_lock<std::mutex> lk(g_ui.modules_mutex, std::try_to_lock);
	if (!lk.owns_lock()) return;
	bool same_base = (g_ui.selected_module_base == base);
	g_ui.selected_module_base = base;
	if (!same_base || !fallback_name.empty())
		g_ui.selected_module_name = fallback_name;
	sync_selected_module_locked();
	g_ui.data_generation.fetch_add(1, std::memory_order_release);
}

inline selected_module_snapshot_t selected_module_snapshot()
{
	selected_module_snapshot_t out;
	std::unique_lock<std::mutex> lk(g_ui.modules_mutex, std::try_to_lock);
	if (!lk.owns_lock()) return out;
	sync_selected_module_locked();
	out.base = g_ui.selected_module_base;
	out.name = g_ui.selected_module_name;
	for (const auto& m : g_ui.modules) {
		if (m.base == g_ui.selected_module_base) {
			out.base = m.base;
			out.size = static_cast<uint64_t>(m.size);
			out.name = m.name;
			out.path = m.path;
			out.present = true;
			break;
		}
	}
	return out;
}

inline void refresh()
{
	if (!driver_bridge::is_loaded() || driver_bridge::attached_pid() == 0) {
		diag::log_tagged_fmt("modules",
			"modules_refresh_skipped driver_loaded=%d attached_pid=%u",
			driver_bridge::is_loaded() ? 1 : 0,
			static_cast<unsigned>(driver_bridge::attached_pid()));
		return;
	}

	bool expected = false;
	if (!g_ui.loading.compare_exchange_strong(expected, true))
		return;

	diag::log_tagged_fmt("modules",
		"modules_refresh_request attached_pid=%u",
		static_cast<unsigned>(driver_bridge::attached_pid()));
	try {
		aida::infra::executor::submission_t sub;
		sub.owner_subsystem = "debugger";
		sub.label = "debugger.modules_refresh";
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
			g_ui.loading.store(false);
			return;
		}
		auto mods = driver_bridge::enumerate_modules();
		size_t n = mods.size();
		if (driver_bridge::attached_pid() == target_pid &&
			debugger_interaction::current_stop_generation() == target_generation) {
			std::lock_guard<std::mutex> lk(g_ui.modules_mutex);
			g_ui.modules = std::move(mods);
			g_ui.last_error.clear();
			sync_selected_module_locked();
			g_ui.data_generation.fetch_add(1, std::memory_order_release);
		}
		diag::log_tagged_fmt("modules",
			"modules_refresh_done count=%zu", n);
		g_ui.loading.store(false);
		} catch (const std::exception& ex) {
			diag::log_tagged_fmt("modules", "modules_refresh_worker_exception err='%s'", ex.what());
			{
				std::lock_guard<std::mutex> lk(g_ui.modules_mutex);
				g_ui.last_error = std::string("Module refresh failed: ") + ex.what();
			}
			g_ui.loading.store(false);
			throw;
		} catch (...) {
			diag::log_tagged("modules", "modules_refresh_worker_exception err='<unknown>'");
			{
				std::lock_guard<std::mutex> lk(g_ui.modules_mutex);
				g_ui.last_error = "Module refresh failed with an unknown error.";
			}
			g_ui.loading.store(false);
			throw;
		}
	};
		const auto submitted = aida::infra::executor::submit(std::move(sub));
		if (!submitted.submitted) {
			diag::log_tagged("modules", "modules_refresh_worker_post_failed");
			std::unique_lock<std::mutex> lock(g_ui.modules_mutex, std::try_to_lock);
			if (lock.owns_lock())
				g_ui.last_error = "Module refresh could not be queued: " + submitted.reject_reason;
			g_ui.loading.store(false);
		} else register_background_task(submitted, "debugger.modules_refresh",
			"Refresh target modules");
	} catch (const std::exception& ex) {
		diag::log_tagged_fmt("modules", "modules_refresh_worker_create_failed err='%s'", ex.what());
		std::unique_lock<std::mutex> lock(g_ui.modules_mutex, std::try_to_lock);
		if (lock.owns_lock()) g_ui.last_error = std::string("Module refresh setup failed: ") + ex.what();
		g_ui.loading.store(false);
	} catch (...) {
		diag::log_tagged("modules", "modules_refresh_worker_create_failed err='<unknown>'");
		std::unique_lock<std::mutex> lock(g_ui.modules_mutex, std::try_to_lock);
		if (lock.owns_lock()) g_ui.last_error = "Module refresh setup failed with an unknown error.";
		g_ui.loading.store(false);
	}
}

inline void ensure_subscriptions()
{
	bool expected = false;
	if (!g_ui.subscriptions_initialized.compare_exchange_strong(expected, true))
		return;

	g_ui.dll_loaded_sub = aida::events::subscribe(
		aida::events::event_dll_loaded,
		[](const aida::events::dll_loaded_t& evt) {
			uint32_t attached = driver_bridge::attached_pid();
			if (attached == 0 || attached != evt.process_id)
				return;
			uint64_t now_ms = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count());
			g_ui.last_event_refresh_ms.store(now_ms, std::memory_order_release);
			refresh();
		});

	g_ui.process_created_sub = aida::events::subscribe(
		aida::events::event_process_created,
		[](const aida::events::process_created_t&) {
			uint64_t now_ms = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count());
			g_ui.last_event_refresh_ms.store(now_ms, std::memory_order_release);
			refresh();
		});
}

inline void load_module_details_by_base(uint64_t base)
{
	if (base == 0)
		return;

	select_module_by_base(base);

	bool expected = false;
	if (!g_ui.loading.compare_exchange_strong(expected, true))
		return;

	diag::log_tagged_fmt("modules",
		"module_details_request base=0x%llx",
		static_cast<unsigned long long>(base));
	try {
		aida::infra::executor::submission_t sub;
		sub.owner_subsystem = "debugger";
		sub.label = "debugger.module_details";
		sub.thread_class = "debugger_refresh";
		sub.domain = aida::infra::executor::domain_t::feature_worker;
		sub.priority = 3;
		const std::uint32_t target_pid = driver_bridge::attached_pid();
		const std::uint64_t target_generation = debugger_interaction::current_stop_generation();
		sub.target_pid = target_pid;
		sub.generation = target_generation;
		sub.body = [base, target_pid, target_generation]() {
		try {
		if (driver_bridge::attached_pid() != target_pid ||
			debugger_interaction::current_stop_generation() != target_generation) {
			g_ui.loading.store(false);
			return;
		}
		pe_parser::pe_info_t pe;
		if (!pe_parser::parse(base, pe))
			throw std::runtime_error("PE parsing did not produce module details");
		size_t exp_n = pe.exports.size();
		size_t imp_n = pe.imports.size();
		if (driver_bridge::attached_pid() == target_pid &&
			debugger_interaction::current_stop_generation() == target_generation) {
			std::lock_guard<std::mutex> lk(g_ui.modules_mutex);
			g_ui.exports = std::move(pe.exports);
			g_ui.imports = std::move(pe.imports);
			g_ui.data_generation.fetch_add(1, std::memory_order_release);
			g_ui.last_error.clear();
		}
		diag::log_tagged_fmt("modules",
			"module_details_loaded base=0x%llx exports=%zu imports=%zu",
			static_cast<unsigned long long>(base), exp_n, imp_n);
		g_ui.loading.store(false);
		} catch (const std::exception& ex) {
			diag::log_tagged_fmt("modules", "module_details_worker_exception base=0x%llx err='%s'",
				static_cast<unsigned long long>(base), ex.what());
			{
				std::lock_guard<std::mutex> lk(g_ui.modules_mutex);
				g_ui.last_error = std::string("Module details failed: ") + ex.what();
			}
			g_ui.loading.store(false);
			throw;
		} catch (...) {
			diag::log_tagged_fmt("modules", "module_details_worker_exception base=0x%llx err='<unknown>'",
				static_cast<unsigned long long>(base));
			{
				std::lock_guard<std::mutex> lk(g_ui.modules_mutex);
				g_ui.last_error = "Module details failed with an unknown error.";
			}
			g_ui.loading.store(false);
			throw;
		}
	};
		const auto submitted = aida::infra::executor::submit(std::move(sub));
		if (!submitted.submitted) {
			diag::log_tagged("modules", "module_details_worker_post_failed");
			std::unique_lock<std::mutex> lock(g_ui.modules_mutex, std::try_to_lock);
			if (lock.owns_lock())
				g_ui.last_error = "Module details could not be queued: " + submitted.reject_reason;
			g_ui.loading.store(false);
		} else register_background_task(submitted, "debugger.module_details",
			"Load module imports/exports");
	} catch (const std::exception& ex) {
		diag::log_tagged_fmt("modules", "module_details_worker_create_failed err='%s'", ex.what());
		std::unique_lock<std::mutex> lock(g_ui.modules_mutex, std::try_to_lock);
		if (lock.owns_lock()) g_ui.last_error = std::string("Module details setup failed: ") + ex.what();
		g_ui.loading.store(false);
	} catch (...) {
		diag::log_tagged("modules", "module_details_worker_create_failed err='<unknown>'");
		std::unique_lock<std::mutex> lock(g_ui.modules_mutex, std::try_to_lock);
		if (lock.owns_lock()) g_ui.last_error = "Module details setup failed with an unknown error.";
		g_ui.loading.store(false);
	}
}

// GUI-safe snapshot accessors (try-lock; on busy the pane keeps its last
// applied snapshot).
inline std::uint64_t modules_generation()
{
	return g_ui.data_generation.load(std::memory_order_acquire);
}

inline std::shared_ptr<const std::vector<driver_bridge::module_info_t>>
modules_snapshot()
{
	std::unique_lock<std::mutex> lk(g_ui.modules_mutex, std::try_to_lock);
	if (!lk.owns_lock()) return nullptr;
	return std::make_shared<const std::vector<driver_bridge::module_info_t>>(
		g_ui.modules);
}

inline bool details_snapshot(std::vector<pe_parser::export_entry_t>& exports_out,
	std::vector<pe_parser::import_entry_t>& imports_out)
{
	std::unique_lock<std::mutex> lk(g_ui.modules_mutex, std::try_to_lock);
	if (!lk.owns_lock()) return false;
	exports_out = g_ui.exports;
	imports_out = g_ui.imports;
	return true;
}

}
