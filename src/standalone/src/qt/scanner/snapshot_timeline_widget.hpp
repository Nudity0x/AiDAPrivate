#pragma once

#include <QWidget>

#include <QLinearGradient>
#include <QVector>

#include <cstdint>
#include <optional>

class QVariantAnimation;

namespace aida::qt::scanner {

class SnapshotTimelineWidget : public QWidget {
	Q_OBJECT
public:
	explicit SnapshotTimelineWidget(QWidget* parent = nullptr);
	~SnapshotTimelineWidget() override;

	void refresh_from_engine();
	void set_comparing(bool comparing);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

Q_SIGNALS:
	void selectionChanged();

protected:
	void paintEvent(QPaintEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;
	void leaveEvent(QEvent* event) override;

private:
	struct marker_t {
		std::uint64_t id = 0;
		QString name;
		QPointF position;
	};

	int marker_at(const QPoint& pos) const;
	void activate_marker(int index, bool clear);
	void set_hot_marker(int index);

	QVector<marker_t> markers_;
	std::uint64_t selected_a_ = 0;
	std::uint64_t selected_b_ = 0;
	int hot_marker_ = -1;
	bool comparing_ = false;
	QVariantAnimation* cursor_anim_ = nullptr;
	float cursor_t_ = 0.f;
	std::optional<QLinearGradient> span_gradient_;
};

}
