#include "qt/scanner/aob_byte_grid_widget.hpp"

#include <QPaintEvent>
#include <QPainter>

#include <algorithm>
#include <cstdio>

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::scanner {

AobByteGridWidget::AobByteGridWidget(QWidget* parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("aida.memory.aob.byte_grid"));
	setAttribute(Qt::WA_OpaquePaintEvent);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
	const auto& tokens = theme::tokens();
	const QFontMetricsF metrics(theme::fonts::codeRegular());
	cell_w_ = static_cast<int>(metrics.horizontalAdvance(
		QStringLiteral("00")) + 0.5) + tokens.spacing.sm;
	cell_h_ = static_cast<int>(metrics.height() + 0.5) + 2 * tokens.spacing.xxs;
	row_stride_ = cell_h_ + tokens.spacing.xxs;
}

void AobByteGridWidget::set_bytes(const std::vector<aob_generator::aob_byte_t>& bytes)
{
	const bool same = bytes_.size() == bytes.size() &&
		std::equal(bytes_.begin(), bytes_.end(), bytes.begin(),
			[](const auto& left, const auto& right) {
				return left.value == right.value && left.wildcard == right.wildcard;
			});
	if (same)
		return;
	bytes_ = bytes;
	updateGeometry();
	update();
}

void AobByteGridWidget::clear()
{
	if (bytes_.empty())
		return;
	bytes_.clear();
	updateGeometry();
	update();
}

int AobByteGridWidget::rows_for_width(int width) const
{
	if (bytes_.empty())
		return 0;
	const int per_row = (std::max)(1, width / cell_w_);
	return static_cast<int>((bytes_.size() + static_cast<std::size_t>(per_row) - 1) /
		static_cast<std::size_t>(per_row));
}

QSize AobByteGridWidget::sizeHint() const
{
	const int w = cell_w_ * 10;
	return QSize(w, rows_for_width(w) * row_stride_);
}

QSize AobByteGridWidget::minimumSizeHint() const
{
	return QSize(cell_w_ * 4, rows_for_width(cell_w_ * 4) * row_stride_);
}

int AobByteGridWidget::heightForWidth(int width) const
{
	return rows_for_width(width) * row_stride_;
}

void AobByteGridWidget::paintEvent(QPaintEvent* event)
{
	QPainter painter(this);
	const auto& tokens = theme::tokens();
	painter.fillRect(rect(), tokens.bg_base);
	if (bytes_.empty()) {
		painter.setFont(theme::fonts::body());
		painter.setPen(tokens.text_dim);
		painter.drawText(rect(), Qt::AlignCenter,
			QStringLiteral("Generate a signature to inspect its bytes"));
		return;
	}
	painter.setFont(theme::fonts::codeRegular());
	const QFontMetricsF metrics(painter.font());
	const int width = rect().width();
	const int per_row = (std::max)(1, width / cell_w_);
	const int row_stride = row_stride_;
	const int first_row = (std::max)(0, event->rect().top() / row_stride);
	const int last_row = (std::min)(rows_for_width(width) - 1,
		event->rect().bottom() / row_stride);
	const std::size_t first_index = static_cast<std::size_t>(first_row) *
		static_cast<std::size_t>(per_row);
	const std::size_t last_index = (std::min)(bytes_.size(),
		static_cast<std::size_t>(last_row + 1) * static_cast<std::size_t>(per_row));
	for (std::size_t index = first_index; index < last_index; ++index) {
		const int row = static_cast<int>(index / static_cast<std::size_t>(per_row));
		const int column = static_cast<int>(index % static_cast<std::size_t>(per_row));
		const int x = column * cell_w_;
		const int y = row * row_stride;
		char hex[4]{};
		if (bytes_[index].wildcard) {
			hex[0] = '?';
			hex[1] = '?';
		} else {
			std::snprintf(hex, sizeof(hex), "%02X", bytes_[index].value);
		}
		painter.setPen(bytes_[index].wildcard ? tokens.warning : tokens.info);
		const QString text = QString::fromLatin1(hex);
		const qreal text_x = x +
			(cell_w_ - metrics.horizontalAdvance(text)) / 2.0;
		const qreal baseline = y + (cell_h_ - metrics.height()) / 2.0 + metrics.ascent();
		painter.drawText(QPointF(text_x, baseline), text);
	}
}

}
