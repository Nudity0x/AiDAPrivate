#pragma once

#include <QWidget>

#include <cstdint>

#include "core/workbench/adapters/diff_document.hpp"
#include "core/workbench/workbench_shell_integration.hpp"

class QComboBox;
class QLabel;
class QPushButton;
class QTableView;
class QAbstractTableModel;

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::workbench {

// Diff view (07 sec. 8.3), document.diff. Provider paging preserved (256-row
// pages, 30 ms cancellation budget, Previous/Next).
class QtWorkbenchDiffView : public QWidget {
    Q_OBJECT
public:
    explicit QtWorkbenchDiffView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void reloadPage();
    void showRowMenu(const QPoint& global_pos, int view_row);

    QComboBox* kind_combo_ = nullptr;
    QPushButton* prev_button_ = nullptr;
    QPushButton* next_button_ = nullptr;
    QLabel* status_label_ = nullptr;
    QAbstractTableModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    aida::workbench::diff_document::diff_kind_t kind_ =
        aida::workbench::diff_document::diff_kind_t::generation;
    std::uint64_t offset_ = 0;
    std::uint64_t total_ = 0;
    std::uint64_t observed_generation_ = 0;
    QMetaObject::Connection context_connection_;
};

}
