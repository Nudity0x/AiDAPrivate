#include "qt/debugger/call_stack_pane.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableView>
#include <QTimer>

#include <chrono>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_view.hpp"
#include "core/infra/executor.hpp"
#include "core/runtime/standalone_driver.hpp"

#include "qt/debugger/debugger_models.hpp"
#include "qt/debugger/debugger_session_controller.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::debugger {

CallStackPane::CallStackPane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.call_stack"));
    setOwnerViewId("view.debug.call_stack");
    setEmptyTargetText(QStringLiteral("No paused target"),
        QStringLiteral(
            "Pause or step the target to capture a call stack. Double-click a "
            "frame to jump, right-click to copy the address."));
    setEmptyContentText(QStringLiteral("No call stack captured"),
        QStringLiteral(
            "The paused thread reported no walkable frames at this stop."));
    setLoadingText(QStringLiteral("Walking the call stack"),
        QStringLiteral(
            "The engine is capturing frames for the current stop."));
    setErrorText(QStringLiteral("Call-stack refresh failed"),
        QStringLiteral(
            "The call-stack refresh worker could not be queued; the pane "
            "retries on the next refresh cycle."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    auto* hint = new QLabel(QStringLiteral(
        "double-click or Enter to jump, right-click to copy address"), bar);
    hint->setObjectName(QStringLiteral("aida.view.debug.call_stack.hint"));
    hint->setProperty("aidaVariant", QStringLiteral("secondary"));
    bar_layout->addWidget(hint);
    bar_layout->addStretch(1);
    setToolBar(bar);

    model_ = new CallStackModel(this);
    view_ = new QTableView(this);
    view_->setObjectName(QStringLiteral("aida.view.debug.call_stack.table"));
    wireTable(view_, model_);
    setContent(view_);

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(500);
    refresh_timer_->setTimerType(Qt::CoarseTimer);
    connect(refresh_timer_, &QTimer::timeout, this,
        &CallStackPane::refreshCallStack);

    model_timer_ = new QTimer(this);
    model_timer_->setInterval(250);
    model_timer_->setTimerType(Qt::CoarseTimer);
    connect(model_timer_, &QTimer::timeout, this, &CallStackPane::pollModel);

    connect(view_, &QAbstractItemView::activated, this,
        [this](const QModelIndex&) { jumpToSelected(); });
}

void CallStackPane::onShown() {
    refresh_timer_->start();
    model_timer_->start();
    refreshCallStack();
    pollModel();
}

void CallStackPane::onHidden() {
    refresh_timer_->stop();
    model_timer_->stop();
}

bool CallStackPane::hasTargetContent() const {
    return driver_bridge::attached_pid() != 0 &&
        DebuggerSessionController::instance().pausedOrStepping();
}

bool CallStackPane::hasContentRows() const {
    return model_ && model_->rowCount() > 0;
}

bool CallStackPane::isContentLoading() const {
    return refresh_in_flight_.load(std::memory_order_acquire) ||
        snapshots_.generation() !=
            debugger_engine::g_state.call_stack_generation.load(
                std::memory_order_acquire);
}

bool CallStackPane::contentError(QString* detail) const {
    if (last_error_.isEmpty())
        return false;
    if (detail)
        *detail = last_error_;
    return true;
}

void CallStackPane::refreshCallStack() {
    if (driver_bridge::attached_pid() == 0)
        return;
    const auto status = debugger_engine::g_state.status.load(
        std::memory_order_acquire);
    if (status != debugger_engine::dbg_status_t::paused &&
        status != debugger_engine::dbg_status_t::stepping)
        return;
    const std::uint64_t now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const std::uint64_t last = last_refresh_ms_.load(std::memory_order_acquire);
    if (refresh_in_flight_.load(std::memory_order_acquire) ||
        now_ms - last <= 500)
        return;
    bool expected = false;
    if (!refresh_in_flight_.compare_exchange_strong(expected, true))
        return;
    const std::uint32_t target_pid = driver_bridge::attached_pid();
    const std::uint64_t target_generation =
        debugger_interaction::current_stop_generation();
    aida::infra::executor::submission_t sub;
    sub.owner_subsystem = "debugger";
    sub.label = "debugger.call_stack_refresh";
    sub.thread_class = "debugger_refresh";
    sub.domain = aida::infra::executor::domain_t::feature_worker;
    sub.priority = 3;
    sub.target_pid = target_pid;
    sub.generation = target_generation;
    sub.body = [this, now_ms, target_pid, target_generation]() {
        try {
            const std::uint64_t publication_generation =
                debugger_engine::g_state.call_stack_generation.load(
                    std::memory_order_acquire);
            if (driver_bridge::attached_pid() == target_pid &&
                debugger_interaction::current_stop_generation() ==
                    target_generation) {
                static_cast<void>(debugger_engine::get_call_stack());
                if (driver_bridge::attached_pid() != target_pid ||
                    debugger_interaction::current_stop_generation() !=
                        target_generation) {
                    std::lock_guard<std::mutex> lock(
                        debugger_engine::g_state.stack_mutex);
                    if (debugger_engine::g_state.call_stack_generation.load(
                            std::memory_order_acquire) ==
                            publication_generation + 1U) {
                        debugger_engine::g_state.call_stack.clear();
                        debugger_engine::g_state.call_stack_generation
                            .fetch_add(1U, std::memory_order_release);
                    }
                } else {
                    last_refresh_ms_.store(now_ms, std::memory_order_release);
                }
            }
        } catch (...) {
            refresh_in_flight_.store(false, std::memory_order_release);
            throw;
        }
        refresh_in_flight_.store(false, std::memory_order_release);
    };
    const auto submitted = debugger_view::submit_owned_debugger_task(
        std::move(sub), "view.debug.call_stack", "debugger.call_stack_refresh",
        "Refresh call stack", false);
    if (!submitted.submitted) {
        diag::log_tagged("debugger", "call_stack_refresh_post_failed");
        last_error_ = QStringLiteral(
            "The call-stack refresh could not be queued: %1").arg(
                submitted.reject_reason.empty()
                    ? QStringLiteral("the executor rejected the submission")
                    : QString::fromStdString(submitted.reject_reason));
        refresh_in_flight_.store(false, std::memory_order_release);
        updateOverlayState();
    } else if (!last_error_.isEmpty()) {
        last_error_.clear();
        updateOverlayState();
    }
}

void CallStackPane::pollModel() {
    auto& st = debugger_engine::g_state;
    const auto snapshot = snapshots_.poll(st.stack_mutex, st.call_stack,
        st.call_stack_generation, "call_stack");
    if (snapshot.refreshed) {
        const auto selected = capture_selected_row_ids(*model_,
            view_->selectionModel());
        const quint64 focus = view_->currentIndex().isValid()
            ? model_->rowId(view_->currentIndex().row()) : 0;
        model_->applySnapshot(snapshot.items, snapshot.generation);
        restore_selected_row_ids(*model_, view_, selected, focus);
        if (!last_error_.isEmpty())
            last_error_.clear();
    }
}

void CallStackPane::jumpToSelected() {
    const auto index = view_->currentIndex();
    if (!index.isValid())
        return;
    const auto* frame = model_->rowAt(index.row());
    if (frame && frame->address != 0)
        debugger_view::jump_to_disasm(frame->address);
}

}
