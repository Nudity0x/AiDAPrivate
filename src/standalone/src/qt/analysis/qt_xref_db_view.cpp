#include "qt/analysis/qt_xref_db_view.hpp"

#include <QComboBox>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "helpers/diag_log.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/analysis/workspace/publication_indexes.hpp"
#include "core/analysis/workspace/workspace_registry.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/infra/taskflow_runtime.hpp"
#include "core/ui/context_menu_contract.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_analysis_menu_builder.hpp"
#include "qt/analysis/qt_gui_post.hpp"
#include "qt/analysis/qt_revision_poller.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_search_field.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::analysis {

QtXrefDbView::QtXrefDbView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.analysis.references"));
    const auto& tokens = aida::qt::theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.xrefs.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.toolbar.group_gap);
    const QFontMetricsF ui_metrics(font());
    address_edit_ = new widgets::AidaLineEdit(QStringLiteral("Address"), toolbar);
    address_edit_->setObjectName(QStringLiteral("aida.xrefs.address"));
    address_edit_->setMinimumWidth(static_cast<int>(ui_metrics.horizontalAdvance(
        QStringLiteral("0xDDDDDDDDDDDDDDDD"))) + 2 * tokens.table.cell_pad_x +
        tokens.spacing.lg);
    address_edit_->setToolTip(QStringLiteral(
        "Address whose cross-references are indexed, in hexadecimal"));
    toolbar_layout->addWidget(address_edit_);
    mode_combo_ = new QComboBox(toolbar);
    mode_combo_->setObjectName(QStringLiteral("aida.xrefs.mode"));
    mode_combo_->setToolTip(QStringLiteral(
        "Choose references to or from the indexed address"));
    mode_combo_->addItems({QStringLiteral("XRefs To"), QStringLiteral("XRefs From")});
    toolbar_layout->addWidget(mode_combo_);
    auto* index_button = new QPushButton(QStringLiteral("Index"), toolbar);
    index_button->setObjectName(QStringLiteral("aida.xrefs.index"));
    index_button->setToolTip(QStringLiteral(
        "Materialize the cross-reference list for the address"));
    toolbar_layout->addWidget(index_button);
    filter_debounce_ = new QTimer(this);
    filter_debounce_->setSingleShot(true);
    filter_debounce_->setInterval(300);
    filter_edit_ = new widgets::AidaSearchField(
        QStringLiteral("Filter symbols or addresses"), toolbar);
    filter_edit_->setObjectName(QStringLiteral("aida.xrefs.filter"));
    filter_edit_->setMinimumWidth(static_cast<int>(ui_metrics.averageCharWidth() *
        24.0) + 2 * tokens.table.cell_pad_x + tokens.spacing.lg);
    filter_edit_->setClearButtonEnabled(true);
    toolbar_layout->addWidget(filter_edit_);
    toolbar_layout->addStretch(1);
    layout->addWidget(toolbar);

    model_ = new QtXrefModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.xrefs.table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(tokens.table.compact_row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* horizontal = table_->horizontalHeader();
    const QFontMetricsF code_metrics(aida::qt::theme::fonts::codeRegular());
    horizontal->setSectionResizeMode(static_cast<int>(QtXrefModel::Column::target),
        QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(QtXrefModel::Column::target),
        static_cast<int>(code_metrics.horizontalAdvance(
            QStringLiteral("0xDDDDDDDDDDDDDDDD"))) + 2 * tokens.table.cell_pad_x +
            tokens.spacing.xs);
    horizontal->setSectionResizeMode(static_cast<int>(QtXrefModel::Column::kind),
        QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(QtXrefModel::Column::kind),
        (std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Kind"))),
            static_cast<int>(ui_metrics.horizontalAdvance(
                QStringLiteral("relocation")))) + 2 * tokens.table.cell_pad_x +
            tokens.spacing.xs);
    horizontal->setSectionResizeMode(static_cast<int>(QtXrefModel::Column::name),
        QHeaderView::Stretch);
    horizontal->setStretchLastSection(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.xrefs.state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);
    layout->addWidget(table_, 1);

    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.xrefs.status"));
    status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(status_label_);

    connect(address_edit_, &QLineEdit::returnPressed, this, [this] { submitQuery(); });
    connect(index_button, &QPushButton::clicked, this, [this] { submitQuery(); });
    connect(mode_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!state_) return;
        state_->query_to = index == 0;
        submitQuery();
    });
    connect(filter_edit_, &QLineEdit::textChanged, this, [this](const QString&) {
        filter_debounce_->start();
    });
    connect(filter_debounce_, &QTimer::timeout, this, [this] {
        if (!state_) return;
        state_->filter = filter_edit_->text();
        requestFilter();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!state_ || !current.isValid()) return;
        const auto* row = model_->rowAt(current.row());
        if (!row) return;
        state_->selected_runtime = row->runtime;
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        const auto context = disasm_view::capture_workspace(workspace);
        if (context)
            disasm_view::select_address(row->runtime, context);
    });
    connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid()) return;
        const auto* row = model_->rowAt(index.row());
        if (!row) return;
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        QtAnalysisBridge::instance().navigateTo(workspace, row->runtime,
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

void QtXrefDbView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (context_ && context_->poller())
        context_->poller()->arm();
}

void QtXrefDbView::rebindContext(QtWorkspaceContext* context) {
    if (context_ == context) return;
    if (poller_connection_) disconnect(poller_connection_);
    context_ = context;
    state_ = context ? context->xrefState : nullptr;
    if (state_) {
        poller_connection_ = connect(context_->poller(),
            &QtRevisionPoller::revisionsChanged, this,
            [this](quint64, quint64, quint64, quint64) {
                // Revision change invalidates the published results view; the
                // user re-submits or the address field re-triggers.
                requestFilter();
                refreshPresentation();
            });
        model_->setResults(state_->visible_results);
        refreshPresentation();
    } else {
        model_->setResults(nullptr);
        refreshPresentation();
    }
}

void QtXrefDbView::submitQuery() {
    if (!state_ || !context_) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context) return;
    const auto value = qt_xref_parse_address(address_edit_->text().toStdString());
    if (!value) return;
    const auto address = disasm_view::typed_address(context, *value);
    if (!address) return;
    state_->address = address_edit_->text();
    state_->query_to = mode_combo_->currentIndex() == 0;
    auto state = state_;
    const bool query_to = state->query_to;
    if (!context.publication || !context.publication->snapshot ||
        state->searching.exchange(true, std::memory_order_acq_rel))
        return;
    std::uint64_t serial = 0;
    auto cancellation = std::make_shared<aida::analysis::cancellation_source_t>();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->cancellation)
            state->cancellation->request_cancel();
        state->cancellation = cancellation;
        state->results.reset();
        state->visible_results.reset();
        ++state->results_version;
        state->visible_version = 0;
        state->error.clear();
        serial = state->serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    const std::string target_id = context.workspace->identity().binary_id().to_hex();
    aida::infra::taskflow_runtime::task_descriptor_t descriptor;
    descriptor.domain = aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
    descriptor.owner_subsystem = "xref_db_view";
    descriptor.label = "workspace_xref_page";
    descriptor.target_id = target_id.c_str();
    descriptor.generation = context.publication->generation;
    descriptor.cancellable_body = [this, context, state, address = *address, query_to,
        cancellation, serial](
        const aida::infra::taskflow_runtime::cancellation_token_t& runtime_cancel) {
        std::vector<qt_xref_result_t> results;
        constexpr std::size_t maximum_results = 100000;
        results.reserve((std::min)(context.publication->snapshot->xrefs.size(),
            static_cast<std::size_t>(1024)));
        const auto indexes = aida::analysis::publication_indexes::for_publication(
            context.publication, cancellation->token());
        if (indexes) {
            const auto& xrefs = context.publication->snapshot->xrefs;
            const auto range = query_to ? indexes->xrefs_to(address)
                : indexes->xrefs_from(address);
            for (std::uint32_t ordinal = range.begin; ordinal < range.end; ++ordinal) {
                if (runtime_cancel.requested.load(std::memory_order_acquire) ||
                    cancellation->token().stop_requested())
                    break;
                const auto& xref = xrefs[query_to ? indexes->xref_to_entry(ordinal)
                    : indexes->xref_from_entry(ordinal)];
                if ((query_to && xref.target == address) ||
                    (!query_to && xref.source == address))
                    results.push_back({xref.source, xref.target, xref.kind});
                if (results.size() >= maximum_results)
                    break;
            }
        } else {
            diag::log_tagged_fmt("xref_db_view",
                "index_unavailable address=%llx query_to=%d",
                static_cast<unsigned long long>(address.value), query_to ? 1 : 0);
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->serial.load(std::memory_order_acquire) == serial) {
                state->results = std::make_shared<const std::vector<qt_xref_result_t>>(
                    std::move(results));
                state->visible_results.reset();
                ++state->results_version;
                state->visible_version = 0;
                if (!indexes)
                    state->error = "index unavailable";
            }
        }
        state->searching.store(false, std::memory_order_release);
        gui_post(this, [this, state, serial] {
            if (state->serial.load(std::memory_order_acquire) != serial) return;
            requestFilter();
        });
    };
    const auto submitted = aida::infra::taskflow_runtime::submit(std::move(descriptor));
    if (!submitted.submitted) {
        state->searching.store(false, std::memory_order_release);
        state->error = submitted.reject_reason;
        refreshPresentation();
    }
}

void QtXrefDbView::requestFilter() {
    if (!state_ || !context_) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context || !context.publication) return;
    auto state = state_;
    std::shared_ptr<const std::vector<qt_xref_result_t>> results;
    std::string filter;
    std::uint64_t version = 0;
    bool query_to = true;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        results = state->results;
        filter = state->filter.toStdString();
        version = state->results_version;
        query_to = state->query_to;
        if (!results || (state->visible_results && state->visible_version == version &&
            state->visible_filter == filter))
            return;
    }
    if (state->filtering.exchange(true, std::memory_order_acq_rel))
        return;
    const std::string target_id = context.workspace->identity().binary_id().to_hex();
    aida::infra::taskflow_runtime::task_descriptor_t descriptor;
    descriptor.domain = aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
    descriptor.owner_subsystem = "xref_db_view";
    descriptor.label = "workspace_xref_filter";
    descriptor.target_id = target_id.c_str();
    descriptor.generation = context.publication->generation;
    descriptor.cancellable_body = [this, context, state, results,
        filter = std::move(filter), version, query_to](
        const aida::infra::taskflow_runtime::cancellation_token_t& cancel) {
        std::vector<qt_xref_display_result_t> visible;
        visible.reserve((std::min)(results->size(), static_cast<std::size_t>(4096)));
        for (const auto& result : *results) {
            if (cancel.requested.load(std::memory_order_acquire) ||
                context.workspace->cancellation_token().stop_requested())
                break;
            const auto display = query_to ? result.source : result.target;
            const auto runtime = disasm_view::runtime_address(context, display).value_or(
                display.value);
            const std::string name = disasm_view::resolve_name(context, display);
            char address[32]{};
            std::snprintf(address, sizeof(address), "%016llX",
                static_cast<unsigned long long>(runtime));
            if (!filter.empty() && name.find(filter) == std::string::npos &&
                std::string(address).find(filter) == std::string::npos)
                continue;
            char label[256]{};
            std::snprintf(label, sizeof(label), "%016llX  %-10s  %s",
                static_cast<unsigned long long>(runtime), qt_xref_kind_name(result.kind),
                name.c_str());
            visible.push_back({result, runtime, name, label});
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->results_version == version &&
                state->filter.toStdString() == filter) {
                state->visible_results =
                    std::make_shared<const std::vector<qt_xref_display_result_t>>(
                        std::move(visible));
                state->visible_version = version;
                state->visible_filter = filter;
            }
        }
        state->filtering.store(false, std::memory_order_release);
        gui_post(this, [this, state, version] {
            if (state->visible_version != version) return;
            adoptResults();
        });
    };
    const auto submitted = aida::infra::taskflow_runtime::submit(std::move(descriptor));
    if (!submitted.submitted) {
        state->filtering.store(false, std::memory_order_release);
        state->error = submitted.reject_reason;
        refreshPresentation();
    }
}

void QtXrefDbView::adoptResults() {
    if (!state_) return;
    std::shared_ptr<const std::vector<qt_xref_display_result_t>> results;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        results = state_->visible_results;
    }
    model_->setResults(std::move(results));
    refreshPresentation();
}

void QtXrefDbView::refreshPresentation() {
    if (!state_ || !context_ || context_->workspace().expired()) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No analysis workspace"));
        state_view_->setMessage(QStringLiteral(
            "No analysis workspace is selected."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        status_label_->clear();
        return;
    }
    const auto workspace = context_->workspace().lock();
    if (workspace && workspace->target_kind() ==
            aida::analysis::target_kind_t::live_snapshot) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral(
            "Bulk xref index unavailable for live targets"));
        state_view_->setMessage(QStringLiteral(
            "Live targets support bounded on-demand disassembly and decompilation, not a whole-process xref index."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    const bool searching = state_->searching.load(std::memory_order_acquire);
    const bool filtering = state_->filtering.load(std::memory_order_acquire);
    bool has_results = false;
    bool has_visible = false;
    QString status;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        has_results = static_cast<bool>(state_->results);
        has_visible = state_->visible_results && !state_->visible_results->empty();
        if (!state_->error.empty())
            status = QString::fromStdString(state_->error);
    }
    if (status.isEmpty()) {
        if (searching)
            status = QStringLiteral("Searching the immutable xref publication...");
        else if (filtering)
            status = QStringLiteral("Filtering xref results...");
    }
    status_label_->setText(status);
    if (!has_results && (searching || filtering)) {
        state_view_->setState(widgets::AidaStateView::State::Loading);
        state_view_->setTitle(QStringLiteral("Indexing cross-references"));
        state_view_->setMessage(QStringLiteral(
            "The cancellable xref walk publishes rows here as soon as it completes."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (!has_results) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No cross-references indexed"));
        state_view_->setMessage(QStringLiteral(
            "Enter an address, choose To or From, and press Index to materialize the cross-reference list."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (!has_visible && !(searching || filtering)) {
        const bool filtered = !state_->filter.trimmed().isEmpty();
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(filtered ? QStringLiteral("No matches")
            : QStringLiteral("No cross-references"));
        state_view_->setMessage(filtered
            ? QStringLiteral("No cross-reference matches the current filter.")
            : QStringLiteral(
                "The indexed address has no cross-references in this publication."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    state_view_->setVisible(false);
    table_->setVisible(true);
}

bool QtXrefDbView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
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

void QtXrefDbView::showRowMenu(const QPoint& global_pos, int view_row) {
    if (!state_ || !context_) return;
    const auto* row = model_->rowAt(view_row);
    if (!row) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context) return;
    state_->selected_runtime = row->runtime;
    disasm_view::select_address(row->runtime, context, false);
    auto menu = qt_analysis_menus::build_xref_menu(context, state_, row->runtime,
        row->name, row->label);
    QtAnalysisBridge::instance().showRetainedMenu(menu,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

bool QtXrefDbView::queryAddress(std::uint64_t runtime_address, bool query_to) {
    if (!state_) return false;
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(runtime_address));
    address_edit_->setText(QString::fromLatin1(buf));
    mode_combo_->setCurrentIndex(query_to ? 0 : 1);
    submitQuery();
    return true;
}

}
