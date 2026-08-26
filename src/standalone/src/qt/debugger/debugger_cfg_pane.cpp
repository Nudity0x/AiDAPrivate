#include "qt/debugger/debugger_cfg_pane.hpp"

#include <QHBoxLayout>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_engine.hpp"
#include "core/disasm/cfg_view.hpp"
#include "core/runtime/standalone_driver.hpp"

#include "qt/debugger/debugger_action_bridge.hpp"
#include "qt/graph/aida_cfg_view.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"

namespace aida::qt::debugger {

DebuggerCfgPane::DebuggerCfgPane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.cfg"));
    setOwnerViewId("view.debug.cfg");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral(
            "Attach and pause a target to build a CFG at the instruction "
            "pointer."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    bar_layout->setSpacing(tokens.toolbar.group_gap);
    build_button_ = new widgets::AidaButton(QStringLiteral("Build CFG at RIP"),
        bar);
    build_button_->setObjectName(QStringLiteral("aida.view.debug.cfg.build"));
    build_button_->setKind(widgets::AidaButton::Kind::Primary);
    build_button_->setToolTip(QStringLiteral(
        "Build the control-flow graph at the current instruction pointer"));
    connect(build_button_, &widgets::AidaButton::clicked, this,
        &DebuggerCfgPane::buildAtRip);
    bar_layout->addWidget(build_button_);
    open_graph_button_ = new widgets::AidaButton(
        QStringLiteral("Open in Graph View"), bar);
    open_graph_button_->setObjectName(
        QStringLiteral("aida.view.debug.cfg.open_graph"));
    open_graph_button_->setKind(widgets::AidaButton::Kind::Secondary);
    open_graph_button_->setToolTip(QStringLiteral(
        "Open the built control-flow graph in the full graph document"));
    connect(open_graph_button_, &widgets::AidaButton::clicked, this,
        &DebuggerCfgPane::openInGraphView);
    bar_layout->addWidget(open_graph_button_);
    bar_layout->addStretch(1);
    setToolBar(bar);

    cfg_view_ = new graph::AidaCfgView(this);
    setContent(cfg_view_);
}

void DebuggerCfgPane::onShown() {
    cfg_view_->refreshContext();
}

void DebuggerCfgPane::onSessionTick() {
    const std::uint64_t rip = debugger_engine::cached_registers().rip;
    const bool can_build = rip != 0;
    build_button_->setText(can_build
        ? QStringLiteral("Build CFG at RIP")
        : QStringLiteral("Build CFG (no RIP)"));
    build_button_->setEnabled(can_build);
    open_graph_button_->setEnabled(can_build || last_built_addr_ != 0);
    updateOverlayState();
}

void DebuggerCfgPane::buildAtRip() {
    const std::uint64_t rip = debugger_engine::cached_registers().rip;
    if (rip == 0)
        return;
    cfg_view::build_cfg(rip);
    last_built_addr_ = rip;
    diag::log_tagged_critical_fmt("cfg", "cfg_build_from_debugger rip=0x%llx",
        static_cast<unsigned long long>(rip));
    diag::log_tagged("dbg_audit", "[dbg_audit] cfg build_at_rip ok=1");
}

void DebuggerCfgPane::openInGraphView() {
    const std::uint64_t rip = debugger_engine::cached_registers().rip;
    const bool can_build = rip != 0;
    const std::uint64_t target = can_build ? rip : last_built_addr_;
    if (target != 0) {
        DebuggerActionBridge::instance().openView("document.graph");
        cfg_view::build_cfg(target);
        last_built_addr_ = target;
        diag::log_tagged_critical_fmt("cfg", "cfg_open_graph_view target=0x%llx",
            static_cast<unsigned long long>(target));
        diag::log_tagged("dbg_audit", "[dbg_audit] cfg open_full ok=1");
    } else {
        diag::log_tagged("dbg_audit",
            "[dbg_audit] cfg open_full fail reason=no_address");
    }
}

}
