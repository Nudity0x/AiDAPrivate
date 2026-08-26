#include "qt/workbench/qt_workbench_diff_view.hpp"

#include <QAbstractTableModel>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include <chrono>
#include <cstdio>

#include "core/analysis/workspace/workspace_registry.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/shell_host_contract.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_revision_poller.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::workbench {

namespace {

class frame_cancellation_t final
    : public aida::workbench::diff_document::diff_cancellation_t {
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

class QtDiffModel : public QAbstractTableModel {
public:
    explicit QtDiffModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setPage(aida::workbench::diff_document::diff_page_t page) {
        beginResetModel();
        page_ = std::move(page);
        endResetModel();
    }
    const aida::workbench::diff_document::diff_page_t& page() const noexcept {
        return page_;
    }
    const aida::workbench::diff_document::diff_entry_t* rowAt(
        int row) const noexcept {
        if (row < 0 || static_cast<std::size_t>(row) >= page_.entries.size())
            return nullptr;
        return &page_.entries[static_cast<std::size_t>(row)];
    }

    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(page_.entries.size());
    }
    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : 4;
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) return {};
        const auto* entry = rowAt(index.row());
        if (!entry) return {};
        if (role == Qt::DisplayRole) {
            char address[32]{};
            std::snprintf(address, sizeof(address), "%016llX",
                static_cast<unsigned long long>(entry->address));
            switch (index.column()) {
            case 0: return QString::fromLatin1(address);
            case 1: return QString::fromStdString(entry->entity_key);
            case 2: return QString::fromStdString(entry->old_value);
            case 3: return QString::fromStdString(entry->new_value);
            default: return {};
            }
        }
        if (role == Qt::ForegroundRole) {
            const auto& tokens = aida::qt::theme::tokens();
            switch (index.column()) {
            case 0: return tokens.syn_address;
            case 1: return tokens.text_secondary;
            case 2: return tokens.error;
            case 3: return tokens.success;
            default: return {};
            }
        }
        if (role == Qt::ToolTipRole)
            return data(index, Qt::DisplayRole);
        if (role == Qt::FontRole && index.column() == 0)
            return aida::qt::theme::fonts::codeRegular();
        return {};
    }
    void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override {
        for (QModelRoleData& roleData : span) {
            if (roleData.role() == Qt::DisplayRole ||
                roleData.role() == Qt::ToolTipRole ||
                roleData.role() == Qt::FontRole ||
                roleData.role() == Qt::ForegroundRole)
                roleData.setData(data(index, roleData.role()));
            else
                roleData.clearData();
        }
    }
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (section) {
        case 0: return QStringLiteral("Address");
        case 1: return QStringLiteral("Entity");
        case 2: return QStringLiteral("Old");
        case 3: return QStringLiteral("New");
        default: return {};
        }
    }

private:
    aida::workbench::diff_document::diff_page_t page_;
};

}

QtWorkbenchDiffView::QtWorkbenchDiffView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.document.diff"));
    const auto& tokens = aida::qt::theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.diff.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x, tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.toolbar.group_gap);
    kind_combo_ = new QComboBox(toolbar);
    kind_combo_->setObjectName(QStringLiteral("aida.diff.kind"));
    kind_combo_->setToolTip(QStringLiteral(
        "Choose which retained revisions this document compares"));
    kind_combo_->addItem(QStringLiteral("Generation"),
        static_cast<int>(aida::workbench::diff_document::diff_kind_t::generation));
    kind_combo_->addItem(QStringLiteral("Overlay"),
        static_cast<int>(aida::workbench::diff_document::diff_kind_t::overlay));
    kind_combo_->addItem(QStringLiteral("Workspace"),
        static_cast<int>(aida::workbench::diff_document::diff_kind_t::workspace));
    toolbar_layout->addWidget(kind_combo_);
    prev_button_ = new QPushButton(QStringLiteral("Previous"), toolbar);
    prev_button_->setObjectName(QStringLiteral("aida.diff.prev"));
    prev_button_->setToolTip(QStringLiteral("Show the previous page of changes"));
    next_button_ = new QPushButton(QStringLiteral("Next"), toolbar);
    next_button_->setObjectName(QStringLiteral("aida.diff.next"));
    next_button_->setToolTip(QStringLiteral("Show the next page of changes"));
    toolbar_layout->addWidget(prev_button_);
    toolbar_layout->addWidget(next_button_);
    status_label_ = new QLabel(toolbar);
    status_label_->setObjectName(QStringLiteral("aida.diff.status"));
    status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    toolbar_layout->addWidget(status_label_);
    toolbar_layout->addStretch(1);
    layout->addWidget(toolbar);
    model_ = new QtDiffModel(this);
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("aida.diff.table"));
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
    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.diff.state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);
    layout->addWidget(table_, 1);

    connect(kind_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        kind_ = static_cast<aida::workbench::diff_document::diff_kind_t>(
            kind_combo_->itemData(index).toInt());
        offset_ = 0;
        reloadPage();
    });
    connect(prev_button_, &QPushButton::clicked, this, [this] {
        offset_ = offset_ > 256 ? offset_ - 256 : 0;
        reloadPage();
    });
    connect(next_button_, &QPushButton::clicked, this, [this] {
        offset_ = (std::min)(offset_ + 256,
            total_ > 256 ? total_ - 256 : std::uint64_t{0});
        reloadPage();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid()) return;
        auto* model = static_cast<QtDiffModel*>(model_);
        const auto* entry = model ? model->rowAt(current.row()) : nullptr;
        if (!entry || entry->address == 0) return;
        const auto workspace =
            aida::analysis::workspace_registry().selected_for_ui();
        if (!workspace) return;
        aida::workbench::workbench_shell_workspace_context_t context;
        if (!aida::workbench::workbench_shell_runtime_t::instance()
                .workspace_context(workspace, context))
            return;
        aida::workbench::selection_context_t selection;
        selection.kind = aida::workbench::selection_kind_t::address;
        selection.has_address = true;
        selection.address = entry->address;
        selection.entity_key = entry->entity_key;
        aida::workbench::document_local_cursor_t cursor;
        cursor.has_position = true;
        cursor.position = entry->address;
        static_cast<void>(aida::workbench::workbench_shell_runtime_t::instance()
            .navigate_document(workspace, aida::workbench::document_kind_t::diff,
                std::nullopt, selection, cursor, context));
    });
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = table_->indexAt(pos);
        if (index.isValid())
            showRowMenu(table_->viewport()->mapToGlobal(pos), index.row());
    });
    table_->installEventFilter(this);

    auto* bridge = &analysis::QtAnalysisBridge::instance();
    connect(bridge, &analysis::QtAnalysisBridge::activeContextChanged, this,
            [this](analysis::QtWorkspaceContext* context) {
        if (context_connection_) disconnect(context_connection_);
        context_connection_ = QMetaObject::Connection();
        if (!context) {
            reloadPage();
            return;
        }
        context_connection_ = connect(context->poller(),
            &analysis::QtRevisionPoller::revisionsChanged, this,
            [this](quint64, quint64, quint64, quint64) { reloadPage(); });
        reloadPage();
    });
    reloadPage();
}

void QtWorkbenchDiffView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    reloadPage();
}

bool QtWorkbenchDiffView::eventFilter(QObject* watched, QEvent* event) {
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

void QtWorkbenchDiffView::reloadPage() {
    const auto workspace = aida::analysis::workspace_registry().selected_for_ui();
    if (!workspace) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No analysis workspace"));
        state_view_->setMessage(QStringLiteral(
            "Open and analyze a binary to compare its retained revisions."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    aida::workbench::workbench_shell_workspace_context_t context;
    const auto loaded = aida::workbench::workbench_shell_runtime_t::instance()
        .workspace_context(workspace, context);
    if (!loaded) {
        state_view_->setState(widgets::AidaStateView::State::Error);
        state_view_->setTitle(QStringLiteral("Diff document failed to load"));
        state_view_->setMessage(QStringLiteral(
            "The Workbench context for the active workspace could not be loaded (error %1).")
            .arg(static_cast<unsigned>(loaded.code)));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (!context.diff_document) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("Diff provider unavailable"));
        state_view_->setMessage(QStringLiteral(
            "The active workspace did not publish a Diff provider for this revision."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (observed_generation_ != context.analysis_generation) {
        observed_generation_ = context.analysis_generation;
        offset_ = 0;
        total_ = 0;
    }
    aida::workbench::diff_document::diff_scope_t scope;
    scope.kind = kind_;
    scope.before.workspace_id = context.workspace.value;
    scope.after.workspace_id = context.workspace.value;
    scope.before.generation = context.analysis_generation;
    scope.after.generation = context.analysis_generation;
    bool scoped = false;
    if (kind_ == aida::workbench::diff_document::diff_kind_t::generation) {
        if (context.analysis_generation >= 2) {
            scope.before.generation = context.analysis_generation - 1;
            scoped = true;
        }
    } else if (kind_ == aida::workbench::diff_document::diff_kind_t::overlay) {
        if (context.overlay_revision != 0) {
            scope.before.overlay_revision = context.overlay_revision - 1;
            scope.after.overlay_revision = context.overlay_revision;
            scoped = true;
        }
    } else {
        for (const auto& other :
            aida::workbench::workbench_shell_runtime_t::instance()
                .analysis_workspaces()) {
            if (!other || other == workspace) continue;
            aida::workbench::workbench_shell_workspace_context_t other_context;
            const auto other_loaded =
                aida::workbench::workbench_shell_runtime_t::instance()
                    .workspace_context(other, other_context);
            if (!other_loaded) continue;
            scope.after.workspace_id = other_context.workspace.value;
            scope.after.generation = other_context.analysis_generation;
            scoped = true;
            break;
        }
    }
    if (!scoped) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No comparable revision"));
        state_view_->setMessage(QStringLiteral(
            "The selected diff requires another retained generation, overlay revision, or workspace."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    aida::workbench::diff_document::diff_page_t page;
    frame_cancellation_t cancellation(30);
    const auto loaded_page = context.diff_document->page(
        {offset_, 256, static_cast<aida::workbench::diff_document::diff_domain_t>(0xFF)},
        context.analysis_generation, scope, &cancellation, page);
    if (!loaded_page) {
        state_view_->setState(widgets::AidaStateView::State::Loading);
        state_view_->setTitle(QStringLiteral("Diff materialization deferred"));
        state_view_->setMessage(QStringLiteral(
            "The provider has not published a coherent comparison page for the selected scope yet."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    total_ = page.total_entries;
    prev_button_->setEnabled(offset_ != 0);
    next_button_->setEnabled(total_ > offset_ + 256);
    if (page.entries.empty()) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No differences"));
        state_view_->setMessage(QStringLiteral(
            "The selected revisions contain no changes in this diff domain."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        status_label_->setText(QStringLiteral("%1 changes")
            .arg(static_cast<unsigned long long>(total_)));
        return;
    }
    status_label_->setText(QStringLiteral("%1 changes")
        .arg(static_cast<unsigned long long>(total_)));
    state_view_->setVisible(false);
    table_->setVisible(true);
    static_cast<QtDiffModel*>(model_)->setPage(std::move(page));
}

void QtWorkbenchDiffView::showRowMenu(const QPoint& global_pos, int view_row) {
    auto* model = static_cast<QtDiffModel*>(model_);
    const auto* entry = model ? model->rowAt(view_row) : nullptr;
    if (!entry) return;
    const auto workspace = aida::analysis::workspace_registry().selected_for_ui();
    if (!workspace) return;
    aida::workbench::workbench_shell_workspace_context_t context;
    if (!aida::workbench::workbench_shell_runtime_t::instance()
            .workspace_context(workspace, context))
        return;
    aida::ui::application_ui::retained_entity_context_t retained;
    retained.owner_id = "workbench.diff";
    retained.entity_id = std::to_string(context.workspace.value) + ":" +
        std::to_string(model->page().offset + static_cast<std::size_t>(view_row)) +
        ":" + std::to_string(std::hash<std::string>{}(entry->entity_key)) + ":" +
        std::to_string(entry->address);
    retained.entity_generation = context.analysis_generation;
    retained.active_view = aida::ui::stable_view_id_t("document.diff");
    const auto analysis_generation = context.analysis_generation;
    const auto analysis_revision = context.analysis_revision;
    const auto overlay_revision = context.overlay_revision;
    retained.validate_identity = [workspace, analysis_generation, analysis_revision,
        overlay_revision] {
        const auto current = aida::analysis::workspace_registry().selected_for_ui();
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
    const auto add = [&retained](const char* id, bool enabled, const char* reason,
                                 auto invoke) {
        aida::ui::application_ui::retained_entity_action_t action;
        action.action_id = id;
        action.capability = enabled ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(reason);
        action.invoke = std::move(invoke);
        retained.actions.push_back(std::move(action));
    };
    const auto address = entry->address;
    const auto entity = entry->entity_key;
    add("workbench.diff.follow", address != 0,
        "The retained change has no navigable address.",
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
                .navigate_document(workspace, aida::workbench::document_kind_t::diff,
                    std::nullopt, selection, cursor, context));
            return aida::ui::action_handler_result_t::completed();
        });
    add("workbench.diff.copy_address", address != 0,
        "The retained change has no address.", [address] {
            char text[32]{};
            std::snprintf(text, sizeof(text), "0x%llX",
                static_cast<unsigned long long>(address));
            clipboard::set_text(QString::fromLatin1(text));
            return aida::ui::action_handler_result_t::completed();
        });
    const std::string before = entry->old_value;
    add("workbench.diff.copy_before", true, "", [before] {
        clipboard::set_text(QString::fromStdString(before));
        return aida::ui::action_handler_result_t::completed();
    });
    const std::string after = entry->new_value;
    add("workbench.diff.copy_after", true, "", [after] {
        clipboard::set_text(QString::fromStdString(after));
        return aida::ui::action_handler_result_t::completed();
    });
    analysis::QtAnalysisBridge::instance().showRetainedMenu(retained,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}
