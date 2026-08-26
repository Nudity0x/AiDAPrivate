#pragma once

#include <QAbstractTableModel>
#include <QColor>

#include <vector>

#include "core/scanner/aob_generator.hpp"

namespace aida::qt::scanner {

class AobSavedModel : public QAbstractTableModel {
	Q_OBJECT
public:
	enum column_t : int {
		column_grade = 0,
		column_name = 1,
		column_address = 2,
		column_size = 3,
		column_uniqueness = 4,
		column_count = 5
	};

	static constexpr int role_quality = Qt::UserRole + 1;
	static constexpr int role_unique = Qt::UserRole + 2;

	explicit AobSavedModel(QObject* parent = nullptr);

	void adopt(std::vector<aob_generator::signature_t> rows);
	const aob_generator::signature_t* row_at(int row) const noexcept;
	std::size_t size() const noexcept { return rows_.size(); }

	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;
	QVariant data(const QModelIndex& index, int role) const override;
	void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override;
	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override;

	static QColor grade_color(float quality_score);

private:
	std::vector<aob_generator::signature_t> rows_;
};

}
