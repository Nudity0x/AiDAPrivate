#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>

#include "core/workbench/workbench_shell_integration.hpp"

class QComboBox;
class QLabel;
class QTableView;
class QAbstractTableModel;

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::workbench {

class QtAnalysisBridgeAccess;

// Navigator view (07 sec. 8.1), view.navigator. Synchronous 20 ms-budgeted page
// fetch on the GUI thread (frame_cancellation_t verbatim), domain combo, S6
// context menu.
class QtWorkbenchNavigatorView : public QWidget {
    Q_OBJECT
public:
    explicit QtWorkbenchNavigatorView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void reloadPage();
    void showRowMenu(const QPoint& global_pos, int view_row);

    QComboBox* domain_combo_ = nullptr;
    QAbstractTableModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QLabel* status_label_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    aida::workbench::navigator::navigator_domain_t domain_ =
        aida::workbench::navigator::navigator_domain_t::functions;
    std::uint64_t selected_id_ = 0;
    std::uint64_t observed_generation_ = 0;
    QMetaObject::Connection context_connection_;
};

}
