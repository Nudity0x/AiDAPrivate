#pragma once

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QColor>
#include <QFont>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/scanner/memory_scanner.hpp"

namespace aida::qt::scanner {

struct region_cache_entry_t {
	std::uint64_t base = 0;
	std::uint64_t end = 0;
	std::uint32_t state = 0;
	std::uint32_t protect = 0;
	std::uint32_t type = 0;
};

enum class result_sort_t : int {
	by_index = 0,
	by_address,
	by_value,
	by_previous,
	by_module
};

class ScanResultsModel : public QAbstractTableModel {
	Q_OBJECT
public:
	enum column_t : int {
		column_address = 0,
		column_value = 1,
		column_previous = 2,
		column_module = 3,
		column_count = 4
	};

	static constexpr int role_flash = Qt::UserRole + 1;

	explicit ScanResultsModel(QObject* parent = nullptr);

	void adopt(std::shared_ptr<const std::vector<memory_scanner::scan_result_t>> snapshot,
		std::uint64_t scan_revision, memory_scanner::value_type_t value_type,
		int scan_count, std::size_t total_found);
	void apply_order(std::vector<int> order);
	void set_regions(std::shared_ptr<const std::vector<region_cache_entry_t>> regions);

	const std::shared_ptr<const std::vector<memory_scanner::scan_result_t>>& snapshot() const noexcept
		{ return snapshot_; }
	memory_scanner::value_type_t value_type() const noexcept { return value_type_; }
	std::uint64_t scan_revision() const noexcept { return scan_revision_; }
	std::size_t total_found() const noexcept { return total_found_; }
	int scan_count() const noexcept { return scan_count_; }

	int source_row(int view_row) const noexcept;
	int view_row_for_source(int source_row) const noexcept;
	const memory_scanner::scan_result_t* result_at_source(int source_row) const noexcept;
	const memory_scanner::scan_result_t* result_at_view(int view_row) const noexcept;
	QString raw_module_label_at_source(int source_row) const;

	bool flash_active() const;
	qreal flash_alpha_for_source(int source_row) const;
	static int flash_duration_msec();

	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;
	QVariant data(const QModelIndex& index, int role) const override;
	void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override;
	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override;

	static QString module_label(const memory_scanner::scan_result_t& result,
		const std::vector<region_cache_entry_t>& regions);
	static QColor value_color(const memory_scanner::scan_result_t& result,
		const QColor& success, const QColor& error, const QColor& text_primary);

private:
	std::shared_ptr<const std::vector<memory_scanner::scan_result_t>> snapshot_;
	std::shared_ptr<const std::vector<region_cache_entry_t>> regions_;
	std::vector<int> order_;
	std::vector<int> inverse_order_;
	std::vector<quint8> flash_;
	QElapsedTimer flash_clock_;
	memory_scanner::value_type_t value_type_ = memory_scanner::value_type_t::int32_val;
	std::uint64_t scan_revision_ = 0;
	std::size_t total_found_ = 0;
	int scan_count_ = 0;
};

}
