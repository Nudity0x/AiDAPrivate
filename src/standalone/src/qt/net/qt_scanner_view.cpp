#include "qt/net/qt_scanner_view.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#include "core/infra/executor.hpp"
#include "core/network/burp/passive_scanner.hpp"
#include "core/network/burp/scanner_module.hpp"
#include "core/scanner/scanner_async_io.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/task_center.hpp"
#include "core/ui/interaction_context.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/aida_dialog.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/net/qt_scanner_new_audit_dialog.hpp"
#include "qt/network/shared/exchange_context_menu.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_badge.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_headers.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/widgets/aida_state_view.hpp"
#include "qt/widgets/aida_status.hpp"
#include "qt/widgets/aida_toggle.hpp"

namespace aida::qt::net {

namespace {

constexpr std::size_t kMaxNewAuditUrlBytes = 1023;
constexpr std::size_t kMaxNewAuditRequestBytes = 65535;

QColor sevColor(aida::burp::severity_t severity)
{
    const auto& t = theme::tokens();
    switch (severity) {
    case aida::burp::severity_t::info:     return t.info;
    case aida::burp::severity_t::low:      return t.success;
    case aida::burp::severity_t::medium:   return t.warning;
    case aida::burp::severity_t::high:     return widgets::mix_colors(t.warning, t.error, 0.5);
    case aida::burp::severity_t::critical: return t.error;
    }
    return t.text_secondary;
}

widgets::AidaSemantic sevSemantic(aida::burp::severity_t severity)
{
    switch (severity) {
    case aida::burp::severity_t::info:     return widgets::AidaSemantic::Info;
    case aida::burp::severity_t::low:      return widgets::AidaSemantic::Success;
    case aida::burp::severity_t::medium:   return widgets::AidaSemantic::Warning;
    case aida::burp::severity_t::high:     return widgets::AidaSemantic::Error;
    case aida::burp::severity_t::critical: return widgets::AidaSemantic::Error;
    }
    return widgets::AidaSemantic::Neutral;
}

QColor confColor(aida::burp::confidence_t confidence)
{
    const auto& t = theme::tokens();
    switch (confidence) {
    case aida::burp::confidence_t::tentative: return t.text_secondary;
    case aida::burp::confidence_t::firm:      return t.info;
    case aida::burp::confidence_t::certain:   return t.success;
    }
    return t.text_secondary;
}

// Staged new-audit draft for openNewAuditWith when no scanner view instance
// exists yet (the ImGui version staged fixed buffers consumed on next render).
struct scanner_pending_open_t {
    bool pending = false;
    QString url;
    QString raw;
};

scanner_pending_open_t& scannerPendingOpen()
{
    static scanner_pending_open_t store;
    return store;
}

QPointer<QtScannerView> g_scanner_view;

QRect cancelButtonRect(const QRect& rowRect)
{
    const auto& t = theme::tokens();
    const int w = t.spacing.xl * 4;
    const int h = t.table.row_h - t.spacing.xs;
    return QRect(rowRect.right() - t.spacing.xs - w, rowRect.top() + t.spacing.xs, w, h);
}

}

QtAuditListModel::QtAuditListModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtAuditListModel::adopt(std::vector<aida::burp::active_scanner::audit_status_t> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const aida::burp::active_scanner::audit_status_t* QtAuditListModel::rowAt(
    int row) const noexcept
{
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

int QtAuditListModel::rowForId(std::uint64_t id) const noexcept
{
    for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
        if (rows_[static_cast<std::size_t>(row)].id == id)
            return row;
    }
    return -1;
}

int QtAuditListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int QtAuditListModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 1;
}

QVariant QtAuditListModel::data(const QModelIndex& index, int role) const
{
    const auto* row = rowAt(index.isValid() ? index.row() : -1);
    if (!row)
        return {};
    if (role == Qt::ToolTipRole)
        return QStringLiteral("#%1 %2:%3 %4\n%5")
            .arg(static_cast<quint64>(row->id))
            .arg(QString::fromStdString(row->host))
            .arg(row->port)
            .arg(row->tls ? QStringLiteral("https") : QStringLiteral("http"))
            .arg(QString::fromStdString(row->url));
    return {};
}

// Rich audit row: primary "#id host:port scheme", middle-elided URL, progress
// bar, status text; Cancel affordance on the selected running row (the click
// is hit-tested by the view against the same rect â€” with NoEditTriggers the
// delegate editorEvent path is gated off, qabstractitemview.cpp:4380-4398).
// Owner-drawn paint skipping CE_ItemViewItem per the delegate fast path
// (qstyleditemdelegate.cpp:368-379, qcommonstyle.cpp:2299-2376).
class QtAuditDelegate : public QStyledItemDelegate {
public:
    explicit QtAuditDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    std::uint64_t selectedId = 0;

    static int contentRowHeight(const QFontMetrics& fm)
    {
        const auto& t = theme::tokens();
        const int lineH = fm.height();
        return t.spacing.xs + (lineH + t.spacing.xxs) * 2 + t.spacing.xs +
            (t.spacing.sm - t.spacing.xxs) + t.spacing.sm + lineH;
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        static_cast<void>(index);
        return QSize(theme::tokens().row.property_label_w,
            contentRowHeight(QFontMetrics(option.font)));
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        const auto* model = static_cast<const QtAuditListModel*>(index.model());
        const auto* audit = model->rowAt(index.row());
        if (!audit)
            return;
        const auto& t = theme::tokens();
        painter->save();
        const bool selected = audit->id == selectedId;
        if (option.state & QStyle::State_MouseOver)
            painter->fillRect(option.rect, t.hover_wash);
        if (selected) {
            painter->fillRect(option.rect, t.selection);
            painter->fillRect(QRect(option.rect.left(), option.rect.top(), t.radius.xs,
                option.rect.height()), t.accent);
        }
        const QRect content = option.rect.adjusted(t.spacing.md, 0, -t.spacing.md, 0);
        const int lineH = painter->fontMetrics().height();
        const bool showCancel = selected && audit->running;
        const int cancelZone = showCancel ? t.spacing.xl * 4 + t.spacing.sm : 0;
        const int primaryWidth = (std::max)(t.spacing.lg, content.width() - cancelZone);

        painter->setPen(t.text_primary);
        const QString primary = QStringLiteral("#%1  %2:%3  %4")
            .arg(static_cast<quint64>(audit->id))
            .arg(QString::fromStdString(audit->host))
            .arg(audit->port)
            .arg(audit->tls ? QStringLiteral("https") : QStringLiteral("http"));
        painter->drawText(QRect(content.left(), content.top() + t.spacing.xs,
            primaryWidth, lineH), Qt::AlignLeft | Qt::AlignVCenter,
            painter->fontMetrics().elidedText(primary, Qt::ElideRight, primaryWidth));

        painter->setPen(t.text_secondary);
        const QString url = painter->fontMetrics().elidedText(
            QString::fromStdString(audit->url), Qt::ElideMiddle, primaryWidth);
        painter->drawText(QRect(content.left(),
            content.top() + t.spacing.xs + lineH + t.spacing.xxs,
            primaryWidth, lineH), Qt::AlignLeft | Qt::AlignVCenter, url);

        const float fraction = audit->total_probes > 0
            ? static_cast<float>(audit->completed_probes) /
                static_cast<float>(audit->total_probes)
            : 0.0f;
        const int progressY = content.top() + t.spacing.xs +
            (lineH + t.spacing.xxs) * 2 + t.spacing.xs;
        const int progressWidth = (std::max)(1,
            content.width() - (showCancel ? cancelZone + t.spacing.md : 0));
        const QRect barRect(content.left(), progressY, progressWidth,
            t.spacing.sm - t.spacing.xxs);
        painter->setPen(Qt::NoPen);
        painter->setBrush(t.bg_elevated);
        painter->drawRoundedRect(barRect, t.radius.xs, t.radius.xs);
        QRect fillRect = barRect;
        fillRect.setWidth(static_cast<int>(barRect.width() * fraction));
        painter->setBrush(audit->running ? t.accent : t.success);
        painter->drawRoundedRect(fillRect, t.radius.xs, t.radius.xs);

        const QString status = QStringLiteral("%1/%2  %3 issues  %4")
            .arg(static_cast<quint64>(audit->completed_probes))
            .arg(static_cast<quint64>(audit->total_probes))
            .arg(static_cast<quint64>(audit->issues_found))
            .arg(audit->running ? QStringLiteral("Running")
                : audit->cancelled ? QStringLiteral("Cancelled")
                                   : QStringLiteral("Done"));
        painter->setPen(audit->running ? t.warning : t.text_dim);
        painter->drawText(QRect(content.left(), progressY + t.spacing.sm,
            primaryWidth, lineH), Qt::AlignLeft | Qt::AlignVCenter,
            painter->fontMetrics().elidedText(status, Qt::ElideRight, primaryWidth));

        if (showCancel) {
            const QRect cancel = cancelButtonRect(option.rect);
            painter->setPen(QPen(t.border_focus, 1));
            painter->setBrush(t.error_soft);
            painter->drawRoundedRect(cancel, t.radius.sm, t.radius.sm);
            painter->setPen(t.error);
            painter->drawText(cancel, Qt::AlignCenter, QStringLiteral("Cancel"));
        }
        if (option.state & QStyle::State_HasFocus) {
            painter->setPen(QPen(t.border_focus, 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(option.rect.adjusted(1, 1, -1, -1),
                t.radius.sm, t.radius.sm);
        }
        painter->restore();
    }
};

QtIssueModel::QtIssueModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtIssueModel::adopt(std::shared_ptr<const std::vector<aida::burp::issue_t>> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const aida::burp::issue_t* QtIssueModel::rowAt(int row) const noexcept
{
    if (!rows_ || row < 0 || row >= static_cast<int>(rows_->size()))
        return nullptr;
    return &rows_->at(static_cast<std::size_t>(row));
}

int QtIssueModel::rowForId(std::uint64_t id) const noexcept
{
    if (!rows_)
        return -1;
    for (int row = 0; row < static_cast<int>(rows_->size()); ++row) {
        if (rows_->at(static_cast<std::size_t>(row)).id == id)
            return row;
    }
    return -1;
}

int QtIssueModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() || !rows_ ? 0 : static_cast<int>(rows_->size());
}

int QtIssueModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant QtIssueModel::data(const QModelIndex& index, int role) const
{
    const auto* row = rowAt(index.isValid() ? index.row() : -1);
    if (!row)
        return {};
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Severity: return QString::fromLatin1(aida::burp::severity_label(row->severity));
        case Conf:     return QString::fromLatin1(aida::burp::confidence_label(row->confidence));
        case Host:     return QString::fromStdString(row->host);
        case Param:    return QString::fromStdString(row->parameter);
        case Type:     return QString::fromStdString(row->type_key);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (index.column()) {
        case Severity: return sevColor(row->severity);
        case Conf:     return confColor(row->confidence);
        case Param:    return t.text_secondary;
        default:       return t.text_primary;
        }
    }
    if (role == Qt::ToolTipRole) {
        if (index.column() == Host)
            return QString::fromStdString(row->host);
        if (index.column() == Type)
            return QString::fromStdString(row->type_key);
    }
    return {};
}

QVariant QtIssueModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Severity: return QStringLiteral("Severity");
    case Conf:     return QStringLiteral("Conf.");
    case Host:     return QStringLiteral("Host");
    case Param:    return QStringLiteral("Param");
    case Type:     return QStringLiteral("Type");
    default: return {};
    }
}

QtScannerController::QtScannerController(QObject* parent)
    : QObject(parent) {}

bool QtScannerController::initialize()
{
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true))
        return true;
    const bool issuesReady = aida::burp::issue_store::initialize();
    const bool passiveReady = aida::burp::passive_scanner::initialize();
    const bool activeReady = aida::burp::active_scanner::initialize();
    const bool ready = issuesReady && passiveReady && activeReady;
    if (!ready)
        initialized_.store(false, std::memory_order_release);
    return ready;
}

void QtScannerController::shutdown()
{
    if (!initialized_.load())
        return;
    aida::burp::active_scanner::shutdown();
    aida::burp::passive_scanner::shutdown();
    aida::burp::issue_store::shutdown();
    initialized_.store(false);
}

void QtScannerController::setStatusMessage(const QString& message)
{
    status_message_ = message;
    Q_EMIT statusMessageChanged();
}

std::uint64_t QtScannerController::takeStartedAudit(std::uint64_t& dialogGeneration) noexcept
{
    dialogGeneration = started_audit_dialog_generation_.exchange(0, std::memory_order_acq_rel);
    return started_audit_id_.exchange(0, std::memory_order_acq_rel);
}

std::shared_ptr<const std::vector<aida::burp::issue_t>> QtScannerController::issues() const
{
    return std::atomic_load_explicit(&issues_, std::memory_order_acquire);
}

void QtScannerController::submitInitialization()
{
    bool expected = false;
    if (!initialization_requested_.compare_exchange_strong(expected, true,
        std::memory_order_acq_rel))
        return;
    aida::burp::ui_operation::request_t request;
    request.owner = "burp.scanner";
    request.owner_view = "view.network.scanner";
    request.owner_action = "network.scanner.initialize";
    request.label = "Load Scanner state";
    request.target = "Scanner issue and module catalogs";
    request.affected_entity = "Scanner state";
    request.execute = [this]() {
        aida::burp::ui_operation::result_t result;
        result.success = initialize();
        result.message = result.success ? "Scanner state loaded."
                                        : "Scanner initialization failed.";
        return result;
    };
    if (!operation_.submit(std::move(request)))
        initialization_requested_.store(false, std::memory_order_release);
}

void QtScannerController::submitIssueExport()
{
    aida::burp::ui_operation::request_t request;
    request.owner = "burp.scanner";
    request.owner_view = "view.network.scanner";
    request.owner_action = "network.scanner.export_issues";
    request.label = "Export Scanner issues";
    request.target = "Scanner issue export";
    request.affected_entity = "Scanner issues";
    request.execute = []() {
        aida::burp::ui_operation::result_t result;
        aida::burp::issue_filter_t filter;
        const auto document = aida::burp::issue_store::export_json(filter);
        const std::string path = aida::burp::issue_store::storage_path() + ".export.json";
        const std::string payload = document.dump(2);
        if (payload.size() > scanner_async_io::max_serialized_bytes) {
            result.message = "Scanner issue export exceeds the 64 MiB bound.";
            return result;
        }
        const auto written = scanner_async_io::atomic_replace(path, payload,
            true, {}, []() { return true; });
        result.success = written.success;
        result.message = written.success ? "Scanner issues exported to " + path
                                         : written.error;
        return result;
    };
    static_cast<void>(operation_.submit(std::move(request)));
}

void QtScannerController::submitReviewedIssueClear(
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reviewed)
{
    aida::burp::ui_operation::request_t request;
    request.owner = "burp.scanner";
    request.owner_view = "view.network.scanner";
    request.owner_action = "network.scanner.clear_issues";
    request.label = "Clear Scanner issues";
    request.target = std::to_string(reviewed.size()) + " issues";
    request.affected_entity = request.target;
    request.execute = [reviewed = std::move(reviewed)]() {
        aida::burp::ui_operation::result_t result;
        aida::burp::issue_filter_t filter;
        filter.limit = 10000;
        const auto current = aida::burp::issue_store::list(filter);
        if (current.size() != reviewed.size()) {
            result.message = "The issue catalog changed after review; no issues were cleared.";
            return result;
        }
        for (std::size_t index = 0; index < current.size(); ++index) {
            if (current[index].id != reviewed[index].first ||
                current[index].seen_ms != reviewed[index].second) {
                result.message = "The issue catalog changed after review; no issues were cleared.";
                return result;
            }
        }
        aida::burp::issue_store::clear();
        result.success = aida::burp::issue_store::count() == 0;
        result.message = result.success ? "Scanner issues cleared."
                                        : "Scanner issue clearing could not be verified.";
        return result;
    };
    static_cast<void>(operation_.submit(std::move(request)));
}

bool QtScannerController::submitAudit(std::vector<std::uint8_t> raw, std::string url,
    aida::burp::active_scanner::audit_config_t config, std::uint64_t dialogGeneration)
{
    aida::burp::ui_operation::request_t request;
    request.owner = "burp.scanner";
    request.owner_view = "view.network.scanner";
    request.owner_action = "network.scanner.start_audit";
    request.label = "Start Scanner audit";
    request.target = url;
    request.affected_entity = url;
    request.execute = [this, raw = std::move(raw), url = std::move(url),
                       config = std::move(config), dialogGeneration]() mutable {
        aida::burp::ui_operation::result_t result;
        const std::uint64_t id = aida::burp::active_scanner::enqueue_target(raw, url,
            config);
        result.success = id != 0;
        result.message = result.success ? "Scanner audit started."
                                        : aida::burp::active_scanner::last_error();
        if (id != 0) {
            started_audit_dialog_generation_.store(dialogGeneration,
                std::memory_order_release);
            started_audit_id_.store(id, std::memory_order_release);
        }
        return result;
    };
    const bool submitted = operation_.submit(std::move(request));
    audit_submission_pending_.store(submitted, std::memory_order_release);
    return submitted;
}

void QtScannerController::submitPassiveToggle(bool reviewed, bool desired)
{
    aida::burp::ui_operation::request_t request;
    request.owner = "burp.scanner";
    request.owner_view = "view.network.scanner";
    request.owner_action = "network.scanner.passive_toggle";
    request.label = desired ? "Enable passive scanning" : "Disable passive scanning";
    request.target = "Passive Scanner";
    request.affected_entity = request.target;
    request.execute = [reviewed, desired]() {
        aida::burp::ui_operation::result_t result;
        if (aida::burp::passive_scanner::is_enabled() != reviewed) {
            result.message = "Passive Scanner state changed before the toggle was applied.";
            return result;
        }
        aida::burp::passive_scanner::set_enabled(desired);
        result.success = aida::burp::passive_scanner::is_enabled() == desired;
        result.message = result.success
            ? desired ? "Passive scanning enabled." : "Passive scanning disabled."
            : "Passive Scanner did not reach the requested state.";
        return result;
    };
    static_cast<void>(operation_.submit(std::move(request)));
}

void QtScannerController::requestIssueSnapshot(aida::burp::issue_filter_t filter)
{
    bool expected = false;
    if (!issues_refresh_pending_.compare_exchange_strong(expected, true,
        std::memory_order_acq_rel))
        return;
    filter.limit = 10000;
    QPointer<QtScannerController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.scanner";
    submission.label = "scanner.refresh_issues";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 4;
    submission.body = [guard, filter = std::move(filter)]() mutable {
        auto rows = aida::burp::issue_store::list(filter);
        std::shared_ptr<const std::vector<aida::burp::issue_t>> publication =
            std::make_shared<const std::vector<aida::burp::issue_t>>(std::move(rows));
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(guard.data(),
            [guard, publication = std::move(publication)]() mutable {
                auto* self = guard.data();
                if (!self)
                    return;
                std::atomic_store_explicit(&self->issues_, std::move(publication),
                    std::memory_order_release);
                self->issues_refresh_pending_.store(false, std::memory_order_release);
                Q_EMIT self->issuesChanged();
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted)
        issues_refresh_pending_.store(false, std::memory_order_release);
}

bool QtScannerController::resolveRetainedArtifact(std::uint64_t issueId,
    std::uint64_t seenMs, std::uint64_t evidenceIndex, bool response,
    std::vector<std::uint8_t>& bytes, std::string& reason)
{
    aida::burp::issue_t issue;
    if (!aida::burp::issue_store::get(issueId, issue) || issue.seen_ms != seenMs) {
        reason = "The Scanner issue changed or is no longer retained.";
        return false;
    }
    if (evidenceIndex >= static_cast<std::uint64_t>(issue.evidence.size())) {
        reason = "The reviewed Scanner evidence is no longer retained.";
        return false;
    }
    const auto retainedIndex = static_cast<std::size_t>(evidenceIndex);
    const auto& text = response ? issue.evidence[retainedIndex].response_raw
                                : issue.evidence[retainedIndex].request_raw;
    bytes.assign(text.begin(), text.end());
    reason.clear();
    return true;
}

bool QtScannerController::resolveRetainedEndpoint(std::uint64_t issueId,
    std::uint64_t seenMs, std::string& host, std::uint16_t& port, bool& useTls,
    std::string& reason)
{
    aida::burp::issue_t issue;
    if (!aida::burp::issue_store::get(issueId, issue) || issue.seen_ms != seenMs) {
        reason = "The Scanner issue changed or is no longer retained.";
        return false;
    }
    host = issue.host;
    port = issue.port;
    useTls = issue.scheme == "https";
    if (host.empty() || port == 0) {
        reason = "The retained Scanner evidence has no canonical endpoint.";
        return false;
    }
    reason.clear();
    return true;
}

network_view::artifact_identity_t QtScannerController::evidenceIdentity(
    const aida::burp::issue_t& issue, std::size_t evidenceIndex, bool response)
{
    const auto& text = response ? issue.evidence[evidenceIndex].response_raw
                                : issue.evidence[evidenceIndex].request_raw;
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    network_view::artifact_identity_t identity;
    identity.id = "scanner." + std::to_string(issue.id) + ".evidence." +
        std::to_string(evidenceIndex) + (response ? ".response" : ".request");
    identity.parent_id = "scanner." + std::to_string(issue.id) + ".evidence." +
        std::to_string(evidenceIndex);
    identity.source_view_id = "view.network.scanner";
    identity.session_id = issue.session_id;
    identity.kind = response ? network_view::artifact_kind_t::scanner_response
                             : network_view::artifact_kind_t::scanner_request;
    identity.source_id = issue.id;
    identity.timestamp = issue.seen_ms;
    identity.revision = evidenceIndex;
    identity.content_size = bytes.size();
    identity.content_hash = network_view::artifact_content_hash(bytes);
    identity.label = std::string("Scanner evidence ") + (response ? "response" : "request") +
        " #" + std::to_string(evidenceIndex + 1);
    identity.target_host = issue.host;
    identity.target_port = issue.port;
    identity.use_tls = issue.scheme == "https";
    return identity;
}

QtScannerView::QtScannerView(QWidget* parent)
    : NetworkPaneBase(parent)
{
    setObjectName(QStringLiteral("view.network.scanner"));
    setRequiresTarget(false);

    const auto& t = theme::tokens();
    controller_ = new QtScannerController(this);
    g_scanner_view = this;

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.xs);

    auto* toolbarFrame = new QFrame(content);
    toolbarFrame->setObjectName(QStringLiteral("view.network.scanner.toolbar"));
    toolbarFrame->setProperty("aidaRole", QStringLiteral("toolbar"));
    auto* toolbar = new QHBoxLayout(toolbarFrame);
    toolbar->setContentsMargins(t.toolbar.padding_x, t.toolbar.padding_y,
        t.toolbar.padding_x, t.toolbar.padding_y);
    toolbar->setSpacing(t.spacing.xs);
    auto* title = new QLabel(QStringLiteral("Scanner"), toolbarFrame);
    title->setFont(theme::fonts::strong());
    toolbar->addWidget(title);
    newAuditButton_ = new widgets::AidaButton(QStringLiteral("New Audit"), toolbarFrame);
    newAuditButton_->setObjectName(QStringLiteral("view.network.scanner.new_audit"));
    newAuditButton_->setKind(widgets::AidaButton::Kind::Primary);
    newAuditButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    toolbar->addWidget(newAuditButton_);
    retryInitButton_ = new widgets::AidaButton(QStringLiteral("Retry initialization"),
        toolbarFrame);
    retryInitButton_->setKind(widgets::AidaButton::Kind::Secondary);
    retryInitButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    retryInitButton_->setVisible(false);
    toolbar->addWidget(retryInitButton_);
    exportButton_ = new widgets::AidaButton(QStringLiteral("Export Issues"), toolbarFrame);
    exportButton_->setObjectName(QStringLiteral("view.network.scanner.export"));
    exportButton_->setKind(widgets::AidaButton::Kind::Secondary);
    exportButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    toolbar->addWidget(exportButton_);
    clearIssuesButton_ = new widgets::AidaButton(QStringLiteral("Clear Issues"), toolbarFrame);
    clearIssuesButton_->setObjectName(QStringLiteral("view.network.scanner.clear_issues"));
    clearIssuesButton_->setKind(widgets::AidaButton::Kind::Destructive);
    clearIssuesButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    toolbar->addWidget(clearIssuesButton_);
    passiveToggle_ = new widgets::AidaToggleSwitch(toolbarFrame);
    passiveToggle_->setObjectName(QStringLiteral("view.network.scanner.passive"));
    passiveToggle_->setToolTip(QStringLiteral("Enable or disable passive scanning of proxied traffic"));
    toolbar->addWidget(passiveToggle_);
    passiveLabel_ = new QLabel(QStringLiteral("Passive"), toolbarFrame);
    passiveLabel_->setProperty("aidaVariant", QStringLiteral("secondary"));
    toolbar->addWidget(passiveLabel_);
    toolbar->addStretch(1);
    scannedItem_ = new widgets::AidaStatusItem(QStringLiteral("Scanned"), QString(),
        widgets::AidaSemantic::Neutral, toolbarFrame);
    issuesItem_ = new widgets::AidaStatusItem(QStringLiteral("Issues"), QString(),
        widgets::AidaSemantic::Neutral, toolbarFrame);
    modulesItem_ = new widgets::AidaStatusItem(QStringLiteral("Modules"), QString(),
        widgets::AidaSemantic::Neutral, toolbarFrame);
    modulesItem_->setSeparatorVisible(false);
    toolbar->addWidget(scannedItem_);
    toolbar->addWidget(issuesItem_);
    toolbar->addWidget(modulesItem_);
    layout->addWidget(toolbarFrame);

    splitter_ = new QSplitter(Qt::Horizontal, content);
    splitter_->setOpaqueResize(true);
    splitter_->setChildrenCollapsible(false);

    auto* auditsPane = new QWidget(splitter_);
    auto* auditsLayout = new QVBoxLayout(auditsPane);
    auditsLayout->setContentsMargins(0, 0, 0, 0);
    auditsLayout->setSpacing(t.spacing.xs);
    auto* auditsHeader = new widgets::AidaSectionHeader(QStringLiteral("Audits"), auditsPane);
    auditsLayout->addWidget(auditsHeader);
    auditModel_ = new QtAuditListModel(auditsPane);
    auditsView_ = new QTableView(auditsPane);
    auditsView_->setObjectName(QStringLiteral("view.network.scanner.audits"));
    auditsView_->horizontalHeader()->hide();
    auditsView_->horizontalHeader()->setStretchLastSection(true);
    auditsView_->verticalHeader()->hide();
    auditsView_->verticalHeader()->setDefaultSectionSize(
        QtAuditDelegate::contentRowHeight(auditsView_->fontMetrics()));
    auditsView_->setAlternatingRowColors(true);
    auditsView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    auditsView_->setSelectionMode(QAbstractItemView::SingleSelection);
    auditsView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auditsView_->setModel(auditModel_);
    auto* auditDelegate = new QtAuditDelegate(auditsView_);
    auditsView_->setItemDelegate(auditDelegate);
    auditsView_->viewport()->installEventFilter(this);
    auditsView_->installEventFilter(this);
    auditsLayout->addWidget(auditsView_, 1);
    auditsEmpty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No audits yet"),
        QStringLiteral("Start a New Audit to probe a target for vulnerabilities."),
        auditsPane);
    auditsLayout->addWidget(auditsEmpty_);

    auto* issuesPane = new QWidget(splitter_);
    auto* issuesLayout = new QVBoxLayout(issuesPane);
    issuesLayout->setContentsMargins(0, 0, 0, 0);
    issuesLayout->setSpacing(t.spacing.xs);
    auto* issuesHeader = new widgets::AidaSectionHeader(QStringLiteral("Issues"), issuesPane);
    issuesLayout->addWidget(issuesHeader);
    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(t.spacing.xs);
    auto* sevLabel = new QLabel(QStringLiteral("Sev:"), issuesPane);
    sevLabel->setProperty("aidaVariant", QStringLiteral("secondary"));
    filterRow->addWidget(sevLabel);
    sevCombo_ = new QComboBox(issuesPane);
    sevCombo_->setObjectName(QStringLiteral("view.network.scanner.filter_sev"));
    for (const char* item : { "Any", "Info", "Low", "Medium", "High", "Critical" })
        sevCombo_->addItem(QString::fromLatin1(item));
    sevCombo_->setToolTip(QStringLiteral("Minimum severity"));
    filterRow->addWidget(sevCombo_);
    auto* confLabel = new QLabel(QStringLiteral("Conf:"), issuesPane);
    confLabel->setProperty("aidaVariant", QStringLiteral("secondary"));
    filterRow->addWidget(confLabel);
    confCombo_ = new QComboBox(issuesPane);
    confCombo_->setObjectName(QStringLiteral("view.network.scanner.filter_conf"));
    for (const char* item : { "Any", "Tentative", "Firm", "Certain" })
        confCombo_->addItem(QString::fromLatin1(item));
    confCombo_->setToolTip(QStringLiteral("Minimum confidence"));
    filterRow->addWidget(confCombo_);
    hostFilterEdit_ = new QLineEdit(issuesPane);
    hostFilterEdit_->setObjectName(QStringLiteral("view.network.scanner.filter_host"));
    hostFilterEdit_->setMaxLength(127);
    hostFilterEdit_->setPlaceholderText(QStringLiteral("Host filter"));
    hostFilterEdit_->setToolTip(QStringLiteral("Substring filter on the issue host"));
    hostFilterEdit_->setClearButtonEnabled(true);
    filterRow->addWidget(hostFilterEdit_);
    typeFilterEdit_ = new QLineEdit(issuesPane);
    typeFilterEdit_->setObjectName(QStringLiteral("view.network.scanner.filter_type"));
    typeFilterEdit_->setMaxLength(127);
    typeFilterEdit_->setPlaceholderText(QStringLiteral("Type filter (e.g. sqli)"));
    typeFilterEdit_->setToolTip(QStringLiteral("Substring filter on the issue type key"));
    typeFilterEdit_->setClearButtonEnabled(true);
    filterRow->addWidget(typeFilterEdit_);
    issuesLayout->addLayout(filterRow);

    issueModel_ = new QtIssueModel(issuesPane);
    issuesView_ = new QTableView(issuesPane);
    issuesView_->setObjectName(QStringLiteral("view.network.scanner.issues"));
    issuesView_->verticalHeader()->hide();
    issuesView_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    issuesView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    issuesView_->horizontalHeader()->setStretchLastSection(true);
    issuesView_->setAlternatingRowColors(true);
    issuesView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    issuesView_->setSelectionMode(QAbstractItemView::SingleSelection);
    issuesView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    issuesView_->setModel(issueModel_);
    issuesView_->horizontalHeader()->resizeSection(QtIssueModel::Severity, t.spacing.xl * 4);
    issuesView_->horizontalHeader()->resizeSection(QtIssueModel::Conf, t.spacing.xl * 4);
    issuesView_->horizontalHeader()->resizeSection(QtIssueModel::Host,
        t.row.property_label_w + t.spacing.section);
    issuesView_->horizontalHeader()->resizeSection(QtIssueModel::Param,
        t.row.property_label_w);
    issuesLayout->addWidget(issuesView_, 55);
    issuesEmpty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No issues match"),
        QStringLiteral("Adjust the filters, select an audit, or start a new audit."),
        issuesPane);
    issuesLayout->addWidget(issuesEmpty_);

    detailPane_ = new QWidget(issuesPane);
    auto* detailLayoutOuter = new QVBoxLayout(detailPane_);
    detailLayoutOuter->setContentsMargins(0, 0, 0, 0);
    detailLayoutOuter->setSpacing(t.spacing.xs);
    detailEmpty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No issue selected"),
        QStringLiteral("Select an issue above to review its details and evidence."),
        detailPane_);
    detailLayoutOuter->addWidget(detailEmpty_);
    auto* detailContent = new QWidget(detailPane_);
    detailContent->setObjectName(QStringLiteral("view.network.scanner.issue_detail"));
    auto* detailLayout = new QVBoxLayout(detailContent);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(t.spacing.xs);
    auto* severityRow = new QHBoxLayout();
    severityRow->setSpacing(t.spacing.xs);
    detailSeverity_ = new widgets::AidaBadge(detailContent);
    severityRow->addWidget(detailSeverity_);
    detailName_ = new QLabel(detailContent);
    detailName_->setWordWrap(true);
    detailName_->setFont(theme::fonts::strong());
    severityRow->addWidget(detailName_, 1);
    detailLayout->addLayout(severityRow);
    detailUrl_ = new QLabel(detailContent);
    detailUrl_->setWordWrap(true);
    detailUrl_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detailLayout->addWidget(detailUrl_);
    detailConfidence_ = new QLabel(detailContent);
    detailConfidence_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detailLayout->addWidget(detailConfidence_);
    detailCwe_ = new QLabel(detailContent);
    detailCwe_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detailLayout->addWidget(detailCwe_);
    auto* descriptionHeader = new QLabel(QStringLiteral("Description"), detailContent);
    descriptionHeader->setFont(theme::fonts::strong());
    detailLayout->addWidget(descriptionHeader);
    detailDescription_ = new QLabel(detailContent);
    detailDescription_->setWordWrap(true);
    detailDescription_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detailLayout->addWidget(detailDescription_);
    auto* remediationHeader = new QLabel(QStringLiteral("Remediation"), detailContent);
    remediationHeader->setFont(theme::fonts::strong());
    detailLayout->addWidget(remediationHeader);
    detailRemediation_ = new QLabel(detailContent);
    detailRemediation_->setWordWrap(true);
    detailRemediation_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detailLayout->addWidget(detailRemediation_);
    auto* evidenceHeader = new QLabel(QStringLiteral("Evidence"), detailContent);
    evidenceHeader->setFont(theme::fonts::strong());
    detailLayout->addWidget(evidenceHeader);
    evidenceHost_ = new QWidget(detailContent);
    evidenceHostLayout_ = new QVBoxLayout(evidenceHost_);
    evidenceHostLayout_->setContentsMargins(0, 0, 0, 0);
    evidenceHostLayout_->setSpacing(t.spacing.xs);
    detailLayout->addWidget(evidenceHost_);
    detailLayout->addStretch(1);
    detailLayoutOuter->addWidget(detailContent, 1);
    detailContent->setVisible(false);
    detailContent_ = detailContent;
    issuesLayout->addWidget(detailPane_, 45);

    splitter_->addWidget(auditsPane);
    splitter_->addWidget(issuesPane);
    splitter_->setStretchFactor(0, 36);
    splitter_->setStretchFactor(1, 64);
    layout->addWidget(splitter_, 1);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(200);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        refreshAudits();
        requestIssuesNow();
        refreshToolbar();
    });
    completionTimer_ = new QTimer(this);
    completionTimer_->setInterval(100);
    connect(completionTimer_, &QTimer::timeout, this, &QtScannerView::observeCompletion);

    connect(newAuditButton_, &QAbstractButton::clicked, this, [this] {
        openNewAuditDialog(QString(), QString());
    });
    connect(retryInitButton_, &QAbstractButton::clicked, this, [this] {
        controller_->submitInitialization();
    });
    connect(exportButton_, &QAbstractButton::clicked, this, [this] {
        controller_->submitIssueExport();
    });
    connect(clearIssuesButton_, &QAbstractButton::clicked, this,
        &QtScannerView::presentClearIssuesReview);
    connect(passiveToggle_, &QAbstractButton::toggled, this, [this](bool checked) {
        const bool reviewed = aida::burp::passive_scanner::is_enabled();
        if (checked != reviewed)
            controller_->submitPassiveToggle(reviewed, checked);
    });
    connect(auditsView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* audit = auditModel_->rowAt(current.isValid() ? current.row() : -1);
            selected_audit_id_ = audit ? audit->id : 0;
            if (auto* delegate = static_cast<QtAuditDelegate*>(auditsView_->itemDelegate()))
                delegate->selectedId = selected_audit_id_;
            auditsView_->viewport()->update();
            requestIssuesNow();
        });
    connect(issuesView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* issue = issueModel_->rowAt(current.isValid() ? current.row() : -1);
            selected_issue_id_ = issue ? issue->id : 0;
            refreshIssueDetail();
        });
    const auto filterChanged = [this](auto&&...) { requestIssuesNow(); };
    connect(sevCombo_, &QComboBox::currentIndexChanged, this, filterChanged);
    connect(confCombo_, &QComboBox::currentIndexChanged, this, filterChanged);
    connect(hostFilterEdit_, &QLineEdit::textChanged, this, filterChanged);
    connect(typeFilterEdit_, &QLineEdit::textChanged, this, filterChanged);
    connect(controller_, &QtScannerController::issuesChanged, this, [this] {
        const auto next = controller_->issues();
        bool changed = issueModel_->rowCount() != static_cast<int>(next->size());
        for (int i = 0; !changed && i < issueModel_->rowCount(); ++i) {
            const auto* have = issueModel_->rowAt(i);
            const auto& incoming = next->at(static_cast<std::size_t>(i));
            if (!have || have->id != incoming.id || have->seen_ms != incoming.seen_ms ||
                have->evidence.size() != incoming.evidence.size())
                changed = true;
        }
        if (changed) {
            issueModel_->adopt(next);
            const int row = issueModel_->rowForId(selected_issue_id_);
            if (row >= 0)
                issuesView_->setCurrentIndex(issueModel_->index(row, 0));
            refreshIssueDetail();
        }
        const bool empty = issueModel_->rowCount() == 0;
        issuesEmpty_->setVisible(empty);
        issuesView_->setVisible(!empty);
        refreshToolbar();
    });
    connect(controller_, &QtScannerController::statusMessageChanged, this, [this] {
        refreshToolbar();
    });
    connect(splitter_, &QSplitter::splitterMoved, this, [this](int, int) {
        applyResponsiveColumns();
    });

    setContent(content);
    refreshToolbar();
}

QtScannerView::~QtScannerView()
{
    if (g_scanner_view == this)
        g_scanner_view = nullptr;
}

void QtScannerView::onPaneShown()
{
    if (!controller_->initialized() && !controller_->initializationRequested() &&
        !controller_->operation().pending())
        controller_->submitInitialization();
    requestIssuesNow();
    refreshTimer_->start();
    completionTimer_->start();
    refreshAudits();
    refreshToolbar();
    if (scannerPendingOpen().pending) {
        const QString url = scannerPendingOpen().url;
        const QString raw = scannerPendingOpen().raw;
        scannerPendingOpen().pending = false;
        scannerPendingOpen().url.clear();
        scannerPendingOpen().raw.clear();
        openNewAuditDialog(url, raw);
    }
}

void QtScannerView::onPaneHidden()
{
    refreshTimer_->stop();
    completionTimer_->stop();
}

void QtScannerView::resizeEvent(QResizeEvent* event)
{
    NetworkPaneBase::resizeEvent(event);
    applyResponsiveColumns();
}

bool QtScannerView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == auditsView_->viewport() && event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QModelIndex index = auditsView_->indexAt(mouse->pos());
        const auto* audit = auditModel_->rowAt(index.isValid() ? index.row() : -1);
        if (audit && audit->id == selected_audit_id_ && audit->running) {
            const QRect buttonRect = cancelButtonRect(auditsView_->visualRect(index));
            if (buttonRect.contains(mouse->pos())) {
                ::diag::log_tagged_fmt("burp", "scanner_view cancel id=%llu",
                    static_cast<unsigned long long>(audit->id));
                aida::burp::active_scanner::cancel_audit(audit->id);
                return true;
            }
        }
    }
    if (watched == auditsView_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) {
            const QModelIndex current = auditsView_->selectionModel()->currentIndex();
            const auto* audit = auditModel_->rowAt(current.isValid() ? current.row() : -1);
            if (audit && audit->running) {
                ::diag::log_tagged_fmt("burp", "scanner_view cancel_key id=%llu",
                    static_cast<unsigned long long>(audit->id));
                aida::burp::active_scanner::cancel_audit(audit->id);
                return true;
            }
        }
    }
    return NetworkPaneBase::eventFilter(watched, event);
}

void QtScannerView::observeCompletion()
{
    const auto completion = controller_->operation().completion();
    if (!completion ||
        completion->generation == controller_->observedOperationGeneration())
        return;
    controller_->setObservedOperationGeneration(completion->generation);
    if (controller_->takeInitializationRequested())
        controller_->setInitialized(completion->result.success);
    if (completion->result.success) {
        std::uint64_t dialogGeneration = 0;
        const std::uint64_t auditId = controller_->takeStartedAudit(dialogGeneration);
        if (auditId != 0) {
            selected_audit_id_ = auditId;
            if (new_audit_dialog_ != nullptr && new_audit_dialog_->isVisible() &&
                dialogGeneration == new_dialog_generation_)
                new_audit_dialog_->accept();
        }
        if (completion->result.message.find("cleared") != std::string::npos)
            selected_issue_id_ = 0;
    }
    if (controller_->auditSubmissionPending())
        controller_->clearAuditSubmissionPending();
    controller_->setStatusMessage(QString::fromStdString(completion->result.message));
    requestIssuesNow();
    refreshToolbar();
}

void QtScannerView::refreshAudits()
{
    auto audits = aida::burp::active_scanner::list_audits();
    std::unordered_set<std::uint64_t> visibleAudits;
    visibleAudits.reserve(audits.size());
    for (const auto& audit : audits) {
        visibleAudits.insert(audit.id);
        const std::string taskId = "network.scanner.audit." + std::to_string(audit.id);
        const float auditProgress = audit.total_probes == 0
            ? -1.0f
            : (std::min)(1.0f, static_cast<float>(audit.completed_probes) /
                static_cast<float>(audit.total_probes));
        if (audit.running && task_center_audits_.insert(audit.id).second) {
            aida::ui::task_center::task_registration_t registration;
            registration.id = taskId;
            registration.owner = "network.scanner";
            registration.owner_view = "view.network.scanner";
            registration.owner_action = "network.scanner.cancel_audit";
            registration.label = "Scanner audit: " + audit.host;
            registration.stage = "Running active audit";
            registration.cancellation_is_safe = true;
            registration.callbacks.cancel = [id = audit.id] {
                return aida::burp::active_scanner::cancel_audit(id);
            };
            registration.callbacks.focus = [] {
                static_cast<void>(network_view::open_view("view.network.scanner"));
            };
            if (!aida::ui::task_center::register_task(std::move(registration)))
                task_center_audits_.erase(audit.id);
            else
                static_cast<void>(aida::ui::task_center::update_task(taskId,
                    aida::ui::task_center::task_state_t::running, auditProgress,
                    "Running active audit"));
        } else if (audit.running && task_center_audits_.count(audit.id) != 0U) {
            static_cast<void>(aida::ui::task_center::update_task(taskId,
                aida::ui::task_center::task_state_t::running, auditProgress,
                "Running active audit"));
        } else if (!audit.running && task_center_audits_.count(audit.id) != 0U &&
                   terminal_audits_.insert(audit.id).second) {
            const bool transportFailed = !audit.cancelled &&
                audit.responses_received == 0 && audit.transport_failures != 0;
            static_cast<void>(aida::ui::task_center::update_task(taskId,
                audit.cancelled ? aida::ui::task_center::task_state_t::cancelled
                    : transportFailed ? aida::ui::task_center::task_state_t::failed
                                      : aida::ui::task_center::task_state_t::completed,
                1.0f,
                audit.cancelled ? "Cancelled" : transportFailed ? "Transport failed"
                                                                : "Completed",
                transportFailed ? audit.last_transport_error : std::string{}));
            task_center_audits_.erase(audit.id);
        }
    }
    for (auto it = task_center_audits_.begin(); it != task_center_audits_.end();) {
        if (visibleAudits.count(*it) != 0U) {
            ++it;
            continue;
        }
        const std::string taskId = "network.scanner.audit." + std::to_string(*it);
        static_cast<void>(aida::ui::task_center::update_task(taskId,
            aida::ui::task_center::task_state_t::interrupted, 1.0f,
            "Audit no longer exists in the scanner registry"));
        it = task_center_audits_.erase(it);
    }
    constexpr std::size_t maximumTerminalAudits = 4096U;
    while (terminal_audits_.size() > maximumTerminalAudits)
        terminal_audits_.erase(terminal_audits_.begin());

    bool auditsChanged = auditModel_->rowCount() != static_cast<int>(audits.size());
    for (int i = 0; !auditsChanged && i < auditModel_->rowCount(); ++i) {
        const auto* have = auditModel_->rowAt(i);
        const auto& incoming = audits[static_cast<std::size_t>(i)];
        if (!have || have->id != incoming.id || have->host != incoming.host ||
            have->port != incoming.port || have->tls != incoming.tls ||
            have->url != incoming.url || have->running != incoming.running ||
            have->cancelled != incoming.cancelled ||
            have->completed_probes != incoming.completed_probes ||
            have->total_probes != incoming.total_probes ||
            have->issues_found != incoming.issues_found)
            auditsChanged = true;
    }
    if (auditsChanged) {
        auditModel_->adopt(std::move(audits));
        const int row = auditModel_->rowForId(selected_audit_id_);
        if (row >= 0)
            auditsView_->setCurrentIndex(auditModel_->index(row, 0));
    }
    auditsEmpty_->setVisible(auditModel_->rowCount() == 0);
    auditsView_->setVisible(auditModel_->rowCount() != 0);
}

void QtScannerView::requestIssuesNow()
{
    aida::burp::issue_filter_t filter;
    if (sevCombo_->currentIndex() > 0) {
        filter.has_severity_min = true;
        filter.severity_min =
            static_cast<aida::burp::severity_t>(sevCombo_->currentIndex() - 1);
    }
    if (confCombo_->currentIndex() > 0) {
        filter.has_confidence_min = true;
        filter.confidence_min =
            static_cast<aida::burp::confidence_t>(confCombo_->currentIndex() - 1);
    }
    if (!hostFilterEdit_->text().isEmpty())
        filter.host_substring = hostFilterEdit_->text().toStdString();
    if (!typeFilterEdit_->text().isEmpty())
        filter.type_key_substring = typeFilterEdit_->text().toStdString();
    if (selected_audit_id_ != 0) {
        filter.has_audit_id = true;
        filter.audit_id = selected_audit_id_;
    }
    controller_->requestIssueSnapshot(std::move(filter));
}

void QtScannerView::refreshIssueDetail()
{
    const auto issues = controller_->issues();
    const aida::burp::issue_t* selected = nullptr;
    for (const auto& issue : *issues) {
        if (issue.id == selected_issue_id_)
            selected = &issue;
    }
    const bool have = selected != nullptr;
    if (!have) {
        detail_key_id_ = 0;
        detail_key_seen_ms_ = 0;
        detail_key_evidence_ = 0;
        detailEmpty_->setVisible(true);
        detailContent_->setVisible(false);
        return;
    }
    const auto& issue = *selected;
    if (detail_key_id_ == issue.id && detail_key_seen_ms_ == issue.seen_ms &&
        detail_key_evidence_ == issue.evidence.size())
        return;
    detail_key_id_ = issue.id;
    detail_key_seen_ms_ = issue.seen_ms;
    detail_key_evidence_ = issue.evidence.size();
    detailEmpty_->setVisible(false);
    detailContent_->setVisible(true);
    const auto& t = theme::tokens();
    detailSeverity_->setText(QString::fromLatin1(aida::burp::severity_label(issue.severity)));
    detailSeverity_->setKind(sevSemantic(issue.severity));
    detailName_->setText(QString::fromStdString(issue.name));
    detailUrl_->setText(QStringLiteral("%1://%2:%3%4   param=%5   ip=%6")
        .arg(QString::fromStdString(issue.scheme))
        .arg(QString::fromStdString(issue.host))
        .arg(issue.port)
        .arg(QString::fromStdString(issue.path))
        .arg(QString::fromStdString(issue.parameter))
        .arg(QString::fromStdString(issue.insertion_point)));
    detailConfidence_->setText(QStringLiteral("Confidence: %1   Type: %2")
        .arg(QString::fromLatin1(aida::burp::confidence_label(issue.confidence)))
        .arg(QString::fromStdString(issue.type_key)));
    if (issue.cwe.empty()) {
        detailCwe_->setVisible(false);
    } else {
        QString cwe;
        for (std::size_t i = 0; i < issue.cwe.size(); ++i) {
            if (i)
                cwe += QStringLiteral(", ");
            cwe += QString::fromStdString(issue.cwe[i]);
        }
        detailCwe_->setText(QStringLiteral("CWE: %1").arg(cwe));
        detailCwe_->setVisible(true);
    }
    detailDescription_->setText(QString::fromStdString(issue.description));
    detailRemediation_->setText(QString::fromStdString(issue.remediation));

    while (auto* item = evidenceHostLayout_->takeAt(0)) {
        if (auto* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    for (std::size_t i = 0; i < issue.evidence.size(); ++i) {
        auto* marker = new QLabel(QStringLiteral("Evidence #%1  marker=%2")
            .arg(static_cast<quint64>(i + 1))
            .arg(QString::fromStdString(issue.evidence[i].marker)), evidenceHost_);
        marker->setWordWrap(true);
        marker->setProperty("aidaVariant", QStringLiteral("secondary"));
        evidenceHostLayout_->addWidget(marker);
        for (bool response : { false, true }) {
            const auto& text = response ? issue.evidence[i].response_raw
                                        : issue.evidence[i].request_raw;
            if (text.empty())
                continue;
            auto* label = new QLabel(response ? QStringLiteral("Response:")
                                              : QStringLiteral("Request:"), evidenceHost_);
            label->setProperty("aidaVariant", QStringLiteral("secondary"));
            evidenceHostLayout_->addWidget(label);
            auto* artifactView = new QPlainTextEdit(evidenceHost_);
            artifactView->setObjectName(QStringLiteral("view.network.scanner.evidence.%1.%2")
                .arg(static_cast<quint64>(i))
                .arg(response ? QStringLiteral("resp") : QStringLiteral("req")));
            artifactView->setReadOnly(true);
            artifactView->setFont(theme::fonts::codeRegular());
            artifactView->setMaximumHeight(t.table.row_h * 4);
            artifactView->setPlainText(QString::fromStdString(text));
            artifactView->setContextMenuPolicy(Qt::CustomContextMenu);
            artifactView->setToolTip(QStringLiteral(
                "Right-click, Menu, or Shift+F10 for request/response actions"));
            const auto issueCopy = issue;
            connect(artifactView, &QWidget::customContextMenuRequested, this,
                [this, issueCopy, i, response, artifactView](const QPoint& viewportPos) {
                    showEvidenceContext(issueCopy, i, response, artifactView,
                        artifactView->viewport()->mapToGlobal(viewportPos),
                        aida::ui::context_menu_open_origin_t::pointer);
                });
            evidenceHostLayout_->addWidget(artifactView);
        }
    }
}

void QtScannerView::showEvidenceContext(const aida::burp::issue_t& issue,
    std::size_t evidenceIndex, bool response, QWidget* invoker, const QPoint& globalPos,
    aida::ui::context_menu_open_origin_t origin)
{
    if (evidenceIndex >= issue.evidence.size())
        return;
    const auto primary = QtScannerController::evidenceIdentity(issue, evidenceIndex,
        response);
    network_view::artifact_identity_t related;
    const auto& relatedText = response ? issue.evidence[evidenceIndex].request_raw
                                       : issue.evidence[evidenceIndex].response_raw;
    if (!relatedText.empty())
        related = QtScannerController::evidenceIdentity(issue, evidenceIndex, !response);
    exchange_context_host().show(invoker, globalPos, primary, related,
        origin == aida::ui::context_menu_open_origin_t::pointer
            ? network_view::exchange_context_origin_t::pointer
            : network_view::exchange_context_origin_t::menu_key);
}

void QtScannerView::refreshToolbar()
{
    const auto completion = controller_->operation().completion();
    const bool initFailed = !controller_->initialized() && completion &&
        !completion->result.success && !controller_->operation().pending();
    retryInitButton_->setVisible(initFailed);
    const bool operationPending = controller_->operation().pending();
    const bool ready = controller_->initialized();
    exportButton_->setEnabled(!operationPending && ready);
    clearIssuesButton_->setEnabled(!operationPending && ready);
    passiveToggle_->setEnabled(!operationPending);
    {
        const QSignalBlocker blocker(passiveToggle_);
        passiveToggle_->setChecked(aida::burp::passive_scanner::is_enabled());
    }
    const auto stats = aida::burp::passive_scanner::get_stats();
    scannedItem_->setValue(QString::number(static_cast<quint64>(stats.exchanges_scanned)));
    issuesItem_->setValue(QString::number(
        static_cast<quint64>(aida::burp::issue_store::count())));
    modulesItem_->setValue(QString::number(
        static_cast<quint64>(aida::burp::scanner::count())));
}

void QtScannerView::applyResponsiveColumns()
{
    const auto& t = theme::tokens();
    auto* header = issuesView_->horizontalHeader();
    const qreal density = (std::max)(1.0, static_cast<qreal>(fontMetrics().height()) /
        static_cast<qreal>(theme::fonts::body().pixelSize()));
    const qreal usable = (std::max)(1.0, static_cast<qreal>(
        issuesView_->viewport()->width()) - static_cast<qreal>(t.spacing.md));
    const qreal unit = t.shell.min_panel_w * density;
    header->setSectionHidden(QtIssueModel::Host, !(usable >= unit * 3.0));
    header->setSectionHidden(QtIssueModel::Conf, !(usable >= unit * 5.0));
    header->setSectionHidden(QtIssueModel::Param, !(usable >= unit * 6.5));
}

void QtScannerView::presentClearIssuesReview()
{
    const auto issues = controller_->issues();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reviewed;
    reviewed.reserve(issues->size());
    for (const auto& issue : *issues)
        reviewed.emplace_back(issue.id, issue.seen_ms);
    if (reviewed.empty())
        return;

    auto* dialog = new aida::qt::bridge::AidaDialog(this);
    dialog->setWindowTitle(QStringLiteral("Review Scanner issue clearing"));
    dialog->setMinimumWidth(dialog_min_width_chars(dialog, 48));
    dialog->setMinimumHeight(editor_min_height_lines(dialog, 12));
    dialog->resize(dialog_min_width_chars(dialog, 60), editor_min_height_lines(dialog, 16));
    auto* layout = new QVBoxLayout(dialog);
    auto* title = new QLabel(QStringLiteral("Permanently clear all Scanner issues?"),
        dialog);
    title->setFont(theme::fonts::strong());
    layout->addWidget(title);
    auto* count = new QLabel(QStringLiteral("Affected issues: %1")
        .arg(static_cast<quint64>(reviewed.size())), dialog);
    layout->addWidget(count);
    auto* body = new QLabel(QStringLiteral(
        "The exact reviewed issue identities and timestamps will be revalidated before persistence."),
        dialog);
    body->setProperty("aidaVariant", QStringLiteral("secondary"));
    body->setWordWrap(true);
    layout->addWidget(body);
    layout->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Clear issues"));
    connect(buttons, &QDialogButtonBox::accepted, dialog,
        [this, dialog, reviewed = std::move(reviewed)]() mutable {
            controller_->submitReviewedIssueClear(std::move(reviewed));
            dialog->accept();
        });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void QtScannerView::openNewAuditDialog(const QString& url, const QString& rawRequest)
{
    if (!new_audit_dialog_) {
        new_audit_dialog_ = new QtNewAuditDialog(controller_, this);
        connect(new_audit_dialog_, &QtNewAuditDialog::auditSubmitted, this,
            [this](bool submitted) {
                if (submitted)
                    new_audit_dialog_->setPending(true);
            });
    }
    ++new_dialog_generation_;
    if (new_dialog_generation_ == 0)
        ++new_dialog_generation_;
    new_audit_dialog_->openStaged(url, rawRequest, new_dialog_generation_);
    new_audit_dialog_->raise();
    new_audit_dialog_->activateWindow();
}

bool QtScannerView::openNewAuditWith(const std::string& url, const std::string& raw_request)
{
    if (url.size() > kMaxNewAuditUrlBytes ||
        raw_request.size() > kMaxNewAuditRequestBytes ||
        http_text::contains_binary_bytes(url) ||
        http_text::contains_binary_bytes(raw_request))
        return false;
    if (g_scanner_view) {
        if (g_scanner_view->new_audit_dialog_ != nullptr &&
            g_scanner_view->new_audit_dialog_->isVisible())
            return false;
        g_scanner_view->openNewAuditDialog(QString::fromStdString(url),
            QString::fromStdString(raw_request));
        return true;
    }
    scannerPendingOpen().pending = true;
    scannerPendingOpen().url = QString::fromStdString(url);
    scannerPendingOpen().raw = QString::fromStdString(raw_request);
    return true;
}

}
