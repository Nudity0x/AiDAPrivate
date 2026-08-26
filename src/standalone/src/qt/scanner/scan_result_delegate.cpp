#include "qt/scanner/scan_result_delegate.hpp"

#include <QPainter>
#include <QStyle>

#include "qt/scanner/scan_results_model.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::scanner {

ScanResultDelegate::ScanResultDelegate(QObject* parent)
	: QStyledItemDelegate(parent), palette_(make_scanner_palette()) {}

void ScanResultDelegate::setPalette(const scanner_palette_t& palette)
{
	palette_ = palette;
	elide_cache_.clear();
}

QSize ScanResultDelegate::sizeHint(const QStyleOptionViewItem& option,
	const QModelIndex& index) const
{
	QSize size = QStyledItemDelegate::sizeHint(option, index);
	size.setHeight(theme::tokens().table.row_h);
	return size;
}

QString ScanResultDelegate::elided(const QFontMetricsF& metrics,
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

void ScanResultDelegate::paint(QPainter* painter,
	const QStyleOptionViewItem& option, const QModelIndex& index) const
{
	if (!index.isValid())
		return;
	const QRect rect = option.rect;
	const bool selected = (option.state & QStyle::State_Selected) != 0;
	const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
	const bool alternate = (option.features & QStyleOptionViewItem::Alternate) != 0;

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
	const qreal flash = index.data(ScanResultsModel::role_flash).toReal();
	if (flash > 0.0) {
		painter->fillRect(rect,
			widgets::with_alpha(palette_.accent_glow, 0.85 * flash));
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

}
