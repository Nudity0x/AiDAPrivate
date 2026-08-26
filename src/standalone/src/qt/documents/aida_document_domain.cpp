#include "qt/documents/aida_document_domain.hpp"

#include <QWidget>

#include <DockManager.h>

#include <optional>
#include <string>
#include <utility>

#include "helpers/diag_log.hpp"
#include "qt/analysis_bridge/disasm_workspace_model.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/bridge/menu_bridge.hpp"
#include "qt/disasm/aida_disasm_view.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/documents/aida_document_controller.hpp"
#include "qt/documents/aida_document_model.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/editor/aida_code_document.hpp"
#include "qt/editor/aida_code_editor.hpp"
#include "qt/editor/aida_code_group_host.hpp"
#include "qt/editor/aida_hex_editor.hpp"
#include "qt/editor/aida_image_view.hpp"
#include "qt/explorer/aida_explorer_view.hpp"
#include "qt/explorer/exchange/open_dispatch.hpp"
#include "qt/graph/aida_workspace_graph_view.hpp"
#include "qt/pseudocode/aida_pseudocode_view.hpp"
#include "qt/registry/qt_view_registry.hpp"

namespace aida::qt::documents {

namespace {

bool parse_group_key(const std::string& key, std::uint32_t& group)
{
    constexpr const char* prefix = "group.";
    if (key.compare(0, 6, prefix) != 0)
        return false;
    try {
        group = static_cast<std::uint32_t>(std::stoul(key.substr(6)));
        return true;
    } catch (...) {
        return false;
    }
}

}

document_domain_t& document_domain()
{
    static document_domain_t domain;
    return domain;
}

void install_document_domain(docking::AidaDockHost* host, bridge::MenuBridge* menus)
{
    if (!host)
        return;
    auto& domain = document_domain();
    if (!domain.model)
        domain.model = new AidaDocumentModel(host);
    if (!domain.controller)
        domain.controller = new AidaDocumentController(domain.model, host);
    auto* controller = domain.controller;
    auto* code_registry = &editor::AidaCodeDocumentRegistry::instance();

    controller->model()->syncFromBackend();

    host->install_view_factory(registry::stable_view_id_t("document.code"),
        [controller, code_registry](QWidget* parent, const registry::view_instance_id_t& instance) -> QWidget* {
            std::uint32_t group = 0;
            if (!parse_group_key(instance.instance.value(), group))
                group = 0;
            return new editor::AidaCodeGroupHost(group, controller, code_registry, parent);
        });

    host->install_view_factory(registry::stable_view_id_t("document.hex"),
        [host](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
            auto* editor_widget = new editor::AidaHexEditor(parent);
            QObject::connect(editor_widget, &editor::AidaHexEditor::openDisassemblyRequested,
                host, [host] {
                    host->open_or_focus(registry::stable_view_id_t("document.disassembly"));
                });
            return editor_widget;
        });

    host->install_view_factory(registry::stable_view_id_t("document.image"),
        [](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
            auto* view = new editor::AidaImageView(parent);
            explorer::AidaOpenDispatch::instance().setImageView(view);
            return view;
        });

    host->install_view_factory(registry::stable_view_id_t("view.project_explorer"),
        [controller](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
            auto* view = new explorer::AidaExplorerView(parent);
            view->setDocumentModel(controller->model());
            explorer::AidaOpenDispatch::instance().setExplorerModel(view->model());
            return view;
        });

    host->install_view_factory(registry::stable_view_id_t("document.disassembly"),
        [](QWidget* parent, const registry::view_instance_id_t& instance) -> QWidget* {
            return new disasm::AidaDisasmView(instance.instance.value(), parent);
        });

    host->install_view_factory(registry::stable_view_id_t("document.pseudocode"),
        [](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
            return new pseudocode::AidaPseudocodeView(parent);
        });

    host->install_view_factory(registry::stable_view_id_t("document.graph"),
        [](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
            return new graph::AidaWorkspaceGraphView(parent);
        });

    analysis_bridge::set_view_focus_hook([host](const char* view_id) {
        if (host && view_id)
            static_cast<void>(host->open_or_focus(registry::stable_view_id_t(view_id)));
    });
    analysis_bridge::set_focused_presentation_key_hook([host]() -> std::string {
        return host ? host->focused_disassembly_presentation_key() : std::string();
    });
    analysis_bridge::set_export_path_hook([host](
            const disasm_view::workspace_context_t&) -> std::optional<std::string> {
        static const char k_export_filter[] =
            "Text files (*.txt)\0*.txt\0"
            "All files (*.*)\0*.*\0\0";
        return dialogs::save_file(host ? host->manager() : nullptr,
            QStringLiteral("Export Disassembly Listing"), k_export_filter,
            QStringLiteral("txt"));
    });

    docking::AidaDockHost::code_group_hooks_t hooks;
    hooks.label_for_group = [controller](std::uint32_t group) {
        return controller->labelForGroup(group);
    };
    hooks.prepare_close = [controller](
            const registry::view_instance_id_t& instance) -> registry::view_operation_result_t {
        std::uint32_t group = 0;
        if (!parse_group_key(instance.instance.value(), group))
            return {registry::view_operation_status_t::invalid_instance,
                "The editor group identity is invalid"};
        std::string reason;
        if (controller->prepareGroupClose(group, reason))
            return {};
        return {registry::view_operation_status_t::unavailable, reason};
    };
    hooks.close_group = [controller](const registry::view_instance_id_t& instance) {
        std::uint32_t group = 0;
        if (parse_group_key(instance.instance.value(), group))
            controller->closeGroup(group);
    };
    hooks.current_groups = [controller] {
        const auto groups = controller->currentGroups();
        return std::set<std::uint32_t>(groups.begin(), groups.end());
    };
    hooks.active_group = [controller] {
        return controller->activeGroup();
    };
    host->set_code_group_hooks(std::move(hooks));

    auto& dispatch = explorer::AidaOpenDispatch::instance();
    dispatch.setDocumentController(controller);
    dispatch.setViewFocusHook([host](const std::string& view_id) {
        host->open_or_focus(registry::stable_view_id_t(view_id));
    });

    if (menus) {
        set_context_menu_display(
            [menus](const aida::ui::stable_menu_id_t& menu,
                    aida::ui::interaction_context_t snapshot,
                    aida::ui::context_menu_open_origin_t origin,
                    const QPoint& global_pos, QWidget* parent) {
                menus->show_context_menu(menu, std::move(snapshot), origin, global_pos, parent);
            });
    }

    diag::log_tagged("qt_documents", "document_domain_installed");
}

}
