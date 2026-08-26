#include "qt/analysis/qt_types_catalog_view.hpp"

#include <QFontMetricsF>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>
#include <cstring>

#include "helpers/diag_log.hpp"

#include "core/analysis/struct_dissector.hpp"
#include "core/analysis/symbol_store.hpp"
#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/disasm/pseudocode_view.hpp"
#include "core/infra/taskflow_runtime.hpp"
#include "core/session/analysis_session.hpp"
#include "core/ai/entity_evidence_handoff.hpp"
#include "core/workbench/workbench_shell_integration.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_gui_post.hpp"
#include "qt/analysis/qt_revision_poller.hpp"
#include "qt/analysis/qt_types_catalog_menu.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/analysis/qt_xref_model.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_search_field.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::analysis {

namespace {

class QtStringListTableModel : public QAbstractTableModel {
public:
    explicit QtStringListTableModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setRows(QStringList rows) {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }

    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : 1;
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() ||
            (role != Qt::DisplayRole && role != Qt::ToolTipRole) ||
            index.row() < 0 || index.row() >= rows_.size())
            return {};
        return rows_.at(index.row());
    }
    void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override {
        for (QModelRoleData& roleData : span) {
            if (roleData.role() == Qt::DisplayRole ||
                roleData.role() == Qt::ToolTipRole)
                roleData.setData(data(index, roleData.role()));
            else
                roleData.clearData();
        }
    }

private:
    QStringList rows_;
};

class QtTypeReferencesModel : public QAbstractTableModel {
public:
    explicit QtTypeReferencesModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setReferences(
        std::shared_ptr<const std::vector<qt_type_reference_t>> references) {
        beginResetModel();
        references_ = std::move(references);
        endResetModel();
    }
    const qt_type_reference_t* rowAt(int row) const noexcept {
        if (!references_ || row < 0 ||
            static_cast<std::size_t>(row) >= references_->size())
            return nullptr;
        return &(*references_)[static_cast<std::size_t>(row)];
    }
    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() || !references_
            ? 0 : static_cast<int>(references_->size());
    }
    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : 1;
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() ||
            (role != Qt::DisplayRole && role != Qt::ToolTipRole)) return {};
        const auto* reference = rowAt(index.row());
        return reference ? QString::fromStdString(reference->label) : QVariant{};
    }
    void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override {
        for (QModelRoleData& roleData : span) {
            if (roleData.role() == Qt::DisplayRole ||
                roleData.role() == Qt::ToolTipRole)
                roleData.setData(data(index, roleData.role()));
            else
                roleData.clearData();
        }
    }

private:
    std::shared_ptr<const std::vector<qt_type_reference_t>> references_;
};

bool catalog_context_is_current(const disasm_view::workspace_context_t& context,
                                const QtTypesHubState& state) {
    return context.publication && context.workspace &&
        !context.workspace->closing() && !context.workspace->closed() &&
        state.context_generation == context.publication->generation &&
        state.context_analysis_revision == context.publication->analysis_revision &&
        state.catalog_generation == state.context_generation &&
        state.catalog_analysis_revision == state.context_analysis_revision;
}

}

QtTypesCatalogView::QtTypesCatalogView(qt_types_tab_t tab, QWidget* parent)
    : QWidget(parent), tab_(tab) {
    static const char* k_view_ids[] = {
        "view.types.structures", "view.types.unions", "view.types.enums",
        "view.types.typedefs", "view.types.functions", "view.types.inferred",
        "view.types.dissector"
    };
    setObjectName(QStringLiteral("aida.") +
        QString::fromLatin1(k_view_ids[static_cast<int>(tab)]));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    const auto& tokens = aida::qt::theme::tokens();

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.") +
        QString::fromLatin1(k_view_ids[static_cast<int>(tab)]) +
        QStringLiteral(".toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.toolbar.group_gap);
    filter_debounce_ = new QTimer(this);
    filter_debounce_->setSingleShot(true);
    filter_debounce_->setInterval(300);
    search_ = new widgets::AidaSearchField(
        QStringLiteral("Filter names and canonical types"), toolbar);
    search_->setObjectName(QStringLiteral("aida.types_catalog.search"));
    search_->setClearButtonEnabled(true);
    toolbar_layout->addWidget(search_, 1);
    load_pdb_ = new QPushButton(QStringLiteral("Load PDB..."), toolbar);
    load_pdb_->setObjectName(QStringLiteral("aida.types_catalog.load_pdb"));
    load_pdb_->setToolTip(QStringLiteral(
        "Load debug symbols from a local PDB file"));
    toolbar_layout->addWidget(load_pdb_);
    pdb_progress_ = new QProgressBar(toolbar);
    pdb_progress_->setObjectName(QStringLiteral("aida.types_catalog.pdb_progress"));
    pdb_progress_->setMinimumWidth(static_cast<int>(
        QFontMetricsF(font()).horizontalAdvance(
            QStringLiteral("Loading PDB 100%"))) + 4 * tokens.spacing.xxl);
    pdb_progress_->setFormat(QStringLiteral("Loading PDB"));
    pdb_progress_->setVisible(false);
    toolbar_layout->addWidget(pdb_progress_);
    pdb_cancel_ = new QPushButton(QStringLiteral("Cancel PDB"), toolbar);
    pdb_cancel_->setObjectName(QStringLiteral("aida.types_catalog.cancel_pdb"));
    pdb_cancel_->setToolTip(QStringLiteral("Cancel the active PDB download"));
    pdb_cancel_->setVisible(false);
    toolbar_layout->addWidget(pdb_cancel_);
    layout->addWidget(toolbar);

    workspace_label_ = new QLabel(this);
    workspace_label_->setObjectName(QStringLiteral("aida.types_catalog.workspace"));
    workspace_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(workspace_label_);
    error_label_ = new QLabel(this);
    error_label_->setObjectName(QStringLiteral("aida.types_catalog.error"));
    error_label_->setProperty("aidaVariant", QStringLiteral("error"));
    error_label_->setWordWrap(true);
    error_label_->setVisible(false);
    layout->addWidget(error_label_);

    splitter_ = new QSplitter(this);
    splitter_->setObjectName(QStringLiteral("aida.types_catalog.splitter"));
    model_ = new QtTypesCatalogModel(this);
    model_->setTab(tab);
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("aida.types_catalog.table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(tokens.table.compact_row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setModel(model_);
    splitter_->addWidget(table_);

    detail_host_ = new QWidget(this);
    detail_host_->setObjectName(QStringLiteral("aida.types_catalog.detail"));
    auto* detail_layout = new QVBoxLayout(detail_host_);
    detail_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    detail_title_ = new QLabel(detail_host_);
    detail_title_->setObjectName(QStringLiteral("aida.types_catalog.detail_title"));
    detail_title_->setWordWrap(true);
    detail_subtitle_ = new QLabel(detail_host_);
    detail_subtitle_->setObjectName(
        QStringLiteral("aida.types_catalog.detail_subtitle"));
    detail_subtitle_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detail_subtitle_->setWordWrap(true);
    detail_layout->addWidget(detail_title_);
    detail_layout->addWidget(detail_subtitle_);
    detail_members_model_ = new QtStringListTableModel(this);
    detail_members_ = new QTableView(detail_host_);
    detail_members_->setObjectName(QStringLiteral("aida.types_catalog.members"));
    detail_members_->verticalHeader()->setVisible(false);
    detail_members_->verticalHeader()->setDefaultSectionSize(tokens.table.compact_row_h);
    detail_members_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    detail_members_->horizontalHeader()->setVisible(false);
    detail_members_->horizontalHeader()->setStretchLastSection(true);
    detail_members_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    detail_members_->setSelectionMode(QAbstractItemView::NoSelection);
    detail_members_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    detail_members_->setShowGrid(false);
    detail_members_->setAlternatingRowColors(true);
    detail_members_->setModel(detail_members_model_);
    detail_layout->addWidget(detail_members_, 1);
    references_model_ = new QtTypeReferencesModel(this);
    references_table_ = new QTableView(detail_host_);
    references_table_->setObjectName(QStringLiteral("aida.types_catalog.references"));
    references_table_->verticalHeader()->setVisible(false);
    references_table_->verticalHeader()->setDefaultSectionSize(tokens.table.compact_row_h);
    references_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    references_table_->horizontalHeader()->setVisible(false);
    references_table_->horizontalHeader()->setStretchLastSection(true);
    references_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    references_table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    references_table_->setShowGrid(false);
    references_table_->setAlternatingRowColors(true);
    references_table_->setModel(references_model_);
    references_table_->setVisible(false);
    detail_layout->addWidget(references_table_, 1);
    splitter_->addWidget(detail_host_);
    splitter_->setStretchFactor(0, 2);
    splitter_->setStretchFactor(1, 3);
    layout->addWidget(splitter_, 1);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.types_catalog.state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);

    auto* apply_strip = new QWidget(this);
    apply_strip->setObjectName(QStringLiteral("aida.types_catalog.apply_strip"));
    auto* apply_layout = new QHBoxLayout(apply_strip);
    apply_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    apply_layout->setSpacing(tokens.toolbar.group_gap);
    apply_layout->addWidget(new QLabel(QStringLiteral("Apply type through reversible overlay"),
        apply_strip));
    apply_address_ = new widgets::AidaLineEdit(QStringLiteral("Address"), apply_strip);
    apply_address_->setObjectName(QStringLiteral("aida.types_catalog.apply_address"));
    apply_address_->setMinimumWidth(static_cast<int>(QFontMetricsF(font())
            .horizontalAdvance(QStringLiteral("0xDDDDDDDDDDDDDDDD"))) +
        2 * tokens.table.cell_pad_x + tokens.spacing.lg);
    apply_address_->setToolTip(QStringLiteral(
        "Address receiving the canonical type overlay, in hexadecimal"));
    apply_layout->addWidget(apply_address_);
    apply_type_ = new widgets::AidaLineEdit(QStringLiteral("Canonical type"), apply_strip);
    apply_type_->setObjectName(QStringLiteral("aida.types_catalog.apply_type"));
    apply_layout->addWidget(apply_type_, 1);
    auto* apply_button = new QPushButton(QStringLiteral("Apply"), apply_strip);
    apply_button->setObjectName(QStringLiteral("aida.types_catalog.apply"));
    apply_button->setToolTip(QStringLiteral(
        "Apply the canonical type at the address through the reversible overlay"));
    apply_layout->addWidget(apply_button);
    layout->addWidget(apply_strip);
    apply_status_ = new QLabel(this);
    apply_status_->setObjectName(QStringLiteral("aida.types_catalog.apply_status"));
    apply_status_->setWordWrap(true);
    apply_status_->setVisible(false);
    layout->addWidget(apply_status_);

    apply_watch_ = new QTimer(this);
    apply_watch_->setInterval(100);
    connect(apply_watch_, &QTimer::timeout, this, [this] {
        if (!state_ || !state_->apply_pending) {
            apply_watch_->stop();
            return;
        }
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        const auto context = disasm_view::capture_workspace(workspace);
        const auto mutation = disasm_view::mutation_state(context);
        if (!workspace || workspace->generation() != state_->apply_generation) {
            state_->apply_pending = false;
            state_->apply_error = true;
            state_->apply_status =
                "The analysis generation changed before the type application committed.";
        } else if (mutation.overlay_revision > state_->apply_expected_overlay_revision) {
            state_->apply_pending = false;
            state_->apply_error = false;
            state_->apply_status =
                "Committed to the reversible type overlay and published to analysis views.";
        } else if (mutation.pending == 0 && !mutation.error.empty()) {
            state_->apply_pending = false;
            state_->apply_error = true;
            state_->apply_status = mutation.error;
        } else {
            return;
        }
        const char* apply_variant = state_->apply_error ? "error" : "success";
        if (apply_status_->property("aidaVariant").toString() !=
                QLatin1String(apply_variant)) {
            apply_status_->setProperty("aidaVariant",
                QString::fromLatin1(apply_variant));
            theme::stylesheet::repolish(apply_status_);
        }
        apply_status_->setText(QString::fromStdString(state_->apply_status));
        apply_status_->setVisible(true);
        apply_watch_->stop();
    });

    connect(search_, &QLineEdit::textChanged, this, [this](const QString&) {
        filter_debounce_->start();
    });
    connect(filter_debounce_, &QTimer::timeout, this, [this] {
        if (!state_) return;
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->search = search_->text();
        requestVisible();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid()) return;
        showDetailFor(current.row());
        publishSelection(current.row());
    });
    connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid() || !context_) return;
        aida::analysis::address_t address{};
        if (tab_ == qt_types_tab_t::functions &&
            model_->addressAt(index.row(), address)) {
            const auto workspace = context_->workspace().lock();
            const auto context = disasm_view::capture_workspace(workspace);
            const auto runtime = disasm_view::runtime_address(context, address)
                .value_or(address.value);
            QtAnalysisBridge::instance().navigateTo(workspace, runtime,
                "document.disassembly");
        }
    });
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = table_->indexAt(pos);
        if (index.isValid())
            showRowMenu(table_->viewport()->mapToGlobal(pos), index.row());
    });
    table_->installEventFilter(this);
    connect(load_pdb_, &QPushButton::clicked, this, [this] {
        if (!state_ || !context_) return;
        const auto workspace = context_->workspace().lock();
        const auto context = disasm_view::capture_workspace(workspace);
        if (!context.workspace) return;
        const auto path = dialogs::open_file(this, QStringLiteral("Select PDB file"),
            "Program Database (*.pdb)\0*.pdb\0All files (*.*)\0*.*\0\0");
        if (!path || path->empty()) return;
        const auto accepted = analysis_session::approve_local_pdb(
            context.workspace, *path, true, true);
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->pdb_error = accepted ? std::string()
            : accepted.error().stable_code() + ": " + accepted.error().message;
    });
    connect(pdb_cancel_, &QPushButton::clicked, this, [this] {
        if (!state_ || !context_) return;
        const auto workspace = context_->workspace().lock();
        const auto cancelled = analysis_session::cancel_pdb(workspace);
        if (!cancelled) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->pdb_error = cancelled.error().stable_code() + ": " +
                cancelled.error().message;
        }
    });
    connect(apply_button, &QPushButton::clicked, this, [this] { applyType(); });
    connect(references_table_, &QTableView::activated, this,
            [this](const QModelIndex& index) {
        auto* model = static_cast<QtTypeReferencesModel*>(references_model_);
        const auto* reference = model ? model->rowAt(index.row()) : nullptr;
        if (!reference || !context_) return;
        const auto workspace = context_->workspace().lock();
        const auto context = disasm_view::capture_workspace(workspace);
        const auto runtime = disasm_view::runtime_address(context, reference->address)
            .value_or(reference->address.value);
        QtAnalysisBridge::instance().navigateTo(workspace, runtime,
            "document.disassembly");
    });

    connect(&QtAnalysisBridge::instance(), &QtAnalysisBridge::activeContextChanged,
            this, [this](QtWorkspaceContext* context) { rebindContext(context); });
    rebindContext(QtAnalysisBridge::instance().activeContext());
}

void QtTypesCatalogView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (context_ && context_->poller())
        context_->poller()->arm();
    requestCatalog();
}

void QtTypesCatalogView::rebindContext(QtWorkspaceContext* context) {
    if (context_ == context) return;
    if (poller_connection_) disconnect(poller_connection_);
    context_ = context;
    state_ = context ? context->typesHubState : nullptr;
    if (state_) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->search.isEmpty() && search_->text() != state_->search)
                search_->setText(state_->search);
            if (!state_->apply_address.isEmpty())
                apply_address_->setText(state_->apply_address);
            if (!state_->apply_type.isEmpty())
                apply_type_->setText(state_->apply_type);
        }
        poller_connection_ = connect(context_->poller(),
            &QtRevisionPoller::revisionsChanged, this,
            [this](quint64, quint64, quint64, quint64) { requestCatalog(); });
        requestCatalog();
    } else {
        model_->setContent(nullptr, nullptr);
        refreshPresentation();
    }
}

void QtTypesCatalogView::requestCatalog() {
    if (!state_ || !context_) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context.publication) return;
    auto state = state_;
    if (state->catalog_loading.exchange(true, std::memory_order_acq_rel))
        return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->catalog &&
            state->catalog_generation == context.publication->generation &&
            state->catalog_analysis_revision == context.publication->analysis_revision) {
            state->catalog_loading.store(false, std::memory_order_release);
            return;
        }
    }
    const std::string target_id = context.workspace->identity().binary_id().to_hex();
    aida::infra::taskflow_runtime::task_descriptor_t descriptor;
    descriptor.domain = aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
    descriptor.owner_subsystem = "types_hub";
    descriptor.label = "build_workspace_type_catalog";
    descriptor.target_id = target_id.c_str();
    descriptor.generation = context.publication->generation;
    descriptor.cancellable_body = [this, context, state](
        const aida::infra::taskflow_runtime::cancellation_token_t& cancel) {
        if (!cancel.requested.load(std::memory_order_acquire)) {
            auto catalog = std::make_shared<const qt_type_catalog_t>(
                qt_build_type_catalog(context));
            std::lock_guard<std::mutex> lock(state->mutex);
            if (context.workspace->generation() == context.publication->generation) {
                state->catalog = std::move(catalog);
                state->catalog_generation = context.publication->generation;
                state->catalog_analysis_revision =
                    context.publication->analysis_revision;
                state->visible_indices.reset();
                state->visible_catalog = nullptr;
                state->selected = -1;
            }
        }
        state->catalog_loading.store(false, std::memory_order_release);
        gui_post(this, [this, state] {
            if (!state->catalog) return;
            requestVisible();
            refreshPresentation();
        });
    };
    const auto submitted = aida::infra::taskflow_runtime::submit(std::move(descriptor));
    if (!submitted.submitted) {
        state->catalog_loading.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->pdb_error = submitted.reject_reason;
    }
}

void QtTypesCatalogView::requestVisible() {
    if (!state_ || !context_) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context.publication) return;
    auto state = state_;
    std::shared_ptr<const qt_type_catalog_t> catalog;
    std::string filter;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        catalog = state->catalog;
        filter = state->search.toStdString();
        if (!catalog) return;
        if (state->visible_indices && state->visible_catalog == catalog.get() &&
            state->visible_tab == tab_ && state->visible_filter == filter) {
            model_->setContent(catalog, state->visible_indices);
            refreshPresentation();
            return;
        }
    }
    if (state->visible_loading.exchange(true, std::memory_order_acq_rel))
        return;
    const auto tab = tab_;
    const std::string target_id = context.workspace->identity().binary_id().to_hex();
    aida::infra::taskflow_runtime::task_descriptor_t descriptor;
    descriptor.domain = aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
    descriptor.owner_subsystem = "types_hub";
    descriptor.label = "filter_workspace_type_catalog";
    descriptor.target_id = target_id.c_str();
    descriptor.generation = context.publication->generation;
    descriptor.cancellable_body = [this, context, state, catalog, tab,
        filter = std::move(filter)](
        const aida::infra::taskflow_runtime::cancellation_token_t& cancel) {
        std::vector<std::size_t> visible;
        const auto cancelled = [&]() {
            return cancel.requested.load(std::memory_order_acquire) ||
                context.workspace->cancellation_token().stop_requested();
        };
        if (!cancelled())
            visible = qt_filter_type_catalog(*catalog, tab, filter);
        bool adopted = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->catalog.get() == catalog.get() && state->active == tab) {
                state->visible_indices =
                    std::make_shared<const std::vector<std::size_t>>(std::move(visible));
                state->visible_catalog = catalog.get();
                state->visible_tab = tab;
                state->visible_filter = filter;
                state->selected = -1;
                adopted = true;
            }
        }
        state->visible_loading.store(false, std::memory_order_release);
        if (adopted) {
            gui_post(this, [this, state] {
                std::shared_ptr<const qt_type_catalog_t> current_catalog;
                std::shared_ptr<const std::vector<std::size_t>> current_visible;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    current_catalog = state->catalog;
                    current_visible = state->visible_indices;
                }
                model_->setContent(std::move(current_catalog),
                    std::move(current_visible));
                refreshPresentation();
            });
        }
    };
    const auto submitted = aida::infra::taskflow_runtime::submit(std::move(descriptor));
    if (!submitted.submitted) {
        state->visible_loading.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->pdb_error = submitted.reject_reason;
    }
}

void QtTypesCatalogView::refreshPresentation() {
    const auto workspace = context_ ? context_->workspace().lock() : nullptr;
    if (!workspace) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No analysis workspace"));
        state_view_->setMessage(QStringLiteral(
            "No analysis workspace is selected."));
        state_view_->setVisible(true);
        splitter_->setVisible(false);
        return;
    }
    state_view_->setVisible(false);
    splitter_->setVisible(true);
    std::string pdb_error;
    bool loading = false;
    bool visible_loading = false;
    std::shared_ptr<const qt_type_catalog_t> catalog;
    if (state_) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        pdb_error = state_->pdb_error;
        catalog = state_->catalog;
        loading = state_->catalog_loading.load(std::memory_order_acquire);
        visible_loading = state_->visible_loading.load(std::memory_order_acquire);
    }
    error_label_->setVisible(!pdb_error.empty());
    error_label_->setText(QString::fromStdString(pdb_error));
    const auto prompt = analysis_session::pdb_prompt_snapshot(workspace);
    const bool pdb_loading = prompt && prompt.value().loading;
    pdb_progress_->setVisible(pdb_loading);
    pdb_cancel_->setVisible(pdb_loading);
    if (pdb_loading) {
        const auto& value = prompt.value();
        const float progress = value.bytes_total != 0
            ? static_cast<float>((std::min)(1.0,
                static_cast<double>(value.bytes_received) /
                static_cast<double>(value.bytes_total)))
            : static_cast<float>((std::clamp)(value.progress_percent, 0, 100)) / 100.0f;
        pdb_progress_->setRange(0, 100);
        pdb_progress_->setValue(static_cast<int>(progress * 100.f));
        if (!value.status.empty())
            error_label_->setVisible(true);
        error_label_->setText(QString::fromStdString(value.status));
    } else if (prompt && prompt.value().failed) {
        error_label_->setVisible(true);
        error_label_->setText(QStringLiteral("PDB load failed: %1")
            .arg(QString::fromStdString(prompt.value().status)));
    }
    workspace_label_->setText(QStringLiteral(
        "Static analysis workspace  |  %1").arg(
        catalog ? QStringLiteral("generation %1  |  revision %2")
            .arg(state_->catalog_generation).arg(state_->catalog_analysis_revision)
            : QStringLiteral("catalog publication pending")));
    workspace_label_->setToolTip(workspace_label_->text());
    if (loading || visible_loading) {
        state_view_->setState(widgets::AidaStateView::State::Loading);
        state_view_->setTitle(loading
            ? QStringLiteral("Building type catalog...")
            : QStringLiteral("Filtering type catalog..."));
        state_view_->setVisible(true);
        splitter_->setVisible(false);
        return;
    }
    if (catalog) {
        std::string search;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            search = state_->search.toStdString();
        }
        if (model_->rowCount() == 0) {
            static const char* k_empty_titles[] = {
                "No structures in the catalog", "No unions in the catalog",
                "No enums in the catalog", "No typedefs in the catalog",
                "No functions in the catalog", "No inferred types in the catalog",
                "No dissector records in the catalog"
            };
            const bool filtered = !search.empty();
            state_view_->setState(widgets::AidaStateView::State::Empty);
            state_view_->setTitle(filtered
                ? QStringLiteral("No matches")
                : QString::fromLatin1(k_empty_titles[static_cast<int>(tab_)]));
            state_view_->setMessage(filtered
                ? QStringLiteral("No published type matches the current filter.")
                : QStringLiteral(
                    "The catalog published no entries for this tab. Load a PDB or wait for type recovery to publish candidates."));
            state_view_->setVisible(true);
            splitter_->setVisible(false);
            return;
        }
    }
}

bool QtTypesCatalogView::eventFilter(QObject* watched, QEvent* event) {
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

void QtTypesCatalogView::showDetailFor(int view_row) {
    if (!state_) return;
    std::shared_ptr<const qt_type_catalog_t> catalog;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        catalog = state_->catalog;
        state_->selected = view_row;
        state_->list_focused = true;
    }
    if (!catalog) return;
    auto* members = static_cast<QtStringListTableModel*>(detail_members_model_);
    const auto name = model_->nameAt(view_row);
    switch (tab_) {
    case qt_types_tab_t::structures:
    case qt_types_tab_t::unions_: {
        const auto& entries = tab_ == qt_types_tab_t::structures
            ? catalog->structs : catalog->unions;
        aida::analysis::address_t unused{};
        const auto visible_index = view_row;
        std::size_t source = static_cast<std::size_t>(visible_index);
        if (static_cast<std::size_t>(view_row) >= static_cast<std::size_t>(
                model_->rowCount())) break;
        const auto identity = model_->entityIdAt(view_row);
        const pdb_parser::struct_def_t* definition = qt_catalog_record(*catalog, name);
        if (!definition) break;
        detail_title_->setText(QStringLiteral("%1 %2")
            .arg(definition->is_union ? QStringLiteral("union") : QStringLiteral("struct"))
            .arg(QString::fromStdString(definition->name)));
        QString module_name;
        for (const auto& entry : entries) {
            if (&entry.definition == definition) module_name =
                QString::fromStdString(entry.module);
        }
        detail_subtitle_->setText(QStringLiteral("%1  size 0x%2  %3 members")
            .arg(module_name)
            .arg(definition->size, 0, 16)
            .arg(definition->members.size()));
        QStringList rows;
        rows.reserve(static_cast<int>(definition->members.size()));
        for (const auto& member : definition->members) {
            rows.push_back(QStringLiteral("+0x%1  %2  %3")
                .arg(member.offset, 4, 16, QLatin1Char('0'))
                .arg(QString::fromStdString(member.name))
                .arg(QString::fromStdString(member.type_name)));
        }
        if (members) members->setRows(std::move(rows));
        (void)unused;
        (void)source;
        (void)identity;
        break;
    }
    case qt_types_tab_t::enums: {
        const pdb_parser::enum_def_t* definition = qt_catalog_enum(*catalog, name);
        if (!definition) break;
        detail_title_->setText(QStringLiteral("enum %1")
            .arg(QString::fromStdString(definition->name)));
        detail_subtitle_->setText(QStringLiteral("%1 values")
            .arg(definition->members.size()));
        QStringList rows;
        rows.reserve(static_cast<int>(definition->members.size()));
        for (const auto& member : definition->members) {
            rows.push_back(QStringLiteral("%1  %2")
                .arg(QString::fromStdString(member.name))
                .arg(member.value));
        }
        if (members) members->setRows(std::move(rows));
        break;
    }
    case qt_types_tab_t::functions: {
        aida::analysis::address_t address{};
        if (!model_->addressAt(view_row, address)) break;
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        const auto context = disasm_view::capture_workspace(workspace);
        const auto runtime = disasm_view::runtime_address(context, address)
            .value_or(address.value);
        detail_title_->setText(QString::fromStdString(name));
        detail_subtitle_->setText(QStringLiteral("0x%1").arg(runtime, 16, 16,
            QLatin1Char('0')));
        if (members) members->setRows({});
        break;
    }
    default: {
        detail_title_->setText(QString::fromStdString(name));
        detail_subtitle_->setText(QString());
        if (members) members->setRows({});
        break;
    }
    }
}

void QtTypesCatalogView::publishSelection(int view_row) {
    if (!context_) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context.workspace) return;
    const auto name = model_->nameAt(view_row);
    aida::analysis::address_t address{};
    const bool has_address = model_->addressAt(view_row, address);
    aida::workbench::selection_context_t selection;
    selection.kind = aida::workbench::selection_kind_t::entity;
    selection.entity_key = "type." + std::to_string(static_cast<int>(tab_)) +
        "." + name;
    if (has_address) {
        selection.kind = aida::workbench::selection_kind_t::address;
        selection.has_address = true;
        selection.address = disasm_view::runtime_address(context, address)
            .value_or(address.value);
        selection.extent = 1;
    }
    aida::workbench::document_local_cursor_t cursor;
    if (has_address) {
        cursor.has_position = true;
        cursor.position = selection.address;
    }
    aida::workbench::workbench_shell_workspace_context_t workbench;
    static_cast<void>(aida::workbench::workbench_shell_runtime_t::instance()
        .publish_selection(context.workspace, selection, cursor,
            aida::workbench::navigation_origin_t::navigator, workbench));
    if (has_address)
        disasm_view::select_address(address, context, false);
}

void QtTypesCatalogView::applyType() {
    if (!state_ || !context_) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    state_->apply_error = true;
    const std::string canonical_type = apply_type_->text().toStdString();
    if (!context.workspace || context.workspace->closing() ||
        context.workspace->closed()) {
        state_->apply_status =
            "Workspace changed; reopen the target before applying a type.";
    } else if (canonical_type.empty()) {
        state_->apply_status = "Enter a canonical type before applying.";
    } else if (const auto address = qt_xref_parse_address(
            apply_address_->text().toStdString())) {
        if (const auto typed = disasm_view::typed_address(context, *address)) {
            if (disasm_view::queue_type_application(context, *typed, canonical_type)) {
                state_->apply_error = false;
                state_->apply_pending = true;
                state_->apply_generation = context.workspace->generation();
                state_->apply_expected_overlay_revision =
                    context.workspace->overlay_revision();
                state_->apply_status =
                    "Queued; waiting for the overlay revision to commit.";
                apply_watch_->start();
            } else {
                state_->apply_status =
                    "The overlay rejected the type application; no change was claimed.";
            }
        } else {
            state_->apply_status = "Address is outside the current workspace mapping.";
        }
    } else {
        state_->apply_status = "Address is not a valid hexadecimal or decimal value.";
    }
    apply_status_->setText(QString::fromStdString(state_->apply_status));
    apply_status_->setVisible(true);
}

bool QtTypesCatalogView::stageTypeApplication(std::uint64_t runtime_address,
                                              std::string& error) {
    if (!state_ || !context_) {
        error = "The selected address is not mapped in the active type workspace.";
        return false;
    }
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context.workspace) {
        error = "The selected address is not mapped in the active type workspace.";
        return false;
    }
    char formatted[32]{};
    std::snprintf(formatted, sizeof(formatted), "0x%llX",
        static_cast<unsigned long long>(runtime_address));
    apply_address_->setText(QString::fromLatin1(formatted));
    apply_type_->clear();
    state_->apply_address = apply_address_->text();
    state_->apply_type.clear();
    state_->apply_status =
        "Enter a canonical type and review Apply before changing the overlay.";
    state_->apply_error = false;
    apply_status_->setText(QString::fromStdString(state_->apply_status));
    apply_status_->setVisible(true);
    error.clear();
    return true;
}

void QtTypesCatalogView::showRowMenu(const QPoint& global_pos, int view_row) {
    // Full retained-entity catalog menu is ported in
    // qt_types_catalog_menu.cpp (per-tab action sets verbatim).
    show_types_catalog_menu(this, this, global_pos, view_row);
}

void QtTypesCatalogView::openDeclarationReview(const std::string& name,
                                               std::string declaration) {
    open_type_declaration_review(context_, std::move(name), std::move(declaration),
        this);
}

}
