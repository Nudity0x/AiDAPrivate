#include "qt/dialogs/aida_process_attach_dialog.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QCloseEvent>
#include <QColor>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/infra/executor.hpp"
#include "core/session/analysis_session.hpp"
#include "core/ui/task_center.hpp"
#include "core/ui/ui_thread_dispatcher.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_line_edit.hpp"

namespace aida::qt::dialogs {

namespace {

process_attach_hooks_t& attach_hooks()
{
    static process_attach_hooks_t hooks;
    return hooks;
}

void focus_view(const char* view_id)
{
    if (attach_hooks().focus_view) {
        attach_hooks().focus_view(view_id);
        return;
    }
    diag::log_tagged_fmt("attach", "focus_hook_missing view=%s", view_id ? view_id : "<null>");
}

void push_output(const std::string& text)
{
    if (attach_hooks().push_output_line) {
        attach_hooks().push_output_line(text);
        return;
    }
    diag::log_tagged_fmt("attach", "output_hook_missing len=%llu",
        static_cast<unsigned long long>(text.size()));
}

struct process_attach_request_t final {
    std::uint64_t generation = 0;
    std::uint64_t selection_epoch = 0;
    std::uint32_t pid = 0;
    std::string process_name;
    std::string process_path;
    std::string window_title;
};

struct process_attach_result_t final {
    process_attach_request_t request;
    bool attached = false;
    bool cancelled = false;
    bool cancellation_after_commit = false;
    bool module_available = false;
    std::string session_id;
    std::string detail;
    std::string module_name;
    std::uint64_t module_base = 0;
    std::uint64_t module_size = 0;
};

struct process_attach_operation_t final {
    process_attach_request_t request;
    std::string task_id;
    std::shared_ptr<aida::analysis::cancellation_source_t> cancellation;
    std::function<bool()> selection_is_current;
    std::function<void()> close_dialog;
    std::atomic<bool> commit_reached{false};
    std::atomic<bool> cancellation_after_commit{false};
    std::atomic<bool> terminal{false};
};

std::mutex g_process_attach_operation_mutex;
std::shared_ptr<process_attach_operation_t> g_process_attach_operation;
std::atomic<std::uint64_t> g_process_attach_generation{0};

bool process_attach_operation_active()
{
    std::lock_guard<std::mutex> lock(g_process_attach_operation_mutex);
    return g_process_attach_operation &&
        !g_process_attach_operation->terminal.load(std::memory_order_acquire);
}

void clear_process_attach_operation(
    const std::shared_ptr<process_attach_operation_t>& operation)
{
    std::lock_guard<std::mutex> lock(g_process_attach_operation_mutex);
    if (g_process_attach_operation == operation)
        g_process_attach_operation.reset();
}

void raise_process_attach_diagnostic(
    const std::shared_ptr<process_attach_operation_t>& operation,
    aida::ui::task_center::diagnostic_severity_t severity,
    std::string summary,
    std::string details)
{
    aida::ui::task_center::diagnostic_registration_t diagnostic;
    diagnostic.id = operation->task_id + ".diagnostic";
    diagnostic.task_id = operation->task_id;
    diagnostic.owner = "Process Attach";
    diagnostic.target = "PID " + std::to_string(operation->request.pid);
    diagnostic.summary = std::move(summary);
    diagnostic.details = std::move(details);
    diagnostic.severity = severity;
    diagnostic.callbacks.focus = [] {
        static_cast<void>(aida::ui_thread::post([] {
            focus_view("view.diagnostics");
        }, "process_attach", "focus_diagnostic", "task_center_callback"));
    };
    static_cast<void>(aida::ui::task_center::raise_diagnostic(std::move(diagnostic)));
}

void complete_process_attach_on_ui(
    const std::shared_ptr<process_attach_operation_t>& operation,
    const std::shared_ptr<const process_attach_result_t>& result)
{
    if (!aida::ui_thread::require_owner(
        "process_attach", "publish_completion", "executor_completion")) {
        if (!operation->terminal.exchange(true, std::memory_order_acq_rel)) {
            const auto terminal_state = result->cancelled
                ? aida::ui::task_center::task_state_t::cancelled
                : result->attached
                    ? aida::ui::task_center::task_state_t::partial
                    : aida::ui::task_center::task_state_t::failed;
            static_cast<void>(aida::ui::task_center::update_task(
                operation->task_id, terminal_state, 1.0f,
                "UI ownership fence rejected completion",
                result->attached
                    ? "The attach committed, but its UI completion was rejected"
                    : result->cancelled
                        ? "The attach was cancelled and rolled back"
                        : "The attach failed before its UI completion was rejected"));
        }
        clear_process_attach_operation(operation);
        return;
    }

    const std::uint64_t current_generation =
        g_process_attach_generation.load(std::memory_order_acquire);
    bool operation_is_current = false;
    {
        std::lock_guard<std::mutex> lock(g_process_attach_operation_mutex);
        operation_is_current = g_process_attach_operation == operation;
    }
    const bool generation_is_current = result->request.generation != 0 &&
        result->request.generation == current_generation &&
        result->request.generation == operation->request.generation;
    const bool selection_is_current = operation->selection_is_current &&
        operation->selection_is_current();

    const bool session_target_is_current = !result->attached ||
        analysis_session::active_live_session_matches(
            result->request.pid, result->session_id);

    if (operation->terminal.exchange(true, std::memory_order_acq_rel)) {
        clear_process_attach_operation(operation);
        return;
    }

    if (result->cancelled) {
        push_output("[Driver] Attach to PID " + std::to_string(result->request.pid) +
            " was cancelled and rolled back.\n");
        static_cast<void>(aida::ui::task_center::update_task(
            operation->task_id,
            aida::ui::task_center::task_state_t::cancelled,
            1.0f,
            "Cancelled and rolled back",
            "No reviewed attach transaction was committed"));
    } else if (!result->attached) {
        const std::string detail = result->detail.empty()
            ? "Attach failed without diagnostic detail" : result->detail;
        push_output("[Driver] Failed to attach to PID " + std::to_string(result->request.pid) +
            ": " + detail + "\n");
        static_cast<void>(aida::ui::task_center::update_task(
            operation->task_id,
            aida::ui::task_center::task_state_t::failed,
            1.0f,
            "Attach failed",
            detail,
            operation->task_id + ".diagnostic"));
        raise_process_attach_diagnostic(operation,
            aida::ui::task_center::diagnostic_severity_t::error,
            "Process attach failed", detail);
    } else if (result->cancellation_after_commit ||
        operation->cancellation_after_commit.load(std::memory_order_acquire)) {
        push_output("[Driver] Attach to PID " + std::to_string(result->request.pid) +
            " committed before cancellation was requested.\n");
        static_cast<void>(aida::ui::task_center::update_task(
            operation->task_id,
            aida::ui::task_center::task_state_t::completed,
            1.0f,
            "Attach committed before cancellation request",
            "The active session was retained because the transaction had crossed its commit boundary"));
    } else if (!operation_is_current || !generation_is_current ||
        !selection_is_current || !session_target_is_current) {
        std::string reason;
        if (!operation_is_current) reason = "operation ownership changed";
        else if (!generation_is_current) reason = "request generation changed";
        else if (!selection_is_current) reason = "reviewed process selection changed";
        else reason = "active session target changed";
        push_output("[Driver] Attached to PID " + std::to_string(result->request.pid) +
            ", but discarded stale UI completion: " + reason + ".\n");
        static_cast<void>(aida::ui::task_center::update_task(
            operation->task_id,
            aida::ui::task_center::task_state_t::partial,
            1.0f,
            "Attach committed; stale UI completion discarded",
            reason));
    } else {
        if (!result->module_available) {
            push_output("[Driver] Attached to PID " + std::to_string(result->request.pid) +
                " but could not enumerate modules.\n");
        }
        focus_view("document.disassembly");
        static_cast<void>(aida::ui::task_center::update_task(
            operation->task_id,
            aida::ui::task_center::task_state_t::completed,
            1.0f,
            result->module_available
                ? "Attached; workspace module resolved"
                : "Attached; module list unavailable",
            "PID " + std::to_string(result->request.pid) + " is the active session target"));
    }

    if (operation->close_dialog)
        operation->close_dialog();
    clear_process_attach_operation(operation);
}

void run_process_attach(const std::shared_ptr<process_attach_operation_t>& operation)
{
    diag::log_tagged_critical_fmt("attach",
        "worker_enter generation=%llu selection_epoch=%llu pid=%u name=%s path=%s",
        static_cast<unsigned long long>(operation->request.generation),
        static_cast<unsigned long long>(operation->request.selection_epoch),
        operation->request.pid, operation->request.process_name.c_str(),
        operation->request.process_path.c_str());
    driver_bridge::debug_log("ATTACH: attempting pid=%u name=%s\n",
        operation->request.pid, operation->request.process_name.c_str());
    static_cast<void>(aida::ui::task_center::update_task(
        operation->task_id,
        aida::ui::task_center::task_state_t::running,
        0.05f,
        "Validating reviewed process identity"));

    auto result = std::make_shared<process_attach_result_t>();
    result->request = operation->request;
    try {
        std::string session_error;
        result->attached = analysis_session::open_attach_session(
            operation->request.pid, &session_error, operation->cancellation->token());
        result->detail = std::move(session_error);
        result->cancelled = !result->attached &&
            operation->cancellation->token().stop_requested();
        diag::log_tagged_critical_fmt("attach",
            "session_transaction generation=%llu pid=%u attached=%d cancelled=%d detail=%s",
            static_cast<unsigned long long>(operation->request.generation),
            operation->request.pid, result->attached ? 1 : 0,
            result->cancelled ? 1 : 0, result->detail.c_str());
        if (result->attached) {
            operation->commit_reached.store(true, std::memory_order_release);
            if (operation->cancellation->token().stop_requested())
                operation->cancellation_after_commit.store(true, std::memory_order_release);
            std::size_t session_index = 0;
            if (analysis_session::find_session_by_pid(operation->request.pid, &session_index))
                result->session_id = analysis_session::summarize_session_at(session_index).id;
            static_cast<void>(aida::ui::task_center::update_task(
                operation->task_id,
                aida::ui::task_center::task_state_t::running,
                0.78f,
                "Attach committed; resolving target module"));
            auto modules = driver_bridge::enumerate_modules();
            diag::log_tagged_critical_fmt("attach",
                "module_snapshot generation=%llu pid=%u count=%llu",
                static_cast<unsigned long long>(operation->request.generation),
                operation->request.pid,
                static_cast<unsigned long long>(modules.size()));
            if (!modules.empty()) {
                std::string reviewed_name = operation->request.process_name;
                for (char& character : reviewed_name)
                    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                const auto* target_module = &modules.front();
                for (const auto& module : modules) {
                    std::string module_name = module.name;
                    for (char& character : module_name)
                        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                    if (module_name == reviewed_name) {
                        target_module = &module;
                        break;
                    }
                }
                result->module_available = true;
                result->module_name = target_module->name;
                result->module_base = target_module->base;
                result->module_size = target_module->size == 0
                    ? 0x100000 : target_module->size;
                driver_bridge::debug_log(
                    "ATTACH: workspace snapshot pid=%u base=0x%llX size=0x%llX mod=%s\n",
                    operation->request.pid,
                    static_cast<unsigned long long>(result->module_base),
                    static_cast<unsigned long long>(result->module_size),
                    result->module_name.c_str());
            }
        }
    } catch (const std::exception& exception) {
        if (!result->attached)
            result->detail = exception.what();
        diag::log_tagged_critical_fmt("attach",
            "worker_exception pid=%u detail=%s",
            operation->request.pid, exception.what());
    } catch (...) {
        if (!result->attached)
            result->detail = "Unknown exception during process attach";
        diag::log_tagged_critical_fmt("attach",
            "worker_exception pid=%u detail=unknown", operation->request.pid);
    }

    static_cast<void>(aida::ui::task_center::update_task(
        operation->task_id,
        result->cancelled
            ? aida::ui::task_center::task_state_t::cancellation_requested
            : aida::ui::task_center::task_state_t::running,
        0.95f,
        result->cancelled
            ? "Rollback complete; publishing cancellation"
            : "Publishing immutable attach completion"));
    result->cancellation_after_commit =
        operation->cancellation_after_commit.load(std::memory_order_acquire);
    const std::shared_ptr<const process_attach_result_t> immutable_result = result;
    const bool posted = aida::ui_thread::post(
        [operation, immutable_result] {
            complete_process_attach_on_ui(operation, immutable_result);
        }, "process_attach", "publish_completion", "executor_completion");
    if (posted)
        return;
    diag::log_tagged_critical_fmt("attach",
        "ui_dispatch_failed generation=%llu pid=%u attached=%d cancelled=%d",
        static_cast<unsigned long long>(operation->request.generation),
        operation->request.pid, result->attached ? 1 : 0,
        result->cancelled ? 1 : 0);

    if (!operation->terminal.exchange(true, std::memory_order_acq_rel)) {
        if (result->cancelled) {
            static_cast<void>(aida::ui::task_center::update_task(
                operation->task_id,
                aida::ui::task_center::task_state_t::cancelled,
                1.0f,
                "Cancelled and rolled back; UI dispatch unavailable",
                "No reviewed attach transaction was committed"));
        } else if (result->attached) {
            static_cast<void>(aida::ui::task_center::update_task(
                operation->task_id,
                aida::ui::task_center::task_state_t::partial,
                1.0f,
                "Attach committed; UI dispatch failed",
                "The active session is valid, but its UI completion could not be delivered"));
            raise_process_attach_diagnostic(operation,
                aida::ui::task_center::diagnostic_severity_t::warning,
                "Attach UI completion was not delivered",
                "The process attach committed before the UI dispatcher rejected publication");
        } else {
            const std::string detail = result->detail.empty()
                ? "Attach failed and UI completion dispatch was unavailable" : result->detail;
            static_cast<void>(aida::ui::task_center::update_task(
                operation->task_id,
                aida::ui::task_center::task_state_t::failed,
                1.0f,
                "Attach failed; UI dispatch unavailable",
                detail,
                operation->task_id + ".diagnostic"));
            raise_process_attach_diagnostic(operation,
                aida::ui::task_center::diagnostic_severity_t::error,
                "Process attach failed", detail);
        }
    }
    clear_process_attach_operation(operation);
}

}

void set_process_attach_hooks(process_attach_hooks_t hooks)
{
    attach_hooks() = std::move(hooks);
}

bool process_attach_active()
{
    return process_attach_operation_active();
}

namespace {

bool begin_process_attach(process_attach_request_t request,
    std::function<bool()> selection_is_current,
    std::function<void()> close_dialog,
    std::string& error)
{
    auto operation = std::make_shared<process_attach_operation_t>();
    {
        std::lock_guard<std::mutex> lock(g_process_attach_operation_mutex);
        if (g_process_attach_operation &&
            !g_process_attach_operation->terminal.load(std::memory_order_acquire)) {
            error = "A process attach transaction is already active";
            return false;
        }
        request.generation = g_process_attach_generation.fetch_add(
            1, std::memory_order_acq_rel) + 1;
        operation->request = std::move(request);
        operation->task_id = "process.attach." +
            std::to_string(operation->request.pid) + "." +
            std::to_string(operation->request.generation);
        operation->cancellation =
            std::make_shared<aida::analysis::cancellation_source_t>();
        operation->selection_is_current = std::move(selection_is_current);
        operation->close_dialog = std::move(close_dialog);
        g_process_attach_operation = operation;
    }

    aida::ui::task_center::task_registration_t registration;
    registration.id = operation->task_id;
    registration.source = "process_attach_dialog";
    registration.owner = "Process Attach";
    registration.owner_view = "document.disassembly";
    registration.owner_action = "session.attach_process";
    registration.target = "PID " + std::to_string(operation->request.pid);
    const std::string reviewed_label = !operation->request.process_name.empty()
        ? operation->request.process_name
        : !operation->request.window_title.empty()
            ? operation->request.window_title : registration.target;
    registration.label = "Attach to " + reviewed_label;
    registration.stage = "Reviewed request queued";
    registration.affected_entity = operation->request.process_path.empty()
        ? registration.target : operation->request.process_path;
    registration.progress = 0.0f;
    registration.cancellation_is_safe = true;
    registration.callbacks.cancel = [weak = std::weak_ptr<process_attach_operation_t>(operation)] {
        const auto current = weak.lock();
        if (!current || current->terminal.load(std::memory_order_acquire))
            return false;
        if (current->commit_reached.load(std::memory_order_acquire))
            current->cancellation_after_commit.store(true, std::memory_order_release);
        else
            current->cancellation->request_cancel();
        return true;
    };
    registration.callbacks.focus = [] {
        static_cast<void>(aida::ui_thread::post([] {
            focus_view("view.background_tasks");
        }, "process_attach", "focus_task", "task_center_callback"));
    };
    if (!aida::ui::task_center::register_task(std::move(registration))) {
        error = "The Task Center rejected process attach ownership";
        operation->terminal.store(true, std::memory_order_release);
        clear_process_attach_operation(operation);
        return false;
    }

    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "process_attach";
    submission.label = "process_attach.transaction";
    submission.thread_class = "driver_session_transaction";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 2;
    submission.target_pid = operation->request.pid;
    submission.generation = operation->request.generation;
    submission.ui_access_policy = "immutable_snapshots_only";
    submission.failure_policy = "typed_diagnostic";
    submission.shutdown_policy = "cancel";
    submission.cancel_hook = [weak = std::weak_ptr<process_attach_operation_t>(operation)] {
        if (const auto current = weak.lock())
            current->cancellation->request_cancel();
    };
    submission.body = [operation] { run_process_attach(operation); };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (submitted.submitted)
        return true;

    error = submitted.reject_reason.empty()
        ? "The process attach executor rejected dispatch" : submitted.reject_reason;
    operation->terminal.store(true, std::memory_order_release);
    static_cast<void>(aida::ui::task_center::update_task(
        operation->task_id,
        aida::ui::task_center::task_state_t::failed,
        1.0f,
        "Dispatch rejected before execution",
        error,
        operation->task_id + ".diagnostic"));
    raise_process_attach_diagnostic(operation,
        aida::ui::task_center::diagnostic_severity_t::error,
        "Process attach dispatch was rejected", error);
    clear_process_attach_operation(operation);
    return false;
}

bool request_active_process_attach_cancel()
{
    std::shared_ptr<process_attach_operation_t> operation;
    {
        std::lock_guard<std::mutex> lock(g_process_attach_operation_mutex);
        operation = g_process_attach_operation;
    }
    if (!operation || operation->terminal.load(std::memory_order_acquire))
        return false;
    return aida::ui::task_center::request_cancel(operation->task_id);
}

}

AidaProcessTableModel::AidaProcessTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int AidaProcessTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(filtered_.size());
}

int AidaProcessTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant AidaProcessTableModel::headerData(int section, Qt::Orientation orientation,
                                           int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case PidColumn: return QStringLiteral("PID");
    case NameColumn: return QStringLiteral("Name");
    case WindowTitleColumn: return QStringLiteral("Window Title");
    }
    return {};
}

QVariant AidaProcessTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(filtered_.size()))
        return {};
    const auto& process = processes_[static_cast<std::size_t>(
        filtered_[static_cast<std::size_t>(index.row())])];
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case PidColumn:
            return static_cast<unsigned long long>(process.pid);
        case NameColumn:
            return QString::fromStdString(process.name);
        case WindowTitleColumn:
            if (!process.window_title.empty())
                return QString::fromStdString(process.window_title);
            if (!process.path.empty()) {
                const auto slash = process.path.find_last_of("\\/");
                return QString::fromStdString(slash == std::string::npos
                    ? process.path : process.path.substr(0, slash));
            }
            return {};
        }
        return {};
    case Qt::ForegroundRole:
        if (index.column() == NameColumn)
            return process.window_title.empty()
                ? QColor(theme::tokens().text_secondary)
                : QColor(theme::tokens().text_primary);
        if (index.column() == WindowTitleColumn)
            return process.window_title.empty()
                ? QColor(theme::tokens().text_dim)
                : QColor(theme::tokens().accent);
        return {};
    }
    return {};
}

void AidaProcessTableModel::applySnapshot(
    std::vector<driver_bridge::process_info_t> processes, std::uint64_t epoch)
{
    beginResetModel();
    processes_ = std::move(processes);
    applied_epoch_ = epoch;
    rebuildFiltered();
    endResetModel();
}

void AidaProcessTableModel::setFilter(const QString& filter)
{
    const std::string lowered = [&] {
        std::string text = filter.toStdString();
        for (auto& c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }();
    if (lowered == filter_)
        return;
    filter_ = lowered;
    rebuildFiltered();
    Q_EMIT layoutChanged();
}

const driver_bridge::process_info_t* AidaProcessTableModel::processAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(filtered_.size()))
        return nullptr;
    return &processes_[static_cast<std::size_t>(filtered_[static_cast<std::size_t>(row)])];
}

int AidaProcessTableModel::rowForPid(std::uint32_t pid) const
{
    for (std::size_t i = 0; i < filtered_.size(); ++i)
        if (processes_[static_cast<std::size_t>(filtered_[i])].pid == pid)
            return static_cast<int>(i);
    return -1;
}

void AidaProcessTableModel::rebuildFiltered()
{
    if (cached_filter_epoch_ == applied_epoch_ && cached_filter_ == filter_)
        return;
    cached_filter_ = filter_;
    cached_filter_epoch_ = applied_epoch_;
    filtered_.clear();
    filtered_.reserve(processes_.size());
    for (std::size_t i = 0; i < processes_.size(); ++i) {
        const auto& process = processes_[i];
        bool matches = filter_.empty();
        if (!matches) {
            std::string searchable = process.name + "\n" + process.window_title + "\n" +
                process.path + "\n" + std::to_string(process.pid);
            for (auto& character : searchable)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            matches = searchable.find(filter_) != std::string::npos;
        }
        if (matches)
            filtered_.push_back(static_cast<int>(i));
    }
}

AidaProcessAttachDialog::AidaProcessAttachDialog(QWidget* parent)
    : bridge::AidaDialog(parent)
{
    setObjectName(QStringLiteral("aida.process_attach"));
    setWindowTitle(QStringLiteral("Attach to Process"));
    setModal(false);

    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
                             t.panel.padding);
    root->setSpacing(t.spacing.sm);

    filter_edit_ = new widgets::AidaLineEdit(QStringLiteral("Search processes..."), this);
    filter_edit_->setObjectName(QStringLiteral("aida.process_attach.filter"));
    filter_edit_->setClearButtonEnabled(true);
    bridge::InteractionContextProvider::mark_text_input(filter_edit_);
    connect(filter_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        model_->setFilter(text);
        updateStatusLine();
        if (!table_->currentIndex().isValid() && model_->rowCount() > 0)
            table_->setCurrentIndex(model_->index(0, AidaProcessTableModel::PidColumn));
    });
    root->addWidget(filter_edit_);

    model_ = new AidaProcessTableModel(this);
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("aida.process_attach.table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    const QFontMetricsF header_fm(theme::fonts::body());
    const int cell_frame = 2 * (t.table.cell_pad_x + t.panel.border);
    table_->horizontalHeader()->setSectionResizeMode(AidaProcessTableModel::PidColumn,
        QHeaderView::Fixed);
    table_->setColumnWidth(AidaProcessTableModel::PidColumn,
        static_cast<int>(header_fm.horizontalAdvance(QStringLiteral("99999999"))) + cell_frame);
    table_->horizontalHeader()->setSectionResizeMode(AidaProcessTableModel::NameColumn,
        QHeaderView::Fixed);
    table_->setColumnWidth(AidaProcessTableModel::NameColumn,
        static_cast<int>(header_fm.horizontalAdvance(
            QStringLiteral("application_frame_host.exe"))) + cell_frame);
    table_->horizontalHeader()->setSectionResizeMode(AidaProcessTableModel::WindowTitleColumn,
        QHeaderView::Stretch);
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (index.isValid())
            onAttachClicked();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        attach_button_->setEnabled(model_->processAt(current.row()) != nullptr);
    });
    root->addWidget(table_, 1);

    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.process_attach.status"));
    status_label_->setFont(theme::fonts::caption());
    status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    root->addWidget(status_label_);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    attach_button_ = new QPushButton(QStringLiteral("Attach"), this);
    attach_button_->setObjectName(QStringLiteral("aida.process_attach.confirm"));
    attach_button_->setEnabled(false);
    attach_button_->setDefault(true);
    connect(attach_button_, &QPushButton::clicked, this, [this] { onAttachClicked(); });
    buttons->addWidget(attach_button_);
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_button_->setObjectName(QStringLiteral("aida.process_attach.cancel"));
    connect(cancel_button_, &QPushButton::clicked, this, [this] { requestClose(); });
    buttons->addWidget(cancel_button_);
    root->addLayout(buttons);

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(2000);
    refresh_timer_->setTimerType(Qt::CoarseTimer);
    connect(refresh_timer_, &QTimer::timeout, this, [this] { onRefreshFired(); });

    apply_timer_ = new QTimer(this);
    apply_timer_->setInterval(50);
    apply_timer_->setTimerType(Qt::CoarseTimer);
    connect(apply_timer_, &QTimer::timeout, this, [this] { applyPendingIfReady(); });

    setMinimumSize(420, 320);
    resize(620, 490);
}

AidaProcessAttachDialog::~AidaProcessAttachDialog()
{
    refresh_timer_->stop();
    apply_timer_->stop();
}

void AidaProcessAttachDialog::openFresh()
{
    closing_ = false;
    filter_edit_->clear();
    model_->setFilter(QString());
    updateStatusLine();
    refresh_timer_->start();
    apply_timer_->start();
    scheduleRefresh(true);
    open();
    filter_edit_->setFocus(Qt::OtherFocusReason);
    connect(this, &QDialog::finished, this, [this](int) {
        refresh_timer_->stop();
        apply_timer_->stop();
    }, Qt::SingleShotConnection);
}

void AidaProcessAttachDialog::updateStatusLine()
{
    const int shown = model_->rowCount();
    const int total = model_->totalCount();
    if (model_->appliedEpoch() == 0 && total == 0) {
        status_label_->setText(QStringLiteral("Enumerating processes..."));
        return;
    }
    if (total == 0) {
        status_label_->setText(QStringLiteral("No processes were reported by the driver."));
        return;
    }
    if (model_->filtering()) {
        status_label_->setText(shown == 0
            ? QStringLiteral("No processes match the filter.")
            : QStringLiteral("%1 of %2 processes match").arg(shown).arg(total));
        return;
    }
    status_label_->setText(QStringLiteral("%1 processes").arg(total));
}

void AidaProcessAttachDialog::scheduleRefresh(bool immediate)
{
    Q_UNUSED(immediate);
    onRefreshFired();
}

namespace {

struct pending_snapshot_t {
    std::mutex mutex;
    std::vector<driver_bridge::process_info_t> processes;
    std::uint64_t epoch = 0;
};

pending_snapshot_t& pending_snapshot()
{
    static pending_snapshot_t value;
    return value;
}

std::atomic<bool> g_enumerate_inflight{false};
std::atomic<bool> g_enumerate_ready{false};
std::uint64_t g_enumerate_epoch = 0;

}

void AidaProcessAttachDialog::onRefreshFired()
{
    if (process_attach_operation_active())
        return;
    if (g_enumerate_inflight.exchange(true, std::memory_order_acq_rel))
        return;
    const std::uint64_t epoch = ++g_enumerate_epoch;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "process_attach";
    submission.label = "process_attach.enumerate_processes";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.body = [epoch]() {
        std::vector<driver_bridge::process_info_t> list;
        try {
            list = driver_bridge::enumerate_processes();
        } catch (...) {
            OutputDebugStringA("AiDA Standalone: EXCEPTION in enumerate_processes()\n");
        }
        {
            std::lock_guard<std::mutex> lock(pending_snapshot().mutex);
            pending_snapshot().processes = std::move(list);
            pending_snapshot().epoch = epoch;
        }
        g_enumerate_ready.store(true, std::memory_order_release);
        g_enumerate_inflight.store(false, std::memory_order_release);
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted)
        g_enumerate_inflight.store(false, std::memory_order_release);
}

void AidaProcessAttachDialog::applyPendingIfReady()
{
    if (process_attach_operation_active())
        return;
    if (!g_enumerate_ready.exchange(false, std::memory_order_acq_rel))
        return;
    std::vector<driver_bridge::process_info_t> snapshot;
    std::uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(pending_snapshot().mutex);
        if (pending_snapshot().epoch <= model_->appliedEpoch())
            return;
        snapshot = std::move(pending_snapshot().processes);
        epoch = pending_snapshot().epoch;
    }
    std::uint32_t selected_pid = 0;
    if (const auto* current = model_->processAt(table_->currentIndex().row()))
        selected_pid = current->pid;
    model_->applySnapshot(std::move(snapshot), epoch);
    if (selected_pid != 0) {
        const int row = model_->rowForPid(selected_pid);
        if (row >= 0)
            table_->setCurrentIndex(model_->index(row, AidaProcessTableModel::PidColumn));
    }
    if (!table_->currentIndex().isValid() && model_->rowCount() > 0)
        table_->setCurrentIndex(model_->index(0, AidaProcessTableModel::PidColumn));
    attach_button_->setEnabled(model_->processAt(table_->currentIndex().row()) != nullptr);
    updateStatusLine();
}

void AidaProcessAttachDialog::onAttachClicked()
{
    const auto* process = model_->processAt(table_->currentIndex().row());
    if (!process || process_attach_operation_active())
        return;
    const std::uint64_t reviewed_epoch = model_->appliedEpoch();
    const auto reviewed = *process;
    process_attach_request_t request;
    request.selection_epoch = reviewed_epoch;
    request.pid = reviewed.pid;
    request.process_name = reviewed.name;
    request.process_path = reviewed.path;
    request.window_title = reviewed.window_title;
    std::string dispatch_error;
    const std::uint32_t reviewed_pid = reviewed.pid;
    const bool queued = begin_process_attach(
        std::move(request),
        [this, reviewed_epoch, reviewed_pid] {
            if (closing_)
                return false;
            if (model_->appliedEpoch() != reviewed_epoch)
                return false;
            const auto* current = model_->processAt(table_->currentIndex().row());
            return current && current->pid == reviewed_pid;
        },
        [this] { requestClose(); },
        dispatch_error);
    if (!queued) {
        push_output("[Driver] Attach request was not queued: " + dispatch_error + "\n");
        diag::log_tagged_critical_fmt("attach",
            "dispatch_rejected pid=%u detail=%s",
            reviewed_pid, dispatch_error.c_str());
    }
}

void AidaProcessAttachDialog::requestClose()
{
    if (closing_)
        return;
    closing_ = true;
    if (process_attach_operation_active())
        static_cast<void>(request_active_process_attach_cancel());
    refresh_timer_->stop();
    apply_timer_->stop();
    reject();
    closing_ = false;
}

void AidaProcessAttachDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        requestClose();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Down && filter_edit_->hasFocus() &&
        model_->rowCount() > 0) {
        table_->setFocus(Qt::OtherFocusReason);
        if (!table_->currentIndex().isValid())
            table_->setCurrentIndex(model_->index(0, AidaProcessTableModel::PidColumn));
        event->accept();
        return;
    }
    bridge::AidaDialog::keyPressEvent(event);
}

void AidaProcessAttachDialog::closeEvent(QCloseEvent* event)
{
    if (process_attach_operation_active())
        static_cast<void>(request_active_process_attach_cancel());
    bridge::AidaDialog::closeEvent(event);
}

}
