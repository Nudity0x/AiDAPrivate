#include "qt/workbench/qt_sessions_view.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "helpers/diag_log.hpp"

#include "core/infra/executor.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/task_center.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::workbench {

QtSessionsView::QtSessionsView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.sessions"));
    const auto& tokens = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.sessions.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x, tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.toolbar.group_gap);
    open_button_ = new QPushButton(QStringLiteral("Open Binary..."), toolbar);
    open_button_->setObjectName(QStringLiteral("aida.sessions.open"));
    open_button_->setToolTip(QStringLiteral(
        "Open a binary and create an analysis session"));
    attach_button_ = new QPushButton(QStringLiteral("Attach..."), toolbar);
    attach_button_->setObjectName(QStringLiteral("aida.sessions.attach"));
    attach_button_->setToolTip(QStringLiteral("Attach to a live process"));
    run_button_ = new QPushButton(QStringLiteral("Run..."), toolbar);
    run_button_->setObjectName(QStringLiteral("aida.sessions.run"));
    run_button_->setToolTip(QStringLiteral("Launch a target under the debugger"));
    reattach_button_ = new QPushButton(QStringLiteral("Reattach"), toolbar);
    reattach_button_->setObjectName(QStringLiteral("aida.sessions.reattach"));
    detach_button_ = new QPushButton(QStringLiteral("Detach"), toolbar);
    detach_button_->setObjectName(QStringLiteral("aida.sessions.detach"));
    close_button_ = new QPushButton(QStringLiteral("Close"), toolbar);
    close_button_->setObjectName(QStringLiteral("aida.sessions.close"));
    close_button_->setToolTip(QStringLiteral(
        "Close the selected session and release its workspace"));
    reattach_button_->setToolTip(QStringLiteral(
        "Revalidate the retained process identity and attach this session"));
    detach_button_->setToolTip(QStringLiteral(
        "Activate this exact live session and detach it without terminating the target"));
    toolbar_layout->addWidget(open_button_);
    toolbar_layout->addWidget(attach_button_);
    toolbar_layout->addWidget(run_button_);
    toolbar_layout->addSpacing(tokens.toolbar.group_gap);
    toolbar_layout->addWidget(reattach_button_);
    toolbar_layout->addWidget(detach_button_);
    toolbar_layout->addWidget(close_button_);
    toolbar_layout->addStretch(1);
    layout->addWidget(toolbar);
    model_ = new QtSessionsModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.sessions.table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(tokens.table.row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* horizontal = table_->horizontalHeader();
    horizontal->setSectionResizeMode(
        static_cast<int>(QtSessionsModel::Column::name), QHeaderView::Stretch);
    for (int column = static_cast<int>(QtSessionsModel::Column::kind);
         column < static_cast<int>(QtSessionsModel::Column::column_count); ++column)
        horizontal->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    horizontal->setStretchLastSection(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.sessions.state_view"));
    state_view_->setState(widgets::AidaStateView::State::Empty);
    state_view_->setTitle(QStringLiteral("No open sessions"));
    state_view_->setMessage(QStringLiteral(
        "Open a binary, attach to a process, or launch a target to create a session."));
    layout->addWidget(state_view_, 1);
    layout->addWidget(table_, 1);
    auto* feedback_host = new QWidget(this);
    feedback_host->setObjectName(QStringLiteral("aida.sessions.feedback_host"));
    auto* feedback_row = new QHBoxLayout(feedback_host);
    feedback_row->setContentsMargins(tokens.toolbar.padding_x, 0,
        tokens.toolbar.padding_x, tokens.toolbar.padding_y);
    feedback_row->setSpacing(tokens.toolbar.group_gap);
    feedback_label_ = new QLabel(feedback_host);
    feedback_label_->setObjectName(QStringLiteral("aida.sessions.feedback"));
    feedback_label_->setWordWrap(true);
    feedback_row->addWidget(feedback_label_, 1);
    feedback_dismiss_ = new QPushButton(QStringLiteral("Dismiss"), feedback_host);
    feedback_dismiss_->setObjectName(QStringLiteral("aida.sessions.feedback.dismiss"));
    feedback_dismiss_->setToolTip(QStringLiteral("Dismiss this message"));
    feedback_row->addWidget(feedback_dismiss_);
    layout->addWidget(feedback_host);
    feedback_label_->setVisible(false);
    feedback_dismiss_->setVisible(false);
    feedback_host->setVisible(false);

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, [this] { poll(); });

    connect(open_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(aida::ui::application_ui::execute_action(
            "tools.load_binary",
            aida::ui::action_invocation_source_t::toolbar));
    });
    connect(attach_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(aida::ui::application_ui::execute_action(
            "tools.attach_process",
            aida::ui::action_invocation_source_t::toolbar));
    });
    connect(run_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(aida::ui::application_ui::execute_action(
            "debugger.launch", aida::ui::action_invocation_source_t::toolbar));
    });
    connect(reattach_button_, &QPushButton::clicked, this, [this] {
        const auto index = table_->currentIndex();
        const auto* session = model_->rowAt(index.isValid() ? index.row() : -1);
        if (session) requestReattach(*session);
    });
    connect(detach_button_, &QPushButton::clicked, this, [this] {
        const auto index = table_->currentIndex();
        const auto* session = model_->rowAt(index.isValid() ? index.row() : -1);
        if (session) requestDetach(session->id);
    });
    connect(close_button_, &QPushButton::clicked, this, [this] {
        const auto index = table_->currentIndex();
        const auto* session = model_->rowAt(index.isValid() ? index.row() : -1);
        if (session) requestClose(session->id);
    });
    connect(feedback_dismiss_, &QPushButton::clicked, this, [this] {
        if (feedback_is_selection_error_)
            dismissed_error_key_ = selection_error_key_;
        feedback_is_selection_error_ = false;
        feedback_.clear();
        feedback_label_->setVisible(false);
        feedback_dismiss_->setVisible(false);
        if (auto* host = feedback_label_->parentWidget())
            host->setVisible(false);
    });
    connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid()) return;
        const int session_index = model_->sessionIndexFor(index.row());
        if (session_index < 0) return;
        analysis_session::switch_session(static_cast<std::size_t>(session_index));
        aida::qt::analysis::QtAnalysisBridge::instance().openView(
            "document.disassembly");
    });
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = table_->indexAt(pos);
        if (index.isValid())
            showRowMenu(table_->viewport()->mapToGlobal(pos), index.row());
    });
    poll();
}

void QtSessionsView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    timer_->start();
    poll();
}

void QtSessionsView::hideEvent(QHideEvent* event) {
    timer_->stop();
    QWidget::hideEvent(event);
}

void QtSessionsView::presentFeedback(bool dismiss_visible) {
    const QString text = QString::fromStdString(feedback_);
    const char* variant = feedback_error_ ? "error" : "info";
    if (feedback_label_->property("aidaVariant").toString() !=
        QLatin1String(variant)) {
        feedback_label_->setProperty("aidaVariant", QString::fromLatin1(variant));
        theme::stylesheet::repolish(feedback_label_);
    }
    feedback_label_->setText(text);
    feedback_label_->setVisible(!text.isEmpty());
    feedback_dismiss_->setVisible(dismiss_visible && !text.isEmpty());
    if (auto* host = feedback_label_->parentWidget())
        host->setVisible(!text.isEmpty());
}

void QtSessionsView::poll() {
    if (reattach_ && reattach_->completed.load(std::memory_order_acquire)) {
        const bool attached = reattach_->attached.load(std::memory_order_acquire);
        const bool cancelled = reattach_->cancelled.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(reattach_->result_mutex);
            feedback_ = cancelled
                ? "Reattach cancelled before commit." : reattach_->detail;
        }
        feedback_error_ = !attached && !cancelled;
        feedback_is_selection_error_ = false;
        diag::log_tagged_fmt("qt_sessions", "reattach_done attached=%d cancelled=%d %s",
            attached ? 1 : 0, cancelled ? 1 : 0, feedback_.c_str());
        reattach_.reset();
        presentFeedback(true);
    }
    const auto summaries = analysis_session::list_session_summaries();
    model_->setRows(summaries, analysis_session::active_session_idx(),
        driver_bridge::attached_pid());
    refreshPresentation();
}

void QtSessionsView::requestReattach(
    const analysis_session::session_summary_t& summary) {
    if (summary.kind != analysis_session::session_kind_t::live_attach ||
        summary.pid == 0 || reattach_)
        return;
    auto operation = std::make_shared<reattach_operation_t>();
    operation->pid = summary.pid;
    operation->process_creation_time_100ns = summary.process_creation_time_100ns;
    operation->session_id = summary.id;
    operation->cancellation =
        std::make_shared<aida::analysis::cancellation_source_t>();
    const std::uint32_t pid = operation->pid;
    const std::string session_id = operation->session_id;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "analysis_sessions";
    submission.label = "session.reattach";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 2;
    submission.body = [operation] {
        std::string detail;
        bool attached = false;
        try {
            attached = analysis_session::reattach_session_exact(
                operation->session_id, operation->pid,
                operation->process_creation_time_100ns, &detail,
                operation->cancellation->token());
        } catch (const std::exception& exception) {
            detail = exception.what();
        } catch (...) {
            detail = "Unknown exception while reattaching the session";
        }
        if (detail.empty())
            detail = attached ? "The live session is active."
                : "The session could not be reattached.";
        {
            std::lock_guard<std::mutex> lock(operation->result_mutex);
            operation->detail = std::move(detail);
        }
        operation->attached.store(attached, std::memory_order_release);
        operation->cancelled.store(!attached &&
            operation->cancellation->token().stop_requested(),
            std::memory_order_release);
        operation->completed.store(true, std::memory_order_release);
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted || submitted.task_id == 0) {
        feedback_ = submitted.reject_reason.empty()
            ? "The bounded executor rejected the reattach request."
            : submitted.reject_reason;
        feedback_error_ = true;
        feedback_is_selection_error_ = false;
        presentFeedback(true);
        return;
    }
    operation->task_id = submitted.task_id;
    reattach_ = operation;
    feedback_ = "Reattaching PID " + std::to_string(pid) + "...";
    feedback_error_ = false;
    feedback_is_selection_error_ = false;
    presentFeedback(false);
    aida::ui::task_center::task_registration_t registration;
    registration.owner = "analysis";
    registration.owner_view = "view.sessions";
    registration.owner_action = "session.reattach";
    registration.session = session_id;
    registration.target = "PID " + std::to_string(pid);
    registration.label = "Reattach live session";
    registration.stage = "Validating retained process identity";
    registration.cancellation_is_safe = true;
    registration.callbacks.cancel = [operation] {
        operation->cancellation->request_cancel();
        static_cast<void>(aida::infra::executor::cancel(operation->task_id));
        return true;
    };
    registration.callbacks.focus = [] {
        aida::qt::analysis::QtAnalysisBridge::instance().openView("view.sessions");
    };
    if (!aida::ui::task_center::register_executor_job(submitted.task_id,
            std::move(registration))) {
        operation->cancellation->request_cancel();
        static_cast<void>(aida::infra::executor::cancel(submitted.task_id));
        feedback_ = "Task Center rejected the reattach owner; cancellation was requested.";
        feedback_error_ = true;
        feedback_is_selection_error_ = false;
        reattach_.reset();
        presentFeedback(true);
    }
    poll();
}

void QtSessionsView::requestDetach(const std::string& session_id) {
    std::size_t index = 0;
    if (!analysis_session::find_session_by_id(session_id, &index)) {
        feedback_ = "The selected session changed before detach could start.";
        feedback_error_ = true;
    } else if (!analysis_session::switch_session(index)) {
        feedback_ = "The selected live session could not be activated for detach.";
        feedback_error_ = true;
    } else {
        const auto result = aida::ui::application_ui::execute_action(
            "debugger.detach", aida::ui::action_invocation_source_t::toolbar);
        feedback_ = result.executed()
            ? "Detach queued for the selected live session."
            : result.message.empty() ? "The detach request was rejected."
                : result.message;
        feedback_error_ = !result.executed();
    }
    feedback_is_selection_error_ = false;
    presentFeedback(!reattach_);
}

void QtSessionsView::requestClose(const std::string& session_id) {
    std::size_t index = 0;
    if (analysis_session::find_session_by_id(session_id, &index))
        static_cast<void>(analysis_session::close_session(index));
    else {
        feedback_ = "The selected session changed before close could start.";
        feedback_error_ = true;
        feedback_is_selection_error_ = false;
        presentFeedback(true);
    }
    poll();
}

void QtSessionsView::showRowMenu(const QPoint& global_pos, int view_row) {
    const auto* session = model_->rowAt(view_row);
    if (!session) return;
    const int session_index = model_->sessionIndexFor(view_row);
    if (session_index >= 0)
        analysis_session::switch_session(static_cast<std::size_t>(session_index));
    aida::qt::analysis::QtAnalysisBridge::instance().showRecentMenu(
        session->path, true, aida::ui::context_menu_open_origin_t::pointer,
        global_pos, this);
}

void QtSessionsView::refreshPresentation() {
    const bool empty = model_->rowCount() == 0;
    state_view_->setVisible(empty);
    table_->setVisible(!empty);
    const auto index = table_->currentIndex();
    const auto* session = index.isValid() ? model_->rowAt(index.row()) : nullptr;
    const std::uint32_t active_pid = driver_bridge::attached_pid();
    bool can_reattach = false;
    bool can_detach = false;
    if (session) {
        const bool dead = !session->is_alive ||
            session->load_state == analysis_session::session_load_state_t::failed ||
            session->load_state == analysis_session::session_load_state_t::closed;
        const bool loading =
            session->load_state == analysis_session::session_load_state_t::opening ||
            session->load_state == analysis_session::session_load_state_t::analyzing;
        const bool closing =
            session->load_state == analysis_session::session_load_state_t::closing;
        const bool live_attached =
            session->kind == analysis_session::session_kind_t::live_attach &&
            session->pid != 0 && session->pid == active_pid && session->is_active &&
            analysis_session::active_live_session_matches(session->pid, session->id);
        can_reattach = session->kind == analysis_session::session_kind_t::live_attach &&
            session->pid != 0 && session->process_creation_time_100ns != 0 &&
            !live_attached && !closing && !loading && reattach_ == nullptr;
        can_detach = session->kind == analysis_session::session_kind_t::live_attach &&
            live_attached && !dead && !closing && !loading &&
            session->load_state == analysis_session::session_load_state_t::ready;
    }
    reattach_button_->setEnabled(can_reattach);
    detach_button_->setEnabled(can_detach);
    close_button_->setEnabled(session != nullptr);
    std::string load_error;
    std::string load_error_key;
    if (session &&
        session->load_state == analysis_session::session_load_state_t::failed &&
        session->error) {
        load_error_key = session->id + ":" + session->error->stable_code();
        if (load_error_key != dismissed_error_key_)
            load_error = session->error->stable_code() + ": " +
                session->error->message;
    }
    if (!load_error.empty()) {
        if ((feedback_.empty() || feedback_is_selection_error_) &&
            feedback_ != load_error) {
            selection_error_key_ = load_error_key;
            feedback_ = load_error;
            feedback_error_ = true;
            feedback_is_selection_error_ = true;
            presentFeedback(true);
        }
    } else if (feedback_is_selection_error_) {
        feedback_is_selection_error_ = false;
        feedback_.clear();
        presentFeedback(false);
    }
}

}
