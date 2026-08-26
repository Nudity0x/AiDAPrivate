#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "debugger_view.hpp"
#include "debugger_engine.hpp"
#include "debugger_interaction_context.hpp"
#include "source_debug_service.hpp"
#include "spawn_target_dialog.hpp"
#include "standalone_driver.hpp"
#include "../runtime/standalone_driver_identity.hpp"
#include "../helpers/diag_log.hpp"
#include "memory_map_view.hpp"
#include "../scanner/pointer_scanner.hpp"
#include "../ui/application_ui_runtime.hpp"
#include "../ui/task_center.hpp"
#include "../ui/ui_thread_dispatcher.hpp"
#include "../ai/entity_evidence_handoff.hpp"
#include "../analysis/stealth_engine.hpp"
#include "../analysis/code_patcher.hpp"
#include "../disasm/disasm_view.hpp"
#include "../editor/hex_view.hpp"
#include "../infra/executor.hpp"
#include "toast_notification.hpp"

#include "qt/analysis/qt_analysis_host_hooks.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace debugger_view {

bool is_visible_sub_tab(sub_tab_t tab) {
	switch (tab) {
		case sub_tab_t::cpu:
		case sub_tab_t::breakpoints:
		case sub_tab_t::memory_map:
		case sub_tab_t::call_stack:
		case sub_tab_t::threads:
		case sub_tab_t::watches:
		case sub_tab_t::handles:
		case sub_tab_t::trace_log:
		case sub_tab_t::strings:
		case sub_tab_t::bookmarks:
		case sub_tab_t::modules:
		case sub_tab_t::patches:
		case sub_tab_t::seh_chain:
		case sub_tab_t::cfg:
			return true;
		case sub_tab_t::COUNT:
			return false;
	}
	return false;
}

int visible_sub_tab_count() {
	return static_cast<int>(sub_tab_t::COUNT);
}

namespace {

std::atomic<bool> g_execution_command_pending{false};
std::atomic<bool> g_target_mutation_pending{false};

host_ui_hooks_t g_hooks;
std::atomic<bool> g_hooks_installed{false};

bool debugger_context_identity_equal(const debugger_interaction::context_t& left,
	const debugger_interaction::context_t& right) noexcept {
	return left.kind == right.kind && left.target_pid == right.target_pid &&
		left.process_creation_time_100ns == right.process_creation_time_100ns &&
		left.stop_generation == right.stop_generation && left.address == right.address &&
		left.value == right.value && left.extent == right.extent &&
		left.thread_id == right.thread_id && left.index == right.index &&
		left.primary_text == right.primary_text && left.secondary_text == right.secondary_text;
}

std::vector<debugger_interaction::context_t> debugger_action_contexts(
	const debugger_interaction::context_t& focused) {
	auto contexts = debugger_interaction::selected_set();
	const bool compatible = !contexts.empty() &&
		contexts.front().kind == focused.kind &&
		contexts.front().target_pid == focused.target_pid &&
		contexts.front().process_creation_time_100ns == focused.process_creation_time_100ns &&
		contexts.front().stop_generation == focused.stop_generation;
	if (!compatible)
		contexts.assign(1, focused);
	else if (std::none_of(contexts.begin(), contexts.end(), [&](const auto& item) {
		return debugger_context_identity_equal(item, focused);
	}))
		contexts.assign(1, focused);
	return contexts;
}

std::uint64_t debugger_action_set_hash(
	const std::vector<debugger_interaction::context_t>& contexts) noexcept {
	std::uint64_t hash = 1469598103934665603ULL;
	const auto mix = [&](std::uint64_t value) {
		hash ^= value;
		hash *= 1099511628211ULL;
	};
	for (const auto& context : contexts) {
		mix(static_cast<std::uint64_t>(context.kind));
		mix(context.target_pid);
		mix(context.process_creation_time_100ns);
		mix(context.stop_generation);
		mix(context.address);
		mix(context.value);
		mix(context.extent);
		mix(context.thread_id);
		mix(static_cast<std::uint64_t>(context.index));
		for (const char ch : context.primary_text)
			mix(static_cast<unsigned char>(ch));
		for (const char ch : context.secondary_text)
			mix(static_cast<unsigned char>(ch));
	}
	return hash;
}

std::string debugger_action_entity_id(
	const debugger_interaction::context_t& focused,
	const std::vector<debugger_interaction::context_t>& contexts) {
	return std::to_string(static_cast<int>(focused.kind)) + ":" +
		std::to_string(focused.address) + ":" + std::to_string(focused.index) + ":" +
		std::to_string(contexts.size()) + ":" +
		std::to_string(debugger_action_set_hash(contexts));
}

struct code_cave_search_state_t {
	std::atomic<bool> pending{false};
	std::string dialog_error;
};

code_cave_search_state_t g_code_cave_search;
std::atomic<std::uint64_t> g_code_cave_publication_sequence{1};
std::shared_ptr<const code_cave_publication_view_t> g_code_cave_publication =
	std::make_shared<const code_cave_publication_view_t>();

template <typename Fn>
bool post_debugger_ui(Fn&& fn, const char* label) {
	return aida::ui_thread::post(std::forward<Fn>(fn), "debugger", label,
		"worker_completion");
}

const char* execution_command_label(execution_command_t command) {
	switch (command) {
		case execution_command_t::launch: return "Launch target";
		case execution_command_t::run_continue: return "Continue target";
		case execution_command_t::pause: return "Pause target";
		case execution_command_t::step_over: return "Step over";
		case execution_command_t::step_into: return "Step into";
		case execution_command_t::step_out: return "Step out";
		case execution_command_t::stop: return "Stop target";
		case execution_command_t::restart: return "Restart target";
		case execution_command_t::detach: return "Detach target";
		case execution_command_t::toggle_breakpoint_at_instruction_pointer:
			return "Toggle instruction-pointer breakpoint";
	}
	return "Debugger command";
}

}

bool write_file_atomic_exact(const std::string& destination,
	const void* bytes, std::size_t size, std::string& error) {
	if (destination.empty() || (bytes == nullptr && size != 0)) {
		error = "Invalid export destination or payload";
		return false;
	}
	const std::filesystem::path final_path(destination);
	const std::filesystem::path temporary = final_path.wstring() + L".aida-tmp-" +
		std::to_wstring(GetCurrentProcessId()) + L"-" +
		std::to_wstring(GetTickCount64());
	HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
		FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		error = "Could not create the temporary export file (Win32 " +
			std::to_string(GetLastError()) + ")";
		return false;
	}
	const auto* cursor = static_cast<const std::uint8_t*>(bytes);
	std::size_t remaining = size;
	bool ok = true;
	while (remaining != 0) {
		const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1U << 20U));
		DWORD written = 0;
		if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk) {
			error = "The export write was incomplete (Win32 " +
				std::to_string(GetLastError()) + ")";
			ok = false;
			break;
		}
		cursor += written;
		remaining -= written;
	}
	if (ok && !FlushFileBuffers(file)) {
		error = "The export could not be flushed to disk (Win32 " +
			std::to_string(GetLastError()) + ")";
		ok = false;
	}
	CloseHandle(file);
	if (ok && !MoveFileExW(temporary.c_str(), final_path.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		error = "The completed export could not replace its destination (Win32 " +
			std::to_string(GetLastError()) + ")";
		ok = false;
	}
	if (!ok)
		static_cast<void>(DeleteFileW(temporary.c_str()));
	return ok;
}

bool register_debugger_task(const aida::infra::executor::submit_result_t& submitted,
	const char* owner_view, const char* owner_action, const char* label,
	bool cancellable) {
	if (!submitted.submitted || submitted.task_id == 0)
		return false;
	aida::ui::task_center::task_registration_t registration;
	registration.owner = "debugger";
	registration.owner_view = owner_view ? owner_view : "view.debug.cpu";
	registration.owner_action = owner_action ? owner_action : "debugger.task";
	registration.target = driver_bridge::attached_pid() == 0 ? std::string{} :
		"PID " + std::to_string(driver_bridge::attached_pid());
	registration.label = label ? label : "Debugger task";
	registration.stage = "Queued";
	registration.progress = -1.f;
	registration.cancellation_is_safe = cancellable;
	const std::string focus_view = registration.owner_view;
	registration.callbacks.focus = [focus_view]() {
		static_cast<void>(post_debugger_ui([focus_view]() {
			if (g_hooks_installed.load(std::memory_order_acquire) &&
				g_hooks.open_or_focus)
				static_cast<void>(g_hooks.open_or_focus(focus_view.c_str()));
		}, "task_focus"));
	};
	return aida::ui::task_center::register_executor_job(
		submitted.task_id, std::move(registration));
}

namespace {

struct debugger_task_admission_t {
	std::mutex mutex;
	std::condition_variable condition;
	unsigned state = 0;
};

}

aida::infra::executor::submit_result_t submit_owned_debugger_task(
	aida::infra::executor::submission_t submission, const char* owner_view,
	const char* owner_action, const char* label, bool cancellable) {
	auto admission = std::make_shared<debugger_task_admission_t>();
	auto body = std::move(submission.body);
	submission.body = [admission, body = std::move(body)]() mutable {
		std::unique_lock<std::mutex> lock(admission->mutex);
		admission->condition.wait(lock, [&admission] { return admission->state != 0U; });
		const bool admitted = admission->state == 1U;
		lock.unlock();
		if (admitted)
			body();
	};
	const auto publish_admission = [&admission](unsigned state) {
		{
			std::lock_guard<std::mutex> lock(admission->mutex);
			admission->state = state;
		}
		admission->condition.notify_one();
	};
	auto submitted = aida::infra::executor::submit(std::move(submission));
	if (!submitted.submitted)
		return submitted;
	bool registered = false;
	try {
		registered = register_debugger_task(
			submitted, owner_view, owner_action, label, cancellable);
	} catch (...) {
		registered = false;
	}
	if (!registered) {
		publish_admission(2U);
		submitted.submitted = false;
		submitted.reject_reason = "Task Center could not retain ownership";
		return submitted;
	}
	publish_admission(1U);
	return submitted;
}

void install_host_ui_hooks(host_ui_hooks_t hooks) {
	g_hooks = std::move(hooks);
	g_hooks_installed.store(true, std::memory_order_release);
	diag::log_tagged("debugger_context", "host_ui_hooks_installed");
}

bool host_ui_hooks_installed() noexcept {
	return g_hooks_installed.load(std::memory_order_acquire);
}

namespace {

void request_launch_dialog() {
	if (!g_hooks.request_spawn_dialog)
		return;
	const auto context = disasm_view::capture_selected_workspace();
	const std::string path = context.workspace
		? context.workspace->identity().normalized_source_path() : std::string{};
	if (!path.empty() && g_hooks.request_spawn_dialog_with_path)
		g_hooks.request_spawn_dialog_with_path(path);
	else
		g_hooks.request_spawn_dialog();
}

bool spawn_dialog_is_open() {
	return g_hooks.spawn_dialog_open && g_hooks.spawn_dialog_open();
}

}

bool jump_to_disasm(uint64_t addr) {
	if (addr == 0) return false;
	if (g_hooks.open_or_focus)
		static_cast<void>(g_hooks.open_or_focus("document.disassembly"));
	disasm_view::goto_address(addr, disasm_view::capture_selected_workspace());
	return true;
}

bool jump_to_hex(uint64_t addr, size_t bytes) {
	if (addr == 0) return false;
	const auto context = disasm_view::capture_selected_workspace();
	if (!context.workspace || !context.workspace->identity().process() ||
		bytes == 0 || bytes > (64ULL << 20))
		return false;
	if (!hex_view::request_live_memory(context, addr, bytes))
		return false;
	if (g_hooks.open_or_focus)
		static_cast<void>(g_hooks.open_or_focus("document.hex"));
	return true;
}

void copy_to_clipboard(const std::string& text) {
	if (text.empty()) return;
	if (g_hooks.clipboard_set)
		g_hooks.clipboard_set(text);
}

void copy_address_to_clipboard(std::uint64_t addr) {
	char buf[20];
	std::snprintf(buf, sizeof(buf), "0x%016" PRIX64, addr);
	copy_to_clipboard(buf);
}

execution_capability_t execution_capability(execution_command_t command) {
	if (g_execution_command_pending.load(std::memory_order_acquire))
		return {false, "Another debugger execution command is still pending"};
	const std::uint32_t pid = driver_bridge::attached_pid();
	const auto status = debugger_engine::g_state.status.load(std::memory_order_acquire);
	const bool attached = pid != 0;
	const bool running = status == debugger_engine::dbg_status_t::running;
	const bool paused = status == debugger_engine::dbg_status_t::paused ||
		status == debugger_engine::dbg_status_t::stepping;
	switch (command) {
		case execution_command_t::launch:
			return attached
				? execution_capability_t{false, "Detach or stop the current target before launching another process"}
				: execution_capability_t{true, nullptr};
		case execution_command_t::run_continue:
			return !attached || paused || status == debugger_engine::dbg_status_t::idle
				? execution_capability_t{true, nullptr}
				: execution_capability_t{false, running ? "The target is already running" : "The target cannot continue from its current state"};
		case execution_command_t::pause:
			return running ? execution_capability_t{true, nullptr}
				: execution_capability_t{false, attached ? "The target is not running" : "Attach or launch a target first"};
		case execution_command_t::step_over:
		case execution_command_t::step_into:
		case execution_command_t::step_out:
			return paused ? execution_capability_t{true, nullptr}
				: execution_capability_t{false, attached ? "Pause the target before stepping" : "Attach or launch a target first"};
		case execution_command_t::stop:
		case execution_command_t::detach:
			return attached ? execution_capability_t{true, nullptr}
				: execution_capability_t{false, "No live target is attached"};
		case execution_command_t::restart:
			return attached ? execution_capability_t{true, nullptr}
				: execution_capability_t{false, "Launch a target before using Restart"};
		case execution_command_t::toggle_breakpoint_at_instruction_pointer:
			if (!attached)
				return {false, "Attach or launch a target first"};
			if (!paused)
				return {false, "Pause the target before changing a breakpoint at RIP"};
			return debugger_engine::cached_registers().rip != 0
				? execution_capability_t{true, nullptr}
				: execution_capability_t{false, "The instruction pointer is unavailable"};
	}
	return {false, "Unknown debugger command"};
}

bool execution_command_pending() noexcept {
	return g_execution_command_pending.load(std::memory_order_acquire);
}

bool target_mutation_pending() noexcept {
	return g_target_mutation_pending.load(std::memory_order_acquire);
}

bool execute_command(execution_command_t command, std::string* error) {
	auto fail = [&](std::string detail) {
		if (error) *error = std::move(detail);
		return false;
	};
	if (error) error->clear();
	const auto capability = execution_capability(command);
	if (!capability.enabled)
		return fail(capability.disabled_reason ? capability.disabled_reason : "Debugger command is unavailable");

	const std::uint32_t pid = driver_bridge::attached_pid();
	if (command == execution_command_t::launch ||
		(command == execution_command_t::run_continue && pid == 0)) {
		if (!spawn_dialog_is_open())
			request_launch_dialog();
		return true;
	}
	bool expected = false;
	if (!g_execution_command_pending.compare_exchange_strong(expected, true,
		std::memory_order_acq_rel))
		return fail("Another debugger execution command is still pending");
	const std::uint64_t generation = debugger_interaction::current_stop_generation();
	struct command_result_t {
		bool ok = false;
		bool restart = false;
		debugger_engine::dbg_status_t final_status = debugger_engine::dbg_status_t::idle;
		std::string error;
	};
	auto result = std::make_shared<command_result_t>();
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "debugger";
	submission.label = execution_command_label(command);
	submission.thread_class = "debugger_target_control";
	submission.domain = aida::infra::executor::domain_t::feature_worker;
	submission.priority = 2;
	submission.target_pid = pid;
	submission.generation = generation;
	submission.ui_access_policy = "post_completion_only";
	submission.failure_policy = "fail_closed";
	submission.body = [command, pid, generation, result]() {
		auto fail_worker = [&](std::string detail) {
			result->error = std::move(detail);
		};
		if (driver_bridge::attached_pid() != pid ||
			debugger_interaction::current_stop_generation() != generation) {
			fail_worker("The target changed before the debugger command started");
		} else {
			switch (command) {
				case execution_command_t::run_continue:
					result->ok = debugger_engine::run_target();
					break;
				case execution_command_t::pause:
					result->ok = debugger_engine::pause_target();
					break;
				case execution_command_t::step_over:
					result->ok = debugger_engine::step_over();
					break;
				case execution_command_t::step_into:
					result->ok = debugger_engine::step_into();
					break;
				case execution_command_t::step_out:
					result->ok = debugger_engine::step_out();
					break;
				case execution_command_t::stop:
				case execution_command_t::restart: {
					HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
					if (process == nullptr) {
						fail_worker("Windows denied access to terminate the target");
						break;
					}
					const BOOL terminated = TerminateProcess(process, 0xDEADu);
					const DWORD terminate_error = terminated ? ERROR_SUCCESS : GetLastError();
					if (terminated)
						static_cast<void>(WaitForSingleObject(process, 2000));
					CloseHandle(process);
					if (!terminated) {
						char detail[96];
						std::snprintf(detail, sizeof(detail),
							"Target termination failed (Win32 %lu)",
							static_cast<unsigned long>(terminate_error));
						fail_worker(detail);
						break;
					}
					stealth_engine::disable_for_detach(pid,
						command == execution_command_t::restart
							? "debugger_view.command_restart"
							: "debugger_view.command_stop");
					driver_bridge::detach();
					if (driver_bridge::attached_pid() != 0) {
						fail_worker("The target exited but the driver attachment did not clear");
						break;
					}
					result->ok = true;
					result->restart = command == execution_command_t::restart;
					result->final_status = debugger_engine::dbg_status_t::terminated;
					break;
				}
				case execution_command_t::detach:
					stealth_engine::disable_for_detach(pid, "debugger_view.command_detach");
					driver_bridge::detach();
					if (driver_bridge::attached_pid() != 0)
						fail_worker("The driver did not confirm target detachment");
					else {
						result->ok = true;
						result->final_status = debugger_engine::dbg_status_t::idle;
					}
					break;
				case execution_command_t::toggle_breakpoint_at_instruction_pointer: {
					const std::uint64_t rip = debugger_engine::cached_registers().rip;
					auto snapshot = debugger_engine::snapshot_breakpoints();
					int existing = -1;
					for (std::size_t index = 0; index < snapshot.size(); ++index) {
						if (!snapshot[index].is_internal && snapshot[index].address == rip &&
							snapshot[index].type == debugger_engine::bp_type_t::software) {
							existing = static_cast<int>(index);
							break;
						}
					}
					result->ok = existing >= 0 ? debugger_engine::remove_breakpoint(existing)
						: debugger_engine::add_breakpoint(rip) >= 0;
					break;
				}
				case execution_command_t::launch:
					break;
			}
			if (!result->ok && result->error.empty()) {
				result->error = debugger_engine::last_error();
				if (result->error.empty())
					result->error = "The debugger engine rejected the command";
			}
		}
		const bool posted = post_debugger_ui([result]() {
			if (result->ok) {
				if (result->final_status == debugger_engine::dbg_status_t::terminated ||
					result->final_status == debugger_engine::dbg_status_t::idle)
					debugger_engine::g_state.status.store(result->final_status,
						std::memory_order_release);
				debugger_interaction::advance_stop_generation();
				if (result->restart && !spawn_dialog_is_open()) {
					const auto context = disasm_view::capture_selected_workspace();
					const std::string path = context.workspace
						? context.workspace->identity().normalized_source_path() : std::string{};
					if (path.empty()) {
						if (g_hooks.request_spawn_dialog)
							g_hooks.request_spawn_dialog();
					} else if (g_hooks.request_spawn_dialog_with_path) {
						g_hooks.request_spawn_dialog_with_path(path);
					}
				}
			} else {
				toast_notification::push(result->error,
					toast_notification::toast_type_t::error);
			}
			g_execution_command_pending.store(false, std::memory_order_release);
		}, "execution_completion");
		if (!posted) {
			g_execution_command_pending.store(false, std::memory_order_release);
			throw std::runtime_error("Debugger command completion could not be published to the UI thread");
		}
		if (!result->ok)
			throw std::runtime_error(result->error.empty()
				? "Debugger command failed" : result->error);
	};
	const auto submitted = submit_owned_debugger_task(std::move(submission),
		"view.debug.cpu", "debugger.execution", execution_command_label(command), false);
	if (!submitted.submitted) {
		g_execution_command_pending.store(false, std::memory_order_release);
		return fail("The debugger executor rejected the command: " + submitted.reject_reason);
	}
	return true;
}

execution_capability_t patch_panel_capability(patch_panel_command_t command) {
	if (command == patch_panel_command_t::stage)
		return debugger_engine::cached_registers().rip != 0
			? execution_capability_t{true, nullptr}
			: execution_capability_t{false, "No current instruction address is available"};
	if (command == patch_panel_command_t::find_code_caves) {
		if (!driver_bridge::is_loaded() || driver_bridge::attached_pid() == 0)
			return {false, "Attach a live target before searching for code caves"};
		if (debugger_engine::g_state.status.load(std::memory_order_acquire) !=
			debugger_engine::dbg_status_t::paused)
			return {false, "Pause the attached target before searching for code caves"};
		return g_code_cave_search.pending.load(std::memory_order_acquire)
			? execution_capability_t{false, "A code-cave search is already running"}
			: execution_capability_t{true, nullptr};
	}
	const auto snapshot = code_patcher::published_snapshot();
	if (!snapshot)
		return {false, "The immutable Patches publication is unavailable"};
	if (code_patcher::g_state.publication_failure_generation.load(
			std::memory_order_acquire) != 0)
		return {false, "The Patches publication could not be refreshed; preserve the authoritative patch state and retry after memory pressure clears"};
	switch (command) {
		case patch_panel_command_t::stage:
		case patch_panel_command_t::find_code_caves:
			break;
		case patch_panel_command_t::revert_all:
			if (snapshot->total_count == 0)
				return {false, "No patch definitions are available"};
			return snapshot->total_count <= 65536U
				? execution_capability_t{true, nullptr}
				: execution_capability_t{false, "The patch set exceeds the 65,536-entry review bound"};
		case patch_panel_command_t::save_patchset:
			if (snapshot->total_count == 0)
				return {false, "No patch definitions are available"};
			return snapshot->total_count <= 4096U
				? execution_capability_t{true, nullptr}
				: execution_capability_t{false, "The patch set exceeds the 4,096-entry export bound"};
	}
	return {false, "Unknown Patches panel action"};
}

bool execute_patch_panel_command(patch_panel_command_t command, std::string* error) {
	const auto capability = patch_panel_capability(command);
	if (!capability.enabled) {
		if (error) *error = capability.disabled_reason ? capability.disabled_reason
			: "The Patches panel action is unavailable";
		return false;
	}
	bool was_open = false;
	if (g_hooks.is_open)
		was_open = g_hooks.is_open("view.debug.patches");
	bool opened_ok = true;
	if (g_hooks.open_or_focus) {
		const auto opened = g_hooks.open_or_focus("view.debug.patches");
		opened_ok = opened.ok();
	}
	if (!opened_ok) {
		if (error) *error = "The canonical Patches view could not be opened";
		return false;
	}
	if (dispatch_patch_panel_command(command, error))
		return true;
	if (!was_open && g_hooks.close_view)
		static_cast<void>(g_hooks.close_view("view.debug.patches"));
	return false;
}

bool parse_patch_bytes(const std::string& text, std::vector<std::uint8_t>& out) {
	out.clear();
	const char* cursor = text.c_str();
	while (*cursor != '\0') {
		while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
			++cursor;
		if (*cursor == '\0') break;
		if (cursor[1] == '\0' ||
			!std::isxdigit(static_cast<unsigned char>(cursor[0])) ||
			!std::isxdigit(static_cast<unsigned char>(cursor[1]))) {
			out.clear();
			return false;
		}
		char token[3] = {cursor[0], cursor[1], '\0'};
		out.push_back(
			static_cast<std::uint8_t>(std::strtoul(token, nullptr, 16)));
		cursor += 2;
		if (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
			*cursor != '\r' && *cursor != '\n') {
			out.clear();
			return false;
		}
		if (out.size() > 4096U) {
			out.clear();
			return false;
		}
	}
	return !out.empty();
}

bool stage_patch_review(std::uint64_t address, std::uint64_t extent,
	const std::string& description, std::string* error) {
	const auto context = debugger_interaction::capture(
		debugger_interaction::kind_t::instruction, address, 0, -1, 0, extent,
		description);
	return stage_patch_review(context, extent, description, error);
}

bool stage_patch_review(const debugger_interaction::context_t& expected_context,
	std::uint64_t extent, const std::string& description, std::string* error) {
	if (expected_context.address == 0) {
		if (error) *error = "The selected item has no usable address.";
		return false;
	}
	if ((expected_context.kind != debugger_interaction::kind_t::instruction &&
		expected_context.kind != debugger_interaction::kind_t::patch) ||
		!debugger_interaction::is_current(expected_context)) {
		if (error) *error =
			"The retained patch target, process identity, address, or debugger stop changed.";
		return false;
	}
	if (extent > 4096) {
		if (error) *error = "Patch review is limited to 4096 bytes per staged item.";
		return false;
	}
	patch_stage_review_t review;
	review.address = expected_context.address;
	review.extent = extent;
	review.description = description;
	review.context = expected_context;
	review.context.extent = extent;
	review.context.primary_text = description;
	review.exact = false;
	review.expected_before.clear();
	review.initial_bytes.clear();
	if (g_hooks.present_patch_stage)
		g_hooks.present_patch_stage(review);
	return true;
}

bool stage_exact_patch_review(std::uint64_t address,
	const std::vector<std::uint8_t>& expected_before,
	const std::vector<std::uint8_t>& reviewed_after,
	std::uint32_t expected_pid,
	const std::string& description, std::string* error) {
	if (expected_before.empty() || reviewed_after.empty() ||
		expected_before.size() != reviewed_after.size() || reviewed_after.size() > 4096U) {
		if (error) *error = "An exact patch review requires matching before/after ranges from 1 to 4096 bytes.";
		return false;
	}
	if (expected_pid == 0 || !driver_bridge::is_loaded() ||
		driver_bridge::attached_pid() != expected_pid) {
		if (error) *error = "The proposal process is no longer the attached live patch target.";
		return false;
	}
	const auto context = debugger_interaction::capture(
		debugger_interaction::kind_t::instruction, address, 0, -1, 0,
		reviewed_after.size(), description);
	if (context.target_pid != expected_pid) {
		if (error) *error = "The proposal process identity changed before patch review.";
		return false;
	}
	if (!stage_patch_review(context, reviewed_after.size(), description, error))
		return false;
	patch_stage_review_t review;
	review.address = context.address;
	review.extent = reviewed_after.size();
	review.description = description;
	review.context = context;
	review.context.extent = reviewed_after.size();
	review.context.primary_text = description;
	review.exact = true;
	review.expected_before = expected_before;
	review.initial_bytes = reviewed_after;
	if (g_hooks.present_patch_stage)
		g_hooks.present_patch_stage(review);
	return true;
}

bool stage_nop_review(std::uint64_t address, std::uint64_t extent,
	std::string* error) {
	const auto context = debugger_interaction::capture(
		debugger_interaction::kind_t::instruction, address, 0, -1, 0, extent,
		"Reviewed NOP fill");
	return stage_nop_review(context, extent, error);
}

bool stage_nop_review(const debugger_interaction::context_t& expected_context,
	std::uint64_t extent, std::string* error) {
	if (extent == 0 || extent > 4096) {
		if (error) *error = "NOP review requires a selected instruction range from 1 to 4096 bytes.";
		return false;
	}
	if ((expected_context.kind != debugger_interaction::kind_t::instruction &&
		expected_context.kind != debugger_interaction::kind_t::patch) ||
		!debugger_interaction::is_current(expected_context)) {
		if (error) *error =
			"The retained patch target, process identity, address, or debugger stop changed.";
		return false;
	}
	patch_stage_review_t review;
	review.address = expected_context.address;
	review.extent = extent;
	review.description = "Reviewed NOP fill";
	review.context = expected_context;
	review.context.extent = extent;
	review.context.primary_text = "Reviewed NOP fill";
	review.exact = false;
	review.expected_before.clear();
	review.initial_bytes.assign(static_cast<std::size_t>(extent), 0x90);
	if (g_hooks.present_patch_stage)
		g_hooks.present_patch_stage(review);
	return true;
}

bool stage_breakpoint_definition(
	const debugger_interaction::context_t& expected_context,
	breakpoint_definition_mode_t mode, std::string* error) {
	if (expected_context.kind != debugger_interaction::kind_t::breakpoint ||
		expected_context.address == 0) {
		if (error) *error = "The selected item has no usable address.";
		return false;
	}
	if (!debugger_interaction::is_current(expected_context)) {
		if (error) *error =
			"The target process identity or debugger stop changed before breakpoint review.";
		return false;
	}
	const auto capability = debugger_interaction::evaluate(
		debugger_interaction::capability_t::toggle_breakpoint, expected_context);
	if (!capability.enabled) {
		if (error) *error = capability.disabled_reason
			? capability.disabled_reason : "Breakpoint definition review is unavailable.";
		return false;
	}
	if (g_hooks.present_breakpoint_stage)
		g_hooks.present_breakpoint_stage(expected_context.address,
			static_cast<int>(mode), expected_context);
	if (error) error->clear();
	return true;
}

bool commit_patch_stage_review(const patch_stage_review_t& review,
	const std::vector<std::uint8_t>& bytes, const std::string& description,
	std::string* error) {
	const std::uint64_t address = review.address;
	const std::uint64_t extent = review.extent;
	const bool exact = review.exact;
	const auto expected_before = review.expected_before;
	auto context = review.context;
	context.extent = extent == 0
		? static_cast<std::uint64_t>(bytes.size()) : extent;
	context.primary_text = description;
	const auto parsed = bytes;
	const bool queued = queue_debugger_mutation("Capture patch rollback bytes",
		"debugger.patch_stage", context,
		[address, parsed, description, exact, context, expected_before]() mutable {
			mutation_result_t result;
			if (!debugger_interaction::is_current(context)) {
				result.detail =
					"The retained patch target changed before rollback bytes were captured.";
				return result;
			}
			const int index = exact
				? code_patcher::create_patch_exact(address, expected_before,
					parsed, context.target_pid,
					context.process_creation_time_100ns, description)
				: code_patcher::create_patch(address, parsed, description,
					context.target_pid, context.process_creation_time_100ns);
			result.ok = result.verified = index >= 0 &&
				debugger_interaction::is_current(context);
			bool discarded = true;
			if (index >= 0 && !result.verified)
				discarded = code_patcher::discard_inactive_patch_exact(
					index, address, parsed, context.target_pid,
					context.process_creation_time_100ns);
			if (!result.verified)
				result.detail = index < 0
					? "Unable to capture exact rollback bytes; no patch was staged."
					: discarded
					? "The retained patch target changed during rollback capture; the exact inactive definition was discarded."
					: "The retained patch target changed during rollback capture, and the exact inactive definition could not be discarded.";
			else {
				const bool posted = post_debugger_ui([index]() {
					if (g_hooks.focus_patch_row)
						g_hooks.focus_patch_row(index);
					if (g_hooks.open_or_focus)
						static_cast<void>(g_hooks.open_or_focus("view.debug.patches"));
				}, "patch_stage_selection");
				if (!posted) {
					const bool publication_discarded =
						code_patcher::discard_inactive_patch_exact(
							index, address, parsed, context.target_pid,
							context.process_creation_time_100ns);
					result.ok = result.verified = false;
					result.detail = publication_discarded
						? "Patch staging could not publish its reviewed definition; the exact inactive definition was discarded."
						: "Patch staging could not publish its reviewed definition, and the exact inactive definition could not be discarded.";
				}
			}
			return result;
		}, false);
	if (!queued && error)
		*error = "The debugger mutation queue rejected the patch staging";
	return queued;
}

bool queue_debugger_mutation(const char* label, const char* action,
	debugger_interaction::context_t context, mutation_operation_t operation,
	bool advance_generation) {
	bool expected = false;
	if (!g_target_mutation_pending.compare_exchange_strong(expected, true,
		std::memory_order_acq_rel)) {
		toast_notification::push("Another live-target mutation is still pending.",
			toast_notification::toast_type_t::warning);
		return false;
	}
	auto result = std::make_shared<mutation_result_t>();
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "debugger";
	submission.label = label;
	submission.thread_class = "debugger_target_mutation";
	submission.domain = aida::infra::executor::domain_t::feature_worker;
	submission.priority = 2;
	submission.target_pid = context.target_pid;
	submission.generation = context.stop_generation;
	submission.ui_access_policy = "post_completion_only";
	submission.failure_policy = "fail_closed";
	submission.body = [context, result,
		operation = std::move(operation), advance_generation]() mutable {
		if (context.target_pid != 0 && !debugger_interaction::is_current(context)) {
			result->detail = "The target or selected stop changed before the mutation started.";
		} else {
			try {
				*result = operation();
			} catch (const std::exception& exception) {
				result->detail = exception.what();
			} catch (...) {
				result->detail = "The debugger mutation failed with an unknown exception.";
			}
		}
		if (result->verified && context.target_pid != 0 &&
			!debugger_interaction::is_current(context)) {
			result->ok = false;
			result->verified = false;
			result->detail =
				"The mutation completed, but its target or stop generation changed before publication; the stale result was rejected.";
		}
		const bool posted = post_debugger_ui([context, result, advance_generation]() {
			if (result->verified && advance_generation)
				debugger_interaction::advance_stop_generation();
			const std::string message = result->verified ? "Debugger mutation verified."
				: result->detail.empty() ? "Debugger mutation failed or readback did not match."
					: result->detail;
			toast_notification::push(message,
				result->verified ? toast_notification::toast_type_t::success
					: toast_notification::toast_type_t::error);
			diag::log_tagged_critical_fmt("debugger_context",
				"async_mutation pid=%u generation=%llu address=0x%llx index=%d ok=%d verified=%d detail='%s'",
				static_cast<unsigned>(context.target_pid),
				static_cast<unsigned long long>(context.stop_generation),
				static_cast<unsigned long long>(context.address), context.index,
				result->ok ? 1 : 0, result->verified ? 1 : 0, result->detail.c_str());
			g_target_mutation_pending.store(false, std::memory_order_release);
		}, "mutation_completion");
		if (!posted) {
			if (result->verified && advance_generation)
				debugger_interaction::invalidate_stop_generation_async();
			g_target_mutation_pending.store(false, std::memory_order_release);
			throw std::runtime_error("Debugger mutation completion could not be published to the UI thread");
		}
		if (!result->verified)
			throw std::runtime_error(result->detail.empty()
				? "Debugger mutation failed or readback did not match" : result->detail);
	};
	const auto submitted = submit_owned_debugger_task(std::move(submission),
		"view.debug.cpu", action, label, false);
	if (!submitted.submitted) {
		g_target_mutation_pending.store(false, std::memory_order_release);
		toast_notification::push("The debugger mutation could not be admitted: " +
			submitted.reject_reason, toast_notification::toast_type_t::error);
		return false;
	}
	return true;
}

namespace {

struct patch_transaction_result_t {
	bool ok = false;
	bool verified = false;
	bool rollback_attempted = false;
	bool rollback_verified = false;
	std::string detail;
};

}

static bool same_patch_definition(const code_patcher::patch_entry_t& lhs,
	const code_patcher::patch_entry_t& rhs) {
	return lhs.address == rhs.address && lhs.original_bytes == rhs.original_bytes &&
		lhs.patched_bytes == rhs.patched_bytes && lhs.target_pid == rhs.target_pid &&
		lhs.target_process_creation_time_100ns ==
			rhs.target_process_creation_time_100ns;
}

static bool read_patch_bytes_exact(uint64_t address, const std::vector<uint8_t>& expected,
	const debugger_interaction::context_t& context) {
	if (expected.empty() || !debugger_interaction::is_current(context)) return false;
	std::vector<uint8_t> observed;
	const bool read = driver_bridge::read_memory(address, expected.size(), observed);
	return debugger_interaction::is_current(context) && read && observed == expected;
}

static patch_transaction_result_t transition_patch_exact(int index,
	const code_patcher::patch_entry_t& expected_patch, bool target_active,
	const debugger_interaction::context_t& context) {
	patch_transaction_result_t result;
	if (!debugger_interaction::is_current(context)) {
		result.detail = "The exact debugger target changed before the patch transaction.";
		return result;
	}
	std::lock_guard<std::mutex> lock(code_patcher::g_state.mtx);
	if (index < 0 || index >= static_cast<int>(code_patcher::g_state.patches.size()) ||
		!same_patch_definition(code_patcher::g_state.patches[static_cast<size_t>(index)],
			expected_patch)) {
		result.detail = "The selected patch changed before execution.";
		return result;
	}
	auto& patch = code_patcher::g_state.patches[static_cast<size_t>(index)];
	if (patch.target_pid != 0 && patch.target_pid != context.target_pid) {
		result.detail = "The patch belongs to a different target process.";
		return result;
	}
	if (patch.target_process_creation_time_100ns == 0 ||
		patch.target_process_creation_time_100ns !=
			context.process_creation_time_100ns) {
		result.detail = "The patch belongs to a different process creation identity.";
		return result;
	}
	const auto& source = target_active ? patch.original_bytes : patch.patched_bytes;
	const auto& destination = target_active ? patch.patched_bytes : patch.original_bytes;
	if (source.empty() || destination.empty() || source.size() != destination.size()) {
		result.detail = "The patch byte transaction is incomplete.";
		return result;
	}
	if (patch.active == target_active) {
		result.verified = read_patch_bytes_exact(patch.address, destination, context);
		result.ok = result.verified;
		if (!result.verified)
			result.detail = "The recorded patch state does not match live target memory.";
		return result;
	}
	if (!read_patch_bytes_exact(patch.address, source, context)) {
		result.detail = "The live bytes or exact target identity changed before the patch write.";
		return result;
	}
	if (!debugger_interaction::is_current(context)) {
		result.detail = "The exact debugger target changed immediately before the patch write.";
		return result;
	}
	const bool write_ok = driver_bridge::write_memory(patch.address, destination);
	const bool identity_after_write = debugger_interaction::is_current(context);
	if (write_ok && identity_after_write)
		result.verified = read_patch_bytes_exact(patch.address, destination, context);
	if (result.verified) {
		patch.active = target_active;
		code_patcher::publish_snapshot_locked();
		result.ok = true;
		return result;
	}
	result.detail = !identity_after_write
		? "The exact debugger target changed during the patch write; success cannot be verified."
		: "Patch write verification failed.";
	if (!debugger_interaction::is_current(context)) return result;
	result.rollback_attempted = true;
	const bool rollback_write = driver_bridge::write_memory(patch.address, source);
	const bool identity_after_rollback_write = debugger_interaction::is_current(context);
	if (rollback_write && identity_after_rollback_write)
		result.rollback_verified = read_patch_bytes_exact(patch.address, source, context);
	if (result.rollback_verified)
		result.detail += " The prior byte state was restored and verified.";
	else
		result.detail += " Restoration of the prior byte state could not be verified.";
	return result;
}

context_mutation_review_t review_context_mutation(
	context_mutation_t mutation) noexcept {
	context_mutation_review_t review;
	switch (mutation) {
		case context_mutation_t::set_instruction_pointer:
			review.scope = "the target instruction pointer";
			review.consequence = "Execution will resume from the selected address.";
			review.capability =
				debugger_interaction::capability_t::set_instruction_pointer;
			break;
		case context_mutation_t::terminate_thread:
			review.scope = "the selected target thread";
			review.consequence = "Termination can corrupt locks and process state.";
			review.capability =
				debugger_interaction::capability_t::terminate_thread;
			break;
		case context_mutation_t::close_handle:
			review.scope = "the selected target handle";
			review.consequence = "The target may fail if it still owns this resource.";
			review.capability =
				debugger_interaction::capability_t::close_handle;
			break;
		case context_mutation_t::apply_patch:
			review.scope = "the selected live-memory patch";
			review.consequence = "Patched bytes will be written and read back before success is reported.";
			review.capability =
				debugger_interaction::capability_t::apply_patch;
			break;
		case context_mutation_t::revert_patch:
			review.scope = "the selected live-memory patch";
			review.consequence = "Original bytes will be restored and read back before success is reported.";
			review.capability =
				debugger_interaction::capability_t::revert_patch;
			break;
		case context_mutation_t::revert_all_patches:
			review.scope = "all active live-memory patches";
			review.consequence = "Each original byte sequence will be restored and verified before completion is reported.";
			review.capability =
				debugger_interaction::capability_t::revert_patch;
			break;
		case context_mutation_t::remove_patch:
			review.scope = "the selected patch definition";
			review.consequence = "Any active patch will be verified reverted before its definition is removed.";
			review.capability =
				debugger_interaction::capability_t::remove_patch;
			break;
		case context_mutation_t::remove_watch:
			review.scope = "the selected watch definition";
			review.consequence = "The watch will be removed from the debugger workspace; target memory is not modified.";
			review.advances_generation = false;
			break;
		case context_mutation_t::remove_bookmark:
			review.scope = "the selected debugger bookmark";
			review.consequence = "The bookmark will be removed from the debugger workspace; target memory is not modified.";
			review.advances_generation = false;
			break;
	}
	return review;
}

bool execute_context_mutation(context_mutation_t mutation,
	const debugger_interaction::context_t& context) {
	const auto review = review_context_mutation(mutation);
	return queue_debugger_mutation("Apply reviewed debugger mutation",
		"debugger.context_mutation", context, [mutation, context]() {
		mutation_result_t result;
		switch (mutation) {
			case context_mutation_t::set_instruction_pointer:
				result.ok = debugger_engine::set_register("rip", context.address);
				debugger_engine::request_refresh(0);
				result.verified = result.ok &&
					debugger_engine::get_registers().rip == context.address;
				break;
			case context_mutation_t::terminate_thread:
				result.ok = driver_bridge::terminate_thread(context.thread_id, 0xDEADu);
				result.verified = result.ok;
				break;
			case context_mutation_t::close_handle:
				result.ok = driver_bridge::close_process_handle(context.target_pid, context.value);
				result.verified = result.ok;
				break;
			case context_mutation_t::apply_patch:
			case context_mutation_t::revert_patch:
			case context_mutation_t::remove_patch: {
				code_patcher::patch_entry_t patch;
				if (context.value == 0 || code_patcher::g_state.generation.load(
						std::memory_order_acquire) != context.value) {
					result.detail = "The patch set changed after review; reopen the action.";
					break;
				}
				{
					std::lock_guard<std::mutex> lock(code_patcher::g_state.mtx);
					if (context.index < 0 || context.index >=
						static_cast<int>(code_patcher::g_state.patches.size())) {
						result.detail = "The selected patch no longer exists.";
						break;
					}
					patch = code_patcher::g_state.patches[static_cast<size_t>(context.index)];
				}
				if (patch.address != context.address) {
					result.detail = "The selected patch changed before execution.";
					break;
				}
				const bool target_active = mutation == context_mutation_t::apply_patch;
				const auto transaction = transition_patch_exact(context.index, patch,
					target_active, context);
				result.ok = transaction.ok;
				result.verified = transaction.verified;
				result.detail = transaction.detail;
				if (mutation == context_mutation_t::remove_patch &&
					transaction.ok && transaction.verified) {
					std::lock_guard<std::mutex> lock(code_patcher::g_state.mtx);
					if (context.index < 0 || context.index >=
							static_cast<int>(code_patcher::g_state.patches.size()) ||
						!same_patch_definition(code_patcher::g_state.patches[
							static_cast<size_t>(context.index)], patch) ||
						code_patcher::g_state.patches[static_cast<size_t>(context.index)].active) {
						result.ok = result.verified = false;
						result.detail = "The patch definition changed before removal.";
					} else {
						code_patcher::g_state.patches.erase(code_patcher::g_state.patches.begin() +
							static_cast<std::vector<code_patcher::patch_entry_t>::difference_type>(
								context.index));
						code_patcher::publish_snapshot_locked();
					}
				}
				break;
			}
			case context_mutation_t::revert_all_patches: {
				std::vector<code_patcher::patch_entry_t> patches;
				if (context.value == 0 || code_patcher::g_state.generation.load(
						std::memory_order_acquire) != context.value) {
					result.detail = "The patch set changed after review; reopen Revert All.";
					break;
				}
				{
					std::lock_guard<std::mutex> lock(code_patcher::g_state.mtx);
					if (code_patcher::g_state.patches.size() > 65536U) {
						result.detail = "The patch set exceeds the 65,536-entry safety bound.";
						break;
					}
					patches = code_patcher::g_state.patches;
				}
				result.ok = result.verified = true;
				for (size_t remaining = patches.size(); remaining > 0; --remaining) {
					const size_t index = remaining - 1;
					if (!patches[index].active) continue;
					const auto transaction = transition_patch_exact(static_cast<int>(index),
						patches[index], false, context);
					if (!transaction.ok || !transaction.verified) {
						result.ok = result.verified = false;
						result.detail = transaction.detail.empty()
							? "A patch rollback could not be verified."
							: transaction.detail;
						break;
					}
				}
				break;
			}
			case context_mutation_t::remove_watch: {
				std::lock_guard<std::mutex> lock(debugger_engine::g_state.watch_mutex);
				if (context.index < 0 || context.index >=
					static_cast<int>(debugger_engine::g_state.watches.size())) break;
				const auto& watch = debugger_engine::g_state.watches[static_cast<size_t>(context.index)];
				const std::string& expression = watch.persistent_expression.empty()
					? watch.expression : watch.persistent_expression;
				if (expression != context.primary_text) break;
				debugger_engine::g_state.watches.erase(
					debugger_engine::g_state.watches.begin() + context.index);
				debugger_engine::g_state.watches_generation.fetch_add(1, std::memory_order_release);
				result.ok = result.verified = true;
				break;
			}
			case context_mutation_t::remove_bookmark: {
				std::lock_guard<std::mutex> lock(debugger_engine::g_state.anno_mutex);
				const auto found = std::find(debugger_engine::g_state.bookmarks.begin(),
					debugger_engine::g_state.bookmarks.end(), context.address);
				if (found == debugger_engine::g_state.bookmarks.end()) break;
				debugger_engine::g_state.bookmarks.erase(found);
				result.ok = result.verified = true;
				break;
			}
		}
		return result;
	}, review.advances_generation);
}

std::uint64_t breakpoint_fingerprint(
	const debugger_engine::breakpoint_t& breakpoint) noexcept {
	std::uint64_t hash = 1469598103934665603ULL;
	const auto mix = [&](std::uint64_t value) {
		for (unsigned shift = 0; shift < 64; shift += 8) {
			hash ^= static_cast<std::uint8_t>(value >> shift);
			hash *= 1099511628211ULL;
		}
	};
	mix(breakpoint.address);
	mix(static_cast<std::uint64_t>(breakpoint.size));
	mix(static_cast<std::uint64_t>(breakpoint.type));
	mix(breakpoint.auto_continue ? 1U : 0U);
	for (const char value : breakpoint.name) mix(static_cast<unsigned char>(value));
	for (const char value : breakpoint.condition) mix(static_cast<unsigned char>(value));
	for (const char value : breakpoint.log_text) mix(static_cast<unsigned char>(value));
	return hash;
}

namespace {

bool breakpoint_edit_identity_matches(const debugger_engine::breakpoint_t& breakpoint,
	std::uint64_t fingerprint, std::uint64_t address, std::uint64_t size, int type,
	const std::string& name, const std::string& condition, const std::string& log_text,
	bool auto_continue) {
	return breakpoint_fingerprint(breakpoint) == fingerprint &&
		breakpoint.address == address &&
		static_cast<std::uint64_t>(breakpoint.size) == size &&
		static_cast<int>(breakpoint.type) == type && breakpoint.name == name &&
		breakpoint.condition == condition && breakpoint.log_text == log_text &&
		breakpoint.auto_continue == auto_continue;
}

}

bool retain_breakpoint_edit(breakpoint_edit_state_t& state, int index,
	const debugger_engine::breakpoint_t& breakpoint,
	const debugger_interaction::context_t& context,
	breakpoint_editor_focus_t focus) {
	if (index < 0 || context.kind != debugger_interaction::kind_t::breakpoint ||
		context.index != index || context.address != breakpoint.address ||
		context.value != breakpoint_fingerprint(breakpoint) ||
		!debugger_interaction::is_current(context))
		return false;
	std::lock_guard<std::mutex> lock(debugger_engine::g_state.bp_mutex);
	if (!debugger_interaction::is_current(context) ||
		index >= static_cast<int>(debugger_engine::g_state.breakpoints.size()))
		return false;
	const auto& retained = debugger_engine::g_state.breakpoints[static_cast<std::size_t>(index)];
	if (retained.address != context.address ||
		breakpoint_fingerprint(retained) != context.value)
		return false;
	state.idx = index;
	state.context = context;
	state.breakpoints_generation = debugger_engine::g_state.breakpoints_generation.load(
		std::memory_order_acquire);
	state.fingerprint = breakpoint_fingerprint(retained);
	state.address = retained.address;
	state.size = static_cast<std::uint64_t>(retained.size);
	state.type = static_cast<int>(retained.type);
	state.name = retained.name;
	state.original_condition = retained.condition;
	state.original_log = retained.log_text;
	state.original_auto_continue = retained.auto_continue;
	state.identity_retained = true;
	state.focus = focus;
	return true;
}

bool breakpoint_edit_is_current(const breakpoint_edit_state_t& state,
	std::string& reason) {
	if (!state.identity_retained || state.idx < 0) {
		reason = "The reviewed breakpoint identity was not retained.";
		return false;
	}
	if (!debugger_interaction::is_current(state.context)) {
		reason = "The target process identity or debugger stop changed while the editor was open.";
		return false;
	}
	if (debugger_engine::g_state.breakpoints_generation.load(std::memory_order_acquire) !=
		state.breakpoints_generation) {
		reason = "The breakpoint collection changed while the editor was open.";
		return false;
	}
	std::unique_lock<std::mutex> lock(debugger_engine::g_state.bp_mutex, std::defer_lock);
	if (!lock.try_lock()) {
		reason = "Breakpoint state is updating; Apply is temporarily unavailable.";
		return false;
	}
	if (debugger_engine::g_state.breakpoints_generation.load(std::memory_order_acquire) !=
		state.breakpoints_generation ||
		state.idx >= static_cast<int>(debugger_engine::g_state.breakpoints.size()) ||
		!breakpoint_edit_identity_matches(
			debugger_engine::g_state.breakpoints[static_cast<std::size_t>(state.idx)],
			state.fingerprint, state.address, state.size, state.type,
			state.name, state.original_condition, state.original_log,
			state.original_auto_continue)) {
		reason = "The breakpoint was removed, replaced, or modified while the editor was open.";
		return false;
	}
	return true;
}

bool submit_breakpoint_edit(const breakpoint_edit_state_t& state,
	const std::string& condition, const std::string& log_text,
	bool auto_continue) {
	const int edit_index = state.idx;
	const auto context = state.context;
	const auto expected_generation = state.breakpoints_generation;
	const auto expected_fingerprint = state.fingerprint;
	const auto expected_address = state.address;
	const auto expected_size = state.size;
	const auto expected_type = state.type;
	const auto expected_name = state.name;
	const auto expected_condition = state.original_condition;
	const auto expected_log = state.original_log;
	const auto expected_auto_continue = state.original_auto_continue;
	return queue_debugger_mutation("Edit breakpoint",
		"debugger.breakpoint_edit", context,
		[edit_index, condition, log_text, auto_continue, context,
		 expected_generation, expected_fingerprint, expected_address,
		 expected_size, expected_type, expected_name, expected_condition,
		 expected_log, expected_auto_continue]() {
			mutation_result_t result;
			std::lock_guard<std::mutex> lock(debugger_engine::g_state.bp_mutex);
			if (!debugger_interaction::is_current(context) ||
				debugger_engine::g_state.breakpoints_generation.load(std::memory_order_acquire) !=
					expected_generation || edit_index < 0 ||
				edit_index >= static_cast<int>(debugger_engine::g_state.breakpoints.size()) ||
				!breakpoint_edit_identity_matches(
					debugger_engine::g_state.breakpoints[static_cast<std::size_t>(edit_index)],
					expected_fingerprint, expected_address, expected_size, expected_type,
					expected_name, expected_condition, expected_log,
					expected_auto_continue)) {
				result.detail = "Breakpoint edit rejected because the target, stop, index, or breakpoint identity changed.";
				return result;
			}
			auto& breakpoint = debugger_engine::g_state.breakpoints[static_cast<std::size_t>(edit_index)];
			breakpoint.condition = condition;
			breakpoint.log_text = log_text;
			breakpoint.auto_continue = auto_continue;
			debugger_engine::g_state.breakpoints_generation.fetch_add(1, std::memory_order_release);
			result.ok = result.verified = true;
			return result;
		});
}

context_retention_t context_item_retention(const debugger_interaction::context_t& context) {
	switch (context.kind) {
		case debugger_interaction::kind_t::register_value: {
			const auto registers = debugger_engine::cached_registers();
			return !context.primary_text.empty() &&
				resolve_register_token(context.primary_text, registers) == context.value
				? context_retention_t::current : context_retention_t::stale;
		}
		case debugger_interaction::kind_t::stack_slot: {
			std::uint64_t base = 0;
			const auto bytes = debugger_engine::cached_stack_bytes(base);
			if (context.address < base || context.address - base > bytes.size() ||
				bytes.size() - static_cast<std::size_t>(context.address - base) < sizeof(std::uint64_t))
				return context_retention_t::stale;
			std::uint64_t value = 0;
			std::memcpy(&value, bytes.data() + static_cast<std::size_t>(context.address - base),
				sizeof(value));
			return value == context.value ? context_retention_t::current : context_retention_t::stale;
		}
		case debugger_interaction::kind_t::breakpoint: {
			const auto breakpoints = debugger_engine::snapshot_breakpoints();
			if (context.index < 0 || context.index >= static_cast<int>(breakpoints.size()))
				return context_retention_t::stale;
			const auto& breakpoint = breakpoints[static_cast<std::size_t>(context.index)];
			return breakpoint.address == context.address &&
				breakpoint_fingerprint(breakpoint) == context.value
				? context_retention_t::current : context_retention_t::stale;
		}
		case debugger_interaction::kind_t::watch: {
			std::unique_lock<std::mutex> lock(debugger_engine::g_state.watch_mutex,
				std::try_to_lock);
			if (!lock.owns_lock())
				return context_retention_t::busy;
			if (context.index < 0 || context.index >= static_cast<int>(debugger_engine::g_state.watches.size()))
				return context_retention_t::stale;
			const auto& watch = debugger_engine::g_state.watches[static_cast<std::size_t>(context.index)];
			const std::string& expression = watch.persistent_expression.empty()
				? watch.expression : watch.persistent_expression;
			return expression == context.primary_text
				? context_retention_t::current : context_retention_t::stale;
		}
		case debugger_interaction::kind_t::string_value: {
			std::unique_lock<std::mutex> lock(debugger_engine::g_state.strings_mutex,
				std::try_to_lock);
			if (!lock.owns_lock())
				return context_retention_t::busy;
			const bool retained = std::any_of(debugger_engine::g_state.strings.begin(), debugger_engine::g_state.strings.end(),
				[&](const debugger_engine::string_ref_t& item) {
					return item.address == context.address && item.value == context.primary_text;
				});
			return retained ? context_retention_t::current : context_retention_t::stale;
		}
		case debugger_interaction::kind_t::bookmark: {
			std::unique_lock<std::mutex> lock(debugger_engine::g_state.anno_mutex,
				std::try_to_lock);
			if (!lock.owns_lock())
				return context_retention_t::busy;
			const bool retained = std::find(debugger_engine::g_state.bookmarks.begin(),
				debugger_engine::g_state.bookmarks.end(), context.address) !=
				debugger_engine::g_state.bookmarks.end();
			return retained ? context_retention_t::current : context_retention_t::stale;
		}
		case debugger_interaction::kind_t::patch: {
			if (context.value == 0 || code_patcher::g_state.generation.load(
					std::memory_order_acquire) != context.value)
				return context_retention_t::stale;
			if (context.index < 0)
				return context_retention_t::current;
			std::unique_lock<std::mutex> lock(code_patcher::g_state.mtx, std::try_to_lock);
			if (!lock.owns_lock())
				return context_retention_t::busy;
			if (context.index >= static_cast<int>(code_patcher::g_state.patches.size()))
				return context_retention_t::stale;
			const auto& patch = code_patcher::g_state.patches[static_cast<std::size_t>(context.index)];
			return patch.address == context.address &&
				patch.patched_bytes.size() == context.extent &&
				patch.description == context.primary_text
				? context_retention_t::current : context_retention_t::stale;
		}
		default:
			return context_retention_t::current;
	}
}

std::uint64_t parse_hex_address(const std::string& text) {
	const char* s = text.c_str();
	if (!s || !*s) return 0;
	while (*s == ' ' || *s == '\t') ++s;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
	uint64_t v = 0;
	for (; *s; ++s) {
		char c = *s;
		uint8_t d;
		if (c >= '0' && c <= '9') d = static_cast<uint8_t>(c - '0');
		else if (c >= 'a' && c <= 'f') d = static_cast<uint8_t>(10 + (c - 'a'));
		else if (c >= 'A' && c <= 'F') d = static_cast<uint8_t>(10 + (c - 'A'));
		else break;
		v = (v << 4) | d;
	}
	return v;
}

std::uint64_t resolve_register_token(const std::string& tok,
                                     const debugger_engine::register_set_t& r) {
	std::string n;
	n.reserve(tok.size());
	for (char c : tok) n.push_back(static_cast<char>(::toupper(static_cast<unsigned char>(c))));
	if (n == "RAX") return r.rax; if (n == "RBX") return r.rbx;
	if (n == "RCX") return r.rcx; if (n == "RDX") return r.rdx;
	if (n == "RSI") return r.rsi; if (n == "RDI") return r.rdi;
	if (n == "RBP") return r.rbp; if (n == "RSP") return r.rsp;
	if (n == "R8")  return r.r8;  if (n == "R9")  return r.r9;
	if (n == "R10") return r.r10; if (n == "R11") return r.r11;
	if (n == "R12") return r.r12; if (n == "R13") return r.r13;
	if (n == "R14") return r.r14; if (n == "R15") return r.r15;
	if (n == "RIP") return r.rip; if (n == "RFLAGS") return r.rflags;
	if (n == "CS")  return r.cs;  if (n == "SS")  return r.ss;
	if (n == "DS")  return r.ds;  if (n == "ES")  return r.es;
	if (n == "FS")  return r.fs;  if (n == "GS")  return r.gs;
	if (n == "DR0") return r.dr0; if (n == "DR1") return r.dr1;
	if (n == "DR2") return r.dr2; if (n == "DR3") return r.dr3;
	if (n == "DR6") return r.dr6; if (n == "DR7") return r.dr7;
	return 0;
}

std::uint64_t evaluate_watch_expression(const std::string& expr,
                                        const debugger_engine::register_set_t& r,
                                        bool& deref_out, bool& valid_out) {
	deref_out = false;
	valid_out = false;
	std::string s = expr;
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
	while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
	if (s.empty()) return 0;
	bool deref = false;
	if (s.front() == '[' && s.back() == ']') {
		deref = true;
		s = s.substr(1, s.size() - 2);
		while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
		while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
	}
	uint64_t total = 0;
	bool subtract = false;
	bool any_token = false;
	std::string cur;
	auto consume = [&]() {
		while (!cur.empty() && (cur.front() == ' ' || cur.front() == '\t')) cur.erase(cur.begin());
		while (!cur.empty() && (cur.back()  == ' ' || cur.back()  == '\t')) cur.pop_back();
		if (cur.empty()) return;
		uint64_t v = 0;
		bool numeric = (cur[0] == '0' && (cur.size() > 1 && (cur[1] == 'x' || cur[1] == 'X')));
		if (!numeric) {
			bool all_hex_digits = true;
			for (char c : cur) {
				if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
					all_hex_digits = false; break;
				}
			}
			if (all_hex_digits && cur.size() >= 4) numeric = true;
		}
		if (numeric) v = parse_hex_address(cur);
		else v = resolve_register_token(cur, r);
		if (subtract) total -= v;
		else          total += v;
		any_token = true;
		cur.clear();
	};
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (c == '+') { consume(); subtract = false; continue; }
		if (c == '-') { consume(); subtract = true;  continue; }
		cur.push_back(c);
	}
	consume();
	if (!any_token) return 0;
	deref_out = deref;
	valid_out = true;
	return total;
}

execution_capability_t address_mutation_capability(std::uint64_t address,
	bool toggle_breakpoint, std::uint32_t expected_pid) {
	const auto context = debugger_interaction::capture(
		debugger_interaction::kind_t::instruction, address);
	if (expected_pid == 0)
		return {false, "The analysis selection is not owned by a live process workspace"};
	if (context.target_pid != expected_pid)
		return {false, "Attach the debugger to the process that owns this analysis workspace"};
	return address_mutation_capability(context, toggle_breakpoint);
}

execution_capability_t address_mutation_capability(
	const debugger_interaction::context_t& expected_context,
	bool toggle_breakpoint) {
	if (g_target_mutation_pending.load(std::memory_order_acquire))
		return {false, "Another live-target mutation is still pending"};
	if ((expected_context.kind != debugger_interaction::kind_t::instruction &&
		expected_context.kind != debugger_interaction::kind_t::breakpoint) ||
		expected_context.address == 0 ||
		!debugger_interaction::is_current(expected_context))
		return {false,
			"The retained debugger target, process identity, address, or stop changed"};
	const auto result = debugger_interaction::evaluate(toggle_breakpoint
		? debugger_interaction::capability_t::toggle_breakpoint
		: debugger_interaction::capability_t::run_to_address, expected_context);
	return {result.enabled, result.disabled_reason};
}

bool queue_run_to_address(std::uint64_t address, std::uint32_t expected_pid,
	std::string* error) {
	const auto context = debugger_interaction::capture(
		debugger_interaction::kind_t::instruction, address);
	if (context.target_pid != expected_pid) {
		if (error) *error =
			"Attach the debugger to the process that owns this analysis workspace";
		return false;
	}
	return queue_run_to_address(context, error);
}

bool queue_run_to_address(const debugger_interaction::context_t& expected_context,
	std::string* error) {
	const auto capability = address_mutation_capability(expected_context, false);
	if (!capability.enabled) {
		if (error) *error = capability.disabled_reason
			? capability.disabled_reason : "Run to cursor is unavailable";
		return false;
	}
	const auto context = expected_context;
	const bool queued = queue_debugger_mutation("Run to cursor",
		"analysis.debug.run_to_cursor", context, [context]() {
			mutation_result_t result;
			if (!debugger_interaction::is_current(context)) {
				result.detail =
					"The retained Run to Cursor target changed before the final mutation.";
				return result;
			}
			result.ok = debugger_engine::run_to_address(context.address, false);
			result.verified = result.ok && debugger_interaction::is_current(context);
			if (!result.ok)
				result.detail = "The debugger engine rejected Run to Cursor.";
			else if (!result.verified)
				result.detail =
					"The Run to Cursor target changed before its postcondition was verified.";
			return result;
		});
	if (!queued && error) *error = "The debugger mutation queue rejected Run to Cursor";
	return queued;
}

bool queue_toggle_breakpoint(std::uint64_t address, std::uint32_t expected_pid,
	std::string* error) {
	const auto capability = address_mutation_capability(address, true, expected_pid);
	if (!capability.enabled) {
		if (error) *error = capability.disabled_reason
			? capability.disabled_reason : "Breakpoint toggle is unavailable";
		return false;
	}
	const auto context = debugger_interaction::capture(
		debugger_interaction::kind_t::instruction, address);
	return queue_toggle_breakpoint(context, error);
}

bool queue_toggle_breakpoint(const debugger_interaction::context_t& expected_context,
	std::string* error) {
	if ((expected_context.kind != debugger_interaction::kind_t::instruction &&
		expected_context.kind != debugger_interaction::kind_t::breakpoint) ||
		expected_context.address == 0 || !debugger_interaction::is_current(expected_context)) {
		if (error) *error =
			"The retained breakpoint target, process identity, address, or debugger stop changed.";
		return false;
	}
	const auto evaluated = debugger_interaction::evaluate(
		debugger_interaction::capability_t::toggle_breakpoint, expected_context);
	if (!evaluated.enabled) {
		if (error) *error = evaluated.disabled_reason
			? evaluated.disabled_reason : "Breakpoint toggle is unavailable";
		return false;
	}
	const auto context = expected_context;
	const bool queued = queue_debugger_mutation("Toggle breakpoint at analysis cursor",
		"analysis.debug.breakpoint", context, [context]() {
			mutation_result_t result;
			if (!debugger_interaction::is_current(context)) {
				result.detail =
					"The retained breakpoint target changed before the final mutation.";
				return result;
			}
			auto snapshot = debugger_engine::snapshot_breakpoints();
			int found = -1;
			for (std::size_t index = 0; index < snapshot.size(); ++index) {
				if (!snapshot[index].is_internal &&
					snapshot[index].address == context.address) {
					found = static_cast<int>(index);
					break;
				}
			}
			result.ok = found >= 0
				? debugger_engine::remove_breakpoint(found)
				: debugger_engine::add_breakpoint(context.address) >= 0;
			if (!result.ok) {
				result.detail = "The debugger engine rejected the retained breakpoint toggle.";
				return result;
			}
			if (!debugger_interaction::is_current(context)) {
				result.detail =
					"The breakpoint target changed before the final postcondition was verified.";
				return result;
			}
			const auto verified_snapshot = debugger_engine::snapshot_breakpoints();
			const bool present = std::any_of(verified_snapshot.begin(),
				verified_snapshot.end(), [context](const auto& breakpoint) {
					return !breakpoint.is_internal && breakpoint.address == context.address;
				});
			result.verified = found >= 0 ? !present : present;
			if (!result.verified)
				result.detail =
					"The retained breakpoint toggle did not satisfy its final postcondition.";
			return result;
		});
	if (!queued && error) *error = "The debugger mutation queue rejected the breakpoint toggle";
	return queued;
}

const char* debugger_status_label(debugger_engine::dbg_status_t status) noexcept {
	switch (status) {
		case debugger_engine::dbg_status_t::idle: return "Idle";
		case debugger_engine::dbg_status_t::running: return "Running";
		case debugger_engine::dbg_status_t::paused: return "Paused";
		case debugger_engine::dbg_status_t::stepping: return "Stepping";
		case debugger_engine::dbg_status_t::terminated: return "Terminated";
	}
	return "Unknown";
}

namespace {

aida::ui::action_handler_result_t invoke_debugger_entity_action(
	const std::string& action, const debugger_interaction::context_t& context,
	const std::vector<debugger_interaction::context_t>& action_contexts);

}

bool dispatch_patch_panel_command(patch_panel_command_t command, std::string* error) {
	const auto snapshot = code_patcher::published_snapshot();
	if (!snapshot) {
		if (error) *error = "The immutable Patches publication is unavailable";
		return false;
	}
	switch (command) {
		case patch_panel_command_t::stage: {
			const auto registers = debugger_engine::cached_registers();
			return stage_patch_review(registers.rip, 0, "Manual debugger patch", error);
		}
		case patch_panel_command_t::find_code_caves:
			if (g_hooks.present_code_caves)
				g_hooks.present_code_caves();
			return true;
		case patch_panel_command_t::revert_all:
			if (g_hooks.present_context_mutation_review)
				g_hooks.present_context_mutation_review(
					static_cast<int>(context_mutation_t::revert_all_patches),
					debugger_interaction::capture(debugger_interaction::kind_t::patch,
						0, snapshot->generation));
			return true;
		case patch_panel_command_t::save_patchset:
			return request_patchset_save(error);
	}
	if (error) *error = "Unknown Patches panel action";
	return false;
}

bool request_patchset_save(std::string* error) {
	if (!g_hooks.pick_save_file) {
		if (error) *error = "The save-file dialog is unavailable";
		return false;
	}
	const auto destination = g_hooks.pick_save_file("Save Patchset",
		"patches.json", "JSON (*.json);;Text (*.txt);;All files (*.*)");
	if (!destination)
		return true;
	const std::string path = *destination;
	auto cancelled = std::make_shared<std::atomic<bool>>(false);
	auto save_result = std::make_shared<mutation_result_t>();
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "debugger";
	submission.label = "Save patchset";
	submission.thread_class = "debugger_export";
	submission.domain = aida::infra::executor::domain_t::diagnostics;
	submission.priority = 3;
	submission.cancel_hook = [cancelled]() {
		cancelled->store(true, std::memory_order_release);
	};
	submission.ui_access_policy = "post_completion_only";
	submission.body = [path, cancelled, save_result]() {
		std::vector<code_patcher::patch_entry_t> patches;
		{
			std::lock_guard<std::mutex> lock(code_patcher::g_state.mtx);
			if (code_patcher::g_state.patches.size() > 4096U)
				save_result->detail = "Patchset exceeds the 4,096-entry export bound.";
			else
				patches = code_patcher::g_state.patches;
		}
		if (save_result->detail.empty() && patches.empty())
			save_result->detail = "No patches to save.";
		std::string encoded;
		if (save_result->detail.empty()) {
			encoded = "{\n  \"patches\": [\n";
			for (size_t i = 0; i < patches.size(); ++i) {
				if ((i & 0x3fU) == 0U && cancelled->load(std::memory_order_acquire)) {
					save_result->detail = "Patchset export cancelled.";
					break;
				}
				const auto& p = patches[i];
				char line[256];
				std::snprintf(line, sizeof(line),
					"    {\n      \"index\": %zu,\n"
					"      \"address\": \"0x%016llX\",\n"
					"      \"timestamp\": %lld,\n"
					"      \"active\": %s,\n"
					"      \"description\": \"",
					i, static_cast<unsigned long long>(p.address),
					static_cast<long long>(p.timestamp), p.active ? "true" : "false");
				encoded.append(line);
				for (char c : p.description) {
					if (c == '"' || c == '\\') encoded.push_back('\\');
					encoded.push_back(c == '\n' || c == '\r' ? ' ' : c);
				}
				encoded.append("\",\n      \"original\": \"");
				encoded.append(code_patcher::format_bytes(p.original_bytes));
				encoded.append("\",\n      \"patched\": \"");
				encoded.append(code_patcher::format_bytes(p.patched_bytes));
				encoded.append("\"\n    }");
				if (i + 1 < patches.size()) encoded.push_back(',');
				encoded.push_back('\n');
				if (encoded.size() > 16U * 1024U * 1024U) {
					save_result->detail = "Encoded patchset exceeds the 16 MiB export bound.";
					break;
				}
			}
			if (save_result->detail.empty()) encoded.append("  ]\n}\n");
		}
		if (save_result->detail.empty())
			save_result->ok = save_result->verified = write_file_atomic_exact(path,
				encoded.data(), encoded.size(), save_result->detail);
		const bool posted = post_debugger_ui([save_result, count = patches.size()]() {
			toast_notification::push(save_result->verified
				? "Saved " + std::to_string(count) + " patches." : save_result->detail,
				save_result->verified ? toast_notification::toast_type_t::success
					: toast_notification::toast_type_t::error);
		}, "patchset_save_completion");
		if (!posted)
			throw std::runtime_error("Patchset-export completion could not be published to the UI thread");
		if (!save_result->verified)
			throw std::runtime_error(save_result->detail.empty()
				? "Patchset export failed" : save_result->detail);
	};
	const auto submitted = submit_owned_debugger_task(std::move(submission),
		"view.debug.patches", "debugger.patchset_save", "Save patchset", true);
	if (!submitted.submitted) {
		if (error) *error = "Patchset export queue rejected the task: " + submitted.reject_reason;
		return false;
	}
	return true;
}

void dump_memory_region(const debugger_interaction::context_t& context) {
	const std::uint64_t capped_size = context.extent;
	if (capped_size > 256ULL * 1024ULL * 1024ULL) {
		toast_notification::push("Region exceeds 256 MiB dump cap.",
			toast_notification::toast_type_t::warning);
		return;
	}
	if (!g_hooks.pick_save_file)
		return;
	char default_name[96];
	std::snprintf(default_name, sizeof(default_name), "dump_%016llX_%llu.bin",
		static_cast<unsigned long long>(context.address),
		static_cast<unsigned long long>(capped_size));
	const auto destination = g_hooks.pick_save_file("Dump Region", default_name,
		"Binary (*.bin);;All files (*.*)");
	if (!destination)
		return;
	const std::string path = *destination;
	auto result = std::make_shared<mutation_result_t>();
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "debugger";
	submission.label = "Dump memory region";
	submission.thread_class = "debugger_export";
	submission.domain = aida::infra::executor::domain_t::feature_worker;
	submission.priority = 2;
	submission.target_pid = context.target_pid;
	submission.generation = context.stop_generation;
	submission.ui_access_policy = "post_completion_only";
	submission.failure_policy = "fail_closed";
	submission.body = [context, capped_size, path, result]() {
		if (driver_bridge::attached_pid() != context.target_pid ||
			debugger_interaction::current_stop_generation() != context.stop_generation) {
			result->detail = "The target changed before the region dump started.";
		} else {
			std::vector<uint8_t> bytes;
			result->ok = driver_bridge::read_memory(context.address,
				static_cast<size_t>(capped_size), bytes) && bytes.size() == capped_size;
			if (!result->ok)
				result->detail = "Region dump read failed or returned a partial result.";
			else
				result->verified = write_file_atomic_exact(path, bytes.data(),
					bytes.size(), result->detail);
		}
		const bool posted = post_debugger_ui([result]() {
			toast_notification::push(result->verified ? "Memory region dumped."
				: result->detail, result->verified
					? toast_notification::toast_type_t::success
					: toast_notification::toast_type_t::error);
		}, "region_dump_completion");
		if (!posted)
			throw std::runtime_error("Region-dump completion could not be published to the UI thread");
		if (!result->verified)
			throw std::runtime_error(result->detail.empty()
				? "Region dump failed" : result->detail);
	};
	const auto submitted = submit_owned_debugger_task(std::move(submission),
		"view.debug.memory_map", "debugger.dump_region", "Dump memory region", false);
	if (!submitted.submitted)
		toast_notification::push("Region dump queue rejected the task: " +
			submitted.reject_reason, toast_notification::toast_type_t::error);
}

void dump_module_bytes(std::uint64_t base, std::uint64_t size,
	const std::string& name) {
	if (!g_hooks.pick_save_file)
		return;
	char default_name[160] = {};
	std::snprintf(default_name, sizeof(default_name), "%s_%016llX.bin",
		name.empty() ? "module" : name.c_str(),
		static_cast<unsigned long long>(base));
	const auto destination = g_hooks.pick_save_file("Dump Module", default_name,
		"Binary (*.bin);;DLL (*.dll);;EXE (*.exe);;All files (*.*)");
	if (!destination)
		return;
	const std::string path_copy = *destination;
	const std::string name_copy = name;
	const std::uint32_t target_pid = driver_bridge::attached_pid();
	const std::uint64_t target_generation =
		debugger_interaction::current_stop_generation();
	auto result = std::make_shared<mutation_result_t>();
	aida::infra::executor::submission_t sub;
	sub.owner_subsystem = "debugger";
	sub.label = "Dump selected module";
	sub.thread_class = "debugger_dump";
	sub.domain = aida::infra::executor::domain_t::feature_worker;
	sub.priority = 2;
	sub.target_pid = target_pid;
	sub.generation = target_generation;
	sub.ui_access_policy = "post_completion_only";
	sub.failure_policy = "fail_closed";
	sub.body = [base, size, path_copy, name_copy, target_pid, target_generation,
		result]() {
		std::vector<uint8_t> buf;
		if (driver_bridge::attached_pid() != target_pid ||
			debugger_interaction::current_stop_generation() != target_generation)
			result->detail = "The target changed before the module dump started.";
		else {
			result->ok = driver_bridge::read_memory(base, static_cast<size_t>(size),
				buf) && buf.size() == size;
			if (!result->ok)
				result->detail = "Module dump read failed or returned a partial result.";
			else
				result->verified = write_file_atomic_exact(path_copy, buf.data(),
					buf.size(), result->detail);
		}
		diag::log_tagged_critical_fmt("modules",
			"modules_dump name='%s' base=0x%llx size=%llu read=%d write=%d path='%s'",
			name_copy.c_str(), static_cast<unsigned long long>(base),
			static_cast<unsigned long long>(size), result->ok ? 1 : 0,
			result->verified ? 1 : 0, path_copy.c_str());
		diag::log_tagged("dbg_audit", result->verified
			? "[dbg_audit] modules dump ok=1"
			: "[dbg_audit] modules dump fail reason=read_or_write_failed");
		const bool posted = post_debugger_ui([result, count = buf.size(), name_copy]() {
			const std::string message = result->verified
				? "Dumped " + std::to_string(count) + " bytes from " + name_copy
				: result->detail.empty() ? "Module dump failed." : result->detail;
			toast_notification::push(message, result->verified
				? toast_notification::toast_type_t::success
				: toast_notification::toast_type_t::error);
		}, "module_dump_completion");
		if (!posted)
			throw std::runtime_error("Module-dump completion could not be published to the UI thread");
		if (!result->verified)
			throw std::runtime_error(result->detail.empty()
				? "Module dump failed" : result->detail);
	};
	const auto submitted = submit_owned_debugger_task(std::move(sub),
		"view.debug.modules", "debugger.module_dump", "Dump selected module", false);
	if (!submitted.submitted) {
		diag::log_tagged_critical_fmt("modules",
			"modules_dump_post_failed name='%s' base=0x%llx size=%llu path='%s'",
			name_copy.c_str(), static_cast<unsigned long long>(base),
			static_cast<unsigned long long>(size), path_copy.c_str());
		toast_notification::push("Module dump queue rejected the task.",
			toast_notification::toast_type_t::error);
	}
}

std::shared_ptr<const code_cave_publication_view_t> code_cave_publication() {
	return std::atomic_load_explicit(&g_code_cave_publication,
		std::memory_order_acquire);
}

bool code_cave_search_pending() {
	return g_code_cave_search.pending.load(std::memory_order_acquire);
}

bool request_code_cave_search(const std::string& module_filter,
	std::size_t minimum_size, std::string* error) {
	if (g_code_cave_search.pending.load(std::memory_order_acquire)) return false;
	if (minimum_size == 0 || minimum_size > (1U << 20U)) {
		const std::string detail =
			"Code-cave minimum must be between 1 byte and 1 MiB.";
		g_code_cave_search.dialog_error = detail;
		if (error) *error = detail;
		toast_notification::push(detail, toast_notification::toast_type_t::warning);
		return false;
	}
	const std::uint32_t target_pid = driver_bridge::attached_pid();
	const std::uint64_t target_generation = debugger_interaction::current_stop_generation();
	if (target_pid == 0 || debugger_engine::g_state.status.load(
			std::memory_order_acquire) != debugger_engine::dbg_status_t::paused) {
		const std::string detail =
			"Attach a paused debugger target before searching for code caves.";
		g_code_cave_search.dialog_error = detail;
		if (error) *error = detail;
		return false;
	}
	g_code_cave_search.dialog_error.clear();
	g_code_cave_search.pending.store(true, std::memory_order_release);
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "debugger";
	submission.label = "Find code caves";
	submission.thread_class = "debugger_memory_scan";
	submission.domain = aida::infra::executor::domain_t::feature_worker;
	submission.priority = 3;
	submission.target_pid = target_pid;
	submission.generation = target_generation;
	submission.ui_access_policy = "post_completion_only";
	submission.failure_policy = "fail_closed";
	submission.body = [minimum_size, module_filter, target_pid, target_generation]() {
		struct pending_reset_t {
			~pending_reset_t() {
				g_code_cave_search.pending.store(false, std::memory_order_release);
			}
		} pending_reset;
		std::vector<code_cave_entry_t> results;
		std::string error;
		bool terminal_failure = false;
		try {
			if (driver_bridge::attached_pid() != target_pid ||
				debugger_engine::g_state.status.load(std::memory_order_acquire) !=
					debugger_engine::dbg_status_t::paused ||
				debugger_interaction::current_stop_generation() != target_generation) {
				error = "The target changed before code-cave discovery started.";
				terminal_failure = true;
			} else {
				auto modules = driver_bridge::enumerate_modules();
				std::uint64_t scan_bytes = 0;
				for (const auto& module : modules) {
					if (!module_filter.empty()) {
						std::string candidate = module.name;
						std::string needle = module_filter;
						std::transform(candidate.begin(), candidate.end(), candidate.begin(),
							[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
						std::transform(needle.begin(), needle.end(), needle.begin(),
							[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
						if (candidate.find(needle) == std::string::npos) continue;
					}
					constexpr std::uint64_t scan_limit = 512ULL * 1024ULL * 1024ULL;
					if (module.size > scan_limit - scan_bytes) {
						error = "Matching modules exceed the 512 MiB code-cave scan bound; narrow the module filter.";
						break;
					}
					scan_bytes += module.size;
					const auto caves = code_patcher::find_code_caves(module.base, module.size, minimum_size);
					for (const auto& cave : caves) {
						if (results.size() >= 10000U) {
							error = "Code-cave results were truncated at 10,000 entries.";
							break;
						}
						results.push_back({cave.address, static_cast<std::size_t>(cave.size), cave.module_name});
					}
					if (results.size() >= 10000U) break;
				}
			}
		} catch (const std::exception& exception) {
			error = std::string("Code-cave discovery failed: ") + exception.what();
			terminal_failure = true;
		} catch (...) {
			error = "Code-cave discovery failed with an unknown error.";
			terminal_failure = true;
		}
		if (driver_bridge::attached_pid() != target_pid ||
			debugger_engine::g_state.status.load(std::memory_order_acquire) !=
				debugger_engine::dbg_status_t::paused ||
			debugger_interaction::current_stop_generation() != target_generation) {
			results.clear();
			error = "The target changed before code-cave discovery completed.";
			terminal_failure = true;
		}
		try {
			auto publication = std::make_shared<code_cave_publication_view_t>();
			publication->generation = g_code_cave_publication_sequence.fetch_add(
				1, std::memory_order_acq_rel);
			publication->target_pid = target_pid;
			publication->target_stop_generation = target_generation;
			publication->results = std::move(results);
			publication->detail = error;
			std::atomic_store_explicit(&g_code_cave_publication,
				std::shared_ptr<const code_cave_publication_view_t>(std::move(publication)),
				std::memory_order_release);
		} catch (const std::exception& exception) {
			error = std::string("Code-cave result publication failed: ") + exception.what();
			terminal_failure = true;
		} catch (...) {
			error = "Code-cave result publication failed with an unknown error.";
			terminal_failure = true;
		}
		bool posted = false;
		try {
			posted = post_debugger_ui([detail = error]() {
				if (!detail.empty())
					toast_notification::push(detail, toast_notification::toast_type_t::warning);
			}, "code_cave_completion");
		} catch (const std::exception& exception) {
			error = std::string("Code-cave completion publication failed: ") + exception.what();
			terminal_failure = true;
		} catch (...) {
			error = "Code-cave completion publication failed with an unknown error.";
			terminal_failure = true;
		}
		if (!posted) {
			if (error.empty())
				error = "Code-cave completion could not be published to the UI thread.";
			terminal_failure = true;
		}
		if (terminal_failure)
			throw std::runtime_error(error.empty()
				? "Code-cave discovery failed without a diagnostic." : error);
	};
	const auto submitted = submit_owned_debugger_task(std::move(submission),
		"view.debug.patches", "debugger.code_caves", "Find code caves", false);
	if (!submitted.submitted) {
		g_code_cave_search.pending.store(false, std::memory_order_release);
		const std::string detail = "Code-cave search queue rejected the task: " +
			submitted.reject_reason;
		g_code_cave_search.dialog_error = detail;
		if (error) *error = detail;
		toast_notification::push(detail, toast_notification::toast_type_t::error);
		return false;
	}
	return true;
}

bool stage_code_cave_review(std::uint64_t publication_generation, int index,
	std::string* error) {
	const auto current = std::atomic_load_explicit(&g_code_cave_publication,
		std::memory_order_acquire);
	if (!current || current->generation != publication_generation ||
		current->target_pid == 0 ||
		driver_bridge::attached_pid() != current->target_pid ||
		debugger_interaction::current_stop_generation() !=
			current->target_stop_generation ||
		debugger_engine::g_state.status.load(std::memory_order_acquire) !=
			debugger_engine::dbg_status_t::paused ||
		index < 0 || index >= static_cast<int>(current->results.size())) {
		if (error) *error =
			"The exact process, stop generation, publication, or selected cave changed before staging.";
		return false;
	}
	const auto& cave = current->results[static_cast<std::size_t>(index)];
	if (!stage_patch_review(cave.address, cave.size,
			"Code cave patch in " + cave.module, error)) {
		if (error && error->empty())
			*error = "The exact code-cave patch review could not be staged.";
		return false;
	}
	return true;
}

namespace {

aida::ui::action_handler_result_t invoke_debugger_entity_action(
	const std::string& action, const debugger_interaction::context_t& context,
	const std::vector<debugger_interaction::context_t>& action_contexts) {
	using result_t = aida::ui::action_handler_result_t;
	const std::uint64_t memory_value =
		(context.kind == debugger_interaction::kind_t::register_value ||
			context.kind == debugger_interaction::kind_t::stack_slot)
			? context.value : context.address;
	const std::uint64_t navigation_address = memory_value;

	if (action == "debugger.entity.copy_address") {
		if (action_contexts.size() == 1) {
			copy_address_to_clipboard(
				context.address != 0 ? context.address : context.value);
		} else {
			std::ostringstream values;
			values << std::uppercase << std::hex << std::setfill('0');
			bool first = true;
			for (const auto& item : action_contexts) {
				const uint64_t value = item.address != 0 ? item.address : item.value;
				if (value == 0) continue;
				if (!first) values << '\n';
				values << "0x" << std::setw(16) << value;
				first = false;
			}
			copy_to_clipboard(values.str());
		}
		return result_t::completed();
	}
	if (action == "debugger.entity.copy_primary") {
		std::string values;
		for (const auto& item : action_contexts) {
			if (item.primary_text.empty()) continue;
			if (!values.empty()) values.push_back('\n');
			values.append(item.primary_text);
		}
		copy_to_clipboard(values);
		return result_t::completed();
	}
	if (action == "debugger.entity.copy_secondary") {
		std::string values;
		for (const auto& item : action_contexts) {
			if (item.secondary_text.empty()) continue;
			if (!values.empty()) values.push_back('\n');
			values.append(item.secondary_text);
		}
		copy_to_clipboard(values);
		return result_t::completed();
	}
	if (action == "debugger.entity.open_disassembly") {
		if (navigation_address != 0)
			jump_to_disasm(navigation_address);
		return result_t::completed();
	}
	if (action == "debugger.entity.open_hex") {
		if (memory_value != 0)
			jump_to_hex(memory_value,
				context.extent != 0 ? static_cast<size_t>(context.extent) : 256u);
		return result_t::completed();
	}
	if (action == "debugger.instruction.run_to") {
		static_cast<void>(queue_debugger_mutation("Run to address",
			"debugger.run_to_address", context, [context]() {
				mutation_result_t result;
				result.ok = result.verified =
					debugger_engine::run_to_address(context.address, false);
				return result;
			}));
		return result_t::completed();
	}
	if (action == "debugger.instruction.toggle_breakpoint") {
		static_cast<void>(queue_debugger_mutation("Toggle breakpoint",
			"debugger.breakpoint_toggle", context, [context]() {
				mutation_result_t result;
				auto snapshot = debugger_engine::snapshot_breakpoints();
				int found = -1;
				for (size_t i = 0; i < snapshot.size(); ++i)
					if (!snapshot[i].is_internal && snapshot[i].address == context.address) {
						found = static_cast<int>(i);
						break;
					}
				result.ok = result.verified = found >= 0
					? debugger_engine::remove_breakpoint(found)
					: debugger_engine::add_breakpoint(context.address) >= 0;
				return result;
			}));
		return result_t::completed();
	}
	if (action == "debugger.instruction.set_rip") {
		if (g_hooks.present_context_mutation_review)
			g_hooks.present_context_mutation_review(
				static_cast<int>(context_mutation_t::set_instruction_pointer),
				context);
		return result_t::completed();
	}
	if (action == "debugger.instruction.follow_branch") {
		jump_to_disasm(context.value);
		return result_t::completed();
	}
	if (action == "debugger.register.copy_decimal") {
		char decimal[32];
		std::snprintf(decimal, sizeof(decimal), "%llu",
			static_cast<unsigned long long>(context.value));
		copy_to_clipboard(decimal);
		return result_t::completed();
	}
	if (action == "debugger.register.edit") {
		if (g_hooks.present_register_edit)
			g_hooks.present_register_edit(context, context.primary_text,
				context.value);
		return result_t::completed();
	}
	if (action == "debugger.register.zero") {
		if (g_hooks.present_register_edit)
			g_hooks.present_register_edit(context, context.primary_text, 0);
		return result_t::completed();
	}
	if (action == "debugger.register.add_watch") {
		if (g_hooks.stage_watch_expression &&
			context.primary_text.size() < 96)
			g_hooks.stage_watch_expression(context.primary_text);
		return result_t::completed();
	}
	if (action == "debugger.stack.copy_qword") {
		copy_address_to_clipboard(context.value);
		return result_t::completed();
	}
	if (action == "debugger.stack.add_watch") {
		char expression[48]{};
		std::snprintf(expression, sizeof(expression), "[0x%016llX]",
			static_cast<unsigned long long>(context.address));
		if (g_hooks.stage_watch_expression)
			g_hooks.stage_watch_expression(expression);
		return result_t::completed();
	}
	if (action == "debugger.entity.interpret_structure") {
		if (!aida::qt::analysis::analysis_host_hooks_installed() ||
			!aida::qt::analysis::analysis_host_hooks().stage_dissector_target)
			return result_t::failed("The Structure Dissector is unavailable");
		aida::qt::analysis::staged_dissector_target_t staged;
		staged.address = context.value;
		staged.target_pid = context.target_pid;
		staged.target_epoch = context.stop_generation;
		staged.process_creation_time_100ns = context.process_creation_time_100ns;
		staged.source_generation = context.stop_generation;
		staged.live_process = true;
		staged.source_view = "view.debug.cpu";
		staged.source_identity = debugger_action_entity_id(context, {context});
		const auto epoch = staged.target_epoch;
		staged.validate = [context, epoch](std::string& reason) {
			const bool current = epoch == context.stop_generation &&
				context_item_retention(context) == context_retention_t::current &&
				debugger_interaction::is_current(context);
			if (!current)
				reason = "The retained register, target epoch, or debugger stop changed.";
			return current;
		};
		std::string error;
		if (!aida::qt::analysis::analysis_host_hooks().stage_dissector_target(
				std::move(staged), error))
			return result_t::failed(error);
		return result_t::completed();
	}
	if (action == "debugger.entity.pointer_workflow") {
		pointer_scanner::staged_target_context_t staged;
		staged.address = context.value;
		staged.target_pid = context.target_pid;
		staged.target_epoch = context.stop_generation;
		staged.process_creation_time_100ns = context.process_creation_time_100ns;
		staged.source_generation = context.stop_generation;
		staged.source_view = "view.debug.cpu";
		staged.source_identity = debugger_action_entity_id(context, {context});
		const auto epoch = staged.target_epoch;
		staged.validate = [context, epoch](std::string& reason) {
			const bool current = epoch == context.stop_generation &&
				context_item_retention(context) == context_retention_t::current &&
				debugger_interaction::is_current(context);
			if (!current)
				reason = "The retained register, target epoch, or debugger stop changed.";
			return current;
		};
		std::string error;
		if (!pointer_scanner::stage_target_context(std::move(staged), error))
			return result_t::failed(error);
		if (g_hooks.open_or_focus)
			static_cast<void>(g_hooks.open_or_focus("view.memory.pointers"));
		return result_t::completed();
	}
	if (action == "debugger.breakpoint.toggle_enabled") {
		static_cast<void>(queue_debugger_mutation("Toggle breakpoint state",
			"debugger.breakpoint_enable", context, [context]() {
				mutation_result_t result;
				result.ok = result.verified =
					debugger_engine::toggle_breakpoint(context.index);
				return result;
			}));
		return result_t::completed();
	}
	if (action == "debugger.breakpoint.edit" ||
		action == "debugger.breakpoint.condition" ||
		action == "debugger.breakpoint.log_message" ||
		action == "debugger.breakpoint.auto_continue") {
		const auto breakpoints = debugger_engine::snapshot_breakpoints();
		if (context.index >= 0 &&
			context.index < static_cast<int>(breakpoints.size()) &&
			breakpoints[static_cast<size_t>(context.index)].address ==
				context.address &&
			breakpoint_fingerprint(breakpoints[static_cast<size_t>(context.index)]) ==
				context.value) {
			const auto focus =
				action == "debugger.breakpoint.log_message"
					? breakpoint_editor_focus_t::log_message
					: action == "debugger.breakpoint.auto_continue"
						? breakpoint_editor_focus_t::auto_continue
						: breakpoint_editor_focus_t::condition;
			if (g_hooks.present_breakpoint_edit)
				g_hooks.present_breakpoint_edit(context, context.index,
					static_cast<int>(focus));
		}
		return result_t::completed();
	}
	if (action == "debugger.breakpoint.delete") {
		static_cast<void>(queue_debugger_mutation("Delete breakpoint",
			"debugger.breakpoint_delete", context, [context]() {
				mutation_result_t result;
				result.ok = result.verified =
					debugger_engine::remove_breakpoint(context.index);
				return result;
			}));
		return result_t::completed();
	}
	if (action == "debugger.memory.change_protection") {
		debugger_engine::memory_region_t region{};
		if (memory_map_view::find_region_by_base(context.address, region)) {
			if (g_hooks.present_change_protection)
				g_hooks.present_change_protection(context, region.base,
					region.size, region.protect);
		} else {
			toast_notification::push(
				"Memory map is updating; retry Change Protection in a moment.",
				toast_notification::toast_type_t::warning);
		}
		return result_t::completed();
	}
	if (action == "debugger.memory.dump") {
		dump_memory_region(context);
		return result_t::completed();
	}
	if (action == "debugger.thread.suspend") {
		static_cast<void>(queue_debugger_mutation("Suspend thread",
			"debugger.thread_suspend", context, [context]() {
				mutation_result_t result;
				result.ok = result.verified =
					driver_bridge::suspend_thread(context.thread_id, nullptr);
				return result;
			}));
		return result_t::completed();
	}
	if (action == "debugger.thread.resume") {
		static_cast<void>(queue_debugger_mutation("Resume thread",
			"debugger.thread_resume", context, [context]() {
				mutation_result_t result;
				result.ok = result.verified =
					driver_bridge::resume_thread(context.thread_id, nullptr);
				return result;
			}));
		return result_t::completed();
	}
	if (action == "debugger.thread.terminate") {
		if (g_hooks.present_context_mutation_review)
			g_hooks.present_context_mutation_review(
				static_cast<int>(context_mutation_t::terminate_thread), context);
		return result_t::completed();
	}
	if (action == "debugger.handle.close") {
		if (g_hooks.present_context_mutation_review)
			g_hooks.present_context_mutation_review(
				static_cast<int>(context_mutation_t::close_handle), context);
		return result_t::completed();
	}
	if (action == "debugger.patch.apply") {
		if (g_hooks.present_context_mutation_review)
			g_hooks.present_context_mutation_review(
				static_cast<int>(context_mutation_t::apply_patch), context);
		return result_t::completed();
	}
	if (action == "debugger.patch.revert") {
		if (g_hooks.present_context_mutation_review)
			g_hooks.present_context_mutation_review(
				static_cast<int>(context_mutation_t::revert_patch), context);
		return result_t::completed();
	}
	if (action == "debugger.patch.remove") {
		if (g_hooks.present_context_mutation_review)
			g_hooks.present_context_mutation_review(
				static_cast<int>(context_mutation_t::remove_patch), context);
		return result_t::completed();
	}
	if (action == "debugger.watch.remove") {
		if (g_hooks.present_context_mutation_review)
			g_hooks.present_context_mutation_review(
				static_cast<int>(context_mutation_t::remove_watch), context);
		return result_t::completed();
	}
	if (action == "debugger.bookmark.remove") {
		if (g_hooks.present_context_mutation_review)
			g_hooks.present_context_mutation_review(
				static_cast<int>(context_mutation_t::remove_bookmark), context);
		return result_t::completed();
	}
	return result_t::failed("The retained debugger entity did not provide this operation");
}

}

aida::ui::application_ui::retained_entity_context_t
	build_debugger_entity_actions(const debugger_interaction::context_t& context,
		bool include_selected_set) {
	aida::ui::application_ui::retained_entity_context_t retained;
	const auto action_contexts = include_selected_set
		? debugger_action_contexts(context)
		: std::vector<debugger_interaction::context_t>{context};
	const bool multiple = action_contexts.size() > 1;
	retained.owner_id = "debugger.entity";
	retained.entity_id = debugger_action_entity_id(context, action_contexts);
	retained.entity_generation = context.stop_generation;
	const char* owner_view = "view.debug.cpu";
	switch (context.kind) {
		case debugger_interaction::kind_t::breakpoint: owner_view = "view.debug.breakpoints"; break;
		case debugger_interaction::kind_t::memory_region: owner_view = "view.debug.memory_map"; break;
		case debugger_interaction::kind_t::stack_frame: owner_view = "view.debug.call_stack"; break;
		case debugger_interaction::kind_t::thread: owner_view = "view.debug.threads"; break;
		case debugger_interaction::kind_t::module: owner_view = "view.debug.modules"; break;
		case debugger_interaction::kind_t::trace_record: owner_view = "view.debug.trace"; break;
		case debugger_interaction::kind_t::handle: owner_view = "view.debug.handles"; break;
		case debugger_interaction::kind_t::patch: owner_view = "view.debug.patches"; break;
		case debugger_interaction::kind_t::watch: owner_view = "view.debug.watches"; break;
		case debugger_interaction::kind_t::string_value: owner_view = "view.debug.strings"; break;
		case debugger_interaction::kind_t::bookmark: owner_view = "view.debug.bookmarks"; break;
		default: break;
	}
	retained.active_view = aida::ui::stable_view_id_t(owner_view);
	retained.validate_identity = [action_contexts]() {
		for (const auto& item : action_contexts) {
			const auto retention = context_item_retention(item);
			if (retention == context_retention_t::busy)
				return aida::ui::capability_state_t::unavailable("Debugger state is updating; retry when the refresh completes.");
			if (retention != context_retention_t::current || !debugger_interaction::is_current(item))
				return aida::ui::capability_state_t::unavailable("The selected debugger entity or stop generation changed.");
		}
		return aida::ui::capability_state_t::available();
	};
	auto add = [&](const char* id, bool enabled, const char* reason) {
		const std::string action_id(id);
		retained.actions.push_back({id, enabled ? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(reason),
			[action_id, context, action_contexts]() {
				return invoke_debugger_entity_action(action_id, context,
					action_contexts);
			}});
	};
	auto add_handler = [&](const char* id, bool enabled, const char* reason,
		std::function<aida::ui::action_handler_result_t()> handler) {
		retained.actions.push_back({id, enabled ? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(reason), std::move(handler)});
	};
	auto add_capability = [&](const char* id, debugger_interaction::capability_t requested) {
		if (multiple) {
			add(id, false, "This action requires exactly one debugger row.");
			return;
		}
		const auto evaluated = debugger_interaction::evaluate(requested, context);
		add(id, evaluated.enabled, evaluated.disabled_reason ? evaluated.disabled_reason : "The debugger action is unavailable.");
	};
	const bool any_address = std::any_of(action_contexts.begin(), action_contexts.end(),
		[](const auto& item) { return item.address != 0 || item.value != 0; });
	const bool any_primary = std::any_of(action_contexts.begin(), action_contexts.end(),
		[](const auto& item) { return !item.primary_text.empty(); });
	const bool any_secondary = std::any_of(action_contexts.begin(), action_contexts.end(),
		[](const auto& item) { return !item.secondary_text.empty(); });
	add("debugger.entity.copy_address", any_address,
		"The selected item has no resolved address.");
	if (any_primary) add("debugger.entity.copy_primary", true, "");
	add_capability("debugger.entity.open_disassembly", debugger_interaction::capability_t::follow_disassembly);
	add_capability("debugger.entity.open_hex", debugger_interaction::capability_t::follow_memory);
	switch (context.kind) {
		case debugger_interaction::kind_t::instruction:
			add_capability("debugger.instruction.run_to", debugger_interaction::capability_t::run_to_address);
			add_capability("debugger.instruction.toggle_breakpoint", debugger_interaction::capability_t::toggle_breakpoint);
			add_capability("debugger.instruction.set_rip", debugger_interaction::capability_t::set_instruction_pointer);
			if (context.value != 0) add("debugger.instruction.follow_branch", !multiple,
				"Following a branch requires exactly one debugger row.");
			break;
		case debugger_interaction::kind_t::register_value:
			add("debugger.register.copy_decimal", !multiple,
				"Decimal copy requires exactly one debugger row.");
			add_capability("debugger.register.edit", debugger_interaction::capability_t::edit_register);
			add_capability("debugger.register.zero", debugger_interaction::capability_t::edit_register);
			add("debugger.register.add_watch", !multiple && !context.primary_text.empty(),
				"Adding a watch requires one retained register with a stable name.");
			add("debugger.entity.interpret_structure", !multiple && context.value != 0,
				"Structure interpretation requires one retained nonzero register value.");
			add("debugger.entity.pointer_workflow", !multiple && context.value != 0,
				"Pointer scanning requires one retained nonzero register value.");
			break;
		case debugger_interaction::kind_t::stack_slot:
			add("debugger.stack.copy_qword", !multiple,
				"Qword copy requires exactly one debugger row.");
			add("debugger.stack.add_watch", !multiple && context.address != 0,
				"Adding a watch requires one retained stack address.");
			add("debugger.entity.interpret_structure", !multiple && context.value != 0,
				"Structure interpretation requires one retained nonzero stack value.");
			add("debugger.entity.pointer_workflow", !multiple && context.value != 0,
				"Pointer scanning requires one retained nonzero stack value.");
			break;
		case debugger_interaction::kind_t::breakpoint:
			add_capability("debugger.breakpoint.toggle_enabled", debugger_interaction::capability_t::toggle_breakpoint);
			add_capability("debugger.breakpoint.edit", debugger_interaction::capability_t::edit_breakpoint);
			add_capability("debugger.breakpoint.condition", debugger_interaction::capability_t::edit_breakpoint);
			add_capability("debugger.breakpoint.log_message", debugger_interaction::capability_t::edit_breakpoint);
			add_capability("debugger.breakpoint.auto_continue", debugger_interaction::capability_t::edit_breakpoint);
			add_capability("debugger.breakpoint.delete", debugger_interaction::capability_t::remove_breakpoint);
			break;
		case debugger_interaction::kind_t::memory_region:
			add_capability("debugger.memory.change_protection", debugger_interaction::capability_t::change_memory_protection);
			add_capability("debugger.memory.dump", debugger_interaction::capability_t::dump_memory); break;
		case debugger_interaction::kind_t::thread:
			add_capability("debugger.thread.suspend", debugger_interaction::capability_t::suspend_thread);
			add_capability("debugger.thread.resume", debugger_interaction::capability_t::resume_thread);
			add_capability("debugger.thread.terminate", debugger_interaction::capability_t::terminate_thread); break;
		case debugger_interaction::kind_t::handle:
			add_capability("debugger.handle.close", debugger_interaction::capability_t::close_handle); break;
		case debugger_interaction::kind_t::patch:
			add_capability("debugger.patch.apply", debugger_interaction::capability_t::apply_patch);
			add_capability("debugger.patch.revert", debugger_interaction::capability_t::revert_patch);
			add_capability("debugger.patch.remove", debugger_interaction::capability_t::remove_patch); break;
		case debugger_interaction::kind_t::module:
			add_capability("debugger.module.unload", debugger_interaction::capability_t::unload_module); break;
		case debugger_interaction::kind_t::watch:
			if (any_secondary) add("debugger.entity.copy_secondary", true, "");
			add("debugger.watch.remove", !multiple,
				"Removing a watch requires exactly one debugger row."); break;
		case debugger_interaction::kind_t::string_value:
			if (any_secondary) add("debugger.entity.copy_secondary", true, ""); break;
		case debugger_interaction::kind_t::bookmark:
			add("debugger.bookmark.remove", !multiple,
				"Removing a bookmark requires exactly one debugger row."); break;
		default: break;
	}
	const char* evidence_kind = "debugger_entity";
	switch (context.kind) {
		case debugger_interaction::kind_t::instruction: evidence_kind = "debugger_instruction"; break;
		case debugger_interaction::kind_t::register_value: evidence_kind = "debugger_register"; break;
		case debugger_interaction::kind_t::stack_slot: evidence_kind = "debugger_stack_slot"; break;
		case debugger_interaction::kind_t::breakpoint: evidence_kind = "debugger_breakpoint"; break;
		case debugger_interaction::kind_t::memory_region: evidence_kind = "debugger_memory_region"; break;
		case debugger_interaction::kind_t::stack_frame: evidence_kind = "debugger_call_stack_frame"; break;
		case debugger_interaction::kind_t::thread: evidence_kind = "debugger_thread"; break;
		case debugger_interaction::kind_t::module: evidence_kind = "debugger_module"; break;
		case debugger_interaction::kind_t::trace_record: evidence_kind = "debugger_trace_record"; break;
		case debugger_interaction::kind_t::handle: evidence_kind = "debugger_handle"; break;
		case debugger_interaction::kind_t::patch: evidence_kind = "debugger_patch"; break;
		case debugger_interaction::kind_t::watch: evidence_kind = "debugger_watch"; break;
		case debugger_interaction::kind_t::string_value: evidence_kind = "debugger_string"; break;
		case debugger_interaction::kind_t::bookmark: evidence_kind = "debugger_bookmark"; break;
		default: break;
	}
	char address_text[24]{};
	char value_text[24]{};
	std::snprintf(address_text, sizeof(address_text), "0x%016llX",
		static_cast<unsigned long long>(context.address));
	std::snprintf(value_text, sizeof(value_text), "0x%016llX",
		static_cast<unsigned long long>(context.value));
	aida::automation_ui::entity_evidence::snapshot_t evidence;
	evidence.workspace_id = "pid:" + std::to_string(context.target_pid) + ":created:" +
		std::to_string(context.process_creation_time_100ns);
	evidence.source_view_id = owner_view;
	evidence.source_kind = evidence_kind;
	evidence.entity_id = retained.entity_id;
	evidence.display_label = context.primary_text.empty() ? evidence_kind : context.primary_text;
	evidence.excerpt = "PID: " + std::to_string(context.target_pid) +
		"\nProcess creation: " + std::to_string(context.process_creation_time_100ns) +
		"\nStop generation: " + std::to_string(context.stop_generation) +
		"\nSelected rows: " + std::to_string(action_contexts.size()) +
		"\nThread: " + std::to_string(context.thread_id) +
		"\nAddress: " + address_text + "\nValue: " + value_text +
		"\nExtent: " + std::to_string(context.extent) +
		"\nPrimary: " + context.primary_text +
		"\nSecondary: " + context.secondary_text;
	evidence.address = context.address;
	evidence.revision = context.stop_generation;
	evidence.generation = context.stop_generation;
	evidence.sensitive = true;
	const std::size_t evidence_rows = (std::min)(action_contexts.size(), std::size_t{256});
	for (std::size_t index = 0; index < evidence_rows; ++index) {
		const auto& item = action_contexts[index];
		char item_address[24]{};
		char item_value[24]{};
		std::snprintf(item_address, sizeof(item_address), "0x%016llX",
			static_cast<unsigned long long>(item.address));
		std::snprintf(item_value, sizeof(item_value), "0x%016llX",
			static_cast<unsigned long long>(item.value));
		evidence.excerpt += "\n[" + std::to_string(index + 1) + "] Address: " +
			item_address + " Value: " + item_value + " Primary: " + item.primary_text;
	}
	if (evidence_rows < action_contexts.size())
		evidence.excerpt += "\n... " + std::to_string(action_contexts.size() - evidence_rows) +
			" additional selected rows omitted from the excerpt.";
	evidence.return_to_source = [context, action_contexts, owner_view](std::string& reason) {
		for (const auto& item : action_contexts) {
			if (context_item_retention(item) != context_retention_t::current ||
				!debugger_interaction::is_current(item)) {
				reason = "The debugger target or stop generation changed; capture the entity again.";
				return false;
			}
		}
		debugger_interaction::select_set(action_contexts, context);
		if (g_hooks.open_or_focus) {
			const auto opened = g_hooks.open_or_focus(owner_view);
			if (!opened.ok()) {
				reason = opened.detail;
				return false;
			}
		}
		reason.clear();
		return true;
	};
	aida::automation_ui::entity_evidence::append_actions(retained, std::move(evidence),
		context.target_pid != 0 && context.kind != debugger_interaction::kind_t::none
			? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(
				"A retained stopped debugger target entity is required for evidence handoff."));
	return retained;
}

namespace {

aida::ui::action_handler_result_t invoke_source_breakpoint_action(
	const std::string& action, const source_debug_service::definition_t& definition) {
	using result_t = aida::ui::action_handler_result_t;
	std::string error;
	if (action == "debugger.source.open") {
		return source_debug_service::request_open_source(definition.file_path,
			definition.line, &error)
			? result_t::completed() : result_t::failed(error.empty()
				? "The source breakpoint location could not be opened" : error);
	}
	if (action == "debugger.source.open_disassembly") {
		if (definition.locations.empty())
			return result_t::failed("The source breakpoint has no bound address.");
		const auto workspace = disasm_view::capture_selected_workspace();
		if (!workspace) {
			toast_notification::push("Open the matching analysis workspace first.",
				toast_notification::toast_type_t::warning);
			return result_t::failed("Open the matching analysis workspace first.");
		}
		disasm_view::goto_address(definition.locations.front().address, workspace);
		if (g_hooks.open_or_focus)
			static_cast<void>(g_hooks.open_or_focus("document.disassembly"));
		return result_t::completed();
	}
	if (action == "debugger.source.copy_location") {
		copy_to_clipboard(definition.file_path + ":" +
			std::to_string(definition.line));
		return result_t::completed();
	}
	if (action == "debugger.source.copy_address") {
		if (definition.locations.empty())
			return result_t::failed("The source breakpoint has no bound address.");
		copy_address_to_clipboard(definition.locations.front().address);
		return result_t::completed();
	}
	if (action == "debugger.source.rebind_all") {
		return source_debug_service::request_rebind(&error)
			? result_t::completed() : result_t::failed(error.empty()
				? "Source rebinding was rejected" : error);
	}
	if (action == "debugger.source.remove") {
		return source_debug_service::request_remove(definition.id, &error)
			? result_t::completed() : result_t::failed(error.empty()
				? "The source breakpoint removal was rejected" : error);
	}
	return result_t::failed("The source breakpoint did not provide this operation");
}

}

aida::ui::application_ui::retained_entity_context_t
	build_source_breakpoint_actions(
		const source_debug_service::definition_t& definition,
		std::uint64_t publication_generation) {
	aida::ui::application_ui::retained_entity_context_t retained;
	retained.owner_id = "debugger.source.breakpoint";
	retained.entity_id = definition.id;
	retained.entity_generation = publication_generation;
	retained.active_view = aida::ui::stable_view_id_t("view.debug.source");
	retained.validate_identity = [id = definition.id, generation = publication_generation]() {
		const auto current = source_debug_service::snapshot();
		if (!current || current->generation != generation)
			return aida::ui::capability_state_t::unavailable("The source-debug publication changed; reopen the menu.");
		const bool exists = std::any_of(current->definitions.begin(), current->definitions.end(),
			[&](const auto& item) { return item.id == id; });
		return exists ? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable("The source breakpoint was removed.");
	};
	auto add = [&](const char* id, bool enabled, const char* reason) {
		const std::string action_id(id);
		retained.actions.push_back({id, enabled ? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(reason),
			[action_id, definition]() {
				return invoke_source_breakpoint_action(action_id, definition);
			}});
	};
	const auto published = source_debug_service::snapshot();
	const bool operation_pending = published && published->operation_pending;
	add("debugger.source.open", !definition.file_path.empty(), "The source breakpoint has no file path.");
	add("debugger.source.open_disassembly", !definition.locations.empty(), "The source breakpoint has no bound address.");
	add("debugger.source.copy_location", !definition.file_path.empty(), "The source breakpoint has no file path.");
	add("debugger.source.copy_address", !definition.locations.empty(), "The source breakpoint has no bound address.");
	add("debugger.source.rebind_all", !operation_pending, "A source-debug operation is already running.");
	add("debugger.source.remove", !operation_pending, "A source-debug operation is already running.");
	return retained;
}

}


