#include "qt/graph/aida_workspace_graph_view.hpp"

#include "qt/graph/cfg_minimap.hpp"
#include "qt/analysis_bridge/disasm_workspace_model.hpp"
#include "qt/analysis_bridge/revision_combine.hpp"
#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_state_view.hpp"

#include "core/analysis/workspace/workspace_registry.hpp"
#include "core/disasm/cfg_view.hpp"
#include "core/disasm/pseudocode_view.hpp"
#include "core/ui/analysis_context_menu.hpp"

#include <QAction>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QToolButton>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace aida::qt::graph {

namespace {

constexpr qreal k_zoom_min_workspace = 0.16;
constexpr qreal k_zoom_max_workspace = 2.5;
constexpr qreal k_grid_budget = 5000.0;

qreal grid_step()
{
    const auto& t = theme::tokens();
    return static_cast<qreal>(t.spacing.section + t.spacing.sm);
}

}

AidaWorkspaceGraphView::AidaWorkspaceGraphView(QWidget* parent) : QGraphicsView(parent)
{
    setObjectName(QStringLiteral("aida.document.graph.primary"));
    setFrameShape(QFrame::NoFrame);
    setDragMode(QGraphicsView::NoDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
    setOptimizationFlag(QGraphicsView::DontAdjustForAntialiasing, true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Workspace control-flow graph"));
    bridge::InteractionContextProvider::attach_scope(this,
        QStringLiteral("scope.document.graph"), aida::ui::focus_scope_kind_t::document);

    controller_ = new CfgSceneController(this);
    controller_->setWorkspaceContext(disasm_view::capture_selected_workspace());
    setScene(controller_->scene());

    auto* header_frame = new QFrame(this);
    header_frame->setObjectName(QStringLiteral("aida.graph.workspace.header"));
    header_frame->setFrameShape(QFrame::NoFrame);
    header_frame->setProperty("aidaRole", QStringLiteral("header"));
    header_ = header_frame;
    auto* header_layout = new QHBoxLayout(header_);
    const auto& t = theme::tokens();
    header_layout->setContentsMargins(t.toolbar.padding_x, t.toolbar.padding_y,
        t.toolbar.padding_x, t.toolbar.padding_y);
    header_layout->setSpacing(t.toolbar.group_gap);
    name_label_ = new QLabel(QStringLiteral("Recovered function"), header_);
    name_label_->setToolTip(QStringLiteral("Recovered function under the graph cursor"));
    name_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    address_label_ = new QLabel(header_);
    address_label_->setFont(theme::fonts::codeRegular());
    address_label_->setToolTip(QStringLiteral("Entry address of the recovered function"));
    page_label_ = new QLabel(header_);
    page_label_->setToolTip(QStringLiteral(
        "Visible page blocks, visible edges, and total recovered blocks"));
    page_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    const auto header_button = [this](const QString& id, const QString& label,
                                      const QString& tooltip) {
        auto* button = new QToolButton(header_);
        button->setObjectName(QStringLiteral("aida.graph.workspace.header.") + id);
        button->setText(label);
        button->setToolTip(tooltip);
        button->setAutoRaise(true);
        return button;
    };
    disassembly_button_ = header_button(QStringLiteral("disassembly"),
        QStringLiteral("Disassembly"),
        QStringLiteral("Open the selection in Disassembly (Space)"));
    pseudocode_button_ = header_button(QStringLiteral("pseudocode"),
        QStringLiteral("Pseudocode"),
        QStringLiteral("Decompile the function in Pseudocode (F5)"));
    fit_button_ = header_button(QStringLiteral("fit"), QStringLiteral("Fit"),
        QStringLiteral("Fit all graph blocks in the canvas (F)"));
    zoom_out_button_ = header_button(QStringLiteral("zoom_out"), QStringLiteral("-"),
        QStringLiteral("Zoom out (-)"));
    zoom_reset_button_ = header_button(QStringLiteral("zoom_reset"),
        QStringLiteral("100%"), QStringLiteral("Reset zoom to 100% (Home)"));
    zoom_in_button_ = header_button(QStringLiteral("zoom_in"), QStringLiteral("+"),
        QStringLiteral("Zoom in (+)"));
    prev_page_button_ = header_button(QStringLiteral("prev_page"),
        QStringLiteral("Previous"), QStringLiteral("Previous 256-block page"));
    next_page_button_ = header_button(QStringLiteral("next_page"), QStringLiteral("Next"),
        QStringLiteral("Next 256-block page"));
    more_button_ = header_button(QStringLiteral("more"), QStringLiteral("More"),
        QStringLiteral("Graph actions that do not fit the header"));
    more_button_->setPopupMode(QToolButton::InstantPopup);
    more_menu_ = new QMenu(more_button_);
    more_menu_->setToolTipsVisible(true);
    more_button_->setMenu(more_menu_);
    more_button_->setVisible(false);
    header_layout->addWidget(name_label_);
    header_layout->addWidget(address_label_);
    header_layout->addWidget(page_label_);
    header_layout->addStretch(1);
    header_layout->addWidget(disassembly_button_);
    header_layout->addWidget(pseudocode_button_);
    header_layout->addWidget(fit_button_);
    header_layout->addWidget(zoom_out_button_);
    header_layout->addWidget(zoom_reset_button_);
    header_layout->addWidget(zoom_in_button_);
    header_layout->addWidget(prev_page_button_);
    header_layout->addWidget(next_page_button_);
    header_layout->addWidget(more_button_);
    overflow_buttons_ = {disassembly_button_, pseudocode_button_, fit_button_,
        zoom_out_button_, zoom_reset_button_, zoom_in_button_, prev_page_button_,
        next_page_button_};
    header_->adjustSize();
    header_->show();

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.graph.workspace.state_view"));
    state_view_->hide();

    minimap_ = new CfgMinimap(this);
    minimap_->setTrackedView(this);
    minimap_->setScene(controller_->scene());
    minimap_->setFixedSize(k_minimap_logical_size);
    minimap_->show();

    poller_ = new analysis_bridge::AidaRevisionPoller(this);
    poller_->set_source([this] { return sampleRevisions(); });
    connect(poller_, &analysis_bridge::AidaRevisionPoller::revisionChanged, this,
        &AidaWorkspaceGraphView::onRevisionChanged);
    connect(poller_, &analysis_bridge::AidaRevisionPoller::sourceInvalidated, this, [this] {
        recaptureContext();
        rebuildIfNeeded();
        syncHeader();
        syncEmptyState();
    });
    connect(controller_, &CfgSceneController::fitRequested, this,
        &AidaWorkspaceGraphView::onFitRequested);
    connect(controller_, &CfgSceneController::zoomStepRequested, this,
        &AidaWorkspaceGraphView::onZoomStep);
    connect(controller_, &CfgSceneController::resetRequested, this,
        &AidaWorkspaceGraphView::onReset);
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
        syncHeader();
    });
    connect(disassembly_button_, &QToolButton::clicked, this, [this] {
        const auto state = controller_->workspaceState();
        const auto address = state && state->selected_address != 0
            ? state->selected_address : 0;
        const auto target = address != 0 ? address
            : (function_ ? disasm_view::runtime_address(context_, function_->start)
                .value_or(function_->start.value) : 0);
        if (target != 0) {
            disasm_view::goto_address(target, context_);
            const auto hook = aida::qt::analysis_bridge::view_focus_hook();
            if (hook)
                hook("document.disassembly");
        }
    });
    connect(pseudocode_button_, &QToolButton::clicked, this, [this] {
        const auto state = controller_->workspaceState();
        const auto address = state && state->selected_address != 0
            ? state->selected_address : 0;
        const auto target = address != 0 ? address
            : (function_ ? disasm_view::runtime_address(context_, function_->start)
                .value_or(function_->start.value) : 0);
        if (target != 0) {
            const auto function_start = disasm_view::enclosing_function_start(target,
                context_);
            if (function_start != 0) {
                pseudocode_view::request_decompile(context_, function_start, false);
                const auto hook = aida::qt::analysis_bridge::view_focus_hook();
                if (hook)
                    hook("document.pseudocode");
            }
        }
    });
    connect(fit_button_, &QToolButton::clicked, this, [this] { onFitRequested(); });
    connect(zoom_out_button_, &QToolButton::clicked, this, [this] { onZoomStep(0.85); });
    connect(zoom_in_button_, &QToolButton::clicked, this, [this] { onZoomStep(1.18); });
    connect(zoom_reset_button_, &QToolButton::clicked, this, [this] { onReset(); });
    connect(prev_page_button_, &QToolButton::clicked, this, [this] {
        const auto state = controller_->workspaceState();
        if (state && state->block_page != 0) {
            --state->block_page;
            state->layout_signature = 0;
            applied_signature_ = 0;
            rebuildIfNeeded();
        }
    });
    connect(next_page_button_, &QToolButton::clicked, this, [this] {
        const auto state = controller_->workspaceState();
        if (state && state->block_page + 1 < page_count_) {
            ++state->block_page;
            state->layout_signature = 0;
            applied_signature_ = 0;
            rebuildIfNeeded();
        }
    });

    recaptureContext();
    rebuildIfNeeded();
    syncHeader();
    syncEmptyState();
}

AidaWorkspaceGraphView::~AidaWorkspaceGraphView()
{
    poller_->set_polling(false);
}

void AidaWorkspaceGraphView::recaptureContext()
{
    const auto previous_workspace = context_.workspace;
    context_ = disasm_view::capture_selected_workspace();
    controller_->setWorkspaceContext(context_);
    if (context_.workspace != previous_workspace)
        applied_signature_ = 0;
}

analysis_bridge::revision_sample_t AidaWorkspaceGraphView::sampleRevisions()
{
    analysis_bridge::revision_sample_t sample;
    const auto workspace = aida::analysis::workspace_registry().selected_for_ui();
    if (!workspace || workspace->closed())
        return sample;
    if (context_.workspace != workspace)
        recaptureContext();
    if (!context_.workspace)
        return sample;
    sample.workspace = context_.workspace.get();
    sample.generation = context_.workspace->generation();
    sample.analysis_revision = context_.workspace->analysis_revision();
    sample.overlay_revision = context_.workspace->overlay_revision();
    sample.view_revision = context_.workspace->view_state().revision;
    sample.valid = true;
    return sample;
}

void AidaWorkspaceGraphView::onRevisionChanged(quint64, quint64)
{
    recaptureContext();
    rebuildIfNeeded();
    if (controller_->workspaceState())
        controller_->mirrorWorkspaceSelection();
    syncHeader();
    syncEmptyState();
}

const aida::analysis::function_record_t* AidaWorkspaceGraphView::currentFunction() const
{
    if (!context_)
        return nullptr;
    return cfg_view::workspace_graph_function(context_);
}

void AidaWorkspaceGraphView::rebuildIfNeeded()
{
    function_ = currentFunction();
    const auto state = controller_->workspaceState();
    if (!state || !function_ || !context_.publication || !context_.publication->snapshot) {
        page_count_ = 0;
        total_blocks_ = 0;
        updateHeaderOverflow();
        return;
    }
    const auto& snapshot = *context_.publication->snapshot;
    const std::size_t first = function_->first_block;
    const std::size_t available = first <= snapshot.blocks.size()
        ? snapshot.blocks.size() - first : 0;
    total_blocks_ = (std::min)(static_cast<std::size_t>(function_->block_count), available);
    constexpr std::size_t blocks_per_page = 256;
    page_count_ = total_blocks_ == 0 ? 1 : (total_blocks_ + blocks_per_page - 1) / blocks_per_page;
    if (state->block_page >= page_count_)
        state->block_page = page_count_ - 1;
    page_begin_ = state->block_page * blocks_per_page;
    page_end_ = (std::min)(total_blocks_, page_begin_ + blocks_per_page);
    const auto signature = cfg_view::workspace_graph_layout_signature(context_, *function_,
        state->block_page);
    if (applied_signature_ != signature || state->layout_signature != signature) {
        state->layout_signature = signature;
        applied_signature_ = signature;
        controller_->rebuildWorkspaceLayout(*function_, page_begin_, page_end_);
        applyFit();
    }
    updateHeaderOverflow();
}

void AidaWorkspaceGraphView::syncHeader()
{
    const auto state = controller_->workspaceState();
    if (!function_ || !state) {
        full_name_.clear();
        name_label_->setText(QStringLiteral("Recovered function"));
        name_label_->setToolTip(QStringLiteral(
            "Recovered function under the graph cursor"));
        address_label_->clear();
        page_label_->clear();
        prev_page_button_->setVisible(false);
        next_page_button_->setVisible(false);
        page_label_->setVisible(false);
        if (header_compact_)
            rebuildHeaderOverflowMenu();
        return;
    }
    const auto function_address = disasm_view::runtime_address(context_, function_->start)
        .value_or(function_->start.value);
    full_name_ = QString::fromStdString(disasm_view::resolve_name(context_,
        function_->start));
    applyNameElide();
    address_label_->setText(QStringLiteral("%1")
        .arg(QString::number(function_address, 16).toUpper().rightJustified(16, u'0')));
    const std::size_t slice = page_end_ - page_begin_;
    QString page_text;
    if (total_blocks_ == 0) {
        page_text = QStringLiteral("No blocks in this function");
    } else {
        page_text = QStringLiteral("Blocks %1-%2 of %3  ·  %4 edges%5  ·  Page %6/%7")
            .arg(page_begin_ + 1)
            .arg(page_end_)
            .arg(total_blocks_)
            .arg(state->edges.size())
            .arg(state->edge_set_truncated ? QStringLiteral(" (bounded)") : QString())
            .arg(state->block_page + 1)
            .arg(page_count_);
        if (state->layout.nodes.size() != slice) {
            page_text += QStringLiteral("  ·  %1 visible after collapse")
                .arg(state->layout.nodes.size());
        }
    }
    page_label_->setText(page_text);
    const bool paging = page_count_ > 1;
    if (!header_compact_) {
        prev_page_button_->setVisible(paging);
        next_page_button_->setVisible(paging);
    }
    prev_page_button_->setEnabled(paging && state->block_page > 0);
    next_page_button_->setEnabled(paging && state->block_page + 1 < page_count_);
    page_label_->setVisible(true);
    if (header_compact_)
        rebuildHeaderOverflowMenu();
}

void AidaWorkspaceGraphView::applyNameElide()
{
    const QFontMetricsF metrics(name_label_->font());
    if (full_name_.isEmpty()) {
        name_label_->setText(QStringLiteral("Recovered function"));
        name_label_->setToolTip(QStringLiteral(
            "Recovered function under the graph cursor"));
        return;
    }
    const int available = name_label_->contentsRect().width();
    const int cap = available > 0 ? available
        : static_cast<int>(metrics.horizontalAdvance(
            QStringLiteral("MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM")));
    name_label_->setText(metrics.elidedText(full_name_, Qt::ElideMiddle, cap));
    name_label_->setToolTip(full_name_);
}

void AidaWorkspaceGraphView::updateHeaderOverflow()
{
    if (!header_)
        return;
    auto* header_layout = header_->layout();
    const auto margins = header_layout->contentsMargins();
    int required = margins.left() + margins.right() + header_layout->spacing();
    const int label_reserve = static_cast<int>(theme::tokens().shell.min_panel_w);
    required += label_reserve + header_layout->spacing() * 2;
    for (const auto* button : overflow_buttons_) {
        if (button == prev_page_button_ || button == next_page_button_) {
            if (page_count_ <= 1)
                continue;
        }
        required += button->sizeHint().width() + header_layout->spacing();
    }
    if (header_compact_)
        required += more_button_->sizeHint().width() + header_layout->spacing();
    const bool want_compact = header_->width() < required;
    if (want_compact != header_compact_)
        setHeaderCompact(want_compact);
}

void AidaWorkspaceGraphView::setHeaderCompact(bool compact)
{
    header_compact_ = compact;
    for (auto* button : overflow_buttons_)
        button->setVisible(!compact);
    more_button_->setVisible(compact);
    if (compact) {
        rebuildHeaderOverflowMenu();
    } else {
        syncHeader();
    }
}

void AidaWorkspaceGraphView::rebuildHeaderOverflowMenu()
{
    if (!more_menu_)
        return;
    more_menu_->clear();
    const auto state = controller_->workspaceState();
    for (auto* button : overflow_buttons_) {
        const bool paging_button = button == prev_page_button_ ||
            button == next_page_button_;
        if (paging_button && page_count_ <= 1)
            continue;
        if (paging_button && !state)
            continue;
        QAction* action = more_menu_->addAction(button->text());
        action->setObjectName(button->objectName() + QStringLiteral(".overflow"));
        action->setEnabled(button->isEnabled());
        const QString tooltip = button->toolTip();
        if (!tooltip.isEmpty())
            action->setToolTip(tooltip);
        connect(action, &QAction::triggered, this, [button] {
            if (button->isEnabled())
                button->click();
        });
    }
}

void AidaWorkspaceGraphView::syncEmptyState()
{
    const bool no_publication = !context_ || !context_.publication ||
        !context_.publication->snapshot;
    const auto state = controller_->workspaceState();
    bool empty = true;
    const auto progress = context_.workspace
        ? context_.workspace->progress() : aida::analysis::workspace_progress_t{};
    if (no_publication &&
        progress.readiness == aida::analysis::workspace_readiness_t::failed) {
        state_view_->setState(widgets::AidaStateView::State::Error);
        state_view_->setTitle(QStringLiteral("Graph analysis failed"));
        state_view_->setMessage(progress.error && !progress.error->message.empty()
            ? QString::fromStdString(progress.error->message)
            : QStringLiteral(
                "Function recovery failed before publishing a control-flow snapshot."));
        state_view_->setActionLabel(QString());
    } else if (no_publication &&
        (progress.readiness == aida::analysis::workspace_readiness_t::analyzing ||
         progress.readiness == aida::analysis::workspace_readiness_t::partial ||
         progress.readiness == aida::analysis::workspace_readiness_t::cancelling)) {
        state_view_->setState(widgets::AidaStateView::State::Loading);
        state_view_->setTitle(QStringLiteral("Recovering graph data"));
        state_view_->setMessage(QStringLiteral(
            "Control-flow blocks will appear as soon as analysis publishes them."));
        state_view_->setActionLabel(QString());
    } else if (no_publication) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("Graph is not ready"));
        state_view_->setMessage(QStringLiteral(
            "Open a binary and wait for function recovery to publish a control-flow snapshot."));
        state_view_->setActionLabel(QString());
    } else if (!function_) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No recovered function"));
        state_view_->setMessage(QStringLiteral(
            "Select an instruction inside a recovered function, then return to Graph."));
        state_view_->setActionLabel(QString());
    } else if (total_blocks_ == 0 || (state && state->layout.nodes.empty())) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No control-flow blocks"));
        state_view_->setMessage(QStringLiteral(
            "Function recovery published this function without a displayable block graph."));
        state_view_->setActionLabel(QString());
    } else {
        empty = false;
    }
    state_view_->setVisible(empty);
    if (minimap_)
        minimap_->setVisible(!empty);
    if (empty)
        state_view_->raise();
}

void AidaWorkspaceGraphView::onFitRequested()
{
    applyFit();
}

void AidaWorkspaceGraphView::applyFit()
{
    const QRectF bounds = controller_->worldBounds();
    if (bounds.isNull() || bounds.width() < 1.0 || bounds.height() < 1.0)
        return;
    const qreal pad = static_cast<qreal>(theme::tokens().spacing.section) * 3.0;
    const qreal zoom_x = (viewport()->width() - pad) / (std::max)(1.0, bounds.width());
    const qreal zoom_y = (viewport()->height() - pad) / (std::max)(1.0, bounds.height());
    const qreal zoom = (std::clamp)((std::min)(zoom_x, zoom_y), k_zoom_min_workspace,
        1.35);
    resetTransform();
    scale(zoom, zoom);
    current_zoom_ = zoom;
    target_zoom_ = zoom;
    target_center_ = bounds.center();
    clampTargetCenter();
    centerOn(target_center_);
    updateZoomLabel();
    if (minimap_)
        minimap_->viewport()->update();
}

void AidaWorkspaceGraphView::onZoomStep(qreal factor)
{
    applyZoomClamped((std::clamp)(current_zoom_ * factor, k_zoom_min_workspace,
        k_zoom_max_workspace));
}

void AidaWorkspaceGraphView::onReset()
{
    resetTransform();
    current_zoom_ = 1.0;
    target_zoom_ = 1.0;
    target_center_ = controller_->worldBounds().center();
    clampTargetCenter();
    centerOn(target_center_);
    updateZoomLabel();
}

void AidaWorkspaceGraphView::applyZoomClamped(qreal target)
{
    target = (std::clamp)(target, k_zoom_min_workspace, k_zoom_max_workspace);
    target_center_ = mapToScene(viewport()->rect()).boundingRect().center();
    clampTargetCenter();
    const qreal factor = target / (std::max)(current_zoom_, 0.0001);
    setTransformationAnchor(QGraphicsView::AnchorViewCenter);
    scale(factor, factor);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    current_zoom_ = transform().m11();
    target_zoom_ = current_zoom_;
    centerOn(target_center_);
    updateZoomLabel();
    if (minimap_)
        minimap_->viewport()->update();
}

void AidaWorkspaceGraphView::clampTargetCenter()
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

void AidaWorkspaceGraphView::updateZoomLabel()
{
    if (zoom_reset_button_)
        zoom_reset_button_->setText(QStringLiteral("%1%").arg(
            static_cast<int>(current_zoom_ * 100.0)));
    if (header_compact_)
        rebuildHeaderOverflowMenu();
}

void AidaWorkspaceGraphView::updateTransform(bool animate)
{
    clampTargetCenter();
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
    if (minimap_)
        minimap_->viewport()->update();
}

void AidaWorkspaceGraphView::drawBackground(QPainter* painter, const QRectF& rect)
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
    dot.setAlphaF(dot.alphaF() * 0.42);
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

void AidaWorkspaceGraphView::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        const qreal old_zoom = target_zoom_;
        target_zoom_ = (std::clamp)(old_zoom * (delta > 0 ? 1.1 : 0.9),
            k_zoom_min_workspace, k_zoom_max_workspace);
        const qreal factor = target_zoom_ / old_zoom;
        scale(factor, factor);
        current_zoom_ = target_zoom_;
        target_center_ = mapToScene(viewport()->rect()).boundingRect().center();
        clampTargetCenter();
        updateZoomLabel();
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

void AidaWorkspaceGraphView::mousePressEvent(QMouseEvent* event)
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

void AidaWorkspaceGraphView::mouseMoveEvent(QMouseEvent* event)
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

void AidaWorkspaceGraphView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton && middle_dragging_) {
        middle_dragging_ = false;
        viewport()->unsetCursor();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void AidaWorkspaceGraphView::keyPressEvent(QKeyEvent* event)
{
    if (!event->modifiers() &&
        (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal)) {
        onZoomStep(1.18);
        event->accept();
        return;
    }
    if (!event->modifiers() && event->key() == Qt::Key_Minus) {
        onZoomStep(0.85);
        event->accept();
        return;
    }
    if (!event->modifiers() && event->key() == Qt::Key_Home) {
        onReset();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F && !event->modifiers()) {
        onFitRequested();
        event->accept();
        return;
    }
    if (!event->modifiers()) {
        const qreal pan_step = grid_step() / (std::max)(current_zoom_, 0.01);
        switch (event->key()) {
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
    if (event->key() == Qt::Key_Space && !event->modifiers()) {
        disassembly_button_->click();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F5 && !event->modifiers()) {
        pseudocode_button_->click();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        !event->modifiers()) {
        const auto state = controller_->workspaceState();
        if (state && state->selected_block) {
            const auto found = state->node_by_entity.find(*state->selected_block);
            if (found != state->node_by_entity.end() &&
                found->second < state->block_indices.size()) {
                if (auto* item = controller_->selectedItem()) {
                    const auto menu = controller_->buildWorkspaceMenu(item);
                    aida::ui::analysis_context_menu::execute_shortcut(menu,
                        "analysis.graph.navigate_target");
                }
            }
        }
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

void AidaWorkspaceGraphView::drawForeground(QPainter* painter, const QRectF& rect)
{
    QGraphicsView::drawForeground(painter, rect);
    if (!hasFocus())
        return;
    const auto& t = theme::tokens();
    painter->save();
    painter->setWorldTransform(QTransform());
    const qreal inset = static_cast<qreal>(t.control.focus_ring) * 0.5;
    painter->setPen(QPen(t.border_focus, static_cast<qreal>(t.control.focus_ring)));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(QRectF(viewport()->rect()).adjusted(inset, inset, -inset, -inset));
    painter->restore();
}

void AidaWorkspaceGraphView::focusInEvent(QFocusEvent* event)
{
    QGraphicsView::focusInEvent(event);
    viewport()->update();
}

void AidaWorkspaceGraphView::focusOutEvent(QFocusEvent* event)
{
    QGraphicsView::focusOutEvent(event);
    viewport()->update();
}

void AidaWorkspaceGraphView::contextMenuEvent(QContextMenuEvent* event)
{
    if (event->reason() == QContextMenuEvent::Keyboard) {
        if (controller_->selectedItem()) {
            aida::ui::analysis_context_menu::open(
                controller_->buildWorkspaceMenu(controller_->selectedItem()),
                aida::ui::context_menu_open_origin_t::menu_key, event->globalPos(), this);
        } else {
            aida::ui::analysis_context_menu::open(
                controller_->buildWorkspaceCanvasMenu(),
                aida::ui::context_menu_open_origin_t::menu_key, event->globalPos(), this);
        }
        event->accept();
        return;
    }
    if (event->reason() == QContextMenuEvent::Mouse) {
        const auto item = controller_->scene()->itemAt(
            mapToScene(event->pos()), transform());
        if (dynamic_cast<CfgBlockItem*>(item)) {
            QGraphicsView::contextMenuEvent(event);
            return;
        }
        aida::ui::analysis_context_menu::open(
            controller_->buildWorkspaceCanvasMenu(),
            aida::ui::context_menu_open_origin_t::pointer, event->globalPos(), this);
        event->accept();
        return;
    }
    QGraphicsView::contextMenuEvent(event);
}

void AidaWorkspaceGraphView::showEvent(QShowEvent* event)
{
    QGraphicsView::showEvent(event);
    poller_->set_polling(true);
    recaptureContext();
    rebuildIfNeeded();
    syncHeader();
    syncEmptyState();
}

void AidaWorkspaceGraphView::hideEvent(QHideEvent* event)
{
    QGraphicsView::hideEvent(event);
    poller_->set_polling(false);
}

void AidaWorkspaceGraphView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    const int header_height = header_->sizeHint().height();
    header_->setGeometry(0, 0, width(), header_height);
    setViewportMargins(0, header_height, 0, 0);
    state_view_->setGeometry(0, header_height, width(), height() - header_height);
    updateHeaderOverflow();
    applyNameElide();
    updateMinimapGeometry();
}

void AidaWorkspaceGraphView::updateMinimapGeometry()
{
    if (!minimap_)
        return;
    const auto& t = theme::tokens();
    minimap_->move(width() - minimap_->width() - t.spacing.md,
        height() - minimap_->height() - t.spacing.md);
    minimap_->raise();
    state_view_->raise();
    header_->raise();
}

}
