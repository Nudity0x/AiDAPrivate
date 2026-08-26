#include "aida_headers.hpp"
#include <algorithm>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include "../theme/aida_fonts.hpp"
#include "aida_button.hpp"

namespace aida::qt::widgets {

AidaSectionHeader::AidaSectionHeader(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.section_header"));
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setProperty("aidaRole", QStringLiteral("header"));
    setFont(aida::qt::theme::fonts::bodyEm());
}

AidaSectionHeader::AidaSectionHeader(const QString& title, QWidget* parent)
    : AidaSectionHeader(parent)
{
    title_ = title;
}

void AidaSectionHeader::setTitle(const QString& title)
{
    if (title_ == title)
        return;
    title_ = title;
    setAccessibleName(title_);
    updateGeometry();
    update();
}

void AidaSectionHeader::setCountLabel(const QString& count)
{
    if (count_label_ == count)
        return;
    count_label_ = count;
    update();
}

void AidaSectionHeader::setActionLabel(const QString& action)
{
    if (action_label_ == action)
        return;
    action_label_ = action;
    action_hot_ = false;
    action_armed_ = false;
    setFocusPolicy(action_label_.isEmpty() ? Qt::NoFocus : Qt::TabFocus);
    update();
}

qreal AidaSectionHeader::rowHeight() const
{
    const auto& t = aida::qt::theme::tokens();
    const QFontMetricsF fm(font());
    return (std::max)(qreal(t.control.height_md),
        fm.height() + qreal(t.spacing.md + t.spacing.xxs));
}

QSize AidaSectionHeader::sizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    return QSize(t.shell.min_panel_w * 2 + t.spacing.sm, qRound(rowHeight()));
}

QSize AidaSectionHeader::minimumSizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    return QSize(t.shell.min_panel_w / 2, qRound(rowHeight()));
}

QRectF AidaSectionHeader::actionRect() const
{
    if (action_label_.isEmpty())
        return QRectF();
    const auto& t = aida::qt::theme::tokens();
    const QFontMetricsF fm(font());
    const qreal aw = fm.horizontalAdvance(action_label_);
    const qreal h = rowHeight();
    const qreal pad = qreal(t.spacing.sm - t.spacing.xxs);
    return QRectF(rect().right() - aw - qreal(t.spacing.md) - pad, qreal(t.spacing.xs),
        aw + qreal(t.spacing.md), h - qreal(t.spacing.xs) * 2.0);
}

void AidaSectionHeader::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const auto& t = aida::qt::theme::tokens();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(font());

    const QRectF r = QRectF(rect());
    const qreal bg_radius = qreal(t.radius.md);
    p.setPen(Qt::NoPen);
    p.setBrush(with_alpha(t.panel_header, 0.55));
    p.drawRoundedRect(r, bg_radius, bg_radius);

    QPen line_pen(with_alpha(t.border_subtle, 0.75), qreal(t.panel.border));
    line_pen.setCosmetic(true);
    line_pen.setCapStyle(Qt::FlatCap);
    p.setPen(line_pen);
    p.drawLine(QPointF(r.left() + bg_radius, r.bottom() - 0.5),
        QPointF(r.right() - bg_radius, r.bottom() - 0.5));

    const QFontMetricsF fm(font());
    const qreal baseline = text_baseline_centered(QRectF(r.topLeft(), QSizeF(r.width(), rowHeight())), fm);
    const qreal text_left = r.left() + qreal(t.spacing.md);
    qreal text_right = r.right() - qreal(t.spacing.md);
    QRectF ar;
    if (!action_label_.isEmpty()) {
        ar = actionRect();
        text_right = ar.left() - qreal(t.spacing.sm);
    }

    qreal count_w = 0.0;
    if (!count_label_.isEmpty())
        count_w = fm.horizontalAdvance(count_label_) + qreal(t.spacing.xs);

    qreal title_avail = text_right - text_left - count_w;
    if (title_avail < 0.0)
        title_avail = 0.0;
    const QString title_drawn = fm.elidedText(title_, Qt::ElideRight, title_avail);
    p.setPen(t.text_primary);
    p.drawText(QPointF(text_left, baseline), title_drawn);

    if (!count_label_.isEmpty()) {
        const qreal count_x = text_left + fm.horizontalAdvance(title_drawn) + qreal(t.spacing.xs);
        const qreal count_avail = text_right - count_x;
        if (count_avail > 0.0) {
            p.setPen(t.text_dim);
            p.drawText(QPointF(count_x, baseline),
                fm.elidedText(count_label_, Qt::ElideRight, count_avail));
        }
    }

    if (!action_label_.isEmpty()) {
        const qreal action_radius = qreal(t.radius.sm);
        if (action_hot_) {
            p.setPen(Qt::NoPen);
            p.setBrush(t.hover_wash);
            p.drawRoundedRect(ar, action_radius, action_radius);
        }
        if (hasFocus())
            paint_focus_ring(p, ar, action_radius, 0.85);
        p.setPen(action_hot_ ? t.text_primary : t.text_secondary);
        const qreal action_pad = qreal(t.spacing.sm - t.spacing.xxs);
        const qreal action_avail = ar.width() - action_pad * 2.0;
        if (action_avail > 0.0)
            p.drawText(QPointF(ar.left() + action_pad, baseline),
                fm.elidedText(action_label_, Qt::ElideRight, action_avail));
    }
}

void AidaSectionHeader::keyPressEvent(QKeyEvent* event)
{
    if (!action_label_.isEmpty()
        && (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter)) {
        Q_EMIT actionTriggered();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void AidaSectionHeader::mouseMoveEvent(QMouseEvent* event)
{
    const bool hot = !action_label_.isEmpty() && actionRect().contains(event->position());
    if (hot != action_hot_) {
        action_hot_ = hot;
        setCursor(hot ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void AidaSectionHeader::mousePressEvent(QMouseEvent* event)
{
    if (!action_label_.isEmpty() && event->button() == Qt::LeftButton
        && actionRect().contains(event->position())) {
        action_armed_ = true;
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void AidaSectionHeader::mouseReleaseEvent(QMouseEvent* event)
{
    if (action_armed_) {
        action_armed_ = false;
        if (event->button() == Qt::LeftButton && actionRect().contains(event->position()))
            Q_EMIT actionTriggered();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void AidaSectionHeader::leaveEvent(QEvent* event)
{
    if (action_hot_) {
        action_hot_ = false;
        unsetCursor();
        update();
    }
    QWidget::leaveEvent(event);
}

void AidaSectionHeader::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange) {
        updateGeometry();
        update();
    }
    QWidget::changeEvent(event);
}

AidaViewHeader::AidaViewHeader(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.view_header"));
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setProperty("aidaRole", QStringLiteral("header"));
}

AidaViewHeader::AidaViewHeader(const QString& title, const QString& subtitle, QWidget* parent)
    : AidaViewHeader(parent)
{
    title_ = title;
    subtitle_ = subtitle;
}

void AidaViewHeader::setTitle(const QString& title)
{
    if (title_ == title)
        return;
    title_ = title;
    setAccessibleName(title_);
    updateGeometry();
    update();
}

void AidaViewHeader::setSubtitle(const QString& subtitle)
{
    if (subtitle_ == subtitle)
        return;
    subtitle_ = subtitle;
    updateGeometry();
    update();
}

void AidaViewHeader::setStatus(AidaSemantic status)
{
    if (status_ == status)
        return;
    status_ = status;
    update();
}

void AidaViewHeader::setPrimaryAction(const QString& label)
{
    if (label.isEmpty()) {
        if (primary_button_) {
            primary_button_->hide();
            primary_button_->deleteLater();
            primary_button_ = nullptr;
        }
    } else {
        if (!primary_button_) {
            primary_button_ = new AidaButton(this);
            primary_button_->setKind(AidaButton::Kind::Primary);
            primary_button_->setControlSize(AidaButton::ControlSize::Small);
            connect(primary_button_, &QAbstractButton::clicked, this,
                [this] { Q_EMIT primaryActionTriggered(); });
        }
        primary_button_->setText(label);
        primary_button_->show();
    }
    layoutButtons();
    updateGeometry();
}

void AidaViewHeader::setSecondaryAction(const QString& label)
{
    if (label.isEmpty()) {
        if (secondary_button_) {
            secondary_button_->hide();
            secondary_button_->deleteLater();
            secondary_button_ = nullptr;
        }
    } else {
        if (!secondary_button_) {
            secondary_button_ = new AidaButton(this);
            secondary_button_->setKind(AidaButton::Kind::Ghost);
            secondary_button_->setControlSize(AidaButton::ControlSize::Small);
            connect(secondary_button_, &QAbstractButton::clicked, this,
                [this] { Q_EMIT secondaryActionTriggered(); });
        }
        secondary_button_->setText(label);
        secondary_button_->show();
    }
    layoutButtons();
    updateGeometry();
}

qreal AidaViewHeader::headerHeight() const
{
    const auto& t = aida::qt::theme::tokens();
    return subtitle_.isEmpty() ? qreal(t.panel.header_h) : qreal(t.panel.view_header_h);
}

QSize AidaViewHeader::sizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    return QSize(t.shell.min_panel_w * 3 + t.spacing.section, qRound(headerHeight()));
}

QSize AidaViewHeader::minimumSizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    return QSize(t.shell.min_panel_w * 2 - t.spacing.section, qRound(headerHeight()));
}

void AidaViewHeader::layoutButtons()
{
    const auto& t = aida::qt::theme::tokens();
    const qreal h = rect().height() > 0 ? qreal(rect().height()) : headerHeight();
    const qreal action_h = t.control.height_sm;
    const qreal y = (h - action_h) * 0.5;
    qreal x = rect().right() - qreal(t.spacing.md);
    if (primary_button_ && primary_button_->isVisible()) {
        const QSize hint = primary_button_->sizeHint();
        x -= hint.width();
        primary_button_->setGeometry(qRound(x), qRound(y), hint.width(), qRound(action_h));
        x -= qreal(t.spacing.sm);
    }
    if (secondary_button_ && secondary_button_->isVisible()) {
        const QSize hint = secondary_button_->sizeHint();
        x -= hint.width();
        secondary_button_->setGeometry(qRound(x), qRound(y), hint.width(), qRound(action_h));
    }
}

void AidaViewHeader::paintEvent(QPaintEvent* event)
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

    const QColor marker = status_ == AidaSemantic::Neutral ? t.accent : semantic_color(status_);
    const qreal marker_w = qreal(t.radius.xs);
    p.setPen(Qt::NoPen);
    p.setBrush(marker);
    p.drawRoundedRect(QRectF(r.left() + qreal(t.panel.border), r.top() + qreal(t.spacing.sm),
        marker_w, r.height() - qreal(t.spacing.sm) * 2.0), marker_w * 0.5, marker_w * 0.5);

    const qreal text_x = r.left() + qreal(t.spacing.lg);
    const QFont title_font = aida::qt::theme::fonts::h1();
    const QFontMetricsF title_fm(title_font);
    const bool has_sub = !subtitle_.isEmpty();
    const qreal title_y = has_sub
        ? r.top() + qreal(t.spacing.sm + t.spacing.xxs) + title_fm.ascent()
        : text_baseline_centered(r, title_fm);
    p.setFont(title_font);
    p.setPen(t.text_primary);
    qreal avail = r.right() - qreal(t.spacing.md) - text_x;
    if (secondary_button_ && secondary_button_->isVisible())
        avail = secondary_button_->geometry().left() - qreal(t.spacing.sm) - text_x;
    if (primary_button_ && primary_button_->isVisible())
        avail = (std::min)(avail, primary_button_->geometry().left() - qreal(t.spacing.sm) - text_x);
    if (avail > 0.0)
        p.drawText(QPointF(text_x, title_y), title_fm.elidedText(title_, Qt::ElideRight, avail));

    if (has_sub) {
        const QFont sub_font = aida::qt::theme::fonts::caption();
        const QFontMetricsF sub_fm(sub_font);
        const qreal sub_y = r.top() + qreal(t.spacing.sm + t.spacing.xxs)
            + title_fm.height() + qreal(t.spacing.xxs) + sub_fm.ascent();
        p.setFont(sub_font);
        p.setPen(t.text_secondary);
        if (avail > 0.0)
            p.drawText(QPointF(text_x, sub_y),
                sub_fm.elidedText(subtitle_, Qt::ElideRight, avail));
    }
}

void AidaViewHeader::resizeEvent(QResizeEvent* event)
{
    layoutButtons();
    QWidget::resizeEvent(event);
}

void AidaViewHeader::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange) {
        updateGeometry();
        update();
    }
    QWidget::changeEvent(event);
}

}
