#pragma once

#include "qt/bridge/aida_dialog.hpp"

#include <QPointer>
#include <QString>

#include <string>

#include "core/debugger/debugger_view.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace aida::qt::debugger {

// "Edit Breakpoint" modal. Retains the breakpoint identity at open
// (retain_breakpoint_edit) and re-validates it at Apply
// (breakpoint_edit_is_current: index + fingerprint + breakpoints_generation
// re-checked under bp_mutex with the try_lock busy reason) plus a 250ms
// RevalidateScope on the same identity so a mid-edit change closes the dialog
// with the stale notification.
class BreakpointEditDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    static void openFor(const debugger_interaction::context_t& context,
                        int index,
                        debugger_view::breakpoint_editor_focus_t focus,
                        QWidget* parent);

private:
    BreakpointEditDialog(debugger_view::breakpoint_edit_state_t state,
                         QWidget* parent);

    void apply();

    debugger_view::breakpoint_edit_state_t state_;
    QLabel* gate_label_ = nullptr;
    QLineEdit* condition_edit_ = nullptr;
    QLineEdit* log_edit_ = nullptr;
    QCheckBox* auto_continue_check_ = nullptr;
    QPushButton* apply_button_ = nullptr;

    static QPointer<BreakpointEditDialog> active_;
};

}
