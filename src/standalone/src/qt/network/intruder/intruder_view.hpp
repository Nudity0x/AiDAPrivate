#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QModelRoleDataSpan>
#include <QSyntaxHighlighter>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/network/burp/intruder_engine.hpp"
#include "qt/bridge/aida_dialog.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/burp_operation.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QStackedLayout;
class QTableView;
class QTextDocument;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class BoundedPlainTextEdit;
class QtHumanRequestEditor;

class IntruderJobsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Job = 0, State, Sent, Total, Rps, Errors, ColumnCount };

    explicit IntruderJobsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

    void adopt(std::vector<aida::burp::intruder::status_t> jobs);
    const aida::burp::intruder::status_t* rowAt(int row) const noexcept;
    const aida::burp::intruder::status_t* findById(std::uint64_t jobId) const noexcept;

private:
    std::vector<aida::burp::intruder::status_t> rows_;
};

// IntruderResultsModel is the lazy results table: rowCount is the
// materialized count, canFetchMore is true while fewer rows than the job
// total are materialized, and fetchMore posts an executor page fetch
// (intruder::results(job, offset, 128)) delivered through a queued
// invokeMethod and appended in one beginInsertRows batch. Job switches bump
// the generation; stale page deliveries are discarded.
class IntruderResultsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Index = 0, Payload, Status, Length, Latency, Error, ColumnCount };

    explicit IntruderResultsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    void setJob(std::uint64_t jobId, std::size_t total);
    void setTotal(std::size_t total);
    void clearRows();
    const aida::burp::intruder::result_t* rowAt(int row) const noexcept;
    std::uint64_t jobId() const noexcept { return job_id_; }

private:
    std::vector<aida::burp::intruder::result_t> rows_;
    std::uint64_t job_id_ = 0;
    std::size_t total_ = 0;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<bool> fetch_pending_{false};
    QObject* delivery_context_ = nullptr;
    friend class IntruderView;
};

// IntruderMarkerHighlighter scans each block for $...$ marker pairs and
// accents them; an unterminated $ sets block state 1 continued via
// previousBlockState (qsyntaxhighlighter.cpp:140-148,186-203).
class IntruderMarkerHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit IntruderMarkerHighlighter(QTextDocument* parent);

protected:
    void highlightBlock(const QString& text) override;
};

class IntruderNewAttackDialog : public aida::qt::bridge::AidaDialog {
    Q_OBJECT
public:
    IntruderNewAttackDialog(BurpOperationRunner* runner, QWidget* parent = nullptr);

    void preset(const QString& host, int port, bool tls, const QString& rawRequest);
    void setSubmitHandler(
        std::function<void(const aida::burp::intruder::config_t&)> submit);

private:
    void refreshLaunch();
    aida::burp::intruder::config_t buildConfig() const;

    QLineEdit* host_edit_ = nullptr;
    QSpinBox* port_spin_ = nullptr;
    QCheckBox* tls_check_ = nullptr;
    QComboBox* attack_mode_ = nullptr;
    QComboBox* engine_mode_ = nullptr;
    QSpinBox* concurrency_ = nullptr;
    QSpinBox* rps_cap_ = nullptr;
    QSpinBox* total_cap_ = nullptr;
    QSpinBox* timeout_ms_ = nullptr;
    QLabel* race_gate_label_ = nullptr;
    QSpinBox* race_gate_ = nullptr;
    QLabel* race_warmup_label_ = nullptr;
    QSpinBox* race_warmup_ = nullptr;
    QtHumanRequestEditor* request_editor_ = nullptr;
    BoundedPlainTextEdit* payload_set_ = nullptr;
    widgets::AidaButton* launch_button_ = nullptr;
    BurpOperationRunner* runner_ = nullptr;
    std::function<void(const aida::burp::intruder::config_t&)> submit_;
    std::uint64_t request_generation_ = 1;
    QString preset_request_;
    bool editor_valid_ = false;
    bool editor_unapplied_ = false;
};

class IntruderView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit IntruderView(QWidget* parent = nullptr);
    ~IntruderView() override;

    void openNewAttack(const QString& host, int port, bool tls, const QString& rawRequest);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void refreshJobs();
    void refreshJobsButtons();
    void refreshOpStatus();
    void refreshStatusLine();
    void refreshDetail();
    void submitStart(aida::burp::intruder::config_t config);
    void submitStop(aida::burp::intruder::status_t reviewed);
    void submitClear(aida::burp::intruder::status_t reviewed);
    void openClearReview();
    void openResultsContextMenu(const QPoint& viewportPos);
    void updateJobsEmptyState();
    void updateResultsEmptyState();
    void drainStaged();

    IntruderJobsModel* jobs_model_ = nullptr;
    QTableView* jobs_table_ = nullptr;
    QStackedLayout* jobs_stack_ = nullptr;
    widgets::AidaStateView* jobs_empty_ = nullptr;
    widgets::AidaButton* new_attack_button_ = nullptr;
    widgets::AidaButton* stop_button_ = nullptr;
    widgets::AidaButton* clear_button_ = nullptr;
    QLabel* op_status_label_ = nullptr;
    QLabel* results_header_ = nullptr;
    IntruderResultsModel* results_model_ = nullptr;
    QTableView* results_table_ = nullptr;
    QStackedLayout* results_stack_ = nullptr;
    widgets::AidaStateView* results_empty_ = nullptr;
    QLabel* detail_info_ = nullptr;
    QPlainTextEdit* detail_view_ = nullptr;
    BurpOperationRunner* runner_ = nullptr;
    QTimer* jobs_timer_ = nullptr;
    QTimer* status_timer_ = nullptr;

    std::uint64_t selected_job_id_ = 0;
    std::int64_t selected_result_index_ = -1;
    std::atomic<std::uint64_t> started_job_id_{0};
    aida::burp::intruder::status_t reviewed_clear_{};
    std::size_t selected_job_started_ms_ = 0;
    bool hooks_installed_ = false;
};

}
