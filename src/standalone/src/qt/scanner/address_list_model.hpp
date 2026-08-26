#pragma once

#include <QAbstractTableModel>

#include <cstdint>
#include <vector>

#include "core/scanner/memory_scanner.hpp"

namespace aida::qt::scanner {

class AddressListModel : public QAbstractTableModel {
	Q_OBJECT
public:
	enum column_t : int {
		column_active = 0,
		column_description = 1,
		column_address = 2,
		column_type = 3,
		column_value = 4,
		column_count = 5
	};

	static constexpr int role_frozen = Qt::UserRole + 1;

	explicit AddressListModel(QObject* parent = nullptr);

	void reset_entries(std::vector<memory_scanner::address_entry_t> entries);
	void refresh_values(std::vector<memory_scanner::address_entry_t> entries);
	void patch_row(int row);

	const memory_scanner::address_entry_t* entry_at(int row) const noexcept;
	std::size_t entry_count() const noexcept { return entries_.size(); }

	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;
	QVariant data(const QModelIndex& index, int role) const override;
	void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override;
	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override;

private:
	std::vector<memory_scanner::address_entry_t> entries_;
};

}
