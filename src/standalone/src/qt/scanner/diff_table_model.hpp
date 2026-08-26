#pragma once

#include <QAbstractTableModel>
#include <QColor>

#include <memory>

#include "core/scanner/snapshot_diff.hpp"

namespace aida::qt::scanner {

class DiffTableModel : public QAbstractTableModel {
	Q_OBJECT
public:
	enum column_t : int {
		column_address = 0,
		column_old = 1,
		column_new = 2,
		column_type = 3,
		column_module = 4,
		column_count = 5
	};

	static constexpr int role_change_type = Qt::UserRole + 1;

	explicit DiffTableModel(QObject* parent = nullptr);

	void adopt(std::shared_ptr<const snapshot_diff::diff_result_t> diff);
	const snapshot_diff::changed_region_t* row_at(int row) const noexcept;
	std::size_t size() const noexcept { return diff_ ? diff_->changes.size() : 0; }
	bool truncated() const noexcept { return diff_ && diff_->truncated; }

	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;
	QVariant data(const QModelIndex& index, int role) const override;
	void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override;
	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override;

	static QColor change_color(snapshot_diff::change_type_t type);

private:
	std::shared_ptr<const snapshot_diff::diff_result_t> diff_;
};

}
