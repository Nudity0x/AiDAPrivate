#include "qt/scanner/scan_results_view.hpp"

#include <QHeaderView>
#include <QKeyEvent>
#include <QVariantAnimation>
#include <QItemSelectionModel>

#include "qt/scanner/memory_interaction_bridge.hpp"
#include "qt/scanner/scan_result_delegate.hpp"
#include "qt/scanner/scan_results_model.hpp"
#include "qt/scanner/scanner_palette.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::scanner {

ScanResultsView::ScanResultsView(QWidget* parent) : QTableView(parent)
{
	setObjectName(QStringLiteral("aida.memory.scan_results_view"));
	verticalHeader()->setVisible(false);
	verticalHeader()->setDefaultSectionSize(theme::tokens().table.row_h);
	verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
	horizontalHeader()->setStretchLastSection(true);
	horizontalHeader()->setSectionsClickable(true);
	horizontalHeader()->setMinimumHeight(theme::tokens().table.header_h);
	setShowGrid(false);
	setWordWrap(false);
	setSelectionBehavior(QAbstractItemView::SelectRows);
	setSelectionMode(QAbstractItemView::ExtendedSelection);
	setEditTriggers(QAbstractItemView::NoEditTriggers);
	setAlternatingRowColors(true);
	setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
	setContextMenuPolicy(Qt::CustomContextMenu);

	delegate_ = new ScanResultDelegate(this);
	setItemDelegate(delegate_);

	flash_anim_ = new QVariantAnimation(this);
	flash_anim_->setStartValue(0.0);
	flash_anim_->setEndValue(1.0);
	flash_anim_->setDuration(ScanResultsModel::flash_duration_msec());
	connect(flash_anim_, &QVariantAnimation::valueChanged, this,
		[this](const QVariant&) { viewport()->update(); });
	connect(flash_anim_, &QVariantAnimation::finished, this,
		[this] { viewport()->update(); });

	connect(horizontalHeader(), &QHeaderView::sectionClicked, this,
		[this](int logical) { Q_EMIT sortRequested(logical); });
	connect(this, &QTableView::activated, this,
		[this](const QModelIndex& index) {
			if (model_ && index.isValid())
				Q_EMIT rowActivated(model_->source_row(index.row()));
		});
	connect(this, &QTableView::customContextMenuRequested, this,
		[this](const QPoint& pos) {
			const QModelIndex index = indexAt(pos);
			if (model_ && index.isValid() &&
				!selectionModel()->isSelected(index)) {
				setCurrentIndex(index);
				selectionModel()->select(index,
					QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
			}
			const int source = model_ && index.isValid()
				? model_->source_row(index.row()) : -1;
			Q_EMIT contextMenuRequested(viewport()->mapToGlobal(pos), source, 0);
		});
}

ScanResultsView::~ScanResultsView() = default;

void ScanResultsView::sync_sort_indicator(int field, bool descending)
{
	auto* header = horizontalHeader();
	if (field <= 0) {
		header->setSortIndicatorShown(false);
		return;
	}
	header->setSortIndicatorShown(true);
	const int column = field == 1 ? ScanResultsModel::column_address
		: field == 2 ? ScanResultsModel::column_value
		: field == 3 ? ScanResultsModel::column_previous
		: ScanResultsModel::column_module;
	header->setSortIndicator(column,
		descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void ScanResultsView::bind(ScanResultsModel* model, MemoryInteractionBridge* bridge,
	const QString& owner_view_id)
{
	model_ = model;
	bridge_ = bridge;
	owner_view_id_ = owner_view_id;
	setObjectName(owner_view_id_ + QStringLiteral(".table"));
	setModel(model_);
	if (bridge_)
		bridge_->attach_results_view(this);
	connect(model_, &QAbstractItemModel::modelReset, this, [this] {
		tickFlash();
	});
	connect(model_, &QAbstractItemModel::layoutChanged, this, [this] {
		tickFlash();
	});
	const auto& tokens = theme::tokens();
	const int pad_x = tokens.table.cell_pad_x;
	const QFontMetricsF code_metrics(theme::fonts::codeRegular());
	setColumnWidth(ScanResultsModel::column_address, 2 * pad_x +
		static_cast<int>(code_metrics.horizontalAdvance(
			QStringLiteral("0x00007FFA1B2C3D4E")) + 0.5));
	const int value_width = 2 * pad_x +
		static_cast<int>(code_metrics.horizontalAdvance(
			QStringLiteral("-1.23456789E+300")) + 0.5);
	setColumnWidth(ScanResultsModel::column_value, value_width);
	setColumnWidth(ScanResultsModel::column_previous, value_width);
	applyResponsiveColumns();
}

void ScanResultsView::applyResponsiveColumns()
{
	const int w = viewport()->width();
	const int cell = mono_cell_width();
	auto* header = horizontalHeader();
	header->setSectionHidden(ScanResultsModel::column_previous, w < 62 * cell);
	header->setSectionHidden(ScanResultsModel::column_module, w < 40 * cell);
}

void ScanResultsView::resizeEvent(QResizeEvent* event)
{
	QTableView::resizeEvent(event);
	applyResponsiveColumns();
}

void ScanResultsView::tickFlash()
{
	if (!model_ || !model_->flash_active() ||
		theme::AidaMotion::reducedMotion()) {
		flash_anim_->stop();
		viewport()->update();
		return;
	}
	flash_anim_->stop();
	flash_anim_->setDuration(ScanResultsModel::flash_duration_msec());
	flash_anim_->start();
}

void ScanResultsView::keyPressEvent(QKeyEvent* event)
{
	if (model_ && (event->key() == Qt::Key_Menu ||
		(event->key() == Qt::Key_F10 &&
			(event->modifiers() & Qt::ShiftModifier)))) {
		const QModelIndex current = currentIndex();
		if (current.isValid()) {
			const QRect cell = visualRect(current);
			const QPoint global = viewport()->mapToGlobal(
				QPoint(cell.center().x(), cell.bottom()));
			Q_EMIT contextMenuRequested(global,
				model_->source_row(current.row()),
				event->key() == Qt::Key_Menu ? 1 : 2);
			event->accept();
			return;
		}
	}
	QTableView::keyPressEvent(event);
}

}
