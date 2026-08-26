#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QObject>
#include <QString>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/network/burp/burp_ui_operation.hpp"
#include "core/network/burp/content_discovery.hpp"
#include "core/network/burp/crawler.hpp"
#include "core/network/burp/payload_library.hpp"
#include "core/network/burp/subdomain_enum.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QSpinBox;
class QStackedLayout;
class QTabBar;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class QtByteCappedPlainTextEdit;

// Normalized display rows for the three run tables and three result tables.
// The domain backends expose different status structs; each refresh adapts
// them into these columns verbatim from the ImGui cell text.
struct QtReconRunRow {
    std::uint64_t id = 0;
    std::uint64_t started_unix_ms = 0;
    QString phase;
    QString c0;
    QString c1;
    QString c2;
    QString c3;
    QString c4;
};

struct QtReconResultRow {
    QString c0;
    QString c1;
    QString c2;
    QString c3;
    QString c4;
};

class QtReconRunModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Domain { Crawler, ContentDiscovery, Subdomains };

    explicit QtReconRunModel(Domain domain, QObject* parent = nullptr);

    void adopt(std::vector<QtReconRunRow> rows);
    const QtReconRunRow* rowAt(int row) const noexcept;
    int rowForId(std::uint64_t id) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    Domain domain_;
    std::vector<QtReconRunRow> rows_;
};

class QtReconResultModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Domain { CrawlerUrls, ContentHits, SubdomainResults };

    explicit QtReconResultModel(Domain domain, QObject* parent = nullptr);

    void adopt(std::vector<QtReconResultRow> rows);
    std::size_t rawCount() const noexcept { return rows_.size(); }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    Domain domain_;
    std::vector<QtReconResultRow> rows_;
};

class QtPayloadSetModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit QtPayloadSetModel(QObject* parent = nullptr);

    void adopt(std::shared_ptr<const std::vector<aida::burp::payloads::payload_set_t>> sets);
    void setFilter(const QString& needle);
    const aida::burp::payloads::payload_set_t* rowAt(int row) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

private:
    std::shared_ptr<const std::vector<aida::burp::payloads::payload_set_t>> sets_ =
        std::make_shared<const std::vector<aida::burp::payloads::payload_set_t>>();
    QString filter_;
    std::vector<int> visible_;
};

class QtPayloadEntryModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit QtPayloadEntryModel(QObject* parent = nullptr);

    void adopt(std::vector<std::string> entries);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

private:
    std::vector<std::string> entries_;
};

// QtReconController owns the ui_operation (executor + task_center, verbatim),
// the CAS single-flight run refresh, and the run/payload snapshots. All
// completions are observed on the GUI thread by the view's 100 ms poll timer
// (the ImGui version evaluated the completion block per frame).
class QtReconController : public QObject {
    Q_OBJECT
public:
    explicit QtReconController(QObject* parent = nullptr);

    bool initialize();
    void shutdown();

    aida::burp::ui_operation::state_t& operation() noexcept { return operation_; }

    void submitInitialization();
    void submitCrawlerStart(const QString& seedText, int maxDepth, int maxPages,
                            int concurrency, int ratePerHost, bool sameHost, bool scopeOnly,
                            bool respectRobots, bool parseJs, const QString& userAgent,
                            const QString& excludeExtensions);
    void submitDiscoveryStart(const QString& target, const QString& wordlistId,
                              const QString& extensions, int concurrency, int delayMs,
                              const QString& matchStatus, const QString& filterStatus,
                              bool recurse, int recurseDepth, bool autoCalibrate,
                              bool followRedirects, const QString& cookie,
                              const QString& userAgent);
    void submitSubdomainStart(const QString& domain, const QString& wordlistId,
                              int concurrency, bool runPassive, bool runBrute,
                              bool passiveCrtsh, bool passiveBufferover,
                              bool passiveHackertarget);
    void submitStop(int domain, std::uint64_t id, std::uint64_t startedMs);
    bool submitReviewedRemove(int domain, std::uint64_t id, std::uint64_t startedMs);
    bool submitPayloadAdd(QString id, QString label, QString description,
                          std::vector<std::string> entries);
    bool submitPayloadRemove(QString id, bool reviewedBuiltin);
    bool queueSubdomainExport(std::uint64_t runId);
    void requestRunRefresh();

    std::shared_ptr<const std::vector<aida::burp::crawler::crawl_status_t>> crawlerRuns() const;
    std::shared_ptr<const std::vector<aida::burp::content_discovery::disc_status_t>> discoveryRuns() const;
    std::shared_ptr<const std::vector<aida::burp::subdomain_enum::enum_status_t>> subdomainRuns() const;
    std::shared_ptr<const std::vector<aida::burp::payloads::payload_set_t>> payloadSets() const;

    bool initialized() const noexcept { return initialized_.load(); }
    bool subExportPending() const noexcept { return sub_export_pending_.load(); }
    void resetInitializationRequested() noexcept {
        initialization_requested_.store(false, std::memory_order_release);
    }
    std::uint64_t observedOperationGeneration() const noexcept { return observed_operation_generation_; }
    void setObservedOperationGeneration(std::uint64_t generation) noexcept {
        observed_operation_generation_ = generation;
    }
    std::uint64_t takeStartedRun(int& domain) noexcept;
    bool clearPayloadInputsAfterSuccess() const noexcept { return clear_payload_inputs_after_success_; }
    void clearPayloadInputsConsumed() noexcept { clear_payload_inputs_after_success_ = false; }
    bool awaitingRemoveCompletion() const noexcept { return awaiting_remove_completion_; }
    void setAwaitingRemoveCompletion(bool awaiting) noexcept { awaiting_remove_completion_ = awaiting; }
    bool awaitingPayloadRemoveCompletion() const noexcept { return awaiting_payload_remove_completion_; }
    void setAwaitingPayloadRemoveCompletion(bool awaiting) noexcept { awaiting_payload_remove_completion_ = awaiting; }
    int reviewDomain() const noexcept { return review_domain_; }
    std::uint64_t reviewedId() const noexcept { return reviewed_id_; }
    QString reviewedPayloadId() const { return reviewed_payload_id_; }

Q_SIGNALS:
    void runsChanged();

private:
    bool submitOperation(std::string action, std::string label, std::string target,
                         std::function<aida::burp::ui_operation::result_t()> execute);

    std::atomic<bool> initialized_{false};
    std::atomic<bool> initialization_requested_{false};
    aida::burp::ui_operation::state_t operation_;
    std::uint64_t observed_operation_generation_ = 0;
    std::atomic<bool> refresh_pending_{false};
    std::atomic<std::uint64_t> started_run_id_{0};
    std::atomic<int> started_run_domain_{0};
    std::atomic<bool> sub_export_pending_{false};
    int review_domain_ = 0;
    std::uint64_t reviewed_id_ = 0;
    bool awaiting_remove_completion_ = false;
    QString reviewed_payload_id_;
    bool reviewed_payload_builtin_ = false;
    bool awaiting_payload_remove_completion_ = false;
    bool clear_payload_inputs_after_success_ = false;

    std::shared_ptr<const std::vector<aida::burp::crawler::crawl_status_t>> crawler_runs_ =
        std::make_shared<const std::vector<aida::burp::crawler::crawl_status_t>>();
    std::shared_ptr<const std::vector<aida::burp::content_discovery::disc_status_t>> discovery_runs_ =
        std::make_shared<const std::vector<aida::burp::content_discovery::disc_status_t>>();
    std::shared_ptr<const std::vector<aida::burp::subdomain_enum::enum_status_t>> subdomain_runs_ =
        std::make_shared<const std::vector<aida::burp::subdomain_enum::enum_status_t>>();
    std::shared_ptr<const std::vector<aida::burp::payloads::payload_set_t>> payload_sets_ =
        std::make_shared<const std::vector<aida::burp::payloads::payload_set_t>>();
};

// QtReconView ports burp/recon_view.cpp: QTabBar + QStackedLayout with the
// four domains (Crawler / Content Discovery / Subdomains / Payload Library).
// Run catalogs refresh on a 200 ms GUI timer while visible + after each
// operation completion; operation completion is observed on a 100 ms timer.
class QtReconView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit QtReconView(QWidget* parent = nullptr);
    ~QtReconView() override;

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    QWidget* buildCrawlerTab(QWidget* parent);
    QWidget* buildDiscoveryTab(QWidget* parent);
    QWidget* buildSubdomainsTab(QWidget* parent);
    QWidget* buildPayloadsTab(QWidget* parent);
    void observeCompletion();
    void refreshRuns();
    void refreshCrawlerDetail();
    void refreshDiscoveryDetail();
    void refreshSubdomainDetail();
    void refreshPayloadDetail();
    void presentRemoveReview(int domain, std::uint64_t id, std::uint64_t startedMs);
    void presentPayloadRemoveReview(const QString& id, bool builtin);

    QtReconController* controller_ = nullptr;
    QTabBar* tabBar_ = nullptr;
    QStackedLayout* stack_ = nullptr;
    QLabel* operationLabel_ = nullptr;
    widgets::AidaButton* retryButton_ = nullptr;
    QWidget* loadingHost_ = nullptr;
    QLabel* loadingLabel_ = nullptr;
    widgets::AidaButton* initRetryButton_ = nullptr;
    QStackedLayout* initStack_ = nullptr;
    QWidget* tabsHost_ = nullptr;

    QLineEdit* crawlerSeedEdit_ = nullptr;
    QSpinBox* crawlerDepthSpin_ = nullptr;
    QSpinBox* crawlerPagesSpin_ = nullptr;
    QSpinBox* crawlerConcurrencySpin_ = nullptr;
    QSpinBox* crawlerRateSpin_ = nullptr;
    QCheckBox* crawlerSameHostCheck_ = nullptr;
    QCheckBox* crawlerScopeOnlyCheck_ = nullptr;
    QCheckBox* crawlerRobotsCheck_ = nullptr;
    QCheckBox* crawlerParseJsCheck_ = nullptr;
    QLineEdit* crawlerUserAgentEdit_ = nullptr;
    QLineEdit* crawlerExcludeEdit_ = nullptr;
    widgets::AidaButton* crawlerStartButton_ = nullptr;
    widgets::AidaButton* crawlerStopButton_ = nullptr;
    widgets::AidaButton* crawlerRemoveButton_ = nullptr;
    QTableView* crawlerRunsView_ = nullptr;
    QtReconRunModel* crawlerRunModel_ = nullptr;
    widgets::AidaStateView* crawlerRunsEmpty_ = nullptr;
    QLabel* crawlerDetailLabel_ = nullptr;
    QTableView* crawlerDetailView_ = nullptr;
    QtReconResultModel* crawlerDetailModel_ = nullptr;
    widgets::AidaStateView* crawlerDetailEmpty_ = nullptr;
    std::uint64_t crawler_selected_ = 0;

    QLineEdit* discTargetEdit_ = nullptr;
    QLineEdit* discWordlistEdit_ = nullptr;
    QLineEdit* discExtensionsEdit_ = nullptr;
    QLineEdit* discMatchStatusEdit_ = nullptr;
    QLineEdit* discFilterStatusEdit_ = nullptr;
    QSpinBox* discConcurrencySpin_ = nullptr;
    QSpinBox* discDelaySpin_ = nullptr;
    QCheckBox* discRecurseCheck_ = nullptr;
    QSpinBox* discRecurseDepthSpin_ = nullptr;
    QCheckBox* discAutoCalibrateCheck_ = nullptr;
    QCheckBox* discFollowRedirectCheck_ = nullptr;
    QLineEdit* discCookieEdit_ = nullptr;
    QLineEdit* discUserAgentEdit_ = nullptr;
    widgets::AidaButton* discStartButton_ = nullptr;
    widgets::AidaButton* discStopButton_ = nullptr;
    widgets::AidaButton* discRemoveButton_ = nullptr;
    QTableView* discRunsView_ = nullptr;
    QtReconRunModel* discRunModel_ = nullptr;
    widgets::AidaStateView* discRunsEmpty_ = nullptr;
    QLabel* discDetailLabel_ = nullptr;
    QTableView* discDetailView_ = nullptr;
    QtReconResultModel* discDetailModel_ = nullptr;
    widgets::AidaStateView* discDetailEmpty_ = nullptr;
    std::uint64_t disc_selected_ = 0;

    QLineEdit* subDomainEdit_ = nullptr;
    QLineEdit* subWordlistEdit_ = nullptr;
    QSpinBox* subConcurrencySpin_ = nullptr;
    QCheckBox* subPassiveCheck_ = nullptr;
    QCheckBox* subBruteCheck_ = nullptr;
    QCheckBox* subCrtshCheck_ = nullptr;
    QCheckBox* subBufferoverCheck_ = nullptr;
    QCheckBox* subHackertargetCheck_ = nullptr;
    widgets::AidaButton* subStartButton_ = nullptr;
    widgets::AidaButton* subStopButton_ = nullptr;
    widgets::AidaButton* subRemoveButton_ = nullptr;
    widgets::AidaButton* subExportButton_ = nullptr;
    QTableView* subRunsView_ = nullptr;
    QtReconRunModel* subRunModel_ = nullptr;
    widgets::AidaStateView* subRunsEmpty_ = nullptr;
    QLabel* subDetailLabel_ = nullptr;
    QTableView* subDetailView_ = nullptr;
    QtReconResultModel* subDetailModel_ = nullptr;
    widgets::AidaStateView* subDetailEmpty_ = nullptr;
    std::uint64_t sub_selected_ = 0;

    QLineEdit* payloadFilterEdit_ = nullptr;
    QTableView* payloadSetsView_ = nullptr;
    QtPayloadSetModel* payloadSetModel_ = nullptr;
    widgets::AidaStateView* payloadSetsEmpty_ = nullptr;
    QLabel* payloadLabel_ = nullptr;
    QLabel* payloadDescription_ = nullptr;
    QLabel* payloadCount_ = nullptr;
    QTableView* payloadEntriesView_ = nullptr;
    QtPayloadEntryModel* payloadEntryModel_ = nullptr;
    widgets::AidaStateView* payloadEntriesEmpty_ = nullptr;
    widgets::AidaButton* payloadDeleteButton_ = nullptr;
    QLineEdit* payloadNewIdEdit_ = nullptr;
    QLineEdit* payloadNewLabelEdit_ = nullptr;
    QLineEdit* payloadNewDescEdit_ = nullptr;
    QtByteCappedPlainTextEdit* payloadNewEntriesEdit_ = nullptr;
    widgets::AidaButton* payloadAddButton_ = nullptr;
    std::string payload_selected_id_;

    QTimer* refreshTimer_ = nullptr;
    QTimer* completionTimer_ = nullptr;
    bool init_kicked_ = false;
};

}
