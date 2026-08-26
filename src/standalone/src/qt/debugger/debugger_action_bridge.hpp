#pragma once

#include <QObject>
#include <QPoint>
#include <QPointer>

#include <cstdint>
#include <vector>

#include "core/debugger/debugger_interaction_context.hpp"
#include "core/ui/context_menu_contract.hpp"

class QToolBar;

namespace aida::qt::bridge {
class ActionBridge;
class MenuBridge;
}
namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::debugger {

class DebuggerRunToolBar;

// Registry->QAction consumption for the debugger domain plus the retained
// entity context menus (aboutToShow revalidation via the contract's
// validate_identity, composed per open by the W2.2 menu bridge).
class DebuggerActionBridge : public QObject {
    Q_OBJECT
public:
    static DebuggerActionBridge& instance();

    void install(docking::AidaDockHost* host, bridge::MenuBridge* menus,
                 bridge::ActionBridge* actions);
    bool installed() const noexcept { return actions_ != nullptr; }

    docking::AidaDockHost* host() const noexcept { return host_; }
    bridge::MenuBridge* menus() const noexcept { return menus_; }
    bridge::ActionBridge* actions() const noexcept { return actions_; }

    void openView(const char* view_id);

    void showEntityMenu(const debugger_interaction::context_t& context,
                        aida::ui::context_menu_open_origin_t origin,
                        const QPoint& globalPos, QWidget* parent);

    // Cross-pane staged handoffs (the backend entity actions stage into a pane
    // that may not be open; the pane adopts the pending value on construct or
    // via the signal when live).
    void stageWatchExpression(const QString& expression);
    QString consumeWatchExpression();
    void stageBreakpointAddress(
        quint64 address, bool hardwareExecute,
        const debugger_interaction::context_t& context);
    bool consumeBreakpointStage(quint64& address, bool& hardwareExecute,
        debugger_interaction::context_t& context);
    void focusPatchRow(int index);
    int consumePatchRowFocus();

Q_SIGNALS:
    void watchExpressionStaged(const QString& expression);
    void breakpointAddressStaged(quint64 address, bool hardwareExecute);
    void patchRowFocusRequested(int index);

private:
    explicit DebuggerActionBridge(QObject* parent = nullptr);

    docking::AidaDockHost* host_ = nullptr;
    bridge::MenuBridge* menus_ = nullptr;
    bridge::ActionBridge* actions_ = nullptr;
    QString pending_watch_expression_;
    int pending_patch_row_ = -1;
    bool pending_bp_staged_ = false;
    bool pending_bp_hw_ = false;
    quint64 pending_bp_address_ = 0;
    debugger_interaction::context_t pending_bp_context_{};
};

}
