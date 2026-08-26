#include "qt/debugger/rflags_chips_widget.hpp"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::debugger {

const RflagsChipsWidget::chip_t RflagsChipsWidget::k_chips[12] = {
    {"CF", "Carry",      0x00000001ULL},
    {"PF", "Parity",     0x00000004ULL},
    {"AF", "Aux Carry",  0x00000010ULL},
    {"ZF", "Zero",       0x00000040ULL},
    {"SF", "Sign",       0x00000080ULL},
    {"OF", "Overflow",   0x00000800ULL},
    {"TF", "Trap",       0x00000100ULL},
    {"IF", "Interrupt",  0x00000200ULL},
    {"DF", "Direction",  0x00000400ULL},
    {"NT", "Nested",     0x00004000ULL},
    {"RF", "Resume",     0x00010000ULL},
    {"AC", "AlignCheck", 0x00040000ULL},
};

RflagsChipsWidget::RflagsChipsWidget(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    const auto& t = theme::tokens();
    setMinimumHeight(2 * t.control.height_sm + 3 * t.spacing.xs);
}

void RflagsChipsWidget::setRflags(std::uint64_t rflags) {
    if (rflags_ == rflags)
        return;
    rflags_ = rflags;
    update();
}

QSize RflagsChipsWidget::sizeHint() const {
    const auto& t = theme::tokens();
    return {t.row.property_label_w * 2,
        6 * t.control.height_sm + 7 * t.spacing.xs};
}

QSize RflagsChipsWidget::minimumSizeHint() const {
    const auto& t = theme::tokens();
    return {static_cast<int>(t.shell.min_panel_w),
        2 * t.control.height_sm + 3 * t.spacing.xs};
}

QRectF RflagsChipsWidget::chipRect(int index) const {
    const auto& t = theme::tokens();
    const int pad = t.spacing.xs;
    const int columns = 2;
    const qreal chip_w = (width() - pad * (columns + 1)) / 2.0;
    const qreal chip_h = t.control.height_sm;
    const int col = index % columns;
    const int row = index / columns;
    return QRectF(pad + col * (chip_w + pad), pad + row * (chip_h + pad),
        chip_w, chip_h);
}

int RflagsChipsWidget::chipAt(const QPoint& pos) const {
    for (int i = 0; i < 12; ++i) {
        if (chipRect(i).contains(pos))
            return i;
    }
    return -1;
}

void RflagsChipsWidget::toggleChip(int index) {
    if (index < 0 || index >= 12)
        return;
    const auto& def = k_chips[index];
    Q_EMIT flagToggleRequested(QString::fromLatin1(def.full_name), rflags_,
        rflags_ ^ def.mask);
}

void RflagsChipsWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const int chip = chipAt(event->pos());
        if (chip >= 0) {
            current_chip_ = chip;
            toggleChip(chip);
            update();
        }
    }
    QWidget::mousePressEvent(event);
}

void RflagsChipsWidget::mouseMoveEvent(QMouseEvent* event) {
    const int chip = chipAt(event->pos());
    if (chip != hovered_chip_) {
        hovered_chip_ = chip;
        update();
    }
    if (chip >= 0) {
        const auto& def = k_chips[chip];
        int bit = 0;
        for (std::uint64_t mask = def.mask; mask != 0; mask >>= 1, ++bit) {
            if (mask & 1ULL)
                break;
        }
        const QString tip = QStringLiteral(
            "%1 \u2014 %2 flag (bit %3) = %4. Click to toggle.")
            .arg(QString::fromLatin1(def.short_name),
                QString::fromLatin1(def.full_name))
            .arg(bit)
            .arg((rflags_ & def.mask) != 0 ? 1 : 0);
        if (toolTip() != tip)
            setToolTip(tip);
    } else if (!toolTip().isEmpty()) {
        setToolTip(QString());
    }
    QWidget::mouseMoveEvent(event);
}

void RflagsChipsWidget::leaveEvent(QEvent* event) {
    if (hovered_chip_ != -1) {
        hovered_chip_ = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

void RflagsChipsWidget::keyPressEvent(QKeyEvent* event) {
    int next = -1;
    switch (event->key()) {
        case Qt::Key_Left:
            next = current_chip_ % 2 == 1 ? current_chip_ - 1 : current_chip_;
            break;
        case Qt::Key_Right:
            next = current_chip_ % 2 == 0 ? current_chip_ + 1 : current_chip_;
            break;
        case Qt::Key_Up:
            next = (std::max)(0, current_chip_ - 2);
            break;
        case Qt::Key_Down:
            next = (std::min)(11, current_chip_ + 2);
            break;
        case Qt::Key_Home:
            next = 0;
            break;
        case Qt::Key_End:
            next = 11;
            break;
        case Qt::Key_Space:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            toggleChip(current_chip_);
            event->accept();
            return;
        default:
            break;
    }
    if (next >= 0) {
        current_chip_ = next;
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RflagsChipsWidget::focusInEvent(QFocusEvent* event) {
    update();
    QWidget::focusInEvent(event);
}

void RflagsChipsWidget::focusOutEvent(QFocusEvent* event) {
    update();
    QWidget::focusOutEvent(event);
}

void RflagsChipsWidget::paintEvent(QPaintEvent* event) {
    (void)event;
    QPainter painter(this);
    const auto& t = theme::tokens();
    painter.fillRect(rect(), t.bg_base);

    const QFont name_font = theme::fonts::bodyEm();
    const QFont detail_font = theme::fonts::caption();
    const QFont bit_font = theme::fonts::codeRegular();
    const QFontMetricsF name_fm(name_font);
    const QFontMetricsF detail_fm(detail_font);
    const QFontMetricsF bit_fm(bit_font);
    const qreal radius = t.radius.md;
    const qreal led_cx_off = t.spacing.md + 2.0;
    const qreal name_x_off = t.spacing.lg + 12.0;
    const qreal detail_x_off = name_x_off +
        (std::max)(name_fm.horizontalAdvance(QStringLiteral("AC")),
            name_fm.horizontalAdvance(QStringLiteral("RF"))) +
        t.spacing.sm;

    for (int i = 0; i < 12; ++i) {
        const auto& def = k_chips[i];
        const bool set_bit = (rflags_ & def.mask) != 0;
        const QRectF r = chipRect(i);

        QColor bg = set_bit ? widgets::mix_colors(t.panel_header, t.success, 0.35)
                            : t.panel_header;
        if (i == hovered_chip_)
            bg = widgets::mix_colors(bg, t.hover_wash, 0.55);
        painter.setPen(Qt::NoPen);
        painter.setBrush(bg);
        painter.drawRoundedRect(r, radius, radius);
        painter.setPen(QPen(widgets::with_alpha(
            set_bit ? widgets::mix_colors(t.success, t.accent, 0.25)
                    : t.border_subtle,
            set_bit ? 0.95 : 0.65), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(r, radius, radius);

        const QColor led = set_bit ? t.success : t.text_dim;
        painter.setPen(Qt::NoPen);
        painter.setBrush(widgets::with_alpha(led, 0.30));
        painter.drawEllipse(QPointF(r.left() + led_cx_off, r.center().y()), 5, 5);
        painter.setBrush(widgets::with_alpha(led, set_bit ? 1.0 : 0.55));
        painter.drawEllipse(QPointF(r.left() + led_cx_off, r.center().y()), 3, 3);

        painter.setPen(set_bit ? t.text_primary : t.text_secondary);
        painter.setFont(name_font);
        painter.drawText(QPointF(r.left() + name_x_off,
                widgets::text_baseline_centered(r, name_fm)),
            QString::fromLatin1(def.short_name));

        const QString bit = set_bit ? QStringLiteral("1") : QStringLiteral("0");
        const qreal bit_w = bit_fm.horizontalAdvance(bit);
        painter.setFont(detail_font);
        painter.setPen(widgets::with_alpha(t.text_dim, 0.95));
        const qreal detail_x = r.left() + detail_x_off;
        const qreal detail_avail = r.right() - bit_w - t.spacing.md - detail_x;
        if (detail_avail > t.spacing.lg) {
            const QString detail = QString::fromLatin1(def.full_name);
            painter.drawText(QPointF(detail_x,
                    widgets::text_baseline_centered(r, detail_fm)),
                detail_fm.horizontalAdvance(detail) > detail_avail
                    ? detail_fm.elidedText(detail, Qt::ElideRight, detail_avail)
                    : detail);
        }

        painter.setFont(bit_font);
        painter.setPen(set_bit ? t.success : t.text_dim);
        painter.drawText(QPointF(r.right() - bit_w - t.spacing.sm,
                widgets::text_baseline_centered(r, bit_fm)), bit);

        if (i == current_chip_ && hasFocus()) {
            painter.setPen(QPen(widgets::with_alpha(t.border_focus, 0.95),
                static_cast<qreal>(t.control.focus_ring)));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(r.adjusted(1.0, 1.0, -1.0, -1.0),
                radius, radius);
        }
    }
}

}
