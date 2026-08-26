#include "qt/debugger/handles_pane.hpp"

#include <QHBoxLayout>
#include <QTableView>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_view.hpp"
#include "core/infra/executor.hpp"
#include "core/runtime/standalone_driver.hpp"

#include "qt/debugger/debugger_models.hpp"
#include "qt/debugger/dialogs/confirm_dialogs.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"

namespace aida::qt::debugger {

HandlesPane::HandlesPane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.handles"));
    setOwnerViewId("view.debug.handles");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral(
            "Attach to a process to enumerate or close its kernel handles."));
    setEmptyContentText(QStringLiteral("No handles enumerated"),
        QStringLiteral(
            "Press Enumerate Handles to snapshot the target's kernel handle "
            "table."));
    setLoadingText(QStringLiteral("Enumerating handles"),
        QStringLiteral(
            "The engine is snapshotting the attached target's kernel handle "
            "table."));
    setErrorText(QStringLiteral("Handle enumeration failed"),
        QStringLiteral(
            "The handle enumeration worker could not be queued; press "
            "Enumerate Handles to retry."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    bar_layout->setSpacing(tokens.toolbar.group_gap);
    refresh_button_ = new widgets::AidaButton(QStringLiteral("Enumerate Handles"),
        bar);
    refresh_button_->setObjectName(
        QStringLiteral("aida.view.debug.handles.refresh"));
    refresh_button_->setKind(widgets::AidaButton::Kind::Primary);
    refresh_button_->setToolTip(QStringLiteral(
        "Enumerate the attached target's kernel handles"));
    connect(refresh_button_, &widgets::AidaButton::clicked, this,
        &HandlesPane::enumerate);
    bar_layout->addWidget(refresh_button_);
    close_button_ = new widgets::AidaButton(QStringLiteral("Close Handle..."),
        bar);
    close_button_->setObjectName(QStringLiteral("aida.view.debug.handles.close"));
    close_button_->setKind(widgets::AidaButton::Kind::Destructive);
    close_button_->setToolTip(QStringLiteral(
        "Review closing the selected handle in the target"));
    connect(close_button_, &widgets::AidaButton::clicked, this,
        &HandlesPane::closeSelected);
    bar_layout->addWidget(close_button_);
    bar_layout->addStretch(1);
    setToolBar(bar);

    model_ = new HandlesModel(this);
    view_ = new QTableView(this);
    view_->setObjectName(QStringLiteral("aida.view.debug.handles.table"));
    wireTable(view_, model_);
    setContent(view_);
}

void HandlesPane::onShown() {
    pollModel();
}

void HandlesPane::onSessionTick() {
    pollModel();
    updateOverlayState();
}

bool HandlesPane::hasContentRows() const {
    return model_ && model_->rowCount() > 0;
}

bool HandlesPane::isContentLoading() const {
    return enumerate_in_flight_.load(std::memory_order_acquire);
}

bool HandlesPane::contentError(QString* detail) const {
    if (last_error_.isEmpty())
        return false;
    if (detail)
        *detail = last_error_;
    return true;
}

void HandlesPane::pollModel() {
    refresh_button_->setEnabled(!enumerate_in_flight_.load(
        std::memory_order_acquire));
    refresh_button_->setLoading(enumerate_in_flight_.load(
        std::memory_order_acquire));
    auto& st = debugger_engine::g_state;
    const auto snapshot = snapshots_.poll(st.handle_mutex, st.handles,
        st.handles_generation, "handles");
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

void HandlesPane::enumerate() {
    if (driver_bridge::attached_pid() == 0)
        return;
    if (enumerate_in_flight_.load(std::memory_order_acquire))
        return;
    enumerate_in_flight_.store(true, std::memory_order_release);
    updateOverlayState();
    diag::log_tagged_critical_fmt("handles",
        "handles_enumerate_request attached_pid=%u",
        static_cast<unsigned>(driver_bridge::attached_pid()));
    diag::log_tagged("dbg_audit", "[dbg_audit] handles enumerate ok=1");
    aida::infra::executor::submission_t sub;
    sub.owner_subsystem = "debugger";
    sub.label = "debugger.handles_enumerate";
    sub.thread_class = "debugger_refresh";
    sub.domain = aida::infra::executor::domain_t::feature_worker;
    sub.priority = 3;
    const std::uint32_t target_pid = driver_bridge::attached_pid();
    const std::uint64_t target_generation =
        debugger_interaction::current_stop_generation();
    sub.target_pid = target_pid;
    sub.generation = target_generation;
    sub.body = [this, target_pid, target_generation]() {
        try {
            if (driver_bridge::attached_pid() == target_pid &&
                debugger_interaction::current_stop_generation() ==
                    target_generation) {
                debugger_engine::enumerate_handles();
                std::size_t count = 0;
                {
                    std::lock_guard<std::mutex> lock(
                        debugger_engine::g_state.handle_mutex);
                    count = debugger_engine::g_state.handles.size();
                }
                diag::log_tagged_fmt("handles",
                    "handles_enumerate_done count=%zu", count);
            }
        } catch (...) {
            enumerate_in_flight_.store(false, std::memory_order_release);
            throw;
        }
        enumerate_in_flight_.store(false, std::memory_order_release);
    };
    const auto submitted = debugger_view::submit_owned_debugger_task(
        std::move(sub), "view.debug.handles", "debugger.handles_enumerate",
        "Enumerate target handles", false);
    if (!submitted.submitted) {
        diag::log_tagged("handles", "handles_enumerate_post_failed");
        enumerate_in_flight_.store(false, std::memory_order_release);
        last_error_ = QStringLiteral(
            "The handle enumeration could not be queued: %1").arg(
                submitted.reject_reason.empty()
                    ? QStringLiteral("the executor rejected the submission")
                    : QString::fromStdString(submitted.reject_reason));
        updateOverlayState();
    }
}

void HandlesPane::closeSelected() {
    const auto index = view_->currentIndex();
    if (!index.isValid())
        return;
    const auto context = model_->contextForRow(index.row());
    if (context.kind == debugger_interaction::kind_t::none)
        return;
    diag::log_tagged("dbg_audit", "[dbg_audit] handles close_request ok=1");
    confirm_dialogs::confirm_close_handle(context, this);
}

}
