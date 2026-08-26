#include "qt/scanner/chain_diagram_widget.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "qt/scanner/pointer_controller.hpp"
#include "qt/scanner/scanner_palette.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::scanner {

namespace {

constexpr float kMinScale = 0.45f;

float diagram_pad()
{
	return static_cast<float>(theme::tokens().spacing.md);
}

float box_height()
{
	return static_cast<float>(theme::tokens().control.height_md);
}

float gap_width()
{
	return static_cast<float>(theme::tokens().spacing.xxl * 2);
}

QPointF bezier_point(const QPointF& from, const QPointF& to, float t)
{
	const float lift = static_cast<float>(theme::tokens().spacing.xs * 2);
	const QPointF cp1(from.x() + (to.x() - from.x()) * 0.4, from.y() - lift);
	const QPointF cp2(to.x() - (to.x() - from.x()) * 0.4, to.y() + lift);
	const float u = 1.f - t;
	return QPointF(
		u*u*u*from.x() + 3.f*u*u*t*cp1.x() + 3.f*u*t*t*cp2.x() + t*t*t*to.x(),
		u*u*u*from.y() + 3.f*u*u*t*cp1.y() + 3.f*u*t*t*cp2.y() + t*t*t*to.y());
}

QString format_base_label(const pointer_scanner::pointer_chain_t& chain)
{
	if (chain.is_static) {
		const std::string module =
			PointerScanController::cached_module_name(chain.module_index);
		if (!module.empty())
			return QStringLiteral("%1+0x%2")
				.arg(QString::fromStdString(module))
				.arg(chain.base_offset, 0, 16).toUpper();
	}
	return QStringLiteral("0x%1").arg(chain.base_offset, 0, 16).toUpper();
}

}

ChainDiagramWidget::ChainDiagramWidget(QWidget* parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("aida.memory.pointer.chain_diagram"));
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	reveal_ = new QVariantAnimation(this);
	reveal_->setStartValue(0.0);
	reveal_->setEndValue(1.0);
	reveal_->setDuration(theme::AidaMotion::reducedMotion()
		? 0 : theme::tokens().motion.emphasized);
	reveal_->setEasingCurve(QEasingCurve::OutCubic);
	connect(reveal_, &QVariantAnimation::valueChanged, this,
		[this](const QVariant& value) {
			reveal_value_ = value.toFloat();
			update();
		});
	ticker_ = theme::motion::loop(theme::tokens().motion.hero, this);
	connect(ticker_, &QVariantAnimation::valueChanged, this,
		[this](const QVariant& value) {
			const float next = value.toFloat();
			float delta = next - tick_phase_;
			if (delta < 0.f)
				delta += 1.f;
			tick_phase_ = next;
			for (auto& flash : step_flash_)
				if (flash > 0.f)
					flash = (std::max)(0.f, flash - 2.f * delta);
			update();
		});
}

ChainDiagramWidget::~ChainDiagramWidget() = default;

void ChainDiagramWidget::set_chain(const pointer_scanner::pointer_chain_t& chain)
{
	chain_ = chain;
	base_label_ = format_base_label(chain);
	for (auto& flash : step_flash_)
		flash = 0.f;
	hover_step_ = -1;
	relayout();
	reveal_value_ = 0.f;
	reveal_->stop();
	reveal_->setDuration(theme::AidaMotion::reducedMotion()
		? 0 : theme::tokens().motion.emphasized);
	reveal_->start();
	update();
}

void ChainDiagramWidget::clear_chain()
{
	chain_.reset();
	base_label_.clear();
	relayout();
	update();
}

QSize ChainDiagramWidget::sizeHint() const
{
	const auto& tokens = theme::tokens();
	return QSize(mono_cell_width() * 54,
		tokens.row.inspector * 3 + tokens.row.compact);
}

QSize ChainDiagramWidget::minimumSizeHint() const
{
	const auto& tokens = theme::tokens();
	return QSize(mono_cell_width() * 26,
		tokens.row.inspector * 2 + tokens.row.compact);
}

qreal ChainDiagramWidget::text_advance(const QString& label)
{
	const auto found = advance_cache_.constFind(label);
	if (found != advance_cache_.constEnd())
		return found.value();
	const qreal advance = QFontMetricsF(theme::fonts::codeRegular())
		.horizontalAdvance(label);
	if (advance_cache_.size() >= 256)
		advance_cache_.clear();
	advance_cache_.insert(label, advance);
	return advance;
}

void ChainDiagramWidget::relayout()
{
	step_rects_.clear();
	arrow_from_.clear();
	arrow_to_.clear();
	if (!chain_)
		return;
	const auto& tokens = theme::tokens();
	const auto& chain = *chain_;
	const std::size_t total_steps = chain.offsets.size() + 1;
	const float w = static_cast<float>(width());
	const float box_h = box_height();
	const float row_y = static_cast<float>(height()) * 0.5f -
		box_h * 0.5f - static_cast<float>(tokens.spacing.xxs);
	const float pad = diagram_pad();
	const float text_pad = static_cast<float>(2 * tokens.table.cell_pad_x +
		tokens.spacing.xs);

	QVector<float> widths;
	widths.reserve(static_cast<int>(total_steps));
	float total_w = 0.f;
	{
		const float bw = static_cast<float>(text_advance(base_label_)) + text_pad;
		widths.push_back(bw);
		total_w += bw;
	}
	for (std::size_t i = 0; i < chain.offsets.size(); ++i) {
		const QString offset = QString::fromStdString(
			PointerScanController::format_offset(chain.offsets[i]));
		const float bw = static_cast<float>(text_advance(offset)) + text_pad;
		widths.push_back(bw);
		total_w += bw + gap_width();
	}
	float scale = 1.f;
	float gap = gap_width();
	if (total_w + pad * 2.f > w) {
		scale = (w - pad * 2.f) / total_w;
		if (scale < kMinScale)
			scale = kMinScale;
		gap *= scale;
		for (auto& bw : widths)
			bw *= scale;
	}
	float cx = pad;
	step_rects_.push_back(QRectF(cx, row_y, widths[0], box_h));
	cx += widths[0];
	for (std::size_t i = 0; i < chain.offsets.size(); ++i) {
		arrow_from_.push_back(QPointF(cx, row_y + box_h * 0.5f));
		arrow_to_.push_back(QPointF(cx + gap, row_y + box_h * 0.5f));
		cx += gap;
		step_rects_.push_back(QRectF(cx, row_y, widths[i + 1], box_h));
		cx += widths[i + 1];
	}
}

int ChainDiagramWidget::step_at(const QPoint& pos) const
{
	for (int i = 0; i < step_rects_.size(); ++i)
		if (step_rects_[i].contains(pos))
			return i;
	return -1;
}

QString ChainDiagramWidget::step_tooltip(int step) const
{
	if (!chain_)
		return {};
	if (step == 0) {
		const auto base = PointerScanController::chain_base_address(*chain_);
		if (base)
			return QStringLiteral("Resolved: 0x%1")
				.arg(*base, 0, 16).toUpper();
	}
	resolution_status_t status = resolution_status_t::idle;
	std::string error;
	const auto address = PointerScanController::instance().resolved_step_address(
		*chain_, step, &status, &error);
	if (address)
		return QStringLiteral("Resolved: 0x%1").arg(*address, 0, 16).toUpper();
	if (status == resolution_status_t::queued || status == resolution_status_t::running)
		return QStringLiteral("Resolving in background...");
	if (!error.empty())
		return QStringLiteral("Unavailable: %1").arg(QString::fromStdString(error));
	return QStringLiteral("Select the chain to resolve its hops.");
}

void ChainDiagramWidget::paintEvent(QPaintEvent* event)
{
	static_cast<void>(event);
	QPainter painter(this);
	const auto& tokens = theme::tokens();
	const qreal box_radius = tokens.radius.lg;
	const qreal outline_w = qreal(tokens.panel.border);
	const qreal accent_w = qreal(tokens.control.focus_ring);
	painter.fillRect(rect(), widgets::with_alpha(tokens.bg_overlay, 0.55));
	painter.setRenderHint(QPainter::Antialiasing, true);
	if (!chain_) {
		painter.setFont(theme::fonts::body());
		painter.setPen(tokens.text_dim);
		painter.drawText(rect(), Qt::AlignCenter,
			QStringLiteral("Select a chain to inspect its hops"));
		return;
	}
	const auto& chain = *chain_;
	const float a = 1.f;
	painter.setFont(theme::fonts::codeRegular());

	for (int step = 0; step < step_rects_.size(); ++step) {
		float local_t = reveal_value_;
		if (step > 0) {
			const float reveal_offset = static_cast<float>(step) * 0.16f;
			local_t = (reveal_value_ - reveal_offset) /
				(1.f - reveal_offset + 0.0001f);
			local_t = std::clamp(local_t, 0.f, 1.f);
		}
		QRectF box = step_rects_[step];
		const float lift = (1.f - local_t) * static_cast<float>(tokens.spacing.sm);
		box.translate(0, lift);
		QColor fill = step == 0 && chain.is_static
			? widgets::with_alpha(tokens.accent_dim, a * 0.55 * local_t)
			: widgets::with_alpha(tokens.panel_bg, a * local_t);
		QColor border = step == 0 && chain.is_static
			? widgets::with_alpha(tokens.accent, a * local_t)
			: widgets::with_alpha(tokens.border_subtle, a * local_t);
		const float flash = step_flash_[step & 15];
		if (flash > 0.f) {
			border = widgets::with_alpha(tokens.accent,
				a * (0.6 + flash * 0.4));
			fill = widgets::with_alpha(tokens.accent_glow,
				a * (step == 0
					? (0.6 * flash + (chain.is_static ? 0.55 : 0.0))
					: 0.4 * flash));
		}
		if (step > 0) {
			const QPointF from = arrow_from_[step - 1];
			const QPointF to = arrow_to_[step - 1];
			if (local_t > 0.001f) {
				QPen pen(widgets::with_alpha(tokens.accent, a * local_t),
					accent_w);
				painter.setPen(pen);
				const int segments = 24;
				const float clip_t = local_t;
				QVector<QPointF> segments_points;
				segments_points.reserve(segments + 1);
				for (int i = 0; i <= segments; ++i) {
					float t2 = static_cast<float>(i) / static_cast<float>(segments);
					if (t2 > clip_t) t2 = clip_t;
					segments_points.push_back(bezier_point(from, to, t2));
					if (t2 >= clip_t) break;
				}
				painter.drawPolyline(segments_points.constData(),
					segments_points.size());
				if (local_t >= 0.99f) {
					const float head = static_cast<float>(tokens.spacing.xs +
						tokens.panel.border);
					const QPointF left(to.x() - head, to.y() - head * 0.6);
					const QPointF right(to.x() - head, to.y() + head * 0.6);
					painter.setBrush(widgets::with_alpha(tokens.accent, a * local_t));
					painter.setPen(Qt::NoPen);
					painter.drawPolygon(QVector<QPointF>{left, right, to});
				}
				const float phase = tick_phase_;
				if (phase <= local_t) {
					const QPointF dot = bezier_point(from, to, phase);
					painter.setBrush(widgets::with_alpha(tokens.accent, a * local_t));
					painter.setPen(Qt::NoPen);
					painter.drawEllipse(dot, qreal(tokens.spacing.xxs),
						qreal(tokens.spacing.xxs));
				}
			}
		}
		painter.setPen(Qt::NoPen);
		painter.setBrush(fill);
		painter.drawRoundedRect(box, box_radius, box_radius);
		painter.setPen(QPen(border, outline_w));
		painter.setBrush(Qt::NoBrush);
		painter.drawRoundedRect(box, box_radius, box_radius);
		QString label;
		if (step == 0) {
			label = base_label_;
		} else {
			label = QString::fromStdString(PointerScanController::format_offset(
				chain.offsets[static_cast<std::size_t>(step - 1)]));
		}
		const int cell_pad = tokens.table.cell_pad_x;
		if (step > 0) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(widgets::with_alpha(tokens.accent, a * local_t));
			const qreal dot_r = qreal(tokens.radius.xs);
			painter.drawEllipse(QPointF(box.left() + cell_pad, box.center().y()),
				dot_r, dot_r);
		}
		const QFontMetricsF metrics(painter.font());
		const qreal text_x = step > 0 ? box.left() + 2 * cell_pad : box.left() +
			(box.width() - metrics.horizontalAdvance(label)) / 2.0;
		const qreal baseline = box.top() +
			(box.height() - metrics.height()) / 2.0 + metrics.ascent();
		painter.setPen(widgets::with_alpha(tokens.text_primary, a * local_t));
		painter.drawText(QPointF(text_x, baseline), label);
		if (hasFocus() && step == hover_step_) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(Qt::NoBrush);
			widgets::paint_focus_ring(painter, box, box_radius, 1.0);
		}
	}
}

void ChainDiagramWidget::resizeEvent(QResizeEvent* event)
{
	relayout();
	QWidget::resizeEvent(event);
}

void ChainDiagramWidget::activate_step(int step)
{
	if (!chain_ || step < 0)
		return;
	if (step == 0) {
		const auto base = PointerScanController::chain_base_address(*chain_);
		if (base)
			Q_EMIT stepActivated(0, *base);
		return;
	}
	if (const auto address = PointerScanController::instance().resolved_step_address(
			*chain_, step)) {
		Q_EMIT stepActivated(step, *address);
		return;
	}
	Q_EMIT resolutionRequested(step);
}

void ChainDiagramWidget::set_hot_step(int step)
{
	if (step == hover_step_)
		return;
	hover_step_ = step;
	if (step >= 0)
		step_flash_[step & 15] = 1.f;
	update();
}

void ChainDiagramWidget::mousePressEvent(QMouseEvent* event)
{
	if (!chain_ || event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}
	const int step = step_at(event->pos());
	if (step < 0) {
		QWidget::mousePressEvent(event);
		return;
	}
	setFocus();
	set_hot_step(step);
	activate_step(step);
}

void ChainDiagramWidget::mouseMoveEvent(QMouseEvent* event)
{
	const int step = step_at(event->pos());
	if (step != hover_step_) {
		set_hot_step(step);
		if (step >= 0) {
			QToolTip::showText(event->globalPosition().toPoint(),
				step_tooltip(step), this, QRect());
		} else {
			QToolTip::hideText();
		}
	}
	QWidget::mouseMoveEvent(event);
}

void ChainDiagramWidget::keyPressEvent(QKeyEvent* event)
{
	if (!chain_) {
		QWidget::keyPressEvent(event);
		return;
	}
	const int last = step_rects_.size() - 1;
	switch (event->key()) {
	case Qt::Key_Left:
		set_hot_step(hover_step_ < 0 ? last : (hover_step_ > 0 ? hover_step_ - 1 : 0));
		event->accept();
		return;
	case Qt::Key_Right:
		set_hot_step(hover_step_ < 0 ? 0 : (hover_step_ < last ? hover_step_ + 1 : last));
		event->accept();
		return;
	case Qt::Key_Home:
		set_hot_step(0);
		event->accept();
		return;
	case Qt::Key_End:
		set_hot_step(last);
		event->accept();
		return;
	case Qt::Key_Return:
	case Qt::Key_Enter:
	case Qt::Key_Space:
		if (hover_step_ >= 0) {
			activate_step(hover_step_);
			event->accept();
			return;
		}
		break;
	default:
		break;
	}
	QWidget::keyPressEvent(event);
}

void ChainDiagramWidget::leaveEvent(QEvent* event)
{
	if (!hasFocus())
		set_hot_step(-1);
	QToolTip::hideText();
	QWidget::leaveEvent(event);
}

void ChainDiagramWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	if (ticker_->state() != QAbstractAnimation::Running)
		ticker_->start();
}

void ChainDiagramWidget::hideEvent(QHideEvent* event)
{
	ticker_->stop();
	QWidget::hideEvent(event);
}

}
