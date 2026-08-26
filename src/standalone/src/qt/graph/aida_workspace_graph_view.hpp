#pragma once

#include "core/disasm/disasm_view.hpp"
#include "qt/analysis_bridge/revision_poller.hpp"
#include "qt/graph/cfg_scene_controller.hpp"

#include <QGraphicsView>
#include <QPointF>

#include <cstdint>
#include <memory>
#include <vector>

class QLabel;
class QMenu;
class QTimer;
class QToolButton;
class QVariantAnimation;

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::graph {

class CfgMinimap;

class AidaWorkspaceGraphView : public QGraphicsView {
    Q_OBJECT
public:
    explicit AidaWorkspaceGraphView(QWidget* parent = nullptr);
    ~AidaWorkspaceGraphView() override;

    CfgSceneController* controller() const noexcept { return controller_; }
    void refreshContext();

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
    void contextMenuEvent(QContextMenuEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private Q_SLOTS:
    void onRevisionChanged(quint64 combined, quint64 overlayRevision);
    void onFitRequested();
    void onZoomStep(qreal factor);
    void onReset();

private:
    analysis_bridge::revision_sample_t sampleRevisions();
    void recaptureContext();
    void rebuildIfNeeded();
    void syncHeader();
    void syncEmptyState();
    void updateTransform(bool animate);
    void applyZoomClamped(qreal target);
    void applyFit();
    void clampTargetCenter();
    void updateZoomLabel();
    void updateMinimapGeometry();
    void updateHeaderOverflow();
    void setHeaderCompact(bool compact);
    void rebuildHeaderOverflowMenu();
    void applyNameElide();
    const aida::analysis::function_record_t* currentFunction() const;

    disasm_view::workspace_context_t context_;
    analysis_bridge::AidaRevisionPoller* poller_ = nullptr;
    CfgSceneController* controller_ = nullptr;
    CfgMinimap* minimap_ = nullptr;
    QWidget* header_ = nullptr;
    QLabel* name_label_ = nullptr;
    QLabel* address_label_ = nullptr;
    QLabel* page_label_ = nullptr;
    QToolButton* disassembly_button_ = nullptr;
    QToolButton* pseudocode_button_ = nullptr;
    QToolButton* fit_button_ = nullptr;
    QToolButton* zoom_out_button_ = nullptr;
    QToolButton* zoom_reset_button_ = nullptr;
    QToolButton* zoom_in_button_ = nullptr;
    QToolButton* prev_page_button_ = nullptr;
    QToolButton* next_page_button_ = nullptr;
    QToolButton* more_button_ = nullptr;
    QMenu* more_menu_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QVariantAnimation* center_anim_ = nullptr;
    std::vector<QToolButton*> overflow_buttons_;
    QString full_name_;
    bool header_compact_ = false;
    QPointF target_center_;
    qreal target_zoom_ = 1.0;
    qreal current_zoom_ = 1.0;
    bool middle_dragging_ = false;
    QPoint last_drag_pos_;
    std::uint64_t applied_signature_ = 0;
    const aida::analysis::function_record_t* function_ = nullptr;
    std::size_t total_blocks_ = 0;
    std::size_t page_count_ = 0;
    std::size_t page_begin_ = 0;
    std::size_t page_end_ = 0;
};

}
