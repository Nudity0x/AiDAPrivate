#include "qt/workbench/qt_workbench_navigator_view.hpp"

#include <QAbstractTableModel>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QTableView>
#include <QVBoxLayout>

#include <chrono>

#include "core/analysis/workspace/workspace_registry.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/shell_host_contract.hpp"
#include "core/workbench/navigator/workbench_navigator.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/analysis/qt_revision_poller.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::workbench {

namespace {

// 20 ms deadline budget for the synchronous page fetch (verbatim, 07 sec. 8.1).
class frame_cancellation_t final
    : public aida::workbench::navigator::navigator_cancellation_t {
public:
    explicit frame_cancellation_t(std::uint32_t budget_ms)
        : deadline_(std::chrono::steady_clock::now() +
            std::chrono::milliseconds(budget_ms)) {}

    bool cancelled() const noexcept override {
        return std::chrono::steady_clock::now() >= deadline_;
    }

private:
    std::chrono::steady_clock::time_point deadline_;
};

class QtNavigatorModel : public QAbstractTableModel {
public:
    explicit QtNavigatorModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setPage(aida::workbench::navigator::navigator_tree_page_t page) {
        beginResetModel();
        page_ = std::move(page);
        endResetModel();
    }
    const aida::workbench::navigator::navigator_tree_page_t& page() const noexcept {
        return page_;
    }
    const aida::workbench::navigator::navigator_item_view_t* rowAt(
        int row) const noexcept {
        if (row < 0 || static_cast<std::size_t>(row) >= page_.rows.size())
            return nullptr;
        return &page_.rows[static_cast<std::size_t>(row)];
    }

    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(page_.rows.size());
    }
    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : 1;
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) return {};
        const auto* row = rowAt(index.row());
        if (!row) return {};
        if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
            return QString::fromUtf8(row->label.data(),
                static_cast<int>(row->label.size()));
        return {};
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
    aida::workbench::navigator::navigator_tree_page_t page_;
};

}

QtWorkbenchNavigatorView::QtWorkbenchNavigatorView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.navigator"));
    const auto& tokens = aida::qt::theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.navigator.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.toolbar.group_gap);
    domain_combo_ = new QComboBox(toolbar);
    domain_combo_->setObjectName(QStringLiteral("aida.navigator.domain"));
    domain_combo_->setToolTip(QStringLiteral(
        "Choose the entity domain listed by the Navigator"));
    using domain_t = aida::workbench::navigator::navigator_domain_t;
    domain_combo_->addItem(QStringLiteral("Functions"),
        static_cast<int>(domain_t::functions));
    domain_combo_->addItem(QStringLiteral("Imports"),
        static_cast<int>(domain_t::imports));
    domain_combo_->addItem(QStringLiteral("Exports"),
        static_cast<int>(domain_t::exports));
    domain_combo_->addItem(QStringLiteral("Strings"),
        static_cast<int>(domain_t::strings));
    domain_combo_->addItem(QStringLiteral("Symbols"),
        static_cast<int>(domain_t::symbols));
    domain_combo_->addItem(QStringLiteral("Types"),
        static_cast<int>(domain_t::types));
    toolbar_layout->addWidget(domain_combo_, 1);
    status_label_ = new QLabel(toolbar);
    status_label_->setObjectName(QStringLiteral("aida.navigator.status"));
    status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    toolbar_layout->addWidget(status_label_);
    layout->addWidget(toolbar);
    model_ = new QtNavigatorModel(this);
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("aida.navigator.table"));
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setDefaultSectionSize(tokens.table.compact_row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setModel(model_);
    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.navigator.state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);
    layout->addWidget(table_, 1);

    connect(domain_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        domain_ = static_cast<domain_t>(
            domain_combo_->itemData(index).toInt());
        selected_id_ = 0;
        reloadPage();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid()) return;
        auto* model = static_cast<QtNavigatorModel*>(model_);
        const auto* row = model ? model->rowAt(current.row()) : nullptr;
        if (!row) return;
        selected_id_ = row->id.value;
        if (!row->has_address) return;
        const auto workspace =
            aida::analysis::workspace_registry().selected_for_ui();
        if (!workspace) return;
        aida::workbench::workbench_shell_workspace_context_t context;
        const auto loaded =
            aida::workbench::workbench_shell_runtime_t::instance()
                .workspace_context(workspace, context);
        if (!loaded) return;
        aida::workbench::selection_context_t selection;
        selection.kind = aida::workbench::selection_kind_t::address;
        selection.has_address = true;
        selection.address = row->address;
        selection.entity_key = std::to_string(row->id.value);
        aida::workbench::document_local_cursor_t cursor;
        cursor.has_position = true;
        cursor.position = row->address;
        static_cast<void>(aida::workbench::workbench_shell_runtime_t::instance()
            .navigate_document(workspace, aida::workbench::document_kind_t::disassembly,
                std::nullopt, selection, cursor, context));
    });
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = table_->indexAt(pos);
        if (index.isValid())
            showRowMenu(table_->viewport()->mapToGlobal(pos), index.row());
    });
    table_->installEventFilter(this);

    auto* bridge = &aida::qt::analysis::QtAnalysisBridge::instance();
    connect(bridge, &aida::qt::analysis::QtAnalysisBridge::activeContextChanged,
            this, [this](aida::qt::analysis::QtWorkspaceContext* context) {
        if (context_connection_) disconnect(context_connection_);
        context_connection_ = QMetaObject::Connection();
        if (!context) {
            reloadPage();
            return;
        }
        context_connection_ = connect(context->poller(),
            &aida::qt::analysis::QtRevisionPoller::revisionsChanged, this,
            [this](quint64, quint64, quint64, quint64) { reloadPage(); });
        reloadPage();
    });
    reloadPage();
}

void QtWorkbenchNavigatorView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    reloadPage();
}

bool QtWorkbenchNavigatorView::eventFilter(QObject* watched, QEvent* event) {
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

void QtWorkbenchNavigatorView::reloadPage() {
    status_label_->clear();
    const auto workspace = aida::analysis::workspace_registry().selected_for_ui();
    if (!workspace) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No analysis workspace"));
        state_view_->setMessage(QStringLiteral(
            "Open and analyze a binary to use this Workbench surface."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    aida::workbench::workbench_shell_workspace_context_t context;
    const auto loaded = aida::workbench::workbench_shell_runtime_t::instance()
        .workspace_context(workspace, context);
    if (!loaded) {
        state_view_->setState(widgets::AidaStateView::State::Error);
        state_view_->setTitle(QStringLiteral("Navigator failed to load"));
        state_view_->setMessage(QStringLiteral(
            "The Workbench context for the active workspace could not be loaded (error %1).")
            .arg(static_cast<unsigned>(loaded.code)));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (observed_generation_ != context.analysis_generation) {
        selected_id_ = 0;
        observed_generation_ = context.analysis_generation;
    }
    if (!context.navigator_tree) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("Navigator unavailable"));
        state_view_->setMessage(QStringLiteral(
            "The active workspace did not publish a Navigator provider for this revision."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    aida::workbench::navigator::navigator_tree_request_t request;
    request.domain = domain_;
    request.page.limit = 256;
    frame_cancellation_t cancellation(20);
    aida::workbench::navigator::navigator_tree_page_t page;
    const auto page_result = context.navigator_tree->page(request, &cancellation,
        page);
    if (!page_result) {
        state_view_->setState(widgets::AidaStateView::State::Loading);
        state_view_->setTitle(QStringLiteral("Navigator materialization deferred"));
        state_view_->setMessage(QStringLiteral(
            "The provider has not published a coherent page for the active analysis revision yet."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (page.rows.empty()) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No Navigator entries"));
        state_view_->setMessage(QStringLiteral(
            "The selected domain has no published entries in the active workspace revision."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    status_label_->setText(QStringLiteral("%1 items")
        .arg(static_cast<unsigned long long>(page.total_rows)));
    state_view_->setVisible(false);
    table_->setVisible(true);
    static_cast<QtNavigatorModel*>(model_)->setPage(std::move(page));
}


void QtWorkbenchNavigatorView::showRowMenu(const QPoint& global_pos, int view_row) {
    auto* model = static_cast<QtNavigatorModel*>(model_);
    const auto* row = model ? model->rowAt(view_row) : nullptr;
    if (!row) return;
    const auto workspace = aida::analysis::workspace_registry().selected_for_ui();
    if (!workspace) return;
    aida::workbench::workbench_shell_workspace_context_t context;
    if (!aida::workbench::workbench_shell_runtime_t::instance()
            .workspace_context(workspace, context))
        return;
    selected_id_ = row->id.value;
    const std::string row_label(row->label);
    aida::ui::application_ui::retained_entity_context_t retained;
    retained.owner_id = "workbench.navigator";
    retained.entity_id = std::to_string(context.workspace.value) + ":" +
        std::to_string(row->id.value) + ":" + row_label;
    retained.entity_generation = context.analysis_generation;
    retained.active_view = aida::ui::stable_view_id_t("view.navigator");
    const auto analysis_generation = context.analysis_generation;
    const auto analysis_revision = context.analysis_revision;
    const auto overlay_revision = context.overlay_revision;
    retained.validate_identity = [workspace, analysis_generation, analysis_revision,
        overlay_revision] {
        const auto current =
            aida::analysis::workspace_registry().selected_for_ui();
        if (!current || current != workspace)
            return aida::ui::capability_state_t::unavailable(
                "The retained Workbench entity belongs to an older analysis or overlay revision");
        aida::workbench::workbench_shell_workspace_context_t current_context;
        if (!aida::workbench::workbench_shell_runtime_t::instance()
                .workspace_context(current, current_context))
            return aida::ui::capability_state_t::unavailable(
                "The retained Workbench entity belongs to an older analysis or overlay revision");
        return current_context.analysis_generation == analysis_generation &&
            current_context.analysis_revision == analysis_revision &&
            current_context.overlay_revision == overlay_revision
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The retained Workbench entity belongs to an older analysis or overlay revision");
    };
    const auto address = row->address;
    const auto entity = std::to_string(row->id.value);
    const auto add = [&retained](const char* id, bool enabled, const char* reason,
                                 auto invoke) {
        aida::ui::application_ui::retained_entity_action_t action;
        action.action_id = id;
        action.capability = enabled ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(reason);
        action.invoke = std::move(invoke);
        retained.actions.push_back(std::move(action));
    };
    add("workbench.navigator.follow_disassembly", row->has_address,
        "The retained Navigator entity has no address.",
        [workspace, address, entity] {
            aida::workbench::workbench_shell_workspace_context_t context;
            if (!aida::workbench::workbench_shell_runtime_t::instance()
                    .workspace_context(workspace, context))
                return aida::ui::action_handler_result_t::failed(
                    "Workbench context is unavailable");
            aida::workbench::selection_context_t selection;
            selection.kind = aida::workbench::selection_kind_t::address;
            selection.has_address = true;
            selection.address = address;
            selection.entity_key = entity;
            aida::workbench::document_local_cursor_t cursor;
            cursor.has_position = true;
            cursor.position = address;
            static_cast<void>(aida::workbench::workbench_shell_runtime_t::instance()
                .navigate_document(workspace,
                    aida::workbench::document_kind_t::disassembly, std::nullopt,
                    selection, cursor, context));
            return aida::ui::action_handler_result_t::completed();
        });
    add("workbench.navigator.copy_address", row->has_address,
        "The retained Navigator entity has no address.", [address] {
            char text[32]{};
            std::snprintf(text, sizeof(text), "0x%llX",
                static_cast<unsigned long long>(address));
            aida::qt::clipboard::set_text(QString::fromLatin1(text));
            return aida::ui::action_handler_result_t::completed();
        });
    add("workbench.navigator.copy_name", !row_label.empty(),
        "The retained Navigator entity has no display name.", [row_label] {
            aida::qt::clipboard::set_text(QString::fromStdString(row_label));
            return aida::ui::action_handler_result_t::completed();
        });
    aida::qt::analysis::QtAnalysisBridge::instance().showRetainedMenu(retained,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}
