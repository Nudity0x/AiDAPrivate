#pragma once

#include <QAbstractTableModel>
#include <QColor>

#include <vector>

#include "core/scanner/pointer_scanner.hpp"

namespace aida::qt::scanner {

class PointerChainModel : public QAbstractTableModel {
	Q_OBJECT
public:
	enum column_t : int {
		column_depth = 0,
		column_module = 1,
		column_base = 2,
		column_chain = 3,
		column_valid = 4,
		column_count = 5
	};

	enum class validity_t : int { unknown = 0, busy, valid };

	static constexpr int role_validity = Qt::UserRole + 1;

	explicit PointerChainModel(QObject* parent = nullptr);

	void adopt(std::vector<pointer_scanner::pointer_chain_t> rows);
	void refresh_validity();
	const pointer_scanner::pointer_chain_t* row_at(int row) const noexcept;
	std::size_t size() const noexcept { return rows_.size(); }

	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;
	QVariant data(const QModelIndex& index, int role) const override;
	void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override;
	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override;

private:
	struct row_t {
		pointer_scanner::pointer_chain_t chain;
		QString chain_text;
		validity_t validity = validity_t::unknown;
	};

	validity_t probe_validity(const pointer_scanner::pointer_chain_t& chain) const;

	std::vector<row_t> rows_;
};

}
