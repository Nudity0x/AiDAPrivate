#include "qt/scanner/aob_controller.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "core/disasm/disasm_view.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/scanner/scanner_async_io.hpp"
#include "core/ui/task_center.hpp"
#include "core/ui/ui_thread_dispatcher.hpp"
#include "helpers/diag_log.hpp"

#include "qt/bridge/clipboard.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/docking/dock_host.hpp"

namespace aida::qt::scanner {

namespace {

std::atomic<std::uint64_t> operation_sequence{1};

std::string register_operation(const char* action, const char* label,
	const std::string& target, const std::shared_ptr<std::atomic<bool>>& cancellation,
	const std::shared_ptr<std::atomic<std::uint8_t>>& commit_gate = {})
{
	const std::string id = "scanner.aob.operation." +
		std::to_string(operation_sequence.fetch_add(1, std::memory_order_acq_rel));
	aida::ui::task_center::task_registration_t registration;
	registration.id = id;
	registration.source = "human";
	registration.owner = "AOB Generator";
	registration.owner_view = "view.memory.aob";
	registration.owner_action = action;
	registration.target = target;
	registration.label = label;
	registration.stage = "Queued";
	registration.progress = -1.0f;
	registration.cancellation_is_safe = static_cast<bool>(cancellation);
	if (cancellation) registration.callbacks.cancel = [cancellation, commit_gate] {
		if (commit_gate) {
			std::uint8_t expected_gate = scanner_async_io::operation_reversible;
			if (!commit_gate->compare_exchange_strong(expected_gate,
				scanner_async_io::operation_cancelled, std::memory_order_acq_rel,
				std::memory_order_acquire)) return false;
		}
		bool expected = false;
		return cancellation->compare_exchange_strong(expected, true, std::memory_order_acq_rel);
	};
	registration.callbacks.focus = [] {
		if (auto* host = AobController::instance().host())
			static_cast<void>(host->open_or_focus(
				registry::stable_view_id_t("view.memory.aob")));
	};
	return aida::ui::task_center::register_task(std::move(registration)) ? id : std::string();
}

void finish_operation(const std::string& task_id, aob_terminal_t terminal,
	const std::string& stage, const std::string& summary)
{
	if (task_id.empty()) return;
	auto task_state = aida::ui::task_center::task_state_t::failed;
	if (terminal == aob_terminal_t::succeeded)
		task_state = aida::ui::task_center::task_state_t::completed;
	else if (terminal == aob_terminal_t::cancelled || terminal == aob_terminal_t::stale)
		task_state = aida::ui::task_center::task_state_t::cancelled;
	static_cast<void>(aida::ui::task_center::update_task(task_id, task_state, 1.0f,
		stage, summary));
}

void set_operation_status(const std::shared_ptr<aob_view_state_t>& state,
	aob_operation_status_t aob_view_state_t::* member, aob_terminal_t terminal,
	std::string message, std::string path = {})
{
	if (!state) return;
	std::lock_guard<std::mutex> lock(state->operation_mutex);
	auto& status = state.get()->*member;
	status.terminal = terminal;
	status.message = std::move(message);
	status.path = std::move(path);
}

bool workspace_matches(const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
	const std::string& binary_id, std::uint64_t workspace_generation,
	std::uint64_t publication_generation, std::uint32_t pid)
{
	if (!workspace || workspace->closing() || workspace->closed() ||
		workspace->identity().binary_id().to_hex() != binary_id ||
		workspace->generation() != workspace_generation) return false;
	const auto publication = workspace->analysis_publication();
	if (!publication || publication->generation != publication_generation) return false;
	const auto process = workspace->identity().process();
	return pid == 0 ? !process : process && process->pid == pid;
}

}

AobController& AobController::instance()
{
	static AobController* controller = new AobController();
	return *controller;
}

AobController::AobController(QObject* parent) : QObject(parent) {}

void AobController::install(docking::AidaDockHost* host)
{
	host_ = host;
}

std::shared_ptr<aob_view_state_t> AobController::view_state_for(
	const disasm_view::workspace_context_t& context)
{
	if (!context.workspace) return {};
	return view_state_for_key(context.workspace->identity().binary_id().to_hex());
}

std::shared_ptr<aob_view_state_t> AobController::view_state_for_key(
	const std::string& binary_id_hex)
{
	std::lock_guard<std::mutex> lock(states_mutex_);
	auto& state = states_[binary_id_hex];
	if (!state)
		state = std::make_shared<aob_view_state_t>();
	return state;
}

void AobController::reconcile_dispatch_failures(
	const std::shared_ptr<aob_view_state_t>& state)
{
	if (!state) return;
	if (state->export_dispatch_failed.exchange(false, std::memory_order_acq_rel))
		set_operation_status(state, &aob_view_state_t::export_status, aob_terminal_t::failed,
			"UI dispatcher rejected AOB export completion");
	if (state->catalog_dispatch_failed.exchange(false, std::memory_order_acq_rel))
		set_operation_status(state, &aob_view_state_t::catalog_status, aob_terminal_t::failed,
			"UI dispatcher rejected AOB catalog completion");
	if (state->comparison_dispatch_failed.exchange(false, std::memory_order_acq_rel))
		set_operation_status(state, &aob_view_state_t::comparison_status, aob_terminal_t::failed,
			"UI dispatcher rejected AOB comparison completion");
}

void AobController::poll(const std::shared_ptr<aob_generator::state_t>& generator,
	const std::shared_ptr<aob_view_state_t>& state)
{
	if (!generator || !state)
		return;
	reconcile_dispatch_failures(state);
	std::string pending_clip;
	if (aob_generator::take_pending_clipboard(generator, pending_clip))
		clipboard::set_text(QString::fromStdString(pending_clip));
	const std::uint64_t catalog_generation =
		generator->catalog_generation.load(std::memory_order_acquire);
	if (state->render_catalog_generation != catalog_generation) {
		std::lock_guard<std::mutex> lk(generator->mutex);
		const std::uint64_t locked_generation =
			generator->catalog_generation.load(std::memory_order_relaxed);
		if (state->render_catalog_generation != locked_generation) {
			state->render_catalog = generator->saved_signatures;
			state->render_catalog_generation = locked_generation;
			if (state->selected_saved >= 0) {
				const auto selected = std::find_if(state->render_catalog.begin(),
					state->render_catalog.end(), [&](const auto& signature) {
						return signature.address == state->context_address &&
							signature.name == state->context_name;
					});
				state->selected_saved = selected == state->render_catalog.end() ? -1 :
					static_cast<int>(std::distance(state->render_catalog.begin(), selected));
			}
			Q_EMIT stateChanged();
		}
	}
	if (state->selected_saved < 0 ||
		static_cast<std::size_t>(state->selected_saved) >= state->render_catalog.size()) {
		state->selected_saved = -1;
		state->context_address = 0;
		state->context_name.clear();
	}
}

void AobController::request_generate(const disasm_view::workspace_context_t& context)
{
	const auto generator_state = aob_generator::state_for(context);
	if (!generator_state)
		return;
	auto& gen = *generator_state;
	const bool generating = gen.generating.load();
	if (generating)
		return;
	diag::log_tagged_fmt("aob",
		"view generate_button_clicked input='%s' count=%d auto_wildcard=%d generating=%d",
		gen.address_input, gen.instruction_count,
		static_cast<int>(gen.auto_wildcard),
		static_cast<int>(generating));
	chrome::toast_info(QStringLiteral("AOB: Generating signature..."), 1.5);
	uint64_t addr = 0;
	if (gen.address_input[0]) {
		const char* p = gen.address_input;
		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
		addr = std::strtoull(p, nullptr, 16);
	}
	if (addr == 0) {
		uint64_t fallback = 0;
		{
			std::lock_guard<std::mutex> lk(gen.mutex);
			fallback = gen.last_request_addr;
			if (fallback == 0 && gen.current.address != 0)
				fallback = gen.current.address;
		}
		if (fallback != 0) {
			diag::log_tagged_fmt("aob",
				"view generate using_fallback_address va=0x%llX",
				static_cast<unsigned long long>(fallback));
			addr = fallback;
			std::snprintf(gen.address_input, sizeof(gen.address_input),
				"%llX", static_cast<unsigned long long>(addr));
		}
	}
	if (addr != 0) {
		diag::log_tagged_fmt("aob",
			"view generate dispatching addr=0x%llX count=%d",
			static_cast<unsigned long long>(addr), gen.instruction_count);
		aob_generator::generate_from_address(context, addr, gen.instruction_count,
			gen.auto_wildcard);
		return;
	}
	const bool live = context.workspace && context.workspace->target_kind() ==
		aida::analysis::target_kind_t::live_snapshot;
	const bool pe = context.workspace && context.workspace->target_kind() ==
		aida::analysis::target_kind_t::static_file && static_cast<bool>(context.image);
	diag::log_tagged_fmt("aob",
		"view generate refused parse_failed input='%s' live=%d pe=%d",
		gen.address_input, live ? 1 : 0, pe ? 1 : 0);
	chrome::toast_warning(QStringLiteral(
		"AOB: Enter a hex address (e.g. 7FF6A1B20040) or click an instruction in the disassembly first."), 5.0);
	{
		std::lock_guard<std::mutex> lk(gen.mutex);
		if (!live && !pe) {
			gen.last_error =
				"No data source attached. Open a PE file or attach a process before generating signatures.";
		} else {
			gen.last_error =
				"Address is empty or invalid. Enter a hexadecimal address (e.g. 7FF6A1B20040) or click an instruction in the disassembly first.";
		}
		gen.show_no_address_modal = true;
	}
	Q_EMIT stateChanged();
}

void AobController::request_regenerate(const disasm_view::workspace_context_t& context)
{
	const auto generator_state = aob_generator::state_for(context);
	if (!generator_state || generator_state->generating.load())
		return;
	aob_generator::regenerate_last(context, generator_state);
}

void AobController::request_save_current(
	const disasm_view::workspace_context_t& context)
{
	const auto generator_state = aob_generator::state_for(context);
	if (!generator_state)
		return;
	aob_generator::save_current(generator_state);
	Q_EMIT stateChanged();
}

void AobController::request_optimize(const disasm_view::workspace_context_t& context)
{
	const auto generator_state = aob_generator::state_for(context);
	if (!generator_state || !context.workspace)
		return;
	const auto process = context.workspace->target_kind() ==
		aida::analysis::target_kind_t::live_snapshot
		? context.workspace->identity().process() : std::nullopt;
	const std::uint32_t live_pid = process ? process->pid : 0;
	const bool attached_live = driver_bridge::is_loaded() && live_pid != 0;
	if (!attached_live)
		return;
	aob_generator::signature_t to_optimize;
	{
		std::lock_guard<std::mutex> lk(generator_state->mutex);
		to_optimize = generator_state->current;
	}
	diag::log_tagged("scan_audit", "[scan_audit] aob optimize invoked");
	aida::infra::executor::submission_t sub;
	sub.owner_subsystem = "scanner";
	sub.label = "scanner.aob_optimize";
	sub.thread_class = "scanner_sweep";
	sub.domain = aida::infra::executor::domain_t::long_running;
	sub.priority = 2;
	sub.target_pid = live_pid;
	sub.body = [live_pid, to_optimize, generator_state]() mutable {
		aob_generator::optimize_signature(live_pid, to_optimize);
		std::lock_guard<std::mutex> lk(generator_state->mutex);
		if (generator_state->current.id == to_optimize.id)
			generator_state->current = std::move(to_optimize);
	};
	if (!aida::infra::executor::submit(std::move(sub)).submitted)
		diag::log_tagged("aob", "optimize worker_queue_rejected");
}

void AobController::request_export(const disasm_view::workspace_context_t& context,
	aob_generator::export_format_t format, std::string requested_path)
{
	const auto generator = aob_generator::state_for(context);
	const auto state = view_state_for(context);
	if (!context.workspace || !context.publication || !generator || !state) return;
	bool expected = false;
	if (!state->export_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
	const std::uint64_t serial = state->export_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
	std::vector<aob_generator::signature_t> signatures;
	std::uint64_t catalog_generation = 0;
	{
		std::lock_guard<std::mutex> lock(generator->mutex);
		signatures = generator->saved_signatures;
		catalog_generation = generator->catalog_generation.load(std::memory_order_acquire);
	}
	state->last_export_format = format;
	state->last_export_path = requested_path;
	set_operation_status(state, &aob_view_state_t::export_status, aob_terminal_t::queued,
		"Queued immutable AOB export", requested_path);
	const auto workspace = context.workspace;
	const std::string binary_id = workspace->identity().binary_id().to_hex();
	const std::uint64_t workspace_generation = workspace->generation();
	const std::uint64_t publication_generation = context.publication->generation;
	const auto process = workspace->identity().process();
	const std::uint32_t pid = process ? process->pid : 0;
	auto cancellation = std::make_shared<std::atomic<bool>>(false);
	auto commit_gate = std::make_shared<std::atomic<std::uint8_t>>(
		scanner_async_io::operation_reversible);
	const std::string task_id = register_operation("scanner.aob.export", "Export AOB signatures",
		binary_id, cancellation, commit_gate);
	if (task_id.empty()) {
		state->export_pending.store(false, std::memory_order_release);
		set_operation_status(state, &aob_view_state_t::export_status, aob_terminal_t::failed,
			"Task Center rejected AOB export ownership", requested_path);
		return;
	}
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "scanner.aob";
	submission.label = "scanner.aob.export";
	submission.thread_class = "bounded_task";
	submission.domain = aida::infra::executor::domain_t::feature_worker;
	submission.priority = 3;
	submission.target_pid = pid;
	submission.generation = publication_generation;
	submission.cancel_hook = [cancellation, commit_gate] {
		std::uint8_t expected_gate = scanner_async_io::operation_reversible;
		if (commit_gate->compare_exchange_strong(expected_gate, scanner_async_io::operation_cancelled,
			std::memory_order_acq_rel, std::memory_order_acquire))
			cancellation->store(true, std::memory_order_release);
	};
	submission.body = [this, workspace, generator, state, signatures = std::move(signatures), format,
		requested_path = std::move(requested_path), cancellation, task_id, binary_id,
		workspace_generation, publication_generation, pid, catalog_generation, serial,
		commit_gate]() mutable {
		static_cast<void>(aida::ui::task_center::update_task(task_id,
			aida::ui::task_center::task_state_t::running, 0.1f, "Serializing bounded AOB catalog"));
		static_cast<void>(aida::ui_thread::post([state, serial] {
			if (state->export_serial.load(std::memory_order_acquire) == serial &&
				state->export_pending.load(std::memory_order_acquire))
				set_operation_status(state, &aob_view_state_t::export_status, aob_terminal_t::running,
					"Serializing bounded AOB export");
		}, "scanner.aob", "publish_export_running", "worker_progress"));
		std::string output;
		std::string error;
		bool serialized = aob_generator::serialize_catalog(signatures, format, output, error, cancellation);
		std::string path = requested_path;
		if (serialized && path.empty()) {
			const std::string cache = aob_generator::get_aob_cache_dir();
			if (!cache.empty()) {
				const std::string extension = format == aob_generator::export_format_t::json ? ".json" :
					format == aob_generator::export_format_t::yara ? ".yar" : ".hpp";
				path = (std::filesystem::path(cache).parent_path() /
					("aob_export" + extension)).string();
			}
			if (path.empty()) { serialized = false; error = "APPDATA is unavailable for AOB export"; }
		}
		auto current = [workspace, generator, binary_id, workspace_generation,
			publication_generation, pid, catalog_generation] {
			return workspace_matches(workspace, binary_id, workspace_generation,
				publication_generation, pid) &&
				generator->catalog_generation.load(std::memory_order_acquire) == catalog_generation;
		};
		scanner_async_io::result_t write;
		if (serialized && current())
			write = scanner_async_io::atomic_replace(path, output, true, cancellation, current, commit_gate);
		else if (serialized)
			write.error = "AOB workspace, target, publication, or catalog generation changed";
		const bool cancelled = scanner_async_io::cancellation_requested(cancellation) || write.cancelled;
		const bool stale = !cancelled && !current();
		const bool success = serialized && write.success && !stale;
		if (!success && error.empty()) error = write.error;
		const aob_terminal_t terminal = success ? aob_terminal_t::succeeded :
			cancelled ? aob_terminal_t::cancelled : stale ? aob_terminal_t::stale :
			aob_terminal_t::failed;
		auto publish = [this, state, serial, terminal, error = std::move(error), path]() mutable {
			if (state->export_serial.load(std::memory_order_acquire) != serial) return;
			set_operation_status(state, &aob_view_state_t::export_status, terminal,
				terminal == aob_terminal_t::succeeded ? "AOB export committed atomically" : error, path);
			state->export_pending.store(false, std::memory_order_release);
			Q_EMIT stateChanged();
		};
		finish_operation(task_id, terminal, success ? "AOB export complete" : "AOB export did not commit",
			success ? path : error);
		if (!aida::ui_thread::post(std::move(publish), "scanner.aob", "publish_export", "worker_completion")) {
			state->export_pending.store(false, std::memory_order_release);
			state->export_dispatch_failed.store(true, std::memory_order_release);
			finish_operation(task_id, aob_terminal_t::failed, "UI publication rejected",
				"AOB export completion was not published");
		}
	};
	const auto submitted = aida::infra::executor::submit(std::move(submission));
	if (!submitted.submitted) {
		state->export_pending.store(false, std::memory_order_release);
		set_operation_status(state, &aob_view_state_t::export_status, aob_terminal_t::failed,
			"Worker queue rejected AOB export: " + submitted.reject_reason, state->last_export_path);
		finish_operation(task_id, aob_terminal_t::failed, "Worker queue rejected", submitted.reject_reason);
	}
	Q_EMIT stateChanged();
}

void AobController::request_catalog(const disasm_view::workspace_context_t& context,
	bool save)
{
	const auto generator = aob_generator::state_for(context);
	const auto state = view_state_for(context);
	if (!context.workspace || !context.publication || !generator || !state) return;
	bool expected = false;
	if (!state->catalog_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
	const std::uint64_t serial = state->catalog_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
	state->last_catalog_save = save;
	std::vector<aob_generator::signature_t> signatures;
	std::uint64_t catalog_generation = 0;
	{
		std::lock_guard<std::mutex> lock(generator->mutex);
		signatures = generator->saved_signatures;
		catalog_generation = generator->catalog_generation.load(std::memory_order_acquire);
	}
	set_operation_status(state, &aob_view_state_t::catalog_status, aob_terminal_t::queued,
		save ? "Queued saved-signature catalog commit" : "Queued saved-signature catalog load");
	const auto workspace = context.workspace;
	const std::string binary_id = workspace->identity().binary_id().to_hex();
	const std::uint64_t workspace_generation = workspace->generation();
	const std::uint64_t publication_generation = context.publication->generation;
	const auto process = workspace->identity().process();
	const std::uint32_t pid = process ? process->pid : 0;
	auto cancellation = std::make_shared<std::atomic<bool>>(false);
	auto commit_gate = std::make_shared<std::atomic<std::uint8_t>>(
		scanner_async_io::operation_reversible);
	const std::string task_id = register_operation(save ? "scanner.aob.catalog.save" : "scanner.aob.catalog.load",
		save ? "Save AOB signature catalog" : "Load AOB signature catalog", binary_id,
		cancellation, commit_gate);
	if (task_id.empty()) {
		state->catalog_pending.store(false, std::memory_order_release);
		set_operation_status(state, &aob_view_state_t::catalog_status, aob_terminal_t::failed,
			"Task Center rejected AOB catalog ownership");
		return;
	}
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "scanner.aob";
	submission.label = save ? "scanner.aob.catalog.save" : "scanner.aob.catalog.load";
	submission.thread_class = "bounded_task";
	submission.domain = aida::infra::executor::domain_t::feature_worker;
	submission.priority = 3;
	submission.target_pid = pid;
	submission.generation = publication_generation;
	submission.cancel_hook = [cancellation, commit_gate] {
		std::uint8_t expected_gate = scanner_async_io::operation_reversible;
		if (commit_gate->compare_exchange_strong(expected_gate, scanner_async_io::operation_cancelled,
			std::memory_order_acq_rel, std::memory_order_acquire))
			cancellation->store(true, std::memory_order_release);
	};
	submission.body = [this, workspace, generator, state, signatures = std::move(signatures), save,
		cancellation, task_id, binary_id, workspace_generation, publication_generation,
		pid, catalog_generation, serial, commit_gate]() mutable {
		static_cast<void>(aida::ui::task_center::update_task(task_id,
			aida::ui::task_center::task_state_t::running, 0.1f,
			save ? "Serializing saved AOB catalog" : "Reading and validating saved AOB catalog"));
		static_cast<void>(aida::ui_thread::post([state, serial, save] {
			if (state->catalog_serial.load(std::memory_order_acquire) == serial &&
				state->catalog_pending.load(std::memory_order_acquire))
				set_operation_status(state, &aob_view_state_t::catalog_status, aob_terminal_t::running,
					save ? "Serializing saved AOB catalog" : "Reading and validating saved AOB catalog");
		}, "scanner.aob", "publish_catalog_running", "worker_progress"));
		const std::string directory = aob_generator::get_aob_cache_dir();
		const std::string path = directory.empty() ? std::string() :
			(directory + "\\" + binary_id + ".json");
		auto current = [workspace, generator, binary_id, workspace_generation,
			publication_generation, pid, catalog_generation] {
			return workspace_matches(workspace, binary_id, workspace_generation,
				publication_generation, pid) &&
				generator->catalog_generation.load(std::memory_order_acquire) == catalog_generation;
		};
		std::string error;
		bool success = false;
		bool cancelled = false;
		std::vector<aob_generator::signature_t> staged;
		std::uint64_t maximum_id = 0;
		if (path.empty()) {
			error = "APPDATA is unavailable for the AOB saved catalog";
		} else if (save) {
			std::string output;
			if (aob_generator::serialize_catalog(signatures, aob_generator::export_format_t::json,
				output, error, cancellation) && current()) {
				auto write = scanner_async_io::atomic_replace(path, output, true, cancellation, current, commit_gate);
				success = write.success;
				cancelled = write.cancelled;
				if (!success && error.empty()) error = write.error;
			} else if (error.empty()) {
				error = "AOB catalog changed before saved-catalog commit";
			}
		} else {
			std::string input;
			auto read = scanner_async_io::read_bounded(path, aob_generator::max_catalog_file_bytes,
				cancellation, input);
			cancelled = read.cancelled;
			if (read.success && current())
				success = aob_generator::parse_catalog(input, staged, maximum_id, error, cancellation);
			else if (!read.success) error = read.error;
			else error = "AOB catalog changed before saved-catalog validation";
		}
		cancelled = cancelled || scanner_async_io::cancellation_requested(cancellation);
		const bool stale = !cancelled && !current();
		if (stale) { success = false; error = "AOB workspace, target, publication, or catalog generation changed"; }
		const aob_terminal_t terminal = success ? aob_terminal_t::succeeded :
			cancelled ? aob_terminal_t::cancelled : stale ? aob_terminal_t::stale :
			aob_terminal_t::failed;
		auto publish = [this, workspace, generator, state, save, staged = std::move(staged), maximum_id,
			task_id, binary_id, workspace_generation, publication_generation, pid,
			catalog_generation, serial, terminal, error = std::move(error), path, commit_gate]() mutable {
			if (state->catalog_serial.load(std::memory_order_acquire) != serial) return;
			aob_terminal_t final_terminal = terminal;
			std::string final_error = error;
			if (terminal == aob_terminal_t::succeeded && !save) {
				std::uint8_t expected_gate = scanner_async_io::operation_reversible;
				if (!commit_gate->compare_exchange_strong(expected_gate,
					scanner_async_io::operation_committing, std::memory_order_acq_rel,
					std::memory_order_acquire)) {
					final_terminal = expected_gate == scanner_async_io::operation_cancelled
						? aob_terminal_t::cancelled : aob_terminal_t::failed;
					final_error = "AOB catalog load was cancelled before publication";
				} else if (!workspace_matches(workspace, binary_id, workspace_generation,
					publication_generation, pid) ||
					generator->catalog_generation.load(std::memory_order_acquire) != catalog_generation) {
					final_terminal = aob_terminal_t::stale;
					final_error = "AOB catalog changed before atomic publication";
				} else {
					std::lock_guard<std::mutex> lock(generator->mutex);
					generator->saved_signatures = std::move(staged);
					generator->catalog_generation.fetch_add(1, std::memory_order_acq_rel);
					std::uint64_t next = aob_generator::g_next_signature_id.load(std::memory_order_acquire);
					while (maximum_id >= next && !aob_generator::g_next_signature_id.compare_exchange_weak(
						next, maximum_id + 1, std::memory_order_acq_rel)) {}
				}
			}
			set_operation_status(state, &aob_view_state_t::catalog_status, final_terminal,
				final_terminal == aob_terminal_t::succeeded
					? (save ? "Saved-signature catalog committed atomically" :
						"Validated saved-signature catalog published atomically") : final_error, path);
			state->catalog_pending.store(false, std::memory_order_release);
			finish_operation(task_id, final_terminal,
				final_terminal == aob_terminal_t::succeeded ? "AOB catalog complete" : "AOB catalog failed",
				final_terminal == aob_terminal_t::succeeded ? path : final_error);
			Q_EMIT stateChanged();
		};
		if (!aida::ui_thread::post(std::move(publish), "scanner.aob", "publish_catalog", "worker_completion")) {
			state->catalog_pending.store(false, std::memory_order_release);
			state->catalog_dispatch_failed.store(true, std::memory_order_release);
			finish_operation(task_id, aob_terminal_t::failed, "UI publication rejected",
				"AOB catalog completion was not published");
		}
	};
	const auto submitted = aida::infra::executor::submit(std::move(submission));
	if (!submitted.submitted) {
		state->catalog_pending.store(false, std::memory_order_release);
		set_operation_status(state, &aob_view_state_t::catalog_status, aob_terminal_t::failed,
			"Worker queue rejected AOB catalog operation: " + submitted.reject_reason);
		finish_operation(task_id, aob_terminal_t::failed, "Worker queue rejected", submitted.reject_reason);
	}
	Q_EMIT stateChanged();
}

void AobController::request_comparison(const disasm_view::workspace_context_t& context)
{
	const auto generator = aob_generator::state_for(context);
	const auto state = view_state_for(context);
	if (!context.workspace || !context.publication || !generator || !state) return;
	const auto process = context.workspace->identity().process();
	if (!process || process->pid == 0) {
		set_operation_status(state, &aob_view_state_t::comparison_status, aob_terminal_t::failed,
			"Attach a live process before comparing signatures");
		return;
	}
	bool expected = false;
	if (!state->comparison_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
	const std::uint64_t serial = state->comparison_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
	std::vector<aob_generator::signature_t> signatures;
	std::uint64_t catalog_generation = 0;
	{
		std::lock_guard<std::mutex> lock(generator->mutex);
		signatures = generator->saved_signatures;
		catalog_generation = generator->catalog_generation.load(std::memory_order_acquire);
	}
	if (signatures.empty()) {
		state->comparison_pending.store(false, std::memory_order_release);
		set_operation_status(state, &aob_view_state_t::comparison_status, aob_terminal_t::failed,
			"Save at least one signature before comparing");
		return;
	}
	set_operation_status(state, &aob_view_state_t::comparison_status, aob_terminal_t::queued,
		"Queued immutable AOB comparison");
	const auto workspace = context.workspace;
	const std::string binary_id = workspace->identity().binary_id().to_hex();
	const std::uint64_t workspace_generation = workspace->generation();
	const std::uint64_t publication_generation = context.publication->generation;
	const std::uint32_t pid = process->pid;
	auto cancellation = std::make_shared<std::atomic<bool>>(false);
	auto commit_gate = std::make_shared<std::atomic<std::uint8_t>>(
		scanner_async_io::operation_reversible);
	const std::string task_id = register_operation("scanner.aob.compare", "Compare AOB signatures",
		binary_id, cancellation, commit_gate);
	if (task_id.empty()) {
		state->comparison_pending.store(false, std::memory_order_release);
		set_operation_status(state, &aob_view_state_t::comparison_status, aob_terminal_t::failed,
			"Task Center rejected AOB comparison ownership");
		return;
	}
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "scanner.aob";
	submission.label = "scanner.aob.compare";
	submission.thread_class = "scanner_sweep";
	submission.domain = aida::infra::executor::domain_t::long_running;
	submission.priority = 2;
	submission.target_pid = pid;
	submission.generation = publication_generation;
	submission.cancel_hook = [cancellation, commit_gate] {
		std::uint8_t expected_gate = scanner_async_io::operation_reversible;
		if (commit_gate->compare_exchange_strong(expected_gate, scanner_async_io::operation_cancelled,
			std::memory_order_acq_rel, std::memory_order_acquire))
			cancellation->store(true, std::memory_order_release);
	};
	submission.body = [this, workspace, generator, state, signatures = std::move(signatures), cancellation,
		task_id, binary_id, workspace_generation, publication_generation, pid,
		catalog_generation, serial, commit_gate]() mutable {
		static_cast<void>(aida::ui::task_center::update_task(task_id,
			aida::ui::task_center::task_state_t::running, -1.0f, "Comparing signatures against live memory"));
		static_cast<void>(aida::ui_thread::post([state, serial] {
			if (state->comparison_serial.load(std::memory_order_acquire) == serial &&
				state->comparison_pending.load(std::memory_order_acquire))
				set_operation_status(state, &aob_view_state_t::comparison_status, aob_terminal_t::running,
					"Comparing signatures against live memory");
		}, "scanner.aob", "publish_comparison_running", "worker_progress"));
		auto current = [workspace, generator, binary_id, workspace_generation,
			publication_generation, pid, catalog_generation] {
			return workspace_matches(workspace, binary_id, workspace_generation,
				publication_generation, pid) &&
				generator->catalog_generation.load(std::memory_order_acquire) == catalog_generation;
		};
		std::string error;
		auto results = aob_generator::compare_signatures_against_process(
			pid, signatures, cancellation, current, error);
		const bool cancelled = scanner_async_io::cancellation_requested(cancellation);
		const bool stale = !cancelled && !current();
		const bool success = !stale && !cancelled && error.empty() && results.size() == signatures.size();
		const aob_terminal_t terminal = success ? aob_terminal_t::succeeded :
			cancelled ? aob_terminal_t::cancelled : stale ? aob_terminal_t::stale :
			aob_terminal_t::failed;
		auto publish = [this, workspace, generator, state, signatures = std::move(signatures),
			results = std::move(results), task_id, binary_id, workspace_generation,
			publication_generation, pid, catalog_generation, serial, terminal,
			error = std::move(error), commit_gate]() mutable {
			if (state->comparison_serial.load(std::memory_order_acquire) != serial) return;
			aob_terminal_t final_terminal = terminal;
			std::string final_error = error;
			if (terminal == aob_terminal_t::succeeded) {
				std::uint8_t expected_gate = scanner_async_io::operation_reversible;
				if (!commit_gate->compare_exchange_strong(expected_gate,
					scanner_async_io::operation_committing, std::memory_order_acq_rel,
					std::memory_order_acquire)) {
					final_terminal = expected_gate == scanner_async_io::operation_cancelled
						? aob_terminal_t::cancelled : aob_terminal_t::failed;
					final_error = "AOB comparison was cancelled before publication";
				} else if (!workspace_matches(workspace, binary_id, workspace_generation,
					publication_generation, pid) ||
					generator->catalog_generation.load(std::memory_order_acquire) != catalog_generation) {
					final_terminal = aob_terminal_t::stale;
					final_error = "AOB comparison target or catalog changed before publication";
				} else {
					std::lock_guard<std::mutex> lock(generator->mutex);
					for (std::size_t index = 0; index < results.size(); ++index) {
						auto found = std::find_if(generator->saved_signatures.begin(), generator->saved_signatures.end(),
							[id = signatures[index].id](const auto& item) { return item.id == id; });
						if (found == generator->saved_signatures.end()) continue;
						found->unique = results[index].still_found;
						found->uniqueness_count = results[index].match_count;
						found->quality_score = aob_generator::compute_quality_score(*found);
					}
					generator->catalog_generation.fetch_add(1, std::memory_order_acq_rel);
				}
			}
			set_operation_status(state, &aob_view_state_t::comparison_status, final_terminal,
				final_terminal == aob_terminal_t::succeeded
					? "AOB comparison published atomically" : final_error);
			state->comparison_pending.store(false, std::memory_order_release);
			finish_operation(task_id, final_terminal,
				final_terminal == aob_terminal_t::succeeded ? "AOB comparison complete" : "AOB comparison failed",
				final_terminal == aob_terminal_t::succeeded ? binary_id : final_error);
			Q_EMIT stateChanged();
		};
		if (!aida::ui_thread::post(std::move(publish), "scanner.aob", "publish_comparison", "worker_completion")) {
			state->comparison_pending.store(false, std::memory_order_release);
			state->comparison_dispatch_failed.store(true, std::memory_order_release);
			finish_operation(task_id, aob_terminal_t::failed, "UI publication rejected",
				"AOB comparison completion was not published");
		}
	};
	const auto submitted = aida::infra::executor::submit(std::move(submission));
	if (!submitted.submitted) {
		state->comparison_pending.store(false, std::memory_order_release);
		set_operation_status(state, &aob_view_state_t::comparison_status, aob_terminal_t::failed,
			"Worker queue rejected AOB comparison: " + submitted.reject_reason);
		finish_operation(task_id, aob_terminal_t::failed, "Worker queue rejected", submitted.reject_reason);
	}
	Q_EMIT stateChanged();
}

void AobController::select_saved(const std::shared_ptr<aob_view_state_t>& state,
	int index, std::uint64_t address, const std::string& name)
{
	if (!state)
		return;
	state->selected_saved = index;
	state->context_address = index >= 0 ? address : 0;
	state->context_name = index >= 0 ? name : std::string();
}

}
