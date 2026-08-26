#pragma once

#include "qt/debugger/debugger_pane_base.hpp"
#include "qt/debugger/debugger_snapshot_store.hpp"

#include "core/debugger/debugger_engine.hpp"

class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaLineEdit;
}

namespace aida::qt::debugger {

class WatchesModel;

// Watches pane: add-bar + generation-driven table. Refresh routes through the
// registered debugger.watch.refresh_all action (the batch pipeline:
// GUI-capture -> worker evaluate -> publish); auto-refresh on stop-generation
// change.
class WatchesPane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit WatchesPane(QWidget* parent = nullptr);

    void stageExpression(const QString& expression);

protected:
    void onShown() override;
    void onHidden() override;
    void onSessionStateChanged(int status, quint32 pid,
                               quint64 stopGeneration) override;
    bool hasContentRows() const override;

private:
    void addWatch();
    void refreshWatches();

    WatchesModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    widgets::AidaLineEdit* expression_edit_ = nullptr;
    widgets::AidaButton* add_button_ = nullptr;
    widgets::AidaButton* refresh_button_ = nullptr;
    QTimer* poll_timer_ = nullptr;
    SnapshotStore<debugger_engine::watch_entry_t> snapshots_;
    quint64 last_stop_generation_ = 0;
};

}
