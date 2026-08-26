#include "aida_property_row.hpp"

#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include "../theme/aida_fonts.hpp"
#include "../theme/aida_stylesheet.hpp"

namespace aida::qt::widgets {

AidaPropertyRow::AidaPropertyRow(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.property_row"));
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFont(aida::qt::theme::fonts::body());
}

AidaPropertyRow::AidaPropertyRow(const QString& label, const QString& value, QWidget* parent)
    : AidaPropertyRow(parent)
{
    label_ = label;
    value_ = value;
}

void AidaPropertyRow::syncAccessibleName()
{
    setAccessibleName(value_.isEmpty()
        ? label_ : label_ + QStringLiteral(": ") + value_);
}

void AidaPropertyRow::setLabel(const QString& label)
{
    if (label_ == label)
        return;
    label_ = label;
    syncAccessibleName();
    update();
}

void AidaPropertyRow::setValue(const QString& value)
{
    if (value_ == value)
        return;
    value_ = value;
    syncAccessibleName();
    update();
}

void AidaPropertyRow::setValueKind(AidaSemantic kind)
{
    if (value_kind_ == kind)
        return;
    value_kind_ = kind;
    const char* state = semantic_state_name(kind);
    if (state)
        setProperty("aidaState", QString::fromLatin1(state));
    else
        setProperty("aidaState", QVariant());
    aida::qt::theme::stylesheet::repolish(this);
    update();
}

void AidaPropertyRow::setSelectable(bool selectable)
{
    if (selectable_ == selectable)
        return;
    selectable_ = selectable;
    setFocusPolicy(selectable_ ? Qt::TabFocus : Qt::NoFocus);
    setCursor(selectable_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

void AidaPropertyRow::setSelected(bool selected)
{
    if (selected_ == selected)
        return;
    selected_ = selected;
    update();
}

QSize AidaPropertyRow::sizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    return QSize(t.row.property_label_w + t.shell.min_panel_w + t.spacing.md,
        t.row.inspector);
}

QSize AidaPropertyRow::minimumSizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    return QSize(t.spacing.section * 2, t.row.inspector);
}

void AidaPropertyRow::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const auto& t = aida::qt::theme::tokens();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(font());

    const QRectF r = QRectF(rect());
    const qreal radius_xs = t.radius.xs;

    if (selected_) {
        p.setPen(Qt::NoPen);
        p.setBrush(t.selection);
        p.drawRoundedRect(r, radius_xs, radius_xs);
        const qreal bar_w = qreal(t.spacing.xxs);
        p.setBrush(t.selection_strong);
        p.drawRoundedRect(QRectF(r.left() + 1.0, r.top() + 1.0, bar_w, r.height() - 2.0),
            bar_w * 0.5, bar_w * 0.5);
    } else if (selectable_ && hovered_) {
        p.setPen(Qt::NoPen);
        p.setBrush(t.hover_wash);
        p.drawRoundedRect(r, radius_xs, radius_xs);
    }

    if (selectable_ && hasFocus())
        paint_focus_ring_inner(p, r, radius_xs, 0.85);

    QPen base_pen(t.border_subtle, qreal(t.panel.border));
    base_pen.setCosmetic(true);
    base_pen.setCapStyle(Qt::FlatCap);
    p.setPen(base_pen);
    p.drawLine(QPointF(r.left(), r.bottom() - 0.5), QPointF(r.right(), r.bottom() - 0.5));

    const QFontMetricsF fm(font());
    const qreal baseline = text_baseline_centered(r, fm);
    const qreal pad = qreal(t.spacing.sm);
    p.setPen(t.text_secondary);
    const qreal label_w = t.row.property_label_w;
    p.drawText(QPointF(r.left() + pad, baseline),
        fm.elidedText(label_, Qt::ElideRight, label_w - pad * 2.0));

    const QColor value_col = value_kind_ == AidaSemantic::Neutral
        ? t.text_primary
        : semantic_color(value_kind_);
    p.setPen(value_col);
    const qreal vx = r.left() + label_w;
    const qreal value_avail = r.right() - pad - vx;
    if (value_avail > 0.0)
        p.drawText(QPointF(vx, baseline),
            fm.elidedText(value_, Qt::ElideRight, value_avail));
}

void AidaPropertyRow::enterEvent(QEnterEvent* event)
{
    if (selectable_) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void AidaPropertyRow::leaveEvent(QEvent* event)
{
    if (hovered_) {
        hovered_ = false;
        update();
    }
    QWidget::leaveEvent(event);
}

void AidaPropertyRow::keyPressEvent(QKeyEvent* event)
{
    if (selectable_
        && (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter)) {
        Q_EMIT clicked();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void AidaPropertyRow::mouseReleaseEvent(QMouseEvent* event)
{
    if (selectable_ && event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        Q_EMIT clicked();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void AidaPropertyRow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange) {
        updateGeometry();
        update();
    }
    QWidget::changeEvent(event);
}

}
