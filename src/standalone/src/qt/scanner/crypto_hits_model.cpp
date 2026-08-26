#include "qt/scanner/crypto_hits_model.hpp"

#include <cstdio>

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::scanner {

CryptoHitsModel::CryptoHitsModel(QObject* parent) : QAbstractTableModel(parent) {}

void CryptoHitsModel::adopt(
	std::shared_ptr<const std::vector<crypto_scanner::crypto_hit_t>> rows)
{
	if (rows_ == rows)
		return;
	beginResetModel();
	rows_ = std::move(rows);
	endResetModel();
}

const crypto_scanner::crypto_hit_t* CryptoHitsModel::row_at(int row) const noexcept
{
	if (!rows_ || row < 0 || row >= static_cast<int>(rows_->size()))
		return nullptr;
	return &(*rows_)[static_cast<std::size_t>(row)];
}

int CryptoHitsModel::rowCount(const QModelIndex& parent) const
{
	if (parent.isValid() || !rows_)
		return 0;
	return static_cast<int>(rows_->size());
}

int CryptoHitsModel::columnCount(const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : static_cast<int>(column_count);
}

QColor CryptoHitsModel::category_color(crypto_scanner::crypto_category_t category)
{
	const auto& tokens = theme::tokens();
	switch (category) {
	case crypto_scanner::crypto_category_t::symmetric:     return tokens.info;
	case crypto_scanner::crypto_category_t::hash:          return tokens.success;
	case crypto_scanner::crypto_category_t::stream_cipher: return tokens.warning;
	case crypto_scanner::crypto_category_t::block_cipher:  return tokens.accent;
	case crypto_scanner::crypto_category_t::checksum:      return tokens.warning;
	case crypto_scanner::crypto_category_t::encoding:      return tokens.text_secondary;
	case crypto_scanner::crypto_category_t::asymmetric:    return tokens.error;
	default:                                             return tokens.text_secondary;
	}
}

QVariant CryptoHitsModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || index.parent().isValid())
		return {};
	const auto* hit = row_at(index.row());
	if (!hit)
		return {};
	const auto& tokens = theme::tokens();
	if (role == Qt::ToolTipRole)
		role = Qt::DisplayRole;
	switch (role) {
	case Qt::DisplayRole:
		switch (index.column()) {
		case column_algorithm:
			return QString::fromStdString(hit->algorithm);
		case column_signature:
			return QString::fromStdString(hit->signature_name);
		case column_address: {
			char buf[24]{};
			std::snprintf(buf, sizeof(buf), "0x%llX",
				static_cast<unsigned long long>(hit->address));
			return QString::fromLatin1(buf);
		}
		case column_module: {
			char buf[96]{};
			std::snprintf(buf, sizeof(buf), "%s+0x%llX", hit->module_name.c_str(),
				static_cast<unsigned long long>(hit->module_offset));
			return QString::fromLatin1(buf);
		}
		case column_category:
			return QString::fromLatin1(
				crypto_scanner::category_name(hit->category));
		case column_refs:
			return hit->referencing_functions.empty() ? QVariant()
				: QStringLiteral("%1 refs").arg(hit->referencing_functions.size());
		default:
			return {};
		}
	case Qt::FontRole:
		return index.column() == column_address
			? QVariant(theme::fonts::codeRegular())
			: QVariant(theme::fonts::body());
	case Qt::ForegroundRole:
		switch (index.column()) {
		case column_algorithm:
			return category_color(hit->category);
		case column_signature:
			return tokens.text_primary;
		case column_address:
			return tokens.text_address;
		case column_module:
			return tokens.text_secondary;
		case column_category:
			return category_color(hit->category);
		case column_refs:
			return tokens.accent;
		default:
			return {};
		}
	case Qt::TextAlignmentRole:
		return int(Qt::AlignVCenter | Qt::AlignLeft);
	case role_category:
		return static_cast<int>(hit->category);
	default:
		return {};
	}
}

void CryptoHitsModel::multiData(const QModelIndex& index,
	QModelRoleDataSpan span) const
{
	for (QModelRoleData& role_data : span) {
		switch (role_data.role()) {
		case Qt::DisplayRole:
		case Qt::ToolTipRole:
		case Qt::FontRole:
		case Qt::ForegroundRole:
		case Qt::TextAlignmentRole:
		case role_category:
			role_data.setData(data(index, role_data.role()));
			break;
		default:
			role_data.clearData();
			break;
		}
	}
}

QVariant CryptoHitsModel::headerData(int section, Qt::Orientation orientation,
	int role) const
{
	if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
		return {};
	switch (section) {
	case column_algorithm: return QStringLiteral("Algorithm");
	case column_signature: return QStringLiteral("Signature");
	case column_address: return QStringLiteral("Address");
	case column_module: return QStringLiteral("Module + Offset");
	case column_category: return QStringLiteral("Category");
	case column_refs: return QStringLiteral("Refs");
	default: return {};
	}
}

}
