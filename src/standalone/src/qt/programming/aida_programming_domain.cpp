#include "qt/programming/aida_programming_domain.hpp"

#include <DockManager.h>

#include "helpers/diag_log.hpp"

#include "qt/docking/dock_host.hpp"
#include "qt/programming/aida_language_views.hpp"
#include "qt/programming/aida_output_pane.hpp"
#include "qt/programming/aida_programming_tasks.hpp"
#include "qt/programming/aida_task_center_view.hpp"
#include "qt/programming/aida_terminal_view.hpp"
#include "qt/programming/aida_workspace_search_view.hpp"
#include "qt/programming/programming_host_hooks.hpp"
#include "qt/registry/qt_view_registry.hpp"

namespace aida::qt::programming {
namespace {

void register_programming_view_factories(docking::AidaDockHost* host) {
    if (!host)
        return;
    using registry::stable_view_id_t;
    const auto install = [host](const char* id, registry::qt_view_factory_t factory) {
        const auto result = host->install_view_factory(stable_view_id_t(id),
            std::move(factory));
        if (!result.ok())
            diag::log_tagged_fmt("qt_programming",
                "view_factory_install_failed view=%s status=%d detail=%s", id,
                static_cast<int>(result.status), result.detail.c_str());
    };

    install("view.output", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new AidaOutputViewHost(bottom_tab_t::output, QString(), parent);
    });
    install("view.mcp_log", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new AidaOutputViewHost(bottom_tab_t::mcp_log, QString(), parent);
    });
    install("view.driver_log", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new AidaOutputViewHost(bottom_tab_t::driver_log, QString(), parent);
    });
    install("view.sandbox_log", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new AidaOutputViewHost(bottom_tab_t::sandbox_log, QString(), parent);
    });
    install("view.terminal", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new AidaTerminalView(parent);
    });
    install("view.background_tasks", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new AidaTaskCenterView(parent);
    });
    install("view.diagnostics", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new AidaDiagnosticsView(parent);
    });
    install("view.programming.outline", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new AidaOutlineView(parent);
    });
    install("view.programming.references", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new AidaReferencesView(parent);
    });
    install("view.programming.source_debug_console", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new AidaSourceDebugConsole(parent);
    });
    install("view.workspace_search", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new AidaWorkspaceSearchView(parent);
    });
    install("view.ai.scripts", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new AidaAutomationScriptsView(parent);
    });

    diag::log_tagged("qt_programming", "programming_view_factories_registered");
}

}

void install_programming_domain(docking::AidaDockHost* host,
                                bridge::MenuBridge* menus,
                                bridge::ActionBridge* actions) {
    if (!host)
        return;
    static_cast<void>(menus);
    static_cast<void>(actions);
    QWidget* window = host->manager() ? host->manager()->window() : nullptr;
    AidaOutputController::instance().install(host);
    AidaTerminalController::instance().install(host);
    AidaTaskCenterController::instance().install(host);
    AidaLanguageServiceBridge::instance().install();
    AidaProgrammingTasksController::instance().install(host, window);
    host::install(host);
    QObject::connect(&AidaProgrammingTasksController::instance(),
        &AidaProgrammingTasksController::channelSelectionChanged,
        &AidaOutputController::instance(),
        [] { AidaOutputController::instance().noteExternalChannelChange(); },
        Qt::DirectConnection);
    register_programming_view_factories(host);
    globals::terminal_mgr = &AidaTerminalController::instance().manager();
    diag::log_tagged("qt_programming", "programming_domain_installed");
}

}
