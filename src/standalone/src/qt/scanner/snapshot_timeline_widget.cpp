#include "qt/scanner/snapshot_timeline_widget.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>

#include <algorithm>

#include "core/scanner/snapshot_diff.hpp"

#include "qt/scanner/scanner_palette.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::scanner {

SnapshotTimelineWidget::SnapshotTimelineWidget(QWidget* parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("aida.memory.snapshot.timeline"));
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	const int track_h = theme::tokens().row.inspector * 2;
	setMinimumHeight(track_h);
	setMaximumHeight(track_h);
	cursor_anim_ = theme::motion::loop(theme::tokens().motion.xxl, this);
	connect(cursor_anim_, &QVariantAnimation::valueChanged, this,
		[this](const QVariant& value) {
			cursor_t_ = value.toFloat();
			update();
		});
	refresh_from_engine();
}

SnapshotTimelineWidget::~SnapshotTimelineWidget() = default;

QSize SnapshotTimelineWidget::sizeHint() const
{
	return QSize(mono_cell_width() * 54, theme::tokens().row.inspector * 2);
}

QSize SnapshotTimelineWidget::minimumSizeHint() const
{
	return QSize(mono_cell_width() * 22, theme::tokens().row.inspector * 2);
}

void SnapshotTimelineWidget::set_comparing(bool comparing)
{
	if (comparing_ == comparing)
		return;
	comparing_ = comparing;
	if (comparing_)
		cursor_anim_->start();
	else
		cursor_anim_->stop();
	update();
}

void SnapshotTimelineWidget::refresh_from_engine()
{
	auto& state = snapshot_diff::g_state;
	std::lock_guard<std::mutex> lock(state.mutex);
	markers_.clear();
	markers_.reserve(static_cast<int>(state.snapshots.size()));
	for (const auto& snapshot : state.snapshots)
		markers_.push_back({snapshot->id,
			QString::fromStdString(snapshot->name), QPointF()});
	selected_a_ = state.snap_a_id;
	selected_b_ = state.snap_b_id;
	update();
}

int SnapshotTimelineWidget::marker_at(const QPoint& pos) const
{
	const float hit = static_cast<float>(theme::tokens().row.compact);
	for (int i = 0; i < markers_.size(); ++i) {
		const QPointF center = markers_[i].position;
		if (QRectF(center.x() - hit * 0.5f, center.y() - hit * 0.5f,
				hit, hit).contains(pos))
			return i;
	}
	return -1;
}

void SnapshotTimelineWidget::activate_marker(int index, bool clear)
{
	if (index < 0 || index >= markers_.size())
		return;
	auto& state = snapshot_diff::g_state;
	const std::uint64_t this_id = markers_[index].id;
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		if (clear) {
			if (state.snap_a_id == this_id) state.snap_a_id = 0;
			if (state.snap_b_id == this_id) state.snap_b_id = 0;
		} else {
			if (state.snap_a_id == this_id) state.snap_a_id = 0;
			else if (state.snap_b_id == this_id) state.snap_b_id = 0;
			else if (state.snap_a_id == 0) state.snap_a_id = this_id;
			else if (state.snap_b_id == 0) state.snap_b_id = this_id;
			else state.snap_b_id = this_id;
		}
	}
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		selected_a_ = state.snap_a_id;
		selected_b_ = state.snap_b_id;
	}
	Q_EMIT selectionChanged();
	update();
}

void SnapshotTimelineWidget::set_hot_marker(int index)
{
	if (index == hot_marker_)
		return;
	hot_marker_ = index;
	update();
}

void SnapshotTimelineWidget::mousePressEvent(QMouseEvent* event)
{
	const int index = marker_at(event->pos());
	if (index < 0) {
		QWidget::mousePressEvent(event);
		return;
	}
	setFocus();
	set_hot_marker(index);
	if (event->button() == Qt::LeftButton)
		activate_marker(index, false);
	else if (event->button() == Qt::RightButton)
		activate_marker(index, true);
}

void SnapshotTimelineWidget::mouseMoveEvent(QMouseEvent* event)
{
	set_hot_marker(marker_at(event->pos()));
	QWidget::mouseMoveEvent(event);
}

void SnapshotTimelineWidget::keyPressEvent(QKeyEvent* event)
{
	if (markers_.isEmpty()) {
		QWidget::keyPressEvent(event);
		return;
	}
	const int last = markers_.size() - 1;
	switch (event->key()) {
	case Qt::Key_Left:
		set_hot_marker(hot_marker_ < 0 ? last : (hot_marker_ > 0 ? hot_marker_ - 1 : 0));
		event->accept();
		return;
	case Qt::Key_Right:
		set_hot_marker(hot_marker_ < 0 ? 0 : (hot_marker_ < last ? hot_marker_ + 1 : last));
		event->accept();
		return;
	case Qt::Key_Home:
		set_hot_marker(0);
		event->accept();
		return;
	case Qt::Key_End:
		set_hot_marker(last);
		event->accept();
		return;
	case Qt::Key_Return:
	case Qt::Key_Enter:
	case Qt::Key_Space:
		if (hot_marker_ >= 0) {
			activate_marker(hot_marker_, false);
			event->accept();
			return;
		}
		break;
	case Qt::Key_Backspace:
	case Qt::Key_Delete:
		if (hot_marker_ >= 0) {
			activate_marker(hot_marker_, true);
			event->accept();
			return;
		}
		break;
	default:
		break;
	}
	QWidget::keyPressEvent(event);
}

void SnapshotTimelineWidget::leaveEvent(QEvent* event)
{
	if (!hasFocus())
		set_hot_marker(-1);
	QWidget::leaveEvent(event);
}

void SnapshotTimelineWidget::paintEvent(QPaintEvent* event)
{
	static_cast<void>(event);
	QPainter painter(this);
	const auto& tokens = theme::tokens();
	painter.fillRect(rect(), tokens.panel_bg);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const qreal frame_radius = tokens.radius.modal;
	painter.setPen(QPen(tokens.border_subtle, qreal(tokens.panel.border)));
	painter.setBrush(Qt::NoBrush);
	painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
		frame_radius, frame_radius);

	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	const float track_y = h * 0.5f;
	const float pad_x = static_cast<float>(tokens.spacing.xxl);
	const float track_x0 = pad_x;
	const float track_x1 = w - pad_x;
	const float track_w = track_x1 - track_x0;

	painter.setPen(QPen(tokens.border_strong, qreal(tokens.tab.underline)));
	painter.drawLine(QPointF(track_x0, track_y), QPointF(track_x1, track_y));

	if (markers_.isEmpty()) {
		painter.setPen(tokens.text_dim);
		painter.setFont(theme::fonts::body());
		painter.drawText(rect(), Qt::AlignCenter,
			QStringLiteral("Capture snapshots to populate the timeline"));
		return;
	}

	const int snap_count = markers_.size();
	const float seg = snap_count > 1
		? track_w / static_cast<float>(snap_count - 1) : 0.f;
	for (int i = 0; i < snap_count; ++i) {
		const float mx = snap_count > 1
			? track_x0 + seg * static_cast<float>(i)
			: track_x0 + track_w * 0.5f;
		markers_[i].position = QPointF(mx, track_y);
	}

	int idx_a = snap_count;
	int idx_b = snap_count;
	for (int i = 0; i < snap_count; ++i) {
		if (selected_a_ != 0 && markers_[i].id == selected_a_) idx_a = i;
		if (selected_b_ != 0 && markers_[i].id == selected_b_) idx_b = i;
	}

	if (idx_a < snap_count && idx_b < snap_count) {
		float ax = markers_[idx_a].position.x();
		float bx = markers_[idx_b].position.x();
		if (ax > bx)
			std::swap(ax, bx);
		const float span_half = static_cast<float>(tokens.tab.underline);
		if (!span_gradient_ ||
			span_gradient_->start().x() != ax || span_gradient_->finalStop().x() != bx) {
			QLinearGradient gradient(QPointF(ax, track_y - span_half),
				QPointF(ax, track_y + span_half));
			gradient.setColorAt(0, tokens.accent_grad_top);
			gradient.setColorAt(1, tokens.accent_grad_top);
			span_gradient_ = gradient;
		}
		painter.fillRect(QRectF(ax, track_y - span_half, bx - ax, span_half * 2.f),
			*span_gradient_);
		if (comparing_) {
			const float cx_pos = ax + (bx - ax) * cursor_t_;
			const float dot = static_cast<float>(tokens.status_bar.dot);
			painter.setPen(Qt::NoPen);
			painter.setBrush(tokens.accent);
			painter.drawEllipse(QPointF(cx_pos, track_y), dot, dot);
			painter.setPen(QPen(widgets::with_alpha(tokens.accent, 0.4),
				qreal(tokens.control.focus_ring)));
			painter.setBrush(Qt::NoBrush);
			painter.drawEllipse(QPointF(cx_pos, track_y),
				dot + tokens.spacing.xs, dot + tokens.spacing.xs);
		}
	}

	const QFontMetricsF metrics(theme::fonts::body());
	const float marker_r = static_cast<float>(tokens.spacing.sm);
	const float marker_gap = static_cast<float>(tokens.spacing.xxs);
	for (int i = 0; i < snap_count; ++i) {
		const QPointF center = markers_[i].position;
		const bool is_a = idx_a == i;
		const bool is_b = idx_b == i;
		const bool is_hot = hot_marker_ == i;
		QColor outer = tokens.panel_header;
		QColor inner = tokens.text_secondary;
		float r_outer = marker_r;
		if (is_a || is_b) {
			outer = tokens.accent;
			inner = tokens.accent_grad_top;
			r_outer = marker_r + marker_gap;
		}
		if (is_hot) {
			r_outer += marker_gap;
			painter.setPen(QPen(widgets::with_alpha(tokens.accent, 0.5),
				qreal(tokens.control.focus_ring)));
			painter.setBrush(Qt::NoBrush);
			painter.drawEllipse(center, r_outer + tokens.spacing.xs,
				r_outer + tokens.spacing.xs);
		}
		painter.setPen(Qt::NoPen);
		painter.setBrush(outer);
		painter.drawEllipse(center, r_outer, r_outer);
		painter.setBrush(inner);
		painter.drawEllipse(center, r_outer - qreal(tokens.radius.xs),
			r_outer - qreal(tokens.radius.xs));

		const QString name = metrics.elidedText(markers_[i].name,
			Qt::ElideRight, (std::max)(4 * tokens.spacing.sm,
				static_cast<int>(seg) - tokens.spacing.sm));
		const qreal name_w = metrics.horizontalAdvance(name);
		painter.setFont(theme::fonts::body());
		painter.setPen(tokens.text_secondary);
		painter.drawText(QPointF(center.x() - name_w / 2.0,
			center.y() + tokens.spacing.sm + tokens.spacing.xxs + metrics.ascent()),
			name);
		if (is_a) {
			painter.setPen(tokens.accent);
			painter.drawText(QPointF(center.x() - metrics.horizontalAdvance(
					QStringLiteral("A")) * 0.5,
				center.y() - tokens.spacing.xl - tokens.spacing.xxs +
					metrics.ascent()), QStringLiteral("A"));
		}
		if (is_b) {
			painter.setPen(tokens.accent);
			painter.drawText(QPointF(center.x() - metrics.horizontalAdvance(
					QStringLiteral("B")) * 0.5,
				center.y() - tokens.spacing.xl - tokens.spacing.xxs +
					metrics.ascent()), QStringLiteral("B"));
		}
	}
	if (hasFocus()) {
		const qreal ring_inset = tokens.spacing.xxs + tokens.panel.border;
		widgets::paint_focus_ring(painter,
			QRectF(rect()).adjusted(ring_inset, ring_inset, -ring_inset, -ring_inset),
			frame_radius, 1.0);
	}
}

}
