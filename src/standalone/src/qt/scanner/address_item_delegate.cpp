#include "qt/scanner/address_item_delegate.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QStyle>

#include "qt/scanner/address_list_model.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::scanner {

AddressItemDelegate::AddressItemDelegate(QObject* parent)
	: QStyledItemDelegate(parent), palette_(make_scanner_palette()) {}

void AddressItemDelegate::setPalette(const scanner_palette_t& palette)
{
	palette_ = palette;
	elide_cache_.clear();
}

void AddressItemDelegate::set_freeze_toggle_handler(
	std::function<void(int row)> handler)
{
	freeze_toggle_handler_ = std::move(handler);
}

QSize AddressItemDelegate::sizeHint(const QStyleOptionViewItem& option,
	const QModelIndex& index) const
{
	QSize size = QStyledItemDelegate::sizeHint(option, index);
	size.setHeight(theme::tokens().table.row_h);
	return size;
}

QRect AddressItemDelegate::switch_rect(const QRect& cell)
{
	const auto& tokens = theme::tokens();
	const int box = tokens.control.checkbox;
	return QRect(cell.left() + tokens.table.cell_pad_x,
		cell.top() + (cell.height() - box) / 2, box, box);
}

QString AddressItemDelegate::elided(const QFontMetricsF& metrics,
	const QFont& font, const QString& text, int width) const
{
	if (width <= 0)
		return {};
	if (metrics.horizontalAdvance(text) <= width)
		return text;
	const QString key = font.family() + u'|' + QString::number(font.pointSizeF()) +
		u'|' + QString::number(width) + u'|' + text;
	const auto found = elide_cache_.constFind(key);
	if (found != elide_cache_.constEnd())
		return found.value();
	QString result = metrics.elidedText(text, Qt::ElideRight, width);
	if (elide_cache_.size() >= 4096)
		elide_cache_.clear();
	elide_cache_.insert(key, result);
	return result;
}

void AddressItemDelegate::paint(QPainter* painter,
	const QStyleOptionViewItem& option, const QModelIndex& index) const
{
	if (!index.isValid())
		return;
	const QRect rect = option.rect;
	const bool selected = (option.state & QStyle::State_Selected) != 0;
	const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
	const bool alternate = (option.features & QStyleOptionViewItem::Alternate) != 0;
	const bool frozen = index.data(AddressListModel::role_frozen).toBool();

	const auto& tokens = theme::tokens();
	const int pad_x = tokens.table.cell_pad_x;
	painter->save();
	painter->setPen(Qt::NoPen);
	if (selected) {
		painter->fillRect(rect, palette_.selection);
		painter->fillRect(QRect(rect.left(), rect.top(),
			tokens.tab.underline, rect.height()), palette_.accent);
	} else if (hovered) {
		painter->fillRect(rect, widgets::with_alpha(palette_.hover_wash, 0.5));
	} else if (alternate) {
		painter->fillRect(rect, widgets::with_alpha(palette_.alt_row, 0.5));
	}

	if (index.column() == AddressListModel::column_active) {
		const QRect box = switch_rect(rect);
		const qreal radius = tokens.radius.sm;
		painter->setBrush(frozen ? widgets::with_alpha(palette_.accent_dim, 0.92)
			: palette_.panel_bg);
		painter->setPen(QPen(frozen ? palette_.accent : palette_.border_strong,
			qreal(tokens.panel.border)));
		painter->drawRoundedRect(box, radius, radius);
		if (frozen) {
			painter->setPen(QPen(palette_.text_primary,
				qreal(tokens.control.focus_ring)));
			const qreal bw = box.width();
			const qreal bh = box.height();
			painter->drawLine(
				QPointF(box.left() + bw * 0.25, box.top() + bh * 0.5),
				QPointF(box.left() + bw * 0.4375, box.top() + bh * 0.75));
			painter->drawLine(
				QPointF(box.left() + bw * 0.4375, box.top() + bh * 0.75),
				QPointF(box.left() + bw * 0.75, box.top() + bh * 0.3125));
		}
		const QVariant font = index.data(Qt::FontRole);
		if (font.isValid())
			painter->setFont(font.value<QFont>());
		const QVariant foreground = index.data(Qt::ForegroundRole);
		painter->setPen(foreground.isValid() ? foreground.value<QColor>()
			: palette_.text_primary);
		const QRect label_rect(box.right() + pad_x, rect.top(),
			rect.right() - box.right() - 2 * pad_x, rect.height());
		const QString label = elided(QFontMetricsF(painter->font()), painter->font(),
			index.data(Qt::DisplayRole).toString(), label_rect.width());
		if (!label.isEmpty()) {
			const QFontMetricsF metrics(painter->font());
			const qreal baseline = rect.top() +
				(rect.height() - metrics.height()) / 2.0 + metrics.ascent();
			painter->drawText(QPointF(label_rect.left(), baseline), label);
		}
		painter->restore();
		return;
	}

	const QVariant font = index.data(Qt::FontRole);
	if (font.isValid())
		painter->setFont(font.value<QFont>());
	const QString text = elided(QFontMetricsF(painter->font()), painter->font(),
		index.data(Qt::DisplayRole).toString(), rect.width() - 2 * pad_x);
	if (!text.isEmpty()) {
		const QVariant foreground = index.data(Qt::ForegroundRole);
		painter->setPen(foreground.isValid() ? foreground.value<QColor>()
			: palette_.text_primary);
		const QFontMetricsF metrics(painter->font());
		const qreal baseline = rect.top() +
			(rect.height() - metrics.height()) / 2.0 + metrics.ascent();
		painter->drawText(QPointF(rect.left() + pad_x, baseline), text);
	}
	painter->restore();
}

bool AddressItemDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
	const QStyleOptionViewItem& option, const QModelIndex& index)
{
	if (!index.isValid() || index.column() != AddressListModel::column_active)
		return QStyledItemDelegate::editorEvent(event, model, option, index);
	if (event->type() != QEvent::MouseButtonRelease)
		return QStyledItemDelegate::editorEvent(event, model, option, index);
	auto* mouse = static_cast<QMouseEvent*>(event);
	if (mouse->button() != Qt::LeftButton)
		return QStyledItemDelegate::editorEvent(event, model, option, index);
	if (!switch_rect(option.rect).contains(mouse->pos()))
		return QStyledItemDelegate::editorEvent(event, model, option, index);
	if (freeze_toggle_handler_)
		freeze_toggle_handler_(index.row());
	return true;
}

}
