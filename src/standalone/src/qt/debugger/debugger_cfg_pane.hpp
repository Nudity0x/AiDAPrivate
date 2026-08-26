#pragma once

#include "qt/debugger/debugger_pane_base.hpp"

#include <cstdint>

namespace aida::qt::graph {
class AidaCfgView;
}

namespace aida::qt::widgets {
class AidaButton;
}

namespace aida::qt::debugger {

// Debugger CFG pane: thin host embedding the shared QGraphicsView CFG widget
// (06's AidaCfgView) plus the "Build CFG at RIP" control strip with the
// capability gate (rip != 0) and the RIP hand-off.
class DebuggerCfgPane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit DebuggerCfgPane(QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onSessionTick() override;

private:
    void buildAtRip();
    void openInGraphView();

    graph::AidaCfgView* cfg_view_ = nullptr;
    widgets::AidaButton* build_button_ = nullptr;
    widgets::AidaButton* open_graph_button_ = nullptr;
    std::uint64_t last_built_addr_ = 0;
};

}
