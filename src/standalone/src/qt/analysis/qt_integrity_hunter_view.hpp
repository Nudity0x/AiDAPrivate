#pragma once

#include <QWidget>

#include <cstdint>
#include <vector>

#include "core/analysis/integrity_hunter.hpp"

class QAbstractTableModel;
class QLabel;
class QPlainTextEdit;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaLineEdit;
class AidaStateView;
}

namespace aida::qt::analysis {

// Integrity Hunter view (07 sec. 7.3), scan-hub page 6 (view.memory.integrity).
// The engine (integrity_hunter.hpp) is ZERO-CHANGE; view-local 66 ms timer
// polls the published state (S11). The broken custom scrollbar is deleted (sec. 10).
class QtIntegrityHunterView : public QWidget {
    Q_OBJECT
public:
    explicit QtIntegrityHunterView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void pollEngine();
    void refreshPresentation();
    void showRowMenu(const QPoint& global_pos, int view_row);

    QAbstractTableModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaLineEdit* address_edit_ = nullptr;
    widgets::AidaLineEdit* size_edit_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QPlainTextEdit* event_log_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* stat_checkers_ = nullptr;
    QLabel* stat_active_ = nullptr;
    QLabel* stat_neutralized_ = nullptr;
    QLabel* stat_rate_ = nullptr;
    QTimer* timer_ = nullptr;
    std::uint64_t generation_ = 0;
    std::vector<integrity_hunter::integrity_node_t> nodes_copy_;
    std::uint64_t selected_rip_ = 0;
    std::size_t event_log_appended_ = 0;
    bool show_event_log_ = false;
};

}
