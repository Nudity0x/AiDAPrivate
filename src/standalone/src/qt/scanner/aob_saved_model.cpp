#include "qt/scanner/aob_saved_model.hpp"

#include <cstdio>

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::scanner {

AobSavedModel::AobSavedModel(QObject* parent) : QAbstractTableModel(parent) {}

void AobSavedModel::adopt(std::vector<aob_generator::signature_t> rows)
{
	beginResetModel();
	rows_ = std::move(rows);
	endResetModel();
}

const aob_generator::signature_t* AobSavedModel::row_at(int row) const noexcept
{
	if (row < 0 || row >= static_cast<int>(rows_.size()))
		return nullptr;
	return &rows_[static_cast<std::size_t>(row)];
}

int AobSavedModel::rowCount(const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int AobSavedModel::columnCount(const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : static_cast<int>(column_count);
}

QColor AobSavedModel::grade_color(float quality_score)
{
	const auto& tokens = theme::tokens();
	if (quality_score >= 0.85f) return tokens.success;
	if (quality_score >= 0.7f)  return tokens.info;
	if (quality_score >= 0.5f)  return tokens.warning;
	return tokens.error;
}

QVariant AobSavedModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || index.parent().isValid())
		return {};
	const auto* signature = row_at(index.row());
	if (!signature)
		return {};
	const auto& tokens = theme::tokens();
	if (role == Qt::ToolTipRole)
		role = Qt::DisplayRole;
	switch (role) {
	case Qt::DisplayRole:
		switch (index.column()) {
		case column_grade:
			return QString::fromLatin1(
				aob_generator::score_grade(signature->quality_score));
		case column_name:
			return QString::fromStdString(signature->name);
		case column_address: {
			char buf[24]{};
			std::snprintf(buf, sizeof(buf), "0x%llX",
				static_cast<unsigned long long>(signature->address));
			return QString::fromLatin1(buf);
		}
		case column_size:
			return QStringLiteral("%1 B").arg(signature->bytes.size());
		case column_uniqueness:
			if (signature->uniqueness_count > 0)
				return signature->unique ? QStringLiteral("unique")
					: QStringLiteral("non-unique");
			return {};
		default:
			return {};
		}
	case Qt::FontRole:
		return index.column() == column_address
			? QVariant(theme::fonts::codeRegular())
			: QVariant(theme::fonts::body());
	case Qt::ForegroundRole:
		switch (index.column()) {
		case column_grade:
			return grade_color(signature->quality_score);
		case column_name:
			return tokens.text_primary;
		case column_address:
			return tokens.text_address;
		case column_size:
			return tokens.text_dim;
		case column_uniqueness:
			if (signature->uniqueness_count > 0)
				return signature->unique ? tokens.success : tokens.warning;
			return tokens.text_dim;
		default:
			return {};
		}
	case Qt::TextAlignmentRole:
		return int(Qt::AlignVCenter | Qt::AlignLeft);
	case role_quality:
		return static_cast<double>(signature->quality_score);
	case role_unique:
		return signature->unique;
	default:
		return {};
	}
}

void AobSavedModel::multiData(const QModelIndex& index,
	QModelRoleDataSpan span) const
{
	for (QModelRoleData& role_data : span) {
		switch (role_data.role()) {
		case Qt::DisplayRole:
		case Qt::ToolTipRole:
		case Qt::FontRole:
		case Qt::ForegroundRole:
		case Qt::TextAlignmentRole:
		case role_quality:
		case role_unique:
			role_data.setData(data(index, role_data.role()));
			break;
		default:
			role_data.clearData();
			break;
		}
	}
}

QVariant AobSavedModel::headerData(int section, Qt::Orientation orientation,
	int role) const
{
	if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
		return {};
	switch (section) {
	case column_grade: return QStringLiteral("Grade");
	case column_name: return QStringLiteral("Name");
	case column_address: return QStringLiteral("Address");
	case column_size: return QStringLiteral("Size");
	case column_uniqueness: return QStringLiteral("Uniqueness");
	default: return {};
	}
}

}
