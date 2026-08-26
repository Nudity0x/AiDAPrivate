#include "qt/scanner/scanner_context_menus.hpp"

#include <QFileDialog>
#include <QKeyEvent>
#include <QPoint>
#include <QTableView>
#include <QWidget>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "core/ai/entity_evidence_handoff.hpp"
#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/debugger/debugger_view.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/disasm/function_index.hpp"
#include "core/editor/hex_view.hpp"
#include "core/network/burp/comparer.hpp"
#include "core/scanner/memory_scanner.hpp"
#include "core/scanner/pointer_scanner.hpp"
#include "core/scanner/scanner_async_io.hpp"
#include "core/scanner/scanner_task_center.hpp"
#include "helpers/diag_log.hpp"

#include "qt/analysis/qt_analysis_host_hooks.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/scanner/scanner_controller.hpp"

namespace aida::qt::scanner {

namespace {

using aida::ui::action_handler_result_t;
using aida::ui::capability_state_t;

void menu_diag_log(const char* msg) {
	diag::log_tagged("value_scan", msg);
}

void menu_diag_logf(const char* fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	diag::log_tagged("value_scan", buf);
}

memory_interaction::runtime_t runtime_snapshot() {
	return ScannerController::instance().runtime_snapshot();
}

bool memory_context_identity_equal(const memory_interaction::context_t& left,
	const memory_interaction::context_t& right) noexcept {
	return left.kind == right.kind && left.source == right.source &&
		left.target_pid == right.target_pid && left.target_epoch == right.target_epoch &&
		left.process_creation_time_100ns == right.process_creation_time_100ns &&
		left.scan_revision == right.scan_revision && left.workspace_id == right.workspace_id &&
		left.workspace_generation == right.workspace_generation &&
		left.owner_workspace_id == right.owner_workspace_id &&
		left.owner_workspace_generation == right.owner_workspace_generation &&
		left.document_id == right.document_id &&
		left.address == right.address && left.extent == right.extent && left.index == right.index;
}

std::vector<memory_interaction::context_t> memory_action_contexts(
	const memory_interaction::context_t& focused) {
	auto contexts = memory_interaction::selected_set();
	const bool compatible = !contexts.empty() && contexts.front().kind == focused.kind &&
		contexts.front().source == focused.source &&
		contexts.front().target_pid == focused.target_pid &&
		contexts.front().target_epoch == focused.target_epoch &&
		contexts.front().process_creation_time_100ns == focused.process_creation_time_100ns &&
		contexts.front().scan_revision == focused.scan_revision &&
		contexts.front().workspace_id == focused.workspace_id &&
		contexts.front().workspace_generation == focused.workspace_generation &&
		contexts.front().owner_workspace_id == focused.owner_workspace_id &&
		contexts.front().owner_workspace_generation == focused.owner_workspace_generation &&
		contexts.front().document_id == focused.document_id;
	if (!compatible || std::none_of(contexts.begin(), contexts.end(), [&](const auto& item) {
		return memory_context_identity_equal(item, focused);
	}))
		contexts.assign(1, focused);
	return contexts;
}

std::uint64_t memory_action_set_hash(
	const std::vector<memory_interaction::context_t>& contexts) noexcept {
	std::uint64_t hash = 1469598103934665603ULL;
	const auto mix = [&](std::uint64_t value) {
		for (unsigned shift = 0; shift < 64; shift += 8) {
			hash ^= static_cast<std::uint8_t>(value >> shift);
			hash *= 1099511628211ULL;
		}
	};
	for (const auto& context : contexts) {
		mix(static_cast<std::uint64_t>(context.kind));
		mix(static_cast<std::uint64_t>(context.source));
		mix(context.target_pid);
		mix(context.target_epoch);
		mix(context.process_creation_time_100ns);
		mix(context.scan_revision);
		mix(context.workspace_generation);
		mix(context.owner_workspace_generation);
		mix(context.document_id);
		mix(context.address);
		mix(context.extent);
		mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(context.index)));
		for (const char character : context.workspace_id)
			mix(static_cast<unsigned char>(character));
		for (const char character : context.owner_workspace_id)
			mix(static_cast<unsigned char>(character));
	}
	return hash;
}

std::string memory_action_entity_id(const memory_interaction::context_t& focused,
	const std::vector<memory_interaction::context_t>& contexts) {
	return std::to_string(focused.address) + ":" + std::to_string(focused.index) + ":" +
		std::to_string(contexts.size()) + ":" + std::to_string(memory_action_set_hash(contexts));
}

constexpr std::size_t kMaximumExactResultSelection = 4096;
constexpr std::size_t kMaximumExactResultBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumComparerValueBytes = 1024U * 1024U;
constexpr std::size_t kMaximumEvidenceExcerptBytes = 12U * 1024U;

struct exact_result_set_t {
	std::vector<memory_interaction::context_t> contexts;
	std::vector<memory_scanner::scan_result_t> results;
	memory_scanner::value_type_t value_type = memory_scanner::value_type_t::int32_val;
	memory_interaction::runtime_t runtime;
	std::size_t raw_bytes = 0;
};

bool capture_exact_result_set(
	const std::vector<memory_interaction::context_t>& contexts,
	exact_result_set_t& output, std::string& reason) {
	output = {};
	if (contexts.empty() || contexts.size() > kMaximumExactResultSelection) {
		reason = "Select between 1 and 4096 current memory results.";
		return false;
	}
	const auto first = contexts.front();
	if (first.kind != memory_interaction::kind_t::scan_result) {
		reason = "This action requires memory scan results.";
		return false;
	}
	const auto before = runtime_snapshot();
	for (const auto& context : contexts) {
		if (!memory_context_identity_equal(context, first) &&
			(context.kind != first.kind || context.source != first.source ||
			 context.target_pid != first.target_pid || context.target_epoch != first.target_epoch ||
			 context.process_creation_time_100ns != first.process_creation_time_100ns ||
			 context.scan_revision != first.scan_revision ||
			 context.workspace_id != first.workspace_id ||
			 context.workspace_generation != first.workspace_generation ||
			 context.owner_workspace_id != first.owner_workspace_id ||
			 context.owner_workspace_generation != first.owner_workspace_generation ||
			 context.document_id != first.document_id)) {
			reason = "The selected results do not share one target, scan, workspace, and document identity.";
			return false;
		}
		if (!memory_interaction::is_current(context, before)) {
			reason = "The target, scan publication, workspace, document, or selected result changed.";
			return false;
		}
	}
	auto& state = memory_scanner::g_state;
	{
		std::lock_guard<std::mutex> lock(state.results_mutex);
		output.runtime = runtime_snapshot();
		output.value_type = state.config.value_type;
		output.contexts = contexts;
		output.results.reserve(contexts.size());
		for (const auto& context : contexts) {
			if (context.scan_revision != output.runtime.scan_revision || context.index < 0 ||
				!state.results ||
				static_cast<std::size_t>(context.index) >= state.results->size()) {
				reason = "The selected result set no longer exists in the current scan publication.";
				return false;
			}
			const auto& result = (*state.results)[static_cast<std::size_t>(context.index)];
			const std::string current = memory_scanner::format_value(result.current_value,
				output.value_type);
			const std::string previous = memory_scanner::format_value(result.previous_value,
				output.value_type);
			if (result.address != context.address || current != context.value ||
				previous != context.previous_value) {
				reason = "A selected result changed after the context menu was opened.";
				return false;
			}
			const std::size_t added = result.current_value.size() + result.previous_value.size();
			if (added > kMaximumExactResultBytes ||
				output.raw_bytes > kMaximumExactResultBytes - added) {
				reason = "The exact selected result payload exceeds the 8 MiB safety limit.";
				return false;
			}
			output.raw_bytes += added;
			output.results.push_back(result);
		}
	}
	const auto after = runtime_snapshot();
	for (const auto& context : contexts) {
		if (!memory_interaction::is_current(context, after)) {
			reason = "The target, scan publication, workspace, or document changed while capturing results.";
			output = {};
			return false;
		}
	}
	reason.clear();
	return true;
}

std::string bytes_to_hex(const std::vector<std::uint8_t>& bytes) {
	static constexpr char digits[] = "0123456789ABCDEF";
	std::string output;
	output.resize(bytes.size() * 2U);
	for (std::size_t index = 0; index < bytes.size(); ++index) {
		output[index * 2U] = digits[bytes[index] >> 4U];
		output[index * 2U + 1U] = digits[bytes[index] & 0x0FU];
	}
	return output;
}

std::string exact_result_source_hint(const memory_interaction::context_t& context) {
	if (context.source == memory_interaction::source_t::live_process)
		return "memory:pid:" + std::to_string(context.target_pid) + ":created:" +
			std::to_string(context.process_creation_time_100ns) + ":epoch:" +
			std::to_string(context.target_epoch) + ":scan:" +
			std::to_string(context.scan_revision) + ":owner:" + context.owner_workspace_id +
			":" + std::to_string(context.owner_workspace_generation) + ":document:" +
			std::to_string(context.document_id);
	return "memory:workspace:" + context.workspace_id + ":generation:" +
		std::to_string(context.workspace_generation) + ":scan:" +
		std::to_string(context.scan_revision) + ":owner:" + context.owner_workspace_id +
		":" + std::to_string(context.owner_workspace_generation) + ":document:" +
		std::to_string(context.document_id);
}

bool build_exact_evidence_excerpt(const exact_result_set_t& snapshot,
	std::string& excerpt, std::string& reason) {
	const auto& first = snapshot.contexts.front();
	std::ostringstream output;
	output << "Source: " << (first.source == memory_interaction::source_t::live_process
		? "live process" : "static binary")
		<< "\nPID: " << first.target_pid
		<< "\nProcess creation: " << first.process_creation_time_100ns
		<< "\nTarget epoch: " << first.target_epoch
		<< "\nScan revision: " << first.scan_revision
		<< "\nWorkspace: " << first.workspace_id
		<< "\nWorkspace generation: " << first.workspace_generation
		<< "\nOwner workspace: " << first.owner_workspace_id
		<< "\nOwner generation: " << first.owner_workspace_generation
		<< "\nDocument: " << first.document_id
		<< "\nSelected rows: " << snapshot.contexts.size();
	for (std::size_t index = 0; index < snapshot.contexts.size(); ++index) {
		const auto& context = snapshot.contexts[index];
		output << "\n[" << index + 1U << "] 0x" << std::uppercase << std::hex
			<< std::setw(16) << std::setfill('0') << context.address << std::dec
			<< " | " << context.value << " | previous=" << context.previous_value
			<< " | " << context.module_offset;
		if (output.tellp() < 0 || static_cast<std::size_t>(output.tellp()) >
			kMaximumEvidenceExcerptBytes) {
			reason = "The complete selected result evidence exceeds the 12 KiB chat limit; export it instead.";
			return false;
		}
	}
	excerpt = output.str();
	if (excerpt.empty() || excerpt.size() > kMaximumEvidenceExcerptBytes) {
		reason = "The complete selected result evidence exceeds the 12 KiB chat limit; export it instead.";
		return false;
	}
	reason.clear();
	return true;
}

bool exact_scan_publication_matches(const exact_result_set_t& snapshot) {
	if (snapshot.contexts.empty() || snapshot.results.size() != snapshot.contexts.size())
		return false;
	auto& state = memory_scanner::g_state;
	std::lock_guard<std::mutex> lock(state.results_mutex);
	const auto runtime = runtime_snapshot();
	const auto& first = snapshot.contexts.front();
	if (runtime.scan_revision != first.scan_revision ||
		runtime.scan_static_binary != (first.source == memory_interaction::source_t::static_binary))
		return false;
	if (first.source == memory_interaction::source_t::live_process) {
		if (!runtime.live_attached || runtime.target_pid != first.target_pid ||
			runtime.target_epoch != first.target_epoch ||
			runtime.process_creation_time_100ns != first.process_creation_time_100ns ||
			runtime.scan_target_pid != first.target_pid ||
			runtime.scan_target_epoch != first.target_epoch ||
			runtime.scan_process_creation_time_100ns != first.process_creation_time_100ns)
			return false;
	} else if (!runtime.static_loaded || runtime.scan_workspace_id != first.workspace_id ||
		runtime.scan_workspace_generation != first.workspace_generation) {
		return false;
}
	if (!state.results)
		return false;
	for (std::size_t index = 0; index < snapshot.contexts.size(); ++index) {
		const auto& context = snapshot.contexts[index];
		if (context.index < 0 || static_cast<std::size_t>(context.index) >= state.results->size())
			return false;
		const auto& current = (*state.results)[static_cast<std::size_t>(context.index)];
		const auto& captured = snapshot.results[index];
		if (current.address != context.address || current.address != captured.address ||
			current.current_value != captured.current_value ||
			current.previous_value != captured.previous_value ||
			current.module_name != captured.module_name ||
			current.module_offset != captured.module_offset)
			return false;
	}
	return true;
}

capability_state_t exact_result_action_capability(
	const std::vector<memory_interaction::context_t>& contexts,
	bool require_pair, bool require_chat_fit = false) {
	exact_result_set_t snapshot;
	std::string reason;
	if (!capture_exact_result_set(contexts, snapshot, reason))
		return capability_state_t::unavailable(reason);
	if (require_pair && snapshot.results.size() != 2U)
		return capability_state_t::unavailable(
			"Select exactly two current scan results to compare.");
	if (require_pair && (snapshot.results[0].current_value.empty() ||
		snapshot.results[1].current_value.empty()))
		return capability_state_t::unavailable(
			"Both selected results must contain current bytes.");
	if (require_pair && (snapshot.results[0].current_value.size() > kMaximumComparerValueBytes ||
		snapshot.results[1].current_value.size() > kMaximumComparerValueBytes))
		return capability_state_t::unavailable(
			"A selected value exceeds the 1 MiB comparer slot limit.");
	if (require_chat_fit) {
		std::string excerpt;
		if (!build_exact_evidence_excerpt(snapshot, excerpt, reason))
			return capability_state_t::unavailable(reason);
	}
	return capability_state_t::available();
}

action_handler_result_t compare_exact_results(
	const std::vector<memory_interaction::context_t>& contexts) {
	exact_result_set_t snapshot;
	std::string reason;
	if (!capture_exact_result_set(contexts, snapshot, reason))
		return action_handler_result_t::failed(reason);
	if (snapshot.results.size() != 2U)
		return action_handler_result_t::failed(
			"Select exactly two current scan results to compare.");
	for (const auto& result : snapshot.results) {
		if (result.current_value.empty() || result.current_value.size() > kMaximumComparerValueBytes)
			return action_handler_result_t::failed(
				"Each comparer value must contain between 1 byte and 1 MiB.");
	}
	for (std::size_t index = 0; index < snapshot.results.size(); ++index) {
		char label[64]{};
		std::snprintf(label, sizeof(label), "Memory 0x%016llX current",
			static_cast<unsigned long long>(snapshot.results[index].address));
		if (aida::burp::comparer::add_slot_from_bytes(label,
			snapshot.results[index].current_value,
			exact_result_source_hint(snapshot.contexts[index])) == 0)
			return action_handler_result_t::failed(
				aida::burp::comparer::last_error().empty()
					? "The comparer rejected a selected memory value."
					: aida::burp::comparer::last_error());
	}
	ScannerController::instance().open_or_focus_view("view.network.comparer");
	return action_handler_result_t::completed();
}

bool serialize_exact_results(const exact_result_set_t& snapshot,
	std::string& payload, std::string& reason) {
	try {
		const auto& first = snapshot.contexts.front();
		nlohmann::json root;
		root["schema"] = "aida.memory.selected-results.v1";
		root["source"] = first.source == memory_interaction::source_t::live_process
			? "live_process" : "static_binary";
		root["target_pid"] = first.target_pid;
		root["target_epoch"] = first.target_epoch;
		root["process_creation_time_100ns"] = first.process_creation_time_100ns;
		root["scan_revision"] = first.scan_revision;
		root["scan_workspace_id"] = first.workspace_id;
		root["scan_workspace_generation"] = first.workspace_generation;
		root["owner_workspace_id"] = first.owner_workspace_id;
		root["owner_workspace_generation"] = first.owner_workspace_generation;
		root["document_id"] = first.document_id;
		root["value_type"] = memory_scanner::value_type_name(snapshot.value_type);
		root["selected_count"] = snapshot.results.size();
		root["results"] = nlohmann::json::array();
		for (std::size_t index = 0; index < snapshot.results.size(); ++index) {
			const auto& context = snapshot.contexts[index];
			const auto& result = snapshot.results[index];
			root["results"].push_back({
				{"selection_index", context.index},
				{"address", result.address},
				{"address_hex", [&] { char value[24]{}; std::snprintf(value, sizeof(value),
					"0x%016llX", static_cast<unsigned long long>(result.address)); return std::string(value); }()},
				{"current_display", context.value},
				{"previous_display", context.previous_value},
				{"current_bytes_hex", bytes_to_hex(result.current_value)},
				{"previous_bytes_hex", bytes_to_hex(result.previous_value)},
				{"module_name", result.module_name},
				{"module_offset", result.module_offset},
				{"module_offset_display", context.module_offset}
			});
		}
		payload = root.dump(2);
	} catch (const std::exception& error) {
		reason = std::string("Exact memory result serialization failed: ") + error.what();
		return false;
	}
	if (payload.empty() || payload.size() > kMaximumExactResultBytes) {
		reason = "The serialized exact result set exceeds the 8 MiB export limit.";
		payload.clear();
		return false;
	}
	reason.clear();
	return true;
}

action_handler_result_t export_exact_results(
	const std::vector<memory_interaction::context_t>& contexts, QWidget* parent) {
	exact_result_set_t snapshot;
	std::string reason;
	if (!capture_exact_result_set(contexts, snapshot, reason))
		return action_handler_result_t::failed(reason);
	std::string payload;
	if (!serialize_exact_results(snapshot, payload, reason))
		return action_handler_result_t::failed(reason);
	const QString picked = QFileDialog::getSaveFileName(parent,
		QStringLiteral("Export selected memory results"),
		QStringLiteral("aida-memory-selected-results.json"),
		QStringLiteral("JSON (*.json);;All files (*.*)"));
	if (picked.isEmpty())
		return action_handler_result_t::completed();
	if (!capture_exact_result_set(contexts, snapshot, reason) ||
		!serialize_exact_results(snapshot, payload, reason))
		return action_handler_result_t::failed(reason);
	auto cancellation = std::make_shared<std::atomic<bool>>(false);
	auto commit_gate = std::make_shared<std::atomic<std::uint8_t>>(
		scanner_async_io::operation_reversible);
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "scanner.memory";
	submission.label = "scanner.memory.export_selected";
	submission.thread_class = "bounded_task";
	submission.domain = aida::infra::executor::domain_t::feature_worker;
	submission.priority = 3;
	submission.target_pid = snapshot.contexts.front().target_pid;
	submission.generation = snapshot.contexts.front().scan_revision;
	submission.cancel_hook = [cancellation, commit_gate] {
		std::uint8_t expected = scanner_async_io::operation_reversible;
		if (commit_gate->compare_exchange_strong(expected,
			scanner_async_io::operation_cancelled, std::memory_order_acq_rel,
			std::memory_order_acquire))
			cancellation->store(true, std::memory_order_release);
	};
	submission.body = [snapshot = std::move(snapshot), payload = std::move(payload),
		destination = picked.toStdString(), cancellation, commit_gate]() mutable {
		const auto result = scanner_async_io::atomic_replace(destination, payload, false,
			cancellation, [&snapshot] { return exact_scan_publication_matches(snapshot); },
			commit_gate);
		if (!result.success)
			throw std::runtime_error(result.error.empty()
				? "Selected memory result export did not commit." : result.error);
	};
	const auto submitted = aida::infra::executor::submit(std::move(submission));
	if (!submitted.submitted)
		return action_handler_result_t::failed(
			submitted.reject_reason.empty() ? "The memory export worker rejected the operation."
				: submitted.reject_reason);
	scanner_task_center::register_executor_task(submitted, "view.memory.value_scan",
		"memory.result.export_selected", "Export selected memory results",
		contexts.front().target_pid, true, [cancellation, commit_gate] {
			std::uint8_t expected = scanner_async_io::operation_reversible;
			if (commit_gate->compare_exchange_strong(expected,
				scanner_async_io::operation_cancelled, std::memory_order_acq_rel,
				std::memory_order_acquire))
				cancellation->store(true, std::memory_order_release);
			return true;
		});
	return action_handler_result_t::completed();
}

bool memory_action_is_explicit_batch(memory_interaction::capability_t capability) noexcept {
	switch (capability) {
		case memory_interaction::capability_t::copy_address:
		case memory_interaction::capability_t::copy_value:
		case memory_interaction::capability_t::copy_previous_value:
		case memory_interaction::capability_t::copy_module_offset:
		case memory_interaction::capability_t::add_to_address_list:
		case memory_interaction::capability_t::remove:
		case memory_interaction::capability_t::compare_selected:
		case memory_interaction::capability_t::export_selected:
			return true;
		default:
			return false;
	}
}

void copy_memory_addresses(const std::vector<memory_interaction::context_t>& contexts) {
	std::ostringstream output;
	output << std::uppercase << std::hex << std::setfill('0');
	for (std::size_t index = 0; index < contexts.size(); ++index) {
		if (index != 0) output << '\n';
		output << "0x" << std::setw(16) << contexts[index].address;
	}
	clipboard::set_text(QString::fromStdString(output.str()));
}

template <typename Accessor>
void copy_memory_text(const std::vector<memory_interaction::context_t>& contexts,
	Accessor&& accessor) {
	std::string output;
	for (const auto& context : contexts) {
		const auto& value = accessor(context);
		if (value.empty()) continue;
		if (!output.empty()) output.push_back('\n');
		output.append(value);
	}
	clipboard::set_text(QString::fromStdString(output));
}

int find_address_index_impl(const memory_interaction::context_t& context) {
	auto& state = memory_scanner::g_state;
	std::lock_guard<std::mutex> lock(state.address_mutex);
	for (std::size_t index = 0; index < state.address_list.size(); ++index) {
		const auto& entry = state.address_list[index];
		if (entry.address == context.address && entry.target_pid == context.target_pid &&
			entry.target_epoch == context.target_epoch &&
			entry.target_identity.process.creation_time_100ns ==
				context.process_creation_time_100ns)
			return static_cast<int>(index);
	}
	return -1;
}

void open_hex_at(std::uint64_t address) {
	if (address == 0)
		return;
	const auto context = disasm_view::capture_selected_workspace();
	if (hex_view::request_live_memory(context, address, 256))
		ScannerController::instance().open_or_focus_view("document.hex");
}

void open_disassembly_at(std::uint64_t address) {
	if (address == 0)
		return;
	ScannerController::instance().open_or_focus_view("document.disassembly");
	disasm_view::goto_address(address, disasm_view::capture_selected_workspace());
}

aida::ui::application_ui::retained_entity_context_t make_memory_actions(
	const char* owner, const QString& owner_view_id,
	const memory_interaction::context_t& context,
	const memory_interaction::runtime_t& runtime,
	std::initializer_list<std::pair<const char*, memory_interaction::capability_t>> actions,
	QWidget* menu_parent) {
	aida::ui::application_ui::retained_entity_context_t retained;
	const auto action_contexts = memory_action_contexts(context);
	const bool multiple = action_contexts.size() > 1;
	retained.owner_id = owner;
	retained.entity_id = memory_action_entity_id(context, action_contexts);
	retained.entity_generation = context.scan_revision;
	retained.active_view = aida::ui::stable_view_id_t(owner_view_id.toStdString());
	const auto workspace_generation = context.workspace_generation;
	const std::string static_workspace_id = context.workspace_id;
	retained.validate_identity = [action_contexts, workspace_generation, static_workspace_id]() {
		if (!action_contexts.empty() &&
			action_contexts.front().kind == memory_interaction::kind_t::scan_result) {
			exact_result_set_t exact;
			std::string reason;
			if (!capture_exact_result_set(action_contexts, exact, reason))
				return capability_state_t::unavailable(reason);
			return capability_state_t::available();
		}
		const auto current_runtime = runtime_snapshot();
		for (const auto& item : action_contexts) {
			if (!memory_interaction::is_current(item, current_runtime))
				return capability_state_t::unavailable(
					"The target, scan publication, or selected memory entity changed.");
		}
		if (!action_contexts.empty() &&
			action_contexts.front().source == memory_interaction::source_t::static_binary) {
			const auto current_workspace = disasm_view::capture_selected_workspace();
			if (!current_workspace.workspace ||
				current_workspace.workspace->generation() != workspace_generation ||
				current_workspace.workspace->identity().binary_id().to_hex() != static_workspace_id)
				return capability_state_t::unavailable(
					"The static analysis workspace changed; select the memory entity again.");
		}
		return capability_state_t::available();
	};
	const bool result_owner = std::strcmp(owner, "memory.value_scan.result") == 0;
	const bool address_owner = std::strcmp(owner, "memory.value_scan.address") == 0;
	for (const auto& [id, capability] : actions) {
		capability_state_t state;
		if (capability == memory_interaction::capability_t::compare_selected) {
			state = exact_result_action_capability(action_contexts, true);
		} else if (capability == memory_interaction::capability_t::export_selected) {
			state = exact_result_action_capability(action_contexts, false);
		} else if (multiple && !memory_action_is_explicit_batch(capability)) {
			state = capability_state_t::unavailable(
				"This action requires exactly one memory row.");
		} else {
			const bool copy_batch = capability == memory_interaction::capability_t::copy_address ||
				capability == memory_interaction::capability_t::copy_value ||
				capability == memory_interaction::capability_t::copy_previous_value ||
				capability == memory_interaction::capability_t::copy_module_offset;
			bool enabled = !copy_batch;
			const char* reason = nullptr;
			for (const auto& item : action_contexts) {
				const auto evaluated = memory_interaction::evaluate(capability, item, runtime);
				if (copy_batch) {
					enabled = enabled || evaluated.enabled;
					if (!reason && !evaluated.enabled) reason = evaluated.disabled_reason;
				} else if (!evaluated.enabled) {
					enabled = false;
					reason = evaluated.disabled_reason;
					break;
				}
			}
			state = enabled ? capability_state_t::available()
				: capability_state_t::unavailable(
					reason ? reason : "The memory action is unavailable.");
		}
		const std::string action_id = id;
		if (capability == memory_interaction::capability_t::compare_selected) {
			retained.actions.push_back({id, std::move(state), [action_contexts] {
				return compare_exact_results(action_contexts);
			}});
		} else if (capability == memory_interaction::capability_t::export_selected) {
			retained.actions.push_back({id, std::move(state), [action_contexts, menu_parent] {
				return export_exact_results(action_contexts, menu_parent);
			}});
		} else if (action_id == "memory.result.add_address") {
			retained.actions.push_back({id, std::move(state),
				[action_contexts, menu_parent]() {
					std::vector<std::uint64_t> addresses;
					addresses.reserve(action_contexts.size());
					for (const auto& item : action_contexts) addresses.push_back(item.address);
					auto& state = memory_scanner::g_state;
					memory_scanner::value_type_t value_type;
					{
						std::lock_guard<std::mutex> lock(state.results_mutex);
						value_type = state.config.value_type;
					}
					if (addresses.size() == 1) {
						ScannerController::instance().open_add_dialog(addresses[0],
							static_cast<int>(value_type), menu_parent);
					} else {
						for (uint64_t address : addresses)
							memory_scanner::add_address(address, "", value_type);
						chrome::toast_success(QStringLiteral("Added %1 addresses.")
							.arg(addresses.size()), 2.5);
						ScannerController::instance().refresh_from_engine();
					}
					diag::log_tagged("scan_audit",
						"[scan_audit] memory_scanner ctx add_to_list");
					return action_handler_result_t::completed();
				}});
		} else if (action_id == "memory.entity.open_hex") {
			retained.actions.push_back({id, std::move(state), [context]() {
				menu_diag_logf("ctx open_hex addr=0x%llX",
					static_cast<unsigned long long>(context.address));
				open_hex_at(context.address);
				diag::log_tagged("scan_audit", "[scan_audit] memory_scanner ctx open_hex");
				return action_handler_result_t::completed();
			}});
		} else if (action_id == "memory.entity.open_disassembly") {
			retained.actions.push_back({id, std::move(state), [context]() {
				menu_diag_logf("ctx open_disasm addr=0x%llX",
					static_cast<unsigned long long>(context.address));
				open_disassembly_at(context.address);
				diag::log_tagged("scan_audit", "[scan_audit] memory_scanner ctx open_disasm");
				return action_handler_result_t::completed();
			}});
		} else if (action_id == "memory.entity.copy_address") {
			retained.actions.push_back({id, std::move(state), [action_contexts]() {
				copy_memory_addresses(action_contexts);
				menu_diag_log("ctx copy_address");
				diag::log_tagged("scan_audit", "[scan_audit] memory_scanner ctx copy_address");
				return action_handler_result_t::completed();
			}});
		} else if (action_id == "memory.entity.copy_value") {
			retained.actions.push_back({id, std::move(state), [action_contexts]() {
				copy_memory_text(action_contexts, [](const auto& item) -> const std::string& {
					return item.value;
				});
				return action_handler_result_t::completed();
			}});
		} else if (action_id == "memory.entity.copy_previous") {
			retained.actions.push_back({id, std::move(state), [action_contexts]() {
				copy_memory_text(action_contexts, [](const auto& item) -> const std::string& {
					return item.previous_value;
				});
				return action_handler_result_t::completed();
			}});
		} else if (action_id == "memory.entity.copy_module_offset") {
			retained.actions.push_back({id, std::move(state), [action_contexts]() {
				copy_memory_text(action_contexts, [](const auto& item) -> const std::string& {
					return item.module_offset;
				});
				return action_handler_result_t::completed();
			}});
		} else if (action_id == "memory.entity.stage_patch") {
			retained.actions.push_back({id, std::move(state), [context, result_owner]() {
				std::uint64_t extent = 1;
				if (result_owner) {
					auto& state = memory_scanner::g_state;
					std::lock_guard<std::mutex> lock(state.results_mutex);
					extent = static_cast<std::uint64_t>(
						memory_scanner::value_type_size(state.config.value_type));
				} else {
					const int row = find_address_index_impl(context);
					auto& state = memory_scanner::g_state;
					std::lock_guard<std::mutex> lock(state.address_mutex);
					if (row >= 0 && row < static_cast<int>(state.address_list.size()))
						extent = static_cast<std::uint64_t>(memory_scanner::value_type_size(
							state.address_list[static_cast<std::size_t>(row)].value_type));
				}
				std::string error;
				if (debugger_view::stage_patch_review(context.address, extent,
					result_owner ? "Staged from Value Scan result" :
						"Staged from Memory Address List", &error)) {
					ScannerController::instance().open_or_focus_view("view.debug.patches");
					return action_handler_result_t::completed();
				}
				return action_handler_result_t::failed(error.empty()
					? "The patch review could not be staged." : error);
			}});
		} else if (action_id == "memory.address.edit_description") {
			retained.actions.push_back({id, std::move(state), [context, menu_parent]() {
				const int row = find_address_index_impl(context);
				menu_diag_logf("ctx_addr edit_description row=%d", row);
				if (row >= 0)
					ScannerController::instance().open_edit_description_dialog(row, menu_parent);
				return action_handler_result_t::completed();
			}});
		} else if (action_id == "memory.address.change_type") {
			retained.actions.push_back({id, std::move(state), [context, menu_parent]() {
				const int row = find_address_index_impl(context);
				menu_diag_logf("ctx_addr change_type row=%d", row);
				if (row >= 0)
					ScannerController::instance().open_change_type_dialog(row, menu_parent);
				return action_handler_result_t::completed();
			}});
		} else if (action_id == "memory.address.change_value") {
			retained.actions.push_back({id, std::move(state), [context, menu_parent]() {
				const int row = find_address_index_impl(context);
				menu_diag_logf("ctx_addr change_value row=%d", row);
				if (row >= 0)
					ScannerController::instance().open_change_value_dialog(row, menu_parent);
				return action_handler_result_t::completed();
			}});
		} else if (action_id == "memory.address.remove") {
			retained.actions.push_back({id, std::move(state), [action_contexts]() {
				std::vector<int> rows;
				rows.reserve(action_contexts.size());
				for (const auto& item : action_contexts) {
					const int row = find_address_index_impl(item);
					if (row >= 0) rows.push_back(row);
				}
				ScannerController::instance().remove_addresses(rows);
				menu_diag_logf("ctx_addr remove multi=%zu", rows.size());
				return action_handler_result_t::completed();
			}});
		} else {
			retained.actions.push_back({id, std::move(state),
				[]() { return action_handler_result_t::completed(); }});
		}
	}
	if (address_owner && action_contexts.size() == 1U) {
		std::optional<memory_scanner::address_entry_t> retained_entry;
		const int retained_index = context.index;
		{
			std::lock_guard<std::mutex> lock(memory_scanner::g_state.address_mutex);
			if (retained_index >= 0 &&
				retained_index < static_cast<int>(memory_scanner::g_state.address_list.size())) {
				const auto& entry = memory_scanner::g_state.address_list[
					static_cast<std::size_t>(retained_index)];
				if (entry.address == context.address && entry.target_pid == context.target_pid &&
					entry.target_epoch == context.target_epoch &&
					entry.target_identity.process.creation_time_100ns ==
						context.process_creation_time_100ns && entry.frozen == context.frozen)
					retained_entry = entry;
			}
		}
		for (auto& action : retained.actions) {
			const bool unfreeze = action.action_id == "memory.address.unfreeze";
			if (!unfreeze && action.action_id != "memory.address.freeze")
				continue;
			if (!retained_entry)
				action.capability = capability_state_t::unavailable(
					"The exact Address List entry changed before the context menu opened.");
			else if (!unfreeze && retained_entry->last_value.empty())
				action.capability = capability_state_t::unavailable(
					"Freeze requires a current captured value for the retained Address List entry.");
			action.invoke = [context, retained_entry, retained_index, unfreeze] {
				if (!retained_entry || retained_index < 0)
					return action_handler_result_t::failed(
						"The retained Address List entry is unavailable.");
				const auto current_runtime = runtime_snapshot();
				if (!memory_interaction::is_current(context, current_runtime) ||
					!memory_scanner::validate_target_binding(context.target_pid,
						context.target_epoch, context.process_creation_time_100ns))
					return action_handler_result_t::failed(
						"The retained Address List entry or target identity changed.");
				if (!memory_scanner::freeze_address_exact(
						static_cast<std::size_t>(retained_index), !unfreeze, *retained_entry))
					return action_handler_result_t::failed(
						"The exact freeze-state transition was rejected or could not be verified.");
				ScannerController::instance().refresh_from_engine();
				return action_handler_result_t::completed();
			};
		}
	}
	const bool direct_memory_row = result_owner || address_owner;
	if (direct_memory_row) {
		const bool single = action_contexts.size() == 1U;
		const auto exact_gate = [&]() {
			if (!single)
				return capability_state_t::unavailable(
					"This workflow requires exactly one retained memory row.");
			const auto current_runtime = runtime_snapshot();
			if (!memory_interaction::is_current(context, current_runtime))
				return capability_state_t::unavailable(
					"The target, scan publication, workspace, or retained memory row changed.");
			return capability_state_t::available();
		};
		const auto pointer_gate = exact_gate();
		const bool pointer_live = context.source == memory_interaction::source_t::live_process;
		retained.actions.push_back({"memory.entity.pointer_workflow",
			pointer_gate.enabled && pointer_live
				? capability_state_t::available()
				: capability_state_t::unavailable(pointer_gate.enabled
					? "Pointer scanning requires a retained live-process row."
					: pointer_gate.disabled_reason),
			[context, owner_view_id] {
				ScannerController::instance().open_or_focus_view("view.memory.pointers");
				const auto current_runtime = runtime_snapshot();
				if (!memory_interaction::is_current(context, current_runtime))
					return action_handler_result_t::failed(
						"The retained memory row or target identity changed.");
				pointer_scanner::staged_target_context_t staged;
				staged.address = context.address;
				staged.target_pid = context.target_pid;
				staged.target_epoch = context.target_epoch;
				staged.process_creation_time_100ns = context.process_creation_time_100ns;
				staged.source_generation = context.kind == memory_interaction::kind_t::scan_result
					? context.scan_revision : context.target_epoch;
				staged.source_view = owner_view_id.toStdString();
				staged.source_identity = memory_action_entity_id(context, {context});
				staged.validate = [context](std::string& reason) {
					const auto current = runtime_snapshot();
					const bool valid = memory_interaction::is_current(context, current) &&
						memory_scanner::validate_target_binding(context.target_pid,
							context.target_epoch, context.process_creation_time_100ns);
					if (!valid) reason = "The retained memory row, target epoch, or process identity changed.";
					return valid;
				};
				std::string error;
				if (!pointer_scanner::stage_target_context(std::move(staged), error))
					return action_handler_result_t::failed(error);
				return action_handler_result_t::completed();
			}});
		const auto structure_gate = exact_gate();
		retained.actions.push_back({"memory.entity.interpret_structure", structure_gate,
			[context, owner_view_id] {
				ScannerController::instance().open_or_focus_view("view.types.structures");
				const auto current_runtime = runtime_snapshot();
				if (!memory_interaction::is_current(context, current_runtime))
					return action_handler_result_t::failed(
						"The retained memory row or source identity changed.");
				analysis::staged_dissector_target_t staged;
				staged.address = context.address;
				staged.target_pid = context.target_pid;
				staged.target_epoch = context.target_epoch;
				staged.process_creation_time_100ns = context.process_creation_time_100ns;
				staged.source_generation = context.kind == memory_interaction::kind_t::scan_result
					? context.scan_revision : context.source == memory_interaction::source_t::live_process
					? context.target_epoch : context.workspace_generation;
				staged.live_process = context.source == memory_interaction::source_t::live_process;
				staged.source_view = owner_view_id.toStdString();
				staged.source_identity = memory_action_entity_id(context, {context});
				staged.validate = [context](std::string& reason) {
					const auto current = runtime_snapshot();
					if (!memory_interaction::is_current(context, current)) {
						reason = "The retained memory row, scan, target, or workspace identity changed.";
						return false;
					}
					if (context.source == memory_interaction::source_t::live_process &&
						!memory_scanner::validate_target_binding(context.target_pid,
							context.target_epoch, context.process_creation_time_100ns)) {
						reason = "The retained live-process identity changed.";
						return false;
					}
					return true;
				};
				auto& hooks = analysis::analysis_host_hooks();
				if (!hooks.stage_dissector_target)
					return action_handler_result_t::failed(
						"The structure dissector is unavailable.");
				std::string error;
				if (!hooks.stage_dissector_target(std::move(staged), error))
					return action_handler_result_t::failed(error);
				return action_handler_result_t::completed();
			}});
		if (result_owner) {
			std::optional<memory_scanner::address_entry_t> retained_entry;
			int retained_index = -1;
			if (single && context.source == memory_interaction::source_t::live_process) {
				std::lock_guard<std::mutex> lock(memory_scanner::g_state.address_mutex);
				for (std::size_t index = 0; index < memory_scanner::g_state.address_list.size(); ++index) {
					const auto& entry = memory_scanner::g_state.address_list[index];
					if (entry.address == context.address && entry.target_pid == context.target_pid &&
						entry.target_epoch == context.target_epoch &&
						entry.target_identity.process.creation_time_100ns ==
							context.process_creation_time_100ns) {
						retained_entry = entry;
						retained_index = static_cast<int>(index);
						break;
					}
				}
			}
			const bool unfreeze = retained_entry && retained_entry->frozen;
			const char* action_id = unfreeze ? "memory.result.unfreeze" : "memory.result.freeze";
			const auto freeze_state = !single
				? capability_state_t::unavailable(
					"Freeze requires exactly one retained scan result.")
				: context.source != memory_interaction::source_t::live_process
				? capability_state_t::unavailable(
					"Static scan results cannot be frozen.")
				: !retained_entry
				? capability_state_t::unavailable(
					"Add this result to the Address List before freezing it.")
				: capability_state_t::available();
			retained.actions.push_back({action_id, freeze_state,
				[context, retained_entry, retained_index, unfreeze] {
					if (!retained_entry || retained_index < 0)
						return action_handler_result_t::failed(
							"The retained Address List entry is unavailable.");
					const auto runtime = runtime_snapshot();
					if (!memory_interaction::is_current(context, runtime) ||
						!memory_scanner::validate_target_binding(context.target_pid,
							context.target_epoch, context.process_creation_time_100ns))
						return action_handler_result_t::failed(
							"The retained scan result or target identity changed.");
					int current_index = -1;
					{
						std::lock_guard<std::mutex> lock(memory_scanner::g_state.address_mutex);
						for (std::size_t index = 0; index < memory_scanner::g_state.address_list.size(); ++index) {
							const auto& entry = memory_scanner::g_state.address_list[index];
							if (entry.address == retained_entry->address &&
								entry.value_type == retained_entry->value_type &&
								entry.target_pid == retained_entry->target_pid &&
								entry.target_epoch == retained_entry->target_epoch &&
								entry.target_identity.process.creation_time_100ns ==
									retained_entry->target_identity.process.creation_time_100ns &&
								entry.description == retained_entry->description &&
								entry.last_value == retained_entry->last_value &&
								entry.frozen == retained_entry->frozen) {
								current_index = static_cast<int>(index);
								break;
							}
						}
					}
					if (current_index < 0)
						return action_handler_result_t::failed(
							"The exact Address List entry changed after the context menu opened.");
					if (!memory_scanner::freeze_address_exact(
							static_cast<std::size_t>(current_index), !unfreeze, *retained_entry))
						return action_handler_result_t::failed(
							"The exact freeze-state transition was rejected or could not be verified.");
					ScannerController::instance().refresh_from_engine();
					return action_handler_result_t::completed();
				}});
		}
	}
	const char* source_kind = context.kind == memory_interaction::kind_t::scan_result
		? "memory_scan_result" : context.kind == memory_interaction::kind_t::address_entry
		? "memory_address_entry" : "memory_entity";
	char address[24]{};
	std::snprintf(address, sizeof(address), "0x%016llX",
		static_cast<unsigned long long>(context.address));
	aida::automation_ui::entity_evidence::snapshot_t evidence;
	evidence.workspace_id = context.source == memory_interaction::source_t::live_process
		? "pid:" + std::to_string(context.target_pid) + ":created:" +
			std::to_string(context.process_creation_time_100ns) : static_workspace_id;
	evidence.source_view_id = "view.memory.value_scan";
	evidence.source_kind = source_kind;
	evidence.entity_id = retained.entity_id;
	evidence.display_label = multiple ? std::to_string(action_contexts.size()) +
		" selected memory rows" : std::string(source_kind) + " " + address;
	capability_state_t evidence_capability;
	if (context.kind == memory_interaction::kind_t::scan_result) {
		exact_result_set_t exact;
		std::string evidence_reason;
		if (!capture_exact_result_set(action_contexts, exact, evidence_reason) ||
			!build_exact_evidence_excerpt(exact, evidence.excerpt, evidence_reason))
			evidence_capability = capability_state_t::unavailable(evidence_reason);
		else
			evidence_capability = capability_state_t::available();
	} else {
		evidence.excerpt = "Source: " + std::string(context.source == memory_interaction::source_t::live_process
			? "live process" : "static binary") + "\nPID: " + std::to_string(context.target_pid) +
			"\nProcess creation: " + std::to_string(context.process_creation_time_100ns) +
			"\nTarget epoch: " + std::to_string(context.target_epoch) +
			"\nScan revision: " + std::to_string(context.scan_revision) +
			"\nSelected rows: " + std::to_string(action_contexts.size());
		for (std::size_t index = 0; index < action_contexts.size(); ++index) {
			const auto& item = action_contexts[index];
			char item_address[24]{};
			std::snprintf(item_address, sizeof(item_address), "0x%016llX",
				static_cast<unsigned long long>(item.address));
			evidence.excerpt += "\n[" + std::to_string(index + 1) + "] " + item_address +
				" | " + item.value + " | previous=" + item.previous_value +
				" | " + item.module_offset;
		}
		evidence_capability = context.kind != memory_interaction::kind_t::none && context.address != 0
			? capability_state_t::available()
			: capability_state_t::unavailable(
				"A current retained memory entity with a resolved address is required for evidence handoff.");
	}
	evidence.address = context.address;
	evidence.revision = context.scan_revision;
	evidence.generation = context.scan_revision;
	evidence.sensitive = context.source == memory_interaction::source_t::live_process;
	evidence.return_to_source = [context, action_contexts, workspace_generation,
		static_workspace_id, owner_view_id](std::string& reason) {
		const auto current_runtime = runtime_snapshot();
		for (const auto& item : action_contexts) {
			if (!memory_interaction::is_current(item, current_runtime)) {
				reason = "The target, scan publication, or retained memory entity changed; capture it again.";
				return false;
			}
		}
		if (context.source == memory_interaction::source_t::static_binary) {
			const auto current_workspace = disasm_view::capture_selected_workspace();
			if (!current_workspace.workspace ||
				current_workspace.workspace->generation() != workspace_generation ||
				current_workspace.workspace->identity().binary_id().to_hex() != static_workspace_id) {
				reason = "The static analysis workspace changed; capture the memory entity again.";
				return false;
			}
		}
		memory_interaction::select_set(action_contexts, context);
		ScannerController::instance().open_or_focus_view("view.memory.value_scan");
		reason.clear();
		return true;
	};
	aida::automation_ui::entity_evidence::append_actions(retained, std::move(evidence),
		std::move(evidence_capability));
	return retained;
}

}

int find_address_index(const memory_interaction::context_t& context) {
	return find_address_index_impl(context);
}

bool forward_table_menu_key(QObject* watched, QEvent* event, QTableView* view,
	const std::function<void(const QPoint& global_pos, int row, int origin)>& handler) {
	if (!view || watched != view || !handler || !event ||
		event->type() != QEvent::KeyPress)
		return false;
	auto* key = static_cast<QKeyEvent*>(event);
	const bool menu_key = key->key() == Qt::Key_Menu;
	const bool shift_f10 = key->key() == Qt::Key_F10 &&
		(key->modifiers() & Qt::ShiftModifier);
	if (!menu_key && !shift_f10)
		return false;
	const QModelIndex current = view->currentIndex();
	if (!current.isValid())
		return false;
	const QRect cell = view->visualRect(current);
	handler(view->viewport()->mapToGlobal(QPoint(cell.center().x(), cell.bottom())),
		current.row(), menu_key ? 1 : 2);
	return true;
}

void show_result_context_menu(
	const memory_interaction::context_t& focused,
	const memory_interaction::runtime_t& runtime,
	const QString& owner_view_id,
	aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos,
	QWidget* parent) {
	auto retained = make_memory_actions("memory.value_scan.result", owner_view_id,
		focused, runtime, {
			{"memory.result.add_address", memory_interaction::capability_t::add_to_address_list},
			{"memory.result.compare_selected", memory_interaction::capability_t::compare_selected},
			{"memory.result.export_selected", memory_interaction::capability_t::export_selected},
			{"memory.entity.open_hex", memory_interaction::capability_t::open_hex},
			{"memory.entity.open_disassembly", memory_interaction::capability_t::open_disassembly},
			{"memory.entity.copy_address", memory_interaction::capability_t::copy_address},
			{"memory.entity.copy_value", memory_interaction::capability_t::copy_value},
			{"memory.entity.copy_previous", memory_interaction::capability_t::copy_previous_value},
			{"memory.entity.copy_module_offset", memory_interaction::capability_t::copy_module_offset},
			{"memory.entity.stage_patch", memory_interaction::capability_t::stage_patch}},
		parent);
	documents::show_retained_entity_menu(retained, origin, global_pos, parent);
}

void show_address_context_menu(
	const memory_interaction::context_t& focused,
	const memory_interaction::runtime_t& runtime,
	const QString& owner_view_id,
	aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos,
	QWidget* parent) {
	auto context = focused;
	if (find_address_index_impl(context) < 0)
		context.kind = memory_interaction::kind_t::none;
	auto retained = make_memory_actions("memory.value_scan.address", owner_view_id,
		context, runtime, {
			{"memory.address.edit_description", memory_interaction::capability_t::edit_description},
			{"memory.address.change_type", memory_interaction::capability_t::change_type},
			{"memory.address.change_value", memory_interaction::capability_t::change_value},
			{context.frozen ? "memory.address.unfreeze" : "memory.address.freeze",
				context.frozen ? memory_interaction::capability_t::unfreeze : memory_interaction::capability_t::freeze},
			{"memory.entity.open_hex", memory_interaction::capability_t::open_hex},
			{"memory.entity.open_disassembly", memory_interaction::capability_t::open_disassembly},
			{"memory.entity.copy_address", memory_interaction::capability_t::copy_address},
			{"memory.entity.copy_value", memory_interaction::capability_t::copy_value},
			{"memory.entity.stage_patch", memory_interaction::capability_t::stage_patch},
			{"memory.address.remove", memory_interaction::capability_t::remove}},
		parent);
	documents::show_retained_entity_menu(retained, origin, global_pos, parent);
}

}
