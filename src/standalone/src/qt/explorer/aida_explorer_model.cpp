#include "qt/explorer/aida_explorer_model.hpp"

#include <QTimer>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>

#include "helpers/diag_log.hpp"
#include "helpers/globals.h"
#include "qt/documents/aida_document_model.hpp"

namespace aida::qt::explorer {

namespace {

std::filesystem::path path_from_utf8(std::string_view value)
{
#if defined(__cpp_char8_t)
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
#else
    return std::filesystem::u8path(value.begin(), value.end());
#endif
}

std::string path_to_utf8(const std::filesystem::path& value)
{
    const auto encoded = value.generic_u8string();
#if defined(__cpp_char8_t)
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
#else
    return encoded;
#endif
}

std::string normalized_explorer_path(const std::string& input)
{
    std::string value = path_to_utf8(path_from_utf8(input).lexically_normal());
    while (value.size() > 1 && value.back() == '/')
        value.pop_back();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool entry_matches(const FileBrowserEntry& entry, const std::string& normalized_filter)
{
    if (normalized_filter.empty())
        return true;
    return lowercase(entry.name).find(normalized_filter) != std::string::npos ||
        lowercase(entry.full_path).find(normalized_filter) != std::string::npos;
}

}

std::string explorer_path_key(const std::string& path)
{
    return normalized_explorer_path(path);
}

AidaExplorerModel::AidaExplorerModel(QObject* parent) : QAbstractListModel(parent)
{
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(250);
    connect(poll_timer_, &QTimer::timeout, this, &AidaExplorerModel::onPollTimer);
    poll_timer_->start();
}

AidaExplorerModel::~AidaExplorerModel() = default;

int AidaExplorerModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    if (applied_filter_.isEmpty())
        return static_cast<int>(entries_.size());
    return static_cast<int>(visible_indices_.size());
}

QVariant AidaExplorerModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0)
        return {};
    const FileBrowserEntry* item = entry(index.row());
    if (!item)
        return {};
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return QString::fromStdString(item->name);
    case FullPathRole:
        return QString::fromStdString(item->full_path);
    case Qt::ToolTipRole: {
        QString tooltip = QString::fromStdString(item->full_path);
        if (!item->is_dir && documents_) {
            const std::string key = explorer_path_key(item->full_path);
            for (int i = 0; i < documents_->recordCount(); ++i) {
                const OpenTab* tab = documents_->recordAt(i);
                if (!tab || explorer_path_key(tab->filepath) != key)
                    continue;
                if (tab->external_conflict)
                    tooltip += QStringLiteral("\nChanged on disk — review before saving");
                else if (tab->dirty)
                    tooltip += QStringLiteral("\nModified — unsaved changes");
                break;
            }
        }
        return tooltip;
    }
    case Qt::AccessibleTextRole: {
        QString text = QString::fromStdString(item->name);
        if (item->is_dir)
            text += item->expanded ? QStringLiteral(", folder, expanded")
                : QStringLiteral(", folder, collapsed");
        else
            text += QStringLiteral(", file");
        return text;
    }
    case IsDirRole:
        return item->is_dir;
    case DepthRole:
        return item->depth;
    case ExpandedRole:
        return item->expanded;
    case EntryIdRole:
        return QVariant::fromValue(item->entry_id);
    case IsRootRole:
        return item->is_root;
    case SelectedRole:
        return rowSelected(index.row());
    case DocumentDirtyRole:
    case DocumentConflictRole: {
        if (item->is_dir || !documents_)
            return false;
        const std::string key = explorer_path_key(item->full_path);
        for (int i = 0; i < documents_->recordCount(); ++i) {
            const OpenTab* tab = documents_->recordAt(i);
            if (!tab)
                continue;
            const std::string tab_key = explorer_path_key(tab->filepath);
            if (tab_key != key)
                continue;
            return role == DocumentDirtyRole ? tab->dirty : tab->external_conflict;
        }
        return false;
    }
    default:
        return {};
    }
}

int AidaExplorerModel::sourceIndex(int row) const
{
    if (row < 0)
        return -1;
    if (applied_filter_.isEmpty())
        return row;
    if (static_cast<std::size_t>(row) >= visible_indices_.size())
        return -1;
    return visible_indices_[static_cast<std::size_t>(row)];
}

const FileBrowserEntry* AidaExplorerModel::entry(int row) const
{
    const int source = sourceIndex(row);
    if (source < 0 || static_cast<std::size_t>(source) >= entries_.size())
        return nullptr;
    return &entries_[static_cast<std::size_t>(source)];
}

void AidaExplorerModel::refresh(const std::string& dir)
{
    file_browser::refresh(dir);
    Q_EMIT indexingStateChanged();
}

void AidaExplorerModel::cancelRefresh()
{
    file_browser::cancel_refresh();
    Q_EMIT indexingStateChanged();
}

void AidaExplorerModel::setFilter(const QString& filter)
{
    if (filter_ == filter)
        return;
    filter_ = filter;
    ++structure_sync_guard_;
    beginResetModel();
    rebuildVisibleIndices();
    endResetModel();
    --structure_sync_guard_;
    Q_EMIT indexingStateChanged();
}

void AidaExplorerModel::rebuildVisibleIndices()
{
    visible_indices_.clear();
    applied_filter_ = filter_;
    if (!filter_.isEmpty()) {
        const std::string normalized = lowercase(filter_.toStdString());
        visible_indices_.reserve(entries_.size());
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (entry_matches(entries_[index], normalized))
                visible_indices_.push_back(static_cast<int>(index));
        }
    }
    recomputeContentExtent();
}

void AidaExplorerModel::recomputeContentExtent()
{
    int depth = 0;
    std::size_t units = 0;
    if (applied_filter_.isEmpty()) {
        for (const auto& item : entries_) {
            depth = (std::max)(depth, item.depth);
            units = (std::max)(units, item.name.size());
        }
    } else {
        for (const int source : visible_indices_) {
            const auto& item = entries_[static_cast<std::size_t>(source)];
            depth = (std::max)(depth, item.depth);
            units = (std::max)(units, item.name.size());
        }
    }
    max_depth_ = depth;
    max_name_units_ = static_cast<int>((std::min)(units,
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
}

void AidaExplorerModel::toggleDir(int row)
{
    const int source = sourceIndex(row);
    if (source < 0 || static_cast<std::size_t>(source) >= entries_.size())
        return;
    if (!entries_[static_cast<std::size_t>(source)].is_dir)
        return;
    file_browser::toggle_dir(source);
    syncFromBackend();
}

void AidaExplorerModel::openEntry(int row)
{
    const int source = sourceIndex(row);
    if (source < 0 || static_cast<std::size_t>(source) >= entries_.size())
        return;
    if (entries_[static_cast<std::size_t>(source)].is_dir) {
        toggleDir(row);
        return;
    }
    file_browser::open_file(source);
}

void AidaExplorerModel::syncSelectionFromView(const std::vector<int>& source_rows,
    int current_source_row)
{
    constexpr std::size_t selection_limit = 100000;
    file_browser::selected_paths.clear();
    std::size_t applied = 0;
    bool capped = false;
    for (const int source : source_rows) {
        if (source < 0 || static_cast<std::size_t>(source) >= entries_.size())
            continue;
        const std::string key = explorer_path_key(
            entries_[static_cast<std::size_t>(source)].full_path);
        if (key.empty())
            continue;
        if (applied >= selection_limit) {
            capped = true;
            break;
        }
        file_browser::selected_paths.insert(key);
        ++applied;
    }
    if (capped || source_rows.size() > selection_limit)
        file_browser::selection_error = "Project Explorer selection is limited to 100,000 items";
    else
        file_browser::selection_error.clear();
    const bool current_valid = current_source_row >= 0 &&
        static_cast<std::size_t>(current_source_row) < entries_.size();
    file_browser::selected_idx = current_valid ? current_source_row : -1;
    file_browser::selection_anchor_path = current_valid
        ? explorer_path_key(entries_[static_cast<std::size_t>(current_source_row)].full_path)
        : std::string();
    file_browser::selection_interaction_generation = file_browser::index_generation;
    ++file_browser::selection_revision;
    applySelectionRevision();
}

bool AidaExplorerModel::rowSelected(int row) const
{
    const int source = sourceIndex(row);
    return source >= 0 && static_cast<std::size_t>(source) < selected_source_flags_.size() &&
        selected_source_flags_[static_cast<std::size_t>(source)] != 0;
}

int AidaExplorerModel::primarySourceIndex() const
{
    return file_browser::selected_idx >= 0 &&
        static_cast<std::size_t>(file_browser::selected_idx) < entries_.size()
        ? file_browser::selected_idx : -1;
}

int AidaExplorerModel::viewRowForSource(int source) const
{
    if (source < 0 || static_cast<std::size_t>(source) >= entries_.size())
        return -1;
    if (applied_filter_.isEmpty())
        return source;
    const auto found = std::find(visible_indices_.begin(), visible_indices_.end(), source);
    return found == visible_indices_.end() ? -1 :
        static_cast<int>(std::distance(visible_indices_.begin(), found));
}

bool AidaExplorerModel::pathSelected(const std::string& path) const
{
    return file_browser::selected_paths.find(explorer_path_key(path)) !=
        file_browser::selected_paths.end();
}

std::size_t AidaExplorerModel::selectionSize() const
{
    return file_browser::selected_paths.size();
}

QString AidaExplorerModel::selectionError() const
{
    return QString::fromStdString(file_browser::selection_error);
}

bool AidaExplorerModel::indexing() const
{
    return file_browser::index_state == file_browser::index_state_t::loading;
}

bool AidaExplorerModel::truncated() const
{
    return file_browser::index_truncated;
}

std::size_t AidaExplorerModel::indexedDirectoryCount() const
{
    return file_browser::indexed_directory_count;
}

std::size_t AidaExplorerModel::indexedEntryCount() const
{
    return file_browser::indexed_entry_count;
}

QString AidaExplorerModel::indexError() const
{
    return QString::fromStdString(file_browser::index_error);
}

QString AidaExplorerModel::currentRoot() const
{
    return QString::fromStdString(file_browser::current_dir);
}

bool AidaExplorerModel::hasRoot() const
{
    return !file_browser::roots.empty() || !file_browser::current_dir.empty();
}

void AidaExplorerModel::rebuildSelectedSourceFlags()
{
    selected_source_flags_.assign(entries_.size(), 0);
    for (std::size_t source = 0; source < entries_.size(); ++source) {
        if (file_browser::selected_paths.find(
                explorer_path_key(entries_[source].full_path)) != file_browser::selected_paths.end())
            selected_source_flags_[source] = 1;
    }
}

void AidaExplorerModel::applySelectionRevision()
{
    if (file_browser::selection_revision == known_selection_revision_)
        return;
    known_selection_revision_ = file_browser::selection_revision;
    rebuildSelectedSourceFlags();
    if (rowCount() > 0)
        Q_EMIT dataChanged(index(0), index(rowCount() - 1), {SelectedRole});
}

void AidaExplorerModel::onPollTimer()
{
    file_browser::tick_watcher();
    if (file_browser::needs_refresh)
        refresh();
    syncFromBackend();
    applySelectionRevision();
}

void AidaExplorerModel::syncFromBackend()
{
    if (file_browser::index_generation != known_index_generation_) {
        known_index_generation_ = file_browser::index_generation;
        known_entry_count_ = file_browser::entries.size();
        ++structure_sync_guard_;
        beginResetModel();
        entries_ = file_browser::entries;
        rebuildVisibleIndices();
        rebuildSelectedSourceFlags();
        endResetModel();
        --structure_sync_guard_;
        Q_EMIT indexingStateChanged();
        return;
    }
    if (file_browser::entries.size() != known_entry_count_) {
        const std::size_t old_count = known_entry_count_;
        const std::size_t new_count = file_browser::entries.size();
        ++structure_sync_guard_;
        if (new_count > old_count && applied_filter_.isEmpty()) {
            beginInsertRows(QModelIndex(), static_cast<int>(old_count),
                static_cast<int>(new_count) - 1);
            entries_ = file_browser::entries;
            known_entry_count_ = new_count;
            recomputeContentExtent();
            rebuildSelectedSourceFlags();
            endInsertRows();
        } else {
            beginResetModel();
            entries_ = file_browser::entries;
            known_entry_count_ = new_count;
            rebuildVisibleIndices();
            rebuildSelectedSourceFlags();
            endResetModel();
        }
        --structure_sync_guard_;
        Q_EMIT indexingStateChanged();
    }
}

}
