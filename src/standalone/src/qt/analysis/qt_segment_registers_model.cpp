#include "qt/analysis/qt_segment_registers_model.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include <Zydis/Zydis.h>

namespace aida::qt::analysis {

std::string segment_register_text(std::uint16_t value) {
    const char* name = ZydisRegisterGetString(static_cast<ZydisRegister>(value));
    if (name && *name) {
        std::string result(name);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        return result;
    }
    return "Register #" + std::to_string(value);
}

std::string segment_register_provenance_text(
    aida::analysis::fact_provenance_t provenance) {
    using value_t = aida::analysis::fact_provenance_t;
    switch (provenance) {
    case value_t::gap_recovery: return "Gap recovery";
    case value_t::linear_validation: return "Linear validation";
    case value_t::recursive_decode: return "Recursive decode";
    case value_t::relocation: return "Relocation";
    case value_t::call_target: return "Call target";
    case value_t::export_entry: return "Export entry";
    case value_t::tls_entry: return "TLS entry";
    case value_t::image_entry: return "Image entry";
    case value_t::unwind_metadata: return "Unwind metadata";
    case value_t::debug_symbol: return "Debug symbol";
    case value_t::user_definition: return "User definition";
    case value_t::decompiler_feedback: return "Decompiler feedback";
    default: return "Decoded fact";
    }
}

std::string segment_register_observed_span(const segment_register_row_t& row) {
    char first[32]{};
    std::snprintf(first, sizeof(first), "0x%016llX",
        static_cast<unsigned long long>(row.address));
    if (row.address == row.end_address) return first;
    char last[32]{};
    std::snprintf(last, sizeof(last), "0x%016llX",
        static_cast<unsigned long long>(row.end_address));
    return std::string(first) + " - " + last;
}

QtSegmentRegistersModel::QtSegmentRegistersModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtSegmentRegistersModel::resetAll() {
    beginResetModel();
    rows_.clear();
    visible_.clear();
    endResetModel();
}

void QtSegmentRegistersModel::recomputeVisible() {
    visible_.clear();
    visible_.reserve(rows_.size());
    const std::string needle = filter_.toStdString();
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        if (matches(rows_[index], needle))
            visible_.push_back(index);
    }
    std::stable_sort(visible_.begin(), visible_.end(), [&](std::size_t l, std::size_t r) {
        const auto& lhs = rows_[l].row;
        const auto& rhs = rows_[r].row;
        if (lhs.register_id != rhs.register_id) return lhs.register_id < rhs.register_id;
        return lhs.address < rhs.address;
    });
}

void QtSegmentRegistersModel::appendRows(
    std::vector<segment_register_display_row_t> chunk) {
    if (chunk.empty()) return;
    // Chunk completions merge new groups into existing rows mid-scan, so this is
    // a snapshot-identity change: publish with a full reset (07 S3 reset path).
    beginResetModel();
    rows_.insert(rows_.end(), std::make_move_iterator(chunk.begin()),
        std::make_move_iterator(chunk.end()));
    recomputeVisible();
    endResetModel();
}

void QtSegmentRegistersModel::setRows(
    std::vector<segment_register_display_row_t> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    recomputeVisible();
    endResetModel();
}

void QtSegmentRegistersModel::applyFilter(const QString& filter_lower) {
    if (filter_ == filter_lower) return;
    beginResetModel();
    filter_ = filter_lower;
    recomputeVisible();
    endResetModel();
}

bool QtSegmentRegistersModel::matches(const segment_register_display_row_t& row,
                                      const std::string& needle) {
    if (needle.empty()) return true;
    const auto contains = [&needle](const std::string& value) {
        if (needle.size() > value.size()) return false;
        return std::search(value.begin(), value.end(), needle.begin(), needle.end(),
            [](unsigned char left, unsigned char right) {
                return std::tolower(left) == std::tolower(right);
            }) != value.end();
    };
    return contains(segment_register_text(row.row.register_id)) ||
        contains(row.instruction.toStdString()) ||
        contains(segment_register_observed_span(row.row)) ||
        contains(segment_register_provenance_text(row.row.provenance));
}

int QtSegmentRegistersModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

int QtSegmentRegistersModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
}

const segment_register_display_row_t* QtSegmentRegistersModel::rowAt(
    int view_row) const noexcept {
    if (view_row < 0 || static_cast<std::size_t>(view_row) >= visible_.size())
        return nullptr;
    return &rows_[visible_[static_cast<std::size_t>(view_row)]];
}

std::size_t QtSegmentRegistersModel::sourceIndexForViewRow(int view_row) const noexcept {
    if (view_row < 0 || static_cast<std::size_t>(view_row) >= visible_.size())
        return static_cast<std::size_t>(-1);
    return visible_[static_cast<std::size_t>(view_row)];
}

QVariant QtSegmentRegistersModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.parent().isValid()) return {};
    const auto* row = rowAt(index.row());
    if (!row) return {};
    const auto column = static_cast<Column>(index.column());
    const auto& tokens = aida::qt::theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Column::reg:
            return QString::fromStdString(segment_register_text(row->row.register_id));
        case Column::observed_span:
            return QString::fromStdString(segment_register_observed_span(row->row));
        case Column::facts: return QString::number(row->row.observations);
        case Column::instruction: return row->instruction;
        case Column::evidence:
            return row->row.segment_relative
                ? QStringLiteral("Segment-relative")
                : QString::fromStdString(
                    segment_register_provenance_text(row->row.provenance));
        case Column::confidence:
            return QStringLiteral("%1%").arg(static_cast<unsigned>(row->row.confidence));
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Column::observed_span) return tokens.syn_address;
        if (column == Column::confidence) return tokens.text_secondary;
        return {};
    }
    if (role == Qt::FontRole && column == Column::observed_span)
        return aida::qt::theme::fonts::codeRegular();
    if (role == Qt::ToolTipRole) {
        if (column == Column::instruction)
            return row->instruction;
        return data(index, Qt::DisplayRole);
    }
    return {};
}

void QtSegmentRegistersModel::multiData(const QModelIndex& index,
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

QVariant QtSegmentRegistersModel::headerData(int section, Qt::Orientation orientation,
                                             int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (static_cast<Column>(section)) {
    case Column::reg: return QStringLiteral("Register");
    case Column::observed_span: return QStringLiteral("Observed span");
    case Column::facts: return QStringLiteral("Facts");
    case Column::instruction: return QStringLiteral("Instruction");
    case Column::evidence: return QStringLiteral("Evidence");
    case Column::confidence: return QStringLiteral("Confidence");
    default: return {};
    }
}

}
