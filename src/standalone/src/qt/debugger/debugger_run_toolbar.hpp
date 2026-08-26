#pragma once

#include <QToolBar>

#include <QPointer>
#include <QString>

#include <vector>

class QAction;
class QTimer;

namespace aida::qt::widgets {
class AidaPill;
}

namespace aida::qt::debugger {

// Run toolbar over the eight registered debugger.* execution actions. The
// QActions are hydrated from the W2.2 action bridge (capability + disabled
// reason flow through QAction::setEnabled + tooltip); the toolbar re-evaluates
// on the 250ms session tick (capabilities track g_state.status, not the
// registry revision). QToolBar's built-in overflow extension replaces the
// ImGui "More" math. The status pill is exposed via statusPill() for the host
// pane to place beside the toolbar: a QToolBar not parented to a QMainWindow
// cannot carry addWidget() widgets into its overflow popup, so the pill must
// live outside the toolbar to survive narrow widths.
class DebuggerRunToolBar : public QToolBar {
    Q_OBJECT
public:
    explicit DebuggerRunToolBar(QWidget* parent = nullptr);

    void refreshState();

    // Owned by this toolbar; the host pane re-parents it into its own row.
    widgets::AidaPill* statusPill() const noexcept { return status_pill_; }

private:
    void rebuildActions();

    std::vector<QPointer<QAction>> actions_;
    widgets::AidaPill* status_pill_ = nullptr;
    bool built_ = false;
};

}
