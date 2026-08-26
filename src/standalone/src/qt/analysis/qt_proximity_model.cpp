#include "qt/analysis/qt_proximity_model.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

#include <cstdio>

namespace aida::qt::analysis {

std::string proximity_relation_summary(const proximity_node_t& node) {
    std::string result;
    if (node.relation_counts[0] != 0)
        result += std::to_string(node.relation_counts[0]) + " xref" +
            (node.relation_counts[0] == 1 ? "" : "s");
    if (node.relation_counts[1] != 0) {
        if (!result.empty()) result += " | ";
        result += std::to_string(node.relation_counts[1]) + " call" +
            (node.relation_counts[1] == 1 ? "" : "s");
    }
    if (node.relation_counts[2] != 0) {
        if (!result.empty()) result += " | ";
        result += std::to_string(node.relation_counts[2]) + " flow";
    }
    return result.empty() ? "Root entity" : result;
}

QtProximityModel::QtProximityModel(QObject* parent) : QAbstractTableModel(parent) {}

void QtProximityModel::setNodes(std::vector<proximity_node_t> nodes,
                                std::vector<std::size_t> visible) {
    beginResetModel();
    nodes_ = std::move(nodes);
    visible_ = std::move(visible);
    endResetModel();
}

void QtProximityModel::clear() {
    setNodes({}, {});
}

int QtProximityModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

int QtProximityModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
}

const proximity_node_t* QtProximityModel::rowAt(int view_row) const noexcept {
    if (view_row < 0 || static_cast<std::size_t>(view_row) >= visible_.size())
        return nullptr;
    return &nodes_[visible_[static_cast<std::size_t>(view_row)]];
}

std::size_t QtProximityModel::sourceIndexForViewRow(int view_row) const noexcept {
    if (view_row < 0 || static_cast<std::size_t>(view_row) >= visible_.size())
        return static_cast<std::size_t>(-1);
    return visible_[static_cast<std::size_t>(view_row)];
}

QVariant QtProximityModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.parent().isValid()) return {};
    const auto* node = rowAt(index.row());
    if (!node) return {};
    const auto column = static_cast<Column>(index.column());
    const auto& tokens = aida::qt::theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Column::depth: return QString::number(node->depth);
        case Column::address: {
            char buf[32]{};
            std::snprintf(buf, sizeof(buf), "0x%016llX",
                static_cast<unsigned long long>(node->address));
            return QString::fromLatin1(buf);
        }
        case Column::name: return QString::fromStdString(node->name);
        case Column::kind: return QString::fromStdString(node->kind);
        case Column::in_out:
            return QStringLiteral("%1 / %2").arg(node->incoming).arg(node->outgoing);
        case Column::relationships:
            return QString::fromStdString(proximity_relation_summary(*node));
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Column::address) return tokens.syn_address;
        if (column == Column::depth || column == Column::in_out)
            return tokens.text_secondary;
        return {};
    }
    if (role == Qt::FontRole && column == Column::address)
        return aida::qt::theme::fonts::codeRegular();
    if (role == Qt::ToolTipRole) {
        if (column == Column::name)
            return QString::fromStdString(node->name);
        return data(index, Qt::DisplayRole);
    }
    return {};
}

void QtProximityModel::multiData(const QModelIndex& index,
                                 QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan) {
        switch (roleData.role()) {
        case Qt::DisplayRole:
        case Qt::ForegroundRole:
        case Qt::FontRole:
        case Qt::ToolTipRole:
            roleData.setData(data(index, roleData.role()));
            break;
        default:
            roleData.clearData();
            break;
        }
    }
}

QVariant QtProximityModel::headerData(int section, Qt::Orientation orientation,
                                      int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (static_cast<Column>(section)) {
    case Column::depth: return QStringLiteral("Depth");
    case Column::address: return QStringLiteral("Address");
    case Column::name: return QStringLiteral("Name");
    case Column::kind: return QStringLiteral("Kind");
    case Column::in_out: return QStringLiteral("In / Out");
    case Column::relationships: return QStringLiteral("Relationships");
    default: return {};
    }
}

}
