#include "qt/debugger/debugger_pane_base.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QTableView>
#include <QVBoxLayout>

#include "core/runtime/standalone_driver.hpp"

#include "qt/debugger/debugger_action_bridge.hpp"
#include "qt/debugger/debugger_models.hpp"
#include "qt/debugger/debugger_selection_bridge.hpp"
#include "qt/debugger/debugger_session_controller.hpp"
#include "qt/debugger/dialogs/spawn_target_dialog_qt.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::debugger {

DebuggerPaneBase::DebuggerPaneBase(QWidget* parent)
    : QWidget(parent) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);

    toolbar_host_ = new QWidget(this);
    toolbar_layout_ = new QHBoxLayout(toolbar_host_);
    toolbar_layout_->setContentsMargins(0, 0, 0, 0);
    toolbar_host_->setVisible(false);
    layout_->addWidget(toolbar_host_);

    empty_view_ = new widgets::AidaStateView(this);
    empty_view_->setState(widgets::AidaStateView::State::Empty);
    layout_->addWidget(empty_view_);

    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral("Attach or launch a target to begin a live debugging session."));
    connect(empty_view_, &widgets::AidaStateView::actionTriggered, this, [] {
        SpawnTargetDialogQt::requestOpen(QApplication::activeWindow());
    });

    connect(&DebuggerSessionController::instance(),
        &DebuggerSessionController::sessionTick, this,
        &DebuggerPaneBase::onSessionTick);
    connect(&DebuggerSessionController::instance(),
        &DebuggerSessionController::sessionStateChanged, this,
        &DebuggerPaneBase::onSessionStateChanged);
}

DebuggerPaneBase::~DebuggerPaneBase() = default;

void DebuggerPaneBase::setToolBar(QWidget* toolbar) {
    if (!toolbar)
        return;
    toolbar_host_->setVisible(true);
    toolbar_layout_->addWidget(toolbar);
}

void DebuggerPaneBase::setContent(QWidget* content) {
    if (content_)
        layout_->removeWidget(content_);
    content_ = content;
    if (content_) {
        layout_->insertWidget(1, content_, 1);
        content_->setVisible(true);
    }
    updateOverlayState();
}

void DebuggerPaneBase::setEmptyTargetText(const QString& title,
                                          const QString& body) {
    empty_title_ = title;
    empty_body_ = body;
    if (overlay_mode_ == OverlayMode::no_target) {
        empty_view_->setTitle(empty_title_);
        empty_view_->setMessage(empty_body_);
    }
    updateOverlayState();
}

void DebuggerPaneBase::setEmptyContentText(const QString& title,
                                           const QString& body) {
    empty_content_title_ = title;
    empty_content_body_ = body;
    if (overlay_mode_ == OverlayMode::no_rows) {
        empty_view_->setTitle(empty_content_title_);
        empty_view_->setMessage(empty_content_body_);
    }
    updateOverlayState();
}

void DebuggerPaneBase::setLoadingText(const QString& title,
                                      const QString& body) {
    loading_title_ = title;
    loading_body_ = body;
    if (overlay_mode_ == OverlayMode::loading) {
        empty_view_->setTitle(loading_title_);
        empty_view_->setMessage(loading_body_);
    }
    updateOverlayState();
}

void DebuggerPaneBase::setErrorText(const QString& title,
                                     const QString& body) {
    error_title_ = title;
    error_body_ = body;
    if (overlay_mode_ == OverlayMode::error) {
        empty_view_->setTitle(error_title_);
        empty_view_->setMessage(error_body_);
    }
    updateOverlayState();
}

void DebuggerPaneBase::setSessionDriven(bool driven) {
    session_driven_ = driven;
}

void DebuggerPaneBase::setOwnerViewId(const char* id) {
    owner_view_id_ = (id && id[0]) ? id : "view.debug.cpu";
}

void DebuggerPaneBase::refreshPane() {
    updateOverlayState();
}

bool DebuggerPaneBase::hasTargetContent() const {
    return driver_bridge::attached_pid() != 0;
}

bool DebuggerPaneBase::hasContentRows() const {
    return true;
}

bool DebuggerPaneBase::isContentLoading() const {
    return false;
}

bool DebuggerPaneBase::contentError(QString* detail) const {
    if (detail)
        detail->clear();
    return false;
}

void DebuggerPaneBase::onSessionStateChanged(int, quint32, quint64) {
    updateOverlayState();
}

void DebuggerPaneBase::onSessionTick() {
    updateOverlayState();
}

void DebuggerPaneBase::updateOverlayState() {
    if (!content_)
        return;
    const QString state_id = objectName() + QStringLiteral(".state");
    if (empty_view_->objectName() != state_id)
        empty_view_->setObjectName(state_id);
    const bool has_target = hasTargetContent();
    const bool has_rows = has_target && hasContentRows();
    QString error_detail;
    const bool failed = has_target && !has_rows && contentError(&error_detail);
    const bool loading = has_target && !failed && !has_rows &&
        isContentLoading();
    const OverlayMode mode = !has_target ? OverlayMode::no_target
        : failed ? OverlayMode::error
        : loading ? OverlayMode::loading
        : !has_rows ? OverlayMode::no_rows
        : OverlayMode::none;
    if (mode != overlay_mode_) {
        overlay_mode_ = mode;
        if (mode == OverlayMode::no_target) {
            empty_view_->setState(widgets::AidaStateView::State::Empty);
            empty_view_->setTitle(empty_title_);
            empty_view_->setMessage(empty_body_);
        } else if (mode == OverlayMode::no_rows) {
            empty_view_->setState(widgets::AidaStateView::State::Empty);
            empty_view_->setTitle(empty_content_title_);
            empty_view_->setMessage(empty_content_body_);
        } else if (mode == OverlayMode::loading) {
            empty_view_->setState(widgets::AidaStateView::State::Loading);
            empty_view_->setTitle(loading_title_);
            empty_view_->setMessage(loading_body_);
        } else if (mode == OverlayMode::error) {
            empty_view_->setState(widgets::AidaStateView::State::Error);
            empty_view_->setTitle(error_title_);
            empty_view_->setMessage(error_detail.isEmpty()
                ? error_body_ : error_detail);
        }
    } else if (mode == OverlayMode::error) {
        const QString message = error_detail.isEmpty()
            ? error_body_ : error_detail;
        if (empty_view_->message() != message)
            empty_view_->setMessage(message);
    }
    const QString want_action = mode == OverlayMode::no_target &&
            driver_bridge::attached_pid() == 0
        ? QStringLiteral("Launch Target...") : QString();
    if (empty_view_->actionLabel() != want_action)
        empty_view_->setActionLabel(want_action);
    empty_view_->setVisible(mode != OverlayMode::none);
    content_->setVisible(mode == OverlayMode::none);
}

void DebuggerPaneBase::showEvent(QShowEvent* event) {
    paneVisible_ = true;
    updateOverlayState();
    onShown();
    QWidget::showEvent(event);
}

void DebuggerPaneBase::hideEvent(QHideEvent* event) {
    paneVisible_ = false;
    onHidden();
    QWidget::hideEvent(event);
}

void DebuggerPaneBase::contextMenuEvent(QContextMenuEvent* event) {
    openContextMenuAt(event->globalPos());
}

void DebuggerPaneBase::keyPressEvent(QKeyEvent* event) {
    const bool menu_key = event->key() == Qt::Key_Menu;
    const bool shift_f10 = event->key() == Qt::Key_F10 &&
        event->modifiers().testFlag(Qt::ShiftModifier);
    if (menu_key || shift_f10) {
        bool shown = false;
        for (const auto& wired : context_tables_) {
            QTableView* view = wired.view;
            DebuggerTableModelBase* model = wired.model;
            if (!view || !model || !view->isVisible())
                continue;
            if (!view->hasFocus() && !view->viewport()->hasFocus())
                continue;
            const QModelIndex current = view->currentIndex();
            if (!current.isValid())
                continue;
            selection_bridge::publish_rows(*model, view->selectionModel(),
                current);
            DebuggerActionBridge::instance().showEntityMenu(
                model->contextForRow(current.row()),
                menu_key
                    ? aida::ui::context_menu_open_origin_t::menu_key
                    : aida::ui::context_menu_open_origin_t::shift_f10,
                view->viewport()->mapToGlobal(
                    view->visualRect(current).center()),
                view);
            shown = true;
            break;
        }
        if (!shown)
            openContextMenuAt(mapToGlobal(rect().center()));
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void DebuggerPaneBase::openContextMenuAt(const QPoint& globalPos) {
    (void)globalPos;
}

void DebuggerPaneBase::registerContextMenuTable(
    QTableView* view, DebuggerTableModelBase* model) {
    if (!view || !model)
        return;
    for (const auto& wired : context_tables_)
        if (wired.view == view)
            return;
    context_tables_.push_back({view, model});
}

void DebuggerPaneBase::wireTable(QTableView* view,
                                 DebuggerTableModelBase* model,
                                 bool extendedSelection) {
    if (!view || !model)
        return;
    registerContextMenuTable(view, model);
    view->verticalHeader()->setVisible(false);
    view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    view->verticalHeader()->setDefaultSectionSize(
        theme::tokens().table.row_h);
    view->setShowGrid(false);
    view->setAlternatingRowColors(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(extendedSelection
        ? QAbstractItemView::ExtendedSelection
        : QAbstractItemView::SingleSelection);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    view->setModel(model);
    connect(model, &DebuggerTableModelBase::snapshotApplied, this,
        [this](quint64) { updateOverlayState(); });
    for (int section = 0;
         section < static_cast<int>(model->columns().size()); ++section) {
        const auto& column = model->columns()[static_cast<std::size_t>(section)];
        if (column.stretch)
            view->horizontalHeader()->setSectionResizeMode(section,
                QHeaderView::Stretch);
        else if (column.width > 0) {
            view->horizontalHeader()->setSectionResizeMode(section,
                QHeaderView::Interactive);
            view->setColumnWidth(section, column.width);
        }
    }

    connect(view->selectionModel(), &QItemSelectionModel::currentChanged,
        view, [view, model](const QModelIndex& current, const QModelIndex&) {
            selection_bridge::publish_rows(*model, view->selectionModel(),
                current);
        });
    connect(view, &QTableView::customContextMenuRequested, view,
        [this, view, model](const QPoint& pos) {
            const QModelIndex index = view->indexAt(pos);
            if (!index.isValid())
                return;
            selection_bridge::publish_rows(*model, view->selectionModel(),
                index);
            DebuggerActionBridge::instance().showEntityMenu(
                model->contextForRow(index.row()),
                aida::ui::context_menu_open_origin_t::pointer,
                view->viewport()->mapToGlobal(pos), view);
        });
}

}
