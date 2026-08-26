#include "qt/debugger/debugger_status_strip.hpp"

#include <QHBoxLayout>
#include <QLabel>

#include "core/debugger/debugger_engine.hpp"
#include "core/debugger/debugger_view.hpp"
#include "core/runtime/standalone_driver.hpp"

#include "qt/debugger/debugger_session_controller.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_pill.hpp"

namespace aida::qt::debugger {

DebuggerStatusStrip::DebuggerStatusStrip(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("aida.debugger.status_strip"));
    layout_ = new QHBoxLayout(this);
    const auto& tokens = theme::tokens();
    layout_->setContentsMargins(tokens.status_bar.padding_x, 0,
        tokens.status_bar.padding_x, 0);
    layout_->setSpacing(tokens.status_bar.item_gap);
    setFixedHeight(tokens.status_bar.height);

    target_label_ = new QLabel(this);
    target_label_->setObjectName(QStringLiteral("aida.debugger.status_strip.target"));
    layout_->addWidget(target_label_);

    engine_pill_ = new widgets::AidaPill(QStringLiteral("Idle"),
        widgets::AidaSemantic::Neutral, this);
    engine_pill_->setObjectName(
        QStringLiteral("aida.debugger.status_strip.engine"));
    engine_pill_->setSize(widgets::AidaPill::Size::Small);
    layout_->addWidget(engine_pill_);

    panel_label_ = new QLabel(this);
    panel_label_->setObjectName(QStringLiteral("aida.debugger.status_strip.panel"));
    layout_->addWidget(panel_label_);

    layout_->addStretch(1);

    rip_label_ = new QLabel(this);
    rip_label_->setObjectName(QStringLiteral("aida.debugger.status_strip.rip"));
    rip_label_->setFont(theme::fonts::codeRegular());
    layout_->addWidget(rip_label_);

    connect(&DebuggerSessionController::instance(),
        &DebuggerSessionController::sessionTick, this,
        &DebuggerStatusStrip::refreshState);
    connect(&DebuggerSessionController::instance(),
        &DebuggerSessionController::watchdogSampled, this,
        &DebuggerStatusStrip::onWatchdogSampled);
    refreshState();
}

void DebuggerStatusStrip::setPanelLabel(const QString& label) {
    panel_label_text_ = label;
    refreshState();
}

void DebuggerStatusStrip::onWatchdogSampled(quint64 ageMs, bool degraded) {
    (void)ageMs;
    watchdog_degraded_ = degraded;
    refreshState();
}

void DebuggerStatusStrip::refreshState() {
    const quint32 pid = driver_bridge::attached_pid();
    const bool has_target = pid != 0;
    target_label_->setText(has_target
        ? QStringLiteral("Target %1").arg(pid)
        : QStringLiteral("Target none"));

    const int status = DebuggerSessionController::instance().status();
    QString engine_text = QString::fromLatin1(
        debugger_view::debugger_status_label(
            static_cast<debugger_engine::dbg_status_t>(status)));
    if (watchdog_degraded_)
        engine_text += QStringLiteral(" (driver watchdog stale)");
    engine_pill_->setText(engine_text);
    widgets::AidaSemantic kind = widgets::AidaSemantic::Neutral;
    if (!has_target)
        kind = widgets::AidaSemantic::Warning;
    else if (watchdog_degraded_)
        kind = widgets::AidaSemantic::Warning;
    else if (status == static_cast<int>(debugger_engine::dbg_status_t::running))
        kind = widgets::AidaSemantic::Success;
    else if (status == static_cast<int>(debugger_engine::dbg_status_t::paused))
        kind = widgets::AidaSemantic::Info;
    else if (status == static_cast<int>(debugger_engine::dbg_status_t::stepping))
        kind = widgets::AidaSemantic::Accent;
    else if (status == static_cast<int>(debugger_engine::dbg_status_t::terminated))
        kind = widgets::AidaSemantic::Error;
    engine_pill_->setKind(kind);

    const quint64 rip = debugger_engine::cached_registers().rip;
    rip_label_->setText(QString::asprintf("RIP 0x%016llX",
        static_cast<unsigned long long>(rip)));

    const auto& t = theme::tokens();
    int budget = width() - 2 * t.status_bar.padding_x;
    budget -= target_label_->sizeHint().width() + t.status_bar.item_gap;
    budget -= engine_pill_->sizeHint().width() + t.status_bar.item_gap;
    const int rip_budget = rip_label_->sizeHint().width() +
        t.status_bar.item_gap;
    const bool show_rip = budget >= rip_budget;
    rip_label_->setVisible(show_rip);
    if (show_rip)
        budget -= rip_budget;
    panel_label_->setText(panel_label_text_);
    panel_label_->setVisible(!panel_label_text_.isEmpty() &&
        budget >= panel_label_->sizeHint().width() + t.status_bar.item_gap);
}

}
