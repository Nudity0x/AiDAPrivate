#pragma once

#include <QAbstractTableModel>
#include <QColor>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/disasm/disasm_view.hpp"

namespace aida::qt::analysis {

// Ported verbatim from analysis_list_views (engine-side projection semantics).
enum class analysis_list_domain_t : std::uint8_t {
    imports,
    exports,
    names,
    strings,
    segments,
    local_types
};

constexpr std::size_t analysis_list_domain_count = 6;

struct analysis_list_row_t {
    std::uint64_t address = 0;
    bool has_address = false;
    std::string name;
    std::string context;
    std::string detail;
};

std::string analysis_list_row_identity(const analysis_list_row_t& row);
std::string analysis_list_address_text(std::uint64_t address);

struct analysis_list_snapshot_t {
    std::vector<analysis_list_row_t> rows;
    std::vector<std::size_t> visible;
};

struct analysis_list_descriptor_t {
    const char* stable_id;
    const char* title;
    const char* empty_title;
    const char* empty_body;
};

const analysis_list_descriptor_t& analysis_list_descriptor(analysis_list_domain_t domain) noexcept;

// Engine-adjacent projection stage; runs on executor workers (unchanged logic
// from analysis_list_views::project_rows / compute_visible).
std::vector<analysis_list_row_t> analysis_list_project_rows(
    analysis_list_domain_t domain,
    const disasm_view::workspace_context_t& context,
    const std::optional<std::uint64_t>& display_base);

std::vector<std::size_t> analysis_list_compute_visible(
    const std::vector<analysis_list_row_t>& rows,
    const std::string& filter_lower, int column, bool ascending);

class QtWorkspaceContext;
struct QtAnalysisListState;

class QtAnalysisListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int {
        address = 0,
        name = 1,
        kind = 2,
        details = 3,
        column_count = 4
    };

    explicit QtAnalysisListModel(QObject* parent = nullptr);

    void setSnapshot(std::shared_ptr<const analysis_list_snapshot_t> snapshot);
    const analysis_list_snapshot_t* snapshot() const noexcept { return snapshot_.get(); }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const analysis_list_row_t* rowAt(int view_row) const noexcept;
    std::size_t sourceIndexForViewRow(int view_row) const noexcept;

private:
    std::shared_ptr<const analysis_list_snapshot_t> snapshot_ =
        std::make_shared<const analysis_list_snapshot_t>();
};

// Per-binary list-view state (07 sec. 1.3): replaces the never-evicted
// analysis_list_views::states() map. GUI-thread ownership.
struct QtAnalysisListState {
    std::shared_ptr<const analysis_list_snapshot_t> snapshot =
        std::make_shared<const analysis_list_snapshot_t>();
    std::shared_ptr<const analysis_list_snapshot_t> adopted_snapshot;
    std::atomic<std::uint64_t> rebuild_serial{0};
    std::atomic<bool> rebuilding{false};
    bool projected = false;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::uint64_t overlay_revision = 0;
    QString filter;
    std::string filter_lower;
    bool filter_dirty = true;
    bool sort_dirty = true;
    int sort_column = 0;
    bool sort_ascending = true;
    int submitted_sort_column = 0;
    bool submitted_sort_ascending = true;
    std::uint64_t selected_address = 0;
    std::size_t selected_source = static_cast<std::size_t>(-1);
    std::size_t context_source = static_cast<std::size_t>(-1);
};

}
