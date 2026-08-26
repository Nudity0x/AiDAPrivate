#include "aida_state_view.hpp"
#include <algorithm>

#include <QEvent>
#include <QPainter>

#include "../theme/aida_fonts.hpp"
#include "aida_button.hpp"
#include "aida_paint_utils.hpp"
#include "aida_progress.hpp"

namespace aida::qt::widgets {

AidaStateView::AidaStateView(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.state_view"));
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setProperty("aidaRole", QStringLiteral("empty"));
}

AidaStateView::AidaStateView(State state, const QString& title, const QString& message,
    QWidget* parent)
    : AidaStateView(parent)
{
    title_ = title;
    message_ = message;
    setState(state);
}

void AidaStateView::setState(State state)
{
    if (state_ == state)
        return;
    state_ = state;
    if (state_ == State::Loading) {
        if (!ring_) {
            ring_ = new AidaProgressRing(this);
            ring_->setIndeterminate(true);
            ring_->setThickness(qreal(aida::qt::theme::tokens().control.focus_ring));
        }
        ring_->show();
    } else if (ring_) {
        ring_->hide();
    }
    if (action_button_)
        action_button_->setVisible(state_ != State::Loading);
    layoutChildren();
    updateGeometry();
    update();
}

void AidaStateView::setTitle(const QString& title)
{
    if (title_ == title)
        return;
    title_ = title;
    setAccessibleName(title_);
    updateGeometry();
    update();
}

void AidaStateView::setMessage(const QString& message)
{
    if (message_ == message)
        return;
    message_ = message;
    setAccessibleDescription(message_);
    updateGeometry();
    update();
}

void AidaStateView::setActionLabel(const QString& label)
{
    if (label.isEmpty()) {
        if (action_button_) {
            action_button_->hide();
            action_button_->deleteLater();
            action_button_ = nullptr;
        }
    } else {
        if (!action_button_) {
            action_button_ = new AidaButton(this);
            action_button_->setControlSize(AidaButton::ControlSize::Small);
            connect(action_button_, &QAbstractButton::clicked, this,
                [this] { Q_EMIT actionTriggered(); });
        }
        action_button_->setKind(state_ == State::Error
            ? AidaButton::Kind::Secondary : AidaButton::Kind::Primary);
        action_button_->setText(label);
        action_button_->setVisible(state_ != State::Loading);
    }
    layoutChildren();
    updateGeometry();
    update();
}

QString AidaStateView::actionLabel() const
{
    return action_button_ ? action_button_->text() : QString();
}

qreal AidaStateView::contentHeight() const
{
    const auto& t = aida::qt::theme::tokens();
    qreal min_h = qreal(t.shell.min_panel_w);
    if (!message_.isEmpty())
        min_h += qreal(t.spacing.lg);
    if (action_button_)
        min_h += qreal(t.spacing.lg + t.spacing.xl);
    return (std::max)(qreal(t.panel.view_header_h * 2 + t.panel.header_h), min_h);
}

QSize AidaStateView::sizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    return QSize(t.shell.min_panel_w * 3 + t.spacing.section, qRound(contentHeight()));
}

QSize AidaStateView::minimumSizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    return QSize(t.shell.min_panel_w * 2 + t.spacing.sm, qRound(contentHeight()));
}

void AidaStateView::layoutChildren()
{
    const auto& t = aida::qt::theme::tokens();
    const qreal w = rect().width();
    const qreal h = rect().height() > 0 ? qreal(rect().height()) : contentHeight();
    const qreal center_x = w * 0.5;
    const qreal icon_y = qreal(t.spacing.section - t.spacing.xxs);
    if (ring_ && ring_->isVisible()) {
        const int side = t.row.compact;
        ring_->setGeometry(qRound(center_x - side * 0.5), qRound(icon_y - side * 0.5), side, side);
    }
    if (action_button_ && action_button_->isVisible()) {
        const QSize hint = action_button_->sizeHint();
        action_button_->setGeometry(qRound(center_x - hint.width() * 0.5),
            qRound(h - qreal(t.panel.header_h)), hint.width(), t.control.height_sm);
    }
}

void AidaStateView::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const auto& t = aida::qt::theme::tokens();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect());
    const qreal radius = t.radius.md;
    p.setPen(Qt::NoPen);
    p.setBrush(t.bg_elevated);
    p.drawRoundedRect(r, radius, radius);
    paint_border(p, r, radius, t.border_subtle);

    const qreal center_x = r.center().x();
    const qreal icon_r = qreal(t.spacing.sm + t.spacing.xxs);
    const qreal icon_cy = r.top() + qreal(t.spacing.section - t.spacing.xxs);

    if (state_ != State::Loading) {
        const QColor col = state_ == State::Error ? t.error
            : state_ == State::Success ? t.success
            : t.text_dim;
        const qreal disc_r = icon_r + qreal(t.spacing.xxs);
        const QPointF disc_c(center_x, icon_cy);
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(col, 0.10));
        p.drawEllipse(disc_c, disc_r, disc_r);
        QPen disc_edge(with_alpha(col, 0.38), qreal(t.panel.border));
        p.setPen(disc_edge);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(disc_c, disc_r - 0.5, disc_r - 0.5);
        if (state_ == State::Error) {
            QPen mark_pen(col, icon_r * 0.17);
            mark_pen.setCapStyle(Qt::RoundCap);
            p.setPen(mark_pen);
            p.drawLine(QPointF(center_x, icon_cy - icon_r * 0.4),
                QPointF(center_x, icon_cy + icon_r * 0.2));
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            p.drawEllipse(QPointF(center_x, icon_cy + icon_r * 0.5), icon_r * 0.1, icon_r * 0.1);
        } else if (state_ == State::Success) {
            QPen check_pen(col, icon_r * 0.17);
            check_pen.setCapStyle(Qt::RoundCap);
            check_pen.setJoinStyle(Qt::RoundJoin);
            p.setPen(check_pen);
            p.drawLine(QPointF(center_x - icon_r * 0.42, icon_cy),
                QPointF(center_x - icon_r * 0.08, icon_cy + icon_r * 0.34));
            p.drawLine(QPointF(center_x - icon_r * 0.08, icon_cy + icon_r * 0.34),
                QPointF(center_x + icon_r * 0.46, icon_cy - icon_r * 0.30));
        } else {
            QPen dash_pen(col, icon_r * 0.15);
            dash_pen.setCapStyle(Qt::RoundCap);
            p.setPen(dash_pen);
            p.drawLine(QPointF(center_x - icon_r * 0.4, icon_cy),
                QPointF(center_x + icon_r * 0.4, icon_cy));
        }
    }

    const QFont title_font = aida::qt::theme::fonts::h2();
    const QFontMetricsF title_fm(title_font);
    const qreal title_top = icon_cy + icon_r + qreal(t.spacing.sm + t.spacing.xxs);
    p.setFont(title_font);
    p.setPen(t.text_primary);
    const qreal title_avail = r.width() - qreal(t.spacing.md) * 2.0;
    if (title_avail > 0.0) {
        const QString title_drawn = title_fm.elidedText(title_, Qt::ElideRight, title_avail);
        const qreal title_w = title_fm.horizontalAdvance(title_drawn);
        p.drawText(QPointF(center_x - title_w * 0.5, title_top + title_fm.ascent()), title_drawn);
    }

    if (!message_.isEmpty()) {
        const QFont msg_font = aida::qt::theme::fonts::caption();
        const QFontMetricsF msg_fm(msg_font);
        p.setFont(msg_font);
        p.setPen(t.text_secondary);
        const qreal max_w = r.width() - qreal(t.spacing.md) * 2.0;
        const QString msg_drawn = msg_fm.elidedText(message_, Qt::ElideRight, max_w);
        const qreal msg_w = msg_fm.horizontalAdvance(msg_drawn);
        const qreal msg_x = msg_w < max_w ? center_x - msg_w * 0.5 : r.left() + qreal(t.spacing.md);
        const qreal msg_top = title_top + title_fm.height() + qreal(t.spacing.xs);
        p.setClipRect(r.adjusted(qreal(t.spacing.md), 0.0, -qreal(t.spacing.md), 0.0));
        p.drawText(QPointF(msg_x, msg_top + msg_fm.ascent()), msg_drawn);
        p.setClipping(false);
    }
}

void AidaStateView::resizeEvent(QResizeEvent* event)
{
    layoutChildren();
    QWidget::resizeEvent(event);
}

void AidaStateView::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange) {
        updateGeometry();
        update();
    }
    QWidget::changeEvent(event);
}

}
