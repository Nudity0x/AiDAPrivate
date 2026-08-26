#pragma once

#include <QObject>
#include <QModelIndex>
#include <QString>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/network/burp/api_definition.hpp"
#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/shared/snapshot_table_model.hpp"

class QComboBox;
class QContextMenuEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSplitter;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class QtByteCappedPlainTextEdit;

// Byte-verbatim port of api_view::retained_exchange_t (the struct moves with
// the view; the old header dies).
struct QtApiRetainedExchange {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::uint64_t collection_id = 0;
    std::string request_template_id;
    std::string label;
    std::string host;
    std::uint16_t port = 0;
    bool use_tls = false;
    int response_status = 0;
    std::uint64_t response_latency_ms = 0;
    std::uint64_t request_hash = 0;
    std::uint64_t response_hash = 0;
    std::size_t request_size = 0;
    std::size_t response_size = 0;
    std::vector<std::uint8_t> request;
    std::vector<std::uint8_t> response;
};

class QtApiCollectionModel : public SnapshotTableModel<aida::burp::api_definition::api_collection_t> {
    Q_OBJECT
public:
    explicit QtApiCollectionModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;

    int rowForCollectionId(std::uint64_t id) const noexcept;

protected:
    QVariant cellData(const aida::burp::api_definition::api_collection_t& row, int column,
                      int role) const override;
};

class QtApiRequestModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit QtApiRequestModel(QObject* parent = nullptr);

    void adopt(std::shared_ptr<const std::vector<aida::burp::api_definition::api_request_template_t>> rows);
    const aida::burp::api_definition::api_request_template_t* rowAt(int row) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

private:
    std::shared_ptr<const std::vector<aida::burp::api_definition::api_request_template_t>> rows_;
};

class QtApiController : public QObject {
    Q_OBJECT
public:
    explicit QtApiController(QObject* parent = nullptr);

    void refreshCatalog();
    void importDefinition(const QString& what, int formatIndex);
    void auditCollection(std::uint64_t collectionId, const QString& authLines);
    void removeCollection(std::uint64_t collectionId);
    void sendRequest(std::uint64_t collectionId,
                     const aida::burp::api_definition::api_request_template_t& requestTemplate,
                     const QString& pathValues, const QString& queryValues,
                     const QString& headerValues);

    bool importing() const noexcept { return importing_.load(); }
    bool auditing() const noexcept { return auditing_.load(); }
    bool sending() const noexcept { return sending_.load(); }
    QString lastActionMessage() const { return QString::fromStdString(last_action_message_); }
    QString lastActionKind() const { return QString::fromStdString(last_action_kind_); }

    std::shared_ptr<const std::vector<aida::burp::api_definition::api_collection_t>>
    collections() const;
    const QtApiRetainedExchange* retainedFor(std::uint64_t collectionId,
                                             const std::string& templateId) const;

    static bool resolveRetainedArtifact(std::uint64_t exchangeId, std::uint64_t generation,
                                        bool response, std::vector<std::uint8_t>& bytes,
                                        std::string& unavailable_reason);
    static bool resolveRetainedEndpoint(std::uint64_t exchangeId, std::uint64_t generation,
                                        std::string& host, std::uint16_t& port, bool& use_tls,
                                        std::string& unavailable_reason);
    static network_view::artifact_identity_t artifactIdentity(
        const QtApiRetainedExchange& exchange, bool response);

Q_SIGNALS:
    void catalogChanged();
    void actionMessageChanged();
    void busyChanged();
    void responseChanged(std::uint64_t exchangeId);

private:
    void recordRetainedRequest(const QtApiRetainedExchange& exchange);
    void applySendResult(std::uint64_t exchangeId, bool ok, int status,
                         std::uint64_t latencyMs, const std::vector<std::uint8_t>& response,
                         const std::string& errorMessage);
    void setLastAction(const char* kind, const std::string& message);

    std::atomic<bool> importing_{false};
    std::atomic<bool> auditing_{false};
    std::atomic<bool> sending_{false};
    std::string last_action_message_;
    std::string last_action_kind_;
    std::uint64_t import_serial_ = 0;
    std::uint64_t audit_serial_ = 0;
    std::uint64_t send_serial_ = 0;
    std::shared_ptr<const std::vector<aida::burp::api_definition::api_collection_t>> collections_ =
        std::make_shared<const std::vector<aida::burp::api_definition::api_collection_t>>();
};

// QtApiView ports burp/api_view.cpp: import toolbar, three-column splitter
// (collections | requests + audit box | detail + response). All network/parse
// work runs on executor workers; results land on the GUI thread via queued
// invokeMethod against the controller (dropped silently if it is gone,
// qobject.cpp:201-202).
class QtApiView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit QtApiView(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void onCollectionSelected();
    void onRequestSelected();
    void showCollectionContext(const QPoint& viewportPos);
    void presentRemoveCollection(std::uint64_t collectionId, const QString& name);
    void showResponseContext(const QPoint& globalPos,
                             aida::ui::context_menu_open_origin_t origin);
    void refreshDetail();
    void refreshBusy();
    void refreshStatusLine();

    QtApiController* controller_ = nullptr;
    QLineEdit* importEdit_ = nullptr;
    QComboBox* formatCombo_ = nullptr;
    widgets::AidaButton* importButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QTableView* collectionsView_ = nullptr;
    QtApiCollectionModel* collectionModel_ = nullptr;
    widgets::AidaStateView* collectionsEmpty_ = nullptr;
    QTableView* requestsView_ = nullptr;
    QtApiRequestModel* requestModel_ = nullptr;
    widgets::AidaStateView* requestsEmpty_ = nullptr;
    QLineEdit* auditAuthEdit_ = nullptr;
    widgets::AidaButton* auditButton_ = nullptr;
    QLabel* detailHeader_ = nullptr;
    QLabel* detailBase_ = nullptr;
    QLabel* pathParamsLabel_ = nullptr;
    QtByteCappedPlainTextEdit* pathParamsEdit_ = nullptr;
    QLabel* queryParamsLabel_ = nullptr;
    QtByteCappedPlainTextEdit* queryParamsEdit_ = nullptr;
    QtByteCappedPlainTextEdit* headerEdit_ = nullptr;
    widgets::AidaButton* sendButton_ = nullptr;
    widgets::AidaButton* actionsButton_ = nullptr;
    QLabel* responseHeader_ = nullptr;
    QLabel* previewCapLabel_ = nullptr;
    QPlainTextEdit* responseView_ = nullptr;
    QWidget* detailPane_ = nullptr;
    QWidget* detailContent_ = nullptr;
    widgets::AidaStateView* emptyState_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    std::uint64_t selectedCollectionId_ = 0;
    std::string selectedRequestId_;
};

}
