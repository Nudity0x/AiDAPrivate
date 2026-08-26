#pragma once

#include <QAbstractTableModel>
#include <QString>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/analysis/struct_dissector.hpp"

namespace aida::qt::analysis {

// Structure list model (left pane of the dissector).
class QtDissectorStructureModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit QtDissectorStructureModel(QObject* parent = nullptr);

    void syncFromEngine(const QString& filter_lower);
    int engineIndexFor(int view_row) const noexcept;
    int activeStruct() const noexcept { return active_struct_; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override;

private:
    std::vector<std::pair<std::string, std::uint32_t>> entries_;
    std::vector<int> entry_indices_;
    int active_struct_ = -1;
};

// Field table model (right pane). Adaptive column visibility is view-side.
class QtDissectorFieldModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int {
        offset = 0,
        name = 1,
        type = 2,
        value = 3,
        description = 4,
        column_count = 5
    };

    explicit QtDissectorFieldModel(QObject* parent = nullptr);

    void syncFromEngine();
    void syncValues();

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const struct_dissector::field_def_t* fieldAt(int row) const noexcept;
    const struct_dissector::live_value_t* valueAt(int row) const noexcept;

private:
    std::vector<struct_dissector::field_def_t> fields_;
    std::vector<struct_dissector::live_value_t> values_;
};

}
