#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QObject>
#include <QString>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/network/burp/active_scanner.hpp"
#include "core/network/burp/burp_ui_operation.hpp"
#include "core/network/burp/issue.hpp"
#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QSplitter;
class QTableView;
class QTimer;
class QVBoxLayout;

namespace aida::qt::widgets {
class AidaBadge;
class AidaButton;
class AidaStateView;
class AidaStatusItem;
class AidaToggleSwitch;
}

namespace aida::qt::net {

class QtNewAuditDialog;

class QtAuditListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit QtAuditListModel(QObject* parent = nullptr);

    void adopt(std::vector<aida::burp::active_scanner::audit_status_t> rows);
    const aida::burp::active_scanner::audit_status_t* rowAt(int row) const noexcept;
    int rowForId(std::uint64_t id) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

private:
    std::vector<aida::burp::active_scanner::audit_status_t> rows_;
};

class QtIssueModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Severity = 0, Conf, Host, Param, Type, ColumnCount };

    explicit QtIssueModel(QObject* parent = nullptr);

    void adopt(std::shared_ptr<const std::vector<aida::burp::issue_t>> rows);
    const aida::burp::issue_t* rowAt(int row) const noexcept;
    int rowForId(std::uint64_t id) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    std::shared_ptr<const std::vector<aida::burp::issue_t>> rows_ =
        std::make_shared<const std::vector<aida::burp::issue_t>>();
};

class QtScannerController : public QObject {
    Q_OBJECT
public:
    explicit QtScannerController(QObject* parent = nullptr);

    bool initialize();
    void shutdown();
    bool initialized() const noexcept { return initialized_.load(); }
    bool initializationRequested() const noexcept { return initialization_requested_.load(); }

    aida::burp::ui_operation::state_t& operation() noexcept { return operation_; }
    std::uint64_t observedOperationGeneration() const noexcept { return observed_operation_generation_; }
    void setObservedOperationGeneration(std::uint64_t generation) noexcept {
        observed_operation_generation_ = generation;
    }
    void resetInitializationRequested() noexcept {
        initialization_requested_.store(false, std::memory_order_release);
    }
    bool takeInitializationRequested() noexcept {
        return initialization_requested_.exchange(false, std::memory_order_acq_rel);
    }
    void setInitialized(bool ready) noexcept {
        initialized_.store(ready, std::memory_order_release);
    }

    void submitInitialization();
    void submitIssueExport();
    void submitReviewedIssueClear(
        std::vector<std::pair<std::uint64_t, std::uint64_t>> reviewed);
    bool submitAudit(std::vector<std::uint8_t> raw, std::string url,
                     aida::burp::active_scanner::audit_config_t config,
                     std::uint64_t dialogGeneration);
    void submitPassiveToggle(bool reviewed, bool desired);
    void requestIssueSnapshot(aida::burp::issue_filter_t filter);

    std::shared_ptr<const std::vector<aida::burp::issue_t>> issues() const;
    QString statusMessage() const { return status_message_; }
    void setStatusMessage(const QString& message);
    std::uint64_t takeStartedAudit(std::uint64_t& dialogGeneration) noexcept;
    bool auditSubmissionPending() const noexcept { return audit_submission_pending_.load(); }
    void clearAuditSubmissionPending() noexcept {
        audit_submission_pending_.store(false, std::memory_order_release);
    }

    static bool resolveRetainedArtifact(std::uint64_t issueId, std::uint64_t seenMs,
                                        std::uint64_t evidenceIndex, bool response,
                                        std::vector<std::uint8_t>& bytes, std::string& reason);
    static bool resolveRetainedEndpoint(std::uint64_t issueId, std::uint64_t seenMs,
                                        std::string& host, std::uint16_t& port, bool& useTls,
                                        std::string& reason);
    static network_view::artifact_identity_t evidenceIdentity(
        const aida::burp::issue_t& issue, std::size_t evidenceIndex, bool response);

Q_SIGNALS:
    void issuesChanged();
    void statusMessageChanged();
    void operationStateChanged();

private:
    std::atomic<bool> initialized_{false};
    std::atomic<bool> initialization_requested_{false};
    aida::burp::ui_operation::state_t operation_;
    std::uint64_t observed_operation_generation_ = 0;
    std::shared_ptr<const std::vector<aida::burp::issue_t>> issues_ =
        std::make_shared<const std::vector<aida::burp::issue_t>>();
    std::atomic<bool> issues_refresh_pending_{false};
    QString status_message_;
    std::atomic<std::uint64_t> started_audit_id_{0};
    std::atomic<std::uint64_t> started_audit_dialog_generation_{0};
    std::atomic<bool> audit_submission_pending_{false};
};

// QtScannerView ports burp/scanner_view.cpp. Audits pane: rich delegate rows
// (primary/url/progress/status + Cancel hit-zone) with the Task Center
// register/update/terminal/interrupted lifecycle verbatim. Issues pane:
// filter signature + 200 ms refresh + responsive column hiding thresholds
// verbatim.
class QtScannerView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit QtScannerView(QWidget* parent = nullptr);
    ~QtScannerView() override;

    // Byte-verbatim port of scanner_view::open_new_audit_with (URL <= 1023,
    // request <= 65535, binary rejection, generation bump). Stages into the
    // pending store and opens the dialog on the live view when one exists;
    // otherwise the first created view consumes the staged draft.
    static bool openNewAuditWith(const std::string& url, const std::string& raw_request);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void observeCompletion();
    void refreshAudits();
    void refreshIssueDetail();
    void refreshToolbar();
    void applyResponsiveColumns();
    void requestIssuesNow();
    void presentClearIssuesReview();
    void openNewAuditDialog(const QString& url, const QString& rawRequest);
    void showEvidenceContext(const aida::burp::issue_t& issue, std::size_t evidenceIndex,
                             bool response, QWidget* invoker, const QPoint& globalPos,
                             aida::ui::context_menu_open_origin_t origin);

    QtScannerController* controller_ = nullptr;
    widgets::AidaButton* newAuditButton_ = nullptr;
    widgets::AidaButton* retryInitButton_ = nullptr;
    widgets::AidaButton* exportButton_ = nullptr;
    widgets::AidaButton* clearIssuesButton_ = nullptr;
    widgets::AidaToggleSwitch* passiveToggle_ = nullptr;
    QLabel* passiveLabel_ = nullptr;
    widgets::AidaStatusItem* scannedItem_ = nullptr;
    widgets::AidaStatusItem* issuesItem_ = nullptr;
    widgets::AidaStatusItem* modulesItem_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QTableView* auditsView_ = nullptr;
    QtAuditListModel* auditModel_ = nullptr;
    widgets::AidaStateView* auditsEmpty_ = nullptr;
    QComboBox* sevCombo_ = nullptr;
    QComboBox* confCombo_ = nullptr;
    QLineEdit* hostFilterEdit_ = nullptr;
    QLineEdit* typeFilterEdit_ = nullptr;
    QTableView* issuesView_ = nullptr;
    QtIssueModel* issueModel_ = nullptr;
    widgets::AidaStateView* issuesEmpty_ = nullptr;
    QWidget* detailPane_ = nullptr;
    QWidget* detailContent_ = nullptr;
    widgets::AidaBadge* detailSeverity_ = nullptr;
    QLabel* detailName_ = nullptr;
    QLabel* detailUrl_ = nullptr;
    QLabel* detailConfidence_ = nullptr;
    QLabel* detailCwe_ = nullptr;
    QLabel* detailDescription_ = nullptr;
    QLabel* detailRemediation_ = nullptr;
    QWidget* evidenceHost_ = nullptr;
    QVBoxLayout* evidenceHostLayout_ = nullptr;
    widgets::AidaStateView* detailEmpty_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    QTimer* completionTimer_ = nullptr;
    std::uint64_t selected_audit_id_ = 0;
    std::uint64_t selected_issue_id_ = 0;
    std::uint64_t detail_key_id_ = 0;
    std::uint64_t detail_key_seen_ms_ = 0;
    std::size_t detail_key_evidence_ = 0;
    std::unordered_set<std::uint64_t> task_center_audits_;
    std::unordered_set<std::uint64_t> terminal_audits_;
    QtNewAuditDialog* new_audit_dialog_ = nullptr;
    std::uint64_t new_dialog_generation_ = 0;
};

}
