#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/workbench/workbench_shell_integration.hpp"

class QLabel;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace aida::qt::widgets {
class AidaNotice;
class AidaStateView;
}

namespace aida::qt::workbench {

// Inspector view (07 sec. 8.2), view.inspector. Sections are QGroupBox+QFormLayout
// inside a QScrollArea, rebuilt on snapshot change (bounded small data).
// Revision fencing, pin/follow, and the evidence handoff port verbatim.
class QtWorkbenchInspectorView : public QWidget {
    Q_OBJECT
public:
    explicit QtWorkbenchInspectorView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void pollSelection();
    void rebuild();
    void showAddressMenu(const QPoint& global_pos);

    QToolButton* pin_button_ = nullptr;
    QToolButton* follow_button_ = nullptr;
    QLabel* mode_label_ = nullptr;
    QLabel* revision_label_ = nullptr;
    QLabel* name_label_ = nullptr;
    QLabel* qualified_label_ = nullptr;
    QLabel* handoff_label_ = nullptr;
    widgets::AidaNotice* stale_notice_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* sections_host_ = nullptr;
    QVBoxLayout* sections_layout_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;

    bool follow_selection_ = true;
    bool pinned_ = false;
    std::uint64_t observed_generation_ = 0;
    std::uint64_t captured_generation_ = 0;
    std::uint64_t captured_analysis_revision_ = 0;
    std::uint64_t captured_overlay_revision_ = 0;
    std::string handoff_status_;
    QMetaObject::Connection context_connection_;
};

}
