#pragma once

#include "application_action_registry.hpp"
#include "interaction_context.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aida::ui {

enum class view_category_t : std::uint8_t {
    shell,
    explorer,
    document,
    analysis,
    debugger,
    memory,
    types,
    network,
    automation,
    programming,
    output,
    settings
};

struct view_instance_id_t {
    stable_view_id_t view;
    stable_view_instance_key_t instance;

    friend bool operator==(const view_instance_id_t& lhs,
                           const view_instance_id_t& rhs) noexcept {
        return lhs.view == rhs.view && lhs.instance == rhs.instance;
    }

    friend bool operator!=(const view_instance_id_t& lhs,
                           const view_instance_id_t& rhs) noexcept {
        return !(lhs == rhs);
    }

    friend bool operator<(const view_instance_id_t& lhs,
                          const view_instance_id_t& rhs) noexcept {
        if (lhs.view != rhs.view)
            return lhs.view < rhs.view;
        return lhs.instance < rhs.instance;
    }
};

enum class view_operation_status_t : std::uint8_t {
    completed,
    not_registered,
    invalid_instance,
    unavailable,
    not_open,
    not_closeable,
    already_registered,
    invalid_descriptor,
    render_failed
};

struct view_operation_result_t {
    view_operation_status_t status = view_operation_status_t::completed;
    std::string detail;

    bool ok() const noexcept { return status == view_operation_status_t::completed; }
};

inline view_operation_result_t unavailable_view_operation() noexcept {
    return {view_operation_status_t::unavailable,
            "The view host is unavailable"};
}

struct view_host_descriptor_t {
    stable_view_id_t id;
    std::string display_name;
    view_category_t category = view_category_t::document;
    bool closeable = true;
    bool registry_surface = true;
};

struct view_host_instance_t {
    view_instance_id_t id;
    std::string display_name;
    std::string window_name;
    bool open = false;
    bool focused = false;
    bool closeable = true;
    bool pinned = false;
};

enum class dock_region_t : std::uint8_t {
    navigator,
    documents,
    inspector,
    bottom
};

enum class dock_split_direction_t : std::uint8_t {
    left,
    right,
    up,
    down
};

struct surface_placement_t {
    bool realized = false;
    bool docked = false;
    std::optional<dock_region_t> region;
};

enum class workspace_preset_t : std::uint8_t {
    analysis,
    debugging,
    memory,
    types_structures,
    network,
    automation_ai,
    programming,
    safe
};

struct workspace_preset_descriptor_t {
    workspace_preset_t id;
    std::string_view stable_id;
    std::string_view display_name;
    std::string_view description;
    std::uint32_t revision = 1;
};

enum class workspace_identity_kind_t : std::uint8_t {
    built_in,
    user
};

struct workspace_identity_t {
    workspace_identity_kind_t kind = workspace_identity_kind_t::built_in;
    workspace_preset_t preset = workspace_preset_t::analysis;
    std::string user_name;
};

enum class workspace_request_result_t : std::uint8_t {
    completed,
    queued,
    unchanged,
    busy,
    invalid_name,
    already_exists,
    not_found,
    unavailable,
    failed
};

inline constexpr workspace_preset_descriptor_t k_workspace_presets[] = {
    {workspace_preset_t::analysis, "analysis", "Analysis", "Disassembly, pseudocode, graph, symbols, references and inspection", 3},
    {workspace_preset_t::debugging, "debugging", "Debugging", "Execution controls, CPU, registers, breakpoints, threads, stack and trace", 5},
    {workspace_preset_t::memory, "memory", "Memory", "Process sessions, scans, results, memory map, hex, pointers and patches", 5},
    {workspace_preset_t::types_structures, "types-structures", "Types and Structures", "Type catalogs, structure layouts, live values and propagation", 3},
    {workspace_preset_t::network, "network", "Network", "Proxy history, repeater, browser, protocol streams and evidence", 3},
    {workspace_preset_t::automation_ai, "automation-ai", "Automation and AI", "Chat, agents, skills, MCP activity, evidence review and tasks", 4},
    {workspace_preset_t::programming, "programming", "Programming", "Project explorer, source editing, search, terminal, problems and debugging", 4},
    {workspace_preset_t::safe, "safe", "Safe Layout", "Recovery workspace with Start Center, diagnostics and essential navigation", 3}
};

inline constexpr std::size_t k_workspace_preset_count =
    sizeof(k_workspace_presets) / sizeof(k_workspace_presets[0]);

inline const char* view_category_label(view_category_t category) noexcept {
    switch (category) {
        case view_category_t::shell: return "Shell";
        case view_category_t::explorer: return "Explore";
        case view_category_t::document: return "Documents";
        case view_category_t::analysis: return "Analysis";
        case view_category_t::debugger: return "Debugging";
        case view_category_t::memory: return "Memory";
        case view_category_t::types: return "Types and Structures";
        case view_category_t::network: return "Network";
        case view_category_t::automation: return "Automation and AI";
        case view_category_t::programming: return "Programming";
        case view_category_t::output: return "Output";
        case view_category_t::settings: return "Settings";
    }
    return "Views";
}

struct shell_host_services_t {
    std::function<void(const char*)> set_clipboard_text;
    std::function<void(const std::string&, double)> show_error_toast;

    std::function<void(const std::function<void(const view_host_descriptor_t&)>&)>
        for_each_view_descriptor;
    std::function<void(const std::function<void(const view_host_instance_t&)>&)>
        for_each_open_view_instance;
    std::function<std::optional<view_host_descriptor_t>(const stable_view_id_t&)>
        find_view_descriptor;
    std::function<capability_state_t(const stable_view_id_t&, const interaction_context_t&)>
        evaluate_view;
    std::function<bool(const stable_view_id_t&)> is_view_open;
    std::function<bool(const view_instance_id_t&)> is_view_instance_open;
    std::function<std::optional<view_instance_id_t>()> focused_view_instance;
    std::function<std::string(const view_instance_id_t&)> view_window_name;
    std::function<bool(const view_instance_id_t&)> is_view_pinned;
    std::function<bool(const view_instance_id_t&)> can_duplicate_view;
    std::function<bool(const view_instance_id_t&)> can_reset_view_state;
    std::function<bool()> can_reopen_last_closed_view;
    std::function<view_operation_result_t(const stable_view_id_t&)> open_or_focus_view;
    std::function<view_operation_result_t(const stable_view_id_t&)> close_view;
    std::function<view_operation_result_t(const view_instance_id_t&)> close_view_instance;
    std::function<view_operation_result_t(const view_instance_id_t&)> close_other_view_instances;
    std::function<view_operation_result_t(const view_instance_id_t&)> toggle_view_pin;
    std::function<view_operation_result_t(const view_instance_id_t&)> duplicate_view_instance;
    std::function<view_operation_result_t(const view_instance_id_t&)> request_view_reset_state;
    std::function<view_operation_result_t()> reopen_last_closed_view;
    std::function<view_operation_result_t()> open_default_missing_views;

    std::function<workspace_preset_t()> active_workspace_preset;
    std::function<workspace_identity_t()> active_workspace_identity;
    std::function<bool()> user_layout_catalog_ready;
    std::function<bool()> layout_locked;
    std::function<workspace_request_result_t(bool)> set_layout_locked;
    std::function<bool()> workspace_operation_pending;
    std::function<std::string()> workspace_operation_status;
    std::function<bool()> dock_space_ready;
    std::function<bool(dock_region_t)> dock_region_available;
    std::function<surface_placement_t(const std::string&)> inspect_surface_placement;
    std::function<workspace_request_result_t(const std::string&)> float_window;
    std::function<workspace_request_result_t(const std::string&, dock_region_t)> dock_window;
    std::function<workspace_request_result_t(const std::string&, const std::string&,
                                             dock_split_direction_t)> split_window;
    std::function<workspace_request_result_t(workspace_preset_t)> switch_workspace;
    std::function<workspace_request_result_t()> save_active_user_layout;
    std::function<workspace_request_result_t(workspace_preset_t)> restore_builtin_workspace;
    std::function<workspace_request_result_t()> reset_current_layout;
    std::function<workspace_request_result_t()> activate_safe_layout;
    std::function<workspace_request_result_t()> open_missing_views;
};

}

namespace aida::ui::application_ui {

struct retained_entity_action_t {
    std::string action_id;
    capability_state_t capability;
    std::function<action_handler_result_t()> invoke;
    action_check_state_t check_state = action_check_state_t::not_checkable;
};

struct retained_entity_context_t {
    std::string owner_id;
    std::string entity_id;
    std::uint64_t entity_generation = 0;
    stable_view_id_t active_view;
    stable_menu_id_t menu;
    std::function<capability_state_t()> validate_identity;
    std::vector<retained_entity_action_t> actions;
};

struct retained_entity_runtime_context_t {
    retained_entity_context_t retained;
    const retained_entity_context_t* external = nullptr;

    const retained_entity_context_t& context() const noexcept {
        return external ? *external : retained;
    }
};

}
