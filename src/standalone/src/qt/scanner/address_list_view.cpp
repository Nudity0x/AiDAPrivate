#include "qt/scanner/address_list_view.hpp"

#include <QHeaderView>
#include <QKeyEvent>
#include <QItemSelectionModel>

#include <algorithm>

#include "qt/scanner/address_item_delegate.hpp"
#include "qt/scanner/address_list_model.hpp"
#include "qt/scanner/memory_interaction_bridge.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::scanner {

AddressListView::AddressListView(QWidget* parent) : QTableView(parent)
{
	setObjectName(QStringLiteral("aida.memory.address_list_view"));
	verticalHeader()->setVisible(false);
	verticalHeader()->setDefaultSectionSize(theme::tokens().table.row_h);
	verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
	horizontalHeader()->setStretchLastSection(false);
	horizontalHeader()->setSectionsClickable(false);
	horizontalHeader()->setMinimumHeight(theme::tokens().table.header_h);
	setShowGrid(false);
	setWordWrap(false);
	setSelectionBehavior(QAbstractItemView::SelectRows);
	setSelectionMode(QAbstractItemView::ExtendedSelection);
	setEditTriggers(QAbstractItemView::NoEditTriggers);
	setAlternatingRowColors(true);
	setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
	setContextMenuPolicy(Qt::CustomContextMenu);

	delegate_ = new AddressItemDelegate(this);
	setItemDelegate(delegate_);
	delegate_->set_freeze_toggle_handler([this](int row) {
		Q_EMIT freezeToggleRequested(row);
	});

	connect(this, &QTableView::activated, this, [this](const QModelIndex& index) {
		if (!index.isValid())
			return;
		switch (index.column()) {
		case AddressListModel::column_description:
			Q_EMIT editDescriptionRequested(index.row());
			break;
		case AddressListModel::column_value:
			Q_EMIT changeValueRequested(index.row());
			break;
		case AddressListModel::column_type:
			Q_EMIT changeTypeRequested(index.row());
			break;
		default:
			break;
		}
	});
	connect(this, &QTableView::customContextMenuRequested, this,
		[this](const QPoint& pos) {
			const QModelIndex index = indexAt(pos);
			if (index.isValid() && !selectionModel()->isSelected(index)) {
				setCurrentIndex(index);
				selectionModel()->select(index,
					QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
			}
			Q_EMIT contextMenuRequested(viewport()->mapToGlobal(pos),
				index.isValid() ? index.row() : -1, 0);
		});
}

AddressListView::~AddressListView() = default;

void AddressListView::bind(AddressListModel* model, MemoryInteractionBridge* bridge,
	const QString& owner_view_id)
{
	model_ = model;
	bridge_ = bridge;
	owner_view_id_ = owner_view_id;
	setObjectName(owner_view_id_ + QStringLiteral(".table"));
	setModel(model_);
	auto* header = horizontalHeader();
	const auto& tokens = theme::tokens();
	const int pad_x = tokens.table.cell_pad_x;
	const QFontMetricsF body_metrics(theme::fonts::body());
	const int active_width = tokens.table.cell_pad_x + tokens.control.checkbox +
		pad_x + static_cast<int>(body_metrics.horizontalAdvance(
			QStringLiteral("Locked")) + 0.5) + pad_x;
	header->setSectionResizeMode(AddressListModel::column_active,
		QHeaderView::Fixed);
	setColumnWidth(AddressListModel::column_active, active_width);
	int type_text_width = 0;
	for (int i = 0; i < static_cast<int>(memory_scanner::value_type_t::COUNT); ++i)
		type_text_width = (std::max)(type_text_width, static_cast<int>(
			body_metrics.horizontalAdvance(QString::fromLatin1(
				memory_scanner::value_type_name(
					static_cast<memory_scanner::value_type_t>(i)))) + 0.5));
	header->setSectionResizeMode(AddressListModel::column_type,
		QHeaderView::Fixed);
	setColumnWidth(AddressListModel::column_type,
		type_text_width + 2 * pad_x);
	const QFontMetricsF code_metrics(theme::fonts::codeRegular());
	setColumnWidth(AddressListModel::column_address, 2 * pad_x +
		static_cast<int>(code_metrics.horizontalAdvance(
			QStringLiteral("0x00007FFA1B2C3D4E")) + 0.5));
	setColumnWidth(AddressListModel::column_value, 2 * pad_x +
		static_cast<int>(code_metrics.horizontalAdvance(
			QStringLiteral("-1.23456789E+300")) + 0.5));
	header->setSectionResizeMode(AddressListModel::column_description,
		QHeaderView::Stretch);
	if (bridge_)
		bridge_->attach_address_view(this);
}

void AddressListView::keyPressEvent(QKeyEvent* event)
{
	if (event->key() == Qt::Key_Delete) {
		std::vector<int> rows;
		if (selectionModel()) {
			const QItemSelection selection = selectionModel()->selection();
			for (const QItemSelectionRange& range : selection) {
				for (int row = range.top(); row <= range.bottom(); ++row) {
					if (rows.size() >=
						MemoryInteractionBridge::k_maximum_selected_contexts)
						break;
					rows.push_back(row);
				}
				if (rows.size() >= MemoryInteractionBridge::k_maximum_selected_contexts)
					break;
			}
		}
		if (rows.empty()) {
			const QModelIndex current = currentIndex();
			if (current.isValid())
				rows.push_back(current.row());
		}
		if (!rows.empty()) {
			Q_EMIT removeRequested(rows);
			event->accept();
			return;
		}
	}
	if (event->key() == Qt::Key_Menu ||
		(event->key() == Qt::Key_F10 &&
			(event->modifiers() & Qt::ShiftModifier))) {
		const QModelIndex current = currentIndex();
		if (current.isValid()) {
			const QRect cell = visualRect(current);
			const QPoint global = viewport()->mapToGlobal(
				QPoint(cell.center().x(), cell.bottom()));
			Q_EMIT contextMenuRequested(global, current.row(),
				event->key() == Qt::Key_Menu ? 1 : 2);
			event->accept();
			return;
		}
	}
	QTableView::keyPressEvent(event);
}

}
