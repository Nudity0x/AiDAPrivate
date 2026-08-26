#pragma once

#include <QAbstractTableModel>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/analysis/workspace/analysis_workspace.hpp"

namespace aida::qt::analysis {

// Ported from xref_db_view.hpp (types verbatim).
struct qt_xref_result_t {
    aida::analysis::address_t source;
    aida::analysis::address_t target;
    aida::analysis::xref_kind_t kind = aida::analysis::xref_kind_t::code;
};

struct qt_xref_display_result_t {
    qt_xref_result_t result;
    std::uint64_t runtime = 0;
    std::string name;
    std::string label;
};

const char* qt_xref_kind_name(aida::analysis::xref_kind_t kind) noexcept;
std::optional<std::uint64_t> qt_xref_parse_address(std::string text);

// Per-binary xref view state (replaces xref_db_view::registry()).
struct QtXrefViewState {
    std::mutex mutex;
    QString address;
    QString filter;
    bool query_to = true;
    std::shared_ptr<const std::vector<qt_xref_result_t>> results;
    std::shared_ptr<const std::vector<qt_xref_display_result_t>> visible_results;
    std::uint64_t results_version = 0;
    std::uint64_t visible_version = 0;
    std::string visible_filter;
    std::atomic<bool> searching{false};
    std::atomic<bool> filtering{false};
    std::atomic<std::uint64_t> serial{1};
    std::shared_ptr<aida::analysis::cancellation_source_t> cancellation;
    std::string error;
    std::uint64_t selected_runtime = 0;
};

class QtXrefModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int {
        target = 0,
        kind = 1,
        name = 2,
        column_count = 3
    };

    explicit QtXrefModel(QObject* parent = nullptr);

    void setResults(std::shared_ptr<const std::vector<qt_xref_display_result_t>> results);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const qt_xref_display_result_t* rowAt(int view_row) const noexcept;

private:
    std::shared_ptr<const std::vector<qt_xref_display_result_t>> results_;
};

}
