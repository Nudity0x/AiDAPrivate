#include "qt/programming/aida_programming_tasks.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QSplitter>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/programming/aida_output_pane.hpp"
#include "qt/programming/aida_task_center_view.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::programming {
namespace {

namespace tasks = aida::ui::programming_tasks;
namespace task_center = aida::ui::task_center;
using aida::ui::action_handler_result_t;

QPointer<AidaProgrammingTasksController> g_tasks_controller;

bool contains_case_insensitive(std::string_view value, std::string_view query) {
    if (query.empty()) return true;
    return std::search(value.begin(), value.end(), query.begin(), query.end(),
        [](unsigned char left, unsigned char right) {
            return std::tolower(left) == std::tolower(right);
        }) != value.end();
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

struct script_run_identity_t {
    std::string id;
    std::string source;
    std::string owner;
    std::string label;
    std::uint64_t queued_ms = 0;
    std::uint64_t snapshot_generation = 0;
};

script_run_identity_t script_run_identity(const task_center::task_snapshot_t& task,
                                          std::uint64_t snapshot_generation) {
    return {task.id, task.source, task.owner, task.label, task.queued_ms,
        snapshot_generation};
}

bool same_script_run(const task_center::task_snapshot_t& task,
                     const script_run_identity_t& identity) {
    return task.id == identity.id && task.source == identity.source &&
        task.owner == identity.owner && task.label == identity.label &&
        task.queued_ms == identity.queued_ms;
}

const task_center::task_snapshot_t* find_script_run(
    const task_center::immutable_snapshot_t& snapshot,
    const script_run_identity_t& identity) {
    const auto found = std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(),
        [&](const task_center::task_snapshot_t& task) {
            return same_script_run(task, identity);
        });
    return found == snapshot.tasks.end() ? nullptr : &*found;
}

enum class script_run_action_t : std::uint8_t { cancel, retry, focus, open_log };

std::string script_run_action_unavailable_reason(
    const task_center::task_snapshot_t* task, script_run_action_t action) {
    if (!task)
        return "The retained run was removed or replaced; select the run again";
    switch (action) {
    case script_run_action_t::cancel:
        if (task->security_critical)
            return "Security-critical tasks cannot be cancelled";
        if (task->state == task_center::task_state_t::cancellation_requested)
            return "Cancellation is already pending owner confirmation";
        if (!(task->state == task_center::task_state_t::queued ||
              task->state == task_center::task_state_t::running ||
              task->state == task_center::task_state_t::cancellation_requested))
            return "Only an active run can be cancelled";
        if (!task->cancellable)
            return "This run owner did not register safe cancellation";
        return {};
    case script_run_action_t::retry:
        if (task->state == task_center::task_state_t::queued ||
            task->state == task_center::task_state_t::running ||
            task->state == task_center::task_state_t::cancellation_requested)
            return "An active run cannot be retried";
        if (!task->retryable)
            return "This run owner did not register retry";
        return {};
    case script_run_action_t::focus:
        return task->focusable ? std::string{} :
            "This run owner did not register an output focus target";
    case script_run_action_t::open_log:
        return task->log_available ? std::string{} :
            "This run has no retained log target";
    }
    return "The run action is unavailable";
}

action_handler_result_t invoke_script_run_action(const script_run_identity_t& identity,
                                                 script_run_action_t action) {
    const auto current = task_center::snapshot();
    const auto* task = current && current->generation == identity.snapshot_generation
        ? find_script_run(*current, identity) : nullptr;
    const std::string unavailable = current
        ? current->generation == identity.snapshot_generation
            ? script_run_action_unavailable_reason(task, action)
            : "The immutable Task Center snapshot changed; reopen the run context"
        : "Task Center state is unavailable";
    if (!unavailable.empty()) {
        AidaProgrammingTasksController::instance().setScriptActionError(unavailable);
        return action_handler_result_t::failed(unavailable);
    }
    bool accepted = false;
    switch (action) {
    case script_run_action_t::cancel:
        accepted = task_center::request_cancel(identity.id);
        break;
    case script_run_action_t::retry:
        accepted = task_center::retry(identity.id);
        break;
    case script_run_action_t::focus:
        accepted = task_center::focus(identity.id);
        break;
    case script_run_action_t::open_log:
        accepted = task_center::open_log(identity.id);
        break;
    }
    if (accepted) {
        AidaProgrammingTasksController::instance().clearScriptActionError();
        return action_handler_result_t::completed();
    }
    const std::string failure =
        "Task Center rejected the action because the retained run state changed";
    AidaProgrammingTasksController::instance().setScriptActionError(failure);
    return action_handler_result_t::failed(failure);
}

aida::ui::application_ui::retained_entity_action_t retained_script_action(const char* id,
    std::string unavailable, std::function<action_handler_result_t()> invoke) {
    aida::ui::application_ui::retained_entity_action_t action;
    action.action_id = id;
    action.capability = unavailable.empty() ? aida::ui::capability_state_t::available()
        : aida::ui::capability_state_t::unavailable(std::move(unavailable));
    action.invoke = std::move(invoke);
    return action;
}

void open_script_run_context(const task_center::task_snapshot_t& task,
                             std::uint64_t snapshot_generation,
                             aida::ui::context_menu_open_origin_t origin,
                             const QPoint& global_pos, QWidget* parent) {
    const script_run_identity_t identity = script_run_identity(task, snapshot_generation);
    aida::ui::application_ui::retained_entity_context_t context;
    context.owner_id = "programming.scripts.run";
    context.entity_id = task.id;
    context.entity_generation = snapshot_generation;
    context.active_view = aida::ui::stable_view_id_t("view.ai.scripts");
    context.validate_identity = [identity] {
        const auto current = task_center::snapshot();
        return current && current->generation == identity.snapshot_generation &&
                find_script_run(*current, identity)
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The immutable Task Center snapshot or retained run identity changed; reopen the context menu");
    };
    const auto append = [&](const char* id, script_run_action_t action) {
        context.actions.push_back(retained_script_action(id,
            script_run_action_unavailable_reason(&task, action),
            [identity, action] { return invoke_script_run_action(identity, action); }));
    };
    append("programming.run.cancel", script_run_action_t::cancel);
    append("programming.run.retry_review", script_run_action_t::retry);
    append("programming.run.focus", script_run_action_t::focus);
    append("programming.run.open_log", script_run_action_t::open_log);
    documents::show_retained_entity_menu(context, origin, global_pos, parent);
}

void open_configuration_context(int catalog_index,
                                aida::ui::context_menu_open_origin_t origin,
                                const QPoint& global_pos, QWidget* parent) {
    auto& controller = AidaProgrammingTasksController::instance();
    const auto& catalog = controller.catalog();
    if (catalog_index < 0 || catalog_index >= static_cast<int>(catalog.configurations.size()))
        return;
    const tasks::configuration_t retained = catalog.configurations[static_cast<std::size_t>(catalog_index)];
    const std::uint64_t generation = catalog.configuration_generation;
    const std::uint64_t catalog_fingerprint = catalog.catalog_fingerprint;
    const std::string project_root = catalog.project_root;
    const auto validate = [retained, generation, catalog_fingerprint, project_root] {
        return tasks::validate_configuration_identity(retained, generation,
            catalog_fingerprint, project_root);
    };
    auto invoke_selected = [retained, validate](auto&& operation) {
        const auto current = validate();
        if (!current.enabled)
            return action_handler_result_t::failed(current.disabled_reason);
        const int retained_index = tasks::configuration_index(retained.id);
        const auto selected = tasks::select_configuration(retained_index, false);
        if (!selected.succeeded)
            return action_handler_result_t::failed(selected.detail.empty()
                ? "The retained task configuration could not be selected" : selected.detail);
        return operation(retained_index);
    };
    aida::ui::application_ui::retained_entity_context_t context;
    context.owner_id = "programming.scripts.configuration";
    context.entity_id = retained.id;
    context.entity_generation = generation ^ catalog_fingerprint;
    context.active_view = aida::ui::stable_view_id_t("view.ai.scripts");
    context.validate_identity = validate;
    std::string run_unavailable;
    if (catalog.loading)
        run_unavailable = "Programming configurations are loading";
    else if (catalog.editor_save_in_flight && catalog.selected_id != retained.id)
        run_unavailable = "Wait for task configuration persistence before changing selection";
    else if (catalog.editor_dirty && catalog.selected_id != retained.id)
        run_unavailable = "Save or revert the edited configuration before changing selection";
    else
        run_unavailable = tasks::configuration_run_gate_reason(retained,
            tasks::configuration_run_gate_t::run);
    context.actions.push_back(retained_script_action(
        "programming.configuration.run_review", run_unavailable,
        [invoke_selected] {
            return invoke_selected([](int) {
                const auto result = tasks::request_run_selected();
                return result.succeeded ? action_handler_result_t::completed()
                    : action_handler_result_t::failed(result.detail);
            });
        }));
    std::string edit_unavailable = retained.origin == tasks::configuration_origin_t::project &&
        project_root.empty() ? "Open a code workspace before opening .aida/tasks.json" : std::string{};
    if (edit_unavailable.empty() && catalog.editor_save_in_flight &&
        catalog.selected_id != retained.id)
        edit_unavailable = "Wait for task configuration persistence before changing selection";
    if (edit_unavailable.empty() && catalog.editor_dirty && catalog.selected_id != retained.id)
        edit_unavailable = "Save or revert the edited configuration before changing selection";
    context.actions.push_back(retained_script_action(
        "programming.configuration.open_edit", edit_unavailable,
        [invoke_selected, retained] {
            return invoke_selected([retained](int retained_index) {
                static_cast<void>(retained_index);
                if (retained.origin == tasks::configuration_origin_t::project) {
                    const auto opened = tasks::open_project_configuration_file();
                    return opened.succeeded ? action_handler_result_t::completed()
                        : action_handler_result_t::failed(opened.detail);
                }
                AidaProgrammingTasksController::instance().openConfigurationEditor();
                return action_handler_result_t::completed();
            });
        }));
    const std::size_t user_count = static_cast<std::size_t>(std::count_if(
        catalog.configurations.begin(), catalog.configurations.end(),
        [](const tasks::configuration_t& item) {
            return item.origin == tasks::configuration_origin_t::user;
        }));
    const std::string selection_unavailable = catalog.editor_save_in_flight
        ? "Wait for task configuration persistence before changing selection"
        : catalog.editor_dirty
            ? "Save, revert, or discard the edited configuration before changing selection"
            : std::string{};
    context.actions.push_back(retained_script_action(
        "programming.configuration.duplicate", !selection_unavailable.empty()
            ? selection_unavailable : user_count >= 64
                ? "User task configurations reached the 64-entry bound" : std::string{},
        [invoke_selected] {
            return invoke_selected([](int retained_index) {
                tasks::configuration_draft_t draft;
                const auto result = AidaProgrammingTasksController::instance().beginDuplicate(
                    retained_index, draft);
                if (result.succeeded)
                    AidaProgrammingTasksController::instance().openConfigurationEditor();
                return result.succeeded ? action_handler_result_t::completed()
                    : action_handler_result_t::failed(result.detail);
            });
        }));
    std::string delete_unavailable = !selection_unavailable.empty()
        ? selection_unavailable : retained.origin == tasks::configuration_origin_t::user
            ? std::string{} : "Project task configurations must be edited in .aida/tasks.json";
    if (delete_unavailable.empty())
        delete_unavailable = tasks::configuration_run_gate_reason(retained,
            tasks::configuration_run_gate_t::delete_);
    context.actions.push_back(retained_script_action(
        "programming.configuration.delete_review", delete_unavailable,
        [invoke_selected, retained] {
            return invoke_selected([retained](int retained_index) {
                if (retained.origin != tasks::configuration_origin_t::user)
                    return action_handler_result_t::failed(
                        "Project task configurations cannot be deleted from user settings");
                tasks::configuration_draft_t draft;
                const auto begun = AidaProgrammingTasksController::instance().beginEdit(
                    retained_index, draft);
                if (!begun.succeeded)
                    return action_handler_result_t::failed(begun.detail);
                AidaProgrammingTasksController::instance().openDeleteReview();
                return action_handler_result_t::completed();
            });
        }));
    documents::show_retained_entity_menu(context, origin, global_pos, parent);
}

const char* script_run_state_label(task_center::task_state_t value) {
    switch (value) {
    case task_center::task_state_t::running: return "Running";
    case task_center::task_state_t::cancellation_requested: return "Cancelling";
    case task_center::task_state_t::completed: return "Completed";
    case task_center::task_state_t::partial: return "Partial";
    case task_center::task_state_t::cancelled: return "Cancelled";
    case task_center::task_state_t::failed: return "Failed";
    case task_center::task_state_t::timed_out: return "Timed out";
    case task_center::task_state_t::interrupted: return "Interrupted";
    case task_center::task_state_t::queued: return "Queued";
    }
    return "Unknown";
}

class AidaScriptRunActionDelegate : public QStyledItemDelegate {
public:
    explicit AidaScriptRunActionDelegate(AidaScriptRunsModel* model, QObject* parent)
        : QStyledItemDelegate(parent), model_(model) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto* task = model_->rowAt(index.row());
        if (!task || (index.column() !=
                static_cast<int>(AidaScriptRunsModel::Column::actions) &&
            index.column() != static_cast<int>(AidaScriptRunsModel::Column::stage))) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const auto& tokens = theme::tokens();
        if (index.column() == static_cast<int>(AidaScriptRunsModel::Column::stage)) {
            QStyleOptionViewItem opt(option);
            initStyleOption(&opt, index);
            auto* style = opt.widget ? opt.widget->style() : QApplication::style();
            style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
            if (task->progress >= 0.0f && (task->state == task_center::task_state_t::queued ||
                task->state == task_center::task_state_t::running)) {
                const qreal bar_h = static_cast<qreal>(tokens.spacing.xxs);
                const QRectF bar(
                    opt.rect.left() + tokens.table.cell_pad_x,
                    opt.rect.bottom() - bar_h - tokens.table.cell_pad_y,
                    opt.rect.width() - tokens.table.cell_pad_x * 2, bar_h);
                const qreal fraction = (std::max)(0.0, (std::min)(1.0,
                    static_cast<qreal>(task->progress)));
                painter->save();
                painter->setPen(Qt::NoPen);
                painter->setBrush(widgets::with_alpha(tokens.text_secondary, 0.25));
                painter->drawRoundedRect(bar, tokens.radius.xs, tokens.radius.xs);
                painter->setBrush(tokens.accent);
                painter->drawRoundedRect(QRectF(bar.left(), bar.top(),
                    bar.width() * fraction, bar.height()), tokens.radius.xs, tokens.radius.xs);
                painter->restore();
            }
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (option.state & QStyle::State_Selected)
            painter->fillRect(option.rect, widgets::with_alpha(tokens.selection, 1.0));
        const auto action = action_for(*task);
        const QString label = label_for(action);
        if (label.isEmpty()) {
            painter->setPen(tokens.text_dim);
            painter->drawText(option.rect, Qt::AlignCenter,
                task->state == task_center::task_state_t::cancellation_requested
                    ? QStringLiteral("Pending") : QStringLiteral("-"));
            painter->restore();
            return;
        }
        const QColor color = action == script_run_action_t::cancel ? tokens.error
            : action == script_run_action_t::retry ? tokens.text_primary
            : tokens.accent;
        const QRect rect = action_rect(option, label);
        const qreal wash = (option.state & QStyle::State_Sunken) ? 0.40
            : (option.state & QStyle::State_MouseOver) ? 0.28 : 0.16;
        painter->setPen(Qt::NoPen);
        painter->setBrush(widgets::with_alpha(color, wash));
        painter->drawRoundedRect(QRectF(rect), tokens.radius.sm, tokens.radius.sm);
        painter->setPen(color);
        painter->drawText(rect, Qt::AlignCenter, label);
        painter->restore();
    }

    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option, const QModelIndex& index) override {
        static_cast<void>(model);
        if (event->type() != QEvent::MouseButtonRelease)
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton)
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        const auto* task = model_->rowAt(index.row());
        if (!task || index.column() !=
            static_cast<int>(AidaScriptRunsModel::Column::actions))
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        const auto action = action_for(*task);
        const QString label = label_for(action);
        if (label.isEmpty() || !action_rect(option, label).contains(mouse->pos()))
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        const std::string reason = script_run_action_unavailable_reason(task, action);
        if (!reason.empty()) {
            AidaProgrammingTasksController::instance().setScriptActionError(reason);
            return true;
        }
        AidaProgrammingTasksController::instance().clearScriptActionError();
        switch (action) {
        case script_run_action_t::cancel:
            static_cast<void>(task_center::request_cancel(task->id));
            break;
        case script_run_action_t::retry:
            static_cast<void>(task_center::retry(task->id));
            break;
        case script_run_action_t::focus:
            static_cast<void>(task_center::focus(task->id));
            break;
        case script_run_action_t::open_log:
            static_cast<void>(task_center::open_log(task->id));
            break;
        }
        return true;
    }

private:
    static script_run_action_t action_for(const task_center::task_snapshot_t& task) {
        if (task.state == task_center::task_state_t::cancellation_requested)
            return script_run_action_t::open_log;
        if (task.cancellable && (task.state == task_center::task_state_t::queued ||
            task.state == task_center::task_state_t::running))
            return script_run_action_t::cancel;
        if (task.retryable)
            return script_run_action_t::retry;
        if (task.focusable)
            return script_run_action_t::focus;
        return script_run_action_t::open_log;
    }

    static QString label_for(script_run_action_t action) {
        switch (action) {
        case script_run_action_t::cancel: return QStringLiteral("Cancel");
        case script_run_action_t::retry: return QStringLiteral("Retry");
        case script_run_action_t::focus: return QStringLiteral("Focus");
        case script_run_action_t::open_log: return QString();
        }
        return QString();
    }

    static QRect action_rect(const QStyleOptionViewItem& option, const QString& label) {
        const auto& tokens = theme::tokens();
        const int width = option.fontMetrics.horizontalAdvance(label) +
            tokens.table.cell_pad_x * 2 + tokens.spacing.sm * 2;
        const int height = (std::min)(option.rect.height() - tokens.radius.xs * 2,
            tokens.control.height_sm);
        return QRect(option.rect.right() - width - tokens.table.cell_pad_x,
            option.rect.center().y() - height / 2, width, height);
    }

    AidaScriptRunsModel* model_ = nullptr;
};

} 

AidaProgrammingTasksController& AidaProgrammingTasksController::instance() {
    if (!g_tasks_controller)
        g_tasks_controller = new AidaProgrammingTasksController();
    return *g_tasks_controller;
}

bool AidaProgrammingTasksController::exists() noexcept {
    return g_tasks_controller != nullptr;
}

AidaProgrammingTasksController::AidaProgrammingTasksController(QObject* parent)
    : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(250);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &AidaProgrammingTasksController::onTick);
    timer_->start();
}

void AidaProgrammingTasksController::install(docking::AidaDockHost* host,
                                             QWidget* dialog_parent) {
    host_ = host;
    dialog_parent_ = dialog_parent;
    tasks::host_ui_hooks_t hooks;
    hooks.present_configuration_editor = [] {
        AidaProgrammingTasksController::instance().openConfigurationEditor();
    };
    hooks.present_run_review = [] {
        AidaProgrammingTasksController::instance().presentPendingRunReview();
    };
    hooks.open_or_focus_view = [host](const char* view_id) {
        if (!host || !view_id)
            return;
        const bool gui_thread = QThread::currentThread() == qApp->thread();
        if (gui_thread) {
            static_cast<void>(host->open_or_focus(registry::stable_view_id_t(view_id)));
            return;
        }
        QMetaObject::invokeMethod(&AidaProgrammingTasksController::instance(),
            [host, id = std::string(view_id)] {
                if (host)
                    static_cast<void>(host->open_or_focus(registry::stable_view_id_t(id)));
            }, Qt::QueuedConnection);
    };
    tasks::install_host_ui_hooks(std::move(hooks));
    pullCatalog();
}

void AidaProgrammingTasksController::onTick() {
    tasks::tick();
    pullCatalog();
}

void AidaProgrammingTasksController::pullCatalog() {
    auto next = tasks::catalog_snapshot();
    const bool catalog_changed =
        next.configuration_generation != observed_generation_ ||
        next.selected_id != catalog_.selected_id ||
        next.selected_channel != catalog_.selected_channel ||
        next.channels != catalog_.channels ||
        next.loading != catalog_.loading ||
        next.configuration_error != catalog_.configuration_error ||
        next.active_run_count != catalog_.active_run_count ||
        next.problem_count != catalog_.problem_count;
    const bool editor_changed =
        next.editor_dirty != observed_editor_dirty_ ||
        next.editor_save_in_flight != observed_editor_save_in_flight_ ||
        next.editor_selected != observed_editor_selected_ ||
        next.editor_creating != observed_editor_creating_ ||
        next.editor_validation_error != observed_validation_;
    catalog_ = std::move(next);
    observed_generation_ = catalog_.configuration_generation;
    observed_editor_dirty_ = catalog_.editor_dirty;
    observed_editor_save_in_flight_ = catalog_.editor_save_in_flight;
    observed_editor_selected_ = catalog_.editor_selected;
    observed_editor_creating_ = catalog_.editor_creating;
    observed_validation_ = catalog_.editor_validation_error;
    if (catalog_changed)
        Q_EMIT catalogChanged();
    if (editor_changed)
        Q_EMIT editorStateChanged();
}

tasks::operation_result_t AidaProgrammingTasksController::beginEdit(
        int index, tasks::configuration_draft_t& draft) {
    const auto result = tasks::begin_edit(index, draft);
    pullCatalog();
    return result;
}

tasks::operation_result_t AidaProgrammingTasksController::beginCreate(
        tasks::configuration_draft_t& draft) {
    const auto result = tasks::begin_create(draft);
    if (result.succeeded)
        pending_draft_ = draft;
    pullCatalog();
    return result;
}

tasks::operation_result_t AidaProgrammingTasksController::beginDuplicate(
        int index, tasks::configuration_draft_t& draft) {
    const auto result = tasks::begin_duplicate(index, draft);
    if (result.succeeded)
        pending_draft_ = draft;
    pullCatalog();
    return result;
}

tasks::operation_result_t AidaProgrammingTasksController::saveDraft(
        const tasks::configuration_draft_t& draft) {
    const auto result = tasks::save_draft(draft);
    if (result.succeeded)
        pending_draft_.reset();
    pullCatalog();
    return result;
}

void AidaProgrammingTasksController::discardDraft() {
    tasks::discard_draft();
    pending_draft_.reset();
    pullCatalog();
}

tasks::operation_result_t AidaProgrammingTasksController::revertDraft(
        tasks::configuration_draft_t& draft) {
    const auto result = tasks::revert_draft(draft);
    pullCatalog();
    return result;
}

tasks::operation_result_t AidaProgrammingTasksController::deleteSelected() {
    const auto result = tasks::delete_selected_configuration();
    pullCatalog();
    return result;
}

tasks::operation_result_t AidaProgrammingTasksController::selectConfiguration(int index,
        bool persist) {
    const auto result = tasks::select_configuration(index, persist);
    pullCatalog();
    return result;
}

void AidaProgrammingTasksController::setChannel(const std::string& channel) {
    tasks::set_selected_channel(channel);
    pullCatalog();
    Q_EMIT channelSelectionChanged();
}

void AidaProgrammingTasksController::openConfigurationEditor() {
    if (config_dialog_) {
        config_dialog_->raise();
        config_dialog_->activateWindow();
        return;
    }
    auto* dialog = new AidaTaskConfigurationDialog(dialog_parent_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    config_dialog_ = dialog;
    pullCatalog();
    if (catalog_.editor_creating && pending_draft_) {
        dialog->openForDraft(*pending_draft_);
        pending_draft_.reset();
    } else {
        dialog->openAtSelection();
    }
}

void AidaProgrammingTasksController::openDeleteReview() {
    if (delete_dialog_) {
        delete_dialog_->raise();
        delete_dialog_->activateWindow();
        return;
    }
    auto* dialog = new AidaDeleteConfigurationDialog(dialog_parent_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    delete_dialog_ = dialog;
    dialog->open();
}

void AidaProgrammingTasksController::presentPendingRunReview() {
    if (review_dialog_) {
        review_dialog_->raise();
        review_dialog_->activateWindow();
        return;
    }
    auto* dialog = new AidaRunReviewDialog(dialog_parent_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    review_dialog_ = dialog;
    dialog->openForPending();
}

void AidaProgrammingTasksController::openRunReview() {
    presentPendingRunReview();
}

AidaTaskControlsStrip::AidaTaskControlsStrip(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.output.task_controls"));
    const auto& tokens = theme::tokens();
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(tokens.spacing.xs, tokens.spacing.xxs,
        tokens.spacing.xs, tokens.spacing.xxs);
    row->setSpacing(tokens.spacing.xs);
    configuration_combo_ = new QComboBox(this);
    configuration_combo_->setObjectName(QStringLiteral("aida.view.output.task_controls.config"));
    configuration_combo_->setToolTip(QStringLiteral("Active task configuration"));
    configuration_combo_->setMinimumWidth(
        configuration_combo_->fontMetrics().averageCharWidth() * 18);
    configuration_combo_->setMaximumWidth(
        configuration_combo_->fontMetrics().averageCharWidth() * 40);
    row->addWidget(configuration_combo_);
    run_button_ = new QPushButton(QStringLiteral("Run..."), this);
    run_button_->setObjectName(QStringLiteral("aida.view.output.task_controls.run"));
    row->addWidget(run_button_);
    configure_button_ = new QPushButton(QStringLiteral("Configure..."), this);
    configure_button_->setObjectName(QStringLiteral("aida.view.output.task_controls.configure"));
    row->addWidget(configure_button_);
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_button_->setObjectName(QStringLiteral("aida.view.output.task_controls.cancel"));
    row->addWidget(cancel_button_);
    problems_button_ = new QPushButton(QStringLiteral("Problems"), this);
    problems_button_->setObjectName(QStringLiteral("aida.view.output.task_controls.problems"));
    row->addWidget(problems_button_);
    channel_combo_ = new QComboBox(this);
    channel_combo_->setObjectName(QStringLiteral("aida.view.output.task_controls.channel"));
    channel_combo_->setToolTip(QStringLiteral(
        "Restrict the output log to one task-output channel"));
    channel_combo_->setMinimumWidth(
        channel_combo_->fontMetrics().averageCharWidth() * 16);
    channel_combo_->setMaximumWidth(
        channel_combo_->fontMetrics().averageCharWidth() * 34);
    row->addWidget(channel_combo_);
    channel_button_ = new QToolButton(this);
    channel_button_->setObjectName(QStringLiteral("aida.view.output.task_controls.channel.button"));
    channel_button_->setText(QStringLiteral("Channel"));
    channel_button_->setToolTip(QStringLiteral(
        "Restrict the output log to one task-output channel"));
    channel_button_->setPopupMode(QToolButton::InstantPopup);
    channel_button_->setAutoRaise(true);
    channel_menu_ = new QMenu(channel_button_);
    channel_menu_->setObjectName(QStringLiteral("aida.view.output.task_controls.channel.menu"));
    channel_menu_->setToolTipsVisible(true);
    connect(channel_menu_, &QMenu::aboutToShow, this, [this] {
        channel_menu_->clear();
        const auto& catalog = AidaProgrammingTasksController::instance().catalog();
        const auto append = [&](const QString& label, const QString& channel) {
            QAction* action = channel_menu_->addAction(label);
            action->setCheckable(true);
            action->setChecked(channel.toStdString() == catalog.selected_channel);
            connect(action, &QAction::triggered, this, [this, channel](bool) {
                AidaProgrammingTasksController::instance().setChannel(channel.toStdString());
                AidaOutputController::instance().noteExternalChannelChange();
            });
        };
        append(QStringLiteral("All Output"), QString());
        for (const auto& channel : catalog.channels)
            append(QString::fromStdString(channel), QString::fromStdString(channel));
    });
    channel_button_->setMenu(channel_menu_);
    channel_button_->setVisible(false);
    row->addWidget(channel_button_);
    error_label_ = new QLabel(QStringLiteral("Configuration error"), this);
    error_label_->setObjectName(QStringLiteral("aida.view.output.task_controls.error"));
    error_label_->setProperty("aidaVariant", "error");
    error_label_->setVisible(false);
    row->addWidget(error_label_);
    loading_label_ = new QLabel(QStringLiteral("Loading configurations..."), this);
    loading_label_->setObjectName(QStringLiteral("aida.view.output.task_controls.loading"));
    loading_label_->setEnabled(false);
    loading_label_->setVisible(false);
    row->addWidget(loading_label_);
    row->addStretch(1);

    const int output_tab = static_cast<int>(bottom_tab_t::output);
    connect(configuration_combo_, &QComboBox::activated, this, [this](int row) {
        if (updating_combos_ || row < 0)
            return;
        const int index = configuration_combo_->itemData(row).toInt();
        AidaProgrammingTasksController::instance().selectConfiguration(index, true);
    });
    connect(run_button_, &QPushButton::clicked, this, [output_tab] {
        static_cast<void>(aida::ui::application_ui::execute_output_action(output_tab,
            "programming.task.run", aida::ui::action_invocation_source_t::toolbar));
    });
    connect(configure_button_, &QPushButton::clicked, this, [output_tab] {
        static_cast<void>(aida::ui::application_ui::execute_output_action(output_tab,
            "programming.task.configure", aida::ui::action_invocation_source_t::toolbar));
    });
    connect(cancel_button_, &QPushButton::clicked, this, [output_tab] {
        static_cast<void>(aida::ui::application_ui::execute_output_action(output_tab,
            "programming.task.cancel", aida::ui::action_invocation_source_t::toolbar));
    });
    connect(problems_button_, &QPushButton::clicked, this, [output_tab] {
        static_cast<void>(aida::ui::application_ui::execute_output_action(output_tab,
            "programming.show_problems", aida::ui::action_invocation_source_t::toolbar));
    });
    connect(channel_combo_, &QComboBox::activated, this, [this](int row) {
        if (updating_combos_ || row < 0)
            return;
        const QString channel = channel_combo_->itemData(row).toString();
        AidaProgrammingTasksController::instance().setChannel(channel.toStdString());
        AidaOutputController::instance().noteExternalChannelChange();
    });
    auto& controller = AidaProgrammingTasksController::instance();
    connect(&controller, &AidaProgrammingTasksController::catalogChanged,
            this, &AidaTaskControlsStrip::refresh);
    connect(&controller, &AidaProgrammingTasksController::editorStateChanged,
            this, &AidaTaskControlsStrip::refresh);
    refresh();
}

void AidaTaskControlsStrip::refresh() {
    const auto& catalog = AidaProgrammingTasksController::instance().catalog();
    rebuildConfigurationCombo();
    rebuildChannelCombo();
    const auto run = aida::ui::application_ui::present_output_action(
        static_cast<int>(bottom_tab_t::output), "programming.task.run");
    run_button_->setEnabled(run.enabled);
    run_button_->setToolTip(run.enabled ? QString::fromStdString(run.description)
        : QString::fromStdString(run.disabled_reason));
    const auto configure = aida::ui::application_ui::present_output_action(
        static_cast<int>(bottom_tab_t::output), "programming.task.configure");
    configure_button_->setEnabled(configure.enabled);
    configure_button_->setToolTip(configure.enabled
        ? QString::fromStdString(configure.description)
        : QString::fromStdString(configure.disabled_reason));
    const bool active_run = catalog.active_run_count != 0;
    cancel_button_->setVisible(active_run);
    if (active_run) {
        const auto cancel = aida::ui::application_ui::present_output_action(
            static_cast<int>(bottom_tab_t::output), "programming.task.cancel");
        cancel_button_->setEnabled(cancel.enabled);
        cancel_button_->setToolTip(cancel.enabled
            ? QString::fromStdString(cancel.description)
            : QString::fromStdString(cancel.disabled_reason));
    }
    const std::size_t problems = catalog.problem_count;
    problems_button_->setVisible(problems != 0);
    if (problems != 0)
        problems_button_->setText(QStringLiteral("Problems (%1)").arg(problems));
    error_label_->setVisible(!catalog.configuration_error.empty());
    if (!catalog.configuration_error.empty())
        error_label_->setToolTip(QString::fromStdString(catalog.configuration_error));
    loading_label_->setVisible(catalog.loading);
    configuration_combo_->setEnabled(!catalog.editor_save_in_flight && !catalog.loading);
    updateCompactMode();
}

void AidaTaskControlsStrip::rebuildConfigurationCombo() {
    const auto& catalog = AidaProgrammingTasksController::instance().catalog();
    updating_combos_ = true;
    const QSignalBlocker blocker(configuration_combo_);
    configuration_combo_->clear();
    if (catalog.configurations.empty())
        configuration_combo_->addItem(QStringLiteral("No task configuration"), -1);
    int selected_row = -1;
    for (int index = 0; index < static_cast<int>(catalog.configurations.size()); ++index) {
        const auto& config = catalog.configurations[static_cast<std::size_t>(index)];
        configuration_combo_->addItem(QString::fromStdString(config.name), index);
        if (config.id == catalog.selected_id)
            selected_row = index;
    }
    configuration_combo_->setCurrentIndex(selected_row);
    updating_combos_ = false;
}

void AidaTaskControlsStrip::rebuildChannelCombo() {
    const auto& catalog = AidaProgrammingTasksController::instance().catalog();
    updating_combos_ = true;
    const QSignalBlocker blocker(channel_combo_);
    channel_combo_->clear();
    channel_combo_->addItem(QStringLiteral("All Output"), QString());
    int selected_row = 0;
    for (int index = 0; index < static_cast<int>(catalog.channels.size()); ++index) {
        const auto& channel = catalog.channels[static_cast<std::size_t>(index)];
        channel_combo_->addItem(QString::fromStdString(channel),
            QString::fromStdString(channel));
        if (channel == catalog.selected_channel)
            selected_row = index + 1;
    }
    channel_combo_->setCurrentIndex(selected_row);
    updating_combos_ = false;
}

void AidaTaskControlsStrip::updateCompactMode() {
    if (!channel_combo_ || !channel_button_)
        return;
    const bool was_visible = channel_combo_->isVisible();
    channel_combo_->setVisible(true);
    const int wide_width = layout()->totalSizeHint().width();
    channel_combo_->setVisible(was_visible);
    if (width() <= 0)
        return;
    compact_ = width() < wide_width;
    channel_combo_->setVisible(!compact_);
    channel_button_->setVisible(compact_);
}

void AidaTaskControlsStrip::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateCompactMode();
}

AidaConfigurationListModel::AidaConfigurationListModel(QObject* parent)
    : QAbstractListModel(parent) {
}

void AidaConfigurationListModel::setCatalog(tasks::catalog_snapshot_t catalog) {
    beginResetModel();
    const std::string filter = filter_;
    catalog_ = std::move(catalog);
    visible_.clear();
    for (int index = 0; index < static_cast<int>(catalog_.configurations.size()); ++index) {
        const auto& config = catalog_.configurations[static_cast<std::size_t>(index)];
        if (contains_case_insensitive(config.name, filter) ||
            contains_case_insensitive(config.command, filter) ||
            contains_case_insensitive(tasks::kind_name(config.kind), filter))
            visible_.push_back(index);
    }
    endResetModel();
}

void AidaConfigurationListModel::setFilter(const QString& filter) {
    const std::string next = filter.toStdString();
    if (filter_ == next)
        return;
    filter_ = next;
    setCatalog(catalog_);
}

int AidaConfigurationListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

QVariant AidaConfigurationListModel::data(const QModelIndex& index, int role) const {
    const auto* config = rowAt(index.row());
    if (!config)
        return {};
    if (role == Qt::DisplayRole) {
        return QString::fromStdString(config->name) +
            (config->origin == tasks::configuration_origin_t::project
                ? QStringLiteral("  [Project]") : QStringLiteral("  [User]"));
    }
    if (role == Qt::ToolTipRole) {
        QString tip = QString::fromStdString(config->name) + QStringLiteral("\n") +
            (config->origin == tasks::configuration_origin_t::project
                ? QStringLiteral("Project (.aida/tasks.json)") : QStringLiteral("User settings"));
        if (!config->command.empty())
            tip += QStringLiteral("\n") +
                QString::fromStdString(tasks::redacted_command(config->command));
        return tip;
    }
    if (role == Qt::UserRole)
        return QString::fromStdString(config->id);
    return {};
}

const tasks::configuration_t* AidaConfigurationListModel::rowAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(visible_.size()))
        return nullptr;
    return &catalog_.configurations[static_cast<std::size_t>(visible_[static_cast<std::size_t>(row)])];
}

int AidaConfigurationListModel::rowForId(const std::string& id) const noexcept {
    for (int row = 0; row < static_cast<int>(visible_.size()); ++row)
        if (catalog_.configurations[static_cast<std::size_t>(visible_[static_cast<std::size_t>(row)])].id == id)
            return row;
    return -1;
}

int AidaConfigurationListModel::sourceIndex(int view_row) const noexcept {
    if (view_row < 0 || view_row >= static_cast<int>(visible_.size()))
        return -1;
    return visible_[static_cast<std::size_t>(view_row)];
}

AidaScriptRunsModel::AidaScriptRunsModel(QObject* parent) : QAbstractTableModel(parent) {
}

void AidaScriptRunsModel::setSnapshot(task_center::immutable_snapshot_ptr snapshot) {
    const std::uint64_t next_generation = snapshot ? snapshot->generation : 0;
    if (next_generation == generation_)
        return;
    beginResetModel();
    snapshot_ = std::move(snapshot);
    generation_ = next_generation;
    rows_.clear();
    if (snapshot_) {
        for (auto it = snapshot_->tasks.rbegin(); it != snapshot_->tasks.rend(); ++it)
            if (it->source == "programming.config")
                rows_.push_back(&*it);
    }
    endResetModel();
}

int AidaScriptRunsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int AidaScriptRunsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Column::count);
}

const task_center::task_snapshot_t* AidaScriptRunsModel::rowAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return rows_[static_cast<std::size_t>(row)];
}

QVariant AidaScriptRunsModel::data(const QModelIndex& index, int role) const {
    const auto* task = rowAt(index.row());
    if (!task)
        return {};
    if (role == Qt::DisplayRole) {
        switch (static_cast<Column>(index.column())) {
        case Column::script: return QString::fromStdString(task->label);
        case Column::state: return QString::fromLatin1(script_run_state_label(task->state));
        case Column::stage:
            return task->result_summary.empty()
                ? QString::fromStdString(task->stage)
                : QString::fromStdString(task->stage) + QStringLiteral("\n") +
                    QString::fromStdString(task->result_summary);
        case Column::actions: break;
        case Column::count: break;
        }
        return {};
    }
    if (role == Qt::ToolTipRole &&
        static_cast<Column>(index.column()) == Column::stage && !task->stage.empty())
        return QString::fromStdString(task->stage);
    if (role == Qt::UserRole)
        return QString::fromStdString(task->id);
    return {};
}

void AidaScriptRunsModel::multiData(const QModelIndex& index,
                                    QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

QVariant AidaScriptRunsModel::headerData(int section, Qt::Orientation orientation,
                                         int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (static_cast<Column>(section)) {
    case Column::script: return QStringLiteral("Script");
    case Column::state: return QStringLiteral("State");
    case Column::stage: return QStringLiteral("Stage");
    case Column::actions: return QStringLiteral("Actions");
    case Column::count: break;
    }
    return {};
}

AidaAutomationScriptsView::AidaAutomationScriptsView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.ai.scripts"));
    const auto& tokens = theme::tokens();
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(tokens.spacing.sm, tokens.spacing.xs,
        tokens.spacing.sm, tokens.spacing.xs);
    column->setSpacing(tokens.spacing.xs);

    auto* top = new QHBoxLayout();
    filter_edit_ = new QLineEdit(this);
    filter_edit_->setObjectName(QStringLiteral("aida.view.ai.scripts.filter"));
    filter_edit_->setPlaceholderText(QStringLiteral("Filter scripts..."));
    filter_edit_->setToolTip(QStringLiteral(
        "Filter configurations by name, command, or kind (case-insensitive)"));
    filter_edit_->setClearButtonEnabled(true);
    top->addWidget(filter_edit_, 1);
    new_button_ = new QPushButton(QStringLiteral("New"), this);
    new_button_->setObjectName(QStringLiteral("aida.view.ai.scripts.new"));
    new_button_->setToolTip(QStringLiteral("Create a user task configuration"));
    top->addWidget(new_button_);
    reload_button_ = new QPushButton(QStringLiteral("Reload"), this);
    reload_button_->setObjectName(QStringLiteral("aida.view.ai.scripts.reload"));
    reload_button_->setToolTip(QStringLiteral(
        "Reload user and project task configurations from disk"));
    top->addWidget(reload_button_);
    loading_label_ = new QLabel(QStringLiteral("Loading configurations..."), this);
    loading_label_->setObjectName(QStringLiteral("aida.view.ai.scripts.loading"));
    loading_label_->setEnabled(false);
    loading_label_->setVisible(false);
    top->addWidget(loading_label_);
    column->addLayout(top);

    error_strip_ = new widgets::AidaNotice(QStringLiteral("Configuration error"),
        QString(), widgets::AidaSemantic::Error, this);
    error_strip_->setObjectName(QStringLiteral("aida.view.ai.scripts.error"));
    error_strip_->setActionLabel(QStringLiteral("Retry load"));
    error_strip_->setVisible(false);
    column->addWidget(error_strip_);
    stale_hint_ = new QLabel(QStringLiteral(
        "The retained configuration snapshot may be stale."), this);
    stale_hint_->setObjectName(QStringLiteral("aida.view.ai.scripts.stale"));
    stale_hint_->setEnabled(false);
    stale_hint_->setVisible(false);
    column->addWidget(stale_hint_);
    script_error_strip_ = new widgets::AidaNotice(QString(),
        QString(), widgets::AidaSemantic::Error, this);
    script_error_strip_->setObjectName(QStringLiteral("aida.view.ai.scripts.script_error"));
    script_error_strip_->setVisible(false);
    column->addWidget(script_error_strip_);

    splitter_ = new QSplitter(this);
    splitter_->setObjectName(QStringLiteral("aida.view.ai.scripts.splitter"));
    splitter_->setOpaqueResize(true);
    splitter_->setChildrenCollapsible(false);

    catalog_model_ = new AidaConfigurationListModel(this);
    catalog_view_ = new QListView(splitter_);
    catalog_view_->setObjectName(QStringLiteral("aida.view.ai.scripts.catalog"));
    catalog_view_->setModel(catalog_model_);
    catalog_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    catalog_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    catalog_view_->setMinimumWidth(
        catalog_view_->fontMetrics().averageCharWidth() * 26);
    catalog_view_->installEventFilter(this);
    catalog_view_->viewport()->installEventFilter(this);
    splitter_->addWidget(catalog_view_);

    auto* detail = new QWidget(splitter_);
    detail->setObjectName(QStringLiteral("aida.view.ai.scripts.detail"));
    auto* detail_layout = new QVBoxLayout(detail);
    detail_layout->setContentsMargins(tokens.spacing.sm, tokens.spacing.xs,
        tokens.spacing.sm, tokens.spacing.xs);
    detail_layout->setSpacing(tokens.spacing.xs);
    auto* name_row = new QHBoxLayout();
    detail_name_ = new QLabel(detail);
    detail_name_->setObjectName(QStringLiteral("aida.view.ai.scripts.detail.name"));
    name_row->addWidget(detail_name_);
    detail_meta_ = new QLabel(detail);
    detail_meta_->setEnabled(false);
    name_row->addWidget(detail_meta_);
    name_row->addStretch(1);
    detail_layout->addLayout(name_row);
    auto* button_row = new QHBoxLayout();
    detail_run_ = new QPushButton(QStringLiteral("Run with Review..."), detail);
    detail_run_->setObjectName(QStringLiteral("aida.view.ai.scripts.detail.run"));
    detail_edit_ = new QPushButton(QStringLiteral("Edit..."), detail);
    detail_edit_->setObjectName(QStringLiteral("aida.view.ai.scripts.detail.edit"));
    detail_duplicate_ = new QPushButton(QStringLiteral("Duplicate..."), detail);
    detail_duplicate_->setObjectName(QStringLiteral("aida.view.ai.scripts.detail.duplicate"));
    button_row->addWidget(detail_run_);
    button_row->addWidget(detail_edit_);
    button_row->addWidget(detail_duplicate_);
    button_row->addStretch(1);
    detail_layout->addLayout(button_row);
    const auto add_prop = [this, detail, detail_layout](const char* object_name) {
        auto* label = new QLabel(detail);
        label->setObjectName(QString::fromLatin1(object_name));
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detail_layout->addWidget(label);
        return label;
    };
    prop_source_ = add_prop("aida.view.ai.scripts.detail.source");
    prop_scope_ = add_prop("aida.view.ai.scripts.detail.scope");
    prop_command_ = add_prop("aida.view.ai.scripts.detail.command");
    prop_workdir_ = add_prop("aida.view.ai.scripts.detail.workdir");
    prop_output_ = add_prop("aida.view.ai.scripts.detail.output");
    prop_matcher_ = add_prop("aida.view.ai.scripts.detail.matcher");
    auto* env_note = new QLabel(QStringLiteral(
        "Environment: inherited by the canonical executor; credentials are never expanded in this catalog"), detail);
    env_note->setEnabled(false);
    env_note->setWordWrap(true);
    detail_layout->addWidget(env_note);
    auto* gate_note = new QLabel(QStringLiteral(
        "Execution is approval-gated and runs outside the debugger in a cancellable process job."), detail);
    gate_note->setEnabled(false);
    gate_note->setWordWrap(true);
    detail_layout->addWidget(gate_note);

    auto* runs_header = new QLabel(QStringLiteral("Runs"), detail);
    runs_header->setObjectName(QStringLiteral("aida.view.ai.scripts.runs.header"));
    detail_layout->addWidget(runs_header);
    runs_model_ = new AidaScriptRunsModel(detail);
    runs_table_ = new QTableView(detail);
    runs_table_->setModel(runs_model_);
    runs_table_->setObjectName(QStringLiteral("aida.view.ai.scripts.runs"));
    runs_table_->verticalHeader()->hide();
    runs_table_->verticalHeader()->setDefaultSectionSize(
        theme::tokens().table.row_h * 3);
    runs_table_->setShowGrid(false);
    runs_table_->setAlternatingRowColors(true);
    runs_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    runs_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    runs_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    runs_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* runs_delegate = new AidaScriptRunActionDelegate(runs_model_, runs_table_);
    runs_table_->setItemDelegateForColumn(static_cast<int>(AidaScriptRunsModel::Column::stage),
        runs_delegate);
    runs_table_->setItemDelegateForColumn(static_cast<int>(AidaScriptRunsModel::Column::actions),
        runs_delegate);
    runs_table_->installEventFilter(this);
    runs_table_->viewport()->installEventFilter(this);
    auto* runs_h = runs_table_->horizontalHeader();
    const int runs_pad = theme::tokens().table.cell_pad_x;
    runs_h->setSectionResizeMode(0, QHeaderView::Stretch);
    runs_h->setSectionResizeMode(1, QHeaderView::Fixed);
    runs_h->resizeSection(1, runs_h->fontMetrics().horizontalAdvance(
        QStringLiteral("Cancelling")) + runs_pad * 2);
    runs_h->setSectionResizeMode(2, QHeaderView::Stretch);
    runs_h->setSectionResizeMode(3, QHeaderView::Fixed);
    runs_h->resizeSection(3, runs_h->fontMetrics().horizontalAdvance(
        QStringLiteral("Cancel")) + runs_pad * 2 + theme::tokens().spacing.sm * 2 +
        runs_pad * 2);
    detail_layout->addWidget(runs_table_, 1);
    splitter_->addWidget(detail);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    column->addWidget(splitter_, 1);

    auto& controller = AidaProgrammingTasksController::instance();
    connect(&controller, &AidaProgrammingTasksController::catalogChanged,
            this, &AidaAutomationScriptsView::refresh);
    connect(&controller, &AidaProgrammingTasksController::editorStateChanged,
            this, &AidaAutomationScriptsView::refresh);
    connect(&AidaTaskCenterController::instance(),
            &AidaTaskCenterController::snapshotChanged, this, [this](quint64) {
        runs_model_->setSnapshot(AidaTaskCenterController::instance().current());
    });
    connect(filter_edit_, &QLineEdit::textChanged, catalog_model_,
            &AidaConfigurationListModel::setFilter);
    connect(new_button_, &QPushButton::clicked, this, [this] {
        tasks::configuration_draft_t draft;
        const auto result = AidaProgrammingTasksController::instance().beginCreate(draft);
        if (result.succeeded) {
            auto* dialog = new AidaTaskConfigurationDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->openForDraft(draft);
        } else {
            AidaProgrammingTasksController::instance().setScriptActionError(result.detail);
            refresh();
        }
    });
    connect(reload_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(tasks::reload_configurations());
    });
    connect(error_strip_, &widgets::AidaNotice::actionTriggered, this, [] {
        static_cast<void>(tasks::reload_configurations());
    });
    connect(catalog_view_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid())
            return;
        const int source = catalog_model_->sourceIndex(current.row());
        if (source >= 0) {
            AidaProgrammingTasksController::instance().selectConfiguration(source, true);
        }
        refreshDetail();
    });
    connect(catalog_view_, &QListView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const QModelIndex index = catalog_view_->indexAt(pos);
        openConfigurationContext(index.isValid() ? catalog_model_->sourceIndex(index.row()) : -1,
            aida::ui::context_menu_open_origin_t::pointer,
            catalog_view_->viewport()->mapToGlobal(pos));
    });
    connect(runs_table_, &QTableView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        openRunContext(runs_table_->indexAt(pos),
            aida::ui::context_menu_open_origin_t::pointer,
            runs_table_->viewport()->mapToGlobal(pos));
    });
    connect(detail_run_, &QPushButton::clicked, this, &AidaAutomationScriptsView::runSelected);
    connect(detail_edit_, &QPushButton::clicked, this, &AidaAutomationScriptsView::editSelected);
    connect(detail_duplicate_, &QPushButton::clicked, this,
            &AidaAutomationScriptsView::duplicateSelected);
    refresh();
}

void AidaAutomationScriptsView::refresh() {
    auto& controller = AidaProgrammingTasksController::instance();
    const auto& catalog = controller.catalog();
    catalog_model_->setCatalog(catalog);
    loading_label_->setVisible(catalog.loading);
    const bool has_error = !catalog.configuration_error.empty();
    error_strip_->setVisible(has_error);
    if (has_error)
        error_strip_->setMessage(QString::fromStdString(catalog.configuration_error));
    stale_hint_->setVisible(has_error && !catalog.configurations.empty());
    const auto script_error = controller.scriptActionError();
    script_error_strip_->setVisible(!script_error.empty());
    if (!script_error.empty()) {
        script_error_strip_->setTitle(QString::fromStdString(script_error));
    }
    new_button_->setEnabled(!catalog.loading && !catalog.editor_save_in_flight);
    reload_button_->setEnabled(!catalog.loading && !catalog.editor_save_in_flight);
    const int row = catalog_model_->rowForId(catalog.selected_id);
    if (row >= 0 && catalog_view_->currentIndex().row() != row) {
        catalog_view_->setCurrentIndex(catalog_model_->index(row, 0));
    }
    refreshDetail();
}

void AidaAutomationScriptsView::refreshDetail() {
    auto& controller = AidaProgrammingTasksController::instance();
    const auto& catalog = controller.catalog();
    const tasks::configuration_t* selected = nullptr;
    for (const auto& config : catalog.configurations)
        if (config.id == catalog.selected_id)
            selected = &config;
    if (!selected) {
        detail_name_->setText(QStringLiteral("Select a script configuration to inspect it"));
        detail_meta_->clear();
        prop_source_->clear();
        prop_scope_->clear();
        prop_command_->clear();
        prop_workdir_->clear();
        prop_output_->clear();
        prop_matcher_->clear();
        detail_run_->setEnabled(false);
        detail_edit_->setEnabled(false);
        detail_duplicate_->setEnabled(false);
        return;
    }
    detail_name_->setText(QString::fromStdString(selected->name));
    detail_meta_->setText(QStringLiteral("%1 / %2")
        .arg(QString::fromStdString(tasks::kind_name(selected->kind)),
            selected->origin == tasks::configuration_origin_t::project
                ? QStringLiteral("project") : QStringLiteral("user")));
    const auto unavailable = tasks::run_unavailable_reason();
    detail_run_->setEnabled(unavailable.empty());
    detail_run_->setToolTip(unavailable.empty()
        ? QStringLiteral("Resolve and review the exact process command before execution")
        : QString::fromStdString(unavailable));
    detail_edit_->setText(selected->origin == tasks::configuration_origin_t::project
        ? QStringLiteral("Open Source") : QStringLiteral("Edit..."));
    detail_edit_->setEnabled(true);
    detail_duplicate_->setEnabled(true);
    prop_source_->setText(QStringLiteral("Source: %1")
        .arg(selected->origin == tasks::configuration_origin_t::project
            ? QStringLiteral(".aida/tasks.json") : QStringLiteral("User settings")));
    prop_scope_->setText(QStringLiteral("Scope: %1")
        .arg(catalog.project_root.empty() ? QStringLiteral("No workspace")
                                          : QString::fromStdString(catalog.project_root)));
    prop_command_->setText(QStringLiteral("Command: %1")
        .arg(QString::fromStdString(tasks::redacted_command(selected->command))));
    prop_workdir_->setText(QStringLiteral("Working directory: %1")
        .arg(selected->cwd.empty() ? QStringLiteral("Inherited from AiDA")
                                   : QString::fromStdString(selected->cwd)));
    prop_output_->setText(QStringLiteral("Output: %1")
        .arg(selected->output_channel.empty() ? QString::fromStdString(selected->name)
                                              : QString::fromStdString(selected->output_channel)));
    prop_matcher_->setText(QStringLiteral("Problem matcher: %1")
        .arg(QString::fromStdString(selected->problem_matcher)));
}

void AidaAutomationScriptsView::runSelected() {
    const auto result = tasks::request_run_selected();
    if (!result.succeeded) {
        AidaProgrammingTasksController::instance().setScriptActionError(result.detail);
        refresh();
    }
}

void AidaAutomationScriptsView::editSelected() {
    auto& controller = AidaProgrammingTasksController::instance();
    const auto& catalog = controller.catalog();
    const tasks::configuration_t* selected = nullptr;
    for (const auto& config : catalog.configurations)
        if (config.id == catalog.selected_id)
            selected = &config;
    if (!selected)
        return;
    if (selected->origin == tasks::configuration_origin_t::project) {
        static_cast<void>(tasks::open_project_configuration_file());
        return;
    }
    controller.openConfigurationEditor();
}

void AidaAutomationScriptsView::duplicateSelected() {
    auto& controller = AidaProgrammingTasksController::instance();
    const auto& catalog = controller.catalog();
    const int index = tasks::configuration_index(catalog.selected_id);
    if (index < 0)
        return;
    tasks::configuration_draft_t draft;
    const auto result = controller.beginDuplicate(index, draft);
    if (result.succeeded) {
        auto* dialog = new AidaTaskConfigurationDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->openForDraft(draft);
    } else {
        controller.setScriptActionError(result.detail);
        refresh();
    }
}

void AidaAutomationScriptsView::openConfigurationContext(int catalog_index,
        aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos) {
    if (catalog_index < 0)
        return;
    open_configuration_context(catalog_index, origin, global_pos, this);
}

void AidaAutomationScriptsView::openRunContext(const QModelIndex& index,
        aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos) {
    const auto* task = runs_model_->rowAt(index.row());
    if (!task)
        return;
    selected_run_id_ = task->id;
    selected_run_generation_ = runs_model_->snapshotGeneration();
    open_script_run_context(*task, selected_run_generation_, origin, global_pos, this);
}

bool AidaAutomationScriptsView::eventFilter(QObject* watched, QEvent* event) {
    QModelIndex index;
    QPoint global_pos;
    if (keyboard_context_menu_event(watched, event, catalog_view_, &index, &global_pos)) {
        openConfigurationContext(index.isValid() ? catalog_model_->sourceIndex(index.row()) : -1,
            aida::ui::context_menu_open_origin_t::menu_key, global_pos);
        return true;
    }
    if (keyboard_context_menu_event(watched, event, runs_table_, &index, &global_pos)) {
        openRunContext(index, aida::ui::context_menu_open_origin_t::menu_key, global_pos);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

AidaTaskConfigurationDialog::AidaTaskConfigurationDialog(QWidget* parent)
    : bridge::AidaDialog(parent) {
    setObjectName(QStringLiteral("aida.dialog.programming.task.configurations"));
    setWindowTitle(QStringLiteral("Programming Task Configurations"));
    resize(860, 540);
    setMinimumSize(620, 420);
    auto* column = new QVBoxLayout(this);
    auto* title = new QLabel(QStringLiteral("Explicit Programming Tasks and Launches"), this);
    column->addWidget(title);
    auto* subtitle = new QLabel(QStringLiteral(
        "AiDA runs only configurations you define here or in .aida/tasks.json. This does not invoke RE Run Target."), this);
    subtitle->setEnabled(false);
    subtitle->setWordWrap(true);
    column->addWidget(subtitle);

    auto* body = new QSplitter(this);
    body->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.splitter"));
    body->setOpaqueResize(true);
    body->setChildrenCollapsible(false);
    list_model_ = new AidaConfigurationListModel(this);
    list_ = new QListView(body);
    list_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.list"));
    list_->setModel(list_model_);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setMinimumWidth(list_->fontMetrics().averageCharWidth() * 22);
    body->addWidget(list_);

    auto* form = new QWidget(body);
    auto* form_layout = new QVBoxLayout(form);
    const auto& tokens = theme::tokens();
    form_layout->setContentsMargins(tokens.spacing.sm, tokens.spacing.xs,
        tokens.spacing.sm, tokens.spacing.xs);
    form_layout->setSpacing(tokens.spacing.xs);
    add_button_ = new QPushButton(QStringLiteral("Add User Configuration"), form);
    add_button_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.add"));
    form_layout->addWidget(add_button_);
    origin_label_ = new QLabel(form);
    origin_label_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.origin"));
    origin_label_->setEnabled(false);
    origin_label_->setVisible(false);
    form_layout->addWidget(origin_label_);
    open_source_button_ = new QPushButton(QStringLiteral("Open Configuration File"), form);
    open_source_button_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.open_source"));
    open_source_button_->setVisible(false);
    form_layout->addWidget(open_source_button_);
    creating_label_ = new QLabel(QStringLiteral("Unsaved user configuration"), form);
    creating_label_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.creating"));
    creating_label_->setEnabled(false);
    creating_label_->setVisible(false);
    form_layout->addWidget(creating_label_);
    form_layout->addWidget(new QLabel(QStringLiteral("Name"), form));
    name_ = new QLineEdit(form);
    name_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.name"));
    form_layout->addWidget(name_);
    form_layout->addWidget(new QLabel(QStringLiteral("Command"), form));
    command_ = new QPlainTextEdit(form);
    command_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.command"));
    command_->setFont(theme::fonts::codeRegular());
    command_->setMinimumHeight(command_->fontMetrics().lineSpacing() * 4 +
        theme::tokens().spacing.sm * 2);
    form_layout->addWidget(command_);
    form_layout->addWidget(new QLabel(QStringLiteral("Working directory"), form));
    cwd_ = new QLineEdit(form);
    cwd_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.cwd"));
    form_layout->addWidget(cwd_);
    form_layout->addWidget(new QLabel(QStringLiteral("Output channel"), form));
    channel_ = new QLineEdit(form);
    channel_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.channel"));
    form_layout->addWidget(channel_);
    auto* combos = new QHBoxLayout();
    auto* kind_label = new QLabel(QStringLiteral("Kind"), form);
    combos->addWidget(kind_label);
    kind_ = new QComboBox(form);
    kind_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.kind"));
    kind_->addItems({QStringLiteral("Task"), QStringLiteral("Launch"), QStringLiteral("Test")});
    combos->addWidget(kind_);
    auto* matcher_label = new QLabel(QStringLiteral("Problem matcher"), form);
    combos->addWidget(matcher_label);
    matcher_ = new QComboBox(form);
    matcher_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.matcher"));
    matcher_->addItems({QStringLiteral("None"), QStringLiteral("MSVC"),
        QStringLiteral("GCC/Clang"), QStringLiteral("Generic file:line:column")});
    combos->addWidget(matcher_, 1);
    form_layout->addLayout(combos);
    auto* variables = new QLabel(QStringLiteral(
        "Variables: ${workspaceFolder}, ${file}, ${fileDirname}"), form);
    variables->setEnabled(false);
    form_layout->addWidget(variables);
    error_ = new QLabel(form);
    error_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.error"));
    error_->setWordWrap(true);
    error_->setProperty("aidaVariant", "error");
    error_->setVisible(false);
    form_layout->addWidget(error_);
    auto* edit_row = new QHBoxLayout();
    save_button_ = new QPushButton(QStringLiteral("Save Configuration"), form);
    save_button_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.save"));
    discard_button_ = new QPushButton(QStringLiteral("Discard Draft"), form);
    discard_button_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.discard"));
    delete_button_ = new QPushButton(QStringLiteral("Delete..."), form);
    delete_button_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.delete"));
    revert_button_ = new QPushButton(QStringLiteral("Revert Edits"), form);
    revert_button_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.revert"));
    edit_row->addWidget(save_button_);
    edit_row->addWidget(discard_button_);
    edit_row->addWidget(delete_button_);
    edit_row->addWidget(revert_button_);
    edit_row->addStretch(1);
    form_layout->addLayout(edit_row);
    form_layout->addStretch(1);
    body->addWidget(form);
    body->setStretchFactor(0, 0);
    body->setStretchFactor(1, 1);
    column->addWidget(body, 1);

    auto* footer = new QHBoxLayout();
    reload_button_ = new QPushButton(QStringLiteral("Reload User and Project Configurations"), this);
    reload_button_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.reload"));
    footer->addWidget(reload_button_);
    footer->addStretch(1);
    close_button_ = new QPushButton(QStringLiteral("Close"), this);
    close_button_->setObjectName(QStringLiteral("aida.dialog.programming.task.configurations.close"));
    footer->addWidget(close_button_);
    column->addLayout(footer);

    connect(add_button_, &QPushButton::clicked, this, [this] {
        tasks::configuration_draft_t draft;
        const auto result = AidaProgrammingTasksController::instance().beginCreate(draft);
        if (result.succeeded) {
            loadDraft(draft);
            refreshButtons();
        }
    });
    connect(save_button_, &QPushButton::clicked, this, [this] {
        const auto result = AidaProgrammingTasksController::instance().saveDraft(currentDraft());
        if (!result.succeeded) {
            error_->setText(QString::fromStdString(result.detail));
            error_->setVisible(true);
        }
        refreshButtons();
    });
    connect(discard_button_, &QPushButton::clicked, this, [this] {
        AidaProgrammingTasksController::instance().discardDraft();
        name_->clear();
        command_->clear();
        cwd_->clear();
        channel_->clear();
        error_->setVisible(false);
        refreshButtons();
    });
    connect(delete_button_, &QPushButton::clicked, this, [this] {
        AidaProgrammingTasksController::instance().openDeleteReview();
    });
    connect(revert_button_, &QPushButton::clicked, this, [this] {
        tasks::configuration_draft_t draft;
        if (AidaProgrammingTasksController::instance().revertDraft(draft).succeeded)
            loadDraft(draft);
        refreshButtons();
    });
    connect(reload_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(tasks::reload_configurations());
    });
    connect(close_button_, &QPushButton::clicked, this, [this] {
        reject();
    });
    connect(open_source_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(tasks::open_project_configuration_file());
    });
    connect(list_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (current.isValid())
            onSelectionChanged(current.row());
    });
    const auto mark = [this] { markDirty(); };
    connect(name_, &QLineEdit::textEdited, this, mark);
    connect(command_, &QPlainTextEdit::textChanged, this, mark);
    connect(cwd_, &QLineEdit::textEdited, this, mark);
    connect(channel_, &QLineEdit::textEdited, this, mark);
    connect(kind_, &QComboBox::activated, this, mark);
    connect(matcher_, &QComboBox::activated, this, mark);
    auto& controller = AidaProgrammingTasksController::instance();
    connect(&controller, &AidaProgrammingTasksController::catalogChanged, this, [this] {
        const auto& catalog = AidaProgrammingTasksController::instance().catalog();
        list_model_->setCatalog(catalog);
        if (!catalog.editor_creating && catalog.editor_selected < 0) {
            name_->clear();
            command_->clear();
            cwd_->clear();
            channel_->clear();
        }
        refreshButtons();
    });
    connect(&controller, &AidaProgrammingTasksController::editorStateChanged, this, [this] {
        refreshButtons();
    });
    list_model_->setCatalog(controller.catalog());
}

void AidaTaskConfigurationDialog::openAtSelection() {
    const auto& catalog = AidaProgrammingTasksController::instance().catalog();
    const int index = tasks::configuration_index(catalog.selected_id);
    if (index >= 0) {
        tasks::configuration_draft_t draft;
        if (AidaProgrammingTasksController::instance().beginEdit(index, draft).succeeded)
            loadDraft(draft);
        const int row = list_model_->rowForId(catalog.selected_id);
        if (row >= 0)
            list_->setCurrentIndex(list_model_->index(row, 0));
    }
    open();
    if (catalog.configurations.empty())
        add_button_->setFocus();
    refreshButtons();
}

void AidaTaskConfigurationDialog::openForDraft(
        const tasks::configuration_draft_t& draft) {
    list_model_->setCatalog(AidaProgrammingTasksController::instance().catalog());
    loadDraft(draft);
    open();
    name_->setFocus();
    refreshButtons();
}

void AidaTaskConfigurationDialog::reject() {
    const auto& catalog = AidaProgrammingTasksController::instance().catalog();
    if (catalog.editor_dirty || catalog.editor_save_in_flight) {
        error_->setText(QStringLiteral(
            "Save, revert, or delete the edited user configuration before closing."));
        error_->setVisible(true);
        return;
    }
    QDialog::reject();
}

void AidaTaskConfigurationDialog::onSelectionChanged(int row) {
    const int source = list_model_->sourceIndex(row);
    if (source < 0)
        return;
    tasks::configuration_draft_t draft;
    const auto result = AidaProgrammingTasksController::instance().beginEdit(source, draft);
    if (result.succeeded) {
        loadDraft(draft);
        error_->setVisible(false);
    } else if (!result.detail.empty()) {
        error_->setText(QString::fromStdString(result.detail));
        error_->setVisible(true);
    }
    refreshButtons();
}

void AidaTaskConfigurationDialog::loadDraft(const tasks::configuration_draft_t& draft) {
    loading_fields_ = true;
    name_->setText(QString::fromStdString(draft.name));
    command_->setPlainText(QString::fromStdString(draft.command));
    cwd_->setText(QString::fromStdString(draft.cwd));
    channel_->setText(QString::fromStdString(draft.channel));
    kind_->setCurrentIndex(draft.kind);
    matcher_->setCurrentIndex(draft.matcher);
    loading_fields_ = false;
}

tasks::configuration_draft_t AidaTaskConfigurationDialog::currentDraft() const {
    tasks::configuration_draft_t draft;
    draft.name = name_->text().toStdString();
    draft.command = command_->toPlainText().toStdString();
    draft.cwd = cwd_->text().toStdString();
    draft.channel = channel_->text().toStdString();
    draft.kind = kind_->currentIndex();
    draft.matcher = matcher_->currentIndex();
    return draft;
}

void AidaTaskConfigurationDialog::markDirty() {
    if (loading_fields_)
        return;
    tasks::note_draft_edited();
    refreshButtons();
}

void AidaTaskConfigurationDialog::refreshButtons() {
    const auto& catalog = AidaProgrammingTasksController::instance().catalog();
    const bool has_selection = catalog.editor_selected >= 0 &&
        catalog.editor_selected < static_cast<int>(catalog.configurations.size());
    const bool project_owned = has_selection &&
        catalog.configurations[static_cast<std::size_t>(catalog.editor_selected)].origin ==
            tasks::configuration_origin_t::project;
    const bool read_only = project_owned;
    origin_label_->setVisible(project_owned);
    if (project_owned)
        origin_label_->setText(QStringLiteral("Project-owned: .aida/tasks.json"));
    open_source_button_->setVisible(project_owned);
    creating_label_->setVisible(catalog.editor_creating);
    name_->setReadOnly(read_only || catalog.editor_save_in_flight);
    command_->setReadOnly(read_only || catalog.editor_save_in_flight);
    cwd_->setReadOnly(read_only || catalog.editor_save_in_flight);
    channel_->setReadOnly(read_only || catalog.editor_save_in_flight);
    kind_->setEnabled(!read_only && !catalog.editor_save_in_flight);
    matcher_->setEnabled(!read_only && !catalog.editor_save_in_flight);
    save_button_->setEnabled(!catalog.editor_save_in_flight && catalog.editor_dirty);
    save_button_->setText(catalog.editor_save_in_flight ? QStringLiteral("Saving...")
        : QStringLiteral("Save Configuration"));
    discard_button_->setVisible(catalog.editor_creating);
    delete_button_->setVisible(!catalog.editor_creating);
    revert_button_->setVisible(!catalog.editor_creating && catalog.editor_dirty &&
        has_selection);
    add_button_->setEnabled(!catalog.loading && !catalog.editor_save_in_flight);
    reload_button_->setEnabled(!catalog.editor_save_in_flight);
    if (!catalog.editor_validation_error.empty()) {
        error_->setText(QString::fromStdString(catalog.editor_validation_error));
        error_->setVisible(true);
    }
    const bool can_close = !catalog.editor_dirty && !catalog.editor_save_in_flight;
    close_button_->setEnabled(true);
    close_button_->setToolTip(can_close ? QString() : QStringLiteral(
        "Save, revert, or delete the edited user configuration before closing."));
}

AidaDeleteConfigurationDialog::AidaDeleteConfigurationDialog(QWidget* parent)
    : bridge::AidaDialog(parent) {
    setObjectName(QStringLiteral("aida.dialog.programming.task.configuration.delete"));
    setWindowTitle(QStringLiteral("Delete Task Configuration"));
    resize(520, 220);
    setMinimumSize(400, 200);
    auto* column = new QVBoxLayout(this);
    auto* title = new QLabel(QStringLiteral("Delete this user task configuration?"), this);
    column->addWidget(title);
    auto* message = new QLabel(QStringLiteral(
        "This removes only the saved configuration. Existing output and diagnostics remain."), this);
    message->setWordWrap(true);
    column->addWidget(message);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("aida.dialog.programming.task.configuration.delete.error"));
    error_->setWordWrap(true);
    error_->setProperty("aidaVariant", "error");
    error_->setVisible(false);
    column->addWidget(error_);
    column->addStretch(1);
    auto* footer = new QHBoxLayout();
    footer->addStretch(1);
    delete_button_ = new QPushButton(QStringLiteral("Delete Configuration"), this);
    delete_button_->setObjectName(QStringLiteral("aida.dialog.programming.task.configuration.delete.confirm"));
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
    footer->addWidget(delete_button_);
    footer->addWidget(cancel);
    column->addLayout(footer);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(delete_button_, &QPushButton::clicked, this, [this] {
        const auto result = AidaProgrammingTasksController::instance().deleteSelected();
        if (result.succeeded) {
            accept();
        } else {
            error_->setText(result.detail.empty()
                ? QStringLiteral("The configuration could not be deleted")
                : QString::fromStdString(result.detail));
            error_->setVisible(true);
        }
    });
    const auto& catalog = AidaProgrammingTasksController::instance().catalog();
    const bool user_selected = catalog.editor_selected >= 0 &&
        catalog.editor_selected < static_cast<int>(catalog.configurations.size()) &&
        catalog.configurations[static_cast<std::size_t>(catalog.editor_selected)].origin ==
            tasks::configuration_origin_t::user;
    delete_button_->setEnabled(user_selected && !catalog.editor_save_in_flight);
}

AidaRunReviewDialog::AidaRunReviewDialog(QWidget* parent) : bridge::AidaDialog(parent) {
    setObjectName(QStringLiteral("aida.dialog.programming.task.run.review"));
    setWindowTitle(QStringLiteral("Review Programming Run"));
    resize(680, 390);
    setMinimumSize(500, 320);
    auto* column = new QVBoxLayout(this);
    title_label_ = new QLabel(this);
    title_label_->setObjectName(QStringLiteral("aida.dialog.programming.task.run.review.title"));
    column->addWidget(title_label_);
    command_ = new QLabel(this);
    command_->setObjectName(QStringLiteral("aida.dialog.programming.task.run.review.command"));
    command_->setWordWrap(true);
    command_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    command_->setFont(theme::fonts::codeRegular());
    column->addWidget(command_);
    workdir_ = new QLabel(this);
    workdir_->setObjectName(QStringLiteral("aida.dialog.programming.task.run.review.cwd"));
    workdir_->setWordWrap(true);
    column->addWidget(workdir_);
    channel_ = new QLabel(this);
    channel_->setObjectName(QStringLiteral("aida.dialog.programming.task.run.review.channel"));
    column->addWidget(channel_);
    matcher_ = new QLabel(this);
    matcher_->setObjectName(QStringLiteral("aida.dialog.programming.task.run.review.matcher"));
    column->addWidget(matcher_);
    auto* explainer = new QLabel(QStringLiteral(
        "The process and its descendants run outside AiDA's debugger. Cancel/close terminates the complete process tree. This is separate from RE Run Target."), this);
    explainer->setEnabled(false);
    explainer->setWordWrap(true);
    column->addWidget(explainer);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("aida.dialog.programming.task.run.review.error"));
    error_->setWordWrap(true);
    error_->setProperty("aidaVariant", "error");
    error_->setVisible(false);
    column->addWidget(error_);
    column->addStretch(1);
    auto* footer = new QHBoxLayout();
    footer->addStretch(1);
    run_button_ = new QPushButton(QStringLiteral("Run"), this);
    run_button_->setObjectName(QStringLiteral("aida.dialog.programming.task.run.review.run"));
    run_button_->setDefault(true);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
    footer->addWidget(run_button_);
    footer->addWidget(cancel);
    column->addLayout(footer);
    connect(cancel, &QPushButton::clicked, this, [this] {
        tasks::clear_pending_run();
        reject();
    });
    connect(run_button_, &QPushButton::clicked, this, [this] {
        const auto pending = tasks::pending_run_snapshot();
        if (!pending) {
            error_->setText(QStringLiteral("The selected configuration is no longer available."));
            error_->setVisible(true);
            run_button_->setEnabled(false);
            return;
        }
        const auto result = tasks::start_run(*pending);
        if (result.succeeded) {
            tasks::clear_pending_run();
            accept();
        } else {
            error_->setText(QString::fromStdString(result.detail));
            error_->setVisible(true);
        }
    });
}

void AidaRunReviewDialog::openForPending() {
    const auto pending = tasks::pending_run_snapshot();
    if (!pending) {
        title_label_->setText(QStringLiteral("The selected configuration is no longer available."));
        run_button_->setEnabled(false);
    } else {
        title_label_->setText(QStringLiteral("%1: %2")
            .arg(pending->source.kind == tasks::configuration_kind_t::launch
                ? QStringLiteral("Launch") : pending->source.kind == tasks::configuration_kind_t::test
                ? QStringLiteral("Test") : QStringLiteral("Task"),
                QString::fromStdString(pending->source.name)));
        command_->setText(QStringLiteral("Command: %1")
            .arg(QString::fromStdString(pending->command)));
        workdir_->setText(QStringLiteral("Working directory: %1")
            .arg(pending->cwd.empty() ? QStringLiteral("Inherited from AiDA")
                                      : QString::fromStdString(pending->cwd)));
        channel_->setText(QStringLiteral("Output channel: %1")
            .arg(QString::fromStdString(pending->channel)));
        matcher_->setText(QStringLiteral("Problem matcher: %1")
            .arg(QString::fromStdString(pending->source.problem_matcher)));
        run_button_->setEnabled(true);
    }
    open();
}

}
