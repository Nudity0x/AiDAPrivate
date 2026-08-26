#include "qt/scanner/diff_detail_widget.hpp"

#include <QPainter>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "qt/scanner/scanner_palette.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::scanner {

DiffDetailWidget::DiffDetailWidget(QWidget* parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("aida.memory.snapshot.diff_detail"));
	setAttribute(Qt::WA_OpaquePaintEvent);
	const auto& tokens = theme::tokens();
	const QFontMetricsF em_metrics(theme::fonts::bodyEm());
	const QFontMetricsF code_metrics(theme::fonts::codeRegular());
	const int byte_h = static_cast<int>(code_metrics.height() + 0.5) +
		2 * tokens.spacing.xxs;
	content_height_ = tokens.spacing.sm +
		static_cast<int>(em_metrics.height() + 0.5) + tokens.spacing.sm +
		2 * byte_h + 2 * tokens.spacing.xs + tokens.spacing.sm +
		static_cast<int>(code_metrics.height() + 0.5) + tokens.spacing.sm;
	setFixedHeight(content_height_);
}

void DiffDetailWidget::set_change(const snapshot_diff::changed_region_t& change)
{
	change_ = change;
	update();
}

void DiffDetailWidget::clear_change()
{
	change_.reset();
	update();
}

QSize DiffDetailWidget::sizeHint() const
{
	return QSize(mono_cell_width() * 54, content_height_);
}

QSize DiffDetailWidget::minimumSizeHint() const
{
	return QSize(mono_cell_width() * 30, content_height_);
}

void DiffDetailWidget::paintEvent(QPaintEvent* event)
{
	static_cast<void>(event);
	QPainter painter(this);
	const auto& tokens = theme::tokens();
	painter.fillRect(rect(), tokens.panel_bg);
	painter.setPen(QPen(tokens.border_subtle, qreal(tokens.panel.border)));
	painter.drawLine(QPointF(0, 0.5), QPointF(width(), 0.5));
	if (!change_) {
		painter.setFont(theme::fonts::body());
		painter.setPen(tokens.text_dim);
		painter.drawText(rect(), Qt::AlignCenter,
			QStringLiteral("Select a change to inspect its bytes"));
		return;
	}
	const auto& change = *change_;
	const int pad_x = tokens.spacing.lg;
	const int cell_pad = tokens.table.cell_pad_x;

	painter.setFont(theme::fonts::bodyEm());
	painter.setPen(tokens.text_primary);
	const QFontMetricsF em_metrics(painter.font());
	const QString title = QStringLiteral("Detail");
	painter.drawText(QPointF(pad_x, tokens.spacing.sm + em_metrics.ascent()), title);

	painter.setFont(theme::fonts::body());
	painter.setPen(tokens.text_secondary);
	const QFontMetricsF body_metrics(theme::fonts::body());
	painter.drawText(QPointF(pad_x + em_metrics.horizontalAdvance(title) +
			tokens.spacing.lg, tokens.spacing.sm + body_metrics.ascent()),
		QStringLiteral("0x%1  ·  %2 bytes")
			.arg(change.address, 0, 16).toUpper().arg(change.size));

	const QFontMetricsF code_metrics(theme::fonts::codeRegular());
	const float byte_w = static_cast<float>(static_cast<int>(
		code_metrics.horizontalAdvance(QStringLiteral("00")) + 0.5) + cell_pad);
	const float byte_h = static_cast<float>(static_cast<int>(
		code_metrics.height() + 0.5) + 2 * tokens.spacing.xxs);
	const qreal label_w = body_metrics.horizontalAdvance(QStringLiteral("New")) +
		tokens.spacing.sm;
	float hex_y = static_cast<float>(tokens.spacing.sm +
		static_cast<int>(em_metrics.height() + 0.5) + tokens.spacing.sm);

	painter.setFont(theme::fonts::body());
	painter.setPen(tokens.error);
	painter.drawText(QPointF(pad_x, widgets::text_baseline_centered(
		QRectF(pad_x, hex_y, label_w, byte_h), body_metrics)),
		QStringLiteral("Old"));
	float ohx = pad_x + label_w;
	painter.setFont(theme::fonts::codeRegular());
	const std::size_t old_count = (std::min)({static_cast<std::size_t>(change.size),
		change.old_data.size(), std::size_t{32}});
	for (std::size_t j = 0; j < old_count; ++j) {
		char hb[4]{};
		std::snprintf(hb, sizeof(hb), "%02X", static_cast<unsigned int>(change.old_data[j]));
		const bool diff = j < change.new_data.size() &&
			change.old_data[j] != change.new_data[j];
		if (diff) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(tokens.error_soft);
			painter.drawRoundedRect(QRectF(ohx - tokens.spacing.xxs, hex_y,
				byte_w, byte_h), qreal(tokens.radius.sm), qreal(tokens.radius.sm));
		}
		painter.setPen(diff ? tokens.error : tokens.text_dim);
		painter.drawText(QPointF(ohx + (byte_w -
				code_metrics.horizontalAdvance(QString::fromLatin1(hb))) * 0.5,
			widgets::text_baseline_centered(QRectF(ohx, hex_y, byte_w, byte_h),
				code_metrics)),
			QString::fromLatin1(hb));
		ohx += byte_w;
	}
	if (static_cast<std::size_t>(change.size) > old_count) {
		painter.setFont(theme::fonts::body());
		painter.setPen(tokens.text_dim);
		painter.drawText(QPointF(ohx + tokens.spacing.xxs,
				widgets::text_baseline_centered(QRectF(ohx, hex_y, byte_w * 4, byte_h),
					body_metrics)),
			QStringLiteral("+%1").arg(change.size - static_cast<std::uint32_t>(old_count)));
	}

	hex_y += byte_h + tokens.spacing.xs;
	painter.setFont(theme::fonts::body());
	painter.setPen(tokens.success);
	painter.drawText(QPointF(pad_x, widgets::text_baseline_centered(
		QRectF(pad_x, hex_y, label_w, byte_h), body_metrics)),
		QStringLiteral("New"));
	float nhx = pad_x + label_w;
	painter.setFont(theme::fonts::codeRegular());
	const std::size_t new_count = (std::min)({static_cast<std::size_t>(change.size),
		change.new_data.size(), std::size_t{32}});
	for (std::size_t j = 0; j < new_count; ++j) {
		char hb[4]{};
		std::snprintf(hb, sizeof(hb), "%02X", static_cast<unsigned int>(change.new_data[j]));
		const bool diff = j < change.old_data.size() &&
			change.old_data[j] != change.new_data[j];
		if (diff) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(tokens.success_soft);
			painter.drawRoundedRect(QRectF(nhx - tokens.spacing.xxs, hex_y,
				byte_w, byte_h), qreal(tokens.radius.sm), qreal(tokens.radius.sm));
		}
		painter.setPen(diff ? tokens.success : tokens.text_dim);
		painter.drawText(QPointF(nhx + (byte_w -
				code_metrics.horizontalAdvance(QString::fromLatin1(hb))) * 0.5,
			widgets::text_baseline_centered(QRectF(nhx, hex_y, byte_w, byte_h),
				code_metrics)),
			QString::fromLatin1(hb));
		nhx += byte_w;
	}
	if (static_cast<std::size_t>(change.size) > new_count) {
		painter.setFont(theme::fonts::body());
		painter.setPen(tokens.text_dim);
		painter.drawText(QPointF(nhx + tokens.spacing.xxs,
				widgets::text_baseline_centered(QRectF(nhx, hex_y, byte_w * 4, byte_h),
					body_metrics)),
			QStringLiteral("+%1").arg(change.size - static_cast<std::uint32_t>(new_count)));
	}

	hex_y += byte_h + tokens.spacing.sm;
	if (change.size == 4 && change.old_data.size() >= 4 && change.new_data.size() >= 4) {
		float old_f, new_f;
		std::memcpy(&old_f, change.old_data.data(), 4);
		std::memcpy(&new_f, change.new_data.data(), 4);
		std::int32_t old_i, new_i;
		std::memcpy(&old_i, change.old_data.data(), 4);
		std::memcpy(&new_i, change.new_data.data(), 4);
		painter.setPen(tokens.text_secondary);
		painter.drawText(QPointF(pad_x, hex_y + code_metrics.ascent()),
			QStringLiteral("Int32: %1 -> %2   Float: %3 -> %4")
				.arg(old_i).arg(new_i)
				.arg(static_cast<double>(old_f), 0, 'f', 6)
				.arg(static_cast<double>(new_f), 0, 'f', 6));
	} else if (change.size == 8 && change.old_data.size() >= 8 &&
		change.new_data.size() >= 8) {
		std::uint64_t old_v, new_v;
		std::memcpy(&old_v, change.old_data.data(), 8);
		std::memcpy(&new_v, change.new_data.data(), 8);
		painter.setPen(tokens.text_secondary);
		painter.drawText(QPointF(pad_x, hex_y + code_metrics.ascent()),
			QStringLiteral("UInt64: 0x%1 -> 0x%2")
				.arg(old_v, 0, 16).toUpper().arg(new_v, 0, 16).toUpper());
	}
}

}
