#include "qt/network/network_pane_base.hpp"

#include <QWidget>
#include <QHideEvent>
#include <QShowEvent>
#include <QStackedLayout>
#include <QTimer>

#include "core/session/analysis_session.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

NetworkPaneBase::NetworkPaneBase(QWidget* parent)
    : QWidget(parent) {
    setFocusPolicy(Qt::ClickFocus);
    stack_ = new QStackedLayout(this);
    stack_->setStackingMode(QStackedLayout::StackOne);

    placeholder_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No target attached"),
        QStringLiteral("Attach or launch a target to enable this driver-backed Network pane."),
        this);
    placeholder_->setObjectName(QStringLiteral("aida.view.network.pane.no_target"));
    stack_->addWidget(placeholder_);

    target_timer_ = new QTimer(this);
    target_timer_->setInterval(500);
    connect(target_timer_, &QTimer::timeout, this, [this] {
        refreshTargetState();
    });
}

NetworkPaneBase::~NetworkPaneBase() = default;

void NetworkPaneBase::setRequiresTarget(bool requires) {
    requires_target_ = requires;
    refreshTargetState();
}

void NetworkPaneBase::setContent(QWidget* content) {
    if (content_ == content)
        return;
    if (content_)
        stack_->removeWidget(content_);
    content_ = content;
    if (content_) {
        content_->setFocusPolicy(Qt::ClickFocus);
        stack_->insertWidget(0, content_);
    }
    refreshTargetState();
}

void NetworkPaneBase::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    pane_visible_ = true;
    refreshTargetState();
    onPaneShown();
}

void NetworkPaneBase::hideEvent(QHideEvent* event) {
    pane_visible_ = false;
    if (target_timer_)
        target_timer_->stop();
    onPaneHidden();
    QWidget::hideEvent(event);
}

void NetworkPaneBase::onPaneShown() {}

void NetworkPaneBase::onPaneHidden() {}

void NetworkPaneBase::refreshTargetState() {
    if (!requires_target_) {
        if (content_)
            stack_->setCurrentWidget(content_);
        return;
    }
    target_available_ = analysis_session::has_active_target();
    if (target_available_) {
        if (content_)
            stack_->setCurrentWidget(content_);
        if (target_timer_)
            target_timer_->stop();
    } else {
        stack_->setCurrentWidget(placeholder_);
        if (pane_visible_ && target_timer_ && !target_timer_->isActive())
            target_timer_->start();
    }
}

}
