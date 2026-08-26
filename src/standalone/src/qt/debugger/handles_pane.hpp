#pragma once

#include "qt/debugger/debugger_pane_base.hpp"
#include "qt/debugger/debugger_snapshot_store.hpp"

#include "core/debugger/debugger_engine.hpp"

class QTableView;

namespace aida::qt::widgets {
class AidaButton;
}

namespace aida::qt::debugger {

class HandlesModel;

// Handles pane: manual Refresh submits the enumerate_handles worker (the
// verbatim stop_generation+target_pid guard); no poll. Close Handle runs the
// review confirm then the mutation queue.
class HandlesPane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit HandlesPane(QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onSessionTick() override;
    bool hasContentRows() const override;
    bool isContentLoading() const override;
    bool contentError(QString* detail) const override;

private:
    void enumerate();
    void pollModel();
    void closeSelected();

    HandlesModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    widgets::AidaButton* refresh_button_ = nullptr;
    widgets::AidaButton* close_button_ = nullptr;
    SnapshotStore<debugger_engine::handle_info_t> snapshots_;
    std::atomic<bool> enumerate_in_flight_{false};
    QString last_error_;
};

}
