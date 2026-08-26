#include "qt/debugger/debugger_action_bridge.hpp"

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_view.hpp"

#include "qt/bridge/action_bridge.hpp"
#include "qt/bridge/menu_bridge.hpp"
#include "qt/docking/dock_host.hpp"

namespace aida::qt::debugger {

DebuggerActionBridge& DebuggerActionBridge::instance() {
    static QPointer<DebuggerActionBridge> instance;
    if (!instance) {
        instance = new DebuggerActionBridge();
    }
    return *instance;
}

DebuggerActionBridge::DebuggerActionBridge(QObject* parent)
    : QObject(parent) {
}

void DebuggerActionBridge::install(docking::AidaDockHost* host,
                                   bridge::MenuBridge* menus,
                                   bridge::ActionBridge* actions) {
    host_ = host;
    menus_ = menus;
    actions_ = actions;
}

void DebuggerActionBridge::openView(const char* view_id) {
    if (!host_ || !view_id)
        return;
    static_cast<void>(host_->open_or_focus(
        registry::stable_view_id_t(view_id)));
}

void DebuggerActionBridge::showEntityMenu(
    const debugger_interaction::context_t& context,
    aida::ui::context_menu_open_origin_t origin, const QPoint& globalPos,
    QWidget* parent) {
    if (!menus_ || context.kind == debugger_interaction::kind_t::none)
        return;
    auto retained = debugger_view::build_debugger_entity_actions(context);
    if (retained.actions.empty())
        return;
    menus_->show_retained_entity_menu(retained, origin, globalPos, parent);
}

void DebuggerActionBridge::stageWatchExpression(const QString& expression) {
    pending_watch_expression_ = expression;
    openView("view.debug.watches");
    Q_EMIT watchExpressionStaged(expression);
}

QString DebuggerActionBridge::consumeWatchExpression() {
    QString out;
    out.swap(pending_watch_expression_);
    return out;
}

void DebuggerActionBridge::stageBreakpointAddress(
    quint64 address, bool hardwareExecute,
    const debugger_interaction::context_t& context) {
    pending_bp_staged_ = true;
    pending_bp_address_ = address;
    pending_bp_hw_ = hardwareExecute;
    pending_bp_context_ = context;
    openView("view.debug.breakpoints");
    Q_EMIT breakpointAddressStaged(address, hardwareExecute);
}

bool DebuggerActionBridge::consumeBreakpointStage(
    quint64& address, bool& hardwareExecute,
    debugger_interaction::context_t& context) {
    if (!pending_bp_staged_)
        return false;
    address = pending_bp_address_;
    hardwareExecute = pending_bp_hw_;
    context = pending_bp_context_;
    pending_bp_staged_ = false;
    pending_bp_context_ = {};
    return true;
}

void DebuggerActionBridge::focusPatchRow(int index) {
    pending_patch_row_ = index;
    openView("view.debug.patches");
    Q_EMIT patchRowFocusRequested(index);
}

int DebuggerActionBridge::consumePatchRowFocus() {
    const int out = pending_patch_row_;
    pending_patch_row_ = -1;
    return out;
}

}
