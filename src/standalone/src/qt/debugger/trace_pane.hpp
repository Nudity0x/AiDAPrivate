#pragma once

#include "qt/debugger/debugger_pane_base.hpp"
#include "qt/debugger/debugger_snapshot_store.hpp"

#include "core/debugger/debugger_engine.hpp"

class QCheckBox;
class QLabel;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaLineEdit;
class AidaPill;
}

namespace aida::qt::debugger {

class TraceModel;

// Trace pane: 100ms snapshot application while tracing (freeze = stop applying
// snapshots), filter, REC pill with record pulse, dropped-record backpressure
// banner, Clear + bounded CSV export worker.
class TracePane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit TracePane(QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onHidden() override;
    bool hasContentRows() const override;
    bool isContentLoading() const override;

private:
    void poll();
    void toggleTrace();
    void clearTrace();
    void exportTrace();

    TraceModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    widgets::AidaLineEdit* filter_edit_ = nullptr;
    QCheckBox* freeze_check_ = nullptr;
    widgets::AidaButton* trace_button_ = nullptr;
    widgets::AidaButton* clear_button_ = nullptr;
    widgets::AidaButton* export_button_ = nullptr;
    widgets::AidaPill* rec_pill_ = nullptr;
    QLabel* dropped_label_ = nullptr;
    QTimer* timer_ = nullptr;
    SnapshotStore<debugger_engine::trace_record_t> snapshots_;
};

}
