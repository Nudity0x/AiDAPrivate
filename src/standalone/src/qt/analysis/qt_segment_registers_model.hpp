#pragma once

#include <QAbstractTableModel>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <iterator>

#include "core/analysis/workspace/analysis_workspace.hpp"

namespace aida::qt::analysis {

// Ported from analysis_relationship_views::segment_registers (types verbatim).
struct segment_register_row_t {
    std::uint64_t address = 0;
    std::uint64_t end_address = 0;
    aida::analysis::entity_id_t instruction_id = 0;
    std::uint16_t register_id = 0;
    std::uint8_t operand_index = 0;
    bool segment_relative = false;
    aida::analysis::fact_provenance_t provenance =
        aida::analysis::fact_provenance_t::unknown;
    std::uint8_t confidence = 0;
    std::uint64_t observations = 0;
};

struct segment_register_group_identity_t {
    std::uint64_t scope = 0;
    std::uint16_t register_id = 0;
    bool segment_relative = false;

    bool operator==(const segment_register_group_identity_t& other) const noexcept {
        return scope == other.scope && register_id == other.register_id &&
            segment_relative == other.segment_relative;
    }
};

struct segment_register_group_hash_t {
    std::size_t operator()(const segment_register_group_identity_t& value) const noexcept {
        std::uint64_t hash = value.scope ^
            (static_cast<std::uint64_t>(value.register_id) * 0x9E3779B97F4A7C15ULL);
        if (value.segment_relative) hash ^= 0xD6E8FEB86659FD93ULL;
        return static_cast<std::size_t>(hash);
    }
};

std::string segment_register_text(std::uint16_t value);
std::string segment_register_provenance_text(
    aida::analysis::fact_provenance_t provenance);
std::string segment_register_observed_span(const segment_register_row_t& row);

// GUI-thread incremental scan state (07 sec. 5.1). The QTimer(0) chunk pump owns it.
struct QtSegmentRegistersState {
    bool initialized = false;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::uint64_t overlay_revision = 0;
    std::size_t instruction_cursor = 0;
    bool complete = false;
    std::vector<segment_register_row_t> rows;
    std::unordered_map<segment_register_group_identity_t, std::size_t,
        segment_register_group_hash_t> groups;
    std::uint64_t observations = 0;
    QString filter;
    bool filter_dirty = true;
    std::size_t selected = static_cast<std::size_t>(-1);
};

// Presentation rows carry the resolved instruction text so painting never
// touches the disasm formatting cache.
struct segment_register_display_row_t {
    segment_register_row_t row;
    QString instruction;
};

class QtSegmentRegistersModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int {
        reg = 0,
        observed_span = 1,
        facts = 2,
        instruction = 3,
        evidence = 4,
        confidence = 5,
        column_count = 6
    };

    explicit QtSegmentRegistersModel(QObject* parent = nullptr);

    void resetAll();
    // One beginInsertRows batch per chunk (07 S3).
    void appendRows(std::vector<segment_register_display_row_t> chunk);
    void setRows(std::vector<segment_register_display_row_t> rows);
    void applyFilter(const QString& filter_lower);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const segment_register_display_row_t* rowAt(int view_row) const noexcept;
    std::size_t sourceIndexForViewRow(int view_row) const noexcept;
    const std::vector<segment_register_display_row_t>& allRows() const noexcept {
        return rows_;
    }

private:
    void recomputeVisible();
    static bool matches(const segment_register_display_row_t& row,
                        const std::string& needle);

    std::vector<segment_register_display_row_t> rows_;
    std::vector<std::size_t> visible_;
    QString filter_;
};

}
