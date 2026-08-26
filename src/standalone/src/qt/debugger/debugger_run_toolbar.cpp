#include "qt/debugger/debugger_run_toolbar.hpp"

#include <QAction>

#include "core/debugger/debugger_engine.hpp"
#include "core/ui/application_ui_runtime.hpp"

#include "qt/bridge/action_bridge.hpp"
#include "qt/debugger/debugger_action_bridge.hpp"
#include "qt/debugger/debugger_mutation_queue.hpp"
#include "qt/debugger/debugger_session_controller.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_pill.hpp"

namespace aida::qt::debugger {

namespace {
constexpr const char* k_action_ids[8] = {
    "debugger.run_continue", "debugger.pause", "debugger.step_over",
    "debugger.step_into", "debugger.step_out", "debugger.stop",
    "debugger.restart", "debugger.detach"
};
}

DebuggerRunToolBar::DebuggerRunToolBar(QWidget* parent)
    : QToolBar(parent) {
    setObjectName(QStringLiteral("aida.debugger.run_toolbar"));
    const auto& tokens = theme::tokens();
    setIconSize(QSize(tokens.control.icon_glyph, tokens.control.icon_glyph));

    status_pill_ = new widgets::AidaPill(QStringLiteral("IDLE"),
        widgets::AidaSemantic::Neutral, this);
    status_pill_->setObjectName(QStringLiteral("aida.debugger.run_toolbar.status"));

    connect(&DebuggerSessionController::instance(),
        &DebuggerSessionController::sessionTick, this,
        &DebuggerRunToolBar::refreshState);
    connect(&DebuggerSessionController::instance(),
        &DebuggerSessionController::sessionStateChanged, this,
        &DebuggerRunToolBar::refreshState);
    connect(&DebuggerMutationQueue::instance(),
        &DebuggerMutationQueue::commandPendingChanged, this,
        &DebuggerRunToolBar::refreshState);
    connect(DebuggerActionBridge::instance().actions(),
        &bridge::ActionBridge::actions_rebuilt, this,
        &DebuggerRunToolBar::rebuildActions);
    rebuildActions();
    refreshState();
}

void DebuggerRunToolBar::rebuildActions() {
    auto* bridge = DebuggerActionBridge::instance().actions();
    if (!bridge)
        return;
    clear();
    for (const auto& action : actions_) {
        if (action)
            action->deleteLater();
    }
    actions_.clear();
    for (const char* id : k_action_ids) {
        QAction* action = bridge->surface_action(QString::fromLatin1(id),
            aida::ui::action_invocation_source_t::toolbar, this);
        if (!action)
            continue;
        addAction(action);
        actions_.push_back(action);
    }
    built_ = true;
}

void DebuggerRunToolBar::refreshState() {
    if (!built_)
        return;
    const bool command_pending =
        DebuggerMutationQueue::instance().commandPending();
    for (const auto& action : actions_) {
        if (!action)
            continue;
        const auto presentation = aida::ui::application_ui::present_action(
            action->data().toString().toUtf8().constData());
        QString tooltip = QString::fromStdString(presentation.label);
        if (!presentation.shortcut.empty())
            tooltip += QStringLiteral(" (") +
                QString::fromStdString(presentation.shortcut) +
                QStringLiteral(")");
        if (!presentation.enabled && !presentation.disabled_reason.empty())
            tooltip += QStringLiteral("\n") +
                QString::fromStdString(presentation.disabled_reason);
        if (command_pending)
            tooltip += QStringLiteral(
                "\nAnother debugger execution command is pending.");
        action->setEnabled(presentation.enabled && !command_pending);
        action->setToolTip(tooltip);
    }

    const auto status = debugger_engine::g_state.status.load(
        std::memory_order_acquire);
    const bool running = status == debugger_engine::dbg_status_t::running;
    const bool paused = status == debugger_engine::dbg_status_t::paused ||
        status == debugger_engine::dbg_status_t::stepping;
    QString label = QStringLiteral("IDLE");
    auto kind = widgets::AidaSemantic::Neutral;
    if (running) {
        label = QStringLiteral("RUNNING");
        kind = widgets::AidaSemantic::Success;
    } else if (paused) {
        label = QStringLiteral("PAUSED");
        kind = widgets::AidaSemantic::Warning;
    } else if (status == debugger_engine::dbg_status_t::terminated) {
        label = QStringLiteral("STOPPED");
        kind = widgets::AidaSemantic::Error;
    }
    status_pill_->setText(label);
    status_pill_->setKind(kind);
}

}
