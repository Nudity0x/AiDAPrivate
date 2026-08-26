#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "debugger_interaction_context.hpp"
#include "debugger_engine.hpp"

namespace aida::ui {
struct view_operation_result_t;
namespace application_ui {
struct retained_entity_context_t;
}
}
namespace aida::infra::executor {
struct submission_t;
struct submit_result_t;
}
namespace source_debug_service {
struct definition_t;
}

namespace debugger_view {

enum class sub_tab_t : int {
	cpu = 0,
	breakpoints,
	memory_map,
	call_stack,
	threads,
	watches,
	handles,
	trace_log,
	strings,
	bookmarks,
	modules,
	patches,
	seh_chain,
	cfg,
	COUNT
};

struct ui_state_t {
	sub_tab_t active_tab = sub_tab_t::cpu;
	sub_tab_t prev_tab = sub_tab_t::cpu;
};

inline ui_state_t g_ui;

bool is_visible_sub_tab(sub_tab_t tab);
int visible_sub_tab_count();

enum class execution_command_t : std::uint8_t {
	launch,
	run_continue,
	pause,
	step_over,
	step_into,
	step_out,
	stop,
	restart,
	detach,
	toggle_breakpoint_at_instruction_pointer
};

enum class patch_panel_command_t : std::uint8_t {
	stage,
	find_code_caves,
	revert_all,
	save_patchset
};

enum class breakpoint_definition_mode_t : std::uint8_t {
	software,
	hardware_execute
};

enum class breakpoint_editor_focus_t : std::uint8_t {
	condition,
	log_message,
	auto_continue
};

struct execution_capability_t {
	bool enabled = false;
	const char* disabled_reason = nullptr;
};

enum class context_retention_t : std::uint8_t {
	current,
	stale,
	busy
};

struct mutation_result_t {
	bool ok = false;
	bool verified = false;
	std::string detail;
};

using mutation_operation_t = std::function<mutation_result_t()>;

enum class context_mutation_t : std::uint8_t {
	set_instruction_pointer,
	terminate_thread,
	close_handle,
	apply_patch,
	revert_patch,
	revert_all_patches,
	remove_patch,
	remove_watch,
	remove_bookmark
};

struct context_mutation_review_t {
	const char* scope = "the selected debugger item";
	const char* consequence = "This changes live target state.";
	debugger_interaction::capability_t capability =
		debugger_interaction::capability_t::copy;
	bool advances_generation = true;
};

struct breakpoint_edit_state_t {
	int idx = -1;
	debugger_interaction::context_t context;
	std::uint64_t breakpoints_generation = 0;
	std::uint64_t fingerprint = 0;
	std::uint64_t address = 0;
	std::uint64_t size = 0;
	int type = 0;
	std::string name;
	std::string original_condition;
	std::string original_log;
	bool original_auto_continue = false;
	bool identity_retained = false;
	breakpoint_editor_focus_t focus = breakpoint_editor_focus_t::condition;
};

struct patch_stage_review_t {
	debugger_interaction::context_t context;
	std::uint64_t address = 0;
	std::uint64_t extent = 0;
	std::string description;
	bool exact = false;
	std::vector<std::uint8_t> expected_before;
	std::vector<std::uint8_t> initial_bytes;
};

struct code_cave_entry_t {
	std::uint64_t address = 0;
	std::size_t size = 0;
	std::string module;
};

struct code_cave_publication_view_t {
	std::uint64_t generation = 0;
	std::uint32_t target_pid = 0;
	std::uint64_t target_stop_generation = 0;
	std::vector<code_cave_entry_t> results;
	std::string detail;
};

// Host-UI seam installed by the Qt domain (install_debugger_domain). The
// backend never includes Qt headers; every presentation/detail that needs the
// shell routes through these hooks. All hooks run on the GUI thread.
struct host_ui_hooks_t {
	std::function<aida::ui::view_operation_result_t(const char* view_id)>
		open_or_focus;
	std::function<bool(const char* view_id)> is_open;
	std::function<aida::ui::view_operation_result_t(const char* view_id)>
		close_view;
	std::function<void(const std::string& text)> clipboard_set;
	std::function<void()> request_spawn_dialog;
	std::function<void(const std::string& executable_path)>
		request_spawn_dialog_with_path;
	std::function<bool()> spawn_dialog_open;
	std::function<void(const debugger_interaction::context_t& context,
		const std::string& register_name, std::uint64_t value)>
		present_register_edit;
	std::function<void(const debugger_interaction::context_t& context,
		int index, int focus)> present_breakpoint_edit;
	std::function<void(const debugger_interaction::context_t& context,
		std::uint64_t address, std::uint64_t size, std::uint32_t old_protect)>
		present_change_protection;
	std::function<void(int mutation,
		const debugger_interaction::context_t& context)>
		present_context_mutation_review;
	std::function<void(const patch_stage_review_t& review)> present_patch_stage;
	std::function<void()> present_code_caves;
	std::function<std::optional<std::string>(const std::string& title,
		const std::string& default_name, const std::string& filter)>
		pick_save_file;
	std::function<void(const std::string& expression)> stage_watch_expression;
	std::function<void(int patch_index)> focus_patch_row;
	std::function<void(std::uint64_t address, int mode,
		const debugger_interaction::context_t& context)>
		present_breakpoint_stage;
};

void install_host_ui_hooks(host_ui_hooks_t hooks);
bool host_ui_hooks_installed() noexcept;

// Task admission (verbatim port: Task Center ownership registration gates the
// worker body's admission).
bool register_debugger_task(
	const aida::infra::executor::submit_result_t& submitted,
	const char* owner_view, const char* owner_action, const char* label,
	bool cancellable = false);
aida::infra::executor::submit_result_t submit_owned_debugger_task(
	aida::infra::executor::submission_t submission, const char* owner_view,
	const char* owner_action, const char* label, bool cancellable = false);

// Execution commands (CAS single-flight, worker, verbatim completion toasts).
execution_capability_t execution_capability(execution_command_t command);
bool execute_command(execution_command_t command, std::string* error = nullptr);
bool execution_command_pending() noexcept;
bool target_mutation_pending() noexcept;

// Mutation pipeline (CAS + stop-generation fencing + readback verification).
bool queue_debugger_mutation(const char* label, const char* action,
	debugger_interaction::context_t context, mutation_operation_t operation,
	bool advance_generation = true);

// Patch panel commands.
execution_capability_t patch_panel_capability(patch_panel_command_t command);
bool execute_patch_panel_command(patch_panel_command_t command,
	std::string* error = nullptr);
bool dispatch_patch_panel_command(patch_panel_command_t command,
	std::string* error = nullptr);

// Patch staging (entry points called cross-domain; the review is presented by
// the installed hook).
bool stage_patch_review(std::uint64_t address, std::uint64_t extent,
	const std::string& description, std::string* error = nullptr);
bool stage_patch_review(const debugger_interaction::context_t& expected_context,
	std::uint64_t extent, const std::string& description,
	std::string* error = nullptr);
bool stage_exact_patch_review(std::uint64_t address,
	const std::vector<std::uint8_t>& expected_before,
	const std::vector<std::uint8_t>& reviewed_after,
	std::uint32_t expected_pid, const std::string& description,
	std::string* error = nullptr);
bool stage_nop_review(std::uint64_t address, std::uint64_t extent,
	std::string* error = nullptr);
bool stage_nop_review(const debugger_interaction::context_t& expected_context,
	std::uint64_t extent, std::string* error = nullptr);
bool stage_breakpoint_definition(
	const debugger_interaction::context_t& expected_context,
	breakpoint_definition_mode_t mode, std::string* error = nullptr);

// Patch review commit (the queued rollback-byte capture; the dialog calls this
// on Apply).
bool parse_patch_bytes(const std::string& text, std::vector<std::uint8_t>& out);
bool commit_patch_stage_review(const patch_stage_review_t& review,
	const std::vector<std::uint8_t>& bytes, const std::string& description,
	std::string* error = nullptr);

// Address mutations (analysis-side entry points; retained identity checked).
execution_capability_t address_mutation_capability(std::uint64_t address,
	bool toggle_breakpoint, std::uint32_t expected_pid = 0);
execution_capability_t address_mutation_capability(
	const debugger_interaction::context_t& expected_context,
	bool toggle_breakpoint);
bool queue_run_to_address(std::uint64_t address, std::uint32_t expected_pid,
	std::string* error = nullptr);
bool queue_run_to_address(
	const debugger_interaction::context_t& expected_context,
	std::string* error = nullptr);
bool queue_toggle_breakpoint(std::uint64_t address, std::uint32_t expected_pid,
	std::string* error = nullptr);
bool queue_toggle_breakpoint(
	const debugger_interaction::context_t& expected_context,
	std::string* error = nullptr);

// Reviewed context mutations (confirm-dialog flow): the static review table +
// the verbatim queued bodies.
context_mutation_review_t review_context_mutation(
	context_mutation_t mutation) noexcept;
bool execute_context_mutation(context_mutation_t mutation,
	const debugger_interaction::context_t& context);

// Breakpoint identity retention (verbatim index + fingerprint +
// breakpoints_generation re-check under bp_mutex with try_lock busy reason).
std::uint64_t breakpoint_fingerprint(
	const debugger_engine::breakpoint_t& breakpoint) noexcept;
bool retain_breakpoint_edit(breakpoint_edit_state_t& state, int index,
	const debugger_engine::breakpoint_t& breakpoint,
	const debugger_interaction::context_t& context,
	breakpoint_editor_focus_t focus);
bool breakpoint_edit_is_current(const breakpoint_edit_state_t& state,
	std::string& reason);
bool submit_breakpoint_edit(const breakpoint_edit_state_t& state,
	const std::string& condition, const std::string& log_text,
	bool auto_continue);

// Retention evaluation for the confirm dialogs (per-kind staleness/busy).
context_retention_t context_item_retention(
	const debugger_interaction::context_t& context);

// Entity actions (the retained contract + the real invoke dispatch; the menu
// bridge executes the invoke handlers on the GUI thread).
aida::ui::application_ui::retained_entity_context_t
	build_debugger_entity_actions(const debugger_interaction::context_t& context,
		bool include_selected_set = true);
aida::ui::application_ui::retained_entity_context_t
	build_source_breakpoint_actions(
		const source_debug_service::definition_t& definition,
		std::uint64_t publication_generation);

// Navigation + clipboard helpers (hook-driven).
bool jump_to_disasm(std::uint64_t address);
bool jump_to_hex(std::uint64_t address, std::size_t bytes);
void copy_to_clipboard(const std::string& text);
void copy_address_to_clipboard(std::uint64_t address);

// Status strip helpers (shared with the Qt status strip).
const char* debugger_status_label(debugger_engine::dbg_status_t status) noexcept;

// Export/dump workers (the file pick routes through the hook).
bool write_file_atomic_exact(const std::string& destination,
	const void* bytes, std::size_t size, std::string& error);
void dump_memory_region(const debugger_interaction::context_t& context);
void dump_module_bytes(std::uint64_t base, std::uint64_t size,
	const std::string& name);
bool request_patchset_save(std::string* error);

// Code caves (single-flight pending + immutable publication).
std::shared_ptr<const code_cave_publication_view_t> code_cave_publication();
bool code_cave_search_pending();
bool request_code_cave_search(const std::string& module_filter,
	std::size_t minimum_size, std::string* error);
bool stage_code_cave_review(std::uint64_t publication_generation, int index,
	std::string* error);

// Watch expression display helper (verbatim evaluator for the Resolved Address
// column).
std::uint64_t evaluate_watch_expression(const std::string& expression,
	const debugger_engine::register_set_t& registers, bool& deref_out,
	bool& valid_out);
std::uint64_t parse_hex_address(const std::string& text);
std::uint64_t resolve_register_token(const std::string& token,
	const debugger_engine::register_set_t& registers);

}
