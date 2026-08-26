#pragma once

#include <QGraphicsView>
#include <QSize>

namespace aida::qt::graph {

inline constexpr QSize k_minimap_logical_size(220, 140);

class CfgMinimap : public QGraphicsView {
    Q_OBJECT
public:
    explicit CfgMinimap(QWidget* parent = nullptr);

    void setTrackedView(QGraphicsView* view);
    void refit();

protected:
    void drawForeground(QPainter* painter, const QRectF& rect) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void trackTo(const QPointF& view_pos);
    QPointF clampedCenter(const QPointF& center) const;

    QGraphicsView* tracked_ = nullptr;
    bool dragging_ = false;
};

}
