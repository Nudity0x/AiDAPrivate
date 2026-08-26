#include "qt/debugger/threads_pane.hpp"

#include <QTableView>
#include <QTimer>

#include "core/debugger/debugger_view.hpp"
#include "core/runtime/standalone_driver.hpp"

#include "qt/debugger/debugger_models.hpp"

namespace aida::qt::debugger {

ThreadsPane::ThreadsPane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.threads"));
    setOwnerViewId("view.debug.threads");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral(
            "Attach to a process to inspect and control its threads."));
    setEmptyContentText(QStringLiteral("No threads reported"),
        QStringLiteral(
            "The attached target has not reported any threads yet; the pane "
            "refreshes automatically."));
    setLoadingText(QStringLiteral("Reading threads"),
        QStringLiteral("Enumerating the attached target's threads."));

    model_ = new ThreadsModel(this);
    view_ = new QTableView(this);
    view_->setObjectName(QStringLiteral("aida.view.debug.threads.table"));
    wireTable(view_, model_);
    view_->setItemDelegateForColumn(ThreadsModel::State,
        new StatePillDelegate(view_));
    setContent(view_);

    timer_ = new QTimer(this);
    timer_->setInterval(250);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &ThreadsPane::poll);
}

void ThreadsPane::onShown() {
    timer_->start();
    poll();
}

void ThreadsPane::onHidden() {
    timer_->stop();
}

bool ThreadsPane::hasContentRows() const {
    return model_ && model_->rowCount() > 0;
}

bool ThreadsPane::isContentLoading() const {
    return snapshots_.generation() !=
        debugger_engine::g_state.cached_threads_generation.load(
            std::memory_order_acquire);
}

void ThreadsPane::poll() {
    if (driver_bridge::attached_pid() == 0)
        return;
    debugger_engine::request_thread_refresh(250);
    auto& st = debugger_engine::g_state;
    const auto snapshot = snapshots_.poll(st.cache_mtx, st.cached_threads,
        st.cached_threads_generation, "threads");
    if (snapshot.refreshed)
        model_->applyThreads(snapshot.items, snapshot.generation);
}

}
