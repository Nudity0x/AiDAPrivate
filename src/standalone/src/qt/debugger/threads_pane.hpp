#pragma once

#include "qt/debugger/debugger_pane_base.hpp"
#include "qt/debugger/debugger_snapshot_store.hpp"

#include "core/debugger/debugger_engine.hpp"

class QTableView;
class QTimer;

namespace aida::qt::debugger {

class ThreadsModel;

// Threads pane (250ms cadence via request_thread_refresh + snapshot poll of
// cached_threads/cached_threads_generation). Per-row state-change flash via
// the model's FlashMap; suspend/resume/terminate gated per-row through the
// retained entity menu (terminate gets the review confirm).
class ThreadsPane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit ThreadsPane(QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onHidden() override;
    bool hasContentRows() const override;
    bool isContentLoading() const override;

private:
    void poll();

    ThreadsModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    QTimer* timer_ = nullptr;
    SnapshotStore<debugger_engine::cached_thread_t> snapshots_;
};

}
