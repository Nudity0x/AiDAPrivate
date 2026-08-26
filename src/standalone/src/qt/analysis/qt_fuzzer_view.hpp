#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>

#include "core/analysis/fuzzer_engine.hpp"

class QCheckBox;
class QFrame;
class QLabel;
class QPushButton;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaLineEdit;
class AidaStateView;
}

namespace aida::qt::analysis {

class QtFuzzerCanvas;
class QtFuzzerCrashModel;

// Analysis Fuzzer view (07 sec. 7.5), analysis-hub page 3 (view.analysis.fuzzer).
// The engine (fuzzer_engine.hpp) is ZERO-CHANGE; the preview stub is deleted.
class QtFuzzerView : public QWidget {
    Q_OBJECT
public:
    explicit QtFuzzerView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void pollEngine();
    void refreshPresentation();
    void updateStats(const fuzzer_engine::fuzz_stats_t& stats);
    void updateDetailPanel();
    void showCrashMenu(const QPoint& global_pos, int view_row);
    void startFuzzing();
    void resetState();

    widgets::AidaLineEdit* addr_edit_ = nullptr;
    widgets::AidaLineEdit* end_edit_ = nullptr;
    widgets::AidaLineEdit* input_edit_ = nullptr;
    widgets::AidaLineEdit* input_size_edit_ = nullptr;
    widgets::AidaLineEdit* max_iter_edit_ = nullptr;
    QCheckBox* strategy_checks_[6] = {};
    QPushButton* start_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QPushButton* export_button_ = nullptr;
    QPushButton* import_button_ = nullptr;
    QPushButton* reset_button_ = nullptr;
    QLabel* status_pill_ = nullptr;
    QLabel* stat_labels_[8] = {};
    QtFuzzerCanvas* canvas_ = nullptr;
    QtFuzzerCrashModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QFrame* detail_panel_ = nullptr;
    QLabel* detail_title_ = nullptr;
    QLabel* detail_instruction_ = nullptr;
    QLabel* detail_fault_ = nullptr;
    QLabel* detail_registers_ = nullptr;
    QLabel* detail_mutation_ = nullptr;
    QLabel* detail_input_ = nullptr;
    QLabel* detail_minimized_ = nullptr;
    QLabel* detail_ai_ = nullptr;
    QPushButton* analyze_button_ = nullptr;
    QPushButton* minimize_button_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QLabel* truncated_badge_ = nullptr;
    QTimer* timer_ = nullptr;
    qreal scan_phase_ = 0.0;
    std::uint64_t crash_snapshot_generation_ = 0;
    int selected_crash_ = -1;
    std::uint64_t selected_crash_hash_ = 0;
    std::uint64_t last_unique_crashes_ = 0;
    qreal stat_values_[8] = {};
};

}
