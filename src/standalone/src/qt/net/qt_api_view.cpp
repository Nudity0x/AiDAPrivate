#include "qt/net/qt_api_view.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <utility>

#include "core/infra/executor.hpp"
#include "core/network/burp/audit_http.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/interaction_context.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/aida_dialog.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/shared/exchange_context_menu.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_headers.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

const char* k_format_items[] = {
    "auto", "openapi_json", "openapi_yaml", "swagger_v2", "postman_v2_1", "har",
    "graphql_sdl"
};

aida::burp::api_definition::api_format_t formatFromIndex(int index)
{
    switch (index) {
    case 0: return aida::burp::api_definition::api_format_t::auto_detect;
    case 1: return aida::burp::api_definition::api_format_t::openapi_json;
    case 2: return aida::burp::api_definition::api_format_t::openapi_yaml;
    case 3: return aida::burp::api_definition::api_format_t::swagger_v2;
    case 4: return aida::burp::api_definition::api_format_t::postman_v2_1;
    case 5: return aida::burp::api_definition::api_format_t::har;
    case 6: return aida::burp::api_definition::api_format_t::graphql_sdl;
    }
    return aida::burp::api_definition::api_format_t::auto_detect;
}

std::map<std::string, std::string> parseKvLines(const std::string& text)
{
    std::map<std::string, std::string> out;
    const std::string& s = text;
    std::size_t p = 0;
    while (p < s.size()) {
        std::size_t eol = s.find('\n', p);
        if (eol == std::string::npos)
            eol = s.size();
        std::string line = s.substr(p, eol - p);
        p = eol + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            eq = line.find(':');
        if (eq == std::string::npos)
            continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        const std::size_t kb = k.find_first_not_of(" \t");
        const std::size_t ke = k.find_last_not_of(" \t");
        const std::size_t vb = v.find_first_not_of(" \t");
        const std::size_t ve = v.find_last_not_of(" \t");
        if (kb == std::string::npos || vb == std::string::npos)
            continue;
        out[k.substr(kb, ke - kb + 1)] = v.substr(vb, ve - vb + 1);
    }
    return out;
}

constexpr qsizetype kLabelElideChars = 96;

QString elideMiddle(const QString& text, qsizetype cap)
{
    if (text.size() <= cap)
        return text;
    const qsizetype keep = cap - 1;
    const qsizetype head = keep / 2;
    return text.left(head) + QChar(0x2026) + text.right(keep - head);
}

void setElidedLabelText(QLabel* label, const QString& full)
{
    const QString shown = elideMiddle(full, kLabelElideChars);
    label->setText(shown);
    label->setToolTip(shown.size() < full.size() ? full : QString());
}

// The retained exchange store is process-lifetime (the ImGui view kept it in a
// TU static) and is mutated only on the GUI thread; readers load the
// atomically-published immutable snapshot, so resolution is safe from any
// thread without locks.
struct api_retained_store_t {
    std::shared_ptr<const std::vector<std::shared_ptr<const QtApiRetainedExchange>>> exchanges =
        std::make_shared<const std::vector<std::shared_ptr<const QtApiRetainedExchange>>>();
    std::uint64_t next_exchange_id = 1;
};

api_retained_store_t& apiStore()
{
    static api_retained_store_t store;
    return store;
}

using retained_list_t = std::vector<std::shared_ptr<const QtApiRetainedExchange>>;

std::shared_ptr<const retained_list_t> retainedSnapshot()
{
    return std::atomic_load_explicit(&apiStore().exchanges, std::memory_order_acquire);
}

void publishRetained(std::shared_ptr<const retained_list_t> next)
{
    std::atomic_store_explicit(&apiStore().exchanges, std::move(next),
        std::memory_order_release);
}

void trimRetainedHistory(retained_list_t& working)
{
    constexpr std::size_t byteBudget = 64U * 1024U * 1024U;
    std::size_t retainedBytes = 0;
    for (const auto& exchange : working)
        retainedBytes += exchange->request.size() + exchange->response.size();
    while (working.size() > 1 && (working.size() > 64 || retainedBytes > byteBudget)) {
        retainedBytes -= working.front()->request.size();
        retainedBytes -= working.front()->response.size();
        working.erase(working.begin());
    }
}

class QtApiRemoveCollectionDialog : public aida::qt::bridge::AidaDialog {
public:
    QtApiRemoveCollectionDialog(QtApiController* controller, std::uint64_t collectionId,
                                QString name, QWidget* parent)
        : AidaDialog(parent), controller_(controller), collection_id_(collectionId),
          name_(std::move(name))
    {
        setWindowTitle(QStringLiteral("Remove API Collection"));
        setMinimumWidth(dialog_min_width_chars(this, 48));
        auto* layout = new QVBoxLayout(this);
        auto* body = new QLabel(QStringLiteral(
            "Remove '%1' and every request template in this collection?").arg(name_), this);
        body->setWordWrap(true);
        layout->addWidget(body);
        auto* scope = new QLabel(QStringLiteral(
            "Scope: API definition catalog only. Captured traffic and proxy history are unchanged."), this);
        scope->setProperty("aidaVariant", QStringLiteral("secondary"));
        scope->setWordWrap(true);
        layout->addWidget(scope);
        auto* undo = new QLabel(QStringLiteral(
            "This operation cannot be undone after confirmation."), this);
        undo->setProperty("aidaVariant", QStringLiteral("secondary"));
        undo->setWordWrap(true);
        layout->addWidget(undo);
        staleLabel_ = new QLabel(QStringLiteral(
            "The collection changed after review. Cancel and select it again."), this);
        staleLabel_->setProperty("aidaVariant", QStringLiteral("error"));
        staleLabel_->setWordWrap(true);
        staleLabel_->setVisible(false);
        layout->addWidget(staleLabel_);
        layout->addStretch(1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
            this);
        confirmButton_ = buttons->button(QDialogButtonBox::Ok);
        confirmButton_->setText(QStringLiteral("Remove Collection"));
        connect(buttons, &QDialogButtonBox::rejected, this, [this] { reject(); });
        layout->addWidget(buttons);

        RevalidateScope::hooks_t hooks;
        const std::uint64_t retainedId = collection_id_;
        const std::string retainedName = name_.toStdString();
        hooks.identity_fn = [retainedId, retainedName]() -> QString {
            const auto live = aida::burp::api_definition::list_collections();
            const auto found = std::find_if(live.begin(), live.end(),
                [retainedId, &retainedName](const auto& item) {
                    return item.id == retainedId && item.name == retainedName;
                });
            return found != live.end() ? QStringLiteral("present") : QString();
        };
        scope_ = &add_revalidate_scope(std::move(hooks), QStringLiteral(
            "The API collection was removed or replaced; select it again"));
        connect(this, &AidaDialog::validationStale, this, [this](const QString&) {
            staleLabel_->setVisible(true);
            confirmButton_->setEnabled(false);
        });
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (!scope_->check_now())
                return;
            controller_->removeCollection(collection_id_);
            accept();
        });
    }

private:
    QtApiController* controller_ = nullptr;
    std::uint64_t collection_id_ = 0;
    QString name_;
    QLabel* staleLabel_ = nullptr;
    QPushButton* confirmButton_ = nullptr;
    RevalidateScope* scope_ = nullptr;
};

}

QtApiCollectionModel::QtApiCollectionModel(QObject* parent)
    : SnapshotTableModel(parent) {}

int QtApiCollectionModel::rowCount(const QModelIndex& parent) const
{
    return SnapshotTableModel::rowCount(parent);
}

int QtApiCollectionModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 1;
}

int QtApiCollectionModel::rowForCollectionId(std::uint64_t id) const noexcept
{
    const auto* rowsData = rows();
    if (!rowsData)
        return -1;
    for (int row = 0; row < rowsData->size(); ++row) {
        if (rowsData->at(row).id == id)
            return row;
    }
    return -1;
}

QVariant QtApiCollectionModel::cellData(
    const aida::burp::api_definition::api_collection_t& row, int column, int role) const
{
    if (role == Qt::DisplayRole && column == 0) {
        return QStringLiteral("%1 [%2] (%3 reqs)")
            .arg(QString::fromStdString(row.name))
            .arg(QString::fromLatin1(aida::burp::api_definition::format_label(row.format)))
            .arg(static_cast<quint64>(row.requests.size()));
    }
    if (role == Qt::ForegroundRole)
        return theme::tokens().text_secondary;
    if (role == Qt::ToolTipRole)
        return QString::fromStdString(row.source_path);
    return {};
}

QtApiRequestModel::QtApiRequestModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtApiRequestModel::adopt(
    std::shared_ptr<const std::vector<aida::burp::api_definition::api_request_template_t>> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const aida::burp::api_definition::api_request_template_t* QtApiRequestModel::rowAt(
    int row) const noexcept
{
    if (!rows_ || row < 0 || row >= static_cast<int>(rows_->size()))
        return nullptr;
    return &rows_->at(static_cast<std::size_t>(row));
}

int QtApiRequestModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() || !rows_ ? 0 : static_cast<int>(rows_->size());
}

int QtApiRequestModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 1;
}

QVariant QtApiRequestModel::data(const QModelIndex& index, int role) const
{
    const auto* row = rowAt(index.isValid() ? index.row() : -1);
    if (!row)
        return {};
    if (role == Qt::DisplayRole) {
        return QStringLiteral("%1 %2")
            .arg(QString::fromStdString(row->method))
            .arg(QString::fromStdString(row->path.empty() ? row->id : row->path));
    }
    if (role == Qt::ForegroundRole)
        return theme::tokens().text_secondary;
    return {};
}

QtApiController::QtApiController(QObject* parent)
    : QObject(parent) {}

std::shared_ptr<const std::vector<aida::burp::api_definition::api_collection_t>>
QtApiController::collections() const
{
    return std::atomic_load_explicit(&collections_, std::memory_order_acquire);
}

const QtApiRetainedExchange* QtApiController::retainedFor(
    std::uint64_t collectionId, const std::string& templateId) const
{
    const auto snapshot = retainedSnapshot();
    for (auto it = snapshot->rbegin(); it != snapshot->rend(); ++it) {
        if ((*it)->collection_id == collectionId && (*it)->request_template_id == templateId)
            return it->get();
    }
    return nullptr;
}

void QtApiController::setLastAction(const char* kind, const std::string& message)
{
    last_action_kind_ = kind;
    last_action_message_ = message;
    Q_EMIT actionMessageChanged();
}

void QtApiController::refreshCatalog()
{
    auto rows = std::make_shared<const std::vector<aida::burp::api_definition::api_collection_t>>(
        aida::burp::api_definition::list_collections());
    std::atomic_store_explicit(&collections_, std::move(rows), std::memory_order_release);
    Q_EMIT catalogChanged();
}

void QtApiController::importDefinition(const QString& what, int formatIndex)
{
    if (importing_.exchange(true, std::memory_order_acq_rel))
        return;
    Q_EMIT busyChanged();
    const std::string target = what.toStdString();
    const auto format = formatFromIndex(formatIndex);
    ::diag::log_tagged_fmt("api_v", "import what='%s' format_idx=%d", target.c_str(),
        formatIndex);
    QPointer<QtApiController> guard(this);
    const std::uint64_t serial = ++import_serial_;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.api_view";
    submission.label = "api_view.import";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, target, format, serial]() {
        std::uint64_t id = 0;
        if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0)
            id = aida::burp::api_definition::import_from_url(target);
        else
            id = aida::burp::api_definition::import_from_file(target, format);
        std::string error;
        if (id == 0)
            error = aida::burp::api_definition::last_error();
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(), [guard, id, error, serial]() mutable {
            auto* self = guard.data();
            if (!self || serial != self->import_serial_)
                return;
            if (id != 0) {
                ::diag::log_tagged_fmt("api_v", "import_ok id=%llu",
                    static_cast<unsigned long long>(id));
                self->setLastAction("ok",
                    std::string("Imported collection id=") + std::to_string(id));
            } else {
                ::diag::log_tagged_fmt("api_v", "import_failed err='%s'", error.c_str());
                self->setLastAction("error", error);
            }
            self->importing_.store(false, std::memory_order_release);
            Q_EMIT self->busyChanged();
            self->refreshCatalog();
        }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        importing_.store(false, std::memory_order_release);
        setLastAction("error", "The bounded operation queue rejected the request.");
        Q_EMIT busyChanged();
    }
}

void QtApiController::auditCollection(std::uint64_t collectionId, const QString& authLines)
{
    if (auditing_.exchange(true, std::memory_order_acq_rel))
        return;
    Q_EMIT busyChanged();
    const auto auth = parseKvLines(authLines.toStdString());
    ::diag::log_tagged_fmt("api_v", "audit_collection id=%llu",
        static_cast<unsigned long long>(collectionId));
    QPointer<QtApiController> guard(this);
    const std::uint64_t serial = ++audit_serial_;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.api_view";
    submission.label = "api_view.audit_collection";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, collectionId, auth, serial]() {
        aida::burp::api_definition::audit_result_t result;
        const bool ok = aida::burp::api_definition::audit_entire_collection(
            collectionId, auth, result);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(), [guard, ok, result, collectionId, serial]() {
            auto* self = guard.data();
            if (!self || serial != self->audit_serial_)
                return;
            char message[256];
            _snprintf_s(message, sizeof(message), _TRUNCATE,
                "Audit %s: sent=%zu failed=%zu issues=%zu",
                ok ? "completed" : "failed",
                result.requests_sent, result.requests_failed, result.issues_raised);
            ::diag::log_tagged_fmt("api_v",
                "audit_result id=%llu ok=%d sent=%zu failed=%zu issues=%zu",
                static_cast<unsigned long long>(collectionId), ok ? 1 : 0,
                result.requests_sent, result.requests_failed, result.issues_raised);
            self->setLastAction(ok ? "ok" : "error", message);
            self->auditing_.store(false, std::memory_order_release);
            Q_EMIT self->busyChanged();
        }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        auditing_.store(false, std::memory_order_release);
        setLastAction("error", "The bounded operation queue rejected the request.");
        Q_EMIT busyChanged();
    }
}

void QtApiController::removeCollection(std::uint64_t collectionId)
{
    ::diag::log_tagged_fmt("api_v", "collection_remove_queued id=%llu",
        static_cast<unsigned long long>(collectionId));
    QPointer<QtApiController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.api_view";
    submission.label = "api_view.remove_collection";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, collectionId]() {
        aida::burp::api_definition::remove_collection(collectionId);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(), [guard]() {
            auto* self = guard.data();
            if (!self)
                return;
            self->setLastAction("success", "API collection removal queued.");
            self->refreshCatalog();
        }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtApiController::recordRetainedRequest(const QtApiRetainedExchange& exchange)
{
    auto working = std::make_shared<retained_list_t>(*retainedSnapshot());
    working->push_back(std::make_shared<const QtApiRetainedExchange>(exchange));
    trimRetainedHistory(*working);
    publishRetained(std::move(working));
}

void QtApiController::sendRequest(
    std::uint64_t collectionId,
    const aida::burp::api_definition::api_request_template_t& requestTemplate,
    const QString& pathValues, const QString& queryValues, const QString& headerValues)
{
    if (sending_.exchange(true, std::memory_order_acq_rel))
        return;
    Q_EMIT busyChanged();

    const auto pathMap = parseKvLines(pathValues.toStdString());
    const auto queryMap = parseKvLines(queryValues.toStdString());
    const auto headerMap = parseKvLines(headerValues.toStdString());
    aida::burp::api_definition::api_request_template_t templateCopy = requestTemplate;
    std::string scheme, host, parsedPath;
    std::uint16_t port = 0;
    if (!templateCopy.base_url.empty())
        aida::burp::audit_http::parse_url(templateCopy.base_url, scheme, host, port,
            parsedPath);
    const bool tls = scheme == "https";
    std::vector<std::uint8_t> raw =
        aida::burp::api_definition::render_to_raw_request(templateCopy, pathMap, queryMap,
            headerMap, std::string());

    QtApiRetainedExchange exchange;
    exchange.id = apiStore().next_exchange_id++;
    exchange.generation = exchange.id;
    exchange.collection_id = collectionId;
    exchange.request_template_id = requestTemplate.id;
    exchange.label = requestTemplate.method + " " +
        (requestTemplate.path.empty() ? requestTemplate.id : requestTemplate.path);
    exchange.host = host;
    exchange.port = port;
    exchange.use_tls = tls;
    exchange.request = raw;
    exchange.request_size = raw.size();
    exchange.request_hash = http_text::fnv1a64(raw);
    const std::uint64_t exchangeId = exchange.id;
    recordRetainedRequest(exchange);

    ::diag::log_tagged_fmt("api_v", "send_request method='%s' path='%s' base='%s'",
        requestTemplate.method.c_str(), requestTemplate.path.c_str(),
        requestTemplate.base_url.c_str());
    QPointer<QtApiController> guard(this);
    const std::uint64_t serial = ++send_serial_;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.api_view";
    submission.label = "api_view.send_request";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, raw, host, port, tls, exchangeId, serial]() {
        aida::burp::audit_http::send_options_t options;
        options.timeout_ms = 20000;
        options.enforce_scope = false;
        auto response = aida::burp::audit_http::send(raw, host, port, tls, options);
        if (!guard)
            return;
        if (response.has_value()) {
            ::diag::log_tagged_fmt("api_v", "send_response status=%d latency=%llums",
                response->status_code,
                static_cast<unsigned long long>(response->latency_ms));
            std::string out;
            out += "HTTP/1.1 " + std::to_string(response->status_code) + " " +
                response->reason_phrase + "\r\n";
            for (const auto& header : response->resp_headers)
                out += header.first + ": " + header.second + "\r\n";
            out += "\r\n";
            out.append(reinterpret_cast<const char*>(response->resp_body.data()),
                response->resp_body.size());
            std::vector<std::uint8_t> bytes(out.begin(), out.end());
            QMetaObject::invokeMethod(guard.data(),
                [guard, exchangeId, status = response->status_code,
                 latency = response->latency_ms, bytes = std::move(bytes), serial]() mutable {
                    auto* self = guard.data();
                    if (!self || serial != self->send_serial_)
                        return;
                    self->applySendResult(exchangeId, true, status, latency,
                        std::move(bytes), std::string());
                }, Qt::QueuedConnection);
            return;
        }
        const std::string error = aida::burp::audit_http::last_error();
        ::diag::log_tagged_fmt("api_v", "send_failed err='%s'", error.c_str());
        QMetaObject::invokeMethod(guard.data(),
            [guard, exchangeId, error, serial]() {
                auto* self = guard.data();
                if (!self || serial != self->send_serial_)
                    return;
                self->applySendResult(exchangeId, false, 0, 0, {}, error);
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        sending_.store(false, std::memory_order_release);
        setLastAction("error", "The bounded operation queue rejected the request.");
        Q_EMIT busyChanged();
    }
}

void QtApiController::applySendResult(std::uint64_t exchangeId, bool ok, int status,
                                      std::uint64_t latencyMs,
                                      const std::vector<std::uint8_t>& response,
                                      const std::string& errorMessage)
{
    if (ok) {
        const auto snapshot = retainedSnapshot();
        auto working = std::make_shared<retained_list_t>();
        working->reserve(snapshot->size());
        for (const auto& entry : *snapshot) {
            if (entry->id == exchangeId) {
                auto updated = std::make_shared<QtApiRetainedExchange>(*entry);
                updated->response = response;
                updated->response_size = updated->response.size();
                updated->response_hash = http_text::fnv1a64(updated->response);
                updated->response_status = status;
                updated->response_latency_ms = latencyMs;
                working->push_back(std::move(updated));
            } else {
                working->push_back(entry);
            }
        }
        trimRetainedHistory(*working);
        publishRetained(std::move(working));
        setLastAction("ok", std::string("Sent: HTTP ") + std::to_string(status));
        Q_EMIT responseChanged(exchangeId);
    } else {
        setLastAction("error", errorMessage);
    }
    sending_.store(false, std::memory_order_release);
    Q_EMIT busyChanged();
}

bool QtApiController::resolveRetainedArtifact(std::uint64_t exchangeId,
    std::uint64_t generation, bool response, std::vector<std::uint8_t>& bytes,
    std::string& unavailable_reason)
{
    const auto snapshot = retainedSnapshot();
    const auto found = std::find_if(snapshot->begin(), snapshot->end(),
        [exchangeId, generation](const auto& exchange) {
            return exchange->id == exchangeId && exchange->generation == generation;
        });
    if (found == snapshot->end()) {
        unavailable_reason = "The API exchange rolled out of the bounded retained history.";
        return false;
    }
    bytes = response ? (*found)->response : (*found)->request;
    if (bytes.empty()) {
        unavailable_reason = response ? "The API exchange has no retained response."
                                      : "The API exchange has no retained request.";
        return false;
    }
    unavailable_reason.clear();
    return true;
}

bool QtApiController::resolveRetainedEndpoint(std::uint64_t exchangeId,
    std::uint64_t generation, std::string& host, std::uint16_t& port, bool& use_tls,
    std::string& unavailable_reason)
{
    const auto snapshot = retainedSnapshot();
    const auto found = std::find_if(snapshot->begin(), snapshot->end(),
        [exchangeId, generation](const auto& exchange) {
            return exchange->id == exchangeId && exchange->generation == generation;
        });
    if (found == snapshot->end()) {
        unavailable_reason = "The API exchange rolled out of the bounded retained history.";
        return false;
    }
    host = (*found)->host;
    port = (*found)->port;
    use_tls = (*found)->use_tls;
    if (host.empty() || port == 0) {
        unavailable_reason = "The retained API exchange has no canonical endpoint.";
        return false;
    }
    unavailable_reason.clear();
    return true;
}

network_view::artifact_identity_t QtApiController::artifactIdentity(
    const QtApiRetainedExchange& exchange, bool response)
{
    network_view::artifact_identity_t identity;
    const std::uint64_t hash = response ? exchange.response_hash : exchange.request_hash;
    const std::size_t size = response ? exchange.response_size : exchange.request_size;
    if (hash == 0)
        return identity;
    identity.kind = response ? network_view::artifact_kind_t::api_response
                             : network_view::artifact_kind_t::api_request;
    identity.id = "network.api." + std::to_string(exchange.id) +
        (response ? ".response" : ".request");
    identity.parent_id = "network.api." + std::to_string(exchange.id);
    identity.source_view_id = "view.network.api";
    identity.source_id = exchange.id;
    identity.timestamp = exchange.generation;
    identity.revision = exchange.generation;
    identity.content_size = size;
    identity.content_hash = hash;
    identity.label = exchange.label + (response ? " response" : " request");
    identity.target_host = exchange.host;
    identity.target_port = exchange.port;
    identity.use_tls = exchange.use_tls;
    return identity;
}

QtApiView::QtApiView(QWidget* parent)
    : NetworkPaneBase(parent)
{
    setObjectName(QStringLiteral("view.network.api"));
    setRequiresTarget(false);

    const auto& t = theme::tokens();
    controller_ = new QtApiController(this);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.xs);

    auto* importRow = new QHBoxLayout();
    importRow->setSpacing(t.spacing.xs);
    auto* importLabel = new QLabel(QStringLiteral("Import path/url:"), content);
    importLabel->setProperty("aidaVariant", QStringLiteral("secondary"));
    importRow->addWidget(importLabel);
    importEdit_ = new QLineEdit(content);
    importEdit_->setObjectName(QStringLiteral("view.network.api.import"));
    importEdit_->setMaxLength(511);
    importEdit_->setPlaceholderText(QStringLiteral("Local path or http(s):// URL"));
    importEdit_->setClearButtonEnabled(true);
    importRow->addWidget(importEdit_, 1);
    formatCombo_ = new QComboBox(content);
    formatCombo_->setObjectName(QStringLiteral("view.network.api.format"));
    for (const char* item : k_format_items)
        formatCombo_->addItem(QString::fromLatin1(item));
    formatCombo_->setToolTip(QStringLiteral("Definition format (auto-detect works for most inputs)"));
    importRow->addWidget(formatCombo_);
    importButton_ = new widgets::AidaButton(QStringLiteral("Import"), content);
    importButton_->setObjectName(QStringLiteral("view.network.api.import.go"));
    importButton_->setKind(widgets::AidaButton::Kind::Primary);
    importButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    importRow->addWidget(importButton_);
    layout->addLayout(importRow);

    statusLabel_ = new QLabel(content);
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);

    auto* leftPane = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(t.spacing.xs);
    auto* collectionsHeader = new widgets::AidaSectionHeader(
        QStringLiteral("Collections"), leftPane);
    leftLayout->addWidget(collectionsHeader);
    collectionModel_ = new QtApiCollectionModel(leftPane);
    collectionsView_ = new QTableView(leftPane);
    collectionsView_->setObjectName(QStringLiteral("view.network.api.collections"));
    collectionsView_->horizontalHeader()->hide();
    collectionsView_->horizontalHeader()->setStretchLastSection(true);
    collectionsView_->verticalHeader()->hide();
    collectionsView_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    collectionsView_->setAlternatingRowColors(true);
    collectionsView_->setModel(collectionModel_);
    collectionsView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    collectionsView_->setSelectionMode(QAbstractItemView::SingleSelection);
    collectionsView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    collectionsView_->setContextMenuPolicy(Qt::CustomContextMenu);
    leftLayout->addWidget(collectionsView_, 1);
    collectionsEmpty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No API collections"),
        QStringLiteral("Import an OpenAPI, Postman, HAR, or GraphQL definition above."),
        leftPane);
    leftLayout->addWidget(collectionsEmpty_);

    auto* centerPane = new QWidget(splitter);
    auto* centerLayout = new QVBoxLayout(centerPane);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(t.spacing.xs);
    auto* requestsHeader = new widgets::AidaSectionHeader(
        QStringLiteral("Requests"), centerPane);
    centerLayout->addWidget(requestsHeader);
    requestModel_ = new QtApiRequestModel(centerPane);
    requestsView_ = new QTableView(centerPane);
    requestsView_->setObjectName(QStringLiteral("view.network.api.requests"));
    requestsView_->horizontalHeader()->hide();
    requestsView_->horizontalHeader()->setStretchLastSection(true);
    requestsView_->verticalHeader()->hide();
    requestsView_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    requestsView_->setAlternatingRowColors(true);
    requestsView_->setModel(requestModel_);
    requestsView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    requestsView_->setSelectionMode(QAbstractItemView::SingleSelection);
    requestsView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    centerLayout->addWidget(requestsView_, 1);
    requestsEmpty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No request templates"),
        QStringLiteral("Select a collection, or import one with request templates."),
        centerPane);
    centerLayout->addWidget(requestsEmpty_);
    auto* auditLabel = new QLabel(QStringLiteral("Audit all requests:"), centerPane);
    auditLabel->setProperty("aidaVariant", QStringLiteral("secondary"));
    centerLayout->addWidget(auditLabel);
    auditAuthEdit_ = new QLineEdit(centerPane);
    auditAuthEdit_->setObjectName(QStringLiteral("view.network.api.audit.auth"));
    auditAuthEdit_->setMaxLength(1023);
    auditAuthEdit_->setPlaceholderText(
        QStringLiteral("auth values: bearer=<jwt>, api_key=<key>, ..."));
    auditAuthEdit_->setClearButtonEnabled(true);
    centerLayout->addWidget(auditAuthEdit_);
    auditButton_ = new widgets::AidaButton(QStringLiteral("Audit Collection"), centerPane);
    auditButton_->setObjectName(QStringLiteral("view.network.api.audit.go"));
    auditButton_->setKind(widgets::AidaButton::Kind::AccentGradient);
    auditButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    centerLayout->addWidget(auditButton_);

    detailPane_ = new QWidget(splitter);
    auto* detailLayout = new QVBoxLayout(detailPane_);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(t.spacing.xs);
    emptyState_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No request selected"),
        QStringLiteral("Pick a collection on the left, then a request to view and send it."),
        detailPane_);
    detailLayout->addWidget(emptyState_, 1);
    detailContent_ = new QWidget(detailPane_);
    auto* detailContentLayout = new QVBoxLayout(detailContent_);
    detailContentLayout->setContentsMargins(0, 0, 0, 0);
    detailContentLayout->setSpacing(t.spacing.xs);
    detailHeader_ = new QLabel(detailContent_);
    detailHeader_->setFont(theme::fonts::strong());
    detailContentLayout->addWidget(detailHeader_);
    detailBase_ = new QLabel(detailContent_);
    detailBase_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detailContentLayout->addWidget(detailBase_);
    pathParamsLabel_ = new QLabel(QStringLiteral("Path params (one per line, name=value):"),
        detailContent_);
    pathParamsLabel_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detailContentLayout->addWidget(pathParamsLabel_);
    pathParamsEdit_ = new QtByteCappedPlainTextEdit(detailContent_);
    pathParamsEdit_->setObjectName(QStringLiteral("view.network.api.send.path"));
    pathParamsEdit_->setMaxBytes(511);
    pathParamsEdit_->setMaximumHeight(t.table.row_h * 2 + t.spacing.sm);
    pathParamsEdit_->setFont(theme::fonts::codeRegular());
    detailContentLayout->addWidget(pathParamsEdit_);
    queryParamsLabel_ = new QLabel(QStringLiteral("Query params (one per line, name=value):"),
        detailContent_);
    queryParamsLabel_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detailContentLayout->addWidget(queryParamsLabel_);
    queryParamsEdit_ = new QtByteCappedPlainTextEdit(detailContent_);
    queryParamsEdit_->setObjectName(QStringLiteral("view.network.api.send.query"));
    queryParamsEdit_->setMaxBytes(511);
    queryParamsEdit_->setMaximumHeight(t.table.row_h * 2 + t.spacing.sm);
    queryParamsEdit_->setFont(theme::fonts::codeRegular());
    detailContentLayout->addWidget(queryParamsEdit_);
    auto* headerLabel = new QLabel(QStringLiteral("Header overrides (one per line, name=value):"),
        detailContent_);
    headerLabel->setProperty("aidaVariant", QStringLiteral("secondary"));
    detailContentLayout->addWidget(headerLabel);
    headerEdit_ = new QtByteCappedPlainTextEdit(detailContent_);
    headerEdit_->setObjectName(QStringLiteral("view.network.api.send.headers"));
    headerEdit_->setMaxBytes(2047);
    headerEdit_->setMaximumHeight(t.table.row_h * 2 + t.spacing.sm);
    headerEdit_->setFont(theme::fonts::codeRegular());
    detailContentLayout->addWidget(headerEdit_);
    auto* sendRow = new QHBoxLayout();
    sendRow->setSpacing(t.spacing.xs);
    sendButton_ = new widgets::AidaButton(QStringLiteral("Send"), detailContent_);
    sendButton_->setObjectName(QStringLiteral("view.network.api.send.go"));
    sendButton_->setKind(widgets::AidaButton::Kind::Primary);
    sendButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    sendRow->addWidget(sendButton_);
    actionsButton_ = new widgets::AidaButton(QStringLiteral("Actions"), detailContent_);
    actionsButton_->setObjectName(QStringLiteral("view.network.api.response.actions"));
    actionsButton_->setKind(widgets::AidaButton::Kind::Secondary);
    actionsButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionsButton_->setToolTip(QStringLiteral(
        "Exchange actions for the retained request/response (open in editor, copy, save)"));
    sendRow->addWidget(actionsButton_);
    sendRow->addStretch(1);
    detailContentLayout->addLayout(sendRow);
    responseHeader_ = new QLabel(detailContent_);
    responseHeader_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detailContentLayout->addWidget(responseHeader_);
    previewCapLabel_ = new QLabel(detailContent_);
    previewCapLabel_->setProperty("aidaVariant", QStringLiteral("warning"));
    detailContentLayout->addWidget(previewCapLabel_);
    responseView_ = new QPlainTextEdit(detailContent_);
    responseView_->setObjectName(QStringLiteral("view.network.api.response"));
    responseView_->setReadOnly(true);
    responseView_->setFont(theme::fonts::codeRegular());
    responseView_->setPlaceholderText(QStringLiteral(
        "No response captured. Send the request to capture one."));
    responseView_->setContextMenuPolicy(Qt::CustomContextMenu);
    detailContentLayout->addWidget(responseView_, 1);
    detailLayout->addWidget(detailContent_, 1);
    detailContent_->setVisible(false);

    splitter->addWidget(leftPane);
    splitter->addWidget(centerPane);
    splitter->addWidget(detailPane_);
    splitter->setStretchFactor(0, 32);
    splitter->setStretchFactor(1, 36);
    splitter->setStretchFactor(2, 48);
    layout->addWidget(splitter, 1);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(500);
    connect(refreshTimer_, &QTimer::timeout, this, [this] { controller_->refreshCatalog(); });

    const auto submitImport = [this] {
        controller_->importDefinition(importEdit_->text(), formatCombo_->currentIndex());
    };
    connect(importButton_, &QAbstractButton::clicked, this, submitImport);
    connect(importEdit_, &QLineEdit::returnPressed, this, submitImport);
    connect(auditButton_, &QAbstractButton::clicked, this, [this] {
        if (selectedCollectionId_ != 0)
            controller_->auditCollection(selectedCollectionId_, auditAuthEdit_->text());
    });
    connect(sendButton_, &QAbstractButton::clicked, this, [this] {
        const auto collections = controller_->collections();
        const aida::burp::api_definition::api_request_template_t* selected = nullptr;
        for (const auto& collection : *collections) {
            if (collection.id != selectedCollectionId_)
                continue;
            for (const auto& request : collection.requests) {
                if (request.id == selectedRequestId_)
                    selected = &request;
            }
        }
        if (selected != nullptr) {
            const auto templateCopy = *selected;
            controller_->sendRequest(selectedCollectionId_, templateCopy,
                pathParamsEdit_->toPlainText(), queryParamsEdit_->toPlainText(),
                headerEdit_->toPlainText());
        }
    });
    connect(actionsButton_, &QAbstractButton::clicked, this, [this] {
        showResponseContext(actionsButton_->mapToGlobal(QPoint(0, actionsButton_->height())),
            aida::ui::context_menu_open_origin_t::pointer);
    });
    connect(collectionsView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex&, const QModelIndex&) { onCollectionSelected(); });
    connect(requestsView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex&, const QModelIndex&) { onRequestSelected(); });
    connect(collectionsView_, &QWidget::customContextMenuRequested, this,
        &QtApiView::showCollectionContext);
    connect(responseView_, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& viewportPos) {
            showResponseContext(responseView_->viewport()->mapToGlobal(viewportPos),
                aida::ui::context_menu_open_origin_t::pointer);
        });
    connect(controller_, &QtApiController::catalogChanged, this, [this] {
        const auto collections = controller_->collections();
        const auto* current = collectionModel_->rows();
        bool unchanged = current != nullptr &&
            current->size() == static_cast<int>(collections->size());
        for (int i = 0; unchanged && i < current->size(); ++i) {
            const auto& have = current->at(i);
            const auto& next = collections->at(static_cast<std::size_t>(i));
            if (have.id != next.id || have.name != next.name ||
                have.format != next.format || have.requests.size() != next.requests.size())
                unchanged = false;
        }
        if (!unchanged) {
            collectionModel_->adopt(
                std::make_shared<const QVector<aida::burp::api_definition::api_collection_t>>(
                    collections->begin(), collections->end()),
                collectionModel_->generation() + 1);
            const int row = collectionModel_->rowForCollectionId(selectedCollectionId_);
            if (row >= 0)
                collectionsView_->setCurrentIndex(collectionModel_->index(row, 0));
            onCollectionSelected();
        }
        const bool empty = collectionModel_->rowCount() == 0;
        collectionsEmpty_->setVisible(empty);
        collectionsView_->setVisible(!empty);
    });
    connect(controller_, &QtApiController::actionMessageChanged, this,
        &QtApiView::refreshStatusLine);
    connect(controller_, &QtApiController::busyChanged, this, &QtApiView::refreshBusy);
    connect(controller_, &QtApiController::responseChanged, this,
        [this](std::uint64_t) { refreshDetail(); });

    setContent(content);
    refreshStatusLine();
    controller_->refreshCatalog();
}

void QtApiView::onPaneShown()
{
    controller_->refreshCatalog();
    refreshTimer_->start();
}

void QtApiView::onPaneHidden()
{
    refreshTimer_->stop();
}

void QtApiView::onCollectionSelected()
{
    const QModelIndex current = collectionsView_->selectionModel()->currentIndex();
    const auto* collection = collectionModel_->rowAt(current.isValid() ? current.row() : -1);
    selectedCollectionId_ = collection ? collection->id : 0;
    selectedRequestId_.clear();
    if (collection) {
        requestModel_->adopt(
            std::make_shared<const std::vector<aida::burp::api_definition::api_request_template_t>>(
                collection->requests));
    } else {
        requestModel_->adopt(
            std::make_shared<const std::vector<aida::burp::api_definition::api_request_template_t>>());
    }
    const bool noRequests = requestModel_->rowCount() == 0;
    requestsEmpty_->setVisible(noRequests);
    requestsView_->setVisible(!noRequests);
    pathParamsEdit_->clear();
    queryParamsEdit_->clear();
    headerEdit_->clear();
    refreshBusy();
    refreshDetail();
}

void QtApiView::onRequestSelected()
{
    const QModelIndex current = requestsView_->selectionModel()->currentIndex();
    const auto* request = requestModel_->rowAt(current.isValid() ? current.row() : -1);
    selectedRequestId_ = request ? request->id : std::string();
    pathParamsEdit_->clear();
    queryParamsEdit_->clear();
    headerEdit_->clear();
    refreshDetail();
}

void QtApiView::refreshBusy()
{
    importButton_->setEnabled(!controller_->importing());
    auditButton_->setEnabled(selectedCollectionId_ != 0 && !controller_->auditing());
    sendButton_->setEnabled(!selectedRequestId_.empty() && !controller_->sending());
}

void QtApiView::refreshStatusLine()
{
    const QString message = controller_->lastActionMessage();
    const char* variant = "secondary";
    if (!message.isEmpty())
        variant = controller_->lastActionKind() == QStringLiteral("error") ? "error" : "success";
    statusLabel_->setText(message.isEmpty()
        ? QStringLiteral("Drop OpenAPI / Postman / HAR / GraphQL files here, or paste a URL.")
        : message);
    if (statusLabel_->property("aidaVariant").toString() != QLatin1String(variant)) {
        statusLabel_->setProperty("aidaVariant", QString::fromLatin1(variant));
        theme::stylesheet::repolish(statusLabel_);
    }
}

void QtApiView::refreshDetail()
{
    const auto collections = controller_->collections();
    const aida::burp::api_definition::api_collection_t* collection = nullptr;
    const aida::burp::api_definition::api_request_template_t* request = nullptr;
    for (const auto& candidate : *collections) {
        if (candidate.id == selectedCollectionId_)
            collection = &candidate;
    }
    if (collection) {
        for (const auto& candidate : collection->requests) {
            if (candidate.id == selectedRequestId_)
                request = &candidate;
        }
    }
    const bool have = collection != nullptr && request != nullptr;
    emptyState_->setVisible(!have);
    detailContent_->setVisible(have);
    if (!have)
        return;

    setElidedLabelText(detailHeader_, QStringLiteral("%1 %2")
        .arg(QString::fromStdString(request->method))
        .arg(QString::fromStdString(request->path)));
    setElidedLabelText(detailBase_, QStringLiteral("base: %1   auth: %2")
        .arg(request->base_url.empty() ? QStringLiteral("(none)")
                                       : QString::fromStdString(request->base_url))
        .arg(request->auth_kind.empty() ? QStringLiteral("(none)")
                                        : QString::fromStdString(request->auth_kind)));
    const bool havePath = !request->path_params.empty();
    const bool haveQuery = !request->query_params.empty();
    pathParamsLabel_->setVisible(havePath);
    pathParamsEdit_->setVisible(havePath);
    queryParamsLabel_->setVisible(haveQuery);
    queryParamsEdit_->setVisible(haveQuery);

    const auto* retained = controller_->retainedFor(collection->id, request->id);
    const bool hasRetained = retained != nullptr;
    actionsButton_->setEnabled(hasRetained);
    constexpr std::size_t previewLimit = 256U * 1024U;
    const std::size_t fullSize = hasRetained ? retained->response_size : 0;
    responseHeader_->setVisible(hasRetained);
    if (hasRetained) {
        responseHeader_->setText(QStringLiteral("Response (status %1, %2 ms)")
            .arg(retained->response_status)
            .arg(static_cast<quint64>(retained->response_latency_ms)));
    }
    const bool capped = hasRetained && fullSize > previewLimit;
    previewCapLabel_->setVisible(capped);
    if (capped) {
        previewCapLabel_->setText(QStringLiteral(
            "Preview limited to 256 KiB; actions use all %1 bytes")
            .arg(static_cast<quint64>(fullSize)));
    }
    if (hasRetained) {
        const auto& bytes = retained->response;
        const std::size_t previewSize = (std::min)(bytes.size(), previewLimit);
        responseView_->setPlainText(QString::fromUtf8(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<qsizetype>(previewSize)));
    } else {
        responseView_->clear();
    }
    refreshBusy();
}

void QtApiView::showCollectionContext(const QPoint& viewportPos)
{
    const QModelIndex index = collectionsView_->indexAt(viewportPos);
    const auto* collection = collectionModel_->rowAt(index.isValid() ? index.row() : -1);
    if (!collection)
        return;
    collectionsView_->setCurrentIndex(index);

    aida::ui::application_ui::retained_entity_context_t context;
    context.owner_id = "network.api.collection";
    context.entity_id = std::to_string(collection->id);
    context.entity_generation = collection->requests.size();
    context.active_view = aida::ui::stable_view_id_t("view.network.api");
    const auto retainedId = collection->id;
    const auto retainedName = collection->name;
    context.validate_identity = [retainedId, retainedName] {
        const auto live = aida::burp::api_definition::list_collections();
        const auto found = std::find_if(live.begin(), live.end(),
            [retainedId, &retainedName](const auto& item) {
                return item.id == retainedId && item.name == retainedName;
            });
        return found != live.end()
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The API collection was removed or replaced; select it again");
    };
    aida::ui::application_ui::retained_entity_action_t action;
    action.action_id = "network.api.collection.remove_review";
    action.capability = aida::ui::capability_state_t::available();
    QPointer<QtApiView> guard(this);
    action.invoke = [guard, retainedId, retainedName]() {
        if (guard) {
            guard->presentRemoveCollection(retainedId, QString::fromStdString(retainedName));
        }
        return aida::ui::action_handler_result_t::completed();
    };
    context.actions.push_back(std::move(action));
    documents::show_retained_entity_menu(context,
        aida::ui::context_menu_open_origin_t::pointer,
        collectionsView_->viewport()->mapToGlobal(viewportPos), collectionsView_);
}

void QtApiView::presentRemoveCollection(std::uint64_t collectionId, const QString& name)
{
    auto* dialog = new QtApiRemoveCollectionDialog(controller_, collectionId, name, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this] {
        selectedCollectionId_ = 0;
        selectedRequestId_.clear();
        controller_->refreshCatalog();
        onCollectionSelected();
    });
    dialog->open();
}

void QtApiView::showResponseContext(const QPoint& globalPos,
                                    aida::ui::context_menu_open_origin_t origin)
{
    const auto collections = controller_->collections();
    const aida::burp::api_definition::api_collection_t* collection = nullptr;
    const aida::burp::api_definition::api_request_template_t* request = nullptr;
    for (const auto& candidate : *collections) {
        if (candidate.id == selectedCollectionId_)
            collection = &candidate;
    }
    if (!collection)
        return;
    for (const auto& candidate : collection->requests) {
        if (candidate.id == selectedRequestId_)
            request = &candidate;
    }
    if (!request)
        return;
    const auto* retained = controller_->retainedFor(collection->id, request->id);
    if (!retained)
        return;
    const auto requestIdentity = QtApiController::artifactIdentity(*retained, false);
    const auto responseIdentity = QtApiController::artifactIdentity(*retained, true);
    exchange_context_host().show(responseView_, globalPos,
        responseIdentity.valid() ? responseIdentity : requestIdentity,
        responseIdentity.valid() ? requestIdentity : network_view::artifact_identity_t{},
        origin == aida::ui::context_menu_open_origin_t::pointer
            ? network_view::exchange_context_origin_t::pointer
            : network_view::exchange_context_origin_t::menu_key);
}

}
