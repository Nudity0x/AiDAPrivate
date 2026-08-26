#pragma once

#include "core/disasm/disasm_view.hpp"
#include "qt/graph/cfg_scene_controller.hpp"

#include <QGraphicsView>
#include <QPointF>

#include <memory>

class QLabel;
class QTimer;
class QVariantAnimation;

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::graph {

class CfgMinimap;

class AidaCfgModelBroker : public QObject {
    Q_OBJECT
public:
    static AidaCfgModelBroker& instance();

    void notifyModelPublished();

Q_SIGNALS:
    void modelPublished();

private:
    explicit AidaCfgModelBroker(QObject* parent = nullptr);
};

class AidaCfgView : public QGraphicsView {
    Q_OBJECT
public:
    explicit AidaCfgView(QWidget* parent = nullptr);
    ~AidaCfgView() override;

    CfgSceneController* controller() const noexcept { return controller_; }
    void refreshContext();
    void rebuildCurrent();

protected:
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private Q_SLOTS:
    void onModelPublished();
    void onFitRequested();
    void onZoomStep(qreal factor);
    void onReset();
    void onBuildingTick();
    void onZoomLabelUpdate();

private:
    void updateTransform(bool animate);
    void applyZoomClamped(qreal target);
    void centerOnAddress(std::uint64_t address);
    void updateMinimapGeometry();
    void clampTargetCenter();
    void syncEmptyState();

    CfgSceneController* controller_ = nullptr;
    CfgMinimap* minimap_ = nullptr;
    QWidget* zoom_pill_ = nullptr;
    QLabel* zoom_label_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QTimer* building_timer_ = nullptr;
    QVariantAnimation* center_anim_ = nullptr;
    QPointF target_center_;
    qreal target_zoom_ = 1.0;
    qreal current_zoom_ = 1.0;
    bool middle_dragging_ = false;
    QPoint last_drag_pos_;
    bool fit_pending_ = false;
    bool building_shown_ = false;
};

}
