#include "qt/scanner/address_list_model.hpp"

#include <algorithm>
#include <cstdio>

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::scanner {

AddressListModel::AddressListModel(QObject* parent) : QAbstractTableModel(parent) {}

void AddressListModel::reset_entries(
	std::vector<memory_scanner::address_entry_t> entries)
{
	beginResetModel();
	entries_ = std::move(entries);
	endResetModel();
}

void AddressListModel::refresh_values(
	std::vector<memory_scanner::address_entry_t> entries)
{
	if (entries.size() != entries_.size()) {
		reset_entries(std::move(entries));
		return;
	}
	entries_ = std::move(entries);
	if (!entries_.empty())
		Q_EMIT dataChanged(index(0, column_value),
			index(static_cast<int>(entries_.size()) - 1, column_value),
			{Qt::DisplayRole, Qt::ForegroundRole});
}

void AddressListModel::patch_row(int row)
{
	if (row < 0 || row >= static_cast<int>(entries_.size()))
		return;
	Q_EMIT dataChanged(index(row, column_active), index(row, column_value));
}

const memory_scanner::address_entry_t* AddressListModel::entry_at(int row) const noexcept
{
	if (row < 0 || row >= static_cast<int>(entries_.size()))
		return nullptr;
	return &entries_[static_cast<std::size_t>(row)];
}

int AddressListModel::rowCount(const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

int AddressListModel::columnCount(const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : static_cast<int>(column_count);
}

QVariant AddressListModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || index.parent().isValid())
		return {};
	const auto* entry = entry_at(index.row());
	if (!entry)
		return {};
	const auto& tokens = theme::tokens();
	if (role == Qt::ToolTipRole)
		role = Qt::DisplayRole;
	switch (role) {
	case Qt::DisplayRole:
		switch (index.column()) {
		case column_active:
			return entry->frozen ? QStringLiteral("Locked")
				: QStringLiteral("Free");
		case column_description:
			return entry->description.empty()
				? QStringLiteral("<no description>")
				: QString::fromStdString(entry->description);
		case column_address: {
			char buf[24]{};
			std::snprintf(buf, sizeof(buf), "0x%016llX",
				static_cast<unsigned long long>(entry->address));
			return QString::fromLatin1(buf);
		}
		case column_type:
			return QString::fromLatin1(
				memory_scanner::value_type_name(entry->value_type));
		case column_value:
			return QString::fromStdString(memory_scanner::format_value(
				entry->last_value, entry->value_type));
		default:
			return {};
		}
	case Qt::FontRole:
		return (index.column() == column_address || index.column() == column_value)
			? QVariant(theme::fonts::codeRegular())
			: QVariant(theme::fonts::body());
	case Qt::ForegroundRole:
		switch (index.column()) {
		case column_active:
			return entry->frozen ? tokens.accent : tokens.text_dim;
		case column_description:
			return entry->description.empty() ? tokens.text_dim
				: tokens.text_primary;
		case column_address:
			return tokens.text_address;
		case column_type:
			return tokens.text_secondary;
		case column_value:
			return entry->frozen ? tokens.accent : tokens.success;
		default:
			return {};
		}
	case Qt::TextAlignmentRole:
		return int(Qt::AlignVCenter | Qt::AlignLeft);
	case role_frozen:
		return entry->frozen;
	default:
		return {};
	}
}

void AddressListModel::multiData(const QModelIndex& index,
	QModelRoleDataSpan span) const
{
	for (QModelRoleData& role_data : span) {
		switch (role_data.role()) {
		case Qt::DisplayRole:
		case Qt::ToolTipRole:
		case Qt::FontRole:
		case Qt::ForegroundRole:
		case Qt::TextAlignmentRole:
		case role_frozen:
			role_data.setData(data(index, role_data.role()));
			break;
		default:
			role_data.clearData();
			break;
		}
	}
}

QVariant AddressListModel::headerData(int section, Qt::Orientation orientation,
	int role) const
{
	if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
		return {};
	switch (section) {
	case column_active: return QStringLiteral("Active");
	case column_description: return QStringLiteral("Description");
	case column_address: return QStringLiteral("Address");
	case column_type: return QStringLiteral("Type");
	case column_value: return QStringLiteral("Value");
	default: return {};
	}
}

}
