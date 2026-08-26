#include "qt/scanner/scanner_controller.hpp"

#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "core/disasm/disasm_view.hpp"
#include "core/disasm/function_index.hpp"
#include "core/infra/executor.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/scanner/scanner_task_center.hpp"
#include "helpers/diag_log.hpp"

#include "qt/analysis/qt_gui_post.hpp"
#include "qt/bridge/action_bridge.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/scanner/address_list_model.hpp"
#include "qt/scanner/scanner_dialogs.hpp"

namespace aida::qt::scanner {

namespace {

void scan_diag_log(const char* msg) {
	diag::log_tagged("value_scan", msg);
}

void scan_diag_logf(const char* fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	diag::log_tagged("value_scan", buf);
}

const char* region_kind_label(uint32_t type, uint32_t state) {
	if (state != 0x1000) return "Unmapped";
	if (type == 0x1000000) return "Image";
	if (type == 0x40000)   return "Mapped";
	if (type == 0x20000)   return "Private";
	return "Region";
}

void refresh_region_cache_locked(ScannerController::region_store_t& c) {
	constexpr int kRegionRefreshChunk = 4096;
	auto regs = driver_bridge::enumerate_memory_regions(kRegionRefreshChunk);
	std::vector<region_cache_entry_t> out;
	out.reserve(regs.size());
	for (const auto& r : regs) {
		region_cache_entry_t e;
		e.base = r.base;
		e.end  = r.base + r.size;
		e.state = r.state;
		e.protect = r.protect;
		e.type = r.type;
		out.push_back(e);
	}
	std::sort(out.begin(), out.end(),
		[](const region_cache_entry_t& a, const region_cache_entry_t& b) {
			return a.base < b.base;
		});
	std::lock_guard<std::mutex> lk(c.mtx);
	c.entries = std::move(out);
	c.generation += 1;
	c.refreshing = false;
}

bool address_entry_matches_context(const memory_scanner::address_entry_t& entry,
	const memory_interaction::context_t& context,
	memory_scanner::value_type_t value_type) noexcept {
	return entry.address == context.address && entry.value_type == value_type &&
		entry.target_pid == context.target_pid && entry.target_epoch == context.target_epoch &&
		entry.target_identity.process.creation_time_100ns ==
			context.process_creation_time_100ns;
}

double sort_value_to_double(memory_scanner::value_type_t value_type,
	const std::vector<std::uint8_t>& bytes) {
	if (bytes.empty()) return 0.0;
	switch (value_type) {
		case memory_scanner::value_type_t::byte_val:
			return static_cast<double>(bytes[0]);
		case memory_scanner::value_type_t::int16_val: {
			int16_t v; std::memcpy(&v, bytes.data(), (std::min)(bytes.size(), sizeof(v)));
			return static_cast<double>(v);
		}
		case memory_scanner::value_type_t::int32_val: {
			int32_t v; std::memcpy(&v, bytes.data(), (std::min)(bytes.size(), sizeof(v)));
			return static_cast<double>(v);
		}
		case memory_scanner::value_type_t::int64_val: {
			int64_t v; std::memcpy(&v, bytes.data(), (std::min)(bytes.size(), sizeof(v)));
			return static_cast<double>(v);
		}
		case memory_scanner::value_type_t::float_val: {
			float v; std::memcpy(&v, bytes.data(), (std::min)(bytes.size(), sizeof(v)));
			return static_cast<double>(v);
		}
		case memory_scanner::value_type_t::double_val: {
			double v; std::memcpy(&v, bytes.data(), (std::min)(bytes.size(), sizeof(v)));
			return v;
		}
		default:
			return 0.0;
	}
}

}

ScannerController& ScannerController::instance() {
	static ScannerController* controller = new ScannerController();
	return *controller;
}

ScannerController::ScannerController(QObject* parent) : QObject(parent) {
	results_model_ = new ScanResultsModel(this);
	address_model_ = new AddressListModel(this);
	poll_timer_ = new QTimer(this);
	poll_timer_->setInterval(250);
	connect(poll_timer_, &QTimer::timeout, this,
		[this] { poll_edge_detection(); });
	refresh_timer_ = new QTimer(this);
	refresh_timer_->setInterval(500);
	connect(refresh_timer_, &QTimer::timeout, this, [this] {
		const bool attached = driver_bridge::is_loaded() &&
			driver_bridge::attached_pid() != 0;
		if (!auto_refresh_ || !attached)
			return;
		aida::infra::executor::submission_t submission;
		submission.owner_subsystem = "scanner";
		submission.label = "scanner.address_list_refresh";
		submission.thread_class = "scanner_ui_refresh";
		submission.domain = aida::infra::executor::domain_t::diagnostics;
		submission.priority = 4;
		submission.target_pid = driver_bridge::attached_pid();
		submission.body = [this]() {
			memory_scanner::refresh_address_list();
			analysis::gui_post(this, [this] {
				refresh_address_entries(true);
				Q_EMIT addressValuesRefreshed();
			});
		};
		if (!aida::infra::executor::submit(std::move(submission)).submitted)
			diag::log_tagged("value_scan", "address_list_refresh_post_failed");
	});
}

ScannerController::~ScannerController() {
	if (sink_registered_)
		memory_scanner::set_scan_progress_sink(nullptr);
}

void ScannerController::install(docking::AidaDockHost* host,
	bridge::ActionBridge* actions) {
	host_ = host;
	actions_ = actions;
	if (!sink_registered_) {
		sink_registered_ = true;
		memory_scanner::set_scan_progress_sink(
			[this](float progress, const char* stage, std::uint64_t generation) {
				const QString stage_text = stage ? QString::fromUtf8(stage) : QString();
				analysis::gui_post(this, [this, progress, stage_text, generation] {
					on_engine_progress(progress, nullptr, generation);
					Q_EMIT scanProgressed(static_cast<double>(progress), stage_text);
				});
			});
	}
}

void ScannerController::set_value_text(const QString& value, const QString& value2) {
	const QString v1 = value.left(255);
	const QString v2 = value2.left(63);
	if (value_text_ == v1 && value_text2_ == v2)
		return;
	value_text_ = v1;
	value_text2_ = v2;
	auto& state = memory_scanner::g_state;
	std::lock_guard<std::mutex> lock(state.results_mutex);
	state.config.value_text = v1.toStdString();
	state.config.value_text2 = v2.toStdString();
	Q_EMIT stateChanged();
}

void ScannerController::set_prefer_static_source(bool prefer) {
	if (prefer_static_source_ == prefer)
		return;
	prefer_static_source_ = prefer;
	scan_diag_logf("toolbar source_toggle static=%d", static_cast<int>(prefer));
	Q_EMIT stateChanged();
}

void ScannerController::set_auto_refresh(bool enabled) {
	if (auto_refresh_ == enabled)
		return;
	auto_refresh_ = enabled;
	scan_diag_logf("address auto_refresh_toggle now=%d", static_cast<int>(enabled));
	update_refresh_timer();
	Q_EMIT stateChanged();
}

void ScannerController::set_scan_value_type(int value_type) {
	if (value_type < 0 ||
		value_type >= static_cast<int>(memory_scanner::value_type_t::COUNT))
		return;
	auto& state = memory_scanner::g_state;
	{
		std::lock_guard<std::mutex> lock(state.results_mutex);
		if (state.config.value_type == static_cast<memory_scanner::value_type_t>(value_type))
			return;
		state.config.value_type = static_cast<memory_scanner::value_type_t>(value_type);
	}
	scan_diag_logf("toolbar value_type_change to=%s",
		memory_scanner::value_type_name(static_cast<memory_scanner::value_type_t>(value_type)));
	sort_serial_.fetch_add(1, std::memory_order_acq_rel);
	if (sort_field_ != result_sort_t::by_index)
		schedule_sort_worker();
	Q_EMIT stateChanged();
}

void ScannerController::set_scan_mode(int scan_mode) {
	if (scan_mode < 0 || scan_mode >= static_cast<int>(memory_scanner::scan_mode_t::COUNT))
		return;
	auto& state = memory_scanner::g_state;
	{
		std::lock_guard<std::mutex> lock(state.results_mutex);
		if (state.config.scan_mode == static_cast<memory_scanner::scan_mode_t>(scan_mode))
			return;
		state.config.scan_mode = static_cast<memory_scanner::scan_mode_t>(scan_mode);
	}
	scan_diag_logf("toolbar scan_mode_change to=%s",
		memory_scanner::scan_mode_name(static_cast<memory_scanner::scan_mode_t>(scan_mode)));
	Q_EMIT stateChanged();
}

void ScannerController::set_hex_input(bool hex) {
	auto& state = memory_scanner::g_state;
	{
		std::lock_guard<std::mutex> lock(state.results_mutex);
		if (state.config.hex_input == hex)
			return;
		state.config.hex_input = hex;
	}
	scan_diag_logf("toolbar hex_toggle now=%d", static_cast<int>(hex));
	Q_EMIT stateChanged();
}

void ScannerController::register_visible_view() {
	++visible_views_;
	if (visible_views_ == 1 && !poll_timer_->isActive())
		poll_timer_->start();
	update_refresh_timer();
}

void ScannerController::unregister_visible_view() {
	if (visible_views_ > 0)
		--visible_views_;
	if (visible_views_ == 0) {
		poll_timer_->stop();
		refresh_timer_->stop();
	}
}

void ScannerController::update_refresh_timer() {
	const bool attached = driver_bridge::is_loaded() &&
		driver_bridge::attached_pid() != 0;
	if (auto_refresh_ && attached && visible_views_ > 0) {
		if (!refresh_timer_->isActive())
			refresh_timer_->start();
	} else if (refresh_timer_->isActive()) {
		refresh_timer_->stop();
	}
}

memory_interaction::runtime_t ScannerController::runtime_snapshot() const {
	memory_interaction::runtime_t runtime;
	auto& state = memory_scanner::g_state;
	std::size_t result_count = 0;
	int scan_count = 0;
	runtime.driver_loaded = driver_bridge::is_loaded();
	runtime.target_pid = driver_bridge::attached_pid();
	runtime.live_attached = runtime.driver_loaded && runtime.target_pid != 0;
	runtime.static_loaded = function_index::detail::static_pe_active();
	runtime.target_epoch = state.target_epoch.load(std::memory_order_acquire);
	runtime.process_creation_time_100ns =
		state.observed_target_creation_time_100ns.load(std::memory_order_acquire);
	{
		std::lock_guard<std::mutex> lock(state.results_mutex);
		result_count = state.results ? state.results->size() : 0;
		scan_count = state.scan_count;
		runtime.scan_static_binary = state.scan_static_binary;
		runtime.scan_target_pid = state.scan_target_pid;
		runtime.scan_target_epoch = state.scan_target_epoch;
		runtime.scan_process_creation_time_100ns =
			state.scan_target_identity.process.creation_time_100ns;
		runtime.scan_workspace_id = state.scan_workspace_id;
		runtime.scan_workspace_generation = state.scan_workspace_generation;
	}
	const std::uint64_t count_component = static_cast<std::uint64_t>(result_count);
	runtime.scan_revision =
		(static_cast<std::uint64_t>(static_cast<std::uint32_t>(scan_count)) << 32U) ^
		count_component;
	return runtime;
}

scan_command_state_t ScannerController::command_capability(scan_command_t command) {
	auto& state = memory_scanner::g_state;
	const bool scanning = state.scanning.load(std::memory_order_acquire);
	bool has_initial_scan = false;
	bool has_undo_history = false;
	bool static_scan = false;
	{
		std::lock_guard<std::mutex> lock(state.results_mutex);
		has_initial_scan = state.has_initial_scan;
		has_undo_history = !state.scan_history.empty();
		static_scan = state.scan_static_binary;
	}
	const bool attached = driver_bridge::is_loaded() && driver_bridge::attached_pid() != 0;
	const bool static_binary = function_index::detail::static_pe_active();
	const std::string value_utf8 = value_text_.toStdString();
	const std::string value2_utf8 = value_text2_.toStdString();
	const auto requires_value = [](memory_scanner::scan_mode_t mode) {
		return mode == memory_scanner::scan_mode_t::exact ||
			mode == memory_scanner::scan_mode_t::bigger_than ||
			mode == memory_scanner::scan_mode_t::smaller_than ||
			mode == memory_scanner::scan_mode_t::value_between;
	};
	const auto configured_value_capability = [&]() -> scan_command_state_t {
		if (state.config.value_type == memory_scanner::value_type_t::all_types)
			return {false, "Choose a concrete value type; All Types has no value-scan provider"};
		const bool variable_length =
			state.config.value_type == memory_scanner::value_type_t::string_ascii ||
			state.config.value_type == memory_scanner::value_type_t::string_utf16 ||
			state.config.value_type == memory_scanner::value_type_t::byte_array;
		if (variable_length &&
			(state.config.scan_mode == memory_scanner::scan_mode_t::bigger_than ||
			 state.config.scan_mode == memory_scanner::scan_mode_t::smaller_than ||
			 state.config.scan_mode == memory_scanner::scan_mode_t::value_between ||
			 state.config.scan_mode == memory_scanner::scan_mode_t::increased ||
			 state.config.scan_mode == memory_scanner::scan_mode_t::decreased))
			return {false, "Choose Exact, Changed, Unchanged, or Unknown Initial for variable-length values"};
		if (requires_value(state.config.scan_mode) && value_utf8.empty())
			return {false, "Enter the scan value first"};
		if (state.config.scan_mode == memory_scanner::scan_mode_t::value_between &&
			value2_utf8.empty())
			return {false, "Enter the upper value for the Between scan first"};
		return {true, {}};
	};

	switch (command) {
	case scan_command_t::first_scan: {
		if (scanning)
			return {false, "A memory scan is already running"};
		if (has_initial_scan)
			return {false, "Start a New Scan before running another First Scan"};
		if (prefer_static_source_ && !static_binary)
			return {false, "The selected static binary source is no longer available"};
		if (!attached && !static_binary)
			return {false, "Attach to a live process or open a static PE workspace before scanning"};
		if (state.config.scan_mode == memory_scanner::scan_mode_t::changed ||
			state.config.scan_mode == memory_scanner::scan_mode_t::unchanged ||
			state.config.scan_mode == memory_scanner::scan_mode_t::increased ||
			state.config.scan_mode == memory_scanner::scan_mode_t::decreased)
			return {false, "Choose an initial comparison or Unknown Initial scan mode"};
		return configured_value_capability();
	}
	case scan_command_t::next_scan:
		if (scanning)
			return {false, "Wait for the active memory scan to finish or stop it first"};
		if (!has_initial_scan)
			return {false, "Run First Scan before refining results"};
		if (static_scan)
			return {false, "Static binary scans are immutable; change the definition and run New Scan"};
		if (!attached)
			return {false, "The scanned live process is no longer attached"};
		if (!memory_scanner::target_binding_current(
				state.scan_target_pid, state.scan_target_epoch))
			return {false, "The attached process changed; start a New Scan for this target"};
		if (state.config.scan_mode == memory_scanner::scan_mode_t::unknown_initial)
			return {false, "Choose a refinement scan mode before running Next Scan"};
		return configured_value_capability();
	case scan_command_t::stop_scan:
		return scanning ? scan_command_state_t{true, {}}
			: scan_command_state_t{false, "No memory scan is running"};
	case scan_command_t::undo_scan:
		if (scanning)
			return {false, "Wait for the active memory scan to finish or stop it first"};
		return has_undo_history ? scan_command_state_t{true, {}}
			: scan_command_state_t{false, "No completed refinement scan is available to undo"};
	case scan_command_t::new_scan:
		if (scanning)
			return {false, "Stop the active memory scan before starting a new scan"};
		return has_initial_scan ? scan_command_state_t{true, {}}
			: scan_command_state_t{false, "The scanner is already ready for a First Scan"};
	}
	return {false, "The memory scan command is invalid"};
}

scan_command_result_t ScannerController::execute_command(scan_command_t command) {
	const auto capability = command_capability(command);
	if (!capability.enabled)
		return {false, capability.disabled_reason};
	auto& state = memory_scanner::g_state;
	switch (command) {
	case scan_command_t::first_scan: {
		const std::string value_utf8 = value_text_.toStdString();
		const std::string value2_utf8 = value_text2_.toStdString();
		scan_diag_logf("action first_scan val='%s' val2='%s' vtype=%s mode=%s",
			value_utf8.c_str(), value2_utf8.c_str(),
			memory_scanner::value_type_name(state.config.value_type),
			memory_scanner::scan_mode_name(state.config.scan_mode));
		diag::log_tagged("scan_audit",
			"[scan_audit] memory_scanner first_scan invoked");
		state.config.value_text = value_utf8;
		state.config.value_text2 = value2_utf8;
		bool started = false;
		bool static_started = false;
		if (!prefer_static_source_ && driver_bridge::is_loaded() &&
			driver_bridge::attached_pid() != 0) {
			started = memory_scanner::first_scan(state.config);
		} else {
			const auto workspace = disasm_view::capture_selected_workspace();
			if (workspace.workspace && workspace.image) {
				started = memory_scanner::first_static_scan(state.config,
					workspace.workspace->provider_handle(), workspace.image,
					workspace.workspace->identity().binary_id().to_hex(),
					workspace.workspace->generation());
				static_started = started;
			}
		}
		if (!started)
			return {false, "The memory scan engine rejected First Scan; the live target or static workspace changed"};
		request_region_refresh();
		sort_serial_.fetch_add(1, std::memory_order_acq_rel);
		Q_EMIT scanStateEdge();
		Q_EMIT stateChanged();
		return {true, static_started ? "Static binary value scan started" :
			"Initial live memory scan started"};
	}
	case scan_command_t::next_scan: {
		const std::string value_utf8 = value_text_.toStdString();
		const std::string value2_utf8 = value_text2_.toStdString();
		scan_diag_logf("action next_scan mode=%s val='%s' val2='%s'",
			memory_scanner::scan_mode_name(state.config.scan_mode),
			value_utf8.c_str(), value2_utf8.c_str());
		diag::log_tagged("scan_audit",
			"[scan_audit] memory_scanner next_scan invoked");
		if (!memory_scanner::next_scan(state.config.scan_mode, value_utf8, value2_utf8))
			return {false, "The memory scan engine rejected Next Scan; the target, scan generation, or worker admission changed"};
		request_region_refresh();
		sort_serial_.fetch_add(1, std::memory_order_acq_rel);
		Q_EMIT scanStateEdge();
		Q_EMIT stateChanged();
		return {true, "Memory scan refinement started"};
	}
	case scan_command_t::stop_scan:
		scan_diag_log("action stop_scan");
		return memory_scanner::cancel_scan()
			? scan_command_result_t{true, "Memory scan cancellation requested"}
			: scan_command_result_t{false, "The memory scan completed before cancellation was requested"};
	case scan_command_t::undo_scan:
		scan_diag_log("action undo_scan");
		memory_scanner::undo_scan();
		sort_serial_.fetch_add(1, std::memory_order_acq_rel);
		adopt_published_results();
		Q_EMIT scanStateEdge();
		Q_EMIT stateChanged();
		return {true, "Previous memory scan result set restored"};
	case scan_command_t::new_scan:
		scan_diag_log("action new_scan");
		memory_scanner::reset_scan();
		sort_serial_.fetch_add(1, std::memory_order_acq_rel);
		adopt_published_results();
		Q_EMIT scanStateEdge();
		Q_EMIT stateChanged();
		return {true, "Memory scanner reset for a new scan"};
	}
	return {false, "The memory scan command is invalid"};
}

void ScannerController::request_region_refresh() {
	auto& c = region_store_;
	{
		std::lock_guard<std::mutex> lk(c.mtx);
		if (c.refreshing) return;
		c.refreshing = true;
	}
	scan_diag_log("region_cache refresh_post");
	aida::infra::executor::submission_t sub;
	sub.owner_subsystem = "scanner";
	sub.label = "scanner.region_cache_refresh";
	sub.thread_class = "scanner_ui_refresh";
	sub.domain = aida::infra::executor::domain_t::diagnostics;
	sub.priority = 4;
	sub.target_pid = driver_bridge::attached_pid();
	sub.body = [this]() {
		refresh_region_cache_locked(region_store_);
		diag::log_tagged("value_scan", "region_cache refresh_done");
		analysis::gui_post(this, [this] {
			std::uint64_t generation = 0;
			{
				std::lock_guard<std::mutex> lk(region_store_.mtx);
				gui_regions_ = std::make_shared<const std::vector<region_cache_entry_t>>(
					region_store_.entries);
				generation = region_store_.generation;
			}
			gui_region_generation_ = generation;
			results_model_->set_regions(gui_regions_);
			if (sort_field_ == result_sort_t::by_module &&
				last_region_sort_generation_ != generation) {
				last_region_sort_generation_ = generation;
				schedule_sort_worker();
			}
		});
	};
	bool posted = aida::infra::executor::submit(std::move(sub)).submitted;
	if (!posted) {
		std::lock_guard<std::mutex> lk(c.mtx);
		c.refreshing = false;
		scan_diag_log("region_cache refresh_post_failed");
	}
}

void ScannerController::sort_column_clicked(int logical_column) {
	result_sort_t field = result_sort_t::by_index;
	switch (logical_column) {
	case ScanResultsModel::column_address:  field = result_sort_t::by_address;  break;
	case ScanResultsModel::column_value:    field = result_sort_t::by_value;    break;
	case ScanResultsModel::column_previous: field = result_sort_t::by_previous; break;
	case ScanResultsModel::column_module:   field = result_sort_t::by_module;   break;
	default: break;
	}
	request_sort(field);
}

void ScannerController::request_sort(result_sort_t field) {
	const std::size_t total = results_model_->snapshot()
		? results_model_->snapshot()->size() : 0;
	if (total > 10000) {
		chrome::toast_warning(QStringLiteral(
			"Refine results below 10,000 rows before changing sort order."));
		return;
	}
	if (sort_field_ == field) {
		if (!sort_desc_) sort_desc_ = true;
		else { sort_field_ = result_sort_t::by_index; sort_desc_ = false; }
	} else {
		sort_field_ = field;
		sort_desc_ = false;
	}
	scan_diag_logf("results column_sort_click field=%d desc=%d",
		static_cast<int>(sort_field_), static_cast<int>(sort_desc_));
	schedule_sort_worker();
}

void ScannerController::schedule_sort_worker() {
	const auto snapshot = results_model_->snapshot();
	const std::size_t total = snapshot ? snapshot->size() : 0;
	const result_sort_t field = sort_field_;
	const bool desc = sort_desc_;
	if (field == result_sort_t::by_index || !snapshot || total == 0) {
		results_model_->apply_order({});
		Q_EMIT stateChanged();
		return;
	}
	if (total > 10000) {
		results_model_->apply_order({});
		return;
	}
	const std::uint64_t serial = sort_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
	const auto value_type = results_model_->value_type();
	const auto regions = gui_regions_ ? *gui_regions_ : std::vector<region_cache_entry_t>{};
	aida::infra::executor::submission_t sub;
	sub.owner_subsystem = "scanner";
	sub.label = "scanner.result_sort";
	sub.thread_class = "scanner_ui_refresh";
	sub.domain = aida::infra::executor::domain_t::diagnostics;
	sub.priority = 4;
	sub.target_pid = driver_bridge::attached_pid();
	sub.body = [this, snapshot, field, desc, value_type, regions, serial]() {
		const int safe_total = static_cast<int>(snapshot->size());
		std::vector<int> order(static_cast<std::size_t>(safe_total));
		for (int i = 0; i < safe_total; ++i)
			order[static_cast<std::size_t>(i)] = i;
		std::vector<std::string> module_labels;
		if (field == result_sort_t::by_module) {
			module_labels.reserve(static_cast<std::size_t>(safe_total));
			for (int index = 0; index < safe_total; ++index)
				module_labels.push_back(ScanResultsModel::module_label(
					(*snapshot)[static_cast<std::size_t>(index)], regions).toStdString());
		}
		std::sort(order.begin(), order.end(),
			[&](int aa, int bb) {
				if (aa < 0 || bb < 0) return aa < bb;
				if (aa >= safe_total || bb >= safe_total) return aa < bb;
				const auto& ra = (*snapshot)[static_cast<size_t>(aa)];
				const auto& rb = (*snapshot)[static_cast<size_t>(bb)];
				int cmp = 0;
				switch (field) {
					case result_sort_t::by_address:
						cmp = (ra.address < rb.address) ? -1 : (ra.address > rb.address ? 1 : 0);
						break;
					case result_sort_t::by_value: {
						double da = sort_value_to_double(value_type, ra.current_value);
						double db = sort_value_to_double(value_type, rb.current_value);
						cmp = (da < db) ? -1 : (da > db ? 1 : 0);
						break;
					}
					case result_sort_t::by_previous: {
						double da = sort_value_to_double(value_type, ra.previous_value);
						double db = sort_value_to_double(value_type, rb.previous_value);
						cmp = (da < db) ? -1 : (da > db ? 1 : 0);
						break;
					}
					case result_sort_t::by_module:
						cmp = module_labels[static_cast<std::size_t>(aa)].compare(
							module_labels[static_cast<std::size_t>(bb)]);
						break;
					default: break;
				}
				if (cmp == 0) return ra.address < rb.address;
				return desc ? (cmp > 0) : (cmp < 0);
			});
		analysis::gui_post(this, [this, order = std::move(order), serial]() mutable {
			apply_sort_order(std::move(order), serial);
		});
	};
	if (!aida::infra::executor::submit(std::move(sub)).submitted) {
		scan_diag_log("result_sort post_failed");
		results_model_->apply_order({});
	}
}

void ScannerController::apply_sort_order(std::vector<int> order, std::uint64_t serial) {
	if (sort_serial_.load(std::memory_order_acquire) != serial)
		return;
	if (sort_field_ == result_sort_t::by_index)
		order.clear();
	results_model_->apply_order(std::move(order));
	Q_EMIT stateChanged();
}

void ScannerController::on_engine_progress(float progress, const char* stage,
	std::uint64_t generation) {
	static_cast<void>(stage);
	static_cast<void>(generation);
	adopt_published_results();
	poll_edge_detection();
	Q_EMIT stateChanged();
}

void ScannerController::adopt_published_results() {
	auto& state = memory_scanner::g_state;
	std::shared_ptr<const std::vector<memory_scanner::scan_result_t>> snapshot;
	std::size_t total_found = 0;
	int scan_count = 0;
	memory_scanner::value_type_t value_type = memory_scanner::value_type_t::int32_val;
	{
		std::lock_guard<std::mutex> lock(state.results_mutex);
		snapshot = state.results;
		total_found = state.total_found;
		scan_count = state.scan_count;
		value_type = state.config.value_type;
	}
	if (snapshot == results_model_->snapshot())
		return;
	const std::uint64_t revision =
		(static_cast<std::uint64_t>(static_cast<std::uint32_t>(scan_count)) << 32U) ^
		static_cast<std::uint64_t>(snapshot ? snapshot->size() : 0);
	const std::size_t rows = snapshot ? snapshot->size() : 0;
	if (rows > 10000 && sort_field_ != result_sort_t::by_index) {
		sort_field_ = result_sort_t::by_index;
		sort_desc_ = false;
		sort_serial_.fetch_add(1, std::memory_order_acq_rel);
		chrome::toast_info(QStringLiteral(
			"Result sort returned to address order to keep the large scan responsive."));
	}
	results_model_->adopt(snapshot, revision, value_type, scan_count, total_found);
	if (sort_field_ != result_sort_t::by_index && rows > 0 && rows <= 10000)
		schedule_sort_worker();
}

void ScannerController::poll_edge_detection() {
	auto& state = memory_scanner::g_state;
	const std::uint32_t pid = driver_bridge::is_loaded()
		? driver_bridge::attached_pid() : 0;
	const std::uint64_t epoch = state.target_epoch.load(std::memory_order_acquire);
	std::shared_ptr<const std::vector<memory_scanner::scan_result_t>> snapshot;
	int scan_count = 0;
	{
		std::lock_guard<std::mutex> lock(state.results_mutex);
		snapshot = state.results;
		scan_count = state.scan_count;
	}
	std::uint64_t address_signature = 1469598103934665603ULL;
	{
		std::lock_guard<std::mutex> lock(state.address_mutex);
		for (const auto& entry : state.address_list) {
			address_signature ^= entry.address;
			address_signature *= 1099511628211ULL;
			address_signature ^= static_cast<std::uint8_t>(entry.frozen ? 1 : 0);
			address_signature *= 1099511628211ULL;
			address_signature ^= static_cast<std::uint64_t>(entry.value_type);
			address_signature *= 1099511628211ULL;
			address_signature ^= entry.last_value.size();
			address_signature *= 1099511628211ULL;
			for (const char character : entry.description) {
				address_signature ^= static_cast<unsigned char>(character);
				address_signature *= 1099511628211ULL;
			}
		}
	}
	const std::uint64_t revision =
		(static_cast<std::uint64_t>(static_cast<std::uint32_t>(scan_count)) << 32U) ^
		static_cast<std::uint64_t>(snapshot ? snapshot->size() : 0);
	bool changed = false;
	if (pid != observed_pid_ || epoch != observed_epoch_) {
		observed_pid_ = pid;
		observed_epoch_ = epoch;
		if (pid != 0) {
			bool regions_empty = false;
			{
				std::lock_guard<std::mutex> lk(region_store_.mtx);
				regions_empty = region_store_.entries.empty();
			}
			if (regions_empty)
				request_region_refresh();
			apply_persisted_config_if_pending();
		}
		if (pid == 0 && function_index::detail::static_pe_active() &&
			!prefer_static_source_) {
			prefer_static_source_ = true;
		}
		changed = true;
	}
	if (revision != observed_scan_revision_) {
		observed_scan_revision_ = revision;
		adopt_published_results();
		changed = true;
	}
	if (address_signature != observed_address_signature_) {
		observed_address_signature_ = address_signature;
		refresh_address_entries(false);
		changed = true;
	}
	if (changed) {
		Q_EMIT scanStateEdge();
		Q_EMIT stateChanged();
		update_refresh_timer();
	}
	if (state.persisted_config_loaded.load(std::memory_order_acquire))
		apply_persisted_config_if_pending();
}

void ScannerController::apply_persisted_config_if_pending() {
	auto& state = memory_scanner::g_state;
	if (!state.persisted_config_loaded.exchange(false, std::memory_order_acq_rel))
		return;
	{
		std::lock_guard<std::mutex> lock(state.results_mutex);
		value_text_ = QString::fromStdString(state.config.value_text).left(255);
		value_text2_ = QString::fromStdString(state.config.value_text2).left(63);
	}
	Q_EMIT persistedConfigReplayed();
	Q_EMIT stateChanged();
}

void ScannerController::refresh_address_entries(bool values_only) {
	auto& state = memory_scanner::g_state;
	std::vector<memory_scanner::address_entry_t> entries;
	{
		std::lock_guard<std::mutex> lock(state.address_mutex);
		entries = state.address_list;
	}
	if (values_only)
		address_model_->refresh_values(std::move(entries));
	else
		address_model_->reset_entries(std::move(entries));
	Q_EMIT addressListChanged();
}

void ScannerController::freeze_toggled(int row) {
	auto& state = memory_scanner::g_state;
	std::optional<memory_scanner::address_entry_t> entry;
	{
		std::lock_guard<std::mutex> lock(state.address_mutex);
		if (row >= 0 && row < static_cast<int>(state.address_list.size()))
			entry = state.address_list[static_cast<std::size_t>(row)];
	}
	if (!entry)
		return;
	const auto runtime = runtime_snapshot();
	const auto context = memory_interaction::capture_address(runtime, entry->address,
		row, entry->frozen,
		memory_scanner::format_value(entry->last_value, entry->value_type),
		entry->target_pid, entry->target_epoch,
		entry->target_identity.process.creation_time_100ns);
	const auto capability = memory_interaction::evaluate(
		entry->frozen ? memory_interaction::capability_t::unfreeze :
			memory_interaction::capability_t::freeze, context, runtime);
	if (!capability.enabled) {
		chrome::toast_warning(QString::fromLatin1(capability.disabled_reason));
		return;
	}
	scan_diag_logf("address freeze_toggle idx=%d addr=0x%llX now=%d",
		row, static_cast<unsigned long long>(entry->address),
		static_cast<int>(!entry->frozen));
	static_cast<void>(memory_scanner::freeze_address_exact(static_cast<std::size_t>(row),
		!entry->frozen, *entry));
	refresh_address_entries(false);
}

void ScannerController::remove_addresses(const std::vector<int>& rows) {
	if (rows.empty())
		return;
	const auto runtime = runtime_snapshot();
	std::vector<int> descending;
	descending.reserve(rows.size());
	auto& state = memory_scanner::g_state;
	for (const int row : rows) {
		std::optional<memory_scanner::address_entry_t> entry;
		{
			std::lock_guard<std::mutex> lock(state.address_mutex);
			if (row >= 0 && row < static_cast<int>(state.address_list.size()))
				entry = state.address_list[static_cast<std::size_t>(row)];
		}
		if (!entry)
			continue;
		const auto context = memory_interaction::capture_address(runtime,
			entry->address, row, entry->frozen,
			memory_scanner::format_value(entry->last_value, entry->value_type),
			entry->target_pid, entry->target_epoch,
			entry->target_identity.process.creation_time_100ns);
		const auto capability = memory_interaction::evaluate(
			memory_interaction::capability_t::remove, context, runtime);
		if (capability.enabled)
			descending.push_back(row);
	}
	std::sort(descending.begin(), descending.end(), std::greater<int>());
	descending.erase(std::unique(descending.begin(), descending.end()),
		descending.end());
	for (const int row : descending)
		memory_scanner::remove_address(static_cast<std::size_t>(row));
	diag::log_tagged("scan_audit", "[scan_audit] address_list ctx remove");
	refresh_address_entries(false);
}

void ScannerController::add_address(std::uint64_t address,
	const std::string& description, memory_scanner::value_type_t type) {
	memory_scanner::add_address(address, description, type);
	diag::log_tagged("scan_audit", "[scan_audit] memory_scanner add_address invoked");
	refresh_address_entries(false);
}

void ScannerController::edit_description(int row, const std::string& description) {
	auto& state = memory_scanner::g_state;
	{
		std::lock_guard<std::mutex> lock(state.address_mutex);
		if (row >= 0 && row < static_cast<int>(state.address_list.size()))
			state.address_list[static_cast<std::size_t>(row)].description = description;
	}
	scan_diag_logf("dialog edit_desc_save idx=%d", row);
	diag::log_tagged("scan_audit", "[scan_audit] memory_scanner edit_description");
	address_model_->patch_row(row);
}

void ScannerController::change_type(int row, memory_scanner::value_type_t type) {
	auto& state = memory_scanner::g_state;
	{
		std::lock_guard<std::mutex> lock(state.address_mutex);
		if (row >= 0 && row < static_cast<int>(state.address_list.size())) {
			state.address_list[static_cast<std::size_t>(row)].value_type = type;
			state.address_list[static_cast<std::size_t>(row)].last_value.clear();
			state.address_list[static_cast<std::size_t>(row)].freeze_value.clear();
		}
	}
	scan_diag_logf("dialog change_type_save idx=%d vtype=%d", row, static_cast<int>(type));
	address_model_->patch_row(row);
}

bool ScannerController::request_value_write(
	const memory_interaction::context_t& context,
	memory_scanner::value_type_t value_type, std::vector<std::uint8_t> expected,
	std::string& error) {
	if (!memory_scanner::validate_target_binding(context.target_pid, context.target_epoch,
		context.process_creation_time_100ns)) {
		error = "The reviewed process identity is no longer attached.";
		return false;
	}
	bool idle = false;
	if (!write_pending_.compare_exchange_strong(idle, true,
		std::memory_order_acq_rel)) {
		error = "Another reviewed memory write is still pending.";
		return false;
	}
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "memory_scanner";
	submission.label = "Write and verify memory value";
	submission.thread_class = "live_memory_mutation";
	submission.domain = aida::infra::executor::domain_t::feature_worker;
	submission.priority = 4;
	submission.target_pid = context.target_pid;
	submission.generation = context.scan_revision;
	submission.ui_access_policy = "completion_snapshot_only";
	submission.failure_policy = "rollback_and_fail_closed";
	submission.body = [this, context, value_type, expected = std::move(expected)]() mutable {
		auto result = std::make_shared<value_write_result_t>();
		result->context = context;
		result->value_type = value_type;
		try {
			const auto binding_current = [&context]() {
				return memory_scanner::validate_target_binding(context.target_pid,
					context.target_epoch, context.process_creation_time_100ns);
			};
			const auto current_runtime = runtime_snapshot();
			if (!memory_interaction::is_current(context, current_runtime))
				result->detail = "The target or scan generation changed before the memory write.";
			if (result->detail.empty()) {
				std::lock_guard<std::mutex> lock(memory_scanner::g_state.address_mutex);
				if (context.index < 0 || context.index >=
					static_cast<int>(memory_scanner::g_state.address_list.size()))
					result->detail = "The reviewed address-list entry no longer exists.";
				else {
					const auto& entry = memory_scanner::g_state.address_list[
						static_cast<std::size_t>(context.index)];
					if (!address_entry_matches_context(entry, context, value_type))
						result->detail = "The reviewed address-list entry changed before the write.";
				}
			}
			std::vector<std::uint8_t> original;
			if (result->detail.empty()) {
				if (!binding_current()) {
					result->detail = "The reviewed process identity changed before reading original bytes.";
				} else {
					const bool read = driver_bridge::read_memory(
						context.address, expected.size(), original);
					const bool identity_after_read = binding_current();
					if (!identity_after_read)
						result->detail = "The reviewed process identity changed while original bytes were being read.";
					else if (!read || original.size() != expected.size())
						result->detail = "The original target bytes could not be captured exactly.";
				}
			}
			bool write_attempted = false;
			if (result->detail.empty()) {
				if (!binding_current()) {
					result->detail = "The reviewed process identity changed before the memory write.";
				} else {
					write_attempted = true;
					const bool written = driver_bridge::write_memory(context.address, expected);
					const bool identity_after_write = binding_current();
					if (!identity_after_write)
						result->detail = "The reviewed process identity changed during the memory write.";
					else if (!written)
						result->detail = "The target rejected the reviewed memory write.";
				}
			}
			std::vector<std::uint8_t> observed;
			if (result->detail.empty()) {
				if (!binding_current()) {
					result->detail = "The reviewed process identity changed before memory-write verification.";
				} else {
					const bool readback = driver_bridge::read_memory(context.address,
						expected.size(), observed);
					const bool identity_after_readback = binding_current();
					result->verified = readback && identity_after_readback && observed == expected;
					if (!identity_after_readback)
						result->detail = "The reviewed process identity changed during memory-write verification.";
					else if (!result->verified)
						result->detail = "Memory write verification failed.";
				}
			}
			if (result->verified) {
				std::lock_guard<std::mutex> lock(memory_scanner::g_state.address_mutex);
				result->verified = context.index >= 0 && context.index <
						static_cast<int>(memory_scanner::g_state.address_list.size()) &&
					address_entry_matches_context(memory_scanner::g_state.address_list[
						static_cast<std::size_t>(context.index)], context, value_type);
				if (!result->verified)
					result->detail = "The address-list identity changed while the reviewed write was in flight.";
			}
			if (write_attempted && !result->verified) {
				const std::string failure = result->detail.empty()
					? "Memory write verification failed." : result->detail;
				bool rollback_written = false;
				bool rollback_identity_after_write = false;
				if (binding_current()) {
					rollback_written = driver_bridge::write_memory(context.address, original);
					rollback_identity_after_write = binding_current();
				}
				std::vector<std::uint8_t> rollback_readback;
				bool rollback_read = false;
				bool rollback_identity_after_read = false;
				if (rollback_written && rollback_identity_after_write && binding_current()) {
					rollback_read = driver_bridge::read_memory(context.address,
						original.size(), rollback_readback);
					rollback_identity_after_read = binding_current();
				}
				result->rollback_verified = rollback_written &&
					rollback_identity_after_write && rollback_read &&
					rollback_identity_after_read && rollback_readback == original;
				result->detail = failure + (result->rollback_verified
					? " Original bytes were restored and verified for the exact process identity."
					: " Original-byte restoration was not attempted or could not be verified for the exact process identity.");
			}
			if (result->verified) {
				std::lock_guard<std::mutex> lock(memory_scanner::g_state.address_mutex);
				if (context.index >= 0 && context.index <
					static_cast<int>(memory_scanner::g_state.address_list.size())) {
					auto& entry = memory_scanner::g_state.address_list[
						static_cast<std::size_t>(context.index)];
					if (address_entry_matches_context(entry, context, value_type))
						entry.last_value = expected;
				}
				result->detail = "Memory value written and read back exactly.";
			}
		} catch (const std::exception& exception) {
			result->detail = std::string("Memory write failed: ") + exception.what();
		} catch (...) {
			result->detail = "Memory write failed with an unknown error.";
		}
		const bool verified = result->verified;
		const std::string failure = result->detail;
		analysis::gui_post(this, [this, result = std::move(result)]() mutable {
			handle_write_completion(result);
		});
		write_pending_.store(false, std::memory_order_release);
		if (!verified) throw std::runtime_error(failure);
	};
	const auto submitted = aida::infra::executor::submit(std::move(submission));
	if (!submitted.submitted) {
		write_pending_.store(false, std::memory_order_release);
		error = submitted.reject_reason.empty()
			? "The memory-write executor rejected the operation." : submitted.reject_reason;
		return false;
	}
	scanner_task_center::register_executor_task(submitted, "view.memory.value_scan",
		"memory.address.write", "Write and verify memory value", context.target_pid, false);
	return true;
}

void ScannerController::handle_write_completion(
	const std::shared_ptr<const value_write_result_t>& result) {
	if (!result)
		return;
	last_write_result_ = result;
	chrome::AidaToastManager::instance().push(
		QString::fromStdString(result->detail),
		result->verified ? chrome::AidaToastType::success : chrome::AidaToastType::error,
		result->verified ? 3.0 : 6.0);
	address_model_->patch_row(result->context.index);
	Q_EMIT writeCompleted();
	Q_EMIT stateChanged();
}

void ScannerController::open_add_dialog(std::uint64_t address, int value_type,
	QWidget* parent) {
	auto* dialog = new AddAddressDialog(address, value_type, parent);
	dialog->open();
}

void ScannerController::open_edit_description_dialog(int row, QWidget* parent) {
	auto& state = memory_scanner::g_state;
	QString current;
	std::uint64_t address = 0;
	{
		std::lock_guard<std::mutex> lock(state.address_mutex);
		if (row >= 0 && row < static_cast<int>(state.address_list.size())) {
			current = QString::fromStdString(
				state.address_list[static_cast<std::size_t>(row)].description);
			address = state.address_list[static_cast<std::size_t>(row)].address;
		}
	}
	auto* dialog = new EditDescriptionDialog(row, address, current, parent);
	dialog->open();
}

void ScannerController::open_change_value_dialog(int row, QWidget* parent) {
	auto* dialog = new ChangeValueDialog(row, parent);
	dialog->open();
}

void ScannerController::open_change_type_dialog(int row, QWidget* parent) {
	auto& state = memory_scanner::g_state;
	int current_type = static_cast<int>(memory_scanner::value_type_t::int32_val);
	{
		std::lock_guard<std::mutex> lock(state.address_mutex);
		if (row >= 0 && row < static_cast<int>(state.address_list.size()))
			current_type = static_cast<int>(
				state.address_list[static_cast<std::size_t>(row)].value_type);
	}
	auto* dialog = new ChangeTypeDialog(row, current_type, parent);
	dialog->open();
}

void ScannerController::refresh_from_engine() {
	adopt_published_results();
	refresh_address_entries(false);
	Q_EMIT stateChanged();
}

void ScannerController::open_or_focus_view(const char* view_id) {
	if (!host_ || !view_id)
		return;
	static_cast<void>(host_->open_or_focus(
		registry::stable_view_id_t(view_id)));
}

void ScannerController::refresh_actions() {
	if (actions_)
		actions_->ensure_current();
}

scan_command_state_t scan_command_capability(scan_command_t command) {
	return ScannerController::instance().command_capability(command);
}

scan_command_result_t execute_scan_command(scan_command_t command) {
	return ScannerController::instance().execute_command(command);
}

}
