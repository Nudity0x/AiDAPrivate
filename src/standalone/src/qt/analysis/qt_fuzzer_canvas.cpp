#include "qt/analysis/qt_fuzzer_canvas.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>

#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::analysis {

using widgets::mix_colors;
using widgets::paint_focus_ring;
using widgets::with_alpha;

namespace {

int crashes_strategy_index(fuzzer_engine::mutation_strategy_t strategy) {
    switch (strategy) {
    case fuzzer_engine::mutation_strategy_t::bit_flip:           return 0;
    case fuzzer_engine::mutation_strategy_t::byte_flip:          return 1;
    case fuzzer_engine::mutation_strategy_t::arithmetic:         return 2;
    case fuzzer_engine::mutation_strategy_t::interesting_values: return 3;
    case fuzzer_engine::mutation_strategy_t::havoc:              return 4;
    case fuzzer_engine::mutation_strategy_t::splice:             return 5;
    default: return -1;
    }
}

constexpr const char* k_strategy_names[6] = {
    "BitFlip", "ByteFlip", "Arith", "Interest", "Havoc", "Splice"
};

}

QtFuzzerCanvas::QtFuzzerCanvas(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.fuzzer.canvas"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(3 * theme::tokens().control.height_lg);
}

void QtFuzzerCanvas::setSnapshot(
    std::shared_ptr<const fuzzer_engine::render_snapshot_t> snapshot, bool running) {
    snapshot_ = std::move(snapshot);
    running_ = running;
    if (snapshot_) {
        int unique[6] = {};
        for (const auto& crash : snapshot_->unique_crashes) {
            const int index = crashes_strategy_index(crash.mutation.strategy);
            if (index >= 0 && index < 6) ++unique[index];
        }
        int maximum = 1;
        for (int i = 0; i < 6; ++i)
            maximum = (std::max)(maximum, unique[i]);
        for (int i = 0; i < 6; ++i) {
            strategy_unique_[i] = unique[i];
            strategy_efficacy_[i] = static_cast<qreal>(unique[i]) /
                static_cast<qreal>(maximum);
        }
    }
    update();
}

void QtFuzzerCanvas::setScanPhase(qreal phase) {
    scan_phase_ = phase;
    if (running_) update();
}

QtFuzzerCanvas::geometry_t QtFuzzerCanvas::layoutZones(const QRectF& bounds) const {
    geometry_t zones;
    const qreal w = bounds.width();
    const qreal h = bounds.height();
    zones.graph = QRectF(bounds.left(), bounds.top(), w * 0.55, h * 0.62);
    zones.heatmap = QRectF(bounds.left() + w * 0.55, bounds.top(), w * 0.45, h * 0.62);
    zones.bars = QRectF(bounds.left(), bounds.top() + h * 0.62, w, h * 0.38);
    return zones;
}

void QtFuzzerCanvas::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    const auto& tokens = theme::tokens();
    painter.setClipRegion(event->region());
    painter.fillRect(rect(), tokens.bg_base);
    if (!snapshot_) {
        painter.setPen(with_alpha(tokens.text_dim, 0.8));
        painter.drawText(rect(), Qt::AlignCenter,
            QStringLiteral("No fuzzing data yet - start a run to populate the canvas"));
        if (hasFocus()) {
            paint_focus_ring(painter, QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0),
                static_cast<qreal>(tokens.radius.sm), 1.0);
        }
        return;
    }
    const auto zones = layoutZones(rect());
    paintGraph(painter, zones.graph);
    paintHeatmap(painter, zones.heatmap);
    paintStrategyBars(painter, zones.bars);
    if (hasFocus()) {
        paint_focus_ring(painter, QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0),
            static_cast<qreal>(tokens.radius.sm), 1.0);
    }
}

void QtFuzzerCanvas::paintGraph(QPainter& painter, const QRectF& zone) {
    const auto& tokens = theme::tokens();
    painter.save();
    painter.setClipRect(zone, Qt::IntersectClip);
    painter.setPen(Qt::NoPen);
    painter.setBrush(with_alpha(tokens.bg_base, 0.6));
    painter.drawRect(zone);
    QPen border(with_alpha(tokens.border_subtle, 1.0), 1.0);
    painter.setPen(border);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(zone);
    for (int grid = 1; grid < 4; ++grid) {
        const qreal y = zone.top() + zone.height() * static_cast<qreal>(grid) / 4.0;
        painter.setPen(QPen(with_alpha(tokens.border_subtle, 0.6), 1.0));
        painter.drawLine(QPointF(zone.left(), y), QPointF(zone.right(), y));
    }
    painter.setPen(with_alpha(tokens.text_dim, 1.0));
    painter.drawText(zone.adjusted(tokens.spacing.xs + tokens.spacing.xxs,
            tokens.spacing.xs, 0, 0), Qt::AlignLeft | Qt::AlignTop,
        QStringLiteral("exec/s"));
    if (snapshot_ && snapshot_->stats.exec_rate_history.size() >= 2) {
        const auto& history = snapshot_->stats.exec_rate_history;
        std::uint64_t max_rate = *std::max_element(history.begin(), history.end());
        if (max_rate == 0) max_rate = 1;
        const qreal step = zone.width() / static_cast<qreal>(history.size() - 1);
        const qreal label_reserve = static_cast<qreal>(tokens.control.icon_glyph);
        QPolygonF line;
        line.reserve(static_cast<int>(history.size()));
        for (std::size_t i = 0; i < history.size(); ++i) {
            const qreal x = zone.left() + static_cast<qreal>(i) * step;
            const qreal y = zone.bottom() -
                (static_cast<qreal>(history[i]) / static_cast<qreal>(max_rate)) *
                    (zone.height() - label_reserve);
            line << QPointF(x, y);
        }
        QPolygonF area = line;
        area << QPointF(zone.right(), zone.bottom())
             << QPointF(zone.left(), zone.bottom());
        painter.setPen(Qt::NoPen);
        painter.setBrush(with_alpha(tokens.accent_grad_top, 0.30));
        painter.drawPolygon(area);
        painter.setPen(QPen(with_alpha(tokens.accent, 0.85), 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(line);
        if (running_ && !theme::AidaMotion::reducedMotion()) {
            const qreal sweep_w = 2.0 * static_cast<qreal>(tokens.spacing.xxl);
            const qreal scan_x = zone.left() + zone.width() * scan_phase_;
            QLinearGradient sweep(QPointF(scan_x - sweep_w, zone.top()),
                QPointF(scan_x, zone.top()));
            sweep.setColorAt(0.0, with_alpha(tokens.accent_hover, 0.0));
            sweep.setColorAt(1.0, with_alpha(tokens.accent_hover, 0.65));
            painter.setPen(Qt::NoPen);
            painter.setBrush(sweep);
            painter.drawRect(QRectF(scan_x - sweep_w, zone.top(), sweep_w, zone.height()));
            painter.setPen(QPen(with_alpha(tokens.accent_hover, 0.95), 1.5));
            painter.drawLine(QPointF(scan_x, zone.top()),
                QPointF(scan_x, zone.bottom()));
        }
        if (hover_graph_index_ >= 0 &&
            hover_graph_index_ < static_cast<int>(history.size())) {
            const qreal x = zone.left() + static_cast<qreal>(hover_graph_index_) * step;
            const qreal y = zone.bottom() -
                (static_cast<qreal>(history[hover_graph_index_]) /
                    static_cast<qreal>(max_rate)) * (zone.height() - label_reserve);
            painter.setPen(QPen(with_alpha(tokens.text_dim, 0.5), 1.0));
            painter.drawLine(QPointF(x, zone.top()), QPointF(x, zone.bottom()));
            painter.setPen(Qt::NoPen);
            painter.setBrush(with_alpha(tokens.accent, 1.0));
            painter.drawEllipse(QPointF(x, y), tokens.spacing.xs,
                tokens.spacing.xs);
            painter.setPen(QPen(with_alpha(tokens.text_primary, 0.45), 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QPointF(x, y), tokens.radius.md,
                tokens.radius.md);
        }
    } else {
        painter.setPen(with_alpha(tokens.text_dim, 0.8));
        painter.drawText(zone, Qt::AlignCenter,
            QStringLiteral("No execution samples yet - start a fuzzing run"));
    }
    painter.restore();
}

void QtFuzzerCanvas::paintHeatmap(QPainter& painter, const QRectF& zone) {
    const auto& tokens = theme::tokens();
    painter.save();
    const std::uint32_t edges = snapshot_ ? snapshot_->stats.edge_coverage : 0;
    constexpr int cols = 16;
    constexpr int rows = 4;
    const qreal cell_w = zone.width() / cols;
    const qreal cell_h = zone.height() / rows;
    const qreal pad = static_cast<qreal>(tokens.spacing.xxs) * 0.75;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < cols; ++column) {
            const int index = row * cols + column;
            const qreal frac = static_cast<qreal>(index) / static_cast<qreal>(cols * rows);
            qreal fill = 0.0;
            if (edges > 0) {
                const qreal threshold = frac * static_cast<qreal>(edges) / 64.0;
                fill = (std::min)(1.0, threshold);
            }
            const QColor low = with_alpha(tokens.panel_header, 0.6);
            const QColor high = with_alpha(tokens.success, 0.85);
            painter.setPen(Qt::NoPen);
            painter.setBrush(mix_colors(low, high, fill));
            painter.drawRect(QRectF(zone.left() + column * cell_w + pad,
                zone.top() + row * cell_h + pad,
                cell_w - pad * 2.0, cell_h - pad * 2.0));
        }
    }
    painter.setPen(with_alpha(tokens.text_dim, 1.0));
    painter.drawText(zone.adjusted(tokens.spacing.xs, tokens.spacing.xxs,
            -tokens.spacing.xs, -tokens.spacing.xxs),
        Qt::AlignRight | Qt::AlignTop, QStringLiteral("%1 edges").arg(edges));
    painter.restore();
}

void QtFuzzerCanvas::paintStrategyBars(QPainter& painter, const QRectF& zone) {
    const auto& tokens = theme::tokens();
    painter.save();
    painter.setPen(with_alpha(tokens.text_secondary, 1.0));
    painter.drawText(zone.adjusted(tokens.spacing.xs, tokens.spacing.xxs, 0, 0),
        Qt::AlignLeft | Qt::AlignTop, QStringLiteral("Mutation Strategy Efficacy"));
    const QRectF plot = zone.adjusted(tokens.spacing.xs + tokens.spacing.xxs,
        tokens.spacing.xl, -tokens.spacing.xs - tokens.spacing.xxs,
        -tokens.spacing.lg - tokens.spacing.xxs);
    const qreal bar_gap = static_cast<qreal>(tokens.spacing.xs + tokens.spacing.xxs);
    const qreal bar_radius = static_cast<qreal>(tokens.radius.sm);
    const qreal bar_total_w = plot.width();
    const qreal bar_w = bar_total_w / 6.0 - bar_gap;
    if (bar_w <= 1.0 || plot.height() <= 1.0) {
        painter.restore();
        return;
    }
    for (int i = 0; i < 6; ++i) {
        const qreal x = plot.left() + static_cast<qreal>(i) * (bar_w + bar_gap);
        const QColor color = (i % 2 == 0) ? tokens.accent_grad_top : tokens.accent_grad_bot;
        painter.setPen(Qt::NoPen);
        painter.setBrush(with_alpha(tokens.panel_header, 0.5));
        painter.drawRoundedRect(QRectF(x, plot.top(), bar_w, plot.height()),
            bar_radius, bar_radius);
        const qreal fill_h = plot.height() * strategy_efficacy_[i];
        painter.setBrush(with_alpha(color, 0.85));
        painter.drawRoundedRect(QRectF(x, plot.bottom() - fill_h, bar_w, fill_h),
            bar_radius, bar_radius);
        painter.setPen(with_alpha(tokens.text_dim, 1.0));
        painter.drawText(QRectF(x, plot.bottom() + tokens.spacing.xxs, bar_w,
                tokens.control.icon_glyph),
            Qt::AlignHCenter | Qt::AlignTop, QString::fromLatin1(k_strategy_names[i]));
        painter.setPen(with_alpha(tokens.text_primary, 1.0));
        painter.drawText(QRectF(x, plot.bottom() - fill_h - tokens.control.icon_glyph,
                bar_w, tokens.spacing.md),
            Qt::AlignHCenter | Qt::AlignBottom, QString::number(strategy_unique_[i]));
    }
    painter.restore();
}

void QtFuzzerCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (snapshot_ && snapshot_->stats.exec_rate_history.size() >= 2) {
        const auto zones = layoutZones(rect());
        if (zones.graph.contains(event->position())) {
            const auto& history = snapshot_->stats.exec_rate_history;
            const qreal rel = (event->position().x() - zones.graph.left()) /
                zones.graph.width();
            const int index = static_cast<int>(
                rel * static_cast<qreal>(history.size() - 1));
            if (index >= 0 && index < static_cast<int>(history.size())) {
                hover_graph_index_ = index;
                QToolTip::showText(event->globalPosition().toPoint(),
                    QStringLiteral("%1/s").arg(history[index]), this);
                update();
                return;
            }
        }
    }
    if (hover_graph_index_ != -1) {
        hover_graph_index_ = -1;
        QToolTip::hideText();
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void QtFuzzerCanvas::keyPressEvent(QKeyEvent* event) {
    const auto move_cursor = [&](int target) {
        if (!snapshot_ || snapshot_->stats.exec_rate_history.size() < 2)
            return false;
        const int last = static_cast<int>(snapshot_->stats.exec_rate_history.size()) - 1;
        hover_graph_index_ = (std::max)(0, (std::min)(target, last));
        const auto zones = layoutZones(rect());
        const auto& history = snapshot_->stats.exec_rate_history;
        std::uint64_t max_rate = *std::max_element(history.begin(), history.end());
        if (max_rate == 0) max_rate = 1;
        const qreal step = zones.graph.width() /
            static_cast<qreal>(history.size() - 1);
        const qreal label_reserve = static_cast<qreal>(theme::tokens().control.icon_glyph);
        const qreal x = zones.graph.left() +
            static_cast<qreal>(hover_graph_index_) * step;
        const qreal y = zones.graph.bottom() -
            (static_cast<qreal>(history[static_cast<std::size_t>(hover_graph_index_)]) /
                static_cast<qreal>(max_rate)) * (zones.graph.height() - label_reserve);
        QToolTip::showText(mapToGlobal(QPoint(static_cast<int>(x),
                static_cast<int>(y))),
            QStringLiteral("%1/s").arg(
                history[static_cast<std::size_t>(hover_graph_index_)]), this);
        update();
        return true;
    };
    switch (event->key()) {
    case Qt::Key_Left:
        if (move_cursor(hover_graph_index_ < 0 ? 0 : hover_graph_index_ - 1)) {
            event->accept();
            return;
        }
        break;
    case Qt::Key_Right:
        if (move_cursor(hover_graph_index_ < 0 ? 0 : hover_graph_index_ + 1)) {
            event->accept();
            return;
        }
        break;
    case Qt::Key_Home:
        if (move_cursor(0)) {
            event->accept();
            return;
        }
        break;
    case Qt::Key_End:
        if (move_cursor(snapshot_
                ? static_cast<int>(snapshot_->stats.exec_rate_history.size()) - 1
                : 0)) {
            event->accept();
            return;
        }
        break;
    default: break;
    }
    QWidget::keyPressEvent(event);
}

void QtFuzzerCanvas::leaveEvent(QEvent* event) {
    hover_graph_index_ = -1;
    QToolTip::hideText();
    update();
    QWidget::leaveEvent(event);
}

}
