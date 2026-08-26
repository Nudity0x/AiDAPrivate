#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/analysis/stealth_engine.hpp"

class QAbstractTableModel;
class QComboBox;
class QLabel;
class QProgressBar;
class QStackedLayout;
class QTabBar;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::analysis {

// Stealth / Protection view (07 sec. 7.4), analysis-hub page 4
// (view.analysis.protection). The engine (stealth_engine.hpp) is ZERO-CHANGE.
class QtStealthView : public QWidget {
    Q_OBJECT
public:
    explicit QtStealthView(QWidget* parent = nullptr);

    // Inner tab mirror surface for QtAnalysisBridge (replaces the old
    // stealth_view::{active,set}_sub_tab accessors).
    int innerTab() const noexcept;
    void setInnerTab(int index);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QWidget* buildScanPage();
    QWidget* buildStatusPage();
    void pollScan();
    void pollStatus();
    void refreshScanPresentation();
    void showFindingMenu(const QPoint& global_pos, int view_row);

    QTabBar* tabs_ = nullptr;
    QStackedLayout* stack_ = nullptr;
    QTimer* timer_ = nullptr;

    // Protection Scan page
    QAbstractTableModel* findings_model_ = nullptr;
    QTableView* findings_table_ = nullptr;
    QComboBox* severity_combo_ = nullptr;
    QComboBox* category_combo_ = nullptr;
    QProgressBar* scan_progress_ = nullptr;
    QLabel* scan_status_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QLabel* summary_critical_ = nullptr;
    QLabel* summary_high_ = nullptr;
    QLabel* summary_rest_ = nullptr;
    widgets::AidaStateView* scan_state_view_ = nullptr;
    std::uint64_t findings_generation_ = 0;
    std::shared_ptr<const std::vector<stealth_engine::finding_t>> findings_;
    std::vector<std::size_t> filtered_findings_;
    int selected_finding_ = -1;
    int severity_filter_ = -1;
    int category_filter_ = -1;

    // Stealth Status page
    QAbstractTableModel* hooks_model_ = nullptr;
    QTableView* hooks_table_ = nullptr;
    QLabel* status_state_ = nullptr;
    QLabel* status_target_ = nullptr;
    QLabel* status_detail_ = nullptr;
    QWidget* cards_host_ = nullptr;
    QLabel* card_pid_ = nullptr;
    QLabel* card_peb_ = nullptr;
    QLabel* card_rdtsc_ = nullptr;
    widgets::AidaStateView* stealth_state_view_ = nullptr;
};

}
