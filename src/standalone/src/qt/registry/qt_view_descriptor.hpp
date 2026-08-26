#pragma once

#include "core/ui/interaction_context.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class QWidget;

namespace aida::qt::registry {

using aida::ui::stable_view_id_t;
using aida::ui::stable_view_instance_key_t;
using aida::ui::stable_action_id_t;
using aida::ui::capability_state_t;
using aida::ui::interaction_context_t;
using aida::ui::is_valid_stable_id;
using aida::ui::is_valid_stable_instance_key;
using aida::ui::is_valid_display_label;

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

enum class view_identity_policy_t : std::uint8_t {
    singleton,
    multi_instance
};

enum class view_presentation_role_t : std::uint8_t {
    tool_window,
    document,
    inspector,
    bottom_panel,
    shell_surface
};

struct view_minimum_size_t {
    float width = 240.0f;
    float height = 160.0f;
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

struct view_instance_state_t {
    view_instance_id_t id;
    std::string display_name;
    bool open = false;
    bool focused = false;
    std::uint64_t focus_request_generation = 0;
    std::uint64_t consumed_focus_generation = 0;
    std::uint64_t last_focus_sequence = 0;
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

enum class hub_kind_t : std::uint8_t {
    none,
    analysis,
    scan,
    types,
    debugger,
    network
};

enum class content_policy_t : std::uint8_t {
    lazy_dispose,
    lazy_keep
};

using qt_view_capability_fn_t = std::function<capability_state_t(const interaction_context_t&)>;
using qt_view_factory_t = std::function<QWidget*(QWidget* parent, const view_instance_id_t& instance)>;
using qt_view_lifecycle_fn_t = std::function<void(const view_instance_id_t&)>;

struct qt_view_descriptor_t {
    stable_view_id_t id;
    std::string display_name;
    std::string internal_name;
    view_category_t category = view_category_t::document;
    view_identity_policy_t identity_policy = view_identity_policy_t::singleton;
    view_presentation_role_t role = view_presentation_role_t::tool_window;
    view_minimum_size_t minimum_size;
    std::uint32_t persistence_version = 1;
    std::uint32_t preset_introduced_revision = 1;
    std::vector<stable_view_id_t> persistence_aliases;
    std::vector<stable_action_id_t> action_bindings;
    qt_view_capability_fn_t capability;
    qt_view_factory_t factory;
    qt_view_lifecycle_fn_t activate;
    qt_view_lifecycle_fn_t deactivate;
    bool default_open = false;
    bool closeable = true;
    content_policy_t content_policy = content_policy_t::lazy_dispose;
    hub_kind_t hub = hub_kind_t::none;
    int hub_subview = 0;
    bool requires_workspace = false;
    bool ported = false;
};

struct menu_entry_t {
    stable_view_id_t id;
    std::string label;
    view_category_t category = view_category_t::document;
    bool open = false;
    bool enabled = true;
    std::string disabled_reason;
};

const char* category_label(view_category_t category) noexcept;
const char* hub_kind_name(hub_kind_t kind) noexcept;
std::string dock_object_name(const view_instance_id_t& id);
std::string hub_object_name(hub_kind_t kind);

}
