#include "qt/debugger/debugger_domain.hpp"

#include <QApplication>
#include <QFileDialog>
#include <QWidget>

#include <optional>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_view.hpp"
#include "core/ui/application_ui_runtime.hpp"

#include "qt/bridge/action_bridge.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/menu_bridge.hpp"
#include "qt/debugger/breakpoints_pane.hpp"
#include "qt/debugger/bookmarks_pane.hpp"
#include "qt/debugger/call_stack_pane.hpp"
#include "qt/debugger/cpu_pane.hpp"
#include "qt/debugger/debugger_action_bridge.hpp"
#include "qt/debugger/debugger_cfg_pane.hpp"
#include "qt/debugger/debugger_mutation_queue.hpp"
#include "qt/debugger/debugger_session_controller.hpp"
#include "qt/debugger/dialogs/breakpoint_edit_dialog.hpp"
#include "qt/debugger/dialogs/change_protection_dialog.hpp"
#include "qt/debugger/dialogs/code_caves_dialog.hpp"
#include "qt/debugger/dialogs/confirm_dialogs.hpp"
#include "qt/debugger/dialogs/register_edit_dialog.hpp"
#include "qt/debugger/dialogs/spawn_target_dialog_qt.hpp"
#include "qt/debugger/dialogs/stage_patch_dialog.hpp"
#include "qt/debugger/handles_pane.hpp"
#include "qt/debugger/memory_map_pane.hpp"
#include "qt/debugger/modules_pane.hpp"
#include "qt/debugger/patches_pane.hpp"
#include "qt/debugger/seh_pane.hpp"
#include "qt/debugger/source_pane.hpp"
#include "qt/debugger/strings_pane.hpp"
#include "qt/debugger/threads_pane.hpp"
#include "qt/debugger/trace_pane.hpp"
#include "qt/debugger/watches_pane.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/registry/qt_view_registry.hpp"

namespace aida::qt::debugger {

namespace {

// The backend hook surface speaks aida::ui::view_operation_result_t
// (shell_host_contract.hpp); the dock host speaks
// aida::qt::registry::view_operation_result_t (qt_view_descriptor.hpp). Same
// field layout and same enumerator order; convert field-wise.
aida::ui::view_operation_result_t to_ui_result(
    const registry::view_operation_result_t& result) {
    aida::ui::view_operation_result_t out;
    out.status = static_cast<aida::ui::view_operation_status_t>(
        static_cast<int>(result.status));
    out.detail = result.detail;
    return out;
}

void install_backend_hooks() {
    debugger_view::host_ui_hooks_t hooks;

    hooks.open_or_focus = [](const char* view_id)
        -> aida::ui::view_operation_result_t {
        auto* host = DebuggerActionBridge::instance().host();
        if (!host)
            return aida::ui::view_operation_result_t{};
        return to_ui_result(host->open_or_focus(
            registry::stable_view_id_t(view_id ? view_id : "")));
    };
    hooks.is_open = [](const char* view_id) {
        auto* host = DebuggerActionBridge::instance().host();
        return host &&
            host->is_open(registry::stable_view_id_t(view_id ? view_id : ""));
    };
    hooks.close_view = [](const char* view_id)
        -> aida::ui::view_operation_result_t {
        auto* host = DebuggerActionBridge::instance().host();
        if (!host)
            return aida::ui::view_operation_result_t{};
        return to_ui_result(host->close(
            registry::stable_view_id_t(view_id ? view_id : "")));
    };
    hooks.clipboard_set = [](const std::string& text) {
        clipboard::set_text(QString::fromStdString(text));
    };
    hooks.request_spawn_dialog = [] {
        SpawnTargetDialogQt::requestOpen(QApplication::activeWindow());
    };
    hooks.request_spawn_dialog_with_path = [](const std::string& path) {
        SpawnTargetDialogQt::requestOpenWithPath(path,
            QApplication::activeWindow());
    };
    hooks.spawn_dialog_open = [] {
        return SpawnTargetDialogQt::isOpen();
    };
    hooks.present_register_edit =
        [](const debugger_interaction::context_t& context,
           const std::string& name, std::uint64_t value) {
            RegisterEditDialog::openFor(context,
                QString::fromStdString(name), value,
                QApplication::activeWindow());
        };
    hooks.present_breakpoint_edit =
        [](const debugger_interaction::context_t& context, int index,
           int focus) {
            BreakpointEditDialog::openFor(context, index,
                static_cast<debugger_view::breakpoint_editor_focus_t>(focus),
                QApplication::activeWindow());
        };
    hooks.present_change_protection =
        [](const debugger_interaction::context_t& context,
           std::uint64_t address, std::uint64_t size, std::uint32_t old_protect) {
            ChangeProtectionDialog::openFor(context, address, size, old_protect,
                QApplication::activeWindow());
        };
    hooks.present_context_mutation_review =
        [](int mutation, const debugger_interaction::context_t& context) {
            confirm_dialogs::present_mutation(
                static_cast<debugger_view::context_mutation_t>(mutation),
                context, QApplication::activeWindow());
        };
    hooks.present_patch_stage =
        [](const debugger_view::patch_stage_review_t& review) {
            StagePatchDialog::present(review, QApplication::activeWindow());
        };
    hooks.present_code_caves = [] {
        CodeCavesDialog::present(QApplication::activeWindow());
    };
    hooks.pick_save_file = [](const std::string& title,
                              const std::string& default_name,
                              const std::string& filter) {
        const QString picked = QFileDialog::getSaveFileName(
            QApplication::activeWindow(), QString::fromStdString(title),
            QString::fromStdString(default_name),
            QString::fromStdString(filter));
        if (picked.isEmpty())
            return std::optional<std::string>();
        return std::optional<std::string>(picked.toStdString());
    };
    hooks.stage_watch_expression = [](const std::string& expression) {
        DebuggerActionBridge::instance().stageWatchExpression(
            QString::fromStdString(expression));
    };
    hooks.focus_patch_row = [](int index) {
        DebuggerActionBridge::instance().focusPatchRow(index);
    };
    hooks.present_breakpoint_stage =
        [](std::uint64_t address, int mode,
           const debugger_interaction::context_t& context) {
            DebuggerActionBridge::instance().stageBreakpointAddress(address,
                mode == static_cast<int>(
                    debugger_view::breakpoint_definition_mode_t::hardware_execute),
                context);
        };

    debugger_view::install_host_ui_hooks(std::move(hooks));
}

void register_debugger_view_factories(docking::AidaDockHost* host) {
    if (!host)
        return;
    using registry::stable_view_id_t;
    const auto install = [host](const char* id,
                                registry::qt_view_factory_t factory) {
        const auto result = host->install_view_factory(stable_view_id_t(id),
            std::move(factory));
        if (!result.ok())
            diag::log_tagged_fmt("qt_debugger",
                "view_factory_install_failed view=%s status=%d detail=%s", id,
                static_cast<int>(result.status), result.detail.c_str());
    };

    install("view.debug.cpu", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new CpuPaneWidget(CpuPaneWidget::Surface::integrated, parent);
    });
    install("view.debug.registers", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new CpuPaneWidget(CpuPaneWidget::Surface::registers_only,
            parent);
    });
    install("view.debug.stack", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new CpuPaneWidget(CpuPaneWidget::Surface::stack_only, parent);
    });
    install("view.debug.breakpoints", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new BreakpointsPane(parent);
    });
    install("view.debug.memory_map", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new MemoryMapPane(parent);
    });
    install("view.debug.call_stack", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new CallStackPane(parent);
    });
    install("view.debug.threads", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new ThreadsPane(parent);
    });
    install("view.debug.watches", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new WatchesPane(parent);
    });
    install("view.debug.handles", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new HandlesPane(parent);
    });
    install("view.debug.trace", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new TracePane(parent);
    });
    install("view.debug.strings", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new StringsPane(parent);
    });
    install("view.debug.bookmarks", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new BookmarksPane(parent);
    });
    install("view.debug.modules", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new ModulesPane(parent);
    });
    install("view.debug.patches", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new PatchesPane(parent);
    });
    install("view.debug.seh", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new SehPane(parent);
    });
    install("view.debug.cfg", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new DebuggerCfgPane(parent);
    });
    install("view.debug.source", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new SourcePane(parent);
    });

    diag::log_tagged("qt_debugger", "debugger_view_factories_registered");
}

}

void install_debugger_domain(docking::AidaDockHost* host,
                             bridge::MenuBridge* menus,
                             bridge::ActionBridge* actions) {
    if (!host)
        return;
    DebuggerActionBridge::instance().install(host, menus, actions);
    DebuggerSessionController::instance().install();
    static_cast<void>(DebuggerMutationQueue::instance());
    QObject::connect(&DebuggerSessionController::instance(),
        &DebuggerSessionController::sessionTick,
        &DebuggerMutationQueue::instance(),
        &DebuggerMutationQueue::noteTick);
    install_backend_hooks();
    register_debugger_view_factories(host);
    diag::log_tagged("qt_debugger", "debugger_domain_installed");
}

}
