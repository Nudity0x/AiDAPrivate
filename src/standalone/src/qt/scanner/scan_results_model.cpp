#include "qt/scanner/scan_results_model.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::scanner {

namespace {

const char* region_kind_label(std::uint32_t type, std::uint32_t state)
{
	if (state != 0x1000) return "Unmapped";
	if (type == 0x1000000) return "Image";
	if (type == 0x40000)   return "Mapped";
	if (type == 0x20000)   return "Private";
	return "Region";
}

}

ScanResultsModel::ScanResultsModel(QObject* parent) : QAbstractTableModel(parent) {}

void ScanResultsModel::adopt(
	std::shared_ptr<const std::vector<memory_scanner::scan_result_t>> snapshot,
	std::uint64_t scan_revision, memory_scanner::value_type_t value_type,
	int scan_count, std::size_t total_found)
{
	beginResetModel();
	snapshot_ = std::move(snapshot);
	scan_revision_ = scan_revision;
	value_type_ = value_type;
	scan_count_ = scan_count;
	total_found_ = total_found;
	order_.clear();
	inverse_order_.clear();
	flash_.clear();
	const std::size_t rows = snapshot_ ? snapshot_->size() : 0;
	if (rows > 0 && rows <= 10000) {
		flash_.assign(rows, quint8{0});
		const bool initial = scan_count <= 1;
		bool any = false;
		for (std::size_t index = 0; index < rows; ++index) {
			const auto& result = (*snapshot_)[index];
			const bool changed = !result.previous_value.empty() &&
				result.current_value != result.previous_value;
			if (initial || changed) {
				flash_[index] = 1;
				any = true;
			}
		}
		if (any)
			flash_clock_.start();
	}
	endResetModel();
}

void ScanResultsModel::apply_order(std::vector<int> order)
{
	if (!snapshot_ || order.size() != snapshot_->size())
		order.clear();
	Q_EMIT layoutAboutToBeChanged();
	const QModelIndexList persistent = persistentIndexList();
	order_ = std::move(order);
	inverse_order_.assign(order_.empty() ? 0 : order_.size(), 0);
	if (!order_.empty()) {
		for (std::size_t view_row = 0; view_row < order_.size(); ++view_row)
			inverse_order_[static_cast<std::size_t>(order_[view_row])] =
				static_cast<int>(view_row);
	}
	QModelIndexList updated;
	updated.reserve(persistent.size());
	for (const QModelIndex& index : persistent) {
		if (!index.isValid()) {
			updated << index;
			continue;
		}
		const int source = index.row();
		const int view = source >= 0 && source < static_cast<int>(order_.size())
			? inverse_order_[static_cast<std::size_t>(source)] : source;
		updated << this->index(view, index.column(), index.parent());
	}
	changePersistentIndexList(persistent, updated);
	Q_EMIT layoutChanged();
}

void ScanResultsModel::set_regions(
	std::shared_ptr<const std::vector<region_cache_entry_t>> regions)
{
	if (regions_ == regions)
		return;
	regions_ = std::move(regions);
	const int rows = rowCount();
	if (rows > 0)
		Q_EMIT dataChanged(index(0, column_module), index(rows - 1, column_module),
			{Qt::DisplayRole});
}

int ScanResultsModel::source_row(int view_row) const noexcept
{
	if (order_.empty())
		return view_row;
	if (view_row < 0 || view_row >= static_cast<int>(order_.size()))
		return -1;
	return order_[static_cast<std::size_t>(view_row)];
}

int ScanResultsModel::view_row_for_source(int source_row) const noexcept
{
	if (order_.empty())
		return source_row;
	if (source_row < 0 || source_row >= static_cast<int>(inverse_order_.size()))
		return -1;
	return inverse_order_[static_cast<std::size_t>(source_row)];
}

const memory_scanner::scan_result_t* ScanResultsModel::result_at_source(
	int source_row) const noexcept
{
	if (!snapshot_ || source_row < 0 ||
		source_row >= static_cast<int>(snapshot_->size()))
		return nullptr;
	return &(*snapshot_)[static_cast<std::size_t>(source_row)];
}

const memory_scanner::scan_result_t* ScanResultsModel::result_at_view(
	int view_row) const noexcept
{
	return result_at_source(source_row(view_row));
}

QString ScanResultsModel::raw_module_label_at_source(int source_row) const
{
	const auto* result = result_at_source(source_row);
	if (!result)
		return {};
	const auto regions = regions_;
	return regions ? module_label(*result, *regions) : module_label(*result, {});
}

int ScanResultsModel::flash_duration_msec()
{
	return theme::tokens().motion.xxl;
}

bool ScanResultsModel::flash_active() const
{
	return !flash_.empty() && flash_clock_.isValid() &&
		flash_clock_.elapsed() < flash_duration_msec();
}

qreal ScanResultsModel::flash_alpha_for_source(int source_row) const
{
	if (source_row < 0 || source_row >= static_cast<int>(flash_.size()) ||
		flash_[static_cast<std::size_t>(source_row)] == 0 || !flash_clock_.isValid())
		return 0.0;
	const qreal value = 1.0 - static_cast<qreal>(flash_clock_.elapsed()) /
		static_cast<qreal>(flash_duration_msec());
	return value > 0.0 ? value : 0.0;
}

int ScanResultsModel::rowCount(const QModelIndex& parent) const
{
	if (parent.isValid() || !snapshot_)
		return 0;
	return static_cast<int>((std::min)(snapshot_->size(),
		static_cast<std::size_t>((std::numeric_limits<int>::max)())));
}

int ScanResultsModel::columnCount(const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : static_cast<int>(column_count);
}

QString ScanResultsModel::module_label(const memory_scanner::scan_result_t& result,
	const std::vector<region_cache_entry_t>& regions)
{
	if (!result.module_name.empty()) {
		char buf[160]{};
		std::snprintf(buf, sizeof(buf), "%s+0x%llX",
			result.module_name.c_str(),
			static_cast<unsigned long long>(result.module_offset));
		return QString::fromLatin1(buf);
	}
	if (regions.empty())
		return {};
	auto it = std::upper_bound(regions.begin(), regions.end(), result.address,
		[](std::uint64_t address, const region_cache_entry_t& entry) {
			return address < entry.base;
		});
	if (it == regions.begin())
		return {};
	--it;
	if (result.address < it->base || result.address >= it->end)
		return {};
	char buf[96]{};
	std::snprintf(buf, sizeof(buf), "%s+0x%llX", region_kind_label(it->type, it->state),
		static_cast<unsigned long long>(result.address - it->base));
	return QString::fromLatin1(buf);
}

QColor ScanResultsModel::value_color(const memory_scanner::scan_result_t& result,
	const QColor& success, const QColor& error, const QColor& text_primary)
{
	if (result.previous_value.empty() || result.current_value.empty())
		return success;
	std::int64_t current = 0;
	std::int64_t previous = 0;
	std::memcpy(&current, result.current_value.data(),
		(std::min)(result.current_value.size(), sizeof(std::int64_t)));
	std::memcpy(&previous, result.previous_value.data(),
		(std::min)(result.previous_value.size(), sizeof(std::int64_t)));
	if (current > previous)
		return success;
	if (current < previous)
		return error;
	return text_primary;
}

QVariant ScanResultsModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || index.parent().isValid())
		return {};
	const auto* result = result_at_view(index.row());
	if (!result)
		return {};
	const auto& tokens = theme::tokens();
	if (role == Qt::ToolTipRole)
		role = Qt::DisplayRole;
	switch (role) {
	case Qt::DisplayRole: {
		switch (index.column()) {
		case column_address: {
			char buf[24]{};
			std::snprintf(buf, sizeof(buf), "0x%016llX",
				static_cast<unsigned long long>(result->address));
			return QString::fromLatin1(buf);
		}
		case column_value:
			return QString::fromStdString(memory_scanner::format_value(
				result->current_value, value_type_));
		case column_previous:
			return QString::fromStdString(memory_scanner::format_value(
				result->previous_value, value_type_));
		case column_module: {
			const auto regions = regions_;
			QString label = regions
				? module_label(*result, *regions) : module_label(*result, {});
			if (label.isEmpty())
				label = QStringLiteral("Unknown");
			return label;
		}
		default:
			return {};
		}
	}
	case Qt::FontRole:
		return index.column() == column_module
			? QVariant(theme::fonts::body())
			: QVariant(theme::fonts::codeRegular());
	case Qt::ForegroundRole:
		switch (index.column()) {
		case column_address: return tokens.text_address;
		case column_value:
			return value_color(*result, tokens.success, tokens.error,
				tokens.text_primary);
		case column_previous: return tokens.text_dim;
		case column_module: {
			const auto regions = regions_;
			const bool known = regions
				? !module_label(*result, *regions).isEmpty()
				: !result->module_name.empty();
			return known ? tokens.text_secondary : tokens.text_dim;
		}
		default: return {};
		}
	case Qt::TextAlignmentRole:
		return int(Qt::AlignVCenter | Qt::AlignLeft);
	case role_flash:
		return flash_alpha_for_source(source_row(index.row()));
	default:
		return {};
	}
}

void ScanResultsModel::multiData(const QModelIndex& index,
	QModelRoleDataSpan span) const
{
	for (QModelRoleData& role_data : span) {
		switch (role_data.role()) {
		case Qt::DisplayRole:
		case Qt::ToolTipRole:
		case Qt::FontRole:
		case Qt::ForegroundRole:
		case Qt::TextAlignmentRole:
		case role_flash:
			role_data.setData(data(index, role_data.role()));
			break;
		default:
			role_data.clearData();
			break;
		}
	}
}

QVariant ScanResultsModel::headerData(int section, Qt::Orientation orientation,
	int role) const
{
	if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
		return {};
	switch (section) {
	case column_address: return QStringLiteral("Address");
	case column_value: return QStringLiteral("Value");
	case column_previous: return QStringLiteral("Previous");
	case column_module: return QStringLiteral("Module / Region");
	default: return {};
	}
}

}
