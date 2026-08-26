#include "qt/analysis/qt_binary_map_heatmap.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <cmath>

#include "helpers/diag_log.hpp"

#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::analysis {

using widgets::mix_colors;
using widgets::paint_focus_ring;
using widgets::with_alpha;

QtFunctionHeatmapWidget::QtFunctionHeatmapWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.binary_map.heatmap"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    const auto& t = theme::tokens();
    setMinimumHeight(t.control.height_lg + t.spacing.xl);
    pulse_timer_ = new QTimer(this);
    pulse_timer_->setInterval(16);
    connect(pulse_timer_, &QTimer::timeout, this,
            [this] { tickPulses(); });
}

void QtFunctionHeatmapWidget::setFunctions(
    const std::shared_ptr<const aida::binary_map::map_t>& map, quint64 selected_va) {
    map_ = map;
    selected_va_ = selected_va;
    max_xrefs_ = 0;
    if (map_) {
        for (const auto& fn : map_->functions)
            if (fn.xref_count > max_xrefs_) max_xrefs_ = fn.xref_count;
        if (max_xrefs_ <= 0) max_xrefs_ = 1;
    }
    pulses_.clear();
    pulse_timer_->stop();
    update();
}

QRectF QtFunctionHeatmapWidget::cellRect(int index) const {
    const auto& t = theme::tokens();
    const qreal cell = static_cast<qreal>(t.control.icon_glyph);
    const qreal gap = static_cast<qreal>(t.spacing.xxs);
    const int cols = columnCount();
    const int row = index / cols;
    const int column = index % cols;
    return QRectF(static_cast<qreal>(column) * (cell + gap),
        static_cast<qreal>(row) * (cell + gap), cell, cell);
}

int QtFunctionHeatmapWidget::columnCount() const {
    const auto& t = theme::tokens();
    const qreal cell = static_cast<qreal>(t.control.icon_glyph);
    const qreal gap = static_cast<qreal>(t.spacing.xxs);
    return (std::max)(4, static_cast<int>((width() + gap) / (cell + gap)));
}

void QtFunctionHeatmapWidget::seedHover(int hit) {
    if (hit < 0 || !map_ || hit >= static_cast<int>(map_->functions.size())) return;
    const quint64 va = map_->functions[static_cast<std::size_t>(hit)].va;
    if (theme::AidaMotion::reducedMotion()) {
        pulses_.clear();
        pulses_.emplace(va, 1.0);
        return;
    }
    pulses_.try_emplace(va, 0.0);
    if (!pulse_timer_->isActive()) pulse_timer_->start();
}

void QtFunctionHeatmapWidget::showFunctionTooltip(const QPoint& global_pos,
                                                  int index) {
    if (!map_ || index < 0 || index >= static_cast<int>(map_->functions.size()))
        return;
    const auto& fn = map_->functions[static_cast<std::size_t>(index)];
    QString tip = QStringLiteral("%1\n0x%2\nxrefs %3   callees %4")
        .arg(QString::fromStdString(fn.name))
        .arg(fn.va, 0, 16)
        .arg(fn.xref_count)
        .arg(fn.callee_count);
    if (!fn.section_name.empty())
        tip += QStringLiteral("\nin %1").arg(QString::fromStdString(fn.section_name));
    QToolTip::showText(global_pos, tip, this);
}

int QtFunctionHeatmapWidget::hitCell(const QPointF& pos) const {
    if (!map_ || pos.x() < 0.0 || pos.y() < 0.0) return -1;
    const auto& t = theme::tokens();
    const qreal cell = static_cast<qreal>(t.control.icon_glyph);
    const qreal gap = static_cast<qreal>(t.spacing.xxs);
    const int cols = (std::max)(4,
        static_cast<int>((width() + gap) / (cell + gap)));
    const int column = static_cast<int>(pos.x() / (cell + gap));
    const int row = static_cast<int>(pos.y() / (cell + gap));
    if (column >= cols) return -1;
    const int index = row * cols + column;
    if (index < 0 || index >= static_cast<int>(map_->functions.size())) return -1;
    return cellRect(index).contains(pos) ? index : -1;
}

void QtFunctionHeatmapWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    const auto& t = theme::tokens();
    painter.fillRect(rect(), t.bg_base);
    if (!map_ || map_->functions.empty()) {
        painter.setPen(with_alpha(t.text_dim, 1.0));
        painter.drawText(rect(), Qt::AlignCenter,
            QStringLiteral("No functions available"));
        if (hasFocus())
            paint_focus_ring(painter, QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0),
                static_cast<qreal>(t.radius.sm), 1.0);
        return;
    }
    const QRectF exposed(event->rect());
    const int count = static_cast<int>(map_->functions.size());
    for (int i = 0; i < count; ++i) {
        const auto& fn = map_->functions[static_cast<std::size_t>(i)];
        const QRectF cell = cellRect(i);
        if (cell.bottom() > height()) break;
        if (!cell.intersects(exposed)) continue;
        const qreal v = (std::min)(1.0,
            static_cast<qreal>(fn.xref_count) / static_cast<qreal>(max_xrefs_));
        QColor fill = v < 0.5
            ? mix_colors(t.info_soft, t.accent_dim, v * 2.0)
            : mix_colors(t.accent_dim, t.warning, (v - 0.5) * 2.0);
        fill = with_alpha(fill, 0.45 + v * 0.55);
        const bool selected = selected_va_ == fn.va && fn.va != 0;
        const qreal cell_radius = static_cast<qreal>(t.radius.xs);
        if (fn.pinned) {
            painter.setPen(QPen(with_alpha(t.accent, 0.85), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(cell.adjusted(-t.panel.border, -t.panel.border,
                t.panel.border, t.panel.border), cell_radius, cell_radius);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(cell, cell_radius, cell_radius);
        painter.setPen(QPen(selected ? with_alpha(t.accent, 1.0)
            : with_alpha(t.border_subtle, 0.6), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(cell, cell_radius, cell_radius);
        const auto pulse_it = pulses_.find(fn.va);
        if (pulse_it != pulses_.end() && pulse_it->second > 0.01) {
            const qreal pulse = pulse_it->second;
            const qreal exp = pulse * t.spacing.xxs;
            painter.setPen(QPen(with_alpha(t.accent_hover, 0.9 * pulse), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(
                cell.adjusted(-exp, -exp, exp, exp),
                static_cast<qreal>(t.radius.sm), static_cast<qreal>(t.radius.sm));
        }
    }
    if (hasFocus())
        paint_focus_ring(painter, QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0),
            static_cast<qreal>(t.radius.sm), 1.0);
}

void QtFunctionHeatmapWidget::tickPulses() {
    // smooth_lerp rates 18 (rise) / 12 (fall) ported from
    // binary_map_view.hpp:2366-2368 with a fixed 16 ms step. Reduced-motion
    // collapses every pulse to its settled state in one tick.
    if (theme::AidaMotion::reducedMotion()) {
        pulses_.clear();
        pulse_timer_->stop();
        update();
        return;
    }
    constexpr qreal dt = 0.016;
    quint64 hovered_va = 0;
    if (hover_ >= 0 && map_ &&
        hover_ < static_cast<int>(map_->functions.size())) {
        hovered_va = map_->functions[static_cast<std::size_t>(hover_)].va;
    }
    bool any_unsettled = false;
    for (auto it = pulses_.begin(); it != pulses_.end();) {
        const bool rising = it->first == hovered_va;
        const qreal target = rising ? 1.0 : 0.0;
        const qreal rate = rising ? 18.0 : 12.0;
        const qreal next =
            it->second + (target - it->second) * (1.0 - std::exp(-rate * dt));
        if (!rising && next < 0.01) {
            it = pulses_.erase(it);
            continue;
        }
        it->second = next;
        if (std::fabs(target - next) >= 0.001) any_unsettled = true;
        ++it;
    }
    if (any_unsettled) {
        update();
    } else {
        pulse_timer_->stop();
        update();
    }
}

void QtFunctionHeatmapWidget::mouseMoveEvent(QMouseEvent* event) {
    const int hit = hitCell(event->position());
    if (hit != hover_) {
        hover_ = hit;
        seedHover(hit);
        update();
    }
    if (hit >= 0)
        showFunctionTooltip(event->globalPosition().toPoint(), hit);
    else
        QToolTip::hideText();
    QWidget::mouseMoveEvent(event);
}

void QtFunctionHeatmapWidget::mousePressEvent(QMouseEvent* event) {
    const int hit = hitCell(event->position());
    if (hit < 0 || !map_) return;
    const auto& fn = map_->functions[static_cast<std::size_t>(hit)];
    if (event->button() == Qt::LeftButton) {
        diag::log_tagged_fmt("binary_map",
            "heatmap_select name='%s' va=0x%llX xrefs=%d callees=%d",
            fn.name.c_str(), static_cast<unsigned long long>(fn.va), fn.xref_count,
            fn.callee_count);
        Q_EMIT functionClicked(fn.va);
    } else if (event->button() == Qt::RightButton) {
        diag::log_tagged_fmt("binary_map",
            "heatmap_right_click name='%s' va=0x%llX", fn.name.c_str(),
            static_cast<unsigned long long>(fn.va));
        Q_EMIT functionClicked(fn.va);
        Q_EMIT functionMenuRequested(fn.va, event->globalPosition().toPoint());
    }
    QWidget::mousePressEvent(event);
}

void QtFunctionHeatmapWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    const int hit = hitCell(event->position());
    if (hit < 0 || !map_) return;
    const auto& fn = map_->functions[static_cast<std::size_t>(hit)];
    diag::log_tagged_fmt("binary_map",
        "heatmap_double_click name='%s' va=0x%llX", fn.name.c_str(),
        static_cast<unsigned long long>(fn.va));
    Q_EMIT functionDoubleClicked(fn.va);
    QWidget::mouseDoubleClickEvent(event);
}

void QtFunctionHeatmapWidget::keyPressEvent(QKeyEvent* event) {
    if (!map_ || map_->functions.empty()) {
        QWidget::keyPressEvent(event);
        return;
    }
    const int cols = columnCount();
    const int count = static_cast<int>(map_->functions.size());
    int target = -1;
    switch (event->key()) {
    case Qt::Key_Left:  target = hover_ < 0 ? 0 : hover_ - 1; break;
    case Qt::Key_Right: target = hover_ < 0 ? 0 : hover_ + 1; break;
    case Qt::Key_Up:    target = hover_ < 0 ? 0 : hover_ - cols; break;
    case Qt::Key_Down:  target = hover_ < 0 ? 0 : hover_ + cols; break;
    case Qt::Key_Home:  target = 0; break;
    case Qt::Key_End:   target = count - 1; break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (hover_ >= 0 && hover_ < count) {
            Q_EMIT functionClicked(
                map_->functions[static_cast<std::size_t>(hover_)].va);
            event->accept();
            return;
        }
        break;
    default: break;
    }
    if (target >= 0) {
        target = (std::max)(0, (std::min)(target, count - 1));
        hover_ = target;
        seedHover(target);
        showFunctionTooltip(mapToGlobal(cellRect(target).center().toPoint()), target);
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void QtFunctionHeatmapWidget::leaveEvent(QEvent* event) {
    hover_ = -1;
    QToolTip::hideText();
    if (theme::AidaMotion::reducedMotion()) {
        pulses_.clear();
        pulse_timer_->stop();
    } else if (!pulses_.empty() && !pulse_timer_->isActive()) {
        pulse_timer_->start();
    }
    update();
    QWidget::leaveEvent(event);
}

}
