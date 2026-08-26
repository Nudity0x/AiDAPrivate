#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QModelRoleDataSpan>
#include <QVariant>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace aida::qt::net {

// SnapshotTableModel holds one immutable snapshot (shared_ptr<const
// QVector<RowT>>) plus a publication generation. adopt() performs a full
// model reset; persistent indexes are invalidated by beginResetModel
// (qabstractitemmodel.cpp:3399-3434), so callers re-apply selection by their
// own stable key after adoption. multiData() is overridden because the
// delegate pulls seven roles in one call (qstyleditemdelegate.cpp:256-344)
// while the stock implementation loops data() per role
// (qabstractitemmodel.cpp:3694-3700).
template <typename RowT>
class SnapshotTableModel : public QAbstractTableModel {
public:
    using snapshot_ptr_t = std::shared_ptr<const QVector<RowT>>;

    explicit SnapshotTableModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    void adopt(snapshot_ptr_t snapshot, std::uint64_t generation) {
        beginResetModel();
        rows_ = std::move(snapshot);
        generation_ = generation;
        onRowsAdopted();
        endResetModel();
    }

    void clearRows() {
        if (rowCount() == 0 && !rows_) {
            ++generation_;
            return;
        }
        adopt(snapshot_ptr_t{}, generation_ + 1);
    }

    std::uint64_t generation() const noexcept { return generation_; }
    const RowT* rowAt(int row) const noexcept {
        if (!rows_ || row < 0 || row >= rows_->size())
            return nullptr;
        return &rows_->at(row);
    }
    const QVector<RowT>* rows() const noexcept { return rows_.get(); }
    snapshot_ptr_t snapshot() const noexcept { return rows_; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : (rows_ ? rows_->size() : 0);
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid())
            return {};
        const RowT* row = rowAt(index.row());
        if (!row)
            return {};
        return this->cellData(*row, index.column(), role);
    }

    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override {
        if (!index.isValid()) {
            for (auto& roleData : roleDataSpan)
                roleData.clearData();
            return;
        }
        const RowT* row = rowAt(index.row());
        if (!row) {
            for (auto& roleData : roleDataSpan)
                roleData.clearData();
            return;
        }
        for (auto& roleData : roleDataSpan)
            roleData.setData(this->cellData(*row, index.column(), roleData.role()));
    }

protected:
    virtual QVariant cellData(const RowT& row, int column, int role) const = 0;
    virtual void onRowsAdopted() {}

private:
    snapshot_ptr_t rows_;
    std::uint64_t generation_ = 0;
};

// RingTableModel is the append + front-trim model for high-rate streams
// (capture/dns). Each delivery tick performs ONE batched
// beginInsertRows/endInsertRows (contract qabstractitemmodel.cpp:2889-2914:
// rowsAboutToBeInserted is emitted BEFORE the data change; endInsertRows fixes
// persistent indexes) and at most ONE beginRemoveRows(0, n-1)/endRemoveRows
// front trim (qabstractitemmodel.cpp:2944-2963). Persistent indexes are
// adjusted across the removal, so a QPersistentModelIndex selection survives
// trims; a stable-key fallback is left to the pane.
//
// The filter lives inside the model (Q3 rule 5): setFilter() rebuilds the
// visible-index vector once per change; with no filter hook the visible set is
// the identity over the ring. appendBatch inserts only the batch rows that
// match the current filter, so a filtered view never materializes non-matching
// rows.
template <typename RowT>
class RingTableModel : public QAbstractTableModel {
public:
    using filter_fn_t = std::function<bool(const RowT&)>;

    explicit RingTableModel(int capacity, QObject* parent = nullptr)
        : QAbstractTableModel(parent), capacity_(capacity > 0 ? capacity : 1) {}

    int capacity() const noexcept { return capacity_; }

    void setCapacity(int capacity) {
        if (capacity < 1)
            capacity = 1;
        if (capacity == capacity_)
            return;
        capacity_ = capacity;
        trimToCapacity();
    }

    void setFilter(filter_fn_t filter) {
        filter_ = std::move(filter);
        refilter();
    }

    void refilter() {
        beginResetModel();
        rebuildVisible();
        endResetModel();
    }

    // Appends the batch and front-trims in the same tick. Returns the number
    // of rows trimmed from the front.
    int appendBatch(const QVector<RowT>& batch) {
        appendRaw(batch);
        return trimToCapacity();
    }

    int appendBatch(std::shared_ptr<const QVector<RowT>> batch) {
        if (!batch || batch->isEmpty())
            return trimToCapacity();
        return appendBatch(*batch);
    }

    int appendBatch(std::shared_ptr<const std::vector<RowT>> batch) {
        if (!batch || batch->empty())
            return trimToCapacity();
        return appendBatch(toLocalVector(*batch));
    }

    int appendBatch(std::shared_ptr<const std::vector<RowT>> batch,
                    std::size_t trimmedFromFront) {
        if (!batch || batch->empty())
            return trimToCapacity();
        appendRaw(toLocalVector(*batch));
        const int explicitTrim = trimFront(static_cast<int>(trimmedFromFront));
        return explicitTrim + trimToCapacity();
    }

    int trimFront(int count) {
        if (count <= 0)
            return 0;
        const int trim = (std::min)(count, static_cast<int>(ring_.size()));
        int removedVisible = 0;
        while (removedVisible < visible_.size() && visible_.at(removedVisible) < trim)
            ++removedVisible;
        if (removedVisible > 0)
            beginRemoveRows(QModelIndex(), 0, removedVisible - 1);
        ring_.remove(0, trim);
        for (int& entry : visible_)
            entry -= trim;
        visible_.remove(0, removedVisible);
        if (removedVisible > 0)
            endRemoveRows();
        return trim;
    }

    int trimToCapacity() {
        const int excess = ring_.size() - capacity_;
        if (excess <= 0)
            return 0;
        return trimFront(excess);
    }

    void clearRows() {
        if (ring_.isEmpty() && visible_.isEmpty())
            return;
        beginResetModel();
        ring_.clear();
        visible_.clear();
        endResetModel();
    }

    const RowT* visibleRowAt(int row) const noexcept {
        if (row < 0 || row >= visible_.size())
            return nullptr;
        const int ringIndex = visible_.at(row);
        if (ringIndex < 0 || ringIndex >= ring_.size())
            return nullptr;
        return &ring_.at(ringIndex);
    }
    const RowT* ringAt(int ringIndex) const noexcept {
        if (ringIndex < 0 || ringIndex >= ring_.size())
            return nullptr;
        return &ring_.at(ringIndex);
    }
    int ringCount() const noexcept { return ring_.size(); }
    const QVector<RowT>& ringRef() const noexcept { return ring_; }
    std::uint64_t totalAppended() const noexcept { return total_appended_; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : visible_.size();
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid())
            return {};
        const RowT* row = visibleRowAt(index.row());
        if (!row)
            return {};
        return this->cellData(*row, index.column(), role);
    }

    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override {
        if (!index.isValid()) {
            for (auto& roleData : roleDataSpan)
                roleData.clearData();
            return;
        }
        const RowT* row = visibleRowAt(index.row());
        if (!row) {
            for (auto& roleData : roleDataSpan)
                roleData.clearData();
            return;
        }
        for (auto& roleData : roleDataSpan)
            roleData.setData(this->cellData(*row, index.column(), roleData.role()));
    }

protected:
    virtual QVariant cellData(const RowT& row, int column, int role) const = 0;

private:
    static QVector<RowT> toLocalVector(const std::vector<RowT>& batch) {
        QVector<RowT> local;
        local.reserve(static_cast<qsizetype>(batch.size()));
        for (const auto& row : batch)
            local.push_back(row);
        return local;
    }

    void appendRaw(const QVector<RowT>& batch) {
        if (batch.isEmpty())
            return;
        const int ringBase = ring_.size();
        QVector<int> newVisible;
        newVisible.reserve(batch.size());
        for (qsizetype i = 0; i < batch.size(); ++i) {
            ring_.push_back(batch.at(i));
            if (matchesFilter(ring_.constLast()))
                newVisible.push_back(ringBase + static_cast<int>(i));
        }
        total_appended_ += static_cast<std::uint64_t>(batch.size());
        if (!newVisible.isEmpty()) {
            beginInsertRows(QModelIndex(), visible_.size(),
                visible_.size() + newVisible.size() - 1);
            visible_ += newVisible;
            endInsertRows();
        }
    }

    bool matchesFilter(const RowT& row) const {
        return !filter_ || filter_(row);
    }

    void rebuildVisible() {
        visible_.clear();
        visible_.reserve(ring_.size());
        for (int i = 0; i < ring_.size(); ++i) {
            if (matchesFilter(ring_.at(i)))
                visible_.push_back(i);
        }
    }

    QVector<RowT> ring_;
    QVector<int> visible_;
    int capacity_ = 1;
    filter_fn_t filter_;
    std::uint64_t total_appended_ = 0;
};

}
