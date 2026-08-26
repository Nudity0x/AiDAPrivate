#pragma once

#include "core/disasm/pseudocode_view.hpp"
#include "core/workbench/adapters/pseudocode_document.hpp"
#include "core/workbench/workbench_shell_integration.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pseudocode_view {

struct tab_view_t {
    std::uint64_t address = 0;
    std::string entity_locator;
    std::string label;
    aida::workbench::pseudocode_document::pseudocode_request_t request;
    bool has_request = false;
    std::uint64_t job_id = 0;
    aida::workbench::pseudocode_document::pseudocode_cache_state_t state =
        aida::workbench::pseudocode_document::pseudocode_cache_state_t::empty;
    std::string error;
    bool error_acknowledged = false;
    std::uint64_t resolve_ticket = 0;
};

std::vector<tab_view_t> snapshot_tab_views(
    const disasm_view::workspace_context_t& context);
int active_tab_index(const disasm_view::workspace_context_t& context);
std::optional<tab_view_t> active_tab_view(
    const disasm_view::workspace_context_t& context);

bool workbench_context(
    const disasm_view::workspace_context_t& context,
    aida::workbench::workbench_shell_workspace_context_t& output);
aida::workbench::pseudocode_document::pseudocode_document_model_t*
document_model(const disasm_view::workspace_context_t& context);

void refresh_tab_states(
    const disasm_view::workspace_context_t& context);
bool any_tab_requesting(const disasm_view::workspace_context_t& context);

void set_line_selection(const disasm_view::workspace_context_t& context,
                        int line, std::uint32_t token_begin, std::uint32_t token_end);
struct line_selection_t {
    int selected_line = -1;
    std::uint32_t selected_token_begin = 0;
    std::uint32_t selected_token_end = 0;
};
line_selection_t line_selection(const disasm_view::workspace_context_t& context);

void persist_line_selection(
    const disasm_view::workspace_context_t& context,
    std::uint64_t document_address,
    std::string_view entity_locator,
    const aida::workbench::pseudocode_document::pseudocode_line_view_t& line,
    std::optional<std::uint64_t> source_address);

std::optional<aida::analysis::address_t> typed_source_address(
    const disasm_view::workspace_context_t& context, std::uint64_t address);
std::optional<std::uint64_t> line_source_address(
    aida::workbench::pseudocode_document::pseudocode_document_model_t& model,
    const aida::workbench::pseudocode_document::pseudocode_line_view_t& line,
    const aida::workbench::pseudocode_document::pseudocode_page_t& page);
std::optional<std::uint64_t> token_source_address(
    aida::workbench::pseudocode_document::pseudocode_document_model_t& model,
    const aida::workbench::pseudocode_document::pseudocode_token_view_t* token,
    const aida::workbench::pseudocode_document::pseudocode_line_view_t& line,
    const aida::workbench::pseudocode_document::pseudocode_page_t& page);
const aida::workbench::pseudocode_document::pseudocode_token_view_t*
token_for_position(
    const aida::workbench::pseudocode_document::pseudocode_page_t& page,
    std::uint32_t position);
std::string canonical_address_text(std::uint64_t address);
bool local_rename_identifier_text(const std::string& value);
std::string local_rename_candidate(
    aida::workbench::pseudocode_document::pseudocode_document_model_t& model,
    const aida::workbench::pseudocode_document::pseudocode_page_t& page,
    std::uint32_t token_begin,
    const disasm_view::workspace_context_t& context);
bool apply_active_local_rename(const disasm_view::workspace_context_t& context,
                               const std::string& old_name,
                               const std::string& new_name,
                               std::string& error);
void acknowledge_active_error(const disasm_view::workspace_context_t& context);

void navigate_to_disassembly(
    const disasm_view::workspace_context_t& context, std::uint64_t source_address);
void navigate_to_graph(const disasm_view::workspace_context_t& context,
                       std::uint64_t source_address);
bool navigate_to_hex(const disasm_view::workspace_context_t& context,
                     const aida::analysis::address_t& address);

}
