#include "qt/registry/qt_view_registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <set>
#include <utility>

namespace aida::qt::registry {

namespace {

constexpr float k_maximum_minimum_dimension = 16384.0f;
constexpr std::size_t k_closed_history_capacity = 64;

bool valid_minimum_size(const view_minimum_size_t& size) noexcept {
    return std::isfinite(size.width) && std::isfinite(size.height) &&
           size.width >= 1.0f && size.height >= 1.0f &&
           size.width <= k_maximum_minimum_dimension &&
           size.height <= k_maximum_minimum_dimension;
}

}

const char* category_label(view_category_t category) noexcept {
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

const char* hub_kind_name(hub_kind_t kind) noexcept {
    switch (kind) {
    case hub_kind_t::analysis: return "analysis";
    case hub_kind_t::scan: return "scan";
    case hub_kind_t::types: return "types";
    case hub_kind_t::debugger: return "debugger";
    case hub_kind_t::network: return "network";
    case hub_kind_t::none: break;
    }
    return "";
}

std::string dock_object_name(const view_instance_id_t& id) {
    std::string name = "aida.";
    name.append(id.view.value());
    if (!id.instance.empty()) {
        name.push_back('.');
        name.append(id.instance.value());
    }
    return name;
}

std::string hub_object_name(hub_kind_t kind) {
    std::string name = "aida.hub.";
    name.append(hub_kind_name(kind));
    return name;
}

qt_view_registry_t::qt_view_registry_t(QObject* parent)
    : QObject(parent) {
    context_.generation = 1;
}

qt_view_registry_t::~qt_view_registry_t() = default;

void qt_view_registry_t::set_host_hooks(host_hooks_t hooks) {
    hooks_ = std::move(hooks);
}

void qt_view_registry_t::set_hub_activation_hook(std::function<void(hub_kind_t, int)> hook) {
    hub_activation_hook_ = std::move(hook);
}

void qt_view_registry_t::set_workspace_available_hook(std::function<bool()> hook) {
    workspace_available_hook_ = std::move(hook);
}

void qt_view_registry_t::set_image_available_hook(std::function<bool()> hook) {
    image_available_hook_ = std::move(hook);
}

view_operation_result_t qt_view_registry_t::validate_descriptor(
    const qt_view_descriptor_t& descriptor) {
    if (!is_valid_stable_id(descriptor.id.value()))
        return {view_operation_status_t::invalid_descriptor, "View ID is not stable"};
    if (!is_valid_display_label(descriptor.display_name))
        return {view_operation_status_t::invalid_descriptor, "View display name is invalid"};
    if (!is_valid_stable_id(descriptor.internal_name))
        return {view_operation_status_t::invalid_descriptor, "View internal name is invalid"};
    if (!valid_minimum_size(descriptor.minimum_size))
        return {view_operation_status_t::invalid_descriptor, "View minimum size is invalid"};
    if (descriptor.persistence_version == 0 || descriptor.preset_introduced_revision == 0)
        return {view_operation_status_t::invalid_descriptor, "View persistence metadata is invalid"};
    if (!descriptor.factory)
        return {view_operation_status_t::invalid_descriptor, "View has no content factory"};
    if (descriptor.default_open &&
        descriptor.identity_policy == view_identity_policy_t::multi_instance)
        return {view_operation_status_t::invalid_descriptor,
                "Multi-instance view cannot open without an instance key"};
    if (descriptor.hub == hub_kind_t::none) {
        if (descriptor.hub_subview != 0)
            return {view_operation_status_t::invalid_descriptor,
                    "View without hub membership cannot carry a hub subview index"};
    } else {
        if (descriptor.identity_policy != view_identity_policy_t::singleton)
            return {view_operation_status_t::invalid_descriptor,
                    "Hub member view must be a singleton"};
        if (descriptor.role != view_presentation_role_t::tool_window)
            return {view_operation_status_t::invalid_descriptor,
                    "Hub member view must use the tool window role"};
        if (descriptor.hub_subview < 0)
            return {view_operation_status_t::invalid_descriptor,
                    "Hub member view subview index is invalid"};
    }
    std::set<stable_action_id_t> actions;
    for (const auto& action : descriptor.action_bindings) {
        if (!is_valid_stable_id(action.value()))
            return {view_operation_status_t::invalid_descriptor, "View action binding is invalid"};
        if (!actions.insert(action).second)
            return {view_operation_status_t::invalid_descriptor,
                    "View action binding is duplicated"};
    }
    std::set<stable_view_id_t> aliases;
    for (const auto& alias : descriptor.persistence_aliases) {
        if (!is_valid_stable_id(alias.value()) || alias == descriptor.id)
            return {view_operation_status_t::invalid_descriptor, "View persistence alias is invalid"};
        if (!aliases.insert(alias).second)
            return {view_operation_status_t::invalid_descriptor, "View persistence alias is duplicated"};
    }
    return {};
}

view_operation_result_t qt_view_registry_t::register_view(qt_view_descriptor_t descriptor) {
    auto validation = validate_descriptor(descriptor);
    if (!validation.ok())
        return validation;
    if (descriptors_.find(descriptor.id) != descriptors_.end())
        return {view_operation_status_t::already_registered, "View ID is already registered"};
    for (const auto& entry : descriptors_) {
        if (entry.second.internal_name == descriptor.internal_name)
            return {view_operation_status_t::already_registered,
                    "View internal name is already registered"};
        if (std::find(entry.second.persistence_aliases.begin(),
                      entry.second.persistence_aliases.end(), descriptor.id) !=
                entry.second.persistence_aliases.end() ||
            std::find(descriptor.persistence_aliases.begin(), descriptor.persistence_aliases.end(),
                      entry.first) != descriptor.persistence_aliases.end())
            return {view_operation_status_t::already_registered,
                    "View ID conflicts with a persistence alias"};
        for (const auto& alias : descriptor.persistence_aliases)
            if (std::find(entry.second.persistence_aliases.begin(),
                          entry.second.persistence_aliases.end(), alias) !=
                    entry.second.persistence_aliases.end())
                return {view_operation_status_t::already_registered,
                        "View persistence alias is already registered"};
        if (descriptor.hub != hub_kind_t::none &&
            entry.second.hub == descriptor.hub &&
            entry.second.hub_subview == descriptor.hub_subview)
            return {view_operation_status_t::already_registered,
                    "Hub subview index is already registered within the hub"};
    }

    const auto id = descriptor.id;
    const bool default_open = descriptor.default_open;
    const auto inserted = descriptors_.emplace(id, std::move(descriptor));
    try {
        if (default_open) {
            const view_instance_id_t instance{id, {}};
            auto& state = ensure_instance(inserted.first->second, instance, {});
            state.open = true;
        }
    } catch (...) {
        descriptors_.erase(inserted.first);
        throw;
    }
    ++revision_;
    ++visibility_revision_;
    ++context_.generation;
    Q_EMIT descriptorsChanged();
    Q_EMIT instancesChanged();
    Q_EMIT visibilityRevisionChanged();
    return {};
}

std::size_t qt_view_registry_t::register_catalog(const qt_view_factory_t& placeholder_factory) {
    std::size_t registered = 0;
    for (const auto& entry : k_catalog) {
        qt_view_descriptor_t descriptor;
        descriptor.id = stable_view_id_t(entry.id);
        descriptor.display_name = entry.label;
        descriptor.internal_name = std::string("aida.") + entry.id;
        descriptor.category = entry.category;
        descriptor.role = entry.role;
        descriptor.identity_policy = std::string_view(entry.id).compare(0, 9, "document.") == 0
            ? view_identity_policy_t::multi_instance
            : view_identity_policy_t::singleton;
        descriptor.minimum_size = {entry.minimum_width, entry.minimum_height};
        descriptor.persistence_version = entry.persistence_version;
        if (entry.persistence_alias)
            descriptor.persistence_aliases.emplace_back(entry.persistence_alias);
        descriptor.default_open = entry.default_open;
        descriptor.closeable = entry.closeable;
        descriptor.content_policy = entry.content_policy;
        descriptor.hub = entry.hub;
        descriptor.hub_subview = entry.hub_subview;
        descriptor.requires_workspace = entry.requires_workspace;
        descriptor.factory = placeholder_factory;
        descriptor.ported = false;
        const bool requires_workspace = entry.requires_workspace;
        descriptor.capability = [this, requires_workspace,
            is_image_view = std::string_view(entry.id) == "document.image"](
                const interaction_context_t&) {
            if (is_image_view &&
                !(image_available_hook_ && image_available_hook_()))
                return capability_state_t::unavailable("Open an image file first");
            if (!requires_workspace)
                return capability_state_t::available();
            const bool available = workspace_available_hook_ && workspace_available_hook_();
            return available
                ? capability_state_t::available()
                : capability_state_t::unavailable("Open and analyze a binary first");
        };
        if (entry.hub != hub_kind_t::none) {
            const hub_kind_t hub = entry.hub;
            const int subview = entry.hub_subview;
            descriptor.activate = [this, hub, subview](const view_instance_id_t&) {
                if (hub_activation_hook_)
                    hub_activation_hook_(hub, subview);
            };
        }
        if (std::string_view(entry.id) == "document.code" ||
            std::string_view(entry.id) == "document.hex") {
            descriptor.deactivate = [this](const view_instance_id_t& instance) {
                if (hooks_.deactivate_instance)
                    hooks_.deactivate_instance(instance);
            };
        }
        const auto result = register_view(std::move(descriptor));
        if (result.ok())
            ++registered;
    }
    return registered;
}

view_operation_result_t qt_view_registry_t::install_view_factory(
    const stable_view_id_t& id, qt_view_factory_t factory, std::optional<content_policy_t> policy) {
    if (!factory)
        return {view_operation_status_t::invalid_descriptor, "View content factory is empty"};
    const stable_view_id_t canonical = canonical_view_id(id);
    auto found = descriptors_.find(canonical);
    if (found == descriptors_.end())
        return {view_operation_status_t::not_registered, "View is not registered"};
    found->second.factory = std::move(factory);
    if (policy)
        found->second.content_policy = *policy;
    found->second.ported = true;
    ++revision_;
    ++context_.generation;
    Q_EMIT descriptorsChanged();
    Q_EMIT instancesChanged();
    return {};
}

bool qt_view_registry_t::is_ported(const stable_view_id_t& id) const {
    const auto* descriptor = find_descriptor(canonical_view_id(id));
    return descriptor && descriptor->ported;
}

const qt_view_descriptor_t* qt_view_registry_t::find_descriptor(
    const stable_view_id_t& id) const noexcept {
    const auto found = descriptors_.find(id);
    return found == descriptors_.end() ? nullptr : &found->second;
}

const view_instance_state_t* qt_view_registry_t::find_instance(
    const view_instance_id_t& id) const noexcept {
    const auto found = instances_.find(id);
    return found == instances_.end() ? nullptr : &found->second;
}

view_instance_state_t* qt_view_registry_t::find_instance(const view_instance_id_t& id) noexcept {
    const auto found = instances_.find(id);
    return found == instances_.end() ? nullptr : &found->second;
}

stable_view_id_t qt_view_registry_t::canonical_view_id(const stable_view_id_t& id) const {
    stable_view_id_t canonical = id;
    for (const auto& entry : descriptors_) {
        if (std::find(entry.second.persistence_aliases.begin(),
                      entry.second.persistence_aliases.end(), id) !=
            entry.second.persistence_aliases.end())
            canonical = entry.second.id;
    }
    return canonical;
}

view_instance_id_t qt_view_registry_t::instance_for(const stable_view_id_t& id) const {
    const stable_view_id_t canonical = canonical_view_id(id);
    const catalog_entry_t* entry = find_catalog_entry(canonical.value());
    if (entry) {
        const bool document = std::string_view(entry->id).compare(0, 9, "document.") == 0;
        if (std::string_view(entry->id) == "document.code")
            return {stable_view_id_t(entry->id), stable_view_instance_key_t("group.0")};
        return {stable_view_id_t(entry->id), document
            ? stable_view_instance_key_t("primary")
            : stable_view_instance_key_t{}};
    }
    const auto* descriptor = find_descriptor(canonical);
    return {canonical, descriptor &&
            descriptor->identity_policy == view_identity_policy_t::multi_instance
                ? stable_view_instance_key_t("primary")
                : stable_view_instance_key_t{}};
}

capability_state_t qt_view_registry_t::evaluate(const stable_view_id_t& id,
                                                const interaction_context_t& context) const {
    const auto* descriptor = find_descriptor(id);
    if (!descriptor)
        return capability_state_t::unavailable("View is not registered", false);
    try {
        auto result = descriptor->capability
            ? descriptor->capability(context)
            : capability_state_t::available();
        if ((!result.visible || !result.enabled) && result.disabled_reason.empty())
            result.disabled_reason = "View is unavailable in the current context";
        return result;
    } catch (const std::exception& exception) {
        return capability_state_t::unavailable(exception.what());
    } catch (...) {
        return capability_state_t::unavailable("View capability evaluation failed");
    }
}

view_operation_result_t qt_view_registry_t::validate_instance(
    const qt_view_descriptor_t& descriptor,
    const view_instance_id_t& id) const {
    if (descriptor.id != id.view)
        return {view_operation_status_t::invalid_instance, "Instance belongs to another view"};
    if (descriptor.identity_policy == view_identity_policy_t::singleton && !id.instance.empty())
        return {view_operation_status_t::invalid_instance,
                "Singleton view cannot have an instance key"};
    if (descriptor.identity_policy == view_identity_policy_t::multi_instance &&
        !is_valid_stable_instance_key(id.instance.value()))
        return {view_operation_status_t::invalid_instance,
                "Multi-instance view requires a stable instance key"};
    return {};
}

view_instance_state_t& qt_view_registry_t::ensure_instance(
    const qt_view_descriptor_t& descriptor,
    const view_instance_id_t& id,
    std::string display_name) {
    auto found = instances_.find(id);
    if (found != instances_.end()) {
        if (!display_name.empty() && display_name != found->second.display_name)
            found->second.display_name = std::move(display_name);
        return found->second;
    }

    view_instance_state_t state;
    state.id = id;
    state.display_name = display_name.empty() ? descriptor.display_name : std::move(display_name);
    return instances_.emplace(id, std::move(state)).first->second;
}

view_operation_result_t qt_view_registry_t::open(const view_instance_id_t& id,
                                                 const interaction_context_t& context,
                                                 std::string display_name) {
    const auto* descriptor = find_descriptor(id.view);
    if (!descriptor)
        return {view_operation_status_t::not_registered, "View is not registered"};
    auto instance_validation = validate_instance(*descriptor, id);
    if (!instance_validation.ok())
        return instance_validation;
    if (!display_name.empty() && !is_valid_display_label(display_name))
        return {view_operation_status_t::invalid_instance, "Instance display name is invalid"};
    const auto capability = evaluate(id.view, context);
    if (!capability.visible || !capability.enabled)
        return {view_operation_status_t::unavailable, capability.disabled_reason};

    const auto* existing = find_instance(id);
    const bool already_open = existing && existing->open;
    try {
        if (descriptor->activate)
            descriptor->activate(id);
    } catch (const std::exception& exception) {
        return {view_operation_status_t::unavailable, exception.what()};
    } catch (...) {
        return {view_operation_status_t::unavailable,
            "View activation failed with an unknown error"};
    }

    if (!already_open && hooks_.open_instance) {
        const auto host_result = hooks_.open_instance(*descriptor, id, display_name);
        if (!host_result.ok())
            return host_result;
    }

    const bool visibility_changed = !existing || !existing->open;
    const bool changed = visibility_changed ||
        (!display_name.empty() && existing && display_name != existing->display_name);
    auto& state = ensure_instance(*descriptor, id, std::move(display_name));
    state.open = true;
    closed_history_.erase(
        std::remove(closed_history_.begin(), closed_history_.end(), id),
        closed_history_.end());
    if (changed) {
        ++revision_;
        ++context_.generation;
        Q_EMIT instancesChanged();
    }
    if (visibility_changed) {
        ++visibility_revision_;
        Q_EMIT visibilityRevisionChanged();
    }
    return {};
}

view_operation_result_t qt_view_registry_t::ensure_identity(const view_instance_id_t& id,
                                                            std::string display_name) {
    const auto* descriptor = find_descriptor(id.view);
    if (!descriptor)
        return {view_operation_status_t::not_registered, "View is not registered"};
    auto instance_validation = validate_instance(*descriptor, id);
    if (!instance_validation.ok())
        return instance_validation;
    if (!display_name.empty() && !is_valid_display_label(display_name))
        return {view_operation_status_t::invalid_instance, "Instance display name is invalid"};
    const auto* existing = find_instance(id);
    const bool changed = !existing ||
        (!display_name.empty() && display_name != existing->display_name);
    ensure_instance(*descriptor, id, std::move(display_name));
    if (changed) {
        ++revision_;
        Q_EMIT instancesChanged();
    }
    return {};
}

view_operation_result_t qt_view_registry_t::focus(const view_instance_id_t& id) {
    auto* state = find_instance(id);
    if (!state || !state->open)
        return {view_operation_status_t::not_open, "View is not open"};
    state->focus_request_generation = ++focus_sequence_;
    state->last_focus_sequence = focus_sequence_;
    ++revision_;
    if (hooks_.focus_instance) {
        const auto host_result = hooks_.focus_instance(id);
        if (!host_result.ok())
            return host_result;
    }
    Q_EMIT instancesChanged();
    return {};
}

view_operation_result_t qt_view_registry_t::open_or_focus(const view_instance_id_t& id,
                                                          const interaction_context_t& context,
                                                          std::string display_name) {
    auto opened = open(id, context, std::move(display_name));
    if (!opened.ok())
        return opened;
    return focus(id);
}

void qt_view_registry_t::close_bookkeeping(const view_instance_id_t& id, bool record_history) {
    auto* state = find_instance(id);
    if (!state || !state->open)
        return;
    if (record_history) {
        closed_history_.erase(
            std::remove(closed_history_.begin(), closed_history_.end(), id),
            closed_history_.end());
        closed_history_.push_back(id);
        if (closed_history_.size() > k_closed_history_capacity)
            closed_history_.erase(closed_history_.begin(),
                closed_history_.begin() +
                    static_cast<std::ptrdiff_t>(closed_history_.size() -
                        k_closed_history_capacity));
    }
    state->open = false;
    state->focused = false;
    state->focus_request_generation = state->consumed_focus_generation;
    if (focused_instance_ && *focused_instance_ == id)
        focused_instance_.reset();
    ++revision_;
    ++visibility_revision_;
    ++context_.generation;
    Q_EMIT instancesChanged();
    Q_EMIT visibilityRevisionChanged();
}

view_operation_result_t qt_view_registry_t::close(const view_instance_id_t& id) {
    const auto* descriptor = find_descriptor(id.view);
    if (!descriptor)
        return {view_operation_status_t::not_registered, "View is not registered"};
    auto* state = find_instance(id);
    if (!state || !state->open)
        return {view_operation_status_t::not_open, "View is not open"};
    if (!descriptor->closeable)
        return {view_operation_status_t::not_closeable, "View cannot be closed"};

    try {
        if (descriptor->deactivate)
            descriptor->deactivate(id);
    } catch (const std::exception& exception) {
        return {view_operation_status_t::unavailable, exception.what()};
    } catch (...) {
        return {view_operation_status_t::unavailable,
            "View deactivation failed with an unknown error"};
    }

    if (hooks_.close_instance) {
        const auto host_result = hooks_.close_instance(id);
        if (!host_result.ok())
            return host_result;
    }
    close_bookkeeping(id, true);
    return {};
}

view_operation_result_t qt_view_registry_t::reopen_last_closed(
    const interaction_context_t& context) {
    if (closed_history_.empty())
        return {view_operation_status_t::not_open, "No recently closed view is available"};
    const view_instance_id_t id = closed_history_.back();
    return open_or_focus(id, context);
}

view_operation_result_t qt_view_registry_t::open_default_missing(
    const interaction_context_t& context) {
    bool opened = false;
    for (const auto& entry : descriptors_) {
        const auto& descriptor = entry.second;
        if (!descriptor.default_open)
            continue;
        const view_instance_id_t id{descriptor.id, {}};
        if (is_open(id))
            continue;
        const auto result = open(id, context);
        if (!result.ok())
            return result;
        opened = true;
    }
    return opened
        ? view_operation_result_t{}
        : view_operation_result_t{view_operation_status_t::completed,
            "All default views are already open"};
}

view_operation_result_t qt_view_registry_t::erase_closed_instance(const view_instance_id_t& id) {
    const auto* descriptor = find_descriptor(id.view);
    if (!descriptor)
        return {view_operation_status_t::not_registered, "View is not registered"};
    if (descriptor->identity_policy != view_identity_policy_t::multi_instance)
        return {view_operation_status_t::invalid_instance,
                "Singleton view instances are retained"};
    const auto found = instances_.find(id);
    if (found == instances_.end())
        return {view_operation_status_t::invalid_instance, "View instance does not exist"};
    if (found->second.open)
        return {view_operation_status_t::invalid_instance,
                "Open view instance cannot be erased"};
    if (hooks_.erase_instance) {
        const auto host_result = hooks_.erase_instance(id);
        if (!host_result.ok())
            return host_result;
    }
    closed_history_.erase(
        std::remove(closed_history_.begin(), closed_history_.end(), id),
        closed_history_.end());
    instances_.erase(found);
    ++revision_;
    ++context_.generation;
    Q_EMIT instancesChanged();
    return {};
}

void qt_view_registry_t::sync_instance_visibility(const view_instance_id_t& id, bool open,
                                                  bool record_history) {
    const auto* descriptor = find_descriptor(id.view);
    if (!descriptor)
        return;
    if (open) {
        const auto* existing = find_instance(id);
        const bool changed = !existing || !existing->open;
        auto& state = ensure_instance(*descriptor, id, {});
        state.open = true;
        closed_history_.erase(
            std::remove(closed_history_.begin(), closed_history_.end(), id),
            closed_history_.end());
        if (changed) {
            ++revision_;
            ++visibility_revision_;
            ++context_.generation;
            Q_EMIT instancesChanged();
            Q_EMIT visibilityRevisionChanged();
        }
        return;
    }
    close_bookkeeping(id, record_history);
}

void qt_view_registry_t::note_hub_subview(hub_kind_t hub, int subview) {
    if (hub == hub_kind_t::none || subview < 0)
        return;
    for (const auto& entry : descriptors_) {
        if (entry.second.hub == hub && entry.second.hub_subview == subview) {
            hub_active_member_[hub] = entry.second.id;
            context_.active_view = entry.second.id;
            context_.active_view_instance = stable_view_instance_key_t{};
            ++context_.generation;
            return;
        }
    }
}

std::optional<stable_view_id_t> qt_view_registry_t::hub_active_view(hub_kind_t hub) const {
    const auto found = hub_active_member_.find(hub);
    if (found == hub_active_member_.end())
        return std::nullopt;
    return found->second;
}

bool qt_view_registry_t::consume_focus_request(const view_instance_id_t& id) noexcept {
    auto* state = find_instance(id);
    if (!state || !state->open ||
        state->focus_request_generation <= state->consumed_focus_generation)
        return false;
    state->consumed_focus_generation = state->focus_request_generation;
    return true;
}

void qt_view_registry_t::update_focus(const std::optional<view_instance_id_t>& focused) {
    if (focused_instance_ == focused)
        return;
    if (focused_instance_) {
        if (auto* previous = find_instance(*focused_instance_))
            previous->focused = false;
    }
    focused_instance_.reset();
    if (focused) {
        if (auto* current = find_instance(*focused); current && current->open) {
            current->focused = true;
            current->last_focus_sequence = ++focus_sequence_;
            focused_instance_ = *focused;
            context_.active_view = focused->view;
            context_.active_view_instance = focused->instance;
        }
    }
    ++revision_;
    ++context_.generation;
    Q_EMIT instancesChanged();
    Q_EMIT focusedInstanceChanged();
}

bool qt_view_registry_t::is_open(const view_instance_id_t& id) const noexcept {
    const auto* state = find_instance(id);
    return state && state->open;
}

std::optional<view_instance_id_t> qt_view_registry_t::focused_instance() const {
    return focused_instance_;
}

void qt_view_registry_t::for_each_descriptor(
    const std::function<void(const qt_view_descriptor_t&)>& visitor) const {
    if (!visitor)
        return;
    for (const auto& entry : descriptors_)
        visitor(entry.second);
}

void qt_view_registry_t::for_each_instance(
    const std::function<void(const qt_view_descriptor_t&, const view_instance_state_t&)>& visitor,
    bool open_only) const {
    if (!visitor)
        return;
    for (const auto& entry : instances_) {
        if (open_only && !entry.second.open)
            continue;
        const auto* descriptor = find_descriptor(entry.first.view);
        if (descriptor)
            visitor(*descriptor, entry.second);
    }
}

void qt_view_registry_t::for_each_menu_entry(
    const std::function<void(const menu_entry_t&)>& visitor) {
    if (!visitor)
        return;
    std::vector<menu_entry_t> entries;
    entries.reserve(descriptor_count());
    for_each_descriptor([&](const qt_view_descriptor_t& descriptor) {
        if (!descriptor.closeable && descriptor.role == view_presentation_role_t::shell_surface)
            return;
        const view_instance_id_t instance = instance_for(descriptor.id);
        const auto capability = evaluate(descriptor.id, context_);
        entries.push_back({descriptor.id, descriptor.display_name, descriptor.category,
            is_open(instance), capability.visible && capability.enabled,
            capability.disabled_reason});
    });
    std::sort(entries.begin(), entries.end(), [](const menu_entry_t& lhs, const menu_entry_t& rhs) {
        if (lhs.category != rhs.category)
            return lhs.category < rhs.category;
        return lhs.label < rhs.label;
    });
    for (const auto& entry : entries)
        visitor(entry);
}

std::string qt_view_registry_t::persistence_fingerprint() const noexcept {
    try {
        std::uint64_t hash = 14695981039346656037ULL;
        const auto append = [&hash](std::string_view value) {
            for (const char raw_byte : value) {
                const auto byte = static_cast<unsigned char>(raw_byte);
                hash ^= byte;
                hash *= 1099511628211ULL;
            }
            hash ^= 0xFFU;
            hash *= 1099511628211ULL;
        };
        for (const auto& entry : descriptors_) {
            const qt_view_descriptor_t& descriptor = entry.second;
            append(descriptor.id.value());
            append(descriptor.internal_name);
            append(std::to_string(descriptor.persistence_version));
            append(std::to_string(descriptor.preset_introduced_revision));
            append(std::to_string(static_cast<unsigned>(descriptor.category)));
            append(std::to_string(static_cast<unsigned>(descriptor.identity_policy)));
            append(std::to_string(static_cast<unsigned>(descriptor.role)));
            for (const auto& alias : descriptor.persistence_aliases)
                append(alias.value());
        }
        char encoded[17]{};
        const int length = std::snprintf(encoded, sizeof(encoded), "%016llx",
            static_cast<unsigned long long>(hash));
        return length == 16 ? std::string(encoded, 16) : std::string{};
    } catch (...) {
        return {};
    }
}

}
