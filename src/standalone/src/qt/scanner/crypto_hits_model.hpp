#pragma once

#include <QAbstractTableModel>
#include <QColor>

#include <memory>

#include "core/scanner/crypto_scanner.hpp"

namespace aida::qt::scanner {

class CryptoHitsModel : public QAbstractTableModel {
	Q_OBJECT
public:
	enum column_t : int {
		column_algorithm = 0,
		column_signature = 1,
		column_address = 2,
		column_module = 3,
		column_category = 4,
		column_refs = 5,
		column_count = 6
	};

	static constexpr int role_category = Qt::UserRole + 1;

	explicit CryptoHitsModel(QObject* parent = nullptr);

	void adopt(std::shared_ptr<const std::vector<crypto_scanner::crypto_hit_t>> rows);
	const crypto_scanner::crypto_hit_t* row_at(int row) const noexcept;
	std::size_t size() const noexcept { return rows_ ? rows_->size() : 0; }

	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;
	QVariant data(const QModelIndex& index, int role) const override;
	void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override;
	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override;

	static QColor category_color(crypto_scanner::crypto_category_t category);

private:
	std::shared_ptr<const std::vector<crypto_scanner::crypto_hit_t>> rows_;
};

}
