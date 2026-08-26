#pragma once

#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "qt/registry/qt_view_descriptor.hpp"

#include <QHash>
#include <QMenu>
#include <QObject>
#include <QPointer>
#include <QPoint>
#include <QString>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

class QMenu;
class QMenuBar;
class QWidget;

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::bridge {

class ActionBridge;
class InteractionContextProvider;
class MenuBridge;

class ContextMenuController : public QObject {
    Q_OBJECT
public:
    ContextMenuController(aida::ui::stable_menu_id_t menu,
                          ActionBridge* actions,
                          QObject* parent = nullptr);

    void show(aida::ui::interaction_context_t snapshot,
              aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos,
              QWidget* parent);
    void close_active();
    bool active() const noexcept { return active_menu_ != nullptr; }

private:
    void compose_now();
    void execute_item(const QString& action_id);

    ActionBridge* actions_ = nullptr;
    aida::ui::stable_menu_id_t menu_id_;
    aida::ui::interaction_context_t snapshot_;
    aida::ui::context_menu_open_request_t request_;
    QPointer<QMenu> active_menu_;
};

class MenuBridge : public QObject {
    Q_OBJECT
public:
    enum class top_menu_t : std::uint8_t {
        file, edit, view, navigate, analysis, debugger_, memory, types,
        network, workspace, tools, ai, help
    };

    static QString top_menu_object_name(top_menu_t which);

    struct menu_item_spec_t {
        std::string action_id;
        std::string label_override;
        std::string shortcut_override;
    };
    struct menu_section_spec_t {
        std::string label;
        std::vector<menu_item_spec_t> items;
    };

    MenuBridge(ActionBridge* actions, InteractionContextProvider* context,
               QObject* parent = nullptr);

    QMenuBar* installBar(docking::AidaDockHost* host);
    QMenuBar* bar() const noexcept { return bar_; }
    QMenu* menu(top_menu_t which) const;

    void register_menu_contents(top_menu_t menu,
                                std::vector<menu_section_spec_t> sections);

    void show_context_menu(const aida::ui::stable_menu_id_t& menu,
                           aida::ui::interaction_context_t snapshot,
                           aida::ui::context_menu_open_origin_t origin,
                           const QPoint& global_pos, QWidget* parent);
    void show_retained_entity_menu(
        const aida::ui::application_ui::retained_entity_context_t& context,
        aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos,
        QWidget* parent);

Q_SIGNALS:
    void saved_workspace_load_failed(const QString& name, quint64 generation,
                                     const QString& message);

private:
    struct menu_stamp_t {
        std::uint64_t action_revision = 0;
        std::uint64_t shortcut_revision = 0;
        std::uint64_t structure_revision = 0;
        std::uint64_t context_generation = 0;
        bool valid = false;
    };

    bool rebuild_registered_menu(top_menu_t which);
    bool rebuild_view_menu();
    bool rebuild_types_menu();
    bool rebuild_workspace_menu();
    void rebuild_saved_workspaces(QMenu* submenu);
    void append_spec_items(QMenu* target,
                           const std::vector<menu_section_spec_t>& sections);
    void append_registry_entries(top_menu_t which, QMenu* target);
    void append_view_entries(
        QMenu* target,
        std::optional<aida::qt::registry::view_category_t> only_category,
        bool group_by_category);
    bool menu_cache_hit(top_menu_t which) const;
    void note_menu_built(top_menu_t which);
    void refresh_menu_states(QMenu* target);
    void invalidate_menu_structure();

    ActionBridge* actions_ = nullptr;
    InteractionContextProvider* context_ = nullptr;
    docking::AidaDockHost* host_ = nullptr;
    QMenuBar* bar_ = nullptr;
    std::map<top_menu_t, QMenu*> menus_;
    std::map<top_menu_t, std::vector<menu_section_spec_t>> registered_;
    std::map<top_menu_t, menu_stamp_t> menu_stamps_;
    std::uint64_t structure_revision_ = 0;
    bool saved_menu_built_ = false;
    std::uint64_t saved_menu_structure_revision_ = 0;
    QHash<QString, ContextMenuController*> context_controllers_;
};

}
