#pragma once

#include "qt/debugger/debugger_pane_base.hpp"
#include "qt/debugger/debugger_snapshot_store.hpp"

#include <atomic>

#include "core/debugger/debugger_engine.hpp"

class QTableView;
class QTimer;

namespace aida::qt::debugger {

class CallStackModel;

// Call stack pane. Ports the single-flight 500ms refresh worker verbatim
// (submit-time target_pid + stop_generation capture, re-check before driver
// work AND before publication, publication-generation rollback on mismatch).
class CallStackPane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit CallStackPane(QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onHidden() override;
    bool hasTargetContent() const override;
    bool hasContentRows() const override;
    bool isContentLoading() const override;
    bool contentError(QString* detail) const override;

private:
    void refreshCallStack();
    void pollModel();
    void jumpToSelected();

    CallStackModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    QTimer* model_timer_ = nullptr;
    SnapshotStore<debugger_engine::stack_frame_t> snapshots_;
    std::atomic<bool> refresh_in_flight_{false};
    std::atomic<std::uint64_t> last_refresh_ms_{0};
    QString last_error_;
};

}
