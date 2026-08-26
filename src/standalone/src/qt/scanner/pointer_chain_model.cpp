#include "qt/scanner/pointer_chain_model.hpp"

#include <cstdio>

#include "qt/scanner/pointer_controller.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::scanner {

PointerChainModel::PointerChainModel(QObject* parent) : QAbstractTableModel(parent) {}

PointerChainModel::validity_t PointerChainModel::probe_validity(
	const pointer_scanner::pointer_chain_t& chain) const
{
	resolution_status_t status = resolution_status_t::idle;
	static_cast<void>(PointerScanController::instance().resolved_step_address(
		chain, static_cast<int>(chain.offsets.size()), &status));
	if (chain.validated || status == resolution_status_t::ready)
		return validity_t::valid;
	if (status == resolution_status_t::queued || status == resolution_status_t::running)
		return validity_t::busy;
	return validity_t::unknown;
}

void PointerChainModel::adopt(std::vector<pointer_scanner::pointer_chain_t> rows)
{
	beginResetModel();
	rows_.clear();
	rows_.reserve(rows.size());
	for (auto& chain : rows) {
		row_t row;
		QString chain_text;
		for (std::size_t j = 0; j < chain.offsets.size(); ++j) {
			if (j > 0)
				chain_text += QStringLiteral(" -> ");
			chain_text += QString::fromStdString(
				PointerScanController::format_offset(chain.offsets[j]));
		}
		row.chain_text = std::move(chain_text);
		row.validity = probe_validity(chain);
		row.chain = std::move(chain);
		rows_.push_back(std::move(row));
	}
	endResetModel();
}

void PointerChainModel::refresh_validity()
{
	if (rows_.empty())
		return;
	bool any_change = false;
	for (auto& row : rows_) {
		const validity_t current = probe_validity(row.chain);
		if (current != row.validity) {
			row.validity = current;
			any_change = true;
		}
	}
	if (any_change)
		Q_EMIT dataChanged(index(0, column_valid),
			index(static_cast<int>(rows_.size()) - 1, column_valid),
			{Qt::DisplayRole, Qt::ForegroundRole, role_validity});
}

const pointer_scanner::pointer_chain_t* PointerChainModel::row_at(int row) const noexcept
{
	if (row < 0 || row >= static_cast<int>(rows_.size()))
		return nullptr;
	return &rows_[static_cast<std::size_t>(row)].chain;
}

int PointerChainModel::rowCount(const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int PointerChainModel::columnCount(const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : static_cast<int>(column_count);
}

QVariant PointerChainModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || index.parent().isValid())
		return {};
	const auto* chain = row_at(index.row());
	if (!chain)
		return {};
	const auto& tokens = theme::tokens();
	const row_t& row = rows_[static_cast<std::size_t>(index.row())];
	if (role == Qt::ToolTipRole)
		role = Qt::DisplayRole;
	switch (role) {
	case Qt::DisplayRole:
		switch (index.column()) {
		case column_depth:
			return QString::number(chain->depth);
		case column_module:
			return chain->module_name.empty() ? QStringLiteral("dynamic")
				: QString::fromStdString(chain->module_name);
		case column_base: {
			char buf[24]{};
			std::snprintf(buf, sizeof(buf), "0x%llX",
				static_cast<unsigned long long>(chain->base_offset));
			return QString::fromLatin1(buf);
		}
		case column_chain:
			return row.chain_text;
		case column_valid:
			return row.validity == validity_t::valid ? QStringLiteral("valid")
				: row.validity == validity_t::busy ? QStringLiteral("busy")
				: QStringLiteral("?");
		default:
			return {};
		}
	case Qt::FontRole:
		return (index.column() == column_base || index.column() == column_chain)
			? QVariant(theme::fonts::codeRegular())
			: QVariant(theme::fonts::body());
	case Qt::ForegroundRole:
		switch (index.column()) {
		case column_depth:
			return tokens.text_primary;
		case column_module:
			return chain->is_static ? tokens.accent : tokens.text_dim;
		case column_base:
			return tokens.text_address;
		case column_chain:
			return tokens.text_secondary;
		case column_valid:
			return row.validity == validity_t::valid ? tokens.success
				: tokens.text_secondary;
		default:
			return {};
		}
	case Qt::TextAlignmentRole:
		return int(Qt::AlignVCenter | Qt::AlignLeft);
	case role_validity:
		return static_cast<int>(row.validity);
	default:
		return {};
	}
}

void PointerChainModel::multiData(const QModelIndex& index,
	QModelRoleDataSpan span) const
{
	for (QModelRoleData& role_data : span) {
		switch (role_data.role()) {
		case Qt::DisplayRole:
		case Qt::ToolTipRole:
		case Qt::FontRole:
		case Qt::ForegroundRole:
		case Qt::TextAlignmentRole:
		case role_validity:
			role_data.setData(data(index, role_data.role()));
			break;
		default:
			role_data.clearData();
			break;
		}
	}
}

QVariant PointerChainModel::headerData(int section, Qt::Orientation orientation,
	int role) const
{
	if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
		return {};
	switch (section) {
	case column_depth: return QStringLiteral("Depth");
	case column_module: return QStringLiteral("Module");
	case column_base: return QStringLiteral("Base+Offset");
	case column_chain: return QStringLiteral("Chain");
	case column_valid: return QStringLiteral("Valid");
	default: return {};
	}
}

}
