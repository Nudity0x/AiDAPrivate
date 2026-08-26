#include "qt/registry/view_visibility.hpp"

#include "core/settings/standalone_settings.hpp"
#include "core/settings/settings_persistence_service.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <limits>

namespace aida::qt::registry {

namespace {

constexpr std::size_t k_maximum_workspace_visibility_bytes = 64U * 1024U;

nlohmann::json read_visibility_root() {
    try {
        if (g_sa_settings.workspace.view_visibility_json.size() >
            k_maximum_workspace_visibility_bytes)
            return nlohmann::json::object();
        nlohmann::json root = nlohmann::json::parse(
            g_sa_settings.workspace.view_visibility_json.empty()
                ? "{}" : g_sa_settings.workspace.view_visibility_json);
        if (root.is_object())
            return root;
    } catch (...) {
    }
    return nlohmann::json::object();
}

void write_visibility_root(nlohmann::json& root) {
    root["version"] = 3;
    const std::string serialized = root.dump();
    if (serialized.size() > k_maximum_workspace_visibility_bytes)
        return;
    if (serialized == g_sa_settings.workspace.view_visibility_json)
        return;
    g_sa_settings.workspace.view_visibility_json = serialized;
    static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
}

bool workspace_manages_visibility(const qt_view_descriptor_t& descriptor) noexcept {
    return descriptor.identity_policy == view_identity_policy_t::singleton &&
        descriptor.role != view_presentation_role_t::document && descriptor.closeable;
}

std::optional<bool> persisted_visibility_lookup(const nlohmann::json* views,
    const stable_view_id_t& id, const qt_view_registry_t* registry) {
    if (!views)
        return std::nullopt;
    if (views->contains(id.value()) && (*views)[id.value()].is_boolean())
        return (*views)[id.value()].get<bool>();
    for (auto iterator = views->begin(); iterator != views->end(); ++iterator) {
        if (!iterator.value().is_boolean() || !is_valid_stable_id(iterator.key()))
            continue;
        if (registry->canonical_view_id(stable_view_id_t(iterator.key())) == id)
            return iterator.value().get<bool>();
    }
    return std::nullopt;
}

}

ViewVisibilityController::ViewVisibilityController(qt_view_registry_t* registry, QObject* parent)
    : QObject(parent), registry_(registry) {
    if (registry_) {
        connect(registry_, &qt_view_registry_t::visibilityRevisionChanged,
                this, &ViewVisibilityController::capture);
    }
}

void ViewVisibilityController::set_view_actions(view_actions_t actions) {
    actions_ = std::move(actions);
}

void ViewVisibilityController::set_active_context_provider(
    std::function<std::pair<docking::workspace_preset_t, std::string>()> provider) {
    active_context_ = std::move(provider);
}

void ViewVisibilityController::restore(docking::workspace_preset_t preset,
    std::string_view identity) {
    workspace_visibility_.clear();
    deferred_workspace_opens_.clear();
    const nlohmann::json root = read_visibility_root();
    const std::string preset_key(identity);
    const nlohmann::json* persisted_views = nullptr;
    const nlohmann::json* persisted_entry = nullptr;
    std::uint32_t persisted_revision = 1;
    if (root.contains("presets") && root["presets"].is_object()) {
        const auto& presets = root["presets"];
        if (presets.contains(preset_key) && presets[preset_key].is_object() &&
            presets[preset_key].contains("views") && presets[preset_key]["views"].is_object()) {
            persisted_entry = &presets[preset_key];
            persisted_views = &(*persisted_entry)["views"];
        }
        else if (identity.rfind("builtin:", 0) == 0) {
            const std::string legacy = docking::workspace_preset_key(preset);
            if (presets.contains(legacy) && presets[legacy].is_object() &&
                presets[legacy].contains("views") && presets[legacy]["views"].is_object()) {
                persisted_entry = &presets[legacy];
                persisted_views = &(*persisted_entry)["views"];
            }
        }
    }
    if (persisted_entry && persisted_entry->contains("preset_revision") &&
        (*persisted_entry)["preset_revision"].is_number_unsigned()) {
        const auto value = (*persisted_entry)["preset_revision"].get<std::uint64_t>();
        if (value > 0 && value <= (std::numeric_limits<std::uint32_t>::max)())
            persisted_revision = static_cast<std::uint32_t>(value);
    }
    registry_->for_each_descriptor([&](const qt_view_descriptor_t& descriptor) {
        if (!workspace_manages_visibility(descriptor))
            return;
        bool desired = persisted_views == nullptr &&
            docking::preset_default_opens_view(preset, descriptor.id.value());
        if (const auto persisted = persisted_visibility_lookup(persisted_views, descriptor.id, registry_)) {
            desired = *persisted;
        } else if (persisted_views &&
            descriptor.preset_introduced_revision > persisted_revision &&
            descriptor.preset_introduced_revision <= docking::preset_revision(preset)) {
            desired = docking::preset_default_opens_view(
                preset, descriptor.id.value());
        }
        workspace_visibility_[descriptor.id] = desired;
        const view_instance_id_t instance{descriptor.id, {}};
        const bool open = registry_->is_open(instance);
        if (!desired && open) {
            if (actions_.close)
                static_cast<void>(actions_.close(instance));
        } else if (desired && !open) {
            const auto result = actions_.open
                ? actions_.open(instance)
                : view_operation_result_t{view_operation_status_t::unavailable, {}};
            if (!result.ok())
                deferred_workspace_opens_.insert(descriptor.id);
        }
    });
}

void ViewVisibilityController::retry_deferred() noexcept {
    for (auto iterator = deferred_workspace_opens_.begin();
         iterator != deferred_workspace_opens_.end();) {
        const stable_view_id_t id = *iterator;
        const auto desired = workspace_visibility_.find(id);
        if (desired == workspace_visibility_.end() || !desired->second) {
            iterator = deferred_workspace_opens_.erase(iterator);
            continue;
        }
        const view_instance_id_t instance{id, {}};
        if (registry_->is_open(instance) ||
            (actions_.open && actions_.open(instance).ok())) {
            iterator = deferred_workspace_opens_.erase(iterator);
            continue;
        }
        ++iterator;
    }
}

void ViewVisibilityController::capture() noexcept {
    if (capture_suspended_ || !active_context_)
        return;
    const auto context = active_context_();
    capture_for(context.first, context.second);
}

void ViewVisibilityController::capture_for(docking::workspace_preset_t preset,
    std::string_view identity) {
    const std::uint64_t registry_revision = registry_->visibility_revision();
    if (capture_valid_ &&
        capture_registry_revision_ == registry_revision &&
        capture_preset_ == preset &&
        capture_identity_ == identity)
        return;
    nlohmann::json root = read_visibility_root();
    root["version"] = 3;
    nlohmann::json views = nlohmann::json::object();
    registry_->for_each_descriptor([&](const qt_view_descriptor_t& descriptor) {
        if (!workspace_manages_visibility(descriptor))
            return;
        const view_instance_id_t instance{descriptor.id, {}};
        bool open = registry_->is_open(instance);
        if (deferred_workspace_opens_.find(descriptor.id) !=
            deferred_workspace_opens_.end()) {
            const auto desired = workspace_visibility_.find(descriptor.id);
            open = desired != workspace_visibility_.end() && desired->second;
        }
        workspace_visibility_[descriptor.id] = open;
        views[descriptor.id.value()] = open;
    });
    const std::string preset_key(identity);
    if (!root.contains("presets") || !root["presets"].is_object())
        root["presets"] = nlohmann::json::object();
    root["presets"][preset_key] = {
        {"preset_revision", docking::preset_revision(preset)},
        {"views", std::move(views)}};
    const std::string serialized = root.dump();
    if (serialized.size() > k_maximum_workspace_visibility_bytes)
        return;
    capture_valid_ = true;
    capture_registry_revision_ = registry_revision;
    capture_preset_ = preset;
    capture_identity_.assign(identity);
    if (serialized == g_sa_settings.workspace.view_visibility_json)
        return;
    g_sa_settings.workspace.view_visibility_json = serialized;
    static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
    Q_EMIT visibilityCaptured();
}

bool ViewVisibilityController::synchronize(docking::workspace_preset_t preset,
    std::string_view identity) noexcept {
    try {
        const bool changed = !preset_observed_ ||
            preset != observed_preset_ ||
            identity != observed_identity_;
        if (!changed)
            return false;
        begin_switch(preset, identity);
        finish_switch(preset, identity);
        return true;
    } catch (...) {
        return false;
    }
}

void ViewVisibilityController::begin_switch(docking::workspace_preset_t preset,
    std::string_view identity) noexcept {
    try {
        const bool changed = !preset_observed_ ||
            preset != observed_preset_ ||
            identity != observed_identity_;
        if (!changed)
            return;
        if (preset_observed_)
            capture_for(observed_preset_, observed_identity_);
        preset_observed_ = true;
        observed_preset_ = preset;
        observed_identity_ = identity;
    } catch (...) {
    }
}

void ViewVisibilityController::finish_switch(docking::workspace_preset_t preset,
    std::string_view identity) noexcept {
    try {
        restore(preset, identity);
        capture_valid_ = false;
        if (preset == docking::workspace_preset_t::safe) {
            if (actions_.open_or_focus)
                static_cast<void>(actions_.open_or_focus(stable_view_id_t("view.start_center")));
            if (actions_.suppress_start_center_auto_open)
                actions_.suppress_start_center_auto_open();
        }
    } catch (...) {
    }
}

void ViewVisibilityController::reset_persisted(docking::workspace_preset_t preset,
    bool all_presets) noexcept {
    try {
        nlohmann::json root = read_visibility_root();
        if (!root.contains("presets") || !root["presets"].is_object())
            root["presets"] = nlohmann::json::object();
        if (all_presets)
            root["presets"] = nlohmann::json::object();
        else {
            root["presets"].erase("builtin:" + docking::workspace_preset_key(preset));
            root["presets"].erase(docking::workspace_preset_key(preset));
        }
        write_visibility_root(root);
        if (all_presets) {
            capture_valid_ = false;
            preset_observed_ = false;
            observed_identity_.clear();
            workspace_visibility_.clear();
            deferred_workspace_opens_.clear();
        } else if (active_context_) {
            const auto context = active_context_();
            if (context.first == preset) {
                capture_valid_ = false;
                preset_observed_ = false;
                observed_identity_.clear();
                workspace_visibility_.clear();
                deferred_workspace_opens_.clear();
            }
        }
    } catch (...) {
    }
}

void ViewVisibilityController::clone_persisted(std::string_view source_identity,
    std::string_view target_identity) noexcept {
    try {
        if (source_identity.empty() || target_identity.empty() || source_identity == target_identity)
            return;
        capture_valid_ = false;
        nlohmann::json root = read_visibility_root();
        if (!root.contains("presets") || !root["presets"].is_object())
            root["presets"] = nlohmann::json::object();
        if (!root["presets"].contains(std::string(source_identity))) {
            if (active_context_)
                capture_for(active_context_().first, source_identity);
            root = read_visibility_root();
        }
        if (!root.contains("presets") || !root["presets"].is_object())
            return;
        auto& refreshed = root["presets"];
        if (!refreshed.contains(std::string(source_identity)))
            return;
        refreshed[std::string(target_identity)] = refreshed[std::string(source_identity)];
        write_visibility_root(root);
        preset_observed_ = false;
        capture_valid_ = false;
    } catch (...) {
    }
}

void ViewVisibilityController::rename_persisted(std::string_view source_identity,
    std::string_view target_identity) noexcept {
    try {
        if (source_identity.empty() || target_identity.empty() || source_identity == target_identity)
            return;
        nlohmann::json root = read_visibility_root();
        if (!root.contains("presets") || !root["presets"].is_object())
            return;
        auto& entries = root["presets"];
        const std::string source(source_identity);
        const std::string target(target_identity);
        if (!entries.contains(source))
            return;
        entries[target] = std::move(entries[source]);
        entries.erase(source);
        write_visibility_root(root);
        preset_observed_ = false;
        capture_valid_ = false;
    } catch (...) {
    }
}

void ViewVisibilityController::remove_persisted(std::string_view identity) noexcept {
    try {
        if (identity.empty())
            return;
        nlohmann::json root = read_visibility_root();
        if (!root.contains("presets") || !root["presets"].is_object())
            return;
        root["presets"].erase(std::string(identity));
        write_visibility_root(root);
        preset_observed_ = false;
        capture_valid_ = false;
    } catch (...) {
    }
}

}
