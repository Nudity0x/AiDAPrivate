#include "qt/scanner/diff_table_model.hpp"

#include <algorithm>
#include <cstdio>

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::scanner {

namespace {

QString hex_preview(const std::vector<std::uint8_t>& data, std::uint32_t size,
	std::size_t cap)
{
	QString out;
	const std::size_t count = (std::min)({static_cast<std::size_t>(size),
		data.size(), cap});
	for (std::size_t index = 0; index < count; ++index) {
		char byte[4]{};
		std::snprintf(byte, sizeof(byte), "%02X ", static_cast<unsigned int>(data[index]));
		out += QString::fromLatin1(byte);
	}
	if (size > cap)
		out += QStringLiteral("...");
	return out;
}

}

DiffTableModel::DiffTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void DiffTableModel::adopt(std::shared_ptr<const snapshot_diff::diff_result_t> diff)
{
	beginResetModel();
	diff_ = std::move(diff);
	endResetModel();
}

const snapshot_diff::changed_region_t* DiffTableModel::row_at(int row) const noexcept
{
	if (!diff_ || row < 0 || row >= static_cast<int>(diff_->changes.size()))
		return nullptr;
	return &diff_->changes[static_cast<std::size_t>(row)];
}

int DiffTableModel::rowCount(const QModelIndex& parent) const
{
	if (parent.isValid() || !diff_)
		return 0;
	return static_cast<int>(diff_->changes.size());
}

int DiffTableModel::columnCount(const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : static_cast<int>(column_count);
}

QColor DiffTableModel::change_color(snapshot_diff::change_type_t type)
{
	const auto& tokens = theme::tokens();
	switch (type) {
	case snapshot_diff::change_type_t::pointer_changed:     return tokens.warning;
	case snapshot_diff::change_type_t::float_changed:       return tokens.info;
	case snapshot_diff::change_type_t::counter_incremented: return tokens.success;
	case snapshot_diff::change_type_t::counter_decremented: return tokens.success;
	case snapshot_diff::change_type_t::string_modified:     return tokens.accent;
	case snapshot_diff::change_type_t::zeroed_out:          return tokens.error;
	case snapshot_diff::change_type_t::byte_flip:           return tokens.text_secondary;
	default:                                              return tokens.text_secondary;
	}
}

QVariant DiffTableModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || index.parent().isValid())
		return {};
	const auto* change = row_at(index.row());
	if (!change)
		return {};
	const auto& tokens = theme::tokens();
	switch (role) {
	case Qt::ToolTipRole:
		switch (index.column()) {
		case column_old:
			return hex_preview(change->old_data, change->size, 32);
		case column_new:
			return hex_preview(change->new_data, change->size, 32);
		default:
			return data(index, Qt::DisplayRole);
		}
	case Qt::DisplayRole:
		switch (index.column()) {
		case column_address: {
			char buf[24]{};
			std::snprintf(buf, sizeof(buf), "0x%llX",
				static_cast<unsigned long long>(change->address));
			return QString::fromLatin1(buf);
		}
		case column_old:
			return hex_preview(change->old_data, change->size, 8);
		case column_new:
			return hex_preview(change->new_data, change->size, 8);
		case column_type:
			return QString::fromLatin1(
				snapshot_diff::detail::change_type_name(change->type));
		case column_module:
			return QString::fromStdString(change->module_name);
		default:
			return {};
		}
	case Qt::FontRole:
		return index.column() <= column_new
			? QVariant(theme::fonts::codeRegular())
			: QVariant(theme::fonts::body());
	case Qt::ForegroundRole:
		switch (index.column()) {
		case column_address: return tokens.text_address;
		case column_old: return tokens.error;
		case column_new: return tokens.success;
		case column_type: return change_color(change->type);
		case column_module: return tokens.text_dim;
		default: return {};
		}
	case Qt::TextAlignmentRole:
		return int(Qt::AlignVCenter | Qt::AlignLeft);
	case role_change_type:
		return static_cast<int>(change->type);
	default:
		return {};
	}
}

void DiffTableModel::multiData(const QModelIndex& index,
	QModelRoleDataSpan span) const
{
	for (QModelRoleData& role_data : span) {
		switch (role_data.role()) {
		case Qt::DisplayRole:
		case Qt::ToolTipRole:
		case Qt::FontRole:
		case Qt::ForegroundRole:
		case Qt::TextAlignmentRole:
		case role_change_type:
			role_data.setData(data(index, role_data.role()));
			break;
		default:
			role_data.clearData();
			break;
		}
	}
}

QVariant DiffTableModel::headerData(int section, Qt::Orientation orientation,
	int role) const
{
	if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
		return {};
	switch (section) {
	case column_address: return QStringLiteral("Address");
	case column_old: return QStringLiteral("Old Value");
	case column_new: return QStringLiteral("New Value");
	case column_type: return QStringLiteral("Type");
	case column_module: return QStringLiteral("Module");
	default: return {};
	}
}

}
