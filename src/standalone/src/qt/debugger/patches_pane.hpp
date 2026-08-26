#pragma once

#include "qt/debugger/debugger_pane_base.hpp"

#include <string>

class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaNotice;
}

namespace aida::qt::debugger {

class PatchesModel;

// Patches pane: the immutable code_patcher publication table + the four
// registered panel actions (debugger.patch.stage / find_caves / revert_all /
// save_set) driven from the action registry with the patch_panel_capability
// gate, plus the publication-failure notice.
class PatchesPane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit PatchesPane(QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onHidden() override;
    void onSessionTick() override;
    bool hasContentRows() const override;
    bool contentError(QString* detail) const override;

private:
    void pollModel();
    void runPanelCommand(int command);
    void notify_panel_error(const std::string& error);

    PatchesModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    widgets::AidaButton* stage_button_ = nullptr;
    widgets::AidaButton* caves_button_ = nullptr;
    widgets::AidaButton* revert_all_button_ = nullptr;
    widgets::AidaButton* save_button_ = nullptr;
    widgets::AidaNotice* publication_notice_ = nullptr;
    QTimer* poll_timer_ = nullptr;
    std::uint64_t last_generation_ = 0;
};

}
