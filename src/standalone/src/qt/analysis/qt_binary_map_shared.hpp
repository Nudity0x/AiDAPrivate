#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/analysis/binary_map.hpp"
#include "core/disasm/disasm_view.hpp"
#include "qt/analysis/qt_binary_map_types.hpp"

class QWidget;

namespace aida::qt::analysis {

// Toolkit-agnostic binary-map pipeline (07 sec. 6, verbatim from
// binary_map_view.hpp's detail namespace). Workers publish through the shared
// state atomics; views adopt on their timers.

std::string bm_format_size_human(std::uint64_t bytes);
std::string bm_format_protect_word(std::uint32_t protect);
std::string bm_format_state_word(std::uint32_t state);
std::string bm_format_type_word(std::uint32_t type);
std::string bm_region_kind_label(const qt_binary_map_live_region_t& region);
std::string bm_section_perm_string(const aida::binary_map::map_section_t& section);
float bm_section_entropy_normalized(const aida::binary_map::map_section_t& section);
std::string bm_to_lower_copy(const std::string& value);
bool bm_filter_matches(const std::string& filter_lower, const std::string& text);
std::size_t bm_hex_request_size(std::uint64_t size);
std::string bm_region_to_json(const qt_binary_map_live_region_t& region);
std::string bm_export_live_snapshot_json(const qt_binary_map_live_snapshot_t& snap);
std::string bm_make_function_chat_payload(const aida::binary_map::map_function_t& fn);
std::string bm_make_global_chat_payload(const aida::binary_map::map_global_t& global);
std::string bm_make_region_chat_payload(const qt_binary_map_live_region_t& region);
std::string bm_format_function_summary(const aida::binary_map::map_function_t& fn);

qt_binary_map_live_target_binding_t bm_capture_workspace_binding(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    std::uint64_t generation, std::uint64_t refresh_serial);
bool bm_binding_matches_workspace(
    const qt_binary_map_live_target_binding_t& binding,
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace);
bool bm_validate_live_binding(
    const qt_binary_map_live_target_binding_t& binding,
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    std::string& error);

bool bm_live_available(const QtBinaryMapViewState& state);
bool bm_static_available(const QtBinaryMapViewState& state);
qt_binary_map_active_mode_t bm_resolve_active_mode(
    const QtBinaryMapViewState& state, qt_binary_map_display_mode_t pref);

void bm_perform_refresh(const std::shared_ptr<QtBinaryMapViewState>& state);
void bm_perform_live_refresh(const std::shared_ptr<QtBinaryMapViewState>& state);

bool bm_queue_snapshot_export(const std::shared_ptr<QtBinaryMapViewState>& state,
    const std::string& destination, std::string label,
    std::shared_ptr<const qt_binary_map_live_snapshot_t> live,
    std::shared_ptr<const std::string> text);

// DRIVER WRITE - the reviewed protection-change safety pipeline (pre-write
// re-enumeration + preflight protect match + driver protect + readback verify +
// rollback on mismatch). Verbatim.
bool bm_queue_protection_change(
    const std::shared_ptr<QtBinaryMapViewState>& state,
    const disasm_view::workspace_context_t& context,
    const qt_binary_map_live_target_binding_t& target_binding, std::uint64_t address,
    std::uint64_t size, std::uint32_t expected_protect, std::uint32_t new_protect);

bool bm_dump_region_to_disk(QtBinaryMapViewState& state, std::uint64_t base,
    std::uint64_t size, const std::string& kind_label, QWidget* dialog_parent,
    std::optional<qt_binary_map_live_target_binding_t> live_binding = std::nullopt);

void bm_jump_to_address(QtBinaryMapViewState& state, std::uint64_t va);
void bm_jump_to_hex(QtBinaryMapViewState& state, std::uint64_t va, std::size_t size,
    std::optional<qt_binary_map_live_target_binding_t> live_binding = std::nullopt);
void bm_set_function_pinned(QtBinaryMapViewState& state, std::uint64_t va, bool pinned);

// Installs the three per-state event subscriptions (binary_loaded /
// process_created / process_exited) verbatim from
// binary_map_view.hpp::ensure_subscriptions (:1567-1660). Idempotent; called
// once by the owning QtWorkspaceContext. Handles are released by
// ~QtBinaryMapViewState.
void bm_install_event_subscriptions(
    const std::shared_ptr<QtBinaryMapViewState>& state);

}
