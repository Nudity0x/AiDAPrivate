#include "qt/analysis/qt_xref_model.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

#include <charconv>
#include <cstdio>

namespace aida::qt::analysis {

const char* qt_xref_kind_name(aida::analysis::xref_kind_t kind) noexcept {
    using aida::analysis::xref_kind_t;
    switch (kind) {
    case xref_kind_t::code: return "code";
    case xref_kind_t::call: return "call";
    case xref_kind_t::read: return "read";
    case xref_kind_t::write: return "write";
    case xref_kind_t::address: return "address";
    case xref_kind_t::relocation: return "relocation";
    }
    return "unknown";
}

std::optional<std::uint64_t> qt_xref_parse_address(std::string text) {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text.erase(0, 2);
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size()
        ? std::optional<std::uint64_t>(value) : std::nullopt;
}

QtXrefModel::QtXrefModel(QObject* parent) : QAbstractTableModel(parent) {}

void QtXrefModel::setResults(
    std::shared_ptr<const std::vector<qt_xref_display_result_t>> results) {
    beginResetModel();
    results_ = std::move(results);
    endResetModel();
}

int QtXrefModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() || !results_ ? 0 : static_cast<int>(results_->size());
}

int QtXrefModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
}

const qt_xref_display_result_t* QtXrefModel::rowAt(int view_row) const noexcept {
    if (!results_ || view_row < 0 ||
        static_cast<std::size_t>(view_row) >= results_->size())
        return nullptr;
    return &(*results_)[static_cast<std::size_t>(view_row)];
}

QVariant QtXrefModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.parent().isValid()) return {};
    const auto* row = rowAt(index.row());
    if (!row) return {};
    const auto column = static_cast<Column>(index.column());
    const auto& tokens = aida::qt::theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Column::target: {
            char address[32]{};
            std::snprintf(address, sizeof(address), "%016llX",
                static_cast<unsigned long long>(row->runtime));
            return QString::fromLatin1(address);
        }
        case Column::kind: return QString::fromLatin1(qt_xref_kind_name(row->result.kind));
        case Column::name: return QString::fromStdString(row->name);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Column::target) return tokens.syn_address;
        if (column == Column::kind) return tokens.text_secondary;
        return {};
    }
    if (role == Qt::FontRole && column == Column::target)
        return aida::qt::theme::fonts::codeRegular();
    if (role == Qt::ToolTipRole) {
        if (column == Column::name)
            return QString::fromStdString(row->name);
        return data(index, Qt::DisplayRole);
    }
    return {};
}

void QtXrefModel::multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const {
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

QVariant QtXrefModel::headerData(int section, Qt::Orientation orientation,
                                 int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (static_cast<Column>(section)) {
    case Column::target: return QStringLiteral("Address");
    case Column::kind: return QStringLiteral("Kind");
    case Column::name: return QStringLiteral("Name");
    default: return {};
    }
}

}
