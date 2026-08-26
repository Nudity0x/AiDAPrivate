#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "core/scanner/memory_interaction_context.hpp"
#include "core/scanner/memory_scanner.hpp"
#include "qt/scanner/scan_commands.hpp"
#include "qt/scanner/scan_results_model.hpp"

class QTimer;

namespace aida::qt {
namespace bridge {
class ActionBridge;
}
namespace docking {
class AidaDockHost;
}
}

namespace aida::qt::scanner {

class AddressListModel;
class ValueScanWidget;

struct value_write_result_t {
	memory_interaction::context_t context;
	memory_scanner::value_type_t value_type = memory_scanner::value_type_t::int32_val;
	bool verified = false;
	bool rollback_verified = false;
	std::string detail;
};

class ScannerController : public QObject {
	Q_OBJECT
public:
	struct region_store_t {
		std::mutex mtx;
		std::vector<region_cache_entry_t> entries;
		std::uint64_t generation = 0;
		bool refreshing = false;
	};

	static ScannerController& instance();

	void install(docking::AidaDockHost* host, bridge::ActionBridge* actions);
	bool installed() const noexcept { return host_ != nullptr; }
	docking::AidaDockHost* host() const noexcept { return host_; }
	bridge::ActionBridge* actions() const noexcept { return actions_; }

	ScanResultsModel* results_model() const noexcept { return results_model_; }
	AddressListModel* address_model() const noexcept { return address_model_; }

	QString value_text() const { return value_text_; }
	QString value_text2() const { return value_text2_; }
	bool prefer_static_source() const noexcept { return prefer_static_source_; }
	bool auto_refresh() const noexcept { return auto_refresh_; }

	void set_value_text(const QString& value, const QString& value2);
	void set_prefer_static_source(bool prefer);
	void set_auto_refresh(bool enabled);
	void set_scan_value_type(int value_type);
	void set_scan_mode(int scan_mode);
	void set_hex_input(bool hex);

	scan_command_state_t command_capability(scan_command_t command);
	scan_command_result_t execute_command(scan_command_t command);

	result_sort_t sort_field() const noexcept { return sort_field_; }
	bool sort_descending() const noexcept { return sort_desc_; }

	memory_interaction::runtime_t runtime_snapshot() const;

	void register_visible_view();
	void unregister_visible_view();

	void request_region_refresh();
	void request_sort(result_sort_t field);
	void sort_column_clicked(int logical_column);

	void freeze_toggled(int row);
	void remove_addresses(const std::vector<int>& rows);
	void add_address(std::uint64_t address, const std::string& description,
		memory_scanner::value_type_t type);
	void edit_description(int row, const std::string& description);
	void change_type(int row, memory_scanner::value_type_t type);
	bool request_value_write(const memory_interaction::context_t& context,
		memory_scanner::value_type_t value_type, std::vector<std::uint8_t> expected,
		std::string& error);
	bool write_pending() const noexcept
		{ return write_pending_.load(std::memory_order_acquire); }
	std::shared_ptr<const value_write_result_t> last_write_result() const noexcept
		{ return last_write_result_; }

	void open_add_dialog(std::uint64_t address, int value_type, QWidget* parent);
	void open_edit_description_dialog(int row, QWidget* parent);
	void open_change_value_dialog(int row, QWidget* parent);
	void open_change_type_dialog(int row, QWidget* parent);

	void refresh_from_engine();
	void open_or_focus_view(const char* view_id);
	void refresh_actions();

Q_SIGNALS:
	void stateChanged();
	void scanProgressed(double progress, const QString& stage);
	void scanStateEdge();
	void addressListChanged();
	void addressValuesRefreshed();
	void writeCompleted();
	void persistedConfigReplayed();

private:
	explicit ScannerController(QObject* parent = nullptr);
	~ScannerController() override;

	void on_engine_progress(float progress, const char* stage,
		std::uint64_t generation);
	void poll_edge_detection();
	void adopt_published_results();
	void refresh_address_entries(bool values_only);
	void apply_persisted_config_if_pending();
	void apply_sort_order(std::vector<int> order, std::uint64_t serial);
	void schedule_sort_worker();
	void update_refresh_timer();
	void handle_write_completion(
		const std::shared_ptr<const value_write_result_t>& result);

	docking::AidaDockHost* host_ = nullptr;
	bridge::ActionBridge* actions_ = nullptr;
	ScanResultsModel* results_model_ = nullptr;
	AddressListModel* address_model_ = nullptr;
	QTimer* poll_timer_ = nullptr;
	QTimer* refresh_timer_ = nullptr;

	QString value_text_;
	QString value_text2_;
	bool prefer_static_source_ = false;
	bool auto_refresh_ = false;

	result_sort_t sort_field_ = result_sort_t::by_index;
	bool sort_desc_ = false;
	std::atomic<std::uint64_t> sort_serial_{0};

	region_store_t region_store_;
	std::shared_ptr<const std::vector<region_cache_entry_t>> gui_regions_;
	std::uint64_t gui_region_generation_ = 0;
	std::uint64_t last_region_sort_generation_ = 0;

	std::atomic<bool> write_pending_{false};
	std::shared_ptr<const value_write_result_t> last_write_result_;

	std::uint32_t observed_pid_ = 0;
	std::uint64_t observed_epoch_ = 0;
	std::uint64_t observed_scan_revision_ = 0;
	std::uint64_t observed_address_signature_ = 0;
	int visible_views_ = 0;
	bool sink_registered_ = false;
};

}
