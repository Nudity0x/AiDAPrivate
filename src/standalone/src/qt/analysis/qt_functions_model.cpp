#include "qt/analysis/qt_functions_model.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

#include <cstdio>

namespace aida::qt::analysis {

QtFunctionsModel::QtFunctionsModel(QObject* parent) : QAbstractTableModel(parent) {}

void QtFunctionsModel::setPresentation(
    std::shared_ptr<const qt_functions_presentation_t> presentation) {
    beginResetModel();
    presentation_ = std::move(presentation);
    if (!presentation_)
        presentation_ = std::make_shared<const qt_functions_presentation_t>();
    endResetModel();
}

int QtFunctionsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0
        : static_cast<int>(presentation_->sorted_indices.size());
}

int QtFunctionsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
}

const qt_function_entry_t* QtFunctionsModel::entryAt(int view_row) const noexcept {
    if (view_row < 0 ||
        static_cast<std::size_t>(view_row) >= presentation_->sorted_indices.size() ||
        !presentation_->entries)
        return nullptr;
    const int source = presentation_->sorted_indices[static_cast<std::size_t>(view_row)];
    if (source < 0 || static_cast<std::size_t>(source) >= presentation_->entries->size())
        return nullptr;
    return &(*presentation_->entries)[static_cast<std::size_t>(source)];
}

int QtFunctionsModel::viewRowForAddress(std::uint64_t address) const noexcept {
    const auto found = presentation_->row_by_address.find(address);
    if (found == presentation_->row_by_address.end()) return -1;
    return static_cast<int>(found->second);
}

QVariant QtFunctionsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.parent().isValid()) return {};
    const auto* entry = entryAt(index.row());
    if (!entry) return {};
    const auto column = static_cast<Column>(index.column());
    const auto& tokens = aida::qt::theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Column::name: return QString::fromStdString(entry->name);
        case Column::size:
            if (entry->size == 0) return QStringLiteral("-");
            if (entry->size >= 1024)
                return QStringLiteral("%1 (%2K)")
                    .arg(entry->size)
                    .arg(static_cast<double>(entry->size) / 1024.0, 0, 'f', 1);
            return QString::number(entry->size);
        case Column::section:
            return entry->section.empty()
                ? QStringLiteral("-") : QString::fromStdString(entry->section);
        case Column::calls:
            return QStringLiteral("%1/%2").arg(entry->calls_in).arg(entry->calls_out);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (column) {
        case Column::name:
            return entry->synthetic_name ? tokens.text_dim : tokens.text_primary;
        case Column::size:
        case Column::section:
            return entry->section.empty() && column == Column::section
                ? tokens.text_dim : tokens.text_secondary;
        case Column::calls: return tokens.text_dim;
        default: return {};
        }
    }
    if (role == Qt::FontRole &&
        (column == Column::size || column == Column::calls))
        return aida::qt::theme::fonts::codeRegular();
    if (role == Qt::UserRole)
        return QString::fromStdString(entry->name);
    if (role == Qt::ToolTipRole) {
        if (column == Column::name) {
            char address[24]{};
            std::snprintf(address, sizeof(address), "0x%llX",
                static_cast<unsigned long long>(entry->address));
            return QString::fromStdString(entry->name) + QStringLiteral("\n") +
                QString::fromLatin1(address);
        }
        return data(index, Qt::DisplayRole);
    }
    return {};
}

void QtFunctionsModel::multiData(const QModelIndex& index,
                                 QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan) {
        switch (roleData.role()) {
        case Qt::DisplayRole:
        case Qt::ForegroundRole:
        case Qt::FontRole:
        case Qt::UserRole:
        case Qt::ToolTipRole:
            roleData.setData(data(index, roleData.role()));
            break;
        default:
            roleData.clearData();
            break;
        }
    }
}

QVariant QtFunctionsModel::headerData(int section, Qt::Orientation orientation,
                                      int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (static_cast<Column>(section)) {
    case Column::name: return QStringLiteral("Function");
    case Column::size: return QStringLiteral("Size");
    case Column::section: return QStringLiteral("Section");
    case Column::calls: return QStringLiteral("Calls");
    default: return {};
    }
}

}
