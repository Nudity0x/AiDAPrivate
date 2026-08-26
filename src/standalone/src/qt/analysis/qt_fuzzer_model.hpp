#pragma once

#include <QAbstractTableModel>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/analysis/fuzzer_engine.hpp"

namespace aida::qt::analysis {

// Crash catalog model (07 sec. 7.5). Reads the engine's published render snapshot;
// the view feeds it on the 66 ms timer.
class QtFuzzerCrashModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int {
        index = 0,
        score = 1,
        type = 2,
        address = 3,
        instruction = 4,
        description = 5,
        column_count = 6
    };

    explicit QtFuzzerCrashModel(QObject* parent = nullptr);

    void setSnapshot(
        std::shared_ptr<const fuzzer_engine::render_snapshot_t> snapshot);
    const fuzzer_engine::crash_info_t* rowAt(int row) const noexcept;
    std::uint64_t snapshot_generation() const noexcept {
        return snapshot_ ? snapshot_->generation : 0;
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    std::shared_ptr<const fuzzer_engine::render_snapshot_t> snapshot_;
};

}
