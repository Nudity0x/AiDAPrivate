#include "qt/programming/aida_workspace_search_view.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "helpers/globals.h"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/programming/programming_host_hooks.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::programming {
namespace {

constexpr std::size_t k_result_cap = 500;

QString path_leaf(const std::string& path) {
    const auto pos = path.find_last_of("\\/");
    return QString::fromStdString(pos == std::string::npos ? path : path.substr(pos + 1));
}

void open_search_result(const workspace_search::match_result_t& result) {
    file_tabs::request_document_open(result.filepath, path_leaf(result.filepath).toStdString(),
        (std::max)(0, result.line_number - 1), (std::max)(0, result.col_start));
    host::open_or_focus_view("document.code");
}

bool keyboard_context_menu_event(QObject* watched, QEvent* event, QAbstractItemView* view,
        QModelIndex* index, QPoint* global_pos) {
    if (!view)
        return false;
    if (event->type() != QEvent::ContextMenu)
        return false;
    auto* context_event = static_cast<QContextMenuEvent*>(event);
    if (context_event->reason() != QContextMenuEvent::Keyboard)
        return false;
    if (watched != view && watched != view->viewport())
        return false;
    *index = view->currentIndex();
    *global_pos = index->isValid()
        ? view->viewport()->mapToGlobal(view->visualRect(*index).center())
        : view->viewport()->mapToGlobal(view->viewport()->rect().center());
    return true;
}

class AidaSearchResultDelegate : public QStyledItemDelegate {
public:
    AidaSearchResultDelegate(AidaWorkspaceSearchModel* model, QObject* parent)
        : QStyledItemDelegate(parent), model_(model) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto* match = model_->matchAt(index);
        if (!match) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        auto* style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const auto& tokens = theme::tokens();
        const QString prefix = QString::number(match->line_number) + QStringLiteral(": ");
        const QString body = QString::fromStdString(match->line_text);
        const QFontMetricsF fm(opt.font);
        const QRectF text_rect = style->subElementRect(
            QStyle::SE_ItemViewItemText, &opt, opt.widget);
        const qreal baseline = widgets::text_baseline_centered(text_rect, fm);
        painter->save();
        painter->setFont(opt.font);
        painter->setPen(opt.palette.color(QPalette::Text));
        qreal x = text_rect.left();
        painter->drawText(QPointF(x, baseline), prefix);
        x += fm.horizontalAdvance(prefix);

        const qreal available = text_rect.right() - x;
        if (available <= 0.0) {
            painter->restore();
            return;
        }
        QString shown = body;
        qsizetype head = body.size();
        if (fm.horizontalAdvance(body) > available) {
            shown = fm.elidedText(body, Qt::ElideRight, available);
            if (shown.size() < body.size())
                head = (std::max)(qsizetype{0}, shown.size() - 1);
        }
        const auto clamp_col = [](int col) {
            return static_cast<qsizetype>((std::max)(0, col));
        };
        const qsizetype match_from = (std::min)(clamp_col(match->col_start),
            static_cast<qsizetype>(head));
        const qsizetype match_to = (std::min)((std::max)(match_from,
            clamp_col(match->col_end)), static_cast<qsizetype>(head));

        const auto draw_segment = [&](qsizetype from, qsizetype to, bool highlight) {
            if (to <= from)
                return;
            const QString segment = shown.mid(from, to - from);
            const qreal width = fm.horizontalAdvance(segment);
            if (highlight) {
                painter->fillRect(QRectF(x, text_rect.top(), width, text_rect.height()),
                    widgets::with_alpha(tokens.warning, 0.30));
                QFont bold = opt.font;
                bold.setBold(true);
                painter->setFont(bold);
                painter->setPen(tokens.text_primary);
                painter->drawText(QPointF(x, baseline), segment);
                painter->setFont(opt.font);
                painter->setPen(opt.palette.color(QPalette::Text));
            } else {
                painter->drawText(QPointF(x, baseline), segment);
            }
            x += width;
        };
        draw_segment(0, match_from, false);
        draw_segment(match_from, match_to, true);
        draw_segment(match_to, shown.size(), false);
        painter->restore();
    }

private:
    AidaWorkspaceSearchModel* model_ = nullptr;
};

} 

AidaWorkspaceSearchModel::AidaWorkspaceSearchModel(QObject* parent)
    : QAbstractItemModel(parent) {
}

void AidaWorkspaceSearchModel::reset() {
    beginResetModel();
    results_.clear();
    groups_.clear();
    generation_ = workspace_search::g_search.generation.load(std::memory_order_acquire);
    endResetModel();
}

void AidaWorkspaceSearchModel::pollOnce() {
    auto& state = workspace_search::g_search;
    const std::uint64_t generation = state.generation.load(std::memory_order_acquire);
    if (generation != generation_) {
        std::vector<workspace_search::match_result_t> copied;
        {
            std::unique_lock<std::mutex> lock(state.results_mtx, std::try_to_lock);
            if (!lock.owns_lock()) {
                diag::log_tagged("ui", "WORKSPACE_SEARCH_SNAPSHOT_BUSY source=generation_switch");
                return;
            }
            const std::size_t count = (std::min)(state.results.size(), k_result_cap);
            copied.assign(state.results.begin(), state.results.begin() + count);
        }
        beginResetModel();
        results_ = std::move(copied);
        groups_.clear();
        generation_ = generation;
        std::size_t cursor = 0;
        while (cursor < results_.size()) {
            const QString path = QString::fromStdString(results_[cursor].filepath);
            group_t group;
            group.path = path;
            group.first_match = static_cast<int>(cursor);
            std::size_t end = cursor + 1;
            while (end < results_.size() &&
                   results_[end].filepath == results_[cursor].filepath)
                ++end;
            group.match_count = static_cast<int>(end - cursor);
            group.label = path_leaf(results_[cursor].filepath) + QStringLiteral(" (") +
                QString::number(group.match_count) + QStringLiteral(")");
            groups_.push_back(std::move(group));
            cursor = end;
        }
        endResetModel();
        return;
    }
    std::size_t new_results = 0;
    {
        std::unique_lock<std::mutex> lock(state.results_mtx, std::try_to_lock);
        if (!lock.owns_lock()) {
            diag::log_tagged("ui", "WORKSPACE_SEARCH_SNAPSHOT_BUSY source=incremental_append");
            return;
        }
        const std::size_t count = (std::min)(state.results.size(), k_result_cap);
        if (count > results_.size()) {
            new_results = count - results_.size();
            results_.insert(results_.end(), state.results.begin() + results_.size(),
                state.results.begin() + count);
        }
    }
    if (new_results == 0)
        return;
    const std::size_t total = results_.size();
    std::size_t cursor = total - new_results;
    while (cursor < total) {
        const std::string& path = results_[cursor].filepath;
        if (!groups_.empty() &&
            groups_.back().path == QString::fromStdString(path)) {
            auto& last = groups_.back();
            const int group_index = static_cast<int>(groups_.size()) - 1;
            const int insert_at = last.match_count;
            const int added = static_cast<int>(total - cursor);
            const QModelIndex parent_index = createIndex(group_index, 0,
                static_cast<quintptr>(group_index + 1));
            beginInsertRows(parent_index, insert_at, insert_at + added - 1);
            last.match_count += added;
            last.label = path_leaf(path) + QStringLiteral(" (") +
                QString::number(last.match_count) + QStringLiteral(")");
            endInsertRows();
            cursor = total;
            const QModelIndex label_index = createIndex(group_index, 0,
                static_cast<quintptr>(group_index + 1));
            Q_EMIT dataChanged(label_index, label_index, {Qt::DisplayRole, Qt::ToolTipRole});
            continue;
        }
        group_t group;
        group.path = QString::fromStdString(path);
        group.first_match = static_cast<int>(cursor);
        std::size_t end = cursor + 1;
        while (end < total && results_[end].filepath == path)
            ++end;
        group.match_count = static_cast<int>(end - cursor);
        group.label = path_leaf(path) + QStringLiteral(" (") +
            QString::number(group.match_count) + QStringLiteral(")");
        const int group_index = static_cast<int>(groups_.size());
        beginInsertRows(QModelIndex(), group_index, group_index);
        groups_.push_back(std::move(group));
        endInsertRows();
        Q_EMIT groupsAppended(group_index);
        cursor = end;
    }
}

QModelIndex AidaWorkspaceSearchModel::index(int row, int column,
        const QModelIndex& parent) const {
    if (column != 0 || row < 0)
        return {};
    if (!parent.isValid()) {
        if (row >= static_cast<int>(groups_.size()))
            return {};
        return createIndex(row, 0, static_cast<quintptr>(row + 1));
    }
    if (!isGroup(parent))
        return {};
    const int group_index = static_cast<int>(parent.internalId()) - 1;
    if (group_index < 0 || group_index >= static_cast<int>(groups_.size()))
        return {};
    if (row >= groups_[static_cast<std::size_t>(group_index)].match_count)
        return {};
    return createIndex(row, 0,
        (static_cast<quintptr>(group_index + 1) << 32) | static_cast<quintptr>(row + 1));
}

QModelIndex AidaWorkspaceSearchModel::parent(const QModelIndex& child) const {
    if (!child.isValid() || isGroup(child))
        return {};
    const quintptr encoded = child.internalId();
    const int group_index = static_cast<int>((encoded >> 32) & 0x7FFFFFFFu) - 1;
    if (group_index < 0 || group_index >= static_cast<int>(groups_.size()))
        return {};
    return createIndex(group_index, 0, static_cast<quintptr>(group_index + 1));
}

int AidaWorkspaceSearchModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid())
        return static_cast<int>(groups_.size());
    if (!isGroup(parent))
        return 0;
    const int group_index = static_cast<int>(parent.internalId()) - 1;
    if (group_index < 0 || group_index >= static_cast<int>(groups_.size()))
        return 0;
    return groups_[static_cast<std::size_t>(group_index)].match_count;
}

int AidaWorkspaceSearchModel::columnCount(const QModelIndex& parent) const {
    static_cast<void>(parent);
    return 1;
}

bool AidaWorkspaceSearchModel::isGroup(const QModelIndex& index) const {
    return index.isValid() && (index.internalId() >> 32) == 0;
}

const workspace_search::match_result_t* AidaWorkspaceSearchModel::matchAt(
        const QModelIndex& index) const {
    if (!index.isValid() || isGroup(index))
        return nullptr;
    const quintptr encoded = index.internalId();
    const int group_index = static_cast<int>((encoded >> 32) & 0x7FFFFFFFu) - 1;
    const int match_row = static_cast<int>(encoded & 0xFFFFFFFFu) - 1;
    if (group_index < 0 || group_index >= static_cast<int>(groups_.size()))
        return nullptr;
    const auto& group = groups_[static_cast<std::size_t>(group_index)];
    if (match_row < 0 || match_row >= group.match_count)
        return nullptr;
    const int source = group.first_match + match_row;
    if (source < 0 || source >= static_cast<int>(results_.size()))
        return nullptr;
    return &results_[static_cast<std::size_t>(source)];
}

int AidaWorkspaceSearchModel::sourceIndexAt(const QModelIndex& index) const {
    if (!index.isValid() || isGroup(index))
        return -1;
    const quintptr encoded = index.internalId();
    const int group_index = static_cast<int>((encoded >> 32) & 0x7FFFFFFFu) - 1;
    const int match_row = static_cast<int>(encoded & 0xFFFFFFFFu) - 1;
    if (group_index < 0 || group_index >= static_cast<int>(groups_.size()))
        return -1;
    const auto& group = groups_[static_cast<std::size_t>(group_index)];
    if (match_row < 0 || match_row >= group.match_count)
        return -1;
    return group.first_match + match_row;
}

QVariant AidaWorkspaceSearchModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    if (isGroup(index)) {
        const int group_index = static_cast<int>(index.internalId()) - 1;
        if (group_index < 0 || group_index >= static_cast<int>(groups_.size()))
            return {};
        const auto& group = groups_[static_cast<std::size_t>(group_index)];
        if (role == Qt::DisplayRole)
            return group.label;
        if (role == Qt::ToolTipRole)
            return group.path;
        return {};
    }
    const auto* match = matchAt(index);
    if (!match)
        return {};
    if (role == Qt::DisplayRole)
        return QString::number(match->line_number) + QStringLiteral(": ") +
            QString::fromStdString(match->line_text);
    if (role == Qt::ToolTipRole)
        return QString::fromStdString(match->filepath);
    return {};
}

AidaWorkspaceSearchView::AidaWorkspaceSearchView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.workspace_search"));
    const auto& tokens = theme::tokens();
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(tokens.spacing.sm, tokens.spacing.xs,
        tokens.spacing.sm, tokens.spacing.xs);
    column->setSpacing(tokens.spacing.xs);

    auto* scope_row = new QHBoxLayout();
    scope_label_ = new QLabel(this);
    scope_label_->setObjectName(QStringLiteral("aida.view.workspace_search.scope"));
    scope_label_->setEnabled(false);
    scope_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    scope_row->addWidget(scope_label_, 1);
    clear_scope_button_ = new QPushButton(QStringLiteral("Clear Scope"), this);
    clear_scope_button_->setObjectName(QStringLiteral("aida.view.workspace_search.clear_scope"));
    clear_scope_button_->setToolTip(QStringLiteral(
        "Stop restricting the search to the selected directory"));
    scope_row->addWidget(clear_scope_button_);
    column->addLayout(scope_row);

    query_edit_ = new QLineEdit(this);
    query_edit_->setObjectName(QStringLiteral("aida.view.workspace_search.query"));
    query_edit_->setPlaceholderText(QStringLiteral("Search workspace"));
    query_edit_->setToolTip(QStringLiteral(
        "Text or pattern to find across the workspace (Enter to search)"));
    query_edit_->setClearButtonEnabled(true);
    column->addWidget(query_edit_);

    auto* options_row = new QHBoxLayout();
    case_box_ = new QCheckBox(QStringLiteral("Match case"), this);
    case_box_->setObjectName(QStringLiteral("aida.view.workspace_search.case"));
    word_box_ = new QCheckBox(QStringLiteral("Whole word"), this);
    word_box_->setObjectName(QStringLiteral("aida.view.workspace_search.whole_word"));
    regex_box_ = new QCheckBox(QStringLiteral("Regex"), this);
    regex_box_->setObjectName(QStringLiteral("aida.view.workspace_search.regex"));
    options_row->addWidget(case_box_);
    options_row->addWidget(word_box_);
    options_row->addWidget(regex_box_);
    options_row->addStretch(1);
    column->addLayout(options_row);

    include_edit_ = new QLineEdit(this);
    include_edit_->setObjectName(QStringLiteral("aida.view.workspace_search.include"));
    include_edit_->setPlaceholderText(QStringLiteral("Files to include, for example *.cpp,*.h"));
    include_edit_->setToolTip(QStringLiteral(
        "Comma-separated wildcard filters limiting which files are searched"));
    include_edit_->setClearButtonEnabled(true);
    column->addWidget(include_edit_);
    exclude_edit_ = new QLineEdit(this);
    exclude_edit_->setObjectName(QStringLiteral("aida.view.workspace_search.exclude"));
    exclude_edit_->setPlaceholderText(QStringLiteral("Files to exclude, for example build,*_generated.h"));
    exclude_edit_->setToolTip(QStringLiteral(
        "Comma-separated wildcard filters excluding files from the search"));
    exclude_edit_->setClearButtonEnabled(true);
    column->addWidget(exclude_edit_);

    auto* action_row = new QHBoxLayout();
    search_button_ = new QPushButton(QStringLiteral("Search"), this);
    search_button_->setObjectName(QStringLiteral("aida.view.workspace_search.search"));
    search_button_->setToolTip(QStringLiteral("Start the workspace search (Enter)"));
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_button_->setObjectName(QStringLiteral("aida.view.workspace_search.cancel"));
    cancel_button_->setToolTip(QStringLiteral(
        "Cancel the running search (Esc while the results view is focused)"));
    action_row->addWidget(search_button_);
    action_row->addWidget(cancel_button_);
    action_row->addStretch(1);
    column->addLayout(action_row);

    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.view.workspace_search.status"));
    status_label_->setEnabled(false);
    column->addWidget(status_label_);
    cap_label_ = new QLabel(this);
    cap_label_->setObjectName(QStringLiteral("aida.view.workspace_search.cap"));
    cap_label_->setEnabled(false);
    column->addWidget(cap_label_);

    model_ = new AidaWorkspaceSearchModel(this);
    tree_ = new QTreeView(this);
    tree_->setObjectName(QStringLiteral("aida.view.workspace_search.tree"));
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setRootIsDecorated(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setModel(model_);
    tree_->setFont(theme::fonts::codeRegular());
    tree_->setItemDelegate(new AidaSearchResultDelegate(model_, tree_));
    tree_->installEventFilter(this);
    tree_->viewport()->installEventFilter(this);
    tree_->setToolTip(QStringLiteral(
        "Search results grouped by file; Enter or click opens the match"));
    column->addWidget(tree_, 1);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.view.workspace_search.state"));
    state_view_->setVisible(false);
    column->addWidget(state_view_, 1);

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(100);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, &AidaWorkspaceSearchView::onPollTick);

    connect(query_edit_, &QLineEdit::returnPressed, this, &AidaWorkspaceSearchView::startSearch);
    connect(search_button_, &QPushButton::clicked, this, &AidaWorkspaceSearchView::startSearch);
    connect(cancel_button_, &QPushButton::clicked, this, [] {
        workspace_search::g_search.cancel.store(true, std::memory_order_release);
    });
    connect(clear_scope_button_, &QPushButton::clicked, this, [this] {
        host::clear_workspace_search_scope();
        refreshStatus();
    });
    connect(model_, &AidaWorkspaceSearchModel::groupsAppended, this, [this](int) {
        for (int row = 0; row < model_->rowCount(); ++row) {
            const QModelIndex group = model_->index(row, 0);
            const QString path = group.data(Qt::ToolTipRole).toString();
            tree_->setExpanded(group, !collapsed_paths_.contains(path));
        }
    });
    connect(model_, &QAbstractItemModel::modelReset, this, [this] {
        for (int row = 0; row < model_->rowCount(); ++row) {
            const QModelIndex group = model_->index(row, 0);
            const QString path = group.data(Qt::ToolTipRole).toString();
            tree_->setExpanded(group, !collapsed_paths_.contains(path));
        }
    });
    connect(tree_, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
        const QString path = index.data(Qt::ToolTipRole).toString();
        if (!path.isEmpty())
            collapsed_paths_.insert(path);
    });
    connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex& index) {
        collapsed_paths_.remove(index.data(Qt::ToolTipRole).toString());
    });
    const auto open_match = [this](const QModelIndex& index) {
        const auto* match = model_->matchAt(index);
        if (match) {
            workspace_search::g_search.selected_idx = model_->sourceIndexAt(index);
            open_search_result(*match);
        }
    };
    connect(tree_, &QTreeView::clicked, this, open_match);
    connect(tree_, &QTreeView::activated, this, open_match);
    connect(tree_, &QTreeView::customContextMenuRequested, this, [this](const QPoint& pos) {
        openResultContext(tree_->indexAt(pos),
            aida::ui::context_menu_open_origin_t::pointer,
            tree_->viewport()->mapToGlobal(pos));
    });
    model_->reset();
    refreshStatus();
}

void AidaWorkspaceSearchView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshStatus();
    model_->pollOnce();
}

void AidaWorkspaceSearchView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!scope_path_.isEmpty() && scope_label_->isVisible()) {
        const int available = scope_label_->width();
        if (available > 0) {
            scope_label_->setText(QStringLiteral("Scope: %1").arg(
                scope_label_->fontMetrics().elidedText(scope_path_, Qt::ElideMiddle,
                    available)));
        }
    }
}

bool AidaWorkspaceSearchView::eventFilter(QObject* watched, QEvent* event) {
    QModelIndex index;
    QPoint global_pos;
    if (keyboard_context_menu_event(watched, event, tree_, &index, &global_pos)) {
        openResultContext(index, aida::ui::context_menu_open_origin_t::menu_key, global_pos);
        return true;
    }
    if ((watched == tree_ || watched == tree_->viewport()) &&
        event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape &&
            workspace_search::g_search.searching.load(std::memory_order_acquire)) {
            workspace_search::g_search.cancel.store(true, std::memory_order_release);
            refreshStatus();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AidaWorkspaceSearchView::startSearch() {
    auto& state = workspace_search::g_search;
    const std::string query = query_edit_->text().toStdString();
    std::memset(state.query_buf, 0, sizeof(state.query_buf));
    std::strncpy(state.query_buf, query.c_str(), sizeof(state.query_buf) - 1);
    const std::string include = include_edit_->text().toStdString();
    std::memset(state.include_buf, 0, sizeof(state.include_buf));
    std::strncpy(state.include_buf, include.c_str(), sizeof(state.include_buf) - 1);
    const std::string exclude = exclude_edit_->text().toStdString();
    std::memset(state.exclude_buf, 0, sizeof(state.exclude_buf));
    std::strncpy(state.exclude_buf, exclude.c_str(), sizeof(state.exclude_buf) - 1);
    state.case_sensitive = case_box_->isChecked();
    state.whole_word = word_box_->isChecked();
    state.use_regex = regex_box_->isChecked();
    const auto scope = host::workspace_search_scope();
    if (!scope.empty())
        workspace_search::start_search(scope);
    else if (!file_browser::roots.empty())
        workspace_search::start_search(file_browser::roots);
    else
        workspace_search::start_search(file_browser::current_dir);
    collapsed_paths_.clear();
    if (!poll_timer_->isActive())
        poll_timer_->start();
    onPollTick();
}

void AidaWorkspaceSearchView::onPollTick() {
    model_->pollOnce();
    refreshStatus();
    if (!workspace_search::g_search.searching.load(std::memory_order_acquire) &&
        poll_timer_->isActive()) {
        model_->pollOnce();
        poll_timer_->stop();
    }
}

void AidaWorkspaceSearchView::refreshStatus() {
    auto& state = workspace_search::g_search;
    const bool searching = state.searching.load(std::memory_order_acquire);
    const bool truncated = state.truncated.load(std::memory_order_acquire);
    const int files = state.files_scanned.load(std::memory_order_acquire);
    const int matches = state.match_count.load(std::memory_order_acquire);
    cancel_button_->setVisible(searching);
    search_button_->setVisible(!searching);
    if (searching)
        status_label_->setText(QStringLiteral("Searching... %1 files, %2 matches")
            .arg(files).arg(matches));
    else
        status_label_->clear();
    const auto scope = host::workspace_search_scope();
    scope_label_->setVisible(!scope.empty());
    clear_scope_button_->setVisible(!scope.empty());
    if (!scope.empty()) {
        scope_path_ = QString::fromStdString(scope.front());
        const int available = scope_label_->width();
        const QString display = available > 0
            ? scope_label_->fontMetrics().elidedText(scope_path_, Qt::ElideMiddle, available)
            : scope_path_;
        scope_label_->setText(QStringLiteral("Scope: %1").arg(display));
        scope_label_->setToolTip(scope_path_);
    } else {
        scope_path_.clear();
        scope_label_->setToolTip(QString());
    }
    const bool capped = matches > static_cast<int>(k_result_cap);
    QString cap_text;
    if (capped)
        cap_text = QStringLiteral("(first %1 shown)").arg(k_result_cap);
    if (truncated)
        cap_text += QStringLiteral(" (bounded search limit reached)");
    cap_label_->setVisible(!cap_text.isEmpty());
    cap_label_->setText(cap_text.trimmed());
    const bool empty = model_->rowCount() == 0;
    tree_->setVisible(!empty);
    state_view_->setVisible(empty);
    if (empty) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        if (searching) {
            state_view_->setTitle(QStringLiteral("Searching"));
            state_view_->setMessage(QStringLiteral(
                "Results will appear as matching files are scanned."));
        } else if (query_edit_->text().isEmpty()) {
            state_view_->setTitle(QStringLiteral("Workspace Search"));
            state_view_->setMessage(QStringLiteral("Enter text to search the open workspace."));
        } else if (truncated) {
            state_view_->setTitle(QStringLiteral("No matches"));
            state_view_->setMessage(QStringLiteral(
                "No matches were found before the bounded workspace-search limit was reached."));
        } else {
            state_view_->setTitle(QStringLiteral("No matches"));
            state_view_->setMessage(QStringLiteral("No matches were found."));
        }
    }
}

void AidaWorkspaceSearchView::openResultContext(const QModelIndex& index,
        aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos) {
    const int source = model_->sourceIndexAt(index);
    if (source < 0)
        return;
    workspace_search::g_search.selected_idx = source;
    aida::ui::application_ui::open_workspace_search_context_menu(source, origin);
    documents::show_context_menu(
        aida::ui::stable_menu_id_t("menu.workspace_search.result"),
        documents::make_menu_snapshot(
            aida::ui::stable_view_id_t("view.workspace_search"),
            aida::ui::stable_context_type_id_t("context.workspace_search.result")),
        origin, global_pos, this);
}

}
