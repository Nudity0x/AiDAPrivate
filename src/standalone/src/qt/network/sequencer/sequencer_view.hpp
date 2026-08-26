#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QModelRoleDataSpan>
#include <QVariant>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/infra/executor.hpp"
#include "core/network/burp/sequencer.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
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

// SequencerCollectionsModel is fed by sequencer::snapshot_collections(): a
// 250 ms visibility-gated timer reads registry_generation() first and only
// fetches the bounded registry snapshot (<= 128 rows, mutex copy) on change —
// the same call the ImGui render path made every frame.
class SequencerCollectionsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Id = 0, Name, Progress, State, ColumnCount };

    explicit SequencerCollectionsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

    void adopt(aida::burp::sequencer::registry_snapshot_t snapshot);
    const aida::burp::sequencer::collection_status_t* rowAt(int row) const noexcept;
    const aida::burp::sequencer::collection_status_t* findById(std::uint64_t id) const noexcept;
    const aida::burp::sequencer::capacity_snapshot_t& capacity() const noexcept {
        return capacity_;
    }
    std::uint64_t generation() const noexcept { return generation_; }

private:
    std::vector<aida::burp::sequencer::collection_status_t> rows_;
    aida::burp::sequencer::capacity_snapshot_t capacity_;
    std::uint64_t generation_ = 0;
};

// SequencerHistogram: custom QWidget paintEvent (no QtCharts — locked
// decision). WA_OpaquePaintEvent (qwidget.cpp:717-731,2164-2203),
// update()-only (never repaint(), qwidget.cpp:11167-11237), fillRect solid
// fast path (qpaintengine_raster.cpp:1701-1715), and update() is only called
// on a publication change (coalesced low-priority UpdateRequest,
// qwidget.cpp:11167-11317).
class SequencerHistogram : public QWidget {
    Q_OBJECT
public:
    explicit SequencerHistogram(QWidget* parent = nullptr);

    void setAnalysis(
        const std::shared_ptr<const aida::burp::sequencer::analysis_result_t>& analysis);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::shared_ptr<const aida::burp::sequencer::analysis_result_t> analysis_;
};

class SequencerView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit SequencerView(QWidget* parent = nullptr);
    ~SequencerView() override;

    void openNewCollectionWith(const QString& url, const QString& host, int port,
                               bool useTls, const QString& rawRequest);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void refreshCollections();
    void refreshRightSide();
    void refreshSamples();
    void refreshStartGating();
    void startPressed();
    void stopSelected();
    void analyzeSelected();
    void deleteSelected();
    void drainStaged();
    void updateEmptyState();
    bool submitViewTask(aida::infra::executor::submission_t submission, const char* action);
    void publishViewError(std::string error);

    QLineEdit* name_edit_ = nullptr;
    QLineEdit* url_edit_ = nullptr;
    QLineEdit* host_edit_ = nullptr;
    QSpinBox* port_spin_ = nullptr;
    QCheckBox* tls_check_ = nullptr;
    QLineEdit* regex_edit_ = nullptr;
    QSpinBox* group_spin_ = nullptr;
    QSpinBox* target_spin_ = nullptr;
    QSpinBox* threads_spin_ = nullptr;
    QSpinBox* throttle_spin_ = nullptr;
    QCheckBox* use_raw_check_ = nullptr;
    QtHumanRequestEditor* raw_editor_ = nullptr;
    widgets::AidaButton* start_button_ = nullptr;
    widgets::AidaButton* stop_button_ = nullptr;
    widgets::AidaButton* analyze_button_ = nullptr;
    widgets::AidaButton* delete_button_ = nullptr;
    QLabel* capacity_label_ = nullptr;
    QLabel* start_error_label_ = nullptr;
    SequencerCollectionsModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* table_stack_ = nullptr;
    widgets::AidaStateView* empty_view_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* error_label_ = nullptr;
    QLabel* verdict_label_ = nullptr;
    QLabel* fips_label_ = nullptr;
    QFormLayout* stats_form_ = nullptr;
    QLabel* stats_values_[9] = {};
    QLabel* stats_bits_ = nullptr;
    QLabel* stats_ones_ = nullptr;
    QLabel* notes_label_ = nullptr;
    SequencerHistogram* histogram_ = nullptr;
    QPlainTextEdit* samples_ = nullptr;
    QTimer* collections_timer_ = nullptr;
    QTimer* samples_timer_ = nullptr;

    std::uint64_t selected_id_ = 0;
    std::uint64_t cached_generation_ = 0;
    aida::burp::sequencer::analysis_result_t last_analysis_{};
    bool analysis_valid_ = false;
    std::uint64_t analysis_for_id_ = 0;
    std::atomic<std::uint64_t> started_id_{0};
    std::shared_ptr<const std::string> start_error_ =
        std::make_shared<const std::string>();
    std::shared_ptr<const std::pair<std::uint64_t,
        aida::burp::sequencer::analysis_result_t>> analysis_publication_ =
        std::make_shared<const std::pair<std::uint64_t,
            aida::burp::sequencer::analysis_result_t>>();
    std::uint64_t staged_generation_ = 1;
    bool hooks_installed_ = false;
};

}
