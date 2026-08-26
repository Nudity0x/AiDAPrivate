#include "globals.h"
#include "../qt/chrome/aida_legacy_chrome_bridge.hpp"
#include "diag_log.hpp"
#include "win32_dialog.hpp"
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <fstream>
#include <filesystem>
#include <array>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <functional>
#include <thread>
#include <chrono>
#include <cctype>
#include <exception>
#include <utility>
#include <nlohmann/json.hpp>
#include "standalone_chat.hpp"
#include "standalone_settings.hpp"
#include "disasm_view.hpp"
#include "pseudocode_view.hpp"
#include "../core/workbench/workbench_shell_integration.hpp"
#include "analysis_session.hpp"
#include "../core/settings/settings_persistence_service.hpp"
#include "../core/ai/conversation_evidence_store.hpp"
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <shared_mutex>

static bool trusted_show_save_file(HWND owner,
	const char* title,
	const char* filter_pairs,
	const char* default_ext,
	char* out_path,
	size_t out_path_capacity,
	const char* caller_name)
{
	return win32_dialog::show_save_file_dialog(owner, title, filter_pairs, default_ext,
		out_path, out_path_capacity, caller_name);
}





namespace {

file_tabs::save_result_t chrome_bridge_save_document_as(int index)
{
	if (!file_tabs::is_valid_tab_index(index))
		return {false, "The document is no longer open."};
	char buf[MAX_PATH] = {};
	const auto& tab = file_tabs::tabs[file_tabs::tab_index(index)];
	if (!tab.filename.empty())
		strncpy_s(buf, tab.filename.c_str(), _TRUNCATE);
	static const char k_save_as_filter[] = "All files (*.*)\0*.*\0\0";
	if (!trusted_show_save_file(nullptr, "Save As", k_save_as_filter, nullptr,
		buf, sizeof(buf), "file_menu_save_as"))
		return {false, "Save As was canceled."};
	return file_tabs::save_tab_as(index, buf);
}

void chrome_bridge_resolve_saved(int ci, std::uint64_t document_id,
	std::uint64_t revision, bool save_succeeded, const std::string& detail)
{
	if (save_succeeded) {
		file_tabs::pending_close_idx = -1;
		file_tabs::close_confirm_error.clear();
		if (file_tabs::is_valid_tab_index(ci) &&
			file_tabs::tabs[file_tabs::tab_index(ci)].save_in_progress) {
			file_tabs::pending_close_after_save_document_id = document_id;
		} else if (file_tabs::exit_review_requested) {
			file_tabs::resolve_exit_review_document(document_id, revision, false);
		} else {
			file_tabs::close_tab(ci);
			file_tabs::finish_close_all_document(document_id);
			file_tabs::advance_close_all();
		}
	} else {
		file_tabs::close_confirm_error = detail;
	}
}

void chrome_bridge_save_current()
{
	const int ci = file_tabs::pending_close_idx;
	if (!file_tabs::is_valid_tab_index(ci))
		return;
	const std::uint64_t closing_document = file_tabs::tabs[file_tabs::tab_index(ci)].document_id;
	const std::uint64_t closing_revision = file_tabs::tabs[file_tabs::tab_index(ci)].revision;
	const file_tabs::save_result_t saved =
		file_tabs::tabs[file_tabs::tab_index(ci)].filepath.empty()
			? chrome_bridge_save_document_as(ci)
			: file_tabs::save_tab_to_disk_result(ci);
	chrome_bridge_resolve_saved(ci, closing_document, closing_revision,
		saved.succeeded, saved.detail);
}

void chrome_bridge_save_current_as(const std::string& destination)
{
	const int ci = file_tabs::pending_close_idx;
	if (!file_tabs::is_valid_tab_index(ci) || destination.empty())
		return;
	const std::uint64_t closing_document = file_tabs::tabs[file_tabs::tab_index(ci)].document_id;
	const std::uint64_t closing_revision = file_tabs::tabs[file_tabs::tab_index(ci)].revision;
	const auto saved = file_tabs::save_tab_as(ci, destination);
	chrome_bridge_resolve_saved(ci, closing_document, closing_revision,
		saved.succeeded, saved.detail);
}

void chrome_bridge_discard_current()
{
	const int ci = file_tabs::pending_close_idx;
	const std::uint64_t closing_document = file_tabs::is_valid_tab_index(ci)
		? file_tabs::tabs[file_tabs::tab_index(ci)].document_id : 0;
	const std::uint64_t closing_revision = file_tabs::is_valid_tab_index(ci)
		? file_tabs::tabs[file_tabs::tab_index(ci)].revision : 0;
	if (!file_tabs::exit_review_requested && file_tabs::is_valid_tab_index(ci))
		file_tabs::close_tab(ci, true);
	file_tabs::pending_close_idx = -1;
	file_tabs::close_confirm_error.clear();
	if (file_tabs::exit_review_requested)
		file_tabs::resolve_exit_review_document(closing_document, closing_revision, true);
	else {
		file_tabs::finish_close_all_document(closing_document);
		file_tabs::advance_close_all();
	}
}

void chrome_bridge_cancel()
{
	file_tabs::pending_close_idx = -1;
	file_tabs::close_confirm_error.clear();
	file_tabs::cancel_close_all();
}

aida::qt::chrome::exit_review_snapshot_t chrome_bridge_poll_exit_review()
{
	using aida::qt::chrome::exit_review_snapshot_t;
	if (file_tabs::pending_close_after_save_document_id != 0)
		file_tabs::resolve_pending_close_after_save();
	else if (!file_tabs::pending_close_all_document_ids.empty())
		file_tabs::advance_close_all();
	file_tabs::poll_exit_review();

	exit_review_snapshot_t snapshot;
	static std::uint64_t generation = 0;
	snapshot.generation = ++generation;
	snapshot.review_active = file_tabs::exit_review_requested;
	snapshot.dialog_active = file_tabs::is_valid_tab_index(file_tabs::pending_close_idx);
	snapshot.close_error = file_tabs::close_confirm_error;
	if (snapshot.dialog_active) {
		const int ci = file_tabs::pending_close_idx;
		const auto& tab = file_tabs::tabs[file_tabs::tab_index(ci)];
		snapshot.current.document_id = tab.document_id;
		snapshot.current.revision = tab.revision;
		snapshot.current.filename = tab.filename;
		snapshot.current.filepath_empty = tab.filepath.empty();
		snapshot.current.target_current = true;
		const auto gate = file_tabs::verify_tab_save_gate(ci, false);
		snapshot.current.save_disabled = !gate.succeeded;
		snapshot.current.save_gate_detail = gate.detail;
		snapshot.current.close_disabled = file_tabs::close_operation_pending(tab);
	}
	for (const std::uint64_t document_id : file_tabs::pending_close_all_document_ids) {
		std::string name = "Document " + std::to_string(document_id);
		for (const auto& tab : file_tabs::tabs) {
			if (tab.document_id == document_id && !tab.filename.empty()) {
				name = tab.filename;
				break;
			}
		}
		snapshot.queue_names.push_back(std::move(name));
	}
	return snapshot;
}

bool chrome_bridge_quick_open_poll(std::string& query_out)
{
	static bool was_open = false;
	const bool now_open = globals::ui::quick_open_open;
	if (now_open == was_open)
		return false;
	was_open = now_open;
	if (!now_open)
		return false;
	query_out.assign(globals::ui::quick_open_buf);
	return true;
}

void chrome_bridge_focus_view(const std::string& view_id)
{
	auto& hooks = aida::qt::chrome::legacy_chrome_hooks();
	if (hooks.focus_view) {
		hooks.focus_view(view_id);
		return;
	}
	diag::log_tagged_fmt("chrome_bridge", "focus_view_no_qt_sink view=%s", view_id.c_str());
}

const aida::workbench::document_persistence_dto_t* chrome_bridge_workbench_document(
	const aida::workbench::workbench_persistence_dto_t& state,
	aida::workbench::document_id_t document)
{
	const auto found = std::find_if(state.documents.begin(), state.documents.end(),
		[document](const auto& candidate) { return candidate.id == document; });
	return found == state.documents.end() ? nullptr : &*found;
}

aida::ui::capability_state_t chrome_bridge_decompile_capability()
{
	const auto active_workspace_handle = analysis_session::active_workspace();
	const auto active_workspace_context = disasm_view::capture_workspace(active_workspace_handle);
	if (active_workspace_handle &&
		active_workspace_handle->identity().target_kind() ==
			aida::analysis::target_kind_t::static_file) {
		aida::workbench::workbench_shell_workspace_context_t context;
		if (!aida::workbench::workbench_shell_runtime_t::instance()
				.workspace_context(active_workspace_handle, context) ||
			!context.pseudocode_document)
			return aida::ui::capability_state_t::unavailable(
				"The active Workbench has no pseudocode provider");
		const auto* active = chrome_bridge_workbench_document(context.persistence,
			context.persistence.active_document);
		if (!active)
			return aida::ui::capability_state_t::unavailable(
				"Select an analysis document and address first");
		if (active->local_state.selection.has_address ||
			(active->identity.kind == aida::workbench::document_kind_t::pseudocode &&
			 active->identity.has_address))
			return aida::ui::capability_state_t::available();
		const auto& encoded = active->identity.provider_key != "analysis"
			? active->identity.provider_key
			: active->local_state.selection.entity_key;
		const auto parsed = aida::workbench::pseudocode_document::
			parse_pseudocode_entity_locator(encoded);
		const auto canonical = parsed ? aida::workbench::pseudocode_document::
			canonical_pseudocode_entity_locator(*parsed) : std::nullopt;
		return canonical && *canonical == encoded
			? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(
				"Select an analysis address or managed entity first");
	}
	return pseudocode_view::has_active_tab(active_workspace_context)
		? aida::ui::capability_state_t::available()
		: aida::ui::capability_state_t::unavailable(
			"Open a binary with an available Pseudocode document first");
}

aida::ui::action_handler_result_t chrome_bridge_decompile_or_focus()
{
	const auto active_workspace_handle = analysis_session::active_workspace();
	const auto active_workspace_context = disasm_view::capture_workspace(active_workspace_handle);
	if (active_workspace_handle &&
		active_workspace_handle->identity().target_kind() ==
			aida::analysis::target_kind_t::static_file) {
		aida::workbench::workbench_shell_workspace_context_t context;
		const auto loaded = aida::workbench::workbench_shell_runtime_t::instance()
			.workspace_context(active_workspace_handle, context);
		const auto* active = loaded ? chrome_bridge_workbench_document(context.persistence,
			context.persistence.active_document) : nullptr;
		const auto address = active && active->local_state.selection.has_address
			? active->local_state.selection.address
			: active && active->identity.kind ==
				aida::workbench::document_kind_t::pseudocode && active->identity.has_address
				? active->identity.address : 0;
		std::optional<aida::analysis::decompiler_entity_locator_t> managed_locator;
		std::string managed_identity;
		if (active) {
			const auto& encoded = active->identity.provider_key != "analysis"
				? active->identity.provider_key
				: active->local_state.selection.entity_key;
			const auto parsed = aida::workbench::pseudocode_document::
				parse_pseudocode_entity_locator(encoded);
			const auto canonical = parsed ? aida::workbench::pseudocode_document::
				canonical_pseudocode_entity_locator(*parsed) : std::nullopt;
			if (canonical && *canonical == encoded) {
				managed_locator = *parsed;
				managed_identity = *canonical;
			}
		}
		if ((address == 0 && !managed_locator) || !context.pseudocode_document)
			return aida::ui::action_handler_result_t::failed(
				"The active Workbench selection cannot be decompiled");
		aida::workbench::workbench_shell_workspace_context_t activated;
		aida::workbench::workbench_error_t opened;
		if (managed_locator) {
			opened = aida::workbench::workbench_shell_runtime_t::instance()
				.activate_entity_document(active_workspace_handle,
					aida::workbench::document_kind_t::pseudocode,
					managed_identity, activated);
		} else {
			const auto document_address = active && active->identity.kind ==
				aida::workbench::document_kind_t::pseudocode && active->identity.has_address
				? active->identity.address : address;
			opened = aida::workbench::workbench_shell_runtime_t::instance()
				.activate_document(active_workspace_handle,
					aida::workbench::document_kind_t::pseudocode,
					document_address, activated);
		}
		if (!opened || !activated.pseudocode_document)
			return aida::ui::action_handler_result_t::failed(
				"The Pseudocode document could not be activated");
		aida::analysis::decompiler_entity_locator_t locator;
		if (managed_locator) {
			locator = *managed_locator;
		} else {
			locator.address = address;
		}
		std::uint64_t ticket = 0;
		const auto submitted = activated.pseudocode_document->request_async(
			locator, aida::analysis::decompiler_profile_id_t::balanced,
			aida::workbench::pseudocode_document::k_pseudocode_document_default_timeout_ms,
			false, ticket);
		if (submitted) {
			diag::log_tagged_fmt("ui", "workbench_f5 address=0x%llX managed=%d async=1 ticket=%llu",
				static_cast<unsigned long long>(address), managed_locator ? 1 : 0,
				static_cast<unsigned long long>(ticket));
			return aida::ui::action_handler_result_t::completed();
		}
		return aida::ui::action_handler_result_t::failed(
			"The decompiler rejected the active selection");
	}
	if (pseudocode_view::has_active_tab(active_workspace_context)) {
		chrome_bridge_focus_view("document.pseudocode");
		diag::log_tagged("ui", "view_switch to=pseudocode hotkey=F5");
		return aida::ui::action_handler_result_t::completed();
	}
	return aida::ui::action_handler_result_t::failed(
		"No Pseudocode document is available for the active workspace");
}

}

namespace aida::qt::chrome {

void bind_legacy_chrome_hooks()
{
	auto& hooks = legacy_chrome_hooks();
	hooks.quick_open.poll_open_request = [](std::string& query_out) {
		return chrome_bridge_quick_open_poll(query_out);
	};
	hooks.quick_open.mark_closed = [] {
		globals::ui::quick_open_open = false;
		globals::ui::quick_open_buf[0] = '\0';
	};
	hooks.quick_open.close_command_palette = [] {
		globals::ui::command_palette_open = false;
	};
	hooks.exit_review.poll = [] { return chrome_bridge_poll_exit_review(); };
	hooks.exit_review.save_current = [] { chrome_bridge_save_current(); };
	hooks.exit_review.save_current_as = [](const std::string& destination) {
		chrome_bridge_save_current_as(destination);
	};
	hooks.exit_review.discard_current = [] { chrome_bridge_discard_current(); };
	hooks.exit_review.cancel = [] { chrome_bridge_cancel(); };
	hooks.exit_review.set_close_error = [](const std::string& detail) {
		file_tabs::close_confirm_error = detail;
	};
	hooks.exit_gate.committed = [] { return file_tabs::exit_review_committed; };
	hooks.exit_gate.request = []() -> std::pair<bool, std::string> {
		const auto requested = file_tabs::request_exit_review();
		return { requested.succeeded, requested.detail };
	};
	hooks.exit_gate.consume_ready = [] { return file_tabs::consume_exit_review_ready(); };
	hooks.exit_gate.cancel = [] { file_tabs::cancel_close_all(); };
	hooks.new_chat = [] { conversations::new_chat(); };
	hooks.push_output_line = [](const std::string& text) {
		output_log::push(bottom_tab_t::output, text);
	};
	hooks.open_file_path = [](const std::string& path) {
		file_browser::open_path(path);
	};
	hooks.open_folder_path = [](const std::string& path) {
		std::string root_error;
		if (!file_browser::set_workspace_root(path, &root_error))
			diag::log_tagged_fmt("file_dialog", "open_folder_root_transaction_failed path=%.260s error=%s",
				path.c_str(), root_error.c_str());
	};
	hooks.save_active_document_as = [] {
		const auto result = chrome_bridge_save_document_as(file_tabs::active_tab);
		file_tabs::shell_save_as_result = result;
	};
	hooks.decompile_or_focus_pseudocode = [] { return chrome_bridge_decompile_or_focus(); };
	hooks.decompile_or_focus_pseudocode_capability = [] {
		return chrome_bridge_decompile_capability();
	};
	diag::log_tagged_critical("chrome_bridge", "legacy_chrome_hooks_bound");
}

}

