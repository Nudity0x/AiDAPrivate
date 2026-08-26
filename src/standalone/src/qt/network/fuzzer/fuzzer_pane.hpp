#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QModelRoleDataSpan>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/network/network_view.hpp"
#include "qt/network/fuzzer/fuzzer_controller.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QRadioButton;
class QSpinBox;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class QtHumanRequestEditor;
class PayloadSetsEditor;

// FuzzerResultsModel sits over the shared_ptr<const fuzzer_results_snapshot_t>
// publication (O(1) row fetch: page = row >> 7). Column shape changes take
// the layoutAboutToBeChanged/layoutChanged reorder path
// (qabstractitemmodel.cpp:1680-1699); append growth with an unchanged dropped
// counter takes one beginInsertRows batch; an eviction (dropped advanced)
// takes beginResetModel and the pane restores selection by
// fuzzer_result_t.index (reset clears selection,
// qabstractitemview.cpp:1129-1156).
class FuzzerResultsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit FuzzerResultsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

    void adopt(
        const std::shared_ptr<const network_view::state_t::fuzzer_results_snapshot_t>& snapshot);
    const network_view::state_t::fuzzer_result_t* rowAt(int row) const noexcept;
    bool findRowForIndex(std::uint64_t resultIndex, int& rowOut) const;
    std::uint64_t retainedCount() const noexcept { return retained_count_; }
    std::uint64_t droppedCount() const noexcept { return dropped_count_; }

private:
    std::shared_ptr<const network_view::state_t::fuzzer_results_snapshot_t> snapshot_ =
        std::make_shared<const network_view::state_t::fuzzer_results_snapshot_t>();
    std::uint64_t retained_count_ = 0;
    std::uint64_t dropped_count_ = 0;
    std::size_t max_payload_columns_ = 1;
    bool show_extract_ = false;
    bool show_failures_ = false;
};

class FuzzerPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit FuzzerPane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void analyzeTemplate();
    void pullStagedTemplate();
    void refreshStartGating();
    void refreshRunState();
    void refreshResults();
    void updateResultsEmptyState();
    void startClicked();
    void stopClicked();
    void persistConfigToState();
    void loadConfigFromState();

    QLineEdit* host_edit_ = nullptr;
    QSpinBox* port_spin_ = nullptr;
    QCheckBox* tls_check_ = nullptr;
    QRadioButton* sniper_radio_ = nullptr;
    QRadioButton* pitchfork_radio_ = nullptr;
    QRadioButton* clusterbomb_radio_ = nullptr;
    QWidget* sniper_panel_ = nullptr;
    QComboBox* payload_type_combo_ = nullptr;
    QLineEdit* payload_source_edit_ = nullptr;
    QLabel* payload_hint_ = nullptr;
    PayloadSetsEditor* sets_editor_ = nullptr;
    QSpinBox* threads_spin_ = nullptr;
    QSpinBox* delay_spin_ = nullptr;
    QLineEdit* maximum_edit_ = nullptr;
    QCheckBox* reviewed_check_ = nullptr;
    QSpinBox* match_status_spin_ = nullptr;
    QCheckBox* stop_on_match_check_ = nullptr;
    QLineEdit* extract_edit_ = nullptr;
    QtHumanRequestEditor* template_editor_ = nullptr;
    widgets::AidaButton* start_button_ = nullptr;
    QLabel* start_reason_ = nullptr;
    widgets::AidaButton* clear_results_button_ = nullptr;
    QLabel* running_label_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    widgets::AidaButton* stop_button_ = nullptr;
    QLabel* stage_label_ = nullptr;
    QLabel* error_label_ = nullptr;
    QLabel* results_count_label_ = nullptr;
    FuzzerResultsModel* results_model_ = nullptr;
    QTableView* results_table_ = nullptr;
    QStackedLayout* results_stack_ = nullptr;
    widgets::AidaStateView* results_empty_ = nullptr;
    QTimer* analyze_timer_ = nullptr;
    QTimer* progress_timer_ = nullptr;

    fuzzer_template_shape_t template_shape_;
    std::uint64_t last_published_generation_ = 0;
    std::uint64_t last_request_revision_ = 0;
    std::uint64_t selected_result_index_ = 0;
    bool has_result_selection_ = false;
};

}
