#pragma once

#include <QAbstractTableModel>
#include <QString>

#include <cstdint>
#include <string>
#include <vector>

#include "core/session/analysis_session.hpp"

namespace aida::qt::workbench {

// Sessions model (07 sec. 8.4): rows from analysis_session::list_session_summaries().
class QtSessionsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int {
        name = 0,
        kind = 1,
        pid = 2,
        state = 3,
        pdb = 4,
        analysis_revision = 5,
        last_active = 6,
        column_count = 7
    };

    explicit QtSessionsModel(QObject* parent = nullptr);

    void setRows(std::vector<analysis_session::session_summary_t> rows,
                 std::size_t active_index, std::uint32_t active_driver_pid);
    const analysis_session::session_summary_t* rowAt(int row) const noexcept;
    int sessionIndexFor(int view_row) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    std::vector<analysis_session::session_summary_t> rows_;
    std::vector<int> session_indices_;
    std::size_t active_index_ = 0;
    std::uint32_t active_driver_pid_ = 0;
};

}
