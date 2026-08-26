#pragma once

#include "qt/debugger/debugger_pane_base.hpp"
#include "qt/debugger/debugger_snapshot_store.hpp"

#include <QString>

#include "core/debugger/debugger_engine.hpp"
#include "core/debugger/debugger_interaction_context.hpp"

class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaLineEdit;
}

namespace aida::qt::debugger {

class BreakpointsModel;

// Breakpoints pane: add-bar (address + SW/HW add + Clear All, with the staged
// breakpoint-definition handoff from stage_breakpoint_definition) over the
// generation-driven breakpoint table. Edit opens the identity-retaining
// BreakpointEditDialog; enable/disable/delete run through the mutation queue.
class BreakpointsPane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit BreakpointsPane(QWidget* parent = nullptr);

    // Staged definition handoff (backend stage_breakpoint_definition hook).
    void stageAddress(std::uint64_t address, bool hardwareExecute,
                      const debugger_interaction::context_t& context);

protected:
    void onShown() override;
    void onHidden() override;
    void onSessionTick() override;
    bool hasContentRows() const override;

private:
    void addBreakpoint(bool hardwareExecute);
    void clearStaged(bool clearAddress);
    void editSelected();
    void applySnapshotPreservingSelection(
        std::shared_ptr<const std::vector<debugger_engine::breakpoint_t>> rows,
        quint64 generation);

    BreakpointsModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    widgets::AidaLineEdit* address_edit_ = nullptr;
    widgets::AidaButton* add_sw_button_ = nullptr;
    widgets::AidaButton* add_hw_button_ = nullptr;
    widgets::AidaButton* clear_button_ = nullptr;
    QTimer* poll_timer_ = nullptr;
    bool staged_ = false;
    bool staged_hardware_ = false;
    debugger_interaction::context_t staged_context_{};
    SnapshotStore<debugger_engine::breakpoint_t> snapshots_;
};

}
