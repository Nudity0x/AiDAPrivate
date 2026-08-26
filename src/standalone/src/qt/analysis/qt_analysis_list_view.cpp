#include "qt/analysis/qt_analysis_list_view.hpp"

#include <QFontMetrics>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "helpers/diag_log.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/analysis/workspace/workspace_registry.hpp"
#include "core/infra/executor.hpp"
#include "core/ui/context_menu_contract.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_analysis_menu_builder.hpp"
#include "qt/analysis/qt_gui_post.hpp"
#include "qt/analysis/qt_revision_poller.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_search_field.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::analysis {

QtAnalysisListView::QtAnalysisListView(analysis_list_domain_t domain,
                                       QWidget* parent)
    : QWidget(parent), domain_(domain) {
    const auto& descriptor = analysis_list_descriptor(domain);
    setObjectName(QStringLiteral("aida.") +
        QString::fromLatin1(descriptor.stable_id));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.") +
        QString::fromLatin1(descriptor.stable_id) + QStringLiteral(".toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    const auto& tokens = theme::tokens();
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x, tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.toolbar.group_gap);
    title_label_ = new QLabel(QString::fromLatin1(descriptor.title), toolbar);
    title_label_->setObjectName(QStringLiteral("aida.list.title"));
    toolbar_layout->addWidget(title_label_);
    refreshing_label_ = new QLabel(QStringLiteral("Refreshing"), toolbar);
    refreshing_label_->setObjectName(QStringLiteral("aida.list.refreshing"));
    refreshing_label_->setProperty("aidaVariant", QStringLiteral("info"));
    refreshing_label_->setVisible(false);
    toolbar_layout->addWidget(refreshing_label_);
    filter_debounce_ = new QTimer(this);
    filter_debounce_->setSingleShot(true);
    filter_debounce_->setInterval(300);
    filter_ = new widgets::AidaSearchField(
        QStringLiteral("Filter by address, name, or detail..."), toolbar);
    filter_->setObjectName(QStringLiteral("aida.") +
        QString::fromLatin1(descriptor.stable_id) + QStringLiteral(".filter"));
    filter_->setClearButtonEnabled(true);
    filter_->setToolTip(QStringLiteral(
        "Filter the published rows by address, name, kind, or detail"));
    toolbar_layout->addWidget(filter_, 1);
    layout->addWidget(toolbar);

    model_ = new QtAnalysisListModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.") +
        QString::fromLatin1(descriptor.stable_id) + QStringLiteral(".table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(tokens.table.compact_row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* horizontal = table_->horizontalHeader();
    const QFontMetricsF code_metrics(theme::fonts::codeRegular());
    const int address_width = static_cast<int>(code_metrics.horizontalAdvance(
        QStringLiteral("0x0000000000000000"))) + 2 * tokens.table.cell_pad_x +
        tokens.spacing.xs;
    const QFontMetrics ui_metrics(table_->font());
    const int kind_width = (std::max)(
        ui_metrics.horizontalAdvance(QStringLiteral("Kind / Source")),
        ui_metrics.horizontalAdvance(QStringLiteral("Objective-C protocol"))) +
        2 * tokens.table.cell_pad_x + tokens.spacing.xs;
    horizontal->setSectionResizeMode(
        static_cast<int>(QtAnalysisListModel::Column::address), QHeaderView::Interactive);
    table_->setColumnWidth(static_cast<int>(QtAnalysisListModel::Column::address),
        address_width);
    horizontal->setSectionResizeMode(
        static_cast<int>(QtAnalysisListModel::Column::name), QHeaderView::Stretch);
    horizontal->setSectionResizeMode(
        static_cast<int>(QtAnalysisListModel::Column::kind), QHeaderView::Interactive);
    table_->setColumnWidth(static_cast<int>(QtAnalysisListModel::Column::kind),
        kind_width);
    horizontal->setSectionResizeMode(
        static_cast<int>(QtAnalysisListModel::Column::details), QHeaderView::Stretch);
    horizontal->setStretchLastSection(false);
    horizontal->setSectionsClickable(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.") +
        QString::fromLatin1(descriptor.stable_id) + QStringLiteral(".state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);
    layout->addWidget(table_, 1);

    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.list.status"));
    status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(status_label_);

    connect(filter_, &QLineEdit::textChanged, this, [this](const QString&) {
        filter_debounce_->start();
    });
    connect(filter_debounce_, &QTimer::timeout, this, [this] {
        if (!state_) return;
        const QString next = filter_->text();
        if (state_->filter == next) return;
        state_->filter = next;
        state_->filter_dirty = true;
        scheduleRebuild();
    });
    connect(horizontal, &QHeaderView::sectionClicked, this, [this](int logical) {
        if (!state_) return;
        const bool ascending =
            state_->sort_column == logical ? !state_->sort_ascending : true;
        state_->sort_column = logical;
        state_->sort_ascending = ascending;
        state_->sort_dirty = true;
        scheduleRebuild();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!state_ || !current.isValid()) return;
        const auto* row = model_->rowAt(current.row());
        if (!row) return;
        state_->selected_source = model_->sourceIndexForViewRow(current.row());
        state_->selected_address = row->has_address ? row->address : 0;
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        const auto context = disasm_view::capture_workspace(workspace);
        if (context && row->has_address)
            disasm_view::select_address(row->address, context);
    });
    connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid()) return;
        const auto* row = model_->rowAt(index.row());
        if (!row || !row->has_address) return;
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        QtAnalysisBridge::instance().navigateTo(workspace, row->address,
            "document.disassembly");
    });
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = table_->indexAt(pos);
        if (index.isValid())
            showRowMenu(table_->viewport()->mapToGlobal(pos), index.row());
    });
    table_->installEventFilter(this);

    connect(&QtAnalysisBridge::instance(), &QtAnalysisBridge::activeContextChanged,
            this, [this](QtWorkspaceContext* context) { rebindContext(context); });
    rebindContext(QtAnalysisBridge::instance().activeContext());
}

void QtAnalysisListView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (context_ && context_->poller())
        context_->poller()->arm();
    scheduleRebuild();
}

void QtAnalysisListView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
}

bool QtAnalysisListView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers().testFlag(Qt::ControlModifier) &&
            (key->key() == Qt::Key_C || key->key() == Qt::Key_Insert)) {
            copySelectedName();
            return true;
        }
        if (key->key() == Qt::Key_Menu ||
            (key->key() == Qt::Key_F10 &&
                key->modifiers().testFlag(Qt::ShiftModifier))) {
            const auto current = table_->currentIndex();
            if (current.isValid())
                showRowMenu(table_->viewport()->mapToGlobal(
                    table_->visualRect(current).center()), current.row());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QtAnalysisListView::rebindContext(QtWorkspaceContext* context) {
    if (context_ == context) return;
    if (poller_connection_) disconnect(poller_connection_);
    context_ = context;
    state_ = context ? &context->listState[static_cast<std::size_t>(domain_)] : nullptr;
    if (state_) {
        if (!state_->filter.isEmpty() && filter_->text() != state_->filter)
            filter_->setText(state_->filter);
        poller_connection_ = connect(context_->poller(),
            &QtRevisionPoller::revisionsChanged, this,
            [this](quint64, quint64, quint64, quint64) { scheduleRebuild(); });
        model_->setSnapshot(state_->snapshot);
        refreshPresentation();
        scheduleRebuild();
    } else {
        model_->setSnapshot(nullptr);
        refreshPresentation();
    }
}

void QtAnalysisListView::scheduleRebuild() {
    if (!state_ || !context_) return;
    const auto workspace = context_->workspace().lock();
    if (!workspace) {
        model_->setSnapshot(nullptr);
        refreshPresentation();
        return;
    }
    const auto context = disasm_view::capture_workspace(workspace);
    const auto publication = context.publication;
    if (!publication || !publication->snapshot) {
        state_->rebuilding.store(false, std::memory_order_release);
        refreshing_label_->setVisible(false);
        return;
    }
    auto& state = *state_;
    const std::string requested = state.filter.toLower().toStdString();
    if (state.projected && state.generation == publication->generation &&
        state.revision == publication->analysis_revision &&
        state.overlay_revision == publication->overlay_revision &&
        !state.filter_dirty && !state.sort_dirty &&
        requested == state.filter_lower &&
        state.sort_column == state.submitted_sort_column &&
        state.sort_ascending == state.submitted_sort_ascending)
        return;
    state.projected = true;
    state.generation = publication->generation;
    state.revision = publication->analysis_revision;
    state.overlay_revision = publication->overlay_revision;
    state.filter_lower = requested;
    state.filter_dirty = false;
    state.sort_dirty = false;
    state.submitted_sort_column = state.sort_column;
    state.submitted_sort_ascending = state.sort_ascending;
    const auto serial = state.rebuild_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    state.rebuilding.store(true, std::memory_order_release);
    refreshing_label_->setVisible(true);
    const auto display_base = disasm_view::display_base_override(context);
    const int sort_column = state.sort_column;
    const bool sort_ascending = state.sort_ascending;
    const auto domain = domain_;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "analysis";
    submission.label = "analysis.analysis_list_views.rebuild";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 2;
    submission.generation = publication->generation;
    submission.body = [this, domain, context, display_base, requested,
                       sort_column, sort_ascending, serial]() {
        auto next = std::make_shared<analysis_list_snapshot_t>();
        next->rows = analysis_list_project_rows(domain, context, display_base);
        next->visible = analysis_list_compute_visible(next->rows, requested,
            sort_column, sort_ascending);
        gui_post(this, [this, snapshot = std::move(next), serial]() mutable {
            adoptSnapshot(std::move(snapshot), serial);
        });
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        state.rebuilding.store(false, std::memory_order_release);
        state.projected = false;
        state.filter_dirty = true;
        state.sort_dirty = true;
        refreshing_label_->setVisible(false);
        diag::log_tagged_fmt("analysis_list",
            "rebuild_submit_failed serial=%llu domain=%u reason=%s",
            static_cast<unsigned long long>(serial),
            static_cast<unsigned>(domain),
            submitted.reject_reason.c_str());
        return;
    }
    diag::log_tagged_fmt("analysis_list",
        "rebuild_submit serial=%llu domain=%u filter=%s",
        static_cast<unsigned long long>(serial),
        static_cast<unsigned>(domain),
        requested.c_str());
}

void QtAnalysisListView::adoptSnapshot(
    std::shared_ptr<const analysis_list_snapshot_t> snapshot,
    std::uint64_t serial) {
    if (!state_) return;
    auto& state = *state_;
    if (state.rebuild_serial.load(std::memory_order_acquire) != serial) {
        diag::log_tagged_fmt("analysis_list",
            "rebuild_stale_drop serial=%llu current=%llu domain=%u",
            static_cast<unsigned long long>(serial),
            static_cast<unsigned long long>(
                state.rebuild_serial.load(std::memory_order_acquire)),
            static_cast<unsigned>(domain_));
        return;
    }
    state.rebuilding.store(false, std::memory_order_release);
    refreshing_label_->setVisible(false);
    state.snapshot = std::move(snapshot);
    const std::size_t remapped = remapSelection(*state.snapshot);
    state.selected_source = remapped;
    state.selected_address = remapped != static_cast<std::size_t>(-1) &&
            state.snapshot->rows[remapped].has_address
        ? state.snapshot->rows[remapped].address : 0;
    state.adopted_snapshot = state.snapshot;
    model_->setSnapshot(state.snapshot);
    refreshPresentation();
    if (remapped != static_cast<std::size_t>(-1)) {
        const auto& visible = state.snapshot->visible;
        const auto position = std::find(visible.begin(), visible.end(), remapped);
        if (position != visible.end()) {
            const auto view_row = static_cast<int>(std::distance(visible.begin(), position));
            table_->setCurrentIndex(model_->index(view_row, 0));
        }
    }
}

std::size_t QtAnalysisListView::remapSelection(
    const analysis_list_snapshot_t& snapshot) const {
    if (!state_) return static_cast<std::size_t>(-1);
    const auto& state = *state_;
    if (state.selected_source == static_cast<std::size_t>(-1))
        return static_cast<std::size_t>(-1);
    const bool identity_stable = state.adopted_snapshot &&
        state.selected_source < state.adopted_snapshot->rows.size() &&
        state.selected_source < snapshot.rows.size() &&
        analysis_list_row_identity(
            state.adopted_snapshot->rows[state.selected_source]) ==
            analysis_list_row_identity(snapshot.rows[state.selected_source]);
    if (identity_stable)
        return state.selected_source;
    if (state.selected_address != 0) {
        for (std::size_t index = 0; index < snapshot.rows.size(); ++index) {
            if (snapshot.rows[index].has_address &&
                snapshot.rows[index].address == state.selected_address)
                return index;
        }
    }
    return static_cast<std::size_t>(-1);
}

void QtAnalysisListView::refreshPresentation() {
    const auto& descriptor = analysis_list_descriptor(domain_);
    const auto* snapshot = model_->snapshot();
    const std::size_t rows = snapshot ? snapshot->rows.size() : 0;
    const std::size_t visible = snapshot ? snapshot->visible.size() : 0;
    status_label_->setText(visible == rows
        ? QStringLiteral("%1 rows").arg(visible)
        : QStringLiteral("%1 of %2 rows").arg(visible).arg(rows));
    if (!context_ || context_->workspace().expired()) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No analysis workspace"));
        state_view_->setMessage(
            QStringLiteral("Open and analyze a binary to populate this view."));
        state_view_->setActionLabel(QString());
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context.publication || !context.publication->snapshot) {
        state_view_->setState(widgets::AidaStateView::State::Loading);
        state_view_->setTitle(QStringLiteral("Waiting for analysis"));
        state_view_->setMessage(QStringLiteral(
            "The analysis engine has not published this domain yet; the list fills in automatically."));
        state_view_->setActionLabel(QString());
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (rows == 0 && state_ && state_->rebuilding.load(std::memory_order_acquire)) {
        state_view_->setState(widgets::AidaStateView::State::Loading);
        state_view_->setTitle(QStringLiteral("Building list"));
        state_view_->setMessage(QStringLiteral(
            "Projecting the published analysis into this view."));
        state_view_->setActionLabel(QString());
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (rows == 0) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QString::fromLatin1(descriptor.empty_title));
        state_view_->setMessage(QString::fromLatin1(descriptor.empty_body));
        state_view_->setActionLabel(QString());
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (visible == 0) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No matches"));
        state_view_->setMessage(
            QStringLiteral("No published item matches the current filter."));
        state_view_->setActionLabel(QString());
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    state_view_->setVisible(false);
    table_->setVisible(true);
}

void QtAnalysisListView::showRowMenu(const QPoint& global_pos, int view_row) {
    if (!state_ || !context_) return;
    const auto* row = model_->rowAt(view_row);
    if (!row) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context) return;
    const auto source = model_->sourceIndexForViewRow(view_row);
    state_->selected_source = source;
    state_->context_source = source;
    state_->selected_address = row->has_address ? row->address : 0;
    if (row->has_address)
        disasm_view::select_address(row->address, context, false);
    const auto snapshot = model_->snapshot();
    const std::string identity = analysis_list_row_identity(*row);
    auto menu = qt_analysis_menus::build_analysis_row_menu(context,
        row->address, row->has_address, row->name, row->context, row->detail,
        [state = state_, snapshot, expected = source, identity] {
            return snapshot && state->selected_source < snapshot->rows.size() &&
                state->selected_source == expected &&
                analysis_list_row_identity(snapshot->rows[state->selected_source]) ==
                    identity
                ? aida::ui::capability_state_t::available()
                : aida::ui::capability_state_t::unavailable(
                    "The selected analysis-list entity changed");
        });
    QtAnalysisBridge::instance().showRetainedMenu(menu,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

void QtAnalysisListView::copySelectedName() {
    if (!state_) return;
    const auto* snapshot = model_->snapshot();
    if (!snapshot) return;
    const auto index = table_->currentIndex();
    if (!index.isValid()) return;
    const auto* row = model_->rowAt(index.row());
    if (!row) return;
    clipboard::set_text(QString::fromStdString(row->name));
}

}
