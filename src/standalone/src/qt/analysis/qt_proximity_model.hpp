#pragma once

#include <QAbstractTableModel>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aida::qt::analysis {

// Ported from analysis_relationship_views::proximity (types verbatim).
enum class proximity_relation_kind_t : std::uint8_t {
    xref,
    call,
    control_flow
};

struct proximity_node_t {
    std::uint64_t address = 0;
    std::string name;
    std::string kind;
    std::uint32_t depth = 0;
    std::uint32_t incoming = 0;
    std::uint32_t outgoing = 0;
    std::uint32_t relation_counts[3] = {};
};

std::string proximity_relation_summary(const proximity_node_t& node);

// Full incremental BFS state (07 sec. 5.2), owned by the view's QTimer(0) chunk
// pump on the GUI thread. Ported from analysis_relationship_views::proximity.
struct QtProximityState {
    bool initialized = false;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::uint64_t overlay_revision = 0;
    std::uint64_t root = 0;
    int depth_limit = 2;
    int node_limit = 192;
    int pass = 0;
    std::size_t xref_cursor = 0;
    std::size_t call_cursor = 0;
    std::size_t edge_cursor = 0;
    bool complete = false;
    bool capped = false;
    std::uint64_t skipped_relations = 0;
    std::vector<proximity_node_t> nodes;
    struct relation_t {
        std::uint64_t source = 0;
        std::uint64_t target = 0;
        proximity_relation_kind_t kind = proximity_relation_kind_t::xref;

        bool operator==(const relation_t& other) const noexcept {
            return source == other.source && target == other.target && kind == other.kind;
        }
    };
    std::vector<relation_t> relations;
    std::unordered_map<std::uint64_t, std::size_t> node_by_address;
    std::unordered_set<std::uint64_t> frontier;
    std::unordered_set<std::uint64_t> next_frontier;
    struct relation_identity_hash_t {
        std::size_t operator()(const relation_t& value) const noexcept {
            std::uint64_t hash = value.source ^ (value.target + 0x9E3779B97F4A7C15ULL +
                (value.source << 6U) + (value.source >> 2U));
            hash ^= static_cast<std::uint64_t>(value.kind) * 0xD6E8FEB86659FD93ULL;
            return static_cast<std::size_t>(hash);
        }
    };
    std::unordered_set<relation_t, relation_identity_hash_t> relation_keys;
    std::vector<std::uint64_t> history;
    std::size_t history_index = 0;
    QString filter;
    bool filter_dirty = true;
    std::size_t selected = static_cast<std::size_t>(-1);
};

class QtProximityModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int {
        depth = 0,
        address = 1,
        name = 2,
        kind = 3,
        in_out = 4,
        relationships = 5,
        column_count = 6
    };

    explicit QtProximityModel(QObject* parent = nullptr);

    void setNodes(std::vector<proximity_node_t> nodes,
                  std::vector<std::size_t> visible);
    void clear();

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const proximity_node_t* rowAt(int view_row) const noexcept;
    std::size_t sourceIndexForViewRow(int view_row) const noexcept;

private:
    std::vector<proximity_node_t> nodes_;
    std::vector<std::size_t> visible_;
};

}
