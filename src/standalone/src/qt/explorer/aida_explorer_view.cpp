#include "qt/explorer/aida_explorer_view.hpp"

#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QFrame>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMouseEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/explorer_views.hpp"
#include "helpers/diag_log.hpp"
#include "helpers/globals.h"
#include "qt/bridge/aida_dialog.hpp"
#include "qt/documents/aida_document_model.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/explorer/aida_explorer_delegate.hpp"
#include "qt/explorer/aida_explorer_model.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_icons.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_state_view.hpp"
#include "qt/widgets/aida_tool_button.hpp"

namespace aida::ui::explorer_views {

file_operation_result_t submit_confirmed_file_operation(file_operation_t operation,
    std::filesystem::path source, std::filesystem::path destination, bool source_directory);
file_operation_result_t submit_confirmed_batch_file_operation(file_operation_t operation,
    std::vector<file_operation_target_t> targets);

}

namespace aida::qt::explorer {

namespace {

bool valid_leaf_name(const std::string& value, std::string& reason)
{
    if (value.empty()) {
        reason = "Enter a name";
        return false;
    }
    if (value == "." || value == "..") {
        reason = "The name cannot be '.' or '..'";
        return false;
    }
    if (value.size() > 240) {
        reason = "The name exceeds the 240-byte workspace limit";
        return false;
    }
    constexpr const char* invalid = "<>:\"/\\|?*";
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || std::strchr(invalid, static_cast<int>(byte))) {
            reason = "The name contains a control character or a Windows-reserved character";
            return false;
        }
    }
    if (value.back() == ' ' || value.back() == '.') {
        reason = "Windows file names cannot end with a space or period";
        return false;
    }
    std::string device = value.substr(0, value.find('.'));
    std::transform(device.begin(), device.end(), device.begin(),
        [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    const bool numbered_device = device.size() == 4 &&
        (device.compare(0, 3, "COM") == 0 || device.compare(0, 3, "LPT") == 0) &&
        device[3] >= '1' && device[3] <= '9';
    if (device == "CON" || device == "PRN" || device == "AUX" ||
        device == "NUL" || numbered_device) {
        reason = "The name is reserved by Windows";
        return false;
    }
    reason.clear();
    return true;
}

}

AidaExplorerView::AidaExplorerView(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.view.project_explorer"));
    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QFrame(this);
    toolbar->setObjectName(QStringLiteral("aida.explorer.toolbar"));
    toolbar->setFrameShape(QFrame::NoFrame);
    toolbar->setProperty("aidaRole", QStringLiteral("toolbar"));
    toolbar->setFixedHeight(t.toolbar.height);
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(t.toolbar.padding_x, t.toolbar.padding_y,
        t.toolbar.padding_x, t.toolbar.padding_y);
    toolbar_layout->setSpacing(t.toolbar.group_gap);
    auto* open_folder = new widgets::AidaButton(QStringLiteral("Open Folder"), toolbar);
    open_folder->setIcon(theme::icons::icon(QStringLiteral("files-empty")));
    open_folder->setObjectName(QStringLiteral("aida.explorer.open_folder"));
    open_folder->setKind(widgets::AidaButton::Kind::Secondary);
    open_folder->setToolTip(QStringLiteral("Open a folder to create a programming and reverse-engineering workspace"));
    toolbar_layout->addWidget(open_folder);
    auto* refresh = new widgets::AidaToolButton(
        theme::icons::icon(QStringLiteral("spinner")), QStringLiteral("Refresh Explorer"), toolbar);
    refresh->setObjectName(QStringLiteral("aida.explorer.refresh"));
    toolbar_layout->addWidget(refresh);
    auto* search = new widgets::AidaToolButton(
        theme::icons::icon(QStringLiteral("search")), QStringLiteral("Search Workspace"), toolbar);
    search->setObjectName(QStringLiteral("aida.explorer.search"));
    toolbar_layout->addWidget(search);
    toolbar_layout->addStretch(1);
    layout->addWidget(toolbar);

    root_label_ = new QLabel(this);
    root_label_->setObjectName(QStringLiteral("aida.explorer.root"));
    root_label_->setContentsMargins(t.spacing.sm, 0, t.spacing.sm, 0);
    root_label_->setMinimumHeight(t.row.standard);
    root_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root_label_->setAccessibleName(QStringLiteral("Workspace root"));
    root_label_->installEventFilter(this);
    layout->addWidget(root_label_);

    auto* filter_host = new QWidget(this);
    filter_host->setObjectName(QStringLiteral("aida.explorer.filter_host"));
    auto* filter_layout = new QHBoxLayout(filter_host);
    filter_layout->setContentsMargins(t.spacing.sm, 0, t.spacing.sm, t.spacing.xs);
    filter_layout->setSpacing(0);
    filter_edit_ = new widgets::AidaLineEdit(QStringLiteral("Filter files and folders"), filter_host);
    filter_edit_->setClearButtonEnabled(true);
    filter_edit_->setObjectName(QStringLiteral("aida.explorer.filter"));
    filter_edit_->setToolTip(QStringLiteral("Filter the workspace tree by file or folder name; Escape clears the filter"));
    filter_edit_->installEventFilter(this);
    filter_layout->addWidget(filter_edit_);
    layout->addWidget(filter_host);

    state_strip_ = new QFrame(this);
    state_strip_->setObjectName(QStringLiteral("aida.explorer.state_strip"));
    state_strip_->setFrameShape(QFrame::NoFrame);
    state_strip_->setProperty("aidaRole", QStringLiteral("statusbar"));
    auto* strip_layout = new QHBoxLayout(state_strip_);
    strip_layout->setContentsMargins(t.spacing.sm, t.spacing.xxs, t.spacing.sm, t.spacing.xxs);
    strip_layout->setSpacing(t.spacing.sm);
    state_label_ = new QLabel(state_strip_);
    state_label_->setObjectName(QStringLiteral("aida.explorer.state_label"));
    state_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    state_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    state_label_->installEventFilter(this);
    strip_layout->addWidget(state_label_, 1);
    auto* cancel_index = new widgets::AidaButton(QStringLiteral("Cancel"), state_strip_);
    cancel_index->setObjectName(QStringLiteral("aida.explorer.cancel_index"));
    cancel_index->setKind(widgets::AidaButton::Kind::Ghost);
    cancel_index->setControlSize(widgets::AidaButton::ControlSize::Small);
    cancel_index->setToolTip(QStringLiteral("Cancel this project index generation"));
    strip_layout->addWidget(cancel_index);
    state_strip_->setVisible(false);
    layout->addWidget(state_strip_);

    model_ = new AidaExplorerModel(this);
    delegate_ = new AidaExplorerDelegate(this);
    list_ = new QListView(this);
    list_->setObjectName(QStringLiteral("aida.explorer.list"));
    list_->setModel(model_);
    list_->setItemDelegate(delegate_);
    list_->setViewMode(QListView::ListMode);
    list_->setUniformItemSizes(true);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setMouseTracking(true);
    list_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setAccessibleName(QStringLiteral("Project Explorer files"));
    layout->addWidget(list_, 1);

    empty_state_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("Open a workspace folder"),
        QStringLiteral("Browse source files, scripts, binaries, symbols, and project artifacts in one tree."),
        this);
    empty_state_->setObjectName(QStringLiteral("aida.explorer.empty"));
    empty_state_->setVisible(false);
    layout->addWidget(empty_state_, 1);

    connect(open_folder, &QAbstractButton::clicked, this, [this] {
        aida::ui::application_ui::execute_action("file.open_folder",
            aida::ui::action_invocation_source_t::toolbar);
        Q_EMIT openFolderRequested();
    });
    connect(refresh, &QAbstractButton::clicked, this, [this] {
        aida::ui::application_ui::execute_action("explorer.refresh",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(search, &QAbstractButton::clicked, this, [this] {
        aida::ui::application_ui::execute_action("explorer.search",
            aida::ui::action_invocation_source_t::toolbar);
        Q_EMIT searchRequested();
    });
    connect(cancel_index, &QAbstractButton::clicked, this, [this] {
        model_->cancelRefresh();
    });
    connect(empty_state_, &widgets::AidaStateView::actionTriggered, this, [this] {
        if (empty_action_ == EmptyAction::ClearFilter) {
            filter_edit_->clear();
            list_->setFocus(Qt::OtherFocusReason);
        } else if (empty_action_ == EmptyAction::OpenFolder) {
            aida::ui::application_ui::execute_action("file.open_folder",
                aida::ui::action_invocation_source_t::toolbar);
            Q_EMIT openFolderRequested();
        }
    });
    connect(filter_edit_, &QLineEdit::textChanged, this,
            &AidaExplorerView::onFilterChanged);
    connect(list_, &QListView::activated, this, &AidaExplorerView::onActivated);
    connect(list_, &QWidget::customContextMenuRequested, this,
            &AidaExplorerView::onCustomContextMenu);
    connect(list_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) {
                syncBackendSelectionFromView();
            });
    connect(model_, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                if (roles.isEmpty() || roles.contains(AidaExplorerModel::SelectedRole)) {
                    refreshStateStrip();
                    applyBackendSelectionToView();
                }
            });
    connect(model_, &AidaExplorerModel::indexingStateChanged, this,
            &AidaExplorerView::onIndexingStateChanged);
    list_->installEventFilter(this);
    list_->viewport()->installEventFilter(this);

    rebuildHeader();
    onIndexingStateChanged();
}

AidaExplorerView::~AidaExplorerView() = default;

void AidaExplorerView::setDocumentModel(documents::AidaDocumentModel* model)
{
    model_->setDocumentModel(model);
}

void AidaExplorerView::rebuildHeader()
{
    root_full_text_ = model_->currentRoot();
    applyRootLabel();
}

void AidaExplorerView::applyRootLabel()
{
    QString display = root_full_text_.isEmpty()
        ? QStringLiteral("No folder open") : root_full_text_;
    const int available = root_label_->contentsRect().width();
    if (available > 0 && !root_full_text_.isEmpty())
        display = QFontMetricsF(root_label_->font()).elidedText(
            root_full_text_, Qt::ElideMiddle, available);
    root_label_->setText(display);
    root_label_->setToolTip(root_full_text_.isEmpty()
        ? QStringLiteral("No workspace folder is open") : root_full_text_);
    const char* variant = root_full_text_.isEmpty() ? "secondary" : "primary";
    if (root_label_->property("aidaVariant") != QVariant(QString::fromLatin1(variant))) {
        root_label_->setProperty("aidaVariant", QString::fromLatin1(variant));
        theme::stylesheet::repolish(root_label_);
    }
}

void AidaExplorerView::onFilterChanged(const QString& text)
{
    model_->setFilter(text);
}

void AidaExplorerView::onIndexingStateChanged()
{
    rebuildHeader();
    refreshStateStrip();
    refreshEmptyState();
    applyBackendSelectionToView();
    applyContentWidth();
}

void AidaExplorerView::refreshStateStrip()
{
    QStringList parts;
    bool visible = false;
    if (model_->indexing()) {
        parts << QStringLiteral("Indexing... %1 folders, %2 items")
            .arg(static_cast<qulonglong>(model_->indexedDirectoryCount()))
            .arg(static_cast<qulonglong>(model_->indexedEntryCount()));
        visible = true;
    } else if (model_->truncated()) {
        parts << QStringLiteral("Project tree reached its bounded indexing limit; narrow the workspace roots.");
        visible = true;
    }
    if (!model_->indexError().isEmpty()) {
        parts << QStringLiteral("Some workspace locations could not be indexed.");
        visible = true;
    }
    if (model_->selectionSize() > 1)
        parts << QStringLiteral("%1 items selected").arg(static_cast<qulonglong>(model_->selectionSize()));
    if (!model_->selectionError().isEmpty()) {
        parts << model_->selectionError();
        visible = true;
    }
    state_full_text_ = parts.join(QStringLiteral(" • "));
    applyStateLabel();
    QString tooltip = state_full_text_;
    if (!model_->indexError().isEmpty())
        tooltip += tooltip.isEmpty() ? model_->indexError()
            : QStringLiteral("\n") + model_->indexError();
    state_label_->setToolTip(tooltip);
    state_strip_->setVisible(visible);
}

void AidaExplorerView::applyStateLabel()
{
    QString display = state_full_text_;
    const int available = state_label_->contentsRect().width();
    if (available > 0)
        display = QFontMetricsF(state_label_->font()).elidedText(
            state_full_text_, Qt::ElideRight, available);
    state_label_->setText(display);
}

void AidaExplorerView::refreshEmptyState()
{
    const int rows = model_->rowCount();
    if (model_->indexing() && rows == 0) {
        empty_action_ = EmptyAction::None;
        empty_state_->setState(widgets::AidaStateView::State::Loading);
        empty_state_->setTitle(QStringLiteral("Indexing workspace"));
        empty_state_->setMessage(QStringLiteral("Scanning workspace folders and files..."));
        empty_state_->setVisible(true);
        list_->setVisible(false);
        return;
    }
    if (rows > 0) {
        empty_action_ = EmptyAction::None;
        empty_state_->setVisible(false);
        list_->setVisible(true);
        return;
    }
    empty_state_->setState(widgets::AidaStateView::State::Empty);
    if (!model_->hasRoot()) {
        empty_action_ = EmptyAction::OpenFolder;
        empty_state_->setTitle(QStringLiteral("Open a workspace folder"));
        empty_state_->setMessage(QStringLiteral(
            "Browse source files, scripts, binaries, symbols, and project artifacts in one tree."));
        empty_state_->setActionLabel(QStringLiteral("Open Folder"));
    } else if (!model_->filter().isEmpty()) {
        empty_action_ = EmptyAction::ClearFilter;
        empty_state_->setTitle(QStringLiteral("No items match the filter"));
        empty_state_->setMessage(QStringLiteral(
            "Clear the filter to show every workspace item."));
        empty_state_->setActionLabel(QStringLiteral("Clear Filter"));
    } else {
        empty_action_ = EmptyAction::None;
        empty_state_->setTitle(QStringLiteral("This workspace is empty"));
        empty_state_->setMessage(QStringLiteral(
            "Create a file or folder with the Explorer context menu, or open a different folder."));
        empty_state_->setActionLabel(QString());
    }
    empty_state_->setVisible(true);
    list_->setVisible(false);
}

void AidaExplorerView::applyContentWidth()
{
    const int estimate = AidaExplorerDelegate::estimatedRowWidth(
        model_->maxDepth(), model_->maxNameUnits(), QFontMetricsF(list_->font()));
    if (delegate_->setContentWidth(estimate))
        list_->doItemsLayout();
}

void AidaExplorerView::syncBackendSelectionFromView()
{
    if (applying_backend_selection_ || model_->structureSyncInProgress())
        return;
    const QModelIndexList selected = list_->selectionModel()->selectedIndexes();
    std::vector<int> source_rows;
    source_rows.reserve(static_cast<std::size_t>(selected.size()));
    for (const QModelIndex& index : selected) {
        const int source = model_->sourceIndex(index.row());
        if (source >= 0)
            source_rows.push_back(source);
    }
    const QModelIndex current = list_->currentIndex();
    model_->syncSelectionFromView(source_rows,
        current.isValid() ? model_->sourceIndex(current.row()) : -1);
    refreshStateStrip();
}

void AidaExplorerView::applyBackendSelectionToView()
{
    QItemSelectionModel* selection_model = list_->selectionModel();
    QItemSelection selection;
    const int rows = model_->rowCount();
    int run_start = -1;
    for (int row = 0; row <= rows; ++row) {
        const bool selected = row < rows && model_->rowSelected(row);
        if (selected && run_start < 0) {
            run_start = row;
        } else if (!selected && run_start >= 0) {
            selection.select(model_->index(run_start), model_->index(row - 1));
            run_start = -1;
        }
    }
    applying_backend_selection_ = true;
    selection_model->select(selection,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    applying_backend_selection_ = false;
    const int view_row = model_->viewRowForSource(model_->primarySourceIndex());
    if (view_row >= 0 && list_->currentIndex().row() != view_row)
        selection_model->setCurrentIndex(model_->index(view_row),
            QItemSelectionModel::NoUpdate);
}

void AidaExplorerView::onActivated(const QModelIndex& index)
{
    if (!index.isValid())
        return;
    model_->openEntry(index.row());
}

bool AidaExplorerView::eventFilter(QObject* watched, QEvent* event)
{
    if (!list_)
        return QWidget::eventFilter(watched, event);
    if (watched == list_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_F2) {
            const QModelIndex current = list_->currentIndex();
            const FileBrowserEntry* item = current.isValid()
                ? model_->entry(current.row()) : nullptr;
            if (item) {
                requestFileOperation(
                    static_cast<int>(aida::ui::explorer_views::file_operation_t::rename),
                    item->full_path, item->is_dir);
            }
            return true;
        }
        if (key->key() == Qt::Key_Delete) {
            if (!selectedTargets().empty()) {
                requestFileOperation(
                    static_cast<int>(aida::ui::explorer_views::file_operation_t::remove),
                    std::string(), false);
            }
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            if (list_->selectionModel()->hasSelection()) {
                list_->selectionModel()->clearSelection();
                return true;
            }
        }
        if (key->modifiers() == Qt::NoModifier &&
            (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right)) {
            const QModelIndex current = list_->currentIndex();
            if (current.isValid() && current.data(AidaExplorerModel::IsDirRole).toBool()) {
                const bool expanded = current.data(AidaExplorerModel::ExpandedRole).toBool();
                if ((key->key() == Qt::Key_Right && !expanded) ||
                    (key->key() == Qt::Key_Left && expanded)) {
                    model_->toggleDir(current.row());
                    return true;
                }
            }
        }
    }
    if (watched == list_ && event->type() == QEvent::ContextMenu) {
        auto* context = static_cast<QContextMenuEvent*>(event);
        if (context->reason() != QContextMenuEvent::Keyboard)
            return false;
        const QModelIndex current = list_->currentIndex();
        const auto origin = aida::ui::context_menu_open_origin_t::menu_key;
        if (current.isValid()) {
            const QRect rect = list_->visualRect(current);
            const QPoint anchor = rect.isValid() ? rect.center()
                : QPoint(theme::tokens().spacing.lg, theme::tokens().spacing.lg);
            openRowContextMenu(current.row(), list_->viewport()->mapToGlobal(anchor), origin);
        } else {
            openEmptyContextMenu(list_->viewport()->mapToGlobal(
                QPoint(theme::tokens().spacing.lg, theme::tokens().spacing.lg)), origin);
        }
        return true;
    }
    if (watched == list_ && event->type() == QEvent::FontChange) {
        applyContentWidth();
        return false;
    }
    if (watched == list_->viewport() && event->type() == QEvent::Resize) {
        applyContentWidth();
        return false;
    }
    if (watched == list_->viewport() && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            const QModelIndex index = list_->indexAt(mouse->pos());
            if (index.isValid() && index.data(AidaExplorerModel::IsDirRole).toBool()) {
                const auto& t = theme::tokens();
                const int depth = index.data(AidaExplorerModel::DepthRole).toInt();
                const QRect rect = list_->visualRect(index);
                const qreal slot_left = rect.left() + t.spacing.sm +
                    std::max(0, depth) * static_cast<qreal>(t.spacing.lg);
                const QRectF slot(slot_left, static_cast<qreal>(rect.top()),
                    static_cast<qreal>(t.spacing.lg), static_cast<qreal>(rect.height()));
                if (slot.contains(mouse->pos())) {
                    model_->toggleDir(index.row());
                    return true;
                }
            }
        }
        return false;
    }
    if (watched == root_label_ && event->type() == QEvent::Resize) {
        applyRootLabel();
        return false;
    }
    if (watched == state_label_ && event->type() == QEvent::Resize) {
        applyStateLabel();
        return false;
    }
    if (watched == filter_edit_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            if (!filter_edit_->text().isEmpty())
                filter_edit_->clear();
            list_->setFocus(Qt::OtherFocusReason);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AidaExplorerView::onCustomContextMenu(const QPoint& pos)
{
    const QModelIndex index = list_->indexAt(pos);
    const auto origin = aida::ui::context_menu_open_origin_t::pointer;
    if (index.isValid()) {
        const FileBrowserEntry* item = model_->entry(index.row());
        if (item && !model_->pathSelected(item->full_path)) {
            list_->selectionModel()->setCurrentIndex(index,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        } else {
            list_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        }
        openRowContextMenu(index.row(), list_->viewport()->mapToGlobal(pos), origin);
    } else {
        openEmptyContextMenu(list_->viewport()->mapToGlobal(pos), origin);
    }
}

void AidaExplorerView::openRowContextMenu(int row, const QPoint& global_pos,
    aida::ui::context_menu_open_origin_t origin)
{
    const int source = model_->sourceIndex(row);
    if (source < 0)
        return;
    aida::ui::application_ui::open_explorer_context_menu(source, origin);
    documents::show_context_menu(aida::ui::stable_menu_id_t("menu.explorer.entry"),
        documents::make_menu_snapshot(aida::ui::stable_view_id_t("view.project_explorer"),
            aida::ui::stable_context_type_id_t("context.explorer.entry")),
        origin, global_pos, this);
}

void AidaExplorerView::openEmptyContextMenu(const QPoint& global_pos,
    aida::ui::context_menu_open_origin_t origin)
{
    aida::ui::application_ui::open_explorer_empty_context_menu(origin);
    documents::show_context_menu(aida::ui::stable_menu_id_t("menu.explorer.empty"),
        documents::make_menu_snapshot(aida::ui::stable_view_id_t("view.project_explorer"),
            aida::ui::stable_context_type_id_t("context.explorer.empty")),
        origin, global_pos, this);
}

int AidaExplorerView::selectedSourceIndex() const
{
    const QModelIndex current = list_->currentIndex();
    if (current.isValid())
        return model_->sourceIndex(current.row());
    return -1;
}

std::vector<std::pair<std::string, bool>> AidaExplorerView::selectedTargets() const
{
    std::vector<std::pair<std::string, bool>> targets;
    for (const auto& path : file_browser::selected_paths) {
        const auto found = std::find_if(file_browser::entries.begin(), file_browser::entries.end(),
            [&path](const FileBrowserEntry& entry) {
                return explorer_path_key(entry.full_path) == path;
            });
        if (found != file_browser::entries.end())
            targets.push_back({found->full_path, found->is_dir});
    }
    if (targets.empty()) {
        const int source = selectedSourceIndex();
        if (source >= 0 && static_cast<std::size_t>(source) < file_browser::entries.size())
            targets.push_back({file_browser::entries[static_cast<std::size_t>(source)].full_path,
                file_browser::entries[static_cast<std::size_t>(source)].is_dir});
    }
    std::sort(targets.begin(), targets.end(), [](const auto& left, const auto& right) {
        return explorer_path_key(left.first) < explorer_path_key(right.first);
    });
    return targets;
}

void AidaExplorerView::requestFileOperation(int operation, const std::string& path,
    bool directory)
{
    using aida::ui::explorer_views::file_operation_t;
    const auto op = static_cast<file_operation_t>(operation);
    if (op == file_operation_t::new_file || op == file_operation_t::new_folder ||
        op == file_operation_t::rename || op == file_operation_t::duplicate) {
        std::string proposed;
        if (op == file_operation_t::rename) {
            proposed = std::filesystem::path(path).filename().string();
        } else if (op == file_operation_t::duplicate) {
            const auto source = std::filesystem::path(path);
            proposed = source.stem().string() + " copy" + source.extension().string();
        }
        showNameDialog(operation, path, directory, proposed);
        return;
    }
    if (op == file_operation_t::remove) {
        showDeleteDialog(selectedTargets());
        return;
    }
    const auto result = aida::ui::explorer_views::request_file_operation(op, path, directory);
    if (!result.accepted)
        diag::log_tagged_fmt("qt_explorer", "file_operation_rejected op=%d detail=%s",
            static_cast<int>(operation), result.detail.c_str());
}

void AidaExplorerView::showNameDialog(int operation, const std::string& source,
    bool directory, const std::string& proposed)
{
    using aida::ui::explorer_views::file_operation_t;
    const auto op = static_cast<file_operation_t>(operation);
    const char* title = op == file_operation_t::rename ? "Rename item" :
        op == file_operation_t::duplicate ? "Duplicate item" :
        op == file_operation_t::new_folder ? "Create folder" : "Create file";
    const char* confirm_label = op == file_operation_t::rename ? "Rename" :
        op == file_operation_t::duplicate ? "Duplicate" : "Create";

    auto* dialog = new bridge::AidaDialog(this);
    dialog->setWindowTitle(QString::fromLatin1(title));
    dialog->setObjectName(QStringLiteral("aida.explorer.name_dialog"));
    dialog->setModal(true);
    auto* layout = new QVBoxLayout(dialog);
    const std::string target_dir = directory &&
        (op == file_operation_t::new_file || op == file_operation_t::new_folder)
        ? source : std::filesystem::path(source).parent_path().string();
    auto* target_label = new QLabel(dialog);
    target_label->setObjectName(QStringLiteral("aida.explorer.name_target"));
    const QString target_full = QString::fromStdString(target_dir);
    const QFontMetricsF target_fm(target_label->font());
    target_label->setText(QStringLiteral("Target directory: %1").arg(target_fm.elidedText(
        target_full, Qt::ElideMiddle, qRound(target_fm.averageCharWidth() * 72.0))));
    target_label->setToolTip(target_full);
    target_label->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(target_label);
    auto* name_edit = new widgets::AidaLineEdit(dialog);
    name_edit->setObjectName(QStringLiteral("aida.explorer.name_edit"));
    name_edit->setText(QString::fromStdString(proposed));
    layout->addWidget(name_edit);
    auto* validation = new QLabel(dialog);
    validation->setObjectName(QStringLiteral("aida.explorer.name_validation"));
    validation->setProperty("aidaState", QStringLiteral("breakpoint"));
    validation->setMinimumHeight(validation->fontMetrics().height());
    layout->addWidget(validation);
    auto* buttons = new QHBoxLayout();
    auto* confirm = new widgets::AidaButton(QString::fromLatin1(confirm_label), dialog);
    confirm->setObjectName(QStringLiteral("aida.explorer.name_confirm"));
    confirm->setKind(widgets::AidaButton::Kind::Primary);
    auto* cancel = new widgets::AidaButton(QStringLiteral("Cancel"), dialog);
    cancel->setObjectName(QStringLiteral("aida.explorer.name_cancel"));
    buttons->addStretch(1);
    buttons->addWidget(confirm);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);

    const auto validate = [&] {
        std::string reason;
        const bool valid = valid_leaf_name(name_edit->text().toStdString(), reason);
        validation->setText(QString::fromStdString(reason));
        confirm->setEnabled(valid);
        return valid;
    };
    connect(name_edit, &QLineEdit::textChanged, this, [validate](const QString&) {
        validate();
    });
    validate();

    connect(confirm, &QAbstractButton::clicked, this,
        [this, dialog, name_edit, op, source, directory, validation]() {
            const std::string name = name_edit->text().toStdString();
            std::string reason;
            if (!valid_leaf_name(name, reason))
                return;
            const std::filesystem::path base = directory &&
                (op == file_operation_t::new_file || op == file_operation_t::new_folder)
                ? std::filesystem::path(source)
                : std::filesystem::path(source).parent_path();
            const auto queued = aida::ui::explorer_views::submit_confirmed_file_operation(
                op, std::filesystem::path(source), base / name, directory);
            if (queued.accepted) {
                dialog->accept();
            } else {
                validation->setText(QString::fromStdString(queued.detail));
            }
        });
    connect(name_edit, &QLineEdit::returnPressed, confirm, [confirm] {
        if (confirm->isEnabled())
            confirm->click();
    });
    connect(cancel, &QAbstractButton::clicked, dialog, &QDialog::reject);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
    name_edit->setFocus(Qt::OtherFocusReason);
    const QString proposed_text = name_edit->text();
    if (!proposed_text.isEmpty()) {
        const int dot = directory ? -1 : proposed_text.lastIndexOf(QLatin1Char('.'));
        if (dot > 0)
            name_edit->setSelection(0, dot);
        else
            name_edit->selectAll();
    }
}

void AidaExplorerView::showDeleteDialog(const std::vector<std::pair<std::string, bool>>& targets)
{
    if (targets.empty())
        return;
    const bool batch = targets.size() > 1;
    auto* dialog = new bridge::AidaDialog(this);
    dialog->setWindowTitle(batch ? QStringLiteral("Delete Workspace Items")
        : QStringLiteral("Delete Workspace Item"));
    dialog->setObjectName(QStringLiteral("aida.explorer.delete_dialog"));
    dialog->setModal(true);
    auto* layout = new QVBoxLayout(dialog);
    auto* heading = new QLabel(batch
        ? QStringLiteral("Delete these %1 workspace items permanently?")
            .arg(static_cast<qulonglong>(targets.size()))
        : QStringLiteral("Delete this workspace item permanently?"), dialog);
    heading->setObjectName(QStringLiteral("aida.explorer.delete_heading"));
    heading->setFont(theme::fonts::strong());
    layout->addWidget(heading);
    const auto elide_path = [](const QString& path, const QFont& font) {
        const QFontMetricsF fm(font);
        return fm.elidedText(path, Qt::ElideMiddle, qRound(fm.averageCharWidth() * 72.0));
    };
    if (batch) {
        QStringList preview;
        QStringList full_preview;
        const std::size_t preview_count = std::min(targets.size(), std::size_t{8});
        for (std::size_t index = 0; index < preview_count; ++index) {
            const QString full = QString::fromStdString(targets[index].first);
            preview << elide_path(full, dialog->font());
            full_preview << full;
        }
        if (targets.size() > preview_count) {
            const QString more = QStringLiteral("... and %1 more selected items")
                .arg(static_cast<qulonglong>(targets.size() - preview_count));
            preview << more;
            full_preview << more;
        }
        auto* list = new QLabel(preview.join(QStringLiteral("\n")), dialog);
        list->setObjectName(QStringLiteral("aida.explorer.delete_preview"));
        list->setToolTip(full_preview.join(QStringLiteral("\n")));
        list->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(list);
    } else {
        const QString full = QString::fromStdString(targets.front().first);
        auto* target = new QLabel(QStringLiteral("Target: %1")
            .arg(elide_path(full, dialog->font())), dialog);
        target->setObjectName(QStringLiteral("aida.explorer.delete_target"));
        target->setToolTip(full);
        target->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(target);
    }
    auto* warning = new QLabel(QStringLiteral(
        "This operation is not sent to the Recycle Bin and cannot be undone by AiDA. "
        "Every selected folder includes all of its children."), dialog);
    warning->setObjectName(QStringLiteral("aida.explorer.delete_warning"));
    warning->setWordWrap(true);
    warning->setProperty("aidaState", QStringLiteral("stale"));
    layout->addWidget(warning);
    auto* error_label = new QLabel(dialog);
    error_label->setObjectName(QStringLiteral("aida.explorer.delete_error"));
    error_label->setProperty("aidaState", QStringLiteral("breakpoint"));
    error_label->setWordWrap(true);
    error_label->setMinimumHeight(error_label->fontMetrics().height());
    layout->addWidget(error_label);
    auto* buttons = new QHBoxLayout();
    auto* confirm = new widgets::AidaButton(QStringLiteral("Delete Permanently"), dialog);
    confirm->setObjectName(QStringLiteral("aida.explorer.delete_confirm"));
    confirm->setKind(widgets::AidaButton::Kind::Destructive);
    auto* cancel = new widgets::AidaButton(QStringLiteral("Cancel"), dialog);
    cancel->setObjectName(QStringLiteral("aida.explorer.delete_cancel"));
    cancel->setFocus(Qt::OtherFocusReason);
    buttons->addStretch(1);
    buttons->addWidget(confirm);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    connect(confirm, &QAbstractButton::clicked, this, [this, dialog, targets, error_label] {
        std::vector<aida::ui::explorer_views::file_operation_target_t> operation_targets;
        for (const auto& target : targets)
            operation_targets.push_back({target.first, target.second});
        const auto queued = operation_targets.size() > 1
            ? aida::ui::explorer_views::submit_confirmed_batch_file_operation(
                aida::ui::explorer_views::file_operation_t::remove, operation_targets)
            : aida::ui::explorer_views::submit_confirmed_file_operation(
                aida::ui::explorer_views::file_operation_t::remove,
                std::filesystem::path(operation_targets.front().path), {},
                operation_targets.front().directory);
        if (queued.accepted) {
            dialog->accept();
        } else {
            error_label->setText(QString::fromStdString(queued.detail));
        }
    });
    connect(cancel, &QAbstractButton::clicked, dialog, &QDialog::reject);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

}
