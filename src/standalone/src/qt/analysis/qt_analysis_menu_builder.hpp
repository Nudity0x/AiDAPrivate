#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/disasm/disasm_view.hpp"
#include "core/ui/interaction_context.hpp"
#include "core/ui/shell_host_contract.hpp"

namespace aida::qt::analysis {

class QtAnalysisBridge;
struct QtXrefViewState;

// Shared retained-entity menu builders (07 sec. 1.1/sec. 3.4/sec. 4.4/sec. 5/sec. 7). Every
// builder produces a retained_entity_context_t whose validate_identity closure
// ports the ImGui-era correctness invariant verbatim (generation-XOR identity
// formulas included); presentation happens through MenuBridge, which
// revalidates on aboutToShow (S6).
namespace qt_analysis_menus {

using retained_menu_t = aida::ui::application_ui::retained_entity_context_t;
using capability_t = aida::ui::capability_state_t;
using handler_result_t = aida::ui::action_handler_result_t;
using validate_fn_t = std::function<capability_t()>;
using invoke_fn_t = std::function<handler_result_t()>;

void add_action(retained_menu_t& menu, std::string id, bool enabled,
                const char* reason, invoke_fn_t invoke);
void add_separator_marker_copy_actions(retained_menu_t& menu,
    const std::string& address_text, const std::string& name,
    const std::string& line_text);

// Shared navigation/copy action set behind an analysis entity with an address
// (07 sec. 3.4 action set; copy.address/copy.address_va and copy.name/copy.text
// duplicates collapsed per 07 sec. 10).
void fill_analysis_entity_actions(retained_menu_t& menu,
    const disasm_view::workspace_context_t& context, std::uint64_t address,
    bool has_address, const std::string& name, const std::string& context_text,
    const std::string& detail_text);

retained_menu_t build_analysis_row_menu(
    const disasm_view::workspace_context_t& context, std::uint64_t address,
    bool has_address, const std::string& name, const std::string& context_text,
    const std::string& detail_text, validate_fn_t validate_identity);

retained_menu_t build_function_menu(
    const disasm_view::workspace_context_t& context, std::uint64_t address,
    const std::string& name, bool have_entry, validate_fn_t validate_identity);

retained_menu_t build_xref_menu(
    const disasm_view::workspace_context_t& context,
    const std::shared_ptr<QtXrefViewState>& state, std::uint64_t runtime,
    const std::string& name, const std::string& label);

}  // namespace qt_analysis_menus

}
