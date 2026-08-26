#pragma once

#include <QWidget>

#include <QHash>
#include <QRectF>
#include <QVector>

#include <optional>

#include "core/scanner/pointer_scanner.hpp"

class QVariantAnimation;

namespace aida::qt::scanner {

class ChainDiagramWidget : public QWidget {
	Q_OBJECT
public:
	explicit ChainDiagramWidget(QWidget* parent = nullptr);
	~ChainDiagramWidget() override;

	void set_chain(const pointer_scanner::pointer_chain_t& chain);
	void clear_chain();

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

Q_SIGNALS:
	void stepActivated(int step, std::uint64_t resolved_address);
	void resolutionRequested(int step);

protected:
	void paintEvent(QPaintEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;
	void leaveEvent(QEvent* event) override;
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;

private:
	void relayout();
	qreal text_advance(const QString& label);
	int step_at(const QPoint& pos) const;
	QString step_tooltip(int step) const;
	void activate_step(int step);
	void set_hot_step(int step);

	std::optional<pointer_scanner::pointer_chain_t> chain_;
	QString base_label_;
	QVector<QRectF> step_rects_;
	QVector<QPointF> arrow_from_;
	QVector<QPointF> arrow_to_;
	QVariantAnimation* reveal_ = nullptr;
	QVariantAnimation* ticker_ = nullptr;
	QHash<QString, qreal> advance_cache_;
	float reveal_value_ = 0.f;
	float tick_phase_ = 0.f;
	float step_flash_[16] = {};
	int hover_step_ = -1;
};

}
