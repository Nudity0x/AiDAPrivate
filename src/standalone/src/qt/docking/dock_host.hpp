#pragma once

#include "qt/registry/qt_view_registry.hpp"
#include "qt/registry/surface_state.hpp"
#include "qt/docking/preset_recipes.hpp"

#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

class QMainWindow;
class QSplitterHandle;
class QEvent;

namespace ads {
class CDockManager;
class CDockWidget;
class CDockAreaWidget;
}

namespace aida::qt::registry {
class ViewVisibilityController;
}

namespace aida::qt::layout {
class WorkspacePersistenceController;
class MonitorRehomeController;
}

namespace aida::qt::docking {

class AidaHubWidget;

enum class dock_region_t {
    left,
    right,
    bottom,
    center
};

class AidaDockHost : public QObject {
    Q_OBJECT
public:
    struct code_group_hooks_t {
        std::function<std::string(std::uint32_t group)> label_for_group;
        std::function<registry::view_operation_result_t(
            const registry::view_instance_id_t& instance)> prepare_close;
        std::function<void(const registry::view_instance_id_t& instance)> close_group;
        std::function<std::set<std::uint32_t>()> current_groups;
        std::function<std::uint32_t()> active_group;
    };

    explicit AidaDockHost(QMainWindow* window, QObject* parent = nullptr);
    ~AidaDockHost() override;

    ads::CDockManager* manager() const noexcept { return manager_; }
    registry::qt_view_registry_t* registry() const noexcept { return registry_; }
    registry::surface_state_store_t* surfaces() noexcept { return &surfaces_; }
    registry::ViewVisibilityController* visibility() const noexcept { return visibility_; }
    layout::WorkspacePersistenceController* persistence() const noexcept { return persistence_; }

    bool restoreOrBuildDefault();
    void persistNow();

    registry::view_operation_result_t install_view_factory(
        const registry::stable_view_id_t& id,
        registry::qt_view_factory_t factory,
        std::optional<registry::content_policy_t> policy = std::nullopt);
    bool is_ported(const registry::stable_view_id_t& id) const;

    registry::view_operation_result_t open_or_focus(const registry::stable_view_id_t& id);
    registry::view_operation_result_t open_for_layout(const registry::stable_view_id_t& id);
    registry::view_operation_result_t close(const registry::stable_view_id_t& id);
    registry::view_operation_result_t close_instance(const registry::view_instance_id_t& id);
    registry::view_operation_result_t close_other_instances(const registry::view_instance_id_t& keep);
    registry::view_operation_result_t toggle_pin(const registry::view_instance_id_t& id);
    registry::view_operation_result_t request_reset_state(const registry::view_instance_id_t& id);
    registry::view_operation_result_t duplicate_instance(const registry::view_instance_id_t& id);
    registry::view_operation_result_t reopen_last_closed();
    registry::view_operation_result_t open_default_missing();
    bool is_open(const registry::stable_view_id_t& id) noexcept;
    bool is_pinned(const registry::view_instance_id_t& id) noexcept;
    bool can_duplicate(const registry::view_instance_id_t& id) noexcept;
    bool can_reset_state(const registry::view_instance_id_t& id) noexcept;
    bool can_reopen_last_closed() noexcept;
    std::string focused_disassembly_presentation_key() noexcept;
    void for_each_menu_entry(
        const std::function<void(const registry::menu_entry_t&)>& visitor);
    void dismiss_start_center_when_work_available() noexcept;
    void notify_workspace_availability_changed();

    registry::view_operation_result_t float_instance(const registry::view_instance_id_t& id);
    registry::view_operation_result_t move_instance(const registry::view_instance_id_t& id,
                                                    dock_region_t region);

    AidaHubWidget* hub_widget(registry::hub_kind_t hub) const;
    void activate_hub_subview(registry::hub_kind_t hub, int subview);

    void set_code_group_hooks(code_group_hooks_t hooks);
    void synchronize_code_groups();
    registry::view_instance_id_t active_code_instance();

    void set_hex_close_hook(std::function<void()> hook);
    void set_work_indicator_hook(std::function<bool()> hook);

    void set_layout_locked(bool locked);
    bool layout_locked() const noexcept { return layout_locked_; }
    void refresh_splitter_locks();
    void wire_layout_watchers();

    bool eventFilter(QObject* watched, QEvent* event) override;

    void preregister_all_docks();
    bool apply_state(const std::string& dock_xml);
    QByteArray capture_state();
    std::string capture_surface_json();
    bool load_surface_json(const std::string& json_text);
    void apply_surface_state();
    void apply_hub_subview_selections();
    void build_preset_layout(workspace_preset_t preset, bool missing_only = false);
    workspace_request_result_t open_missing_views();
    bool is_restoring() const noexcept { return restoring_; }

    QWidget* create_content(const registry::qt_view_descriptor_t& descriptor,
                            const registry::view_instance_id_t& instance, QWidget* parent);

Q_SIGNALS:
    void layoutDirty();
    void dockFocusGained(const QString& objectName);

private:
    struct dock_record_t {
        registry::view_instance_id_t instance;
        bool content_disposed = false;
        bool content_pending = false;
    };

    ads::CDockWidget* ensure_dock(const registry::qt_view_descriptor_t& descriptor,
                                  const registry::view_instance_id_t& id,
                                  const std::string& display_name);
    ads::CDockWidget* create_dock(const registry::qt_view_descriptor_t& descriptor,
                                  const registry::view_instance_id_t& id,
                                  const std::string& display_name,
                                  bool defer_content = false);
    ads::CDockWidget* dock_for(const registry::view_instance_id_t& id) const;
    ads::CDockWidget* ensure_hub_dock(registry::hub_kind_t hub, bool defer_pages = false);
    bool realize_deferred_content(ads::CDockWidget* dock, dock_record_t& record);
    ads::CDockWidget* hub_dock(registry::hub_kind_t hub) const;
    void wire_dock(ads::CDockWidget* dock, const registry::view_instance_id_t& id);
    void wire_hub_dock(ads::CDockWidget* dock, registry::hub_kind_t hub);
    void dispose_content_for_close(ads::CDockWidget* dock, dock_record_t& record);
    void ensure_content_for_open(ads::CDockWidget* dock,
                                 const registry::qt_view_descriptor_t& descriptor,
                                 const registry::view_instance_id_t& id,
                                 dock_record_t& record);
    void apply_pin(ads::CDockWidget* dock, bool pinned);
    void realize_open_instances();

    registry::view_operation_result_t host_open(const registry::qt_view_descriptor_t& descriptor,
                                                const registry::view_instance_id_t& id,
                                                const std::string& display_name);
    registry::view_operation_result_t host_focus(const registry::view_instance_id_t& id);
    registry::view_operation_result_t host_close(const registry::view_instance_id_t& id);
    registry::view_operation_result_t host_erase(const registry::view_instance_id_t& id);
    void host_deactivate(const registry::view_instance_id_t& id);

    int default_area_for(const registry::qt_view_descriptor_t& descriptor) const;
    void place_new_dock(ads::CDockWidget* dock, const registry::qt_view_descriptor_t& descriptor);
    void reconcile_from_manager();
    void replay_pending_surface_openings();
    void handle_focused_dock_changed(ads::CDockWidget* now);
    void mark_dirty(const char* reason);
    void update_empty_state();
    void position_empty_state();
    void update_start_center_auto_open();

    std::map<std::string, dock_record_t> docks_;
    std::map<registry::hub_kind_t, QPointer<ads::CDockWidget>> hub_docks_;
    std::map<registry::hub_kind_t, QPointer<AidaHubWidget>> hub_widgets_;
    QMainWindow* window_ = nullptr;
    ads::CDockManager* manager_ = nullptr;
    QWidget* empty_state_ = nullptr;
    registry::qt_view_registry_t* registry_ = nullptr;
    registry::surface_state_store_t surfaces_;
    registry::ViewVisibilityController* visibility_ = nullptr;
    layout::WorkspacePersistenceController* persistence_ = nullptr;
    layout::MonitorRehomeController* rehome_ = nullptr;
    QObject* splitter_lock_filter_ = nullptr;
    std::vector<QPointer<QObject>> watched_splitters_;
    std::vector<QPointer<QObject>> watched_areas_;
    code_group_hooks_t code_group_hooks_;
    std::function<void()> hex_close_hook_;
    std::function<bool()> work_indicator_hook_;
    bool restoring_ = false;
    bool layout_locked_ = false;
    bool start_center_auto_open_ = true;
    bool catalog_registered_ = false;
};

}
