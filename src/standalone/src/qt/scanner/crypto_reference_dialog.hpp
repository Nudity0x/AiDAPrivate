#pragma once

#include <QAbstractTableModel>
#include <QString>

#include <cstdint>
#include <vector>

#include "core/disasm/disasm_view.hpp"
#include "qt/bridge/aida_dialog.hpp"

class QTableView;

namespace aida::qt::scanner {

class CryptoReferenceDialog : public bridge::AidaDialog {
	Q_OBJECT
public:
	CryptoReferenceDialog(disasm_view::workspace_context_t context,
		std::vector<std::uint64_t> choices, std::uint64_t publication_generation,
		QWidget* parent = nullptr);

private:
	void activate_row(int row);

	class reference_model_t : public QAbstractTableModel {
	public:
		reference_model_t(disasm_view::workspace_context_t context,
			std::vector<std::uint64_t> choices, QObject* parent = nullptr);
		int rowCount(const QModelIndex& parent = QModelIndex()) const override;
		int columnCount(const QModelIndex& parent = QModelIndex()) const override;
		QVariant data(const QModelIndex& index, int role) const override;
		void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override;
		std::uint64_t address_at(int row) const noexcept;

	private:
		disasm_view::workspace_context_t context_;
		std::vector<std::uint64_t> choices_;
	};

	disasm_view::workspace_context_t context_;
	std::uint64_t publication_generation_ = 0;
	reference_model_t* model_ = nullptr;
	QTableView* table_ = nullptr;
};

}
