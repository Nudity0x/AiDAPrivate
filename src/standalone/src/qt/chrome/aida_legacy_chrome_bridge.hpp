#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/ui/application_action_registry.hpp"
#include "core/ui/interaction_context.hpp"

namespace aida::qt::chrome {

struct legacy_quick_open_bridge_t {
    std::function<bool(std::string& query_out)> poll_open_request;
    std::function<void()> mark_closed;
    std::function<void()> close_command_palette;
};

struct exit_review_document_snapshot_t {
    std::uint64_t document_id = 0;
    std::uint64_t revision = 0;
    std::string filename;
    bool filepath_empty = true;
    bool target_current = false;
    bool save_disabled = false;
    std::string save_gate_detail;
    bool close_disabled = false;
};

struct exit_review_snapshot_t {
    std::uint64_t generation = 0;
    bool review_active = false;
    bool dialog_active = false;
    exit_review_document_snapshot_t current;
    std::string close_error;
    std::vector<std::string> queue_names;
};

struct exit_review_actions_t {
    std::function<exit_review_snapshot_t()> poll;
    std::function<void()> save_current;
    std::function<void(const std::string& destination)> save_current_as;
    std::function<void()> discard_current;
    std::function<void()> cancel;
    std::function<void(const std::string& detail)> set_close_error;
};

struct exit_gate_hooks_t {
    std::function<bool()> committed;
    std::function<std::pair<bool, std::string>()> request;
    std::function<bool()> consume_ready;
    std::function<void()> cancel;
};

struct legacy_chrome_hooks_t {
    legacy_quick_open_bridge_t quick_open;
    exit_review_actions_t exit_review;
    exit_gate_hooks_t exit_gate;
    std::function<void()> new_chat;
    std::function<void(const std::string& text)> push_output_line;
    std::function<void(const std::string& path)> open_file_path;
    std::function<void(const std::string& path)> open_folder_path;
    std::function<void()> save_active_document_as;
    std::function<aida::ui::action_handler_result_t()> decompile_or_focus_pseudocode;
    std::function<aida::ui::capability_state_t()> decompile_or_focus_pseudocode_capability;
    std::function<void(const std::string& view_id)> focus_view;
};

legacy_chrome_hooks_t& legacy_chrome_hooks();

void bind_legacy_chrome_hooks();

}
