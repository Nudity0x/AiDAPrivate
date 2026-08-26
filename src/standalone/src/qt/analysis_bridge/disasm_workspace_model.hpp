#pragma once

#include "core/disasm/disasm_view.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aida::qt::analysis_bridge {

struct format_page_delivery_t {
    std::uint64_t page_key = 0;
    std::uint64_t generation = 0;
    std::uint64_t analysis_revision = 0;
    std::uint64_t overlay_revision = 0;
    std::unordered_map<aida::analysis::entity_id_t, disasm_view::formatted_instruction_t> rows;
    std::string error;
    bool reset = false;
};

using format_delivery_fn = std::function<void(format_page_delivery_t)>;

struct xref_delivery_t {
    aida::analysis::address_t address;
    std::vector<disasm_view::xref_popup_entry_t> results;
    std::string error;
};

using xref_delivery_fn = std::function<void(xref_delivery_t)>;

struct export_delivery_t {
    std::string status;
    std::string error;
};

using export_delivery_fn = std::function<void(export_delivery_t)>;

struct view_hooks_t {
    std::function<void()> show_goto;
    std::function<void(const aida::analysis::address_t&)> show_xref_popup;
};

void set_format_delivery_target(const std::shared_ptr<disasm_view::state_t>& view,
                                std::weak_ptr<format_delivery_fn> target);
void set_xref_delivery_target(const std::shared_ptr<disasm_view::state_t>& view,
                              std::weak_ptr<xref_delivery_fn> target);
void set_export_delivery_target(const std::shared_ptr<disasm_view::state_t>& view,
                                std::weak_ptr<export_delivery_fn> target);
void set_view_hooks(const std::shared_ptr<disasm_view::state_t>& view,
                    std::weak_ptr<view_hooks_t> hooks);
void clear_delivery_targets(const std::shared_ptr<disasm_view::state_t>& view);

void update_section_filter_mirror(const disasm_view::workspace_context_t& context,
                                  int section_index);

std::string normalize_presentation_key(std::string_view key);

disasm_view::dialog_open_hook_t rename_dialog_hook();
disasm_view::dialog_open_hook_t comment_dialog_hook();
disasm_view::rebase_dialog_hook_t rebase_dialog_hook();
disasm_view::static_patch_review_hook_t static_patch_review_hook();

using view_focus_hook_t = std::function<void(const char* view_id)>;
void set_view_focus_hook(view_focus_hook_t hook);
view_focus_hook_t view_focus_hook();

using export_path_hook_t =
    std::function<std::optional<std::string>(const disasm_view::workspace_context_t&)>;
void set_export_path_hook(export_path_hook_t hook);
export_path_hook_t export_path_hook();

using focused_presentation_key_fn_t = std::function<std::string()>;
void set_focused_presentation_key_hook(focused_presentation_key_fn_t hook);
std::string focused_presentation_key();

std::uint64_t disasm_evidence_hash(const std::string& value);

}
