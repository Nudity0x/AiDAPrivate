#pragma once

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QModelIndex>
#include <QVariant>
#include <QVector>

#include <memory>
#include <string>
#include <vector>

#include "core/network/intercept/diagnostics.hpp"
#include "core/network/intercept/instrumentation_provider.hpp"
#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QContextMenuEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QSplitter;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaPill;
class AidaStateView;
}

namespace aida::qt::net {

class ProxyHistoryModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Id = 0, Method, Host, Path, Status, Time, Size, Tls, ColumnCount };

    explicit ProxyHistoryModel(QObject* parent = nullptr);

    void adoptHistory(const std::vector<mitm_proxy::http_exchange>& history);
    const mitm_proxy::http_exchange* rowAt(int row) const noexcept;
    int rowForExchangeId(std::uint64_t id) const noexcept;
    void setFilter(const QString& filter);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QVariant cellData(const mitm_proxy::http_exchange& row, int column, int role) const;
    bool matchesFilter(const mitm_proxy::http_exchange& exchange) const;

    std::vector<mitm_proxy::http_exchange> rows_;
    QString filter_;
};

class ProxySparkline : public QWidget {
    Q_OBJECT
public:
    explicit ProxySparkline(QWidget* parent = nullptr);

    void sample(std::uint64_t totalRequests);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    static constexpr int kSampleCount = 32;
    float values_[kSampleCount] = {};
    int head_ = 0;
    std::uint64_t lastTotal_ = 0;
    QElapsedTimer clock_;
    bool hasSample_ = false;
};

class ProxyPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit ProxyPane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onSnapshot(std::shared_ptr<const network_view::proxy_runtime_snapshot_t> snapshot);
    void onCertDiagnostics(bool success, cert_intercept::process_diagnostics_t report,
                           std::vector<cert_intercept::provider_status_t> providers,
                           QString status);
    void onCertHandoff(bool success, QString status);
    void refreshRuntime();
    void refreshButtons();
    void updateDetail();
    void updateEmptyState();
    void showContextForRow(int row, const QPoint& globalPos,
                           aida::ui::context_menu_open_origin_t origin);
    void revertLegacyPatches();

    ProxyHistoryModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* tableStack_ = nullptr;
    widgets::AidaStateView* emptyView_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QLineEdit* bindAddrEdit_ = nullptr;
    QSpinBox* bindPortSpin_ = nullptr;
    QCheckBox* decodeTlsCheck_ = nullptr;
    widgets::AidaButton* startStopButton_ = nullptr;
    widgets::AidaButton* clearHistoryButton_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    ProxySparkline* sparkline_ = nullptr;
    QLabel* runningPill_ = nullptr;
    QLabel* caPill_ = nullptr;
    QLabel* controlledPill_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    widgets::AidaButton* prepareBrowserButton_ = nullptr;
    widgets::AidaButton* repairTrustButton_ = nullptr;
    widgets::AidaButton* camoufoxControlsButton_ = nullptr;
    QWidget* legacyRow_ = nullptr;
    QLabel* legacyPill_ = nullptr;
    widgets::AidaButton* legacyRevertButton_ = nullptr;
    QSpinBox* certPidSpin_ = nullptr;
    widgets::AidaButton* certDiagButton_ = nullptr;
    QLabel* certStatusLabel_ = nullptr;
    QLabel* certTierLabel_ = nullptr;
    QLabel* certFindingsLabel_ = nullptr;
    QLabel* certProvidersLabel_ = nullptr;
    widgets::AidaButton* handoffButton_ = nullptr;
    QLabel* handoffStatusLabel_ = nullptr;
    QLabel* detailTitle_ = nullptr;
    QLabel* detailMeta_ = nullptr;
    widgets::AidaButton* sendRepeaterButton_ = nullptr;
    widgets::AidaButton* copyUrlButton_ = nullptr;
    QPlainTextEdit* requestView_ = nullptr;
    QPlainTextEdit* responseView_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    cert_intercept::process_diagnostics_t report_;
    std::vector<cert_intercept::provider_status_t> providers_;
    bool hasReport_ = false;
    std::uint64_t selectedExchangeId_ = 0;
    bool bypassActive_ = false;
    std::size_t bypassCount_ = 0;
};

}
