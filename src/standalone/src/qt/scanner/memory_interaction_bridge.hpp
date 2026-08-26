#pragma once

#include <QObject>
#include <QPoint>
#include <QString>

#include <functional>
#include <vector>

#include "core/scanner/memory_interaction_context.hpp"

class QTableView;
class QTimer;

namespace aida::qt::scanner {

class ScanResultsView;
class AddressListView;
class ScanResultsModel;
class AddressListModel;

class MemoryInteractionBridge : public QObject {
	Q_OBJECT
public:
	MemoryInteractionBridge(QObject* parent, QString owner_view_id,
		memory_interaction::kind_t kind);

	void set_runtime_source(
		std::function<memory_interaction::runtime_t()> runtime_source);
	void attach_results_view(ScanResultsView* view);
	void attach_address_view(AddressListView* view);

	QString owner_view_id() const noexcept { return owner_view_id_; }

	void on_view_selection();
	void synchronize_now();
	void set_visible(bool visible);
	bool visible() const noexcept { return visible_; }

	bool capture_rows(const std::vector<int>& view_rows, int focused_view_row,
		std::vector<memory_interaction::context_t>& contexts_out,
		memory_interaction::context_t& focused_out) const;
	memory_interaction::context_t capture_row(int view_row) const;

	static constexpr std::size_t k_maximum_selected_contexts = 4096;

private:
	void on_layout_about_to_change();
	void on_layout_changed();
	void on_model_reset();
	void apply_view_rows(const std::vector<int>& view_rows, int focused_view_row);
	std::vector<int> selected_view_rows(bool* truncated) const;

	QString owner_view_id_;
	memory_interaction::kind_t kind_ = memory_interaction::kind_t::none;
	std::function<memory_interaction::runtime_t()> runtime_source_;
	ScanResultsView* results_view_ = nullptr;
	AddressListView* address_view_ = nullptr;
	QTimer* sync_timer_ = nullptr;
	std::vector<int> pending_source_rows_;
	int pending_focused_source_ = -1;
	bool has_pending_layout_selection_ = false;
	bool applying_ = false;
	bool visible_ = false;
};

}
