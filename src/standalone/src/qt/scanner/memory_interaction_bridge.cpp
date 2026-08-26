#include "qt/scanner/memory_interaction_bridge.hpp"

#include <QItemSelectionModel>
#include <QSignalBlocker>
#include <QTimer>

#include <algorithm>

#include "qt/chrome/aida_toast.hpp"
#include "qt/scanner/address_list_model.hpp"
#include "qt/scanner/address_list_view.hpp"
#include "qt/scanner/scan_results_model.hpp"
#include "qt/scanner/scan_results_view.hpp"

namespace aida::qt::scanner {

MemoryInteractionBridge::MemoryInteractionBridge(QObject* parent,
	QString owner_view_id, memory_interaction::kind_t kind)
	: QObject(parent), owner_view_id_(std::move(owner_view_id)), kind_(kind)
{
	sync_timer_ = new QTimer(this);
	sync_timer_->setInterval(250);
	connect(sync_timer_, &QTimer::timeout, this, [this] { synchronize_now(); });
}

void MemoryInteractionBridge::set_runtime_source(
	std::function<memory_interaction::runtime_t()> runtime_source)
{
	runtime_source_ = std::move(runtime_source);
}

void MemoryInteractionBridge::attach_results_view(ScanResultsView* view)
{
	results_view_ = view;
	if (!results_view_ || !results_view_->scan_model())
		return;
	connect(results_view_, &QObject::destroyed, this, [this] {
		results_view_ = nullptr;
	});
	connect(results_view_->selectionModel(), &QItemSelectionModel::selectionChanged,
		this, [this](const QItemSelection&, const QItemSelection&) {
			on_view_selection();
		});
	auto* model = results_view_->scan_model();
	connect(model, &QAbstractItemModel::layoutAboutToBeChanged, this,
		[this] { on_layout_about_to_change(); });
	connect(model, &QAbstractItemModel::layoutChanged, this,
		[this] { on_layout_changed(); });
	connect(model, &QAbstractItemModel::modelReset, this,
		[this] { on_model_reset(); });
}

void MemoryInteractionBridge::attach_address_view(AddressListView* view)
{
	address_view_ = view;
	if (!address_view_ || !address_view_->address_model())
		return;
	connect(address_view_, &QObject::destroyed, this, [this] {
		address_view_ = nullptr;
	});
	connect(address_view_->selectionModel(), &QItemSelectionModel::selectionChanged,
		this, [this](const QItemSelection&, const QItemSelection&) {
			on_view_selection();
		});
	auto* model = address_view_->address_model();
	connect(model, &QAbstractItemModel::modelReset, this,
		[this] { on_model_reset(); });
}

std::vector<int> MemoryInteractionBridge::selected_view_rows(bool* truncated) const
{
	std::vector<int> rows;
	bool capped = false;
	QTableView* view = results_view_
		? static_cast<QTableView*>(results_view_)
		: static_cast<QTableView*>(address_view_);
	if (!view || !view->selectionModel()) {
		if (truncated) *truncated = false;
		return rows;
	}
	const QItemSelection selection = view->selectionModel()->selection();
	for (const QItemSelectionRange& range : selection) {
		const int top = range.top();
		const int bottom = range.bottom();
		for (int row = top; row <= bottom; ++row) {
			if (rows.size() >= k_maximum_selected_contexts) {
				capped = true;
				break;
			}
			rows.push_back(row);
		}
		if (capped)
			break;
	}
	if (truncated)
		*truncated = capped;
	std::sort(rows.begin(), rows.end());
	rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
	return rows;
}

memory_interaction::context_t MemoryInteractionBridge::capture_row(
	int view_row) const
{
	if (!runtime_source_)
		return {};
	const auto runtime = runtime_source_();
	if (kind_ == memory_interaction::kind_t::scan_result && results_view_ &&
		results_view_->scan_model()) {
		auto* model = results_view_->scan_model();
		const int source = model->source_row(view_row);
		const auto* result = model->result_at_source(source);
		if (!result)
			return {};
		const QString module = model->raw_module_label_at_source(source);
		return memory_interaction::capture_result(runtime, result->address, source,
			memory_scanner::format_value(result->current_value, model->value_type()),
			memory_scanner::format_value(result->previous_value, model->value_type()),
			module.toStdString());
	}
	if (kind_ == memory_interaction::kind_t::address_entry && address_view_ &&
		address_view_->address_model()) {
		auto* model = address_view_->address_model();
		const auto* entry = model->entry_at(view_row);
		if (!entry)
			return {};
		return memory_interaction::capture_address(runtime, entry->address, view_row,
			entry->frozen,
			memory_scanner::format_value(entry->last_value, entry->value_type),
			entry->target_pid, entry->target_epoch,
			entry->target_identity.process.creation_time_100ns);
	}
	return {};
}

bool MemoryInteractionBridge::capture_rows(
	const std::vector<int>& view_rows, int focused_view_row,
	std::vector<memory_interaction::context_t>& contexts_out,
	memory_interaction::context_t& focused_out) const
{
	contexts_out.clear();
	focused_out = {};
	contexts_out.reserve(view_rows.size());
	for (const int row : view_rows) {
		auto context = capture_row(row);
		if (context.kind == memory_interaction::kind_t::none)
			continue;
		if (row == focused_view_row)
			focused_out = context;
		contexts_out.push_back(std::move(context));
	}
	if (contexts_out.empty())
		return false;
	if (focused_out.kind == memory_interaction::kind_t::none)
		focused_out = contexts_out.back();
	return true;
}

void MemoryInteractionBridge::on_view_selection()
{
	if (applying_ || !runtime_source_)
		return;
	bool truncated = false;
	const std::vector<int> rows = selected_view_rows(&truncated);
	if (truncated)
		chrome::toast_warning(
			QStringLiteral("Memory selection is limited to 4,096 rows."));
	QTableView* view = results_view_
		? static_cast<QTableView*>(results_view_)
		: static_cast<QTableView*>(address_view_);
	if (!view || !view->selectionModel())
		return;
	const QModelIndex current = view->currentIndex();
	const int focused_row = current.isValid() ? current.row() : -1;
	if (rows.empty()) {
		memory_interaction::clear_selection();
		return;
	}
	std::vector<memory_interaction::context_t> contexts;
	memory_interaction::context_t focused;
	if (!capture_rows(rows, focused_row, contexts, focused)) {
		memory_interaction::clear_selection();
		return;
	}
	memory_interaction::select_set(std::move(contexts), std::move(focused));
}

void MemoryInteractionBridge::synchronize_now()
{
	if (!runtime_source_)
		return;
	memory_interaction::synchronize_selection(runtime_source_());
	QTableView* view = results_view_
		? static_cast<QTableView*>(results_view_)
		: static_cast<QTableView*>(address_view_);
	if (!view || !view->selectionModel())
		return;
	if (memory_interaction::selected().kind == memory_interaction::kind_t::none &&
		!view->selectionModel()->selection().isEmpty()) {
		const QSignalBlocker blocker(view->selectionModel());
		view->clearSelection();
	}
}

void MemoryInteractionBridge::set_visible(bool visible)
{
	if (visible_ == visible)
		return;
	visible_ = visible;
	if (visible_)
		sync_timer_->start();
	else
		sync_timer_->stop();
}

void MemoryInteractionBridge::on_layout_about_to_change()
{
	if (kind_ != memory_interaction::kind_t::scan_result || !results_view_ ||
		!results_view_->scan_model())
		return;
	has_pending_layout_selection_ = false;
	pending_source_rows_.clear();
	pending_focused_source_ = -1;
	const std::vector<int> rows = selected_view_rows(nullptr);
	if (rows.empty())
		return;
	auto* model = results_view_->scan_model();
	pending_source_rows_.reserve(rows.size());
	for (const int row : rows) {
		const int source = model->source_row(row);
		if (source >= 0)
			pending_source_rows_.push_back(source);
	}
	const QModelIndex current = results_view_->currentIndex();
	pending_focused_source_ = current.isValid()
		? model->source_row(current.row()) : -1;
	has_pending_layout_selection_ = !pending_source_rows_.empty();
}

void MemoryInteractionBridge::on_layout_changed()
{
	if (!has_pending_layout_selection_ || !results_view_ ||
		!results_view_->scan_model())
		return;
	has_pending_layout_selection_ = false;
	auto* model = results_view_->scan_model();
	std::vector<int> view_rows;
	view_rows.reserve(pending_source_rows_.size());
	for (const int source : pending_source_rows_) {
		const int view_row = model->view_row_for_source(source);
		if (view_row >= 0)
			view_rows.push_back(view_row);
	}
	const int focused_view = model->view_row_for_source(pending_focused_source_);
	apply_view_rows(view_rows, focused_view);
}

void MemoryInteractionBridge::on_model_reset()
{
	pending_source_rows_.clear();
	has_pending_layout_selection_ = false;
	memory_interaction::clear_selection();
}

void MemoryInteractionBridge::apply_view_rows(const std::vector<int>& view_rows,
	int focused_view_row)
{
	QTableView* view = results_view_
		? static_cast<QTableView*>(results_view_)
		: static_cast<QTableView*>(address_view_);
	if (!view || !view->selectionModel())
		return;
	applying_ = true;
	const QSignalBlocker blocker(view->selectionModel());
	QItemSelection selection;
	std::vector<int> sorted_rows = view_rows;
	std::sort(sorted_rows.begin(), sorted_rows.end());
	int row = 0;
	while (row < static_cast<int>(sorted_rows.size())) {
		int end = row;
		while (end + 1 < static_cast<int>(sorted_rows.size()) &&
			sorted_rows[static_cast<std::size_t>(end + 1)] ==
				sorted_rows[static_cast<std::size_t>(end)] + 1)
			++end;
		selection.select(view->model()->index(
				sorted_rows[static_cast<std::size_t>(row)], 0),
			view->model()->index(
				sorted_rows[static_cast<std::size_t>(end)], 0));
		row = end + 1;
	}
	view->selectionModel()->select(selection,
		QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
	if (focused_view_row >= 0)
		view->setCurrentIndex(view->model()->index(focused_view_row, 0));
	applying_ = false;
}

}
