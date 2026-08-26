#pragma once

#include <QAbstractListModel>
#include <QTimer>

#include <cstdint>
#include <string>
#include <vector>

struct FileBrowserEntry;

namespace aida::qt::documents {
class AidaDocumentModel;
}

namespace aida::qt::explorer {

class AidaExplorerModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        NameRole = Qt::UserRole,
        FullPathRole,
        IsDirRole,
        DepthRole,
        ExpandedRole,
        EntryIdRole,
        IsRootRole,
        SelectedRole,
        DocumentDirtyRole,
        DocumentConflictRole
    };

    explicit AidaExplorerModel(QObject* parent = nullptr);
    ~AidaExplorerModel() override;

    void setDocumentModel(documents::AidaDocumentModel* model) noexcept { documents_ = model; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    int sourceIndex(int row) const;
    const FileBrowserEntry* entry(int row) const;

    void refresh(const std::string& dir = std::string{});
    void cancelRefresh();
    void setFilter(const QString& filter);
    QString filter() const { return filter_; }
    void toggleDir(int row);
    void openEntry(int row);

    void syncSelectionFromView(const std::vector<int>& source_rows, int current_source_row);
    bool pathSelected(const std::string& path) const;
    bool rowSelected(int row) const;
    int primarySourceIndex() const;
    int viewRowForSource(int source) const;
    bool structureSyncInProgress() const { return structure_sync_guard_ > 0; }
    std::size_t selectionSize() const;
    QString selectionError() const;

    bool indexing() const;
    bool truncated() const;
    std::size_t indexedDirectoryCount() const;
    std::size_t indexedEntryCount() const;
    QString indexError() const;
    QString currentRoot() const;
    bool hasRoot() const;
    int maxDepth() const { return max_depth_; }
    int maxNameUnits() const { return max_name_units_; }

Q_SIGNALS:
    void indexingStateChanged();

private Q_SLOTS:
    void onPollTimer();

private:
    void syncFromBackend();
    void applySelectionRevision();
    void rebuildSelectedSourceFlags();
    void rebuildVisibleIndices();
    void recomputeContentExtent();

    std::vector<FileBrowserEntry> entries_;
    std::vector<int> visible_indices_;
    std::vector<char> selected_source_flags_;
    QString filter_;
    QString applied_filter_;
    std::uint64_t known_index_generation_ = 0;
    std::size_t known_entry_count_ = 0;
    std::uint64_t known_selection_revision_ = 0;
    int structure_sync_guard_ = 0;
    int max_depth_ = 0;
    int max_name_units_ = 0;
    QTimer* poll_timer_ = nullptr;
    documents::AidaDocumentModel* documents_ = nullptr;
};

std::string explorer_path_key(const std::string& path);

}
