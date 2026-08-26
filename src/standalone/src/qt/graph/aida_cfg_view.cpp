#include "qt/graph/aida_cfg_view.hpp"

#include "qt/graph/cfg_minimap.hpp"
#include "qt/analysis_bridge/disasm_workspace_model.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_state_view.hpp"

#include "core/disasm/cfg_view.hpp"
#include "helpers/diag_log.hpp"

#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace aida::qt::graph {

namespace {

constexpr qreal k_zoom_min_live = 0.1;
constexpr qreal k_zoom_max_live = 5.0;
constexpr qreal k_grid_budget = 4000.0;

qreal grid_step()
{
    const auto& t = theme::tokens();
    return static_cast<qreal>(t.spacing.section + t.spacing.sm);
}

}

AidaCfgModelBroker& AidaCfgModelBroker::instance()
{
    static AidaCfgModelBroker broker;
    return broker;
}

AidaCfgModelBroker::AidaCfgModelBroker(QObject* parent) : QObject(parent) {}

void AidaCfgModelBroker::notifyModelPublished()
{
    Q_EMIT modelPublished();
}

AidaCfgView::AidaCfgView(QWidget* parent) : QGraphicsView(parent)
{
    setObjectName(QStringLiteral("aida.view.debug.cfg"));
    setFrameShape(QFrame::NoFrame);
    setDragMode(QGraphicsView::NoDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
    setOptimizationFlag(QGraphicsView::DontAdjustForAntialiasing, true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("Debugger control-flow graph"));
    controller_ = new CfgSceneController(this);
    controller_->setLiveContext(disasm_view::capture_selected_workspace());
    setScene(controller_->scene());

    static std::once_flag hook_once;
    std::call_once(hook_once, [] {
        cfg_view::set_model_publish_hook([] {
            AidaCfgModelBroker::instance().notifyModelPublished();
        });
    });
    connect(&AidaCfgModelBroker::instance(), &AidaCfgModelBroker::modelPublished, this,
        &AidaCfgView::onModelPublished);
    connect(controller_, &CfgSceneController::fitRequested, this,
        &AidaCfgView::onFitRequested);
    connect(controller_, &CfgSceneController::zoomStepRequested, this,
        &AidaCfgView::onZoomStep);
    connect(controller_, &CfgSceneController::resetRequested, this,
        &AidaCfgView::onReset);
    connect(controller_, &CfgSceneController::navigateToDisassembly, this,
        [this](quint64) {
            const auto hook = aida::qt::analysis_bridge::view_focus_hook();
            if (hook)
                hook("document.disassembly");
        });
    connect(controller_, &CfgSceneController::contentChanged, this, [this] {
        if (minimap_) {
            minimap_->refit();
            minimap_->viewport()->update();
        }
    });

    building_timer_ = new QTimer(this);
    building_timer_->setInterval(100);
    connect(building_timer_, &QTimer::timeout, this, &AidaCfgView::onBuildingTick);
    building_timer_->start();

    minimap_ = new CfgMinimap(this);
    minimap_->setTrackedView(this);
    minimap_->setScene(controller_->scene());
    minimap_->setFixedSize(k_minimap_logical_size);
    minimap_->show();
    auto* pill_frame = new QFrame(this);
    pill_frame->setObjectName(QStringLiteral("aida.graph.zoom_pill"));
    pill_frame->setFrameShape(QFrame::NoFrame);
    pill_frame->setProperty("aidaRole", QStringLiteral("overlay_chip"));
    zoom_pill_ = pill_frame;
    const auto& t = theme::tokens();
    auto* pill_layout = new QHBoxLayout(zoom_pill_);
    pill_layout->setContentsMargins(t.spacing.sm, t.spacing.xs, t.spacing.sm,
        t.spacing.xs);
    pill_layout->setSpacing(t.spacing.xs);
    const auto pill_button = [this](const QString& id, const QString& label,
                                    const QString& tooltip) {
        auto* button = new QToolButton(zoom_pill_);
        button->setObjectName(QStringLiteral("aida.graph.zoom_pill.") + id);
        button->setText(label);
        button->setToolTip(tooltip);
        button->setAutoRaise(true);
        return button;
    };
    auto* zoom_out = pill_button(QStringLiteral("out"), QStringLiteral("-"),
        QStringLiteral("Zoom out (-)"));
    zoom_label_ = new QLabel(QStringLiteral("100%"), zoom_pill_);
    zoom_label_->setMinimumWidth(zoom_label_->fontMetrics().horizontalAdvance(
        QStringLiteral("000%")) + t.spacing.sm);
    zoom_label_->setAlignment(Qt::AlignCenter);
    zoom_label_->setAccessibleName(QStringLiteral("Current zoom level"));
    auto* zoom_in = pill_button(QStringLiteral("in"), QStringLiteral("+"),
        QStringLiteral("Zoom in (+)"));
    auto* fit = pill_button(QStringLiteral("fit"), QStringLiteral("Fit"),
        QStringLiteral("Fit all graph blocks in the canvas (F)"));
    auto* home = pill_button(QStringLiteral("home"), QStringLiteral("Home"),
        QStringLiteral("Center the graph entry block (Home)"));
    pill_layout->addWidget(zoom_out);
    pill_layout->addWidget(zoom_label_);
    pill_layout->addWidget(zoom_in);
    pill_layout->addWidget(fit);
    pill_layout->addWidget(home);
    zoom_pill_->adjustSize();
    zoom_pill_->show();
    connect(zoom_out, &QToolButton::clicked, this, [this] { onZoomStep(0.85); });
    connect(zoom_in, &QToolButton::clicked, this, [this] { onZoomStep(1.18); });
    connect(fit, &QToolButton::clicked, this, [this] { onFitRequested(); });
    connect(home, &QToolButton::clicked, this, [this] {
        const auto model = cfg_view::capture_model();
        if (model && model->entry_addr != 0)
            centerOnAddress(model->entry_addr);
    });
    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.view.debug.cfg.state_view"));
    state_view_->hide();
    updateMinimapGeometry();
    syncEmptyState();
}

void AidaCfgView::syncEmptyState()
{
    const bool building = cfg_view::building();
    const bool has_content = !controller_->worldBounds().isNull();
    if (has_content) {
        state_view_->hide();
    } else if (building) {
        state_view_->setState(widgets::AidaStateView::State::Loading);
        state_view_->setTitle(QStringLiteral("Building CFG"));
        state_view_->setMessage(QStringLiteral(
            "Recovering basic blocks and edges for the selected function."));
        state_view_->setActionLabel(QString());
        state_view_->show();
    } else {
        const auto context = disasm_view::capture_selected_workspace();
        const auto progress = context.workspace
            ? context.workspace->progress() : aida::analysis::workspace_progress_t{};
        if (context.workspace &&
            progress.readiness == aida::analysis::workspace_readiness_t::failed) {
            state_view_->setState(widgets::AidaStateView::State::Error);
            state_view_->setTitle(QStringLiteral("Control-flow recovery failed"));
            state_view_->setMessage(progress.error && !progress.error->message.empty()
                ? QString::fromStdString(progress.error->message)
                : QStringLiteral(
                    "Analysis failed before any basic blocks were published."));
            state_view_->setActionLabel(QString());
            state_view_->show();
        } else {
            state_view_->setState(widgets::AidaStateView::State::Empty);
            state_view_->setTitle(QStringLiteral("No control-flow graph"));
            state_view_->setMessage(QStringLiteral(
                "Select a function in the debugger to recover its control-flow graph."));
            state_view_->setActionLabel(QString());
            state_view_->show();
        }
    }
    if (minimap_)
        minimap_->setVisible(has_content);
    if (state_view_->isVisible())
        state_view_->raise();
}

AidaCfgView::~AidaCfgView() = default;

void AidaCfgView::refreshContext()
{
    controller_->setLiveContext(disasm_view::capture_selected_workspace());
}

void AidaCfgView::rebuildCurrent()
{
    controller_->applyLiveModel(cfg_view::capture_model());
}

void AidaCfgView::onModelPublished()
{
    controller_->applyLiveModel(cfg_view::capture_model());
    syncEmptyState();
}

void AidaCfgView::onBuildingTick()
{
    const bool building = cfg_view::building();
    if (building != building_shown_) {
        building_shown_ = building;
        viewport()->update();
        syncEmptyState();
    }
}

void AidaCfgView::onFitRequested()
{
    const qreal pad = static_cast<qreal>(theme::tokens().spacing.section) * 2.0;
    const QRectF bounds = controller_->worldBounds().adjusted(-pad, -pad, pad, pad);
    if (bounds.isNull() || bounds.width() < 1.0 || bounds.height() < 1.0)
        return;
    fitInView(bounds, Qt::KeepAspectRatio);
    const qreal scale = transform().m11();
    if (scale < k_zoom_min_live)
        applyZoomClamped(k_zoom_min_live);
    else if (scale > k_zoom_max_live)
        applyZoomClamped(k_zoom_max_live);
    else
        current_zoom_ = scale;
    target_zoom_ = transform().m11();
    target_center_ = mapToScene(viewport()->rect()).boundingRect().center();
    clampTargetCenter();
    onZoomLabelUpdate();
}

void AidaCfgView::onZoomStep(qreal factor)
{
    applyZoomClamped((std::clamp)(current_zoom_ * factor, k_zoom_min_live,
        k_zoom_max_live));
}

void AidaCfgView::onReset()
{
    target_zoom_ = 1.0;
    target_center_ = controller_->worldBounds().center();
    updateTransform(false);
}

void AidaCfgView::applyZoomClamped(qreal target)
{
    target = (std::clamp)(target, k_zoom_min_live, k_zoom_max_live);
    target_center_ = mapToScene(viewport()->rect()).boundingRect().center();
    clampTargetCenter();
    const qreal factor = target / (std::max)(current_zoom_, 0.0001);
    setTransformationAnchor(QGraphicsView::AnchorViewCenter);
    scale(factor, factor);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    current_zoom_ = transform().m11();
    target_zoom_ = current_zoom_;
    centerOn(target_center_);
    onZoomLabelUpdate();
    if (minimap_)
        minimap_->viewport()->update();
}

void AidaCfgView::clampTargetCenter()
{
    const QRectF bounds = controller_->worldBounds();
    if (bounds.isNull() || bounds.width() < 1.0 || bounds.height() < 1.0)
        return;
    const qreal slack = static_cast<qreal>(theme::tokens().spacing.section) * 6.0 +
        static_cast<qreal>(theme::tokens().spacing.sm);
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    const qreal margin_x = (std::max)(slack, visible.width() * 0.5);
    const qreal margin_y = (std::max)(slack, visible.height() * 0.5);
    target_center_.rx() = (std::clamp)(target_center_.x(),
        bounds.left() - margin_x, bounds.right() + margin_x);
    target_center_.ry() = (std::clamp)(target_center_.y(),
        bounds.top() - margin_y, bounds.bottom() + margin_y);
}

void AidaCfgView::updateTransform(bool animate)
{
    clampTargetCenter();
    const qreal factor = target_zoom_ / (std::max)(current_zoom_, 0.0001);
    if (animate && !theme::AidaMotion::reducedMotion()) {
        if (!center_anim_) {
            center_anim_ = new QVariantAnimation(this);
            center_anim_->setDuration(theme::tokens().motion.fast);
            center_anim_->setEasingCurve(theme::easingFor(theme::Ease::OutCubic));
            connect(center_anim_, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& value) {
                    centerOn(value.toPointF());
                });
        }
        center_anim_->stop();
        center_anim_->setStartValue(
            mapToScene(viewport()->rect()).boundingRect().center());
        center_anim_->setEndValue(target_center_);
        center_anim_->start();
    } else {
        centerOn(target_center_);
    }
    if (std::abs(factor - 1.0) > 0.0001)
        scale(factor, factor);
    current_zoom_ = target_zoom_;
    onZoomLabelUpdate();
    if (minimap_)
        minimap_->viewport()->update();
}

void AidaCfgView::centerOnAddress(std::uint64_t address)
{
    const auto node = controller_->nodeForAddress(address);
    if (!node)
        return;
    const QRectF rect = controller_->blockSceneRect(*node);
    if (rect.isNull())
        return;
    target_center_ = rect.center();
    updateTransform(!theme::AidaMotion::reducedMotion());
}

void AidaCfgView::drawBackground(QPainter* painter, const QRectF& rect)
{
    const auto& t = theme::tokens();
    painter->fillRect(rect, t.bg_base);
    const qreal span_x = rect.width();
    const qreal span_y = rect.height();
    if (span_x <= 0.0 || span_y <= 0.0)
        return;
    const qreal step = grid_step();
    const qreal columns = span_x / step;
    const qreal rows = span_y / step;
    if (columns * rows > k_grid_budget)
        return;
    const qreal gx0 = std::floor(rect.left() / step) * step;
    const qreal gy0 = std::floor(rect.top() / step) * step;
    const qreal gx1 = std::ceil(rect.right() / step) * step;
    const qreal gy1 = std::ceil(rect.bottom() / step) * step;
    QColor dot = t.border_subtle;
    dot.setAlphaF(dot.alphaF() * 0.6);
    QPolygonF dots;
    dots.reserve(static_cast<qsizetype>((gx1 - gx0) / step + 2) *
        static_cast<qsizetype>((gy1 - gy0) / step + 2));
    for (qreal gy = gy0; gy <= gy1; gy += step) {
        for (qreal gx = gx0; gx <= gx1; gx += step)
            dots << QPointF(gx, gy);
    }
    QPen dot_pen(dot, 2.0);
    dot_pen.setCapStyle(Qt::RoundCap);
    dot_pen.setCosmetic(true);
    painter->setPen(dot_pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPoints(dots);
}

void AidaCfgView::drawForeground(QPainter* painter, const QRectF& rect)
{
    QGraphicsView::drawForeground(painter, rect);
    const auto& t = theme::tokens();
    painter->save();
    painter->setWorldTransform(QTransform());
    const QRect view_rect = viewport()->rect();
    if (hasFocus()) {
        const qreal inset = static_cast<qreal>(t.control.focus_ring) * 0.5;
        painter->setPen(QPen(t.border_focus, static_cast<qreal>(t.control.focus_ring)));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(QRectF(view_rect).adjusted(inset, inset, -inset, -inset));
    }
    if (building_shown_ && !controller_->worldBounds().isNull()) {
        const QFontMetricsF metrics(theme::fonts::body());
        const QString text = QStringLiteral("Building CFG...");
        const qreal content_w = metrics.horizontalAdvance(text);
        const qreal content_h = metrics.height();
        const qreal pad_x = static_cast<qreal>(t.panel.padding);
        const qreal pad_y = static_cast<qreal>(t.spacing.sm);
        const qreal panel_w = content_w + pad_x * 2.0;
        const qreal panel_h = content_h + pad_y * 2.0;
        const QRectF panel(view_rect.center().x() - panel_w * 0.5,
            view_rect.top() + static_cast<qreal>(t.panel.padding), panel_w, panel_h);
        QColor glass = t.bg_overlay;
        glass.setAlphaF(glass.alphaF() * 0.94);
        painter->setPen(QPen(t.border_subtle, 1.0));
        painter->setBrush(glass);
        painter->drawRoundedRect(panel, static_cast<qreal>(t.radius.lg),
            static_cast<qreal>(t.radius.lg));
        painter->setFont(theme::fonts::body());
        painter->setPen(t.text_primary);
        painter->drawText(panel, Qt::AlignCenter, text);
    }
    painter->restore();
}

void AidaCfgView::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        const qreal old_zoom = target_zoom_;
        target_zoom_ = (std::clamp)(
            target_zoom_ * (delta > 0 ? 1.1 : 0.9), k_zoom_min_live, k_zoom_max_live);
        const qreal factor = target_zoom_ / old_zoom;
        scale(factor, factor);
        current_zoom_ = target_zoom_;
        target_center_ = mapToScene(viewport()->rect()).boundingRect().center();
        clampTargetCenter();
        onZoomLabelUpdate();
        if (minimap_)
            minimap_->viewport()->update();
        event->accept();
        return;
    }
    const qreal step = grid_step() / (std::max)(current_zoom_, 0.01);
    if (event->modifiers() & Qt::ShiftModifier) {
        target_center_.rx() += delta > 0 ? -step : step;
    } else {
        target_center_.ry() += delta > 0 ? -step : step;
    }
    updateTransform(false);
    event->accept();
}

void AidaCfgView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        middle_dragging_ = true;
        last_drag_pos_ = event->pos();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void AidaCfgView::mouseMoveEvent(QMouseEvent* event)
{
    if (middle_dragging_) {
        const QPoint delta = event->pos() - last_drag_pos_;
        last_drag_pos_ = event->pos();
        target_center_.rx() -= delta.x() / (std::max)(current_zoom_, 0.01);
        target_center_.ry() -= delta.y() / (std::max)(current_zoom_, 0.01);
        updateTransform(false);
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void AidaCfgView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton && middle_dragging_) {
        middle_dragging_ = false;
        viewport()->unsetCursor();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void AidaCfgView::keyPressEvent(QKeyEvent* event)
{
    if (!event->modifiers()) {
        const qreal pan_step = grid_step() / (std::max)(current_zoom_, 0.01);
        switch (event->key()) {
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            onZoomStep(1.18);
            event->accept();
            return;
        case Qt::Key_Minus:
            onZoomStep(0.85);
            event->accept();
            return;
        case Qt::Key_F:
            onFitRequested();
            event->accept();
            return;
        case Qt::Key_Home: {
            const auto model = cfg_view::capture_model();
            if (model && model->entry_addr != 0)
                centerOnAddress(model->entry_addr);
            event->accept();
            return;
        }
        case Qt::Key_Left:
            target_center_.rx() -= pan_step;
            updateTransform(false);
            event->accept();
            return;
        case Qt::Key_Right:
            target_center_.rx() += pan_step;
            updateTransform(false);
            event->accept();
            return;
        case Qt::Key_Up:
            target_center_.ry() -= pan_step;
            updateTransform(false);
            event->accept();
            return;
        case Qt::Key_Down:
            target_center_.ry() += pan_step;
            updateTransform(false);
            event->accept();
            return;
        default:
            break;
        }
    }
    if (event->key() == Qt::Key_Escape) {
        if (controller_->hasTextSelection()) {
            controller_->clearTextSelection();
            event->accept();
            return;
        }
        std::uint64_t back = controller_->lastCursorAddress();
        if (back == 0) {
            const auto model = cfg_view::capture_model();
            back = model ? model->entry_addr : 0;
        }
        if (back != 0) {
            disasm_view::goto_address(back, disasm_view::capture_selected_workspace());
            const auto hook = aida::qt::analysis_bridge::view_focus_hook();
            if (hook)
                hook("document.disassembly");
        }
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

void AidaCfgView::focusInEvent(QFocusEvent* event)
{
    QGraphicsView::focusInEvent(event);
    viewport()->update();
}

void AidaCfgView::focusOutEvent(QFocusEvent* event)
{
    QGraphicsView::focusOutEvent(event);
    viewport()->update();
}

void AidaCfgView::showEvent(QShowEvent* event)
{
    QGraphicsView::showEvent(event);
    building_timer_->start();
    rebuildCurrent();
    syncEmptyState();
}

void AidaCfgView::hideEvent(QHideEvent* event)
{
    QGraphicsView::hideEvent(event);
    building_timer_->stop();
}

void AidaCfgView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    updateMinimapGeometry();
    if (state_view_) {
        state_view_->setGeometry(0, 0, width(), height());
        if (state_view_->isVisible())
            state_view_->raise();
    }
}

void AidaCfgView::updateMinimapGeometry()
{
    const auto& t = theme::tokens();
    if (minimap_) {
        minimap_->move(width() - minimap_->width() - t.spacing.md,
            height() - minimap_->height() - t.spacing.md);
        minimap_->raise();
    }
    if (zoom_pill_) {
        zoom_pill_->move(t.spacing.md, height() - zoom_pill_->height() - t.spacing.md);
        zoom_pill_->raise();
    }
}

void AidaCfgView::onZoomLabelUpdate()
{
    if (zoom_label_)
        zoom_label_->setText(QStringLiteral("%1%").arg(
            static_cast<int>(current_zoom_ * 100.0)));
}

}
