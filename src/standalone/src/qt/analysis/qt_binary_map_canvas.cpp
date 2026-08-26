#include "qt/analysis/qt_binary_map_canvas.hpp"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>

#include "helpers/diag_log.hpp"

#include "qt/analysis/qt_binary_map_shared.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::analysis {

using widgets::paint_focus_ring;
using widgets::with_alpha;

namespace {

QColor region_color(const qt_binary_map_live_region_t& r) {
    const auto& t = theme::tokens();
    QColor base = t.text_dim;
    if (r.is_guard || r.is_noaccess) base = t.error;
    else if (r.is_stack)             base = t.warning;
    else if (r.is_heap)              base = t.info_soft;
    else if (r.is_image)             base = t.success;
    else if (r.is_mapped)            base = t.info;
    else if (r.is_private && r.is_committed) base = t.accent_dim;
    else if (r.is_reserved)          base = t.text_dim;
    qreal fade = 1.0;
    if (!r.is_committed && !r.is_reserved) fade = 0.32;
    else if (r.is_reserved) fade = 0.55;
    return with_alpha(base, fade);
}

}

QtAddressSpaceCanvas::QtAddressSpaceCanvas(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.binary_map.canvas"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    const auto& tokens = theme::tokens();
    setMinimumSize(static_cast<int>(tokens.shell.min_panel_w) +
        2 * tokens.spacing.section, 3 * tokens.control.height_lg);
}

void QtAddressSpaceCanvas::setContent(
    std::shared_ptr<const qt_binary_map_live_snapshot_t> snapshot,
    quint64 selected_base) {
    snapshot_ = std::move(snapshot);
    selected_base_ = selected_base;
    update();
}

QtAddressSpaceCanvas::transform_t QtAddressSpaceCanvas::computeTransform() const {
    transform_t t;
    const auto& tokens = theme::tokens();
    const QFontMetricsF label_metrics(theme::fonts::codeRegular());
    const qreal gutter_labels = label_metrics.horizontalAdvance(
        QStringLiteral("0xDDDDDDDDDDDD"));
    const qreal canvas_top = static_cast<qreal>(tokens.toolbar.height);
    const qreal canvas_left =
        static_cast<qreal>(tokens.spacing.md + tokens.spacing.xxs);
    const qreal canvas_right = width() - gutter_labels -
        tokens.spacing.md - tokens.spacing.xs;
    const qreal canvas_bottom = height() - tokens.spacing.xxl;
    t.canvas = QRectF(canvas_left, canvas_top,
        (std::max)(static_cast<qreal>(tokens.spacing.xl),
            canvas_right - canvas_left),
        (std::max)(static_cast<qreal>(tokens.spacing.md - tokens.spacing.xxs),
            canvas_bottom - canvas_top));
    if (!snapshot_ || snapshot_->regions.empty()) return t;
    t.va_min = snapshot_->regions.front().base;
    t.va_max = snapshot_->regions.back().base + snapshot_->regions.back().size;
    if (t.va_max <= t.va_min) t.va_max = t.va_min + 1;
    t.full_span = static_cast<double>(t.va_max - t.va_min);
    t.visible_span = t.full_span / static_cast<double>(zoom_);
    t.offset_norm = offset_norm_;
    if (t.offset_norm < 0.0) t.offset_norm = 0.0;
    if (t.offset_norm > 1.0 - 1.0 / static_cast<double>(zoom_)) {
        t.offset_norm = 1.0 - 1.0 / static_cast<double>(zoom_);
        if (t.offset_norm < 0.0) t.offset_norm = 0.0;
    }
    const double view_low_off = t.offset_norm * t.full_span;
    t.view_low = t.va_min + static_cast<std::uint64_t>(view_low_off);
    t.view_high = t.view_low + static_cast<std::uint64_t>(t.visible_span);
    return t;
}

void QtAddressSpaceCanvas::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    const auto& tokens = theme::tokens();
    painter.setClipRegion(event->region());
    painter.fillRect(rect(), tokens.bg_base);
    painter.setPen(with_alpha(tokens.text_secondary, 1.0));
    painter.drawText(rect().adjusted(tokens.spacing.md, tokens.spacing.sm, 0, 0),
        Qt::AlignLeft | Qt::AlignTop, QStringLiteral("Address Space"));
    const auto t = computeTransform();
    if (!snapshot_ || snapshot_->regions.empty()) {
        painter.setPen(with_alpha(tokens.text_dim, 1.0));
        painter.drawText(rect(), Qt::AlignCenter,
            QStringLiteral("Attach to a process or refresh to see live mappings."));
        if (hasFocus())
            paint_focus_ring(painter, QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0),
                static_cast<qreal>(tokens.radius.sm), 1.0);
        return;
    }
    painter.save();
    painter.setClipRect(t.canvas, Qt::IntersectClip);
    painter.setPen(Qt::NoPen);
    painter.setBrush(with_alpha(tokens.panel_header, 0.45));
    painter.drawRect(t.canvas);
    for (std::size_t i = 0; i < snapshot_->regions.size(); ++i) {
        const auto& r = snapshot_->regions[i];
        if (r.base + r.size <= t.view_low) continue;
        if (r.base >= t.view_high) break;
        const std::uint64_t clip_lo = (r.base < t.view_low) ? t.view_low : r.base;
        const std::uint64_t clip_hi =
            ((r.base + r.size) > t.view_high) ? t.view_high : (r.base + r.size);
        if (clip_hi <= clip_lo) continue;
        const double lo_frac = static_cast<double>(clip_lo - t.view_low) / t.visible_span;
        const double hi_frac = static_cast<double>(clip_hi - t.view_low) / t.visible_span;
        const qreal yl = t.canvas.top() + static_cast<qreal>(lo_frac) * t.canvas.height();
        const qreal yh = t.canvas.top() + static_cast<qreal>(hi_frac) * t.canvas.height();
        const qreal h = (yh - yl < tokens.spacing.xxs)
            ? static_cast<qreal>(tokens.spacing.xxs) : (yh - yl);
        const QRectF band(t.canvas.left() + tokens.spacing.xxs, yl,
            t.canvas.width() - 2.0 * tokens.spacing.xxs, h);
        const bool is_hovered = (static_cast<int>(i) == hover_);
        painter.setPen(Qt::NoPen);
        painter.setBrush(region_color(r));
        painter.drawRect(band);
        if (is_hovered) {
            painter.setBrush(with_alpha(tokens.hover_wash, 0.18));
            painter.drawRect(band);
            painter.setPen(QPen(with_alpha(tokens.accent_hover, 0.9), 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(band);
        }
        const bool is_selected = (selected_base_ == r.base && r.base != 0);
        if (is_selected) {
            painter.setPen(QPen(with_alpha(tokens.accent, 1.0), 1.6));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(t.canvas.left(), yl - tokens.panel.border,
                t.canvas.width(), h + 2.0 * tokens.panel.border));
        }
    }
    // Thread RIP markers: one batched drawLines + left triangles.
    QVector<QLineF> marker_lines;
    for (const auto& th : snapshot_->threads) {
        if (th.rip == 0) continue;
        if (th.rip < t.view_low || th.rip >= t.view_high) continue;
        const double frac = static_cast<double>(th.rip - t.view_low) / t.visible_span;
        const qreal y = t.canvas.top() + static_cast<qreal>(frac) * t.canvas.height();
        marker_lines << QLineF(t.canvas.left(), y, t.canvas.right(), y);
    }
    painter.setPen(QPen(with_alpha(tokens.accent_glow, 1.0), 1.4));
    painter.drawLines(marker_lines);
    for (const auto& th : snapshot_->threads) {
        if (th.rip == 0) continue;
        if (th.rip < t.view_low || th.rip >= t.view_high) continue;
        const double frac = static_cast<double>(th.rip - t.view_low) / t.visible_span;
        const qreal y = t.canvas.top() + static_cast<qreal>(frac) * t.canvas.height();
        QPolygonF triangle;
        triangle << QPointF(t.canvas.left() - tokens.radius.md, y - tokens.spacing.xs)
                 << QPointF(t.canvas.left() - tokens.radius.md, y + tokens.spacing.xs)
                 << QPointF(t.canvas.left(), y);
        painter.setPen(Qt::NoPen);
        painter.setBrush(with_alpha(tokens.accent_glow, 1.0));
        painter.drawConvexPolygon(triangle);
    }
    painter.restore();
    // Right gutter: 6 ticks + labels (code font).
    painter.setFont(theme::fonts::codeRegular());
    painter.setPen(with_alpha(tokens.text_dim, 1.0));
    for (int i = 0; i < 6; ++i) {
        const qreal frac = static_cast<qreal>(i) / 5.0;
        const qreal y = t.canvas.top() + frac * t.canvas.height();
        painter.drawLine(QPointF(t.canvas.right() + tokens.spacing.xs, y),
            QPointF(t.canvas.right() + tokens.spacing.sm, y));
        const std::uint64_t va = t.view_low +
            static_cast<std::uint64_t>(t.visible_span * static_cast<double>(frac));
        painter.drawText(QPointF(t.canvas.right() + tokens.spacing.md,
                y + tokens.spacing.xs),
            QStringLiteral("0x%1").arg(va, 12, 16, QLatin1Char('0')).toUpper());
    }
    const qreal footer_y = height() - tokens.spacing.sm;
    const QString zoom_text =
        QStringLiteral("zoom %1x").arg(static_cast<double>(zoom_), 0, 'f', 1);
    painter.drawText(QPointF(tokens.spacing.md, footer_y), zoom_text);
    const QFontMetricsF footer_metrics(theme::fonts::codeRegular());
    const qreal range_x = tokens.spacing.md +
        footer_metrics.horizontalAdvance(QStringLiteral("zoom 4096.0x")) +
        tokens.spacing.lg;
    painter.drawText(QPointF(range_x, footer_y),
        QStringLiteral("0x%1 - 0x%2")
            .arg(t.view_low, 0, 16).arg(t.view_high, 0, 16));
    if (hasFocus())
        paint_focus_ring(painter, QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0),
            static_cast<qreal>(tokens.radius.sm), 1.0);
}

void QtAddressSpaceCanvas::wheelEvent(QWheelEvent* event) {
    if (!snapshot_ || snapshot_->regions.empty()) {
        QWidget::wheelEvent(event);
        return;
    }
    // Verbatim mouse-Y-anchored zoom (07 sec. 6.4). QWheelEvent::angleDelta verified
    // at src/gui/kernel/qevent.h:300.
    const auto t = computeTransform();
    const double delta_steps =
        static_cast<double>(event->angleDelta().y()) / 120.0;
    const double mouse_frac =
        static_cast<double>((event->position().y() - t.canvas.top()) /
            t.canvas.height());
    applyZoomSteps(delta_steps, mouse_frac);
    diag::log_tagged_fmt("binary_map",
        "canvas_zoom new_zoom=%.3f offset_norm=%.4f",
        static_cast<double>(zoom_), offset_norm_);
    event->accept();
}

void QtAddressSpaceCanvas::applyZoomSteps(double delta_steps, double anchor_frac) {
    const auto t = computeTransform();
    double new_zoom = zoom_ * (1.0 + delta_steps * 0.18);
    if (new_zoom < 1.0) new_zoom = 1.0;
    if (new_zoom > 4096.0) new_zoom = 4096.0;
    const double view_low_off = t.offset_norm * t.full_span;
    const double mouse_va_off = view_low_off + t.visible_span * anchor_frac;
    const double new_visible_span = t.full_span / new_zoom;
    double new_low_off = mouse_va_off - new_visible_span * anchor_frac;
    if (new_low_off < 0.0) new_low_off = 0.0;
    if (new_low_off > t.full_span - new_visible_span)
        new_low_off = t.full_span - new_visible_span;
    if (new_low_off < 0.0) new_low_off = 0.0;
    zoom_ = static_cast<float>(new_zoom);
    offset_norm_ = new_low_off / t.full_span;
    update();
}

void QtAddressSpaceCanvas::panByFrac(double frac) {
    const double max_off = 1.0 - 1.0 / static_cast<double>(zoom_);
    double new_off = offset_norm_ + frac;
    if (new_off < 0.0) new_off = 0.0;
    if (new_off > max_off) new_off = max_off;
    offset_norm_ = new_off;
    update();
}

void QtAddressSpaceCanvas::keyPressEvent(QKeyEvent* event) {
    const bool has_data = snapshot_ && !snapshot_->regions.empty();
    switch (event->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        if (has_data) {
            applyZoomSteps(1.0, 0.5);
            event->accept();
            return;
        }
        break;
    case Qt::Key_Minus:
        if (has_data) {
            applyZoomSteps(-1.0, 0.5);
            event->accept();
            return;
        }
        break;
    case Qt::Key_Up:
        if (has_data) {
            panByFrac(-1.0 / (16.0 * static_cast<double>(zoom_)));
            event->accept();
            return;
        }
        break;
    case Qt::Key_Down:
        if (has_data) {
            panByFrac(1.0 / (16.0 * static_cast<double>(zoom_)));
            event->accept();
            return;
        }
        break;
    case Qt::Key_PageUp:
        if (has_data) {
            panByFrac(-0.5 / static_cast<double>(zoom_));
            event->accept();
            return;
        }
        break;
    case Qt::Key_PageDown:
        if (has_data) {
            panByFrac(0.5 / static_cast<double>(zoom_));
            event->accept();
            return;
        }
        break;
    case Qt::Key_Home:
        if (has_data) {
            zoom_ = 1.f;
            offset_norm_ = 0.0;
            update();
            event->accept();
            return;
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (has_data && hover_ >= 0 &&
            static_cast<std::size_t>(hover_) < snapshot_->regions.size()) {
            Q_EMIT regionSelected(
                snapshot_->regions[static_cast<std::size_t>(hover_)].base);
            event->accept();
            return;
        }
        break;
    default: break;
    }
    QWidget::keyPressEvent(event);
}

int QtAddressSpaceCanvas::regionAt(const QPointF& pos,
                                   const transform_t& t) const {
    if (!snapshot_ || !t.canvas.contains(pos)) return -1;
    for (std::size_t i = 0; i < snapshot_->regions.size(); ++i) {
        const auto& r = snapshot_->regions[i];
        if (r.base + r.size <= t.view_low) continue;
        if (r.base >= t.view_high) break;
        const std::uint64_t clip_lo = (r.base < t.view_low) ? t.view_low : r.base;
        const std::uint64_t clip_hi =
            ((r.base + r.size) > t.view_high) ? t.view_high : (r.base + r.size);
        if (clip_hi <= clip_lo) continue;
        const double lo_frac = static_cast<double>(clip_lo - t.view_low) / t.visible_span;
        const double hi_frac = static_cast<double>(clip_hi - t.view_low) / t.visible_span;
        const qreal yl = t.canvas.top() + static_cast<qreal>(lo_frac) * t.canvas.height();
        const qreal yh = t.canvas.top() + static_cast<qreal>(hi_frac) * t.canvas.height();
        const qreal h = (yh - yl < 2.0) ? 2.0 : (yh - yl);
        if (pos.y() >= yl && pos.y() <= yl + h) return static_cast<int>(i);
    }
    return -1;
}

void QtAddressSpaceCanvas::showRegionTooltip(const QPoint& global_pos, int index) {
    if (!snapshot_ || index < 0 ||
        static_cast<std::size_t>(index) >= snapshot_->regions.size())
        return;
    const auto& r = snapshot_->regions[static_cast<std::size_t>(index)];
    const QString tip = QStringLiteral("%1\n0x%2 - 0x%3\n%4 | %5 | %6\n%7")
        .arg(r.module_name.empty()
            ? QString::fromStdString(bm_region_kind_label(r))
            : QString::fromStdString(r.module_name))
        .arg(r.base, 16, 16, QLatin1Char('0'))
        .arg(r.base + r.size, 16, 16, QLatin1Char('0'))
        .arg(QString::fromStdString(bm_format_protect_word(r.protect)))
        .arg(QString::fromStdString(bm_format_state_word(r.state)))
        .arg(QString::fromStdString(bm_format_type_word(r.type)))
        .arg(QString::fromStdString(bm_format_size_human(r.size)));
    QToolTip::showText(global_pos, tip, this);
}

void QtAddressSpaceCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        drag_anchor_ = event->position().y();
        drag_offset_start_ = offset_norm_;
    }
    QWidget::mousePressEvent(event);
}

void QtAddressSpaceCanvas::mouseMoveEvent(QMouseEvent* event) {
    const auto t = computeTransform();
    if (dragging_) {
        // The /zoom speed quirk is preserved verbatim (07 sec. 6.4).
        const qreal dy = event->position().y() - drag_anchor_;
        const double dy_frac = static_cast<double>(-dy / t.canvas.height()) /
            static_cast<double>(zoom_);
        double new_off = drag_offset_start_ + dy_frac;
        const double max_off = 1.0 - 1.0 / static_cast<double>(zoom_);
        if (new_off < 0.0) new_off = 0.0;
        if (new_off > max_off) new_off = max_off;
        offset_norm_ = new_off;
        update();
        return;
    }
    const int hit = regionAt(event->position(), t);
    if (hit != hover_) {
        hover_ = hit;
        update();
    }
    if (hit >= 0)
        showRegionTooltip(event->globalPosition().toPoint(), hit);
    else
        QToolTip::hideText();
    QWidget::mouseMoveEvent(event);
}

void QtAddressSpaceCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (dragging_) {
            dragging_ = false;
            diag::log_tagged_fmt("binary_map",
                "canvas_drag_release offset_norm=%.4f zoom=%.3f",
                offset_norm_, static_cast<double>(zoom_));
            const auto t = computeTransform();
            const int hit = regionAt(event->position(), t);
            const qreal moved = std::abs(event->position().y() - drag_anchor_);
            if (hit >= 0 && moved < theme::tokens().spacing.xs) {
                const auto& r = snapshot_->regions[static_cast<std::size_t>(hit)];
                diag::log_tagged_fmt("binary_map",
                    "canvas_select base=0x%llX size=%llu kind=%s",
                    static_cast<unsigned long long>(r.base),
                    static_cast<unsigned long long>(r.size),
                    bm_region_kind_label(r).c_str());
                Q_EMIT regionSelected(r.base);
            }
        }
    }
    QWidget::mouseReleaseEvent(event);
}

void QtAddressSpaceCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    const auto t = computeTransform();
    const int hit = regionAt(event->position(), t);
    if (hit >= 0) {
        const auto& r = snapshot_->regions[static_cast<std::size_t>(hit)];
        diag::log_tagged_fmt("binary_map",
            "canvas_double_click base=0x%llX -> jump_to_disasm",
            static_cast<unsigned long long>(r.base));
        Q_EMIT regionDoubleClicked(r.base);
    }
    QWidget::mouseDoubleClickEvent(event);
}

void QtAddressSpaceCanvas::leaveEvent(QEvent* event) {
    hover_ = -1;
    QToolTip::hideText();
    update();
    QWidget::leaveEvent(event);
}

}
