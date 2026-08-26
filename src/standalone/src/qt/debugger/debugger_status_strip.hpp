#pragma once

#include <QWidget>

#include <QString>

#include <cstdint>

class QHBoxLayout;
class QLabel;

namespace aida::qt::widgets {
class AidaPill;
}

namespace aida::qt::debugger {

// Status strip port of render_debugger_status_bar: Target / Engine / Panel /
// RIP items with budget-based narrowing (RIP hides first, then the panel
// label, sized from each item's sizeHint) and watchdog degradation on the
// Engine pill (age > 3x4000ms per the W2.12 bridge audit).
class DebuggerStatusStrip : public QWidget {
    Q_OBJECT
public:
    explicit DebuggerStatusStrip(QWidget* parent = nullptr);

    void setPanelLabel(const QString& label);
    void refreshState();
    void onWatchdogSampled(quint64 ageMs, bool degraded);

private:
    QHBoxLayout* layout_ = nullptr;
    QLabel* target_label_ = nullptr;
    widgets::AidaPill* engine_pill_ = nullptr;
    QLabel* panel_label_ = nullptr;
    QLabel* rip_label_ = nullptr;
    QString panel_label_text_;
    bool watchdog_degraded_ = false;
};

}
