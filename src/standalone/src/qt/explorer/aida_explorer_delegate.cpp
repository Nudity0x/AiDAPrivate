#include "qt/explorer/aida_explorer_delegate.hpp"

#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>

#include "qt/explorer/aida_explorer_model.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::explorer {

namespace {

qreal glyph_scale()
{
    return theme::tokens().control.icon_glyph / 14.0;
}

}

AidaExplorerDelegate::AidaExplorerDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

bool AidaExplorerDelegate::setContentWidth(int width)
{
    const int clamped = std::max(0, width);
    if (content_width_ == clamped)
        return false;
    content_width_ = clamped;
    return true;
}

int AidaExplorerDelegate::rowBaseWidth()
{
    const auto& t = theme::tokens();
    return t.spacing.sm * 2 + t.spacing.lg * 3 + t.spacing.md;
}

int AidaExplorerDelegate::estimatedRowWidth(int max_depth, int max_name_units,
    const QFontMetricsF& fm)
{
    const auto& t = theme::tokens();
    const qreal indent = std::max(0, max_depth) * static_cast<qreal>(t.spacing.lg);
    const qreal text = fm.averageCharWidth() * std::max(0, max_name_units);
    return qRound(static_cast<qreal>(rowBaseWidth()) + indent + text);
}

void AidaExplorerDelegate::paintFolderIcon(QPainter& painter, const QPointF& center,
    qreal scale, const QColor& color, bool expanded) const
{
    const qreal stroke = std::max(1.0, 1.25 * scale);
    const QPointF minimum(center.x() - 6.5 * scale, center.y() - 4.0 * scale);
    const QPointF maximum(center.x() + 6.5 * scale, center.y() + 5.0 * scale);
    painter.setPen(QPen(color, stroke));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(minimum, maximum));
    painter.drawLine(QPointF(minimum.x() + 0.5 * scale, minimum.y()),
        QPointF(minimum.x() + 3.0 * scale, minimum.y() - 3.0 * scale));
    painter.drawLine(QPointF(minimum.x() + 3.0 * scale, minimum.y() - 3.0 * scale),
        QPointF(center.x() + 1.0 * scale, minimum.y() - 3.0 * scale));
    painter.drawLine(QPointF(center.x() + 1.0 * scale, minimum.y() - 3.0 * scale),
        QPointF(center.x() + 1.0 * scale, minimum.y()));
    if (expanded)
        painter.drawLine(QPointF(minimum.x() + 2.0 * scale, center.y() + 1.0 * scale),
            QPointF(maximum.x() - 2.0 * scale, center.y() + 1.0 * scale));
}

void AidaExplorerDelegate::paintFileIcon(QPainter& painter, const QPointF& center,
    qreal scale, const QColor& color) const
{
    const qreal stroke = std::max(1.0, 1.25 * scale);
    const QPointF minimum(center.x() - 5.0 * scale, center.y() - 7.0 * scale);
    const QPointF maximum(center.x() + 5.0 * scale, center.y() + 7.0 * scale);
    painter.setPen(QPen(color, stroke));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(minimum, maximum));
    painter.drawLine(QPointF(center.x() + 1.0 * scale, minimum.y()),
        QPointF(maximum.x(), minimum.y() + 4.0 * scale));
    painter.drawLine(QPointF(center.x() + 1.0 * scale, minimum.y()),
        QPointF(center.x() + 1.0 * scale, minimum.y() + 4.0 * scale));
    painter.drawLine(QPointF(center.x() + 1.0 * scale, minimum.y() + 4.0 * scale),
        QPointF(maximum.x(), minimum.y() + 4.0 * scale));
}

void AidaExplorerDelegate::paintDisclosure(QPainter& painter, const QPointF& center,
    qreal scale, const QColor& color, bool expanded) const
{
    const qreal half = 3.0 * scale;
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    if (expanded) {
        const QPointF points[3] = {QPointF(center.x() - half, center.y() - half * 0.6),
            QPointF(center.x() + half, center.y() - half * 0.6),
            QPointF(center.x(), center.y() + half)};
        painter.drawPolygon(points, 3);
    } else {
        const QPointF points[3] = {QPointF(center.x() - half * 0.6, center.y() - half),
            QPointF(center.x() - half * 0.6, center.y() + half),
            QPointF(center.x() + half, center.y())};
        painter.drawPolygon(points, 3);
    }
}

QString AidaExplorerDelegate::elidedName(const QString& name, qreal width,
    const QFont& font) const
{
    const QString key = font.key() + QStringLiteral("|") + name + QStringLiteral("|") + QString::number(qRound(width));
    const auto found = elide_cache_.find(key);
    if (found != elide_cache_.end())
        return found.value();
    const QFontMetricsF fm(font);
    const QString elided = fm.elidedText(name, Qt::ElideMiddle, qRound(width));
    if (elide_cache_.size() > 8192)
        elide_cache_.clear();
    elide_cache_.insert(key, elided);
    return elided;
}

void AidaExplorerDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
    const QModelIndex& index) const
{
    const auto& t = theme::tokens();
    const bool is_dir = index.data(AidaExplorerModel::IsDirRole).toBool();
    const bool expanded = index.data(AidaExplorerModel::ExpandedRole).toBool();
    const int depth = index.data(AidaExplorerModel::DepthRole).toInt();
    const bool selected = index.data(AidaExplorerModel::SelectedRole).toBool();
    const bool dirty = index.data(AidaExplorerModel::DocumentDirtyRole).toBool();
    const bool conflict = index.data(AidaExplorerModel::DocumentConflictRole).toBool();
    const QString name = index.data(AidaExplorerModel::NameRole).toString();

    painter->save();
    painter->setClipRect(option.rect);

    if (selected)
        painter->fillRect(option.rect, t.selection);
    else if (option.state & QStyle::State_MouseOver)
        painter->fillRect(option.rect, widgets::with_alpha(t.hover_wash, 0.4));

    const qreal scale = glyph_scale();
    const qreal indent = std::max(0, depth) * static_cast<qreal>(t.spacing.lg);
    const qreal base_left = option.rect.left() + t.spacing.sm;
    const qreal disclosure_x = base_left + indent + t.spacing.lg * 0.5;
    const qreal icon_x = disclosure_x + t.spacing.lg;
    const qreal text_left = icon_x + t.spacing.lg * 0.5 + t.spacing.md;
    const qreal indicator = (dirty || conflict) ? static_cast<qreal>(t.spacing.lg) : 0.0;
    const qreal text_right = option.rect.right() - t.spacing.sm - indicator;
    const qreal center_y = option.rect.top() + option.rect.height() * 0.5;

    if (is_dir)
        paintDisclosure(*painter, QPointF(disclosure_x, center_y), scale,
            option.state & QStyle::State_MouseOver ? t.text_primary : t.text_dim, expanded);
    const QColor icon_color = selected ? t.accent
        : (is_dir ? t.text_secondary : t.text_dim);
    if (is_dir)
        paintFolderIcon(*painter, QPointF(icon_x, center_y), scale, icon_color, expanded);
    else
        paintFileIcon(*painter, QPointF(icon_x, center_y), scale, icon_color);

    painter->setPen(selected ? t.text_primary : t.text_secondary);
    painter->setFont(option.font);
    const QFontMetricsF fm(option.font);
    const QString elided = elidedName(name, text_right - text_left, option.font);
    painter->drawText(QPointF(text_left, widgets::text_baseline_centered(option.rect, fm)),
        elided);

    if (dirty || conflict) {
        const qreal radius = t.status_bar.dot * 0.5 * scale;
        const qreal dot_x = (std::min)(text_left + fm.horizontalAdvance(elided) +
            static_cast<qreal>(t.spacing.xs) + radius,
            option.rect.right() - t.spacing.sm - radius);
        painter->setPen(Qt::NoPen);
        painter->setBrush(conflict ? t.error : t.changed);
        painter->drawEllipse(QPointF(dot_x, center_y), radius, radius);
    }

    if (option.state & QStyle::State_HasFocus) {
        const qreal ring = t.control.focus_ring;
        const qreal inset = ring * 0.5;
        painter->setPen(QPen(t.border_focus, ring));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(QRectF(option.rect).adjusted(inset, inset, -inset, -inset));
    }
    painter->restore();
}

QSize AidaExplorerDelegate::sizeHint(const QStyleOptionViewItem& option,
    const QModelIndex& index) const
{
    Q_UNUSED(index);
    const int text_floor = qRound(option.fontMetrics.averageCharWidth() * 8.0);
    return QSize((std::max)(rowBaseWidth() + text_floor, content_width_),
        theme::tokens().row.compact);
}

}
