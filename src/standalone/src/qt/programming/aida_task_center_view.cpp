#include "qt/programming/aida_task_center_view.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QStackedLayout>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <utility>

#include "core/ai/entity_evidence_handoff.hpp"
#include "core/infra/executor.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/widgets/aida_pill.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::programming {
namespace {

QPointer<AidaTaskCenterController> g_task_center_controller;

using aida::ui::task_center::diagnostic_severity_t;
using aida::ui::task_center::diagnostic_snapshot_t;
using aida::ui::task_center::task_snapshot_t;
using aida::ui::task_center::task_state_t;

const task_snapshot_t* find_task(const aida::ui::task_center::immutable_snapshot_t& current,
                                 const std::string& id) {
    const auto found = std::find_if(current.tasks.begin(), current.tasks.end(),
        [&](const task_snapshot_t& item) { return item.id == id; });
    return found == current.tasks.end() ? nullptr : &*found;
}

const diagnostic_snapshot_t* find_diagnostic(
    const aida::ui::task_center::immutable_snapshot_t& current, const std::string& id) {
    const auto found = std::find_if(current.diagnostics.begin(), current.diagnostics.end(),
        [&](const diagnostic_snapshot_t& item) { return item.id == id; });
    return found == current.diagnostics.end() ? nullptr : &*found;
}

bool keyboard_context_menu_event(QObject* watched, QEvent* event, QAbstractItemView* view,
        QModelIndex* index, QPoint* global_pos) {
    if (!view)
        return false;
    if (event->type() != QEvent::ContextMenu)
        return false;
    auto* context_event = static_cast<QContextMenuEvent*>(event);
    if (context_event->reason() != QContextMenuEvent::Keyboard)
        return false;
    if (watched != view && watched != view->viewport())
        return false;
    *index = view->currentIndex();
    *global_pos = index->isValid()
        ? view->viewport()->mapToGlobal(view->visualRect(*index).center())
        : view->viewport()->mapToGlobal(view->viewport()->rect().center());
    return true;
}

std::string diagnostic_copy_text(const diagnostic_snapshot_t& item) {
    return item.id + "\n" + item.summary + "\n" + item.details;
}

aida::ui::application_ui::retained_entity_action_t retained_action(const char* id,
    bool enabled, const char* disabled_reason,
    std::function<aida::ui::action_handler_result_t()> invoke) {
    aida::ui::application_ui::retained_entity_action_t action;
    action.action_id = id;
    action.capability = enabled
        ? aida::ui::capability_state_t::available()
        : aida::ui::capability_state_t::unavailable(disabled_reason);
    action.invoke = std::move(invoke);
    return action;
}

void open_task_context(const task_snapshot_t& item, std::uint64_t snapshot_generation,
                       aida::ui::context_menu_open_origin_t origin,
                       const QPoint& global_pos, QWidget* parent) {
    namespace task_center = aida::ui::task_center;
    using aida::ui::action_handler_result_t;
    aida::ui::application_ui::retained_entity_context_t context;
    context.owner_id = "task-center.tasks";
    context.entity_id = item.id;
    context.entity_generation = snapshot_generation;
    context.active_view = aida::ui::stable_view_id_t("view.background_tasks");
    const std::string id = item.id;
    const std::uint64_t queued_ms = item.queued_ms;
    const std::string owner = item.owner;
    const std::string label = item.label;
    context.validate_identity = [id, queued_ms, owner, label] {
        const auto current = task_center::snapshot();
        if (!current)
            return aida::ui::capability_state_t::unavailable("The task snapshot is unavailable");
        const auto* retained = find_task(*current, id);
        return retained && retained->queued_ms == queued_ms &&
                retained->owner == owner && retained->label == label
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The retained task was removed or replaced; select it again");
    };
    context.actions.push_back(retained_action("task.focus_owner", item.focusable,
        "This task owner did not register a focus target.", [id] {
            return task_center::focus(id) ? action_handler_result_t::completed()
                             : action_handler_result_t::failed("The task owner could not be focused");
        }));
    context.actions.push_back(retained_action("task.open_log", item.log_available,
        "This task owner did not register a retained log target.", [id] {
            return task_center::open_log(id) ? action_handler_result_t::completed()
                                : action_handler_result_t::failed("The retained task log could not be opened");
        }));
    const bool can_retry = item.retryable && !task_state_active(item.state);
    context.actions.push_back(retained_action("task.retry", can_retry,
        task_state_active(item.state)
            ? "An active task cannot be retried until its owner reports a terminal state."
            : "This task owner did not register a retry operation.", [id] {
            return task_center::retry(id) ? action_handler_result_t::completed()
                             : action_handler_result_t::failed("The task owner rejected the retry request");
        }));
    const bool can_cancel = item.cancellable && task_state_active(item.state) &&
        !item.security_critical;
    const char* cancel_reason = item.state == task_state_t::cancellation_requested
        ? "Waiting for the task owner to confirm a terminal cancelled state."
        : item.security_critical
        ? "Security-critical and liveness jobs are fail-closed and cannot be cancelled here."
        : task_state_active(item.state)
        ? "This task owner did not register a safe cancellation operation."
        : "Only an active task can be cancelled.";
    context.actions.push_back(retained_action("task.request_cancel", can_cancel,
        cancel_reason, [id] {
            return task_center::request_cancel(id) ? action_handler_result_t::completed()
                : action_handler_result_t::failed("The task owner rejected cancellation");
        }));
    context.actions.push_back(retained_action("task.view_diagnostic",
        !item.diagnostic_id.empty(), "This task has no retained diagnostic.", [] {
            auto* host = AidaTaskCenterController::exists()
                ? AidaTaskCenterController::instance().host() : nullptr;
            if (!host)
                return action_handler_result_t::failed("The view host is unavailable");
            const auto opened = host->open_or_focus(
                registry::stable_view_id_t("view.diagnostics"));
            return opened.ok() ? action_handler_result_t::completed()
                : action_handler_result_t::failed(opened.detail);
        }));
    context.actions.push_back(retained_action("task.copy_id", true, "", [id] {
        clipboard::set_text(QString::fromStdString(id));
        return action_handler_result_t::completed();
    }));
    const std::string summary = item.id + "\n" + item.label + "\n" + item.stage +
        "\n" + item.result_summary;
    context.actions.push_back(retained_action("task.copy_summary", true, "", [summary] {
        clipboard::set_text(QString::fromStdString(summary));
        return action_handler_result_t::completed();
    }));
    documents::show_retained_entity_menu(context, origin, global_pos, parent);
}

void open_diagnostic_context(const diagnostic_snapshot_t& item,
                             std::uint64_t snapshot_generation,
                             aida::ui::context_menu_open_origin_t origin,
                             const QPoint& global_pos, QWidget* parent) {
    namespace task_center = aida::ui::task_center;
    using aida::ui::action_handler_result_t;
    aida::ui::application_ui::retained_entity_context_t context;
    context.owner_id = "task-center.diagnostics";
    context.entity_id = item.id;
    context.entity_generation = snapshot_generation;
    context.active_view = aida::ui::stable_view_id_t("view.diagnostics");
    const std::string id = item.id;
    const std::uint64_t raised_ms = item.raised_ms;
    const std::string owner = item.owner;
    const std::string summary = item.summary;
    context.validate_identity = [id, raised_ms, owner, summary] {
        const auto current = task_center::snapshot();
        if (!current)
            return aida::ui::capability_state_t::unavailable("The diagnostic snapshot is unavailable");
        const auto* retained = find_diagnostic(*current, id);
        return retained && retained->raised_ms == raised_ms &&
                retained->owner == owner && retained->summary == summary
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The retained diagnostic was removed or replaced; select it again");
    };
    context.actions.push_back(retained_action("diagnostic.focus_owner", item.focusable,
        "This diagnostic has no registered owner focus target.", [id] {
            return task_center::focus_diagnostic(id) ? action_handler_result_t::completed()
                : action_handler_result_t::failed("The diagnostic owner could not be focused");
        }));
    context.actions.push_back(retained_action("diagnostic.open_log", item.log_available,
        "This diagnostic has no retained log target.", [id] {
            return task_center::open_diagnostic_log(id) ? action_handler_result_t::completed()
                : action_handler_result_t::failed("The diagnostic log could not be opened");
        }));
    context.actions.push_back(retained_action("diagnostic.retry", item.retryable,
        "The diagnostic owner did not register a safe retry operation.", [id] {
            return task_center::retry_diagnostic(id) ? action_handler_result_t::completed()
                : action_handler_result_t::failed("The diagnostic owner rejected the retry request");
        }));
    context.actions.push_back(retained_action("diagnostic.acknowledge", !item.acknowledged,
        "This diagnostic is already acknowledged.", [id] {
            return task_center::acknowledge_diagnostic(id) ? action_handler_result_t::completed()
                : action_handler_result_t::failed("The diagnostic could not be acknowledged");
        }));
    const std::string text = diagnostic_copy_text(item);
    context.actions.push_back(retained_action("diagnostic.copy", true, "", [text] {
        clipboard::set_text(QString::fromStdString(text));
        return action_handler_result_t::completed();
    }));
    context.actions.push_back(retained_action("diagnostic.copy_id", true, "", [id] {
        clipboard::set_text(QString::fromStdString(id));
        return action_handler_result_t::completed();
    }));
    aida::automation_ui::entity_evidence::snapshot_t evidence;
    evidence.workspace_id = item.target;
    evidence.source_view_id = "view.diagnostics";
    evidence.source_kind = "diagnostic";
    evidence.entity_id = item.id;
    evidence.display_label = item.summary;
    evidence.excerpt = "Diagnostic ID: " + item.id + "\nOwner: " + item.owner +
        "\nTarget: " + item.target + "\nTask ID: " + item.task_id +
        "\nRaised: " + std::to_string(item.raised_ms) +
        "\nSeverity: " + std::to_string(static_cast<unsigned>(item.severity)) +
        "\nSummary: " + item.summary + "\nDetails: " + item.details +
        "\nLog target: " + item.log_link;
    evidence.revision = item.raised_ms;
    evidence.generation = snapshot_generation;
    evidence.sensitive = true;
    evidence.return_to_source = [id, raised_ms, owner, summary](std::string& reason) {
        const auto current = task_center::snapshot();
        const auto* retained = current ? find_diagnostic(*current, id) : nullptr;
        if (!retained || retained->raised_ms != raised_ms ||
            retained->owner != owner || retained->summary != summary) {
            reason = "The retained diagnostic was removed or replaced; capture it again.";
            return false;
        }
        auto& controller = AidaTaskCenterController::instance();
        controller.stageDiagnosticSelection(id);
        auto* host = controller.host();
        if (!host) {
            reason = "The view host is unavailable";
            return false;
        }
        const auto opened = host->open_or_focus(registry::stable_view_id_t("view.diagnostics"));
        if (!opened.ok()) {
            reason = opened.detail;
            return false;
        }
        reason.clear();
        return true;
    };
    aida::automation_ui::entity_evidence::append_actions(context,
        std::move(evidence));
    documents::show_retained_entity_menu(context, origin, global_pos, parent);
}

} 

const char* task_state_name(task_state_t value) noexcept {
    switch (value) {
    case task_state_t::queued: return "Queued";
    case task_state_t::running: return "Running";
    case task_state_t::cancellation_requested: return "Cancel requested";
    case task_state_t::completed: return "Completed";
    case task_state_t::partial: return "Partial";
    case task_state_t::cancelled: return "Cancelled";
    case task_state_t::failed: return "Failed";
    case task_state_t::timed_out: return "Timed out";
    case task_state_t::interrupted: return "Interrupted";
    }
    return "Unknown";
}

const char* diagnostic_severity_name(diagnostic_severity_t value) noexcept {
    switch (value) {
    case diagnostic_severity_t::security: return "Security";
    case diagnostic_severity_t::error: return "Error";
    case diagnostic_severity_t::warning: return "Warning";
    case diagnostic_severity_t::information: return "Information";
    }
    return "Unknown";
}

QString task_duration_text(std::uint64_t milliseconds) {
    const std::uint64_t seconds = milliseconds / 1000ULL;
    if (seconds < 60ULL)
        return QString::fromStdString(std::to_string(seconds) + "s");
    return QStringLiteral("%1m %2s")
        .arg(seconds / 60ULL)
        .arg(seconds % 60ULL, 2, 10, QLatin1Char('0'));
}

bool task_state_active(task_state_t value) noexcept {
    return value == task_state_t::queued || value == task_state_t::running ||
        value == task_state_t::cancellation_requested;
}

widgets::AidaSemantic task_semantic(task_state_t value) noexcept {
    switch (value) {
    case task_state_t::completed: return widgets::AidaSemantic::Success;
    case task_state_t::partial:
    case task_state_t::timed_out:
    case task_state_t::interrupted: return widgets::AidaSemantic::Warning;
    case task_state_t::failed: return widgets::AidaSemantic::Error;
    case task_state_t::cancelled: return widgets::AidaSemantic::Warning;
    case task_state_t::running: return widgets::AidaSemantic::Success;
    case task_state_t::cancellation_requested: return widgets::AidaSemantic::Warning;
    case task_state_t::queued: return widgets::AidaSemantic::Info;
    }
    return widgets::AidaSemantic::Neutral;
}

widgets::AidaSemantic diagnostic_semantic(diagnostic_severity_t value) noexcept {
    switch (value) {
    case diagnostic_severity_t::security:
    case diagnostic_severity_t::error: return widgets::AidaSemantic::Error;
    case diagnostic_severity_t::warning: return widgets::AidaSemantic::Warning;
    case diagnostic_severity_t::information: return widgets::AidaSemantic::Info;
    }
    return widgets::AidaSemantic::Neutral;
}

AidaTaskCenterController& AidaTaskCenterController::instance() {
    if (!g_task_center_controller)
        g_task_center_controller = new AidaTaskCenterController();
    return *g_task_center_controller;
}

bool AidaTaskCenterController::exists() noexcept {
    return g_task_center_controller != nullptr;
}

AidaTaskCenterController::AidaTaskCenterController(QObject* parent) : QObject(parent) {
    snapshot_ = aida::ui::task_center::snapshot();
    timer_ = new QTimer(this);
    timer_->setInterval(100);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &AidaTaskCenterController::onTick);
    timer_->start();
}

void AidaTaskCenterController::install(docking::AidaDockHost* host) {
    host_ = host;
}

void AidaTaskCenterController::onTick() {
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "task_center";
    submission.label = "task_center.refresh";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 1;
    submission.body = [controller = QPointer<AidaTaskCenterController>(this)] {
        const auto before = aida::ui::task_center::snapshot();
        const std::uint64_t before_generation = before ? before->generation : 0;
        aida::ui::task_center::refresh();
        const auto after = aida::ui::task_center::snapshot();
        if (!controller || (after ? after->generation : 0) == before_generation)
            return;
        QMetaObject::invokeMethod(controller, [] {
            auto& self = AidaTaskCenterController::instance();
            const auto current = aida::ui::task_center::snapshot();
            if (current && current->generation != self.generation_) {
                self.snapshot_ = current;
                self.generation_ = current->generation;
                Q_EMIT self.snapshotChanged(self.generation_);
            }
        }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
    const auto current = aida::ui::task_center::snapshot();
    if (current && current->generation != generation_) {
        snapshot_ = current;
        generation_ = current->generation;
        Q_EMIT snapshotChanged(generation_);
    }
}

void AidaTaskCenterController::stageDiagnosticSelection(const std::string& diagnostic_id) {
    staged_diagnostic_selection_ = diagnostic_id;
}

std::string AidaTaskCenterController::consumeDiagnosticSelection() {
    return std::exchange(staged_diagnostic_selection_, std::string{});
}

AidaTaskTableModel::AidaTaskTableModel(QObject* parent) : QAbstractTableModel(parent) {
    snapshot_ = aida::ui::task_center::snapshot();
    generation_ = snapshot_ ? snapshot_->generation : 0;
}

void AidaTaskTableModel::setSnapshot(aida::ui::task_center::immutable_snapshot_ptr snapshot) {
    const std::uint64_t next_generation = snapshot ? snapshot->generation : 0;
    if (next_generation == generation_)
        return;
    beginResetModel();
    snapshot_ = std::move(snapshot);
    generation_ = next_generation;
    endResetModel();
}

int AidaTaskTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || !snapshot_)
        return 0;
    return static_cast<int>(snapshot_->tasks.size());
}

int AidaTaskTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(Column::count);
}

const task_snapshot_t* AidaTaskTableModel::rowAt(int row) const noexcept {
    if (!snapshot_ || row < 0 || row >= static_cast<int>(snapshot_->tasks.size()))
        return nullptr;
    return &snapshot_->tasks[static_cast<std::size_t>(row)];
}

int AidaTaskTableModel::rowForTaskId(const std::string& id) const noexcept {
    if (!snapshot_)
        return -1;
    for (int row = 0; row < static_cast<int>(snapshot_->tasks.size()); ++row)
        if (snapshot_->tasks[static_cast<std::size_t>(row)].id == id)
            return row;
    return -1;
}

QVariant AidaTaskTableModel::data(const QModelIndex& index, int role) const {
    const auto* task = rowAt(index.row());
    if (!task)
        return {};
    const auto column = static_cast<Column>(index.column());
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Column::task: return QString::fromStdString(task->label);
        case Column::owner: return QString::fromStdString(task->owner);
        case Column::target:
            return task->target.empty() ? QStringLiteral("-")
                                        : QString::fromStdString(task->target);
        case Column::state: return QString::fromLatin1(task_state_name(task->state));
        case Column::progress: break;
        case Column::elapsed: return task_duration_text(task->elapsed_ms);
        case Column::action: break;
        case Column::count: break;
        }
        return {};
    }
    if (role == Qt::ToolTipRole) {
        if (column == Column::task) {
            QString tip = QString::fromStdString(task->label);
            if (!task->stage.empty()) {
                tip += QStringLiteral("\n");
                tip += QString::fromStdString(task->stage);
            }
            if (!task->result_summary.empty()) {
                tip += QStringLiteral("\n");
                tip += QString::fromStdString(task->result_summary);
            }
            return tip;
        }
        if (column == Column::owner && !task->owner.empty())
            return QString::fromStdString(task->owner);
        if (column == Column::target && !task->target.empty())
            return QString::fromStdString(task->target);
        if (column == Column::state)
            return QString::fromLatin1(task_state_name(task->state));
        return {};
    }
    if (role == Qt::UserRole)
        return QString::fromStdString(task->id);
    return {};
}

void AidaTaskTableModel::multiData(const QModelIndex& index,
                                   QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan) {
        roleData.setData(data(index, roleData.role()));
    }
}

QVariant AidaTaskTableModel::headerData(int section, Qt::Orientation orientation,
                                        int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (static_cast<Column>(section)) {
    case Column::task: return QStringLiteral("Task");
    case Column::owner: return QStringLiteral("Owner");
    case Column::target: return QStringLiteral("Target");
    case Column::state: return QStringLiteral("State");
    case Column::progress: return QStringLiteral("Progress");
    case Column::elapsed: return QStringLiteral("Elapsed");
    case Column::action: return QStringLiteral("Action");
    case Column::count: break;
    }
    return {};
}

AidaTaskRowDelegate::AidaTaskRowDelegate(AidaTaskTableModel* model, QObject* parent)
    : QStyledItemDelegate(parent), model_(model) {
}

QRect AidaTaskRowDelegate::actionRect(const QStyleOptionViewItem& option) const {
    const auto& tokens = theme::tokens();
    const int label_w = (std::max)({option.fontMetrics.horizontalAdvance(QStringLiteral("Cancel")),
        option.fontMetrics.horizontalAdvance(QStringLiteral("Retry")),
        option.fontMetrics.horizontalAdvance(QStringLiteral("Focus"))});
    const int width = label_w + tokens.table.cell_pad_x * 2 + tokens.spacing.sm * 2;
    const int height = (std::min)(option.rect.height() - tokens.radius.xs * 2,
        tokens.control.height_sm);
    return QRect(option.rect.right() - width - tokens.table.cell_pad_x,
        option.rect.center().y() - height / 2, width, height);
}

AidaTaskRowDelegate::cell_action_t AidaTaskRowDelegate::actionFor(
    const task_snapshot_t& task) const noexcept {
    if (task.state == task_state_t::cancellation_requested)
        return cell_action_t::none;
    if (task.cancellable && task_state_active(task.state))
        return cell_action_t::cancel;
    if (task.retryable && !task_state_active(task.state))
        return cell_action_t::retry;
    if (task.focusable)
        return cell_action_t::focus;
    return cell_action_t::none;
}

void AidaTaskRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const {
    const auto* task = model_->rowAt(index.row());
    if (!task) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    const auto column = static_cast<AidaTaskTableModel::Column>(index.column());
    if (column != AidaTaskTableModel::Column::state &&
        column != AidaTaskTableModel::Column::progress &&
        column != AidaTaskTableModel::Column::action) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    const auto& tokens = theme::tokens();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (option.state & QStyle::State_Selected)
        painter->fillRect(option.rect, widgets::with_alpha(tokens.selection, 1.0));
    if (column == AidaTaskTableModel::Column::state) {
        const auto semantic = task_semantic(task->state);
        const QColor color = widgets::semantic_color(semantic);
        const QString label = QString::fromLatin1(task_state_name(task->state));
        const qreal pill_h = static_cast<qreal>((std::max)(0,
            option.rect.height() - tokens.table.cell_pad_y * 2));
        const qreal max_w = static_cast<qreal>(option.rect.width() -
            tokens.table.cell_pad_x * 2);
        const qreal text_w = static_cast<qreal>(
            painter->fontMetrics().horizontalAdvance(label)) +
            static_cast<qreal>(tokens.table.cell_pad_x * 2 + tokens.spacing.xxs * 2);
        const QRectF pill = QRectF(option.rect.left() + tokens.table.cell_pad_x,
            option.rect.center().y() - pill_h * 0.5, (std::min)(max_w, text_w), pill_h);
        painter->setPen(Qt::NoPen);
        painter->setBrush(widgets::semantic_soft_color(semantic));
        painter->drawRoundedRect(pill, tokens.radius.pill, tokens.radius.pill);
        painter->setPen(color);
        painter->drawText(pill, Qt::AlignCenter, label);
    } else if (column == AidaTaskTableModel::Column::progress) {
        const qreal bar_h = static_cast<qreal>(tokens.spacing.xs);
        const QRectF bar(option.rect.left() + tokens.table.cell_pad_x,
            option.rect.center().y() - bar_h * 0.5,
            option.rect.width() - 2.0 * tokens.table.cell_pad_x, bar_h);
        if (task->progress >= 0.0f) {
            const qreal fraction = (std::max)(0.0, (std::min)(1.0,
                static_cast<qreal>(task->progress)));
            painter->setPen(Qt::NoPen);
            painter->setBrush(widgets::with_alpha(tokens.text_secondary, 0.25));
            painter->drawRoundedRect(bar, tokens.radius.xs, tokens.radius.xs);
            painter->setBrush(tokens.accent);
            painter->drawRoundedRect(QRectF(bar.left(), bar.top(),
                bar.width() * fraction, bar.height()), tokens.radius.xs, tokens.radius.xs);
            painter->setPen(tokens.text_secondary);
            painter->drawText(QRectF(option.rect.left(),
                bar.bottom() + tokens.spacing.xxs,
                option.rect.width(),
                option.rect.bottom() - bar.bottom() - tokens.spacing.xxs),
                Qt::AlignHCenter | Qt::AlignTop,
                QStringLiteral("%1%").arg(static_cast<int>(fraction * 100.0)));
        } else if (task_state_active(task->state)) {
            if (theme::AidaMotion::reducedMotion()) {
                painter->setPen(tokens.text_dim);
                painter->drawText(option.rect, Qt::AlignCenter, QStringLiteral("Working"));
            } else {
                painter->setPen(Qt::NoPen);
                painter->setBrush(widgets::with_alpha(tokens.text_secondary, 0.25));
                painter->drawRoundedRect(bar, tokens.radius.xs, tokens.radius.xs);
                const qreal period = static_cast<qreal>(tokens.motion.hero);
                const qreal phase = static_cast<qreal>(
                    QTime::currentTime().msecsSinceStartOfDay() %
                        static_cast<int>(period)) / period;
                const qreal sweep_w = bar.width() * 0.35;
                const qreal sweep_x = bar.left() + (bar.width() - sweep_w) * phase;
                painter->setBrush(widgets::with_alpha(tokens.accent, 0.8));
                painter->drawRoundedRect(QRectF(sweep_x, bar.top(), sweep_w, bar.height()),
                    tokens.radius.xs, tokens.radius.xs);
            }
        } else {
            painter->setPen(tokens.text_dim);
            painter->drawText(option.rect, Qt::AlignCenter, QStringLiteral("-"));
        }
    } else if (column == AidaTaskTableModel::Column::action) {
        const auto action = actionFor(*task);
        const QRect rect = actionRect(option);
        if (action == cell_action_t::none) {
            painter->setPen(tokens.text_dim);
            painter->drawText(option.rect, Qt::AlignCenter,
                task->state == task_state_t::cancellation_requested
                    ? QStringLiteral("Pending") : QStringLiteral("-"));
        } else {
            const QString label = action == cell_action_t::cancel ? QStringLiteral("Cancel")
                : action == cell_action_t::retry ? QStringLiteral("Retry")
                : QStringLiteral("Focus");
            const QColor color = action == cell_action_t::cancel
                ? tokens.error : action == cell_action_t::retry
                ? tokens.text_primary : tokens.accent;
            const qreal wash = (option.state & QStyle::State_Sunken) ? 0.40
                : (option.state & QStyle::State_MouseOver) ? 0.28 : 0.16;
            painter->setPen(Qt::NoPen);
            painter->setBrush(widgets::with_alpha(color, wash));
            painter->drawRoundedRect(QRectF(rect), tokens.radius.sm, tokens.radius.sm);
            painter->setPen(color);
            painter->drawText(rect, Qt::AlignCenter, label);
        }
    }
    painter->restore();
}

bool AidaTaskRowDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                      const QStyleOptionViewItem& option,
                                      const QModelIndex& index) {
    static_cast<void>(model);
    if (event->type() != QEvent::MouseButtonRelease)
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::LeftButton)
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    const auto* task = model_->rowAt(index.row());
    if (!task || index.column() != static_cast<int>(AidaTaskTableModel::Column::action))
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    if (!actionRect(option).contains(mouse->pos()))
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    const QString id = QString::fromStdString(task->id);
    switch (actionFor(*task)) {
    case cell_action_t::cancel: Q_EMIT cancelRequested(id); break;
    case cell_action_t::retry: Q_EMIT retryRequested(id); break;
    case cell_action_t::focus: Q_EMIT focusRequested(id); break;
    case cell_action_t::none: break;
    }
    return true;
}

QSize AidaTaskRowDelegate::sizeHint(const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const {
    static_cast<void>(option);
    static_cast<void>(index);
    return QSize(theme::tokens().spacing.lg, theme::tokens().table.row_h);
}

AidaTaskCenterView::AidaTaskCenterView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.background_tasks"));
    const auto& tokens = theme::tokens();
    auto& controller = AidaTaskCenterController::instance();
    stack_ = new QStackedLayout(this);
    stack_->setContentsMargins(0, 0, 0, 0);

    content_ = new QWidget(this);
    auto* column = new QVBoxLayout(content_);
    column->setContentsMargins(tokens.spacing.sm, tokens.spacing.xs,
        tokens.spacing.sm, tokens.spacing.xs);
    column->setSpacing(tokens.spacing.xs);

    auto* badges = new QWidget(content_);
    badges->setObjectName(QStringLiteral("aida.view.background_tasks.badges"));
    auto* badge_row = new QHBoxLayout(badges);
    badge_row->setContentsMargins(0, 0, 0, 0);
    badge_row->setSpacing(tokens.spacing.sm);
    running_pill_ = new widgets::AidaPill(QStringLiteral("0 running"),
        widgets::AidaSemantic::Neutral, badges);
    queued_pill_ = new widgets::AidaPill(QStringLiteral("0 queued"),
        widgets::AidaSemantic::Neutral, badges);
    cancelling_pill_ = new widgets::AidaPill(QStringLiteral("0 cancelling"),
        widgets::AidaSemantic::Warning, badges);
    failed_pill_ = new widgets::AidaPill(QStringLiteral("0 failed"),
        widgets::AidaSemantic::Error, badges);
    interrupted_pill_ = new widgets::AidaPill(QStringLiteral("0 interrupted"),
        widgets::AidaSemantic::Warning, badges);
    partial_pill_ = new widgets::AidaPill(QStringLiteral("0 partial"),
        widgets::AidaSemantic::Warning, badges);
    for (auto* pill : {running_pill_, queued_pill_, cancelling_pill_, failed_pill_,
                       interrupted_pill_, partial_pill_})
        badge_row->addWidget(pill);
    cancelling_pill_->setVisible(false);
    failed_pill_->setVisible(false);
    interrupted_pill_->setVisible(false);
    partial_pill_->setVisible(false);
    badge_row->addStretch(1);
    column->addWidget(badges);

    model_ = new AidaTaskTableModel(this);
    table_ = new QTableView(content_);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.view.background_tasks.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(theme::tokens().table.row_h);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->installEventFilter(this);
    table_->viewport()->installEventFilter(this);
    delegate_ = new AidaTaskRowDelegate(model_, table_);
    table_->setItemDelegate(delegate_);
    auto* header = table_->horizontalHeader();
    const int cell_pad = theme::tokens().table.cell_pad_x;
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    header->setSectionResizeMode(2, QHeaderView::Stretch);
    header->setSectionResizeMode(3, QHeaderView::Fixed);
    header->resizeSection(3, header->fontMetrics().horizontalAdvance(
        QStringLiteral("Cancel requested")) + cell_pad * 2);
    header->setSectionResizeMode(4, QHeaderView::Fixed);
    header->resizeSection(4, header->fontMetrics().averageCharWidth() * 20 + cell_pad * 2);
    header->setSectionResizeMode(5, QHeaderView::Fixed);
    header->resizeSection(5, header->fontMetrics().horizontalAdvance(
        QStringLiteral("000m 00s")) + cell_pad * 2);
    header->setSectionResizeMode(6, QHeaderView::Fixed);
    header->resizeSection(6, header->fontMetrics().horizontalAdvance(
        QStringLiteral("Cancel")) + cell_pad * 2 + theme::tokens().spacing.sm * 2 +
        cell_pad * 2);
    column->addWidget(table_, 1);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.view.background_tasks.state"));
    state_view_->setState(widgets::AidaStateView::State::Empty);
    state_view_->setTitle(QStringLiteral("No background tasks"));
    state_view_->setMessage(QStringLiteral(
        "Long-running work appears here with owner, target, progress, cancellation, recovery, and diagnostics."));
    stack_->addWidget(content_);
    stack_->addWidget(state_view_);

    connect(&controller, &AidaTaskCenterController::snapshotChanged,
            this, &AidaTaskCenterView::onSnapshotChanged);
    connect(table_, &QTableView::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = table_->indexAt(pos);
        if (index.isValid())
            table_->setCurrentIndex(index);
        openTaskContext(table_->currentIndex(),
            aida::ui::context_menu_open_origin_t::pointer,
            table_->viewport()->mapToGlobal(pos));
    });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto* task = model_->rowAt(index.row());
        if (task && task->focusable)
            static_cast<void>(aida::ui::task_center::focus(task->id));
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        const auto* task = model_->rowAt(current.row());
        selected_task_id_ = task ? QString::fromStdString(task->id) : QString();
    });
    connect(delegate_, &AidaTaskRowDelegate::cancelRequested, this, [](const QString& id) {
        static_cast<void>(aida::ui::task_center::request_cancel(id.toStdString()));
    });
    connect(delegate_, &AidaTaskRowDelegate::retryRequested, this, [](const QString& id) {
        static_cast<void>(aida::ui::task_center::retry(id.toStdString()));
    });
    connect(delegate_, &AidaTaskRowDelegate::focusRequested, this, [](const QString& id) {
        static_cast<void>(aida::ui::task_center::focus(id.toStdString()));
    });
    model_->setSnapshot(controller.current());
    refreshBadges();
    updateStatePage();
}

void AidaTaskCenterView::onSnapshotChanged(quint64 generation) {
    const auto current = AidaTaskCenterController::instance().current();
    if (!current || current->generation != generation)
        return;
    model_->setSnapshot(current);
    refreshBadges();
    reapplySelection();
    updateStatePage();
}

void AidaTaskCenterView::refreshBadges() {
    const auto current = model_->snapshotGeneration()
        ? AidaTaskCenterController::instance().current() : nullptr;
    if (!current)
        return;
    const auto& status = current->status;
    running_pill_->setText(QStringLiteral("%1 running").arg(status.running));
    running_pill_->setKind(status.running ? widgets::AidaSemantic::Success
                                          : widgets::AidaSemantic::Neutral);
    queued_pill_->setText(QStringLiteral("%1 queued").arg(status.queued));
    queued_pill_->setKind(status.queued ? widgets::AidaSemantic::Info
                                        : widgets::AidaSemantic::Neutral);
    cancelling_pill_->setText(QStringLiteral("%1 cancelling").arg(status.cancellation_requested));
    cancelling_pill_->setVisible(status.cancellation_requested != 0);
    failed_pill_->setText(QStringLiteral("%1 failed").arg(status.failures));
    failed_pill_->setVisible(status.failures != 0);
    interrupted_pill_->setText(QStringLiteral("%1 interrupted").arg(status.interrupted));
    interrupted_pill_->setVisible(status.interrupted != 0);
    partial_pill_->setText(QStringLiteral("%1 partial").arg(status.partial));
    partial_pill_->setVisible(status.partial != 0);
}

void AidaTaskCenterView::reapplySelection() {
    if (selected_task_id_.isEmpty())
        return;
    const int row = model_->rowForTaskId(selected_task_id_.toStdString());
    if (row >= 0)
        table_->selectRow(row);
}

void AidaTaskCenterView::updateStatePage() {
    const bool empty = model_->rowCount() == 0;
    stack_->setCurrentWidget(empty ? static_cast<QWidget*>(state_view_)
                                   : static_cast<QWidget*>(content_));
}

void AidaTaskCenterView::openTaskContext(const QModelIndex& index,
                                         aida::ui::context_menu_open_origin_t origin,
                                         const QPoint& global_pos) {
    const auto* task = model_->rowAt(index.row());
    if (!task)
        return;
    selected_task_id_ = QString::fromStdString(task->id);
    open_task_context(*task, model_->snapshotGeneration(), origin,
        global_pos, this);
}

bool AidaTaskCenterView::eventFilter(QObject* watched, QEvent* event) {
    QModelIndex index;
    QPoint global_pos;
    if (keyboard_context_menu_event(watched, event, table_, &index, &global_pos)) {
        openTaskContext(index, aida::ui::context_menu_open_origin_t::menu_key, global_pos);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

AidaDiagnosticsTableModel::AidaDiagnosticsTableModel(QObject* parent)
    : QAbstractTableModel(parent) {
    snapshot_ = aida::ui::task_center::snapshot();
    generation_ = snapshot_ ? snapshot_->generation : 0;
}

void AidaDiagnosticsTableModel::setSnapshot(
    aida::ui::task_center::immutable_snapshot_ptr snapshot) {
    const std::uint64_t next_generation = snapshot ? snapshot->generation : 0;
    if (next_generation == generation_)
        return;
    beginResetModel();
    snapshot_ = std::move(snapshot);
    generation_ = next_generation;
    endResetModel();
}

int AidaDiagnosticsTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || !snapshot_)
        return 0;
    return static_cast<int>(snapshot_->diagnostics.size());
}

int AidaDiagnosticsTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(Column::count);
}

const diagnostic_snapshot_t* AidaDiagnosticsTableModel::rowAt(int row) const noexcept {
    if (!snapshot_ || row < 0 || row >= static_cast<int>(snapshot_->diagnostics.size()))
        return nullptr;
    return &snapshot_->diagnostics[static_cast<std::size_t>(row)];
}

int AidaDiagnosticsTableModel::rowForDiagnosticId(const std::string& id) const noexcept {
    if (!snapshot_)
        return -1;
    for (int row = 0; row < static_cast<int>(snapshot_->diagnostics.size()); ++row)
        if (snapshot_->diagnostics[static_cast<std::size_t>(row)].id == id)
            return row;
    return -1;
}

QVariant AidaDiagnosticsTableModel::data(const QModelIndex& index, int role) const {
    const auto* diagnostic = rowAt(index.row());
    if (!diagnostic || !snapshot_)
        return {};
    const auto column = static_cast<Column>(index.column());
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Column::severity:
            return QString::fromLatin1(diagnostic_severity_name(diagnostic->severity));
        case Column::summary: return QString::fromStdString(diagnostic->summary);
        case Column::owner:
            return diagnostic->owner.empty() ? QStringLiteral("-")
                                             : QString::fromStdString(diagnostic->owner);
        case Column::target:
            return diagnostic->target.empty() ? QStringLiteral("-")
                                              : QString::fromStdString(diagnostic->target);
        case Column::age: {
            const std::uint64_t age = snapshot_->captured_ms > diagnostic->raised_ms
                ? snapshot_->captured_ms - diagnostic->raised_ms : 0;
            return task_duration_text(age);
        }
        case Column::status:
            return diagnostic->acknowledged ? QStringLiteral("Acknowledged")
                                            : QStringLiteral("Attention");
        case Column::count: break;
        }
        return {};
    }
    if (role == Qt::ToolTipRole) {
        if (column == Column::summary && !diagnostic->details.empty())
            return QString::fromStdString(diagnostic->details);
        if (column == Column::owner && !diagnostic->owner.empty())
            return QString::fromStdString(diagnostic->owner);
        if (column == Column::target && !diagnostic->target.empty())
            return QString::fromStdString(diagnostic->target);
        return {};
    }
    if (role == Qt::ForegroundRole) {
        if (column == Column::severity)
            return widgets::semantic_color(diagnostic_semantic(diagnostic->severity));
        if (column == Column::status)
            return diagnostic->acknowledged ? theme::tokens().text_dim
                                            : theme::tokens().warning;
    }
    if (role == Qt::UserRole)
        return QString::fromStdString(diagnostic->id);
    return {};
}

void AidaDiagnosticsTableModel::multiData(const QModelIndex& index,
                                          QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan) {
        roleData.setData(data(index, roleData.role()));
    }
}

QVariant AidaDiagnosticsTableModel::headerData(int section, Qt::Orientation orientation,
                                               int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (static_cast<Column>(section)) {
    case Column::severity: return QStringLiteral("Severity");
    case Column::summary: return QStringLiteral("Summary");
    case Column::owner: return QStringLiteral("Owner");
    case Column::target: return QStringLiteral("Target");
    case Column::age: return QStringLiteral("Age");
    case Column::status: return QStringLiteral("Status");
    case Column::count: break;
    }
    return {};
}

AidaDiagnosticsView::AidaDiagnosticsView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.diagnostics"));
    const auto& tokens = theme::tokens();
    auto& controller = AidaTaskCenterController::instance();
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(tokens.spacing.sm, tokens.spacing.xs,
        tokens.spacing.sm, tokens.spacing.xs);
    column->setSpacing(tokens.spacing.xs);

    badge_ = new widgets::AidaPill(QStringLiteral("0 unacknowledged"),
        widgets::AidaSemantic::Success, this);
    badge_->setObjectName(QStringLiteral("aida.view.diagnostics.badge"));
    auto* badge_row = new QHBoxLayout();
    badge_row->addWidget(badge_);
    badge_row->addStretch(1);
    column->addLayout(badge_row);

    content_ = new QWidget(this);
    auto* content_layout = new QVBoxLayout(content_);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);

    splitter_ = new QSplitter(Qt::Vertical, content_);
    splitter_->setObjectName(QStringLiteral("aida.view.diagnostics.splitter"));
    splitter_->setOpaqueResize(true);
    splitter_->setChildrenCollapsible(false);

    model_ = new AidaDiagnosticsTableModel(this);
    table_ = new QTableView(splitter_);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.view.diagnostics.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(theme::tokens().table.row_h);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->installEventFilter(this);
    table_->viewport()->installEventFilter(this);
    auto* header = table_->horizontalHeader();
    const int cell_pad = theme::tokens().table.cell_pad_x;
    header->setSectionResizeMode(0, QHeaderView::Fixed);
    header->resizeSection(0, header->fontMetrics().horizontalAdvance(
        QStringLiteral("Information")) + cell_pad * 2);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    header->setStretchLastSection(false);
    header->setSectionResizeMode(2, QHeaderView::Stretch);
    header->setSectionResizeMode(3, QHeaderView::Stretch);
    header->setSectionResizeMode(4, QHeaderView::Fixed);
    header->resizeSection(4, header->fontMetrics().horizontalAdvance(
        QStringLiteral("000m 00s")) + cell_pad * 2);
    header->setSectionResizeMode(5, QHeaderView::Fixed);
    header->resizeSection(5, header->fontMetrics().horizontalAdvance(
        QStringLiteral("Acknowledged")) + cell_pad * 2);
    splitter_->addWidget(table_);

    auto* details_pane = new QWidget(splitter_);
    details_pane->setObjectName(QStringLiteral("aida.view.diagnostics.details"));
    auto* details_layout = new QVBoxLayout(details_pane);
    details_layout->setContentsMargins(tokens.spacing.sm, tokens.spacing.xs,
        tokens.spacing.sm, tokens.spacing.xs);
    details_layout->setSpacing(tokens.spacing.xs);
    summary_ = new QLabel(details_pane);
    summary_->setObjectName(QStringLiteral("aida.view.diagnostics.details.summary"));
    summary_->setWordWrap(true);
    details_layout->addWidget(summary_);
    details_ = new QLabel(details_pane);
    details_->setObjectName(QStringLiteral("aida.view.diagnostics.details.text"));
    details_->setWordWrap(true);
    details_layout->addWidget(details_);
    id_label_ = new QLabel(details_pane);
    id_label_->setObjectName(QStringLiteral("aida.view.diagnostics.details.id"));
    id_label_->setEnabled(false);
    details_layout->addWidget(id_label_);
    auto* buttons = new QHBoxLayout();
    focus_button_ = new QPushButton(QStringLiteral("Focus Owner"), details_pane);
    focus_button_->setObjectName(QStringLiteral("aida.view.diagnostics.details.focus"));
    focus_button_->setToolTip(QStringLiteral("Focus the view that owns this diagnostic"));
    log_button_ = new QPushButton(QStringLiteral("Open Log"), details_pane);
    log_button_->setObjectName(QStringLiteral("aida.view.diagnostics.details.log"));
    log_button_->setToolTip(QStringLiteral("Open the retained log for this diagnostic"));
    retry_button_ = new QPushButton(QStringLiteral("Retry"), details_pane);
    retry_button_->setObjectName(QStringLiteral("aida.view.diagnostics.details.retry"));
    retry_button_->setToolTip(QStringLiteral("Retry the failed operation"));
    acknowledge_button_ = new QPushButton(QStringLiteral("Acknowledge"), details_pane);
    acknowledge_button_->setObjectName(
        QStringLiteral("aida.view.diagnostics.details.acknowledge"));
    acknowledge_button_->setToolTip(QStringLiteral(
        "Mark this diagnostic as acknowledged"));
    buttons->addWidget(focus_button_);
    buttons->addWidget(log_button_);
    buttons->addWidget(retry_button_);
    buttons->addWidget(acknowledge_button_);
    buttons->addStretch(1);
    details_layout->addLayout(buttons);
    splitter_->addWidget(details_pane);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 0);
    content_layout->addWidget(splitter_, 1);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.view.diagnostics.state"));
    state_view_->setState(widgets::AidaStateView::State::Empty);
    state_view_->setTitle(QStringLiteral("No diagnostics"));
    state_view_->setMessage(QStringLiteral(
        "Nothing requires attention. Task failures and runtime warnings appear here with owner, target, and recovery actions."));

    stack_ = new QStackedLayout();
    stack_->setContentsMargins(0, 0, 0, 0);
    stack_->addWidget(content_);
    stack_->addWidget(state_view_);
    column->addLayout(stack_, 1);

    connect(&controller, &AidaTaskCenterController::snapshotChanged,
            this, &AidaDiagnosticsView::onSnapshotChanged);
    connect(table_, &QTableView::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = table_->indexAt(pos);
        if (index.isValid())
            table_->setCurrentIndex(index);
        openDiagnosticContext(table_->currentIndex(),
            aida::ui::context_menu_open_origin_t::pointer,
            table_->viewport()->mapToGlobal(pos));
    });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto* diagnostic = model_->rowAt(index.row());
        if (diagnostic && diagnostic->focusable)
            static_cast<void>(aida::ui::task_center::focus_diagnostic(diagnostic->id));
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        const auto* diagnostic = model_->rowAt(current.row());
        selected_diagnostic_id_ = diagnostic
            ? QString::fromStdString(diagnostic->id) : QString();
        refreshDetails();
    });
    connect(focus_button_, &QPushButton::clicked, this, [this] {
        const auto* diagnostic = model_->rowAt(
            model_->rowForDiagnosticId(selected_diagnostic_id_.toStdString()));
        if (diagnostic)
            static_cast<void>(aida::ui::task_center::focus_diagnostic(diagnostic->id));
    });
    connect(log_button_, &QPushButton::clicked, this, [this] {
        const auto* diagnostic = model_->rowAt(
            model_->rowForDiagnosticId(selected_diagnostic_id_.toStdString()));
        if (diagnostic)
            static_cast<void>(aida::ui::task_center::open_diagnostic_log(diagnostic->id));
    });
    connect(retry_button_, &QPushButton::clicked, this, [this] {
        const auto* diagnostic = model_->rowAt(
            model_->rowForDiagnosticId(selected_diagnostic_id_.toStdString()));
        if (diagnostic)
            static_cast<void>(aida::ui::task_center::retry_diagnostic(diagnostic->id));
    });
    connect(acknowledge_button_, &QPushButton::clicked, this, [this] {
        const auto* diagnostic = model_->rowAt(
            model_->rowForDiagnosticId(selected_diagnostic_id_.toStdString()));
        if (diagnostic)
            static_cast<void>(aida::ui::task_center::acknowledge_diagnostic(diagnostic->id));
    });
    {
        const auto current = controller.current();
        model_->setSnapshot(current);
        badge_->setText(QStringLiteral("%1 unacknowledged")
            .arg(current ? current->status.unacknowledged_diagnostics : 0));
        badge_->setKind(current && current->status.unacknowledged_diagnostics
            ? widgets::AidaSemantic::Warning : widgets::AidaSemantic::Success);
        reapplySelection();
        refreshDetails();
        updateStatePage();
    }
}

bool AidaDiagnosticsView::eventFilter(QObject* watched, QEvent* event) {
    QModelIndex index;
    QPoint global_pos;
    if (keyboard_context_menu_event(watched, event, table_, &index, &global_pos)) {
        openDiagnosticContext(index, aida::ui::context_menu_open_origin_t::menu_key,
            global_pos);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void AidaDiagnosticsView::updateStatePage() {
    if (!stack_)
        return;
    const bool empty = model_->rowCount() == 0;
    stack_->setCurrentWidget(empty ? static_cast<QWidget*>(state_view_)
                                   : static_cast<QWidget*>(content_));
}

void AidaDiagnosticsView::onSnapshotChanged(quint64 generation) {
    auto& controller = AidaTaskCenterController::instance();
    const auto current = controller.current();
    if (!current || current->generation != generation)
        return;
    model_->setSnapshot(current);
    badge_->setText(QStringLiteral("%1 unacknowledged")
        .arg(current->status.unacknowledged_diagnostics));
    badge_->setKind(current->status.unacknowledged_diagnostics
        ? widgets::AidaSemantic::Warning : widgets::AidaSemantic::Success);
    const auto staged = controller.consumeDiagnosticSelection();
    if (!staged.empty()) {
        const int row = model_->rowForDiagnosticId(staged);
        if (row >= 0) {
            selected_diagnostic_id_ = QString::fromStdString(staged);
            table_->selectRow(row);
        }
    } else {
        reapplySelection();
    }
    refreshDetails();
    updateStatePage();
}

void AidaDiagnosticsView::reapplySelection() {
    const int count = model_->rowCount();
    if (count == 0) {
        selected_diagnostic_id_.clear();
        return;
    }
    int row = model_->rowForDiagnosticId(selected_diagnostic_id_.toStdString());
    if (row < 0) {
        row = 0;
        const auto* first = model_->rowAt(0);
        selected_diagnostic_id_ = first ? QString::fromStdString(first->id) : QString();
    }
    table_->selectRow(row);
}

void AidaDiagnosticsView::refreshDetails() {
    const auto* diagnostic = model_->rowAt(
        model_->rowForDiagnosticId(selected_diagnostic_id_.toStdString()));
    summary_->setText(diagnostic ? QString::fromStdString(diagnostic->summary) : QString());
    const QString variant = QString::fromLatin1(diagnostic
        ? widgets::semantic_variant_name(diagnostic_semantic(diagnostic->severity))
        : "neutral");
    if (variant != summary_variant_) {
        summary_variant_ = variant;
        summary_->setProperty("aidaVariant", variant);
        theme::stylesheet::repolish(summary_);
    }
    details_->setText(diagnostic && !diagnostic->details.empty()
        ? QString::fromStdString(diagnostic->details) : QString());
    id_label_->setText(diagnostic ? QString::fromStdString(diagnostic->id) : QString());
    focus_button_->setVisible(diagnostic && diagnostic->focusable);
    log_button_->setVisible(diagnostic && diagnostic->log_available);
    retry_button_->setVisible(diagnostic && diagnostic->retryable);
    acknowledge_button_->setVisible(diagnostic && !diagnostic->acknowledged);
}

void AidaDiagnosticsView::openDiagnosticContext(const QModelIndex& index,
        aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos) {
    const auto* diagnostic = model_->rowAt(index.row());
    if (!diagnostic)
        return;
    selected_diagnostic_id_ = QString::fromStdString(diagnostic->id);
    open_diagnostic_context(*diagnostic, model_->snapshotGeneration(),
        origin, global_pos, this);
}

}
