#pragma once

#include "qt/registry/qt_view_registry.hpp"
#include "qt/docking/preset_recipes.hpp"

#include <QObject>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace aida::qt::registry {

class ViewVisibilityController : public QObject {
    Q_OBJECT
public:
    struct view_actions_t {
        std::function<view_operation_result_t(const view_instance_id_t&)> open;
        std::function<view_operation_result_t(const view_instance_id_t&)> close;
        std::function<view_operation_result_t(const stable_view_id_t&)> open_or_focus;
        std::function<void()> suppress_start_center_auto_open;
    };

    explicit ViewVisibilityController(qt_view_registry_t* registry, QObject* parent = nullptr);

    void set_view_actions(view_actions_t actions);
    void set_active_context_provider(
        std::function<std::pair<docking::workspace_preset_t, std::string>()> provider);

    bool synchronize(docking::workspace_preset_t preset, std::string_view identity) noexcept;
    void begin_switch(docking::workspace_preset_t preset, std::string_view identity) noexcept;
    void finish_switch(docking::workspace_preset_t preset, std::string_view identity) noexcept;
    void capture() noexcept;
    void retry_deferred() noexcept;
    void set_capture_suspended(bool suspended) noexcept { capture_suspended_ = suspended; }

    void reset_persisted(docking::workspace_preset_t preset, bool all_presets) noexcept;
    void clone_persisted(std::string_view source_identity, std::string_view target_identity) noexcept;
    void rename_persisted(std::string_view source_identity, std::string_view target_identity) noexcept;
    void remove_persisted(std::string_view identity) noexcept;

Q_SIGNALS:
    void visibilityCaptured();

private:
    void restore(docking::workspace_preset_t preset, std::string_view identity);
    void capture_for(docking::workspace_preset_t preset, std::string_view identity);

    qt_view_registry_t* registry_ = nullptr;
    view_actions_t actions_;
    std::function<std::pair<docking::workspace_preset_t, std::string>()> active_context_;
    std::map<stable_view_id_t, bool> workspace_visibility_;
    std::set<stable_view_id_t> deferred_workspace_opens_;
    bool preset_observed_ = false;
    bool capture_valid_ = false;
    std::uint64_t capture_registry_revision_ = 0;
    std::string capture_identity_;
    docking::workspace_preset_t capture_preset_ = docking::workspace_preset_t::analysis;
    docking::workspace_preset_t observed_preset_ = docking::workspace_preset_t::analysis;
    std::string observed_identity_;
    bool capture_suspended_ = false;
};

}
