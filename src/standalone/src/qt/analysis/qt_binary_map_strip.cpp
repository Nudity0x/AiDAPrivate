#include "qt/analysis/qt_binary_map_strip.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QVariantAnimation>

#include <algorithm>

#include "helpers/diag_log.hpp"

#include "qt/analysis/qt_binary_map_shared.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::analysis {

using widgets::mix_colors;
using widgets::paint_focus_ring;
using widgets::with_alpha;

namespace {

QColor section_color(const aida::binary_map::map_section_t& s) {
    const auto& t = theme::tokens();
    if (s.executable && s.writable) return t.warning;
    if (s.executable) return t.error;
    if (s.writable) return t.success;
    return t.info;
}

}

QtSectionStripWidget::QtSectionStripWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.binary_map.strip"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Section strip"));
    setAccessibleDescription(QStringLiteral(
        "Section size bar chart. Left and Right move between sections, Enter jumps to the section address, Shift+Enter opens the section in Hex."));
    const auto& t = theme::tokens();
    setMinimumHeight(stripHeight() + t.spacing.xs + t.spacing.md);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void QtSectionStripWidget::setSections(
    const std::shared_ptr<const aida::binary_map::map_t>& map) {
    map_ = map;
    hover_ = -1;
    if (theme::AidaMotion::reducedMotion()) {
        if (reveal_) reveal_->stop();
        reveal_value_ = 1.0;
        update();
        return;
    }
    reveal_value_ = 0.0;
    if (!reveal_) {
        reveal_ = new QVariantAnimation(this);
        reveal_->setStartValue(0.0);
        reveal_->setEndValue(1.0);
        reveal_->setDuration(theme::tokens().motion.emphasized);
        reveal_->setEasingCurve(QEasingCurve::OutCubic);
        connect(reveal_, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& value) {
            reveal_value_ = value.toReal();
            update();
        });
    }
    reveal_->start();
    update();
}

QRectF QtSectionStripWidget::sectionRect(std::size_t index) const {
    if (!map_ || map_->sections.empty()) return {};
    const auto& t = theme::tokens();
    const auto& sections = map_->sections;
    qreal total_size = 0;
    for (const auto& s : sections) total_size += static_cast<qreal>(s.size);
    if (total_size < 1.0) total_size = 1.0;
    const qreal gap = static_cast<qreal>(t.spacing.xxs);
    const qreal total_w = width() - gap * static_cast<qreal>(sections.size() - 1);
    qreal x = 0;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        qreal sw = total_w * (static_cast<qreal>(sections[i].size) / total_size);
        if (sw < t.row.standard) sw = static_cast<qreal>(t.row.standard);
        if (x + sw > width()) sw = width() - x;
        if (sw <= 1.0) return {};
        if (i == index) return QRectF(x, 0, sw, strip_height_);
        x += sw + gap;
    }
    return {};
}

int QtSectionStripWidget::hitSection(const QPointF& pos) const {
    if (!map_) return -1;
    for (std::size_t i = 0; i < map_->sections.size(); ++i) {
        if (sectionRect(i).contains(pos)) return static_cast<int>(i);
    }
    return -1;
}

void QtSectionStripWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    const auto& t = theme::tokens();
    const qreal radius = static_cast<qreal>(t.radius.sm);
    const qreal cell_pad = static_cast<qreal>(t.spacing.sm);
    painter.setClipRegion(event->region());
    painter.fillRect(rect(), t.bg_base);
    if (!map_ || map_->sections.empty()) {
        painter.setPen(with_alpha(t.text_dim, 1.0));
        painter.drawText(rect().adjusted(t.spacing.sm, t.spacing.sm, 0, 0),
            Qt::AlignLeft | Qt::AlignTop,
            QStringLiteral("(no section table available)"));
        if (hasFocus())
            paint_focus_ring(painter, QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0),
                radius, 1.0);
        return;
    }
    const auto& sections = map_->sections;
    const qreal reveal = reveal_value_;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const auto& s = sections[i];
        const QRectF cell = sectionRect(i);
        if (cell.isEmpty()) break;
        if (!cell.intersects(QRectF(0.0, 0.0, width(), strip_height_)
                    .intersected(event->rect())))
            continue;
        const QRectF revealed(cell.left(), cell.top(), cell.width() * reveal,
            cell.height());
        const QColor base = section_color(s);
        painter.setPen(Qt::NoPen);
        painter.setBrush(base);
        painter.drawRoundedRect(revealed, radius, radius);
        QPen border(with_alpha(t.border_subtle, 1.0), 1.0);
        painter.setPen(border);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(revealed, radius, radius);
        if (static_cast<int>(i) == hover_) {
            painter.setPen(QPen(with_alpha(t.accent_hover, 0.95), 1.2));
            painter.setBrush(with_alpha(t.hover_wash, 0.14));
            painter.drawRoundedRect(cell, radius, radius);
        }
        const QString name = QString::fromStdString(s.name);
        const QFontMetricsF fm(font());
        const qreal text_w = fm.horizontalAdvance(name);
        if (revealed.width() > text_w + t.control.icon_glyph) {
            painter.setPen(with_alpha(t.text_primary, 0.96));
            painter.drawText(revealed.adjusted(cell_pad, 0, 0, 0),
                Qt::AlignVCenter | Qt::AlignLeft, name);
            const QString perm = QString::fromStdString(bm_section_perm_string(s));
            if (revealed.width() - text_w - t.spacing.lg - t.spacing.xxs >
                    t.toolbar.height) {
                painter.setPen(with_alpha(t.text_primary, 0.73));
                painter.drawText(revealed.adjusted(0, 0, -cell_pad, 0),
                    Qt::AlignVCenter | Qt::AlignRight, perm);
            }
        }
    }
    // Entropy sub-row.
    const qreal entropy_y = strip_height_ + t.spacing.xs;
    const qreal track_radius = t.radius.xs * 0.5;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const auto& s = sections[i];
        const QRectF cell = sectionRect(i);
        if (cell.isEmpty()) break;
        const QRectF sub_row(cell.left(), entropy_y, cell.width(),
            t.spacing.xs + t.control.icon_glyph);
        if (!sub_row.intersects(QRectF(event->rect())))
            continue;
        const qreal entropy = bm_section_entropy_normalized(s) * reveal;
        const qreal track_w = cell.width() - 2.0 * t.spacing.xs;
        const QRectF track(cell.left() + t.spacing.xs, entropy_y, track_w,
            t.spacing.xs);
        painter.setPen(Qt::NoPen);
        painter.setBrush(with_alpha(t.border_subtle, 0.55));
        painter.drawRoundedRect(track, track_radius, track_radius);
        const qreal fill_w = (std::max)(0.0, (std::min)(track_w * entropy, track_w));
        if (fill_w > 0.5) {
            const QColor color = s.sampled_bytes > 0
                ? mix_colors(t.success, t.error, entropy)
                : with_alpha(t.text_dim, 0.5);
            painter.setBrush(with_alpha(color, 0.9));
            painter.drawRoundedRect(QRectF(track.left(), track.top(), fill_w,
                    t.spacing.xs), track_radius, track_radius);
        }
        if (s.sampled_bytes > 0) {
            const QString caption = QString::number(
                static_cast<double>(bm_section_entropy_normalized(s)) * 8.0, 'f', 2);
            const QFontMetricsF fm(font());
            if (cell.width() > fm.horizontalAdvance(caption) + t.spacing.lg +
                    t.spacing.xxs) {
                painter.setPen(with_alpha(t.text_secondary, 0.95));
                painter.drawText(QRectF(cell.left(), entropy_y + t.spacing.xs,
                        cell.width() - t.spacing.xs - t.spacing.xxs,
                        t.control.icon_glyph),
                    Qt::AlignRight | Qt::AlignTop, caption);
            }
        }
    }
    if (hasFocus())
        paint_focus_ring(painter, QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0),
            radius, 1.0);
}

void QtSectionStripWidget::mouseMoveEvent(QMouseEvent* event) {
    const int hit = hitSection(event->position());
    if (hit != hover_) {
        hover_ = hit;
        update();
    }
    if (hit >= 0)
        showSectionTooltip(event->globalPosition().toPoint(), hit);
    else
        QToolTip::hideText();
    QWidget::mouseMoveEvent(event);
}

void QtSectionStripWidget::showSectionTooltip(const QPoint& global_pos, int index) {
    if (!map_ || index < 0 || static_cast<std::size_t>(index) >= map_->sections.size())
        return;
    const auto& s = map_->sections[static_cast<std::size_t>(index)];
    const qreal ent01 = bm_section_entropy_normalized(s);
    QString text = QStringLiteral("%1   %2   %3\nVA   0x%4\nEnd  0x%5")
        .arg(QString::fromStdString(s.name))
        .arg(QString::fromStdString(bm_section_perm_string(s)))
        .arg(QString::fromStdString(bm_format_size_human(s.size)))
        .arg(s.va, 0, 16)
        .arg(s.va + s.size, 0, 16);
    if (s.sampled_bytes > 0) {
        const char* verdict =
            (ent01 > 0.94) ? "very high (packed/encrypted)" :
            (ent01 > 0.80) ? "high (compressed)" :
            (ent01 > 0.55) ? "moderate (mixed code/data)" :
            (ent01 > 0.30) ? "low (text/structured)" :
                             "very low (sparse)";
        text += QStringLiteral("\nEntropy %1 bits/byte  (%2%)\nsampled %3 of %4\nVerdict: %5")
            .arg(static_cast<double>(ent01) * 8.0, 0, 'f', 2)
            .arg(static_cast<double>(ent01) * 100.0, 0, 'f', 0)
            .arg(QString::fromStdString(bm_format_size_human(s.sampled_bytes)))
            .arg(QString::fromStdString(bm_format_size_human(s.size)))
            .arg(QString::fromLatin1(verdict));
    } else {
        text += QStringLiteral("\nEntropy: (not sampled)");
    }
    QToolTip::showText(global_pos, text, this);
}

void QtSectionStripWidget::mousePressEvent(QMouseEvent* event) {
    const int hit = hitSection(event->position());
    if (hit < 0) return;
    const auto& s = map_->sections[static_cast<std::size_t>(hit)];
    if (event->button() == Qt::LeftButton) {
        diag::log_tagged_fmt("binary_map",
            "section_strip_click name='%s' va=0x%llX size=%llu perm=%s entropy01=%.3f",
            s.name.c_str(), static_cast<unsigned long long>(s.va),
            static_cast<unsigned long long>(s.size),
            bm_section_perm_string(s).c_str(),
            static_cast<double>(bm_section_entropy_normalized(s)));
        Q_EMIT jumpToAddress(s.va);
    } else if (event->button() == Qt::RightButton) {
        diag::log_tagged_fmt("binary_map",
            "section_strip_right_click_open_hex name='%s' va=0x%llX size=%llu",
            s.name.c_str(), static_cast<unsigned long long>(s.va),
            static_cast<unsigned long long>(s.size));
        Q_EMIT openHex(s.va, bm_hex_request_size(s.size));
    }
    QWidget::mousePressEvent(event);
}

void QtSectionStripWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    const int hit = hitSection(event->position());
    if (hit < 0) return;
    const auto& s = map_->sections[static_cast<std::size_t>(hit)];
    Q_EMIT openHex(s.va, bm_hex_request_size(s.size));
    QWidget::mouseDoubleClickEvent(event);
}

void QtSectionStripWidget::keyPressEvent(QKeyEvent* event) {
    if (!map_ || map_->sections.empty()) {
        QWidget::keyPressEvent(event);
        return;
    }
    const int count = static_cast<int>(map_->sections.size());
    int target = -1;
    switch (event->key()) {
    case Qt::Key_Left:  target = hover_ < 0 ? 0 : hover_ - 1; break;
    case Qt::Key_Right: target = hover_ < 0 ? 0 : hover_ + 1; break;
    case Qt::Key_Home:  target = 0; break;
    case Qt::Key_End:   target = count - 1; break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (hover_ >= 0 && hover_ < count) {
            const auto& section =
                map_->sections[static_cast<std::size_t>(hover_)];
            if (event->modifiers().testFlag(Qt::ShiftModifier))
                Q_EMIT openHex(section.va, bm_hex_request_size(section.size));
            else
                Q_EMIT jumpToAddress(section.va);
            event->accept();
            return;
        }
        break;
    default: break;
    }
    if (target >= 0) {
        target = (std::max)(0, (std::min)(target, count - 1));
        hover_ = target;
        const QRectF cell = sectionRect(static_cast<std::size_t>(target));
        showSectionTooltip(mapToGlobal(cell.center().toPoint()), target);
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void QtSectionStripWidget::leaveEvent(QEvent* event) {
    hover_ = -1;
    QToolTip::hideText();
    update();
    QWidget::leaveEvent(event);
}

}
