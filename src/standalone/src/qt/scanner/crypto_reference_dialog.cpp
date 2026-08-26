#include "qt/scanner/crypto_reference_dialog.hpp"

#include <QHeaderView>
#include <QLabel>
#include <QTableView>
#include <QVBoxLayout>

#include <cstdio>

#include "helpers/diag_log.hpp"

#include "qt/docking/dock_host.hpp"
#include "qt/scanner/crypto_controller.hpp"
#include "qt/scanner/scanner_palette.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::scanner {

CryptoReferenceDialog::reference_model_t::reference_model_t(
	disasm_view::workspace_context_t context, std::vector<std::uint64_t> choices,
	QObject* parent)
	: QAbstractTableModel(parent), context_(std::move(context)),
	choices_(std::move(choices)) {}

int CryptoReferenceDialog::reference_model_t::rowCount(
	const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : static_cast<int>(choices_.size());
}

int CryptoReferenceDialog::reference_model_t::columnCount(
	const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : 1;
}

std::uint64_t CryptoReferenceDialog::reference_model_t::address_at(
	int row) const noexcept
{
	if (row < 0 || row >= static_cast<int>(choices_.size()))
		return 0;
	return choices_[static_cast<std::size_t>(row)];
}

QVariant CryptoReferenceDialog::reference_model_t::data(const QModelIndex& index,
	int role) const
{
	if (!index.isValid() || index.parent().isValid())
		return {};
	const std::uint64_t address = address_at(index.row());
	if (address == 0)
		return {};
	if (role == Qt::DisplayRole) {
		char raw[32]{};
		std::snprintf(raw, sizeof(raw), "0x%016llX",
			static_cast<unsigned long long>(address));
		QString label = QString::fromLatin1(raw);
		if (const auto typed = disasm_view::typed_address(context_, address)) {
			const std::string name = disasm_view::resolve_name(context_, *typed);
			if (!name.empty())
				label += QStringLiteral("  ") + QString::fromStdString(name);
		}
		return label;
	}
	if (role == Qt::FontRole)
		return theme::fonts::codeRegular();
	if (role == Qt::ForegroundRole)
		return theme::tokens().text_primary;
	if (role == Qt::TextAlignmentRole)
		return int(Qt::AlignVCenter | Qt::AlignLeft);
	return {};
}

void CryptoReferenceDialog::reference_model_t::multiData(const QModelIndex& index,
	QModelRoleDataSpan span) const
{
	for (QModelRoleData& role_data : span) {
		switch (role_data.role()) {
		case Qt::DisplayRole:
		case Qt::FontRole:
		case Qt::ForegroundRole:
		case Qt::TextAlignmentRole:
			role_data.setData(data(index, role_data.role()));
			break;
		default:
			role_data.clearData();
			break;
		}
	}
}

CryptoReferenceDialog::CryptoReferenceDialog(
	disasm_view::workspace_context_t context, std::vector<std::uint64_t> choices,
	std::uint64_t publication_generation, QWidget* parent)
	: bridge::AidaDialog(parent), context_(std::move(context)),
	publication_generation_(publication_generation)
{
	setObjectName(QStringLiteral("aida.memory.crypto.reference_dialog"));
	setWindowTitle(QStringLiteral("Crypto References"));
	setModal(true);
	setAttribute(Qt::WA_DeleteOnClose);
	auto* layout = new QVBoxLayout(this);
	const bool current = context_.publication &&
		context_.publication->generation == publication_generation_;
	auto* count_label = new QLabel(QStringLiteral("%1 retained references")
		.arg(choices.size()), this);
	layout->addWidget(count_label);
	if (!current) {
		auto* stale = new QLabel(QStringLiteral(
			"The analysis publication changed; reopen the hit context menu."), this);
		stale->setEnabled(false);
		layout->addWidget(stale);
	}
	model_ = new reference_model_t(context_, std::move(choices), this);
	table_ = new QTableView(this);
	table_->setObjectName(QStringLiteral("aida.memory.crypto.reference_dialog.table"));
	table_->verticalHeader()->setVisible(false);
	table_->verticalHeader()->setDefaultSectionSize(theme::tokens().table.compact_row_h);
	table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	table_->horizontalHeader()->setVisible(false);
	table_->setShowGrid(false);
	table_->setWordWrap(false);
	table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_->setSelectionMode(QAbstractItemView::SingleSelection);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
	table_->setModel(model_);
	table_->setEnabled(current);
	table_->setMinimumWidth(52 * mono_cell_width());
	layout->addWidget(table_, 1);
	connect(table_, &QTableView::activated, this,
		[this](const QModelIndex& index) {
			if (index.isValid())
				activate_row(index.row());
		});
	if (current && model_->rowCount() > 0) {
		const QModelIndex first = model_->index(0, 0);
		table_->setCurrentIndex(first);
		table_->scrollTo(first, QAbstractItemView::PositionAtTop);
	}
}

void CryptoReferenceDialog::activate_row(int row)
{
	if (!context_.publication ||
		context_.publication->generation != publication_generation_)
		return;
	const std::uint64_t address = model_->address_at(row);
	if (address == 0)
		return;
	if (auto* host = CryptoController::instance().host()) {
		static_cast<void>(host->open_or_focus(
			registry::stable_view_id_t("document.disassembly")));
	}
	disasm_view::goto_address(address, context_);
	diag::log_tagged("scan_audit", "[scan_audit] crypto_scanner ctx show_ref");
	accept();
}

}
