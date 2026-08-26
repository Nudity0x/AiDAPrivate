#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "crypto_scanner.hpp"
#include "disasm_view.hpp"
#include "hex_view.hpp"
#include "scanner_async_io.hpp"
#include "../helpers/diag_log.hpp"
#include "../infra/executor.hpp"
#include "../infra/taskflow_runtime.hpp"
#include "../ui/task_center.hpp"
#include "../ui/ui_thread_dispatcher.hpp"

namespace crypto_workspace_scan {

inline constexpr std::size_t k_max_crypto_reference_publication = 0x400000;

enum class export_terminal_t : std::uint8_t {
	idle,
	queued,
	running,
	succeeded,
	failed,
	cancelled,
	stale
};

struct export_status_t {
	export_terminal_t terminal = export_terminal_t::idle;
	std::string message;
	std::string path;
};

struct state_t {
	int    ctx_hit_idx = -1;
	uint64_t context_address = 0;
	std::string context_algorithm;
	std::string context_signature;
	std::vector<std::uint64_t> reference_choices;
	std::uint64_t reference_generation = 0;
	bool open_reference_chooser = false;
	int reference_selected = 0;
	bool reference_focus_pending = false;
	std::mutex mutex;
	std::string last_error;
	std::atomic<bool> scanning{false};
	std::atomic<bool> cancellation_requested{false};
	std::atomic<float> progress{0.0f};
	std::shared_ptr<const crypto_scanner::workspace_scan_snapshot_t> snapshot;
	std::shared_ptr<const std::vector<crypto_scanner::crypto_hit_t>> filtered_results;
	std::string requested_filter;
	int requested_category = -2;
	int requested_sort_column = -2;
	bool requested_sort_ascending = true;
	const crypto_scanner::workspace_scan_snapshot_t* requested_snapshot = nullptr;
	std::atomic<std::uint64_t> filter_serial{0};
	std::atomic<bool> filtering{false};
	std::atomic<std::size_t> total_hits{0};
	std::atomic<std::size_t> cipher_hits{0};
	std::atomic<std::size_t> hash_hits{0};
	std::atomic<std::size_t> referenced_hits{0};
	export_status_t export_status;
	std::atomic<bool> export_pending{false};
	std::atomic<bool> export_dispatch_failed{false};
	std::atomic<std::uint64_t> export_serial{0};
	bool last_export_csv = false;
	std::string last_export_path;
};

using focus_view_fn = std::function<void(const std::string& stable_view_id)>;
inline focus_view_fn& focus_view_slot()
{
	static focus_view_fn fn;
	return fn;
}
inline void set_focus_view(focus_view_fn fn)
{
	focus_view_slot() = std::move(fn);
}
inline void focus_view(const std::string& stable_view_id)
{
	if (focus_view_slot()) focus_view_slot()(stable_view_id);
}

namespace detail {

struct scan_region_t {
	std::uint64_t provider_offset = 0;
	std::uint64_t size = 0;
	std::uint64_t runtime_address = 0;
};

inline std::vector<scan_region_t> scan_regions(
	const disasm_view::workspace_context_t& context)
{
	std::vector<scan_region_t> regions;
	if (!context.workspace) return regions;
	const std::uint64_t provider_size = context.workspace->provider().size();
	if (context.workspace->target_kind() == aida::analysis::target_kind_t::live_snapshot) {
		const auto& module = context.workspace->identity().module();
		if (module && provider_size != 0) regions.push_back({0, provider_size, module->base});
		return regions;
	}
	if (!context.image) return regions;
	for (const auto& section : context.image->sections()) {
		if (section.raw_size == 0 || section.raw_offset >= provider_size) continue;
		const std::uint64_t size = (std::min)(provider_size - section.raw_offset,
			static_cast<std::uint64_t>(section.raw_size));
		if (size != 0) regions.push_back({section.raw_offset, size,
			context.image->image_base() + section.virtual_address});
	}
	return regions;
}

inline void set_scan_error(const std::shared_ptr<state_t>& state, std::string error)
{
	if (!state) return;
	std::lock_guard<std::mutex> lock(state->mutex);
	state->last_error = std::move(error);
}

inline void start_workspace_scan(
	const disasm_view::workspace_context_t& context,
	const std::shared_ptr<state_t>& view_state,
	bool entropy_only)
{
	if (!context.workspace || !view_state) return;
	bool expected = false;
	if (!view_state->scanning.compare_exchange_strong(expected, true,
		std::memory_order_acq_rel, std::memory_order_acquire)) return;
	view_state->cancellation_requested.store(false, std::memory_order_release);
	view_state->progress.store(0.0f, std::memory_order_release);
	std::shared_ptr<const crypto_scanner::workspace_scan_snapshot_t> previous;
	{
		std::lock_guard<std::mutex> lock(view_state->mutex);
		previous = view_state->snapshot;
	}
	set_scan_error(view_state, {});
	aida::infra::taskflow_runtime::task_descriptor_t descriptor;
	descriptor.owner_subsystem = "scanner";
	descriptor.label = entropy_only ? "scanner.crypto.entropy.workspace" :
		"scanner.crypto.signatures.workspace";
	descriptor.thread_class = "bounded_task";
	descriptor.domain = aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
	descriptor.priority = 2;
	const std::string target_id = context.workspace->identity().binary_id().to_hex();
	descriptor.target_id = target_id.c_str();
	descriptor.generation = context.workspace->generation();
	descriptor.cancellable_body = [context, view_state, previous, entropy_only](
		const aida::infra::taskflow_runtime::cancellation_token_t& runtime_cancel) {
		constexpr std::uint64_t chunk_size = 4ULL * 1024ULL * 1024ULL;
		constexpr std::size_t max_hits = 100000;
		constexpr std::size_t max_entropy_regions = 100000;
		auto regions = scan_regions(context);
		if (regions.empty()) {
			set_scan_error(view_state, "The selected workspace has no bounded scan ranges.");
			view_state->scanning.store(false, std::memory_order_release);
			return;
		}
		auto signatures = crypto_scanner::get_signatures();
		if (previous) {
			for (const auto& custom : previous->custom_signatures) {
				if (custom.pattern.empty()) continue;
				signatures.push_back({custom.name.c_str(), custom.algorithm.c_str(),
					custom.description.c_str(), custom.category, custom.pattern.data(),
					custom.pattern.size(), custom.pattern.size()});
			}
		}
		std::size_t overlap = 0;
		for (const auto& signature : signatures)
			overlap = (std::max)(overlap, signature.min_match > 0 ? signature.min_match - 1 : 0);
		std::uint64_t total = 0;
		for (const auto& region : regions) {
			if (total > UINT64_MAX - region.size) {
				set_scan_error(view_state, "Workspace scan range total overflowed.");
				view_state->scanning.store(false, std::memory_order_release);
				return;
			}
			total += region.size;
		}
		std::uint64_t processed = 0;
		std::vector<crypto_scanner::workspace_crypto_hit_t> hits =
			entropy_only && previous ? previous->results :
			std::vector<crypto_scanner::workspace_crypto_hit_t>{};
		std::vector<crypto_scanner::workspace_entropy_region_t> entropy;
		for (const auto& region : regions) {
			for (std::uint64_t cursor = 0; cursor < region.size;) {
				if (runtime_cancel.requested.load(std::memory_order_acquire) ||
					view_state->cancellation_requested.load(std::memory_order_acquire) ||
					context.workspace->cancellation_token().stop_requested()) {
					view_state->scanning.store(false, std::memory_order_release);
					return;
				}
				const std::uint64_t logical = (std::min)(chunk_size, region.size - cursor);
				const std::uint64_t tail = (std::min)(static_cast<std::uint64_t>(overlap),
					region.size - cursor - logical);
				auto lease = context.workspace->provider().lease(region.provider_offset + cursor,
					logical + tail, context.workspace->cancellation_token());
				if (!lease) {
					set_scan_error(view_state, lease.error().message);
					view_state->scanning.store(false, std::memory_order_release);
					return;
				}
				const auto& bytes = lease.value();
				if (!entropy_only) {
					for (const auto& signature : signatures) {
						if (!signature.pattern || signature.min_match == 0 ||
							signature.min_match > bytes.size()) continue;
						const std::size_t limit = bytes.size() - signature.min_match;
						for (std::size_t offset = 0; offset <= limit; ++offset) {
							if (offset >= logical || hits.size() >= max_hits) break;
							if (std::memcmp(bytes.data() + offset, signature.pattern,
								signature.min_match) != 0) continue;
							const std::uint64_t runtime = region.runtime_address + cursor + offset;
							const auto address = disasm_view::typed_address(context, runtime);
							if (!address) continue;
							crypto_scanner::workspace_crypto_hit_t hit;
							hit.signature_name = signature.name;
							hit.algorithm = signature.algorithm;
							hit.category = signature.category;
							hit.address = *address;
							hit.module_name = context.workspace->identity().bin_name();
							const auto& module = context.workspace->identity().module();
							const std::uint64_t module_base = module ? module->base :
								context.workspace->identity().image_base();
							hit.module_offset = runtime >= module_base ? runtime - module_base : offset;
							hits.push_back(std::move(hit));
						}
						if (hits.size() >= max_hits) break;
					}
				} else {
					for (std::size_t offset = 0; offset + 256 <= logical &&
						entropy.size() < max_entropy_regions; offset += 256) {
						const float value = crypto_scanner::detail::compute_shannon_entropy(
							bytes.data() + offset, 256);
						if (value < 7.0f) continue;
						const auto address = disasm_view::typed_address(context,
							region.runtime_address + cursor + offset);
						if (address) entropy.push_back({*address, value, 256,
							context.workspace->identity().bin_name()});
					}
				}
				cursor += logical;
				processed += logical;
				view_state->progress.store(total == 0 ? 1.0f : static_cast<float>(
					static_cast<double>(processed) / static_cast<double>(total)));
			}
		}
		if (!entropy_only && context.publication && context.publication->snapshot) {
			std::map<aida::analysis::address_t,
				std::vector<aida::analysis::address_t>> references;
			for (const auto& xref : context.publication->snapshot->xrefs)
				references[xref.target].push_back(xref.source);
			for (auto& hit : hits) {
				auto found = references.find(hit.address);
				if (found != references.end()) hit.referencing_functions = std::move(found->second);
			}
		}
		std::size_t cipher_count = 0;
		std::size_t hash_count = 0;
		std::size_t referenced_count = 0;
		for (const auto& hit : hits) {
			const int category = static_cast<int>(hit.category);
			if (category == 0 || category == 3 || category == 4) ++cipher_count;
			else if (category == 1) ++hash_count;
			if (!hit.referencing_functions.empty()) ++referenced_count;
		}
		view_state->total_hits.store(hits.size(), std::memory_order_release);
		view_state->cipher_hits.store(cipher_count, std::memory_order_release);
		view_state->hash_hits.store(hash_count, std::memory_order_release);
		view_state->referenced_hits.store(referenced_count, std::memory_order_release);
		auto publication = std::make_shared<crypto_scanner::workspace_scan_snapshot_t>();
		publication->results = std::move(hits);
		publication->entropy_map = std::move(entropy);
		if (previous) publication->custom_signatures = previous->custom_signatures;
		publication->progress = 1.0f;
		publication->scanning = false;
		publication->cancellation_requested = false;
		{
			std::lock_guard<std::mutex> lock(view_state->mutex);
			view_state->snapshot = std::move(publication);
			view_state->filtered_results.reset();
			view_state->requested_snapshot = nullptr;
			view_state->filter_serial.fetch_add(1, std::memory_order_acq_rel);
		}
		view_state->progress.store(1.0f, std::memory_order_release);
		view_state->scanning.store(false, std::memory_order_release);
	};
	const auto submitted = aida::infra::taskflow_runtime::submit(std::move(descriptor));
	if (!submitted.submitted) {
		set_scan_error(view_state, submitted.reject_reason);
		view_state->scanning.store(false, std::memory_order_release);
	}
}

inline void open_hit_in_hex(const disasm_view::workspace_context_t& context,
	std::uint64_t address)
{
	if (context.workspace->target_kind() == aida::analysis::target_kind_t::live_snapshot) {
		hex_view::request_live_memory(context, address, 256);
		return;
	}
	hex_view::activate(context);
}

inline std::string lowercase(std::string value)
{
	for (char& character : value)
		character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	return value;
}

inline void request_filtered_results(
	const disasm_view::workspace_context_t& context,
	const std::shared_ptr<state_t>& state,
	const std::shared_ptr<const crypto_scanner::workspace_scan_snapshot_t>& snapshot,
	std::string filter,
	int category,
	int sort_column,
	bool sort_ascending)
{
	if (!context.workspace || !state || !snapshot) return;
	filter = lowercase(std::move(filter));
	std::uint64_t serial = 0;
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		if (state->requested_snapshot == snapshot.get() &&
			state->requested_filter == filter && state->requested_category == category &&
			state->requested_sort_column == sort_column &&
			state->requested_sort_ascending == sort_ascending) return;
		state->requested_snapshot = snapshot.get();
		state->requested_filter = filter;
		state->requested_category = category;
		state->requested_sort_column = sort_column;
		state->requested_sort_ascending = sort_ascending;
		serial = state->filter_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
		state->filtering.store(true, std::memory_order_release);
	}
	aida::infra::taskflow_runtime::task_descriptor_t descriptor;
	descriptor.owner_subsystem = "scanner";
	descriptor.label = "scanner.crypto.filter.workspace";
	descriptor.thread_class = "bounded_task";
	descriptor.domain = aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
	descriptor.priority = 3;
	const std::string target_id = context.workspace->identity().binary_id().to_hex();
	descriptor.target_id = target_id.c_str();
	descriptor.generation = context.workspace->generation();
	descriptor.body = [context, state, snapshot, filter = std::move(filter), category,
		sort_column, sort_ascending, serial, generation = context.workspace->generation()]() {
		auto filtered = std::make_shared<std::vector<crypto_scanner::crypto_hit_t>>();
		filtered->reserve(snapshot->results.size());
		for (const auto& workspace_hit : snapshot->results) {
			if (state->filter_serial.load(std::memory_order_acquire) != serial) return;
			if (context.workspace->cancellation_token().stop_requested()) {
				state->filtering.store(false, std::memory_order_release);
				return;
			}
			if (category >= 0 && static_cast<int>(workspace_hit.category) != category) continue;
			if (!filter.empty() && lowercase(workspace_hit.signature_name).find(filter) == std::string::npos &&
				lowercase(workspace_hit.algorithm).find(filter) == std::string::npos &&
				lowercase(workspace_hit.module_name).find(filter) == std::string::npos) continue;
			crypto_scanner::crypto_hit_t hit;
			hit.signature_name = workspace_hit.signature_name;
			hit.algorithm = workspace_hit.algorithm;
			hit.category = workspace_hit.category;
			const auto runtime = disasm_view::runtime_address(context, workspace_hit.address);
			if (!runtime) continue;
			hit.address = *runtime;
			hit.module_name = workspace_hit.module_name;
			hit.module_offset = workspace_hit.module_offset;
			for (const auto& reference : workspace_hit.referencing_functions) {
				const auto address = disasm_view::runtime_address(context, reference);
				if (address) hit.referencing_functions.push_back(*address);
			}
			filtered->push_back(std::move(hit));
		}
		if (sort_column >= 0) {
			std::sort(filtered->begin(), filtered->end(),
				[sort_column, sort_ascending](const auto& left, const auto& right) {
					int comparison = 0;
					switch (sort_column) {
					case 0: comparison = left.algorithm.compare(right.algorithm); break;
					case 1: comparison = left.signature_name.compare(right.signature_name); break;
					case 2: comparison = left.address < right.address ? -1 :
						(left.address > right.address ? 1 : 0); break;
					case 3: comparison = left.module_name.compare(right.module_name); break;
					default: break;
					}
					return sort_ascending ? comparison < 0 : comparison > 0;
				});
		}
		if (context.workspace->generation() != generation ||
			state->filter_serial.load(std::memory_order_acquire) != serial) return;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->filtered_results = std::move(filtered);
		}
		state->filtering.store(false, std::memory_order_release);
	};
	const auto submitted = aida::infra::taskflow_runtime::submit(std::move(descriptor));
	if (!submitted.submitted) {
		set_scan_error(state, submitted.reject_reason);
		state->filtering.store(false, std::memory_order_release);
	}
}

inline constexpr std::size_t max_export_results = 100000;
inline constexpr std::size_t max_export_string = 512;

inline void set_export_status(const std::shared_ptr<state_t>& state,
	export_terminal_t terminal, std::string message, std::string path = {})
{
	if (!state) return;
	std::lock_guard<std::mutex> lock(state->mutex);
	state->export_status = {terminal, std::move(message), std::move(path)};
}

inline bool export_workspace_matches(
	const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
	const std::shared_ptr<state_t>& state,
	const std::shared_ptr<const crypto_scanner::workspace_scan_snapshot_t>& snapshot,
	const std::string& binary_id, std::uint64_t workspace_generation,
	std::uint64_t publication_generation, std::uint32_t pid)
{
	if (!workspace || workspace->closing() || workspace->closed() ||
		workspace->identity().binary_id().to_hex() != binary_id ||
		workspace->generation() != workspace_generation) return false;
	const auto publication = workspace->analysis_publication();
	if (!publication || publication->generation != publication_generation) return false;
	const auto process = workspace->identity().process();
	if (pid == 0 ? static_cast<bool>(process) : !process || process->pid != pid) return false;
	std::lock_guard<std::mutex> lock(state->mutex);
	return state->snapshot == snapshot;
}

inline bool append_export(std::string& output, const std::string& value, std::string& error)
{
	if (output.size() > scanner_async_io::max_serialized_bytes ||
		value.size() > scanner_async_io::max_serialized_bytes - output.size()) {
		error = "Crypto export exceeds the 64 MiB serialized-output limit";
		return false;
	}
	output.append(value);
	return true;
}

inline std::string csv_field(const std::string& value)
{
	std::string output;
	output.reserve(value.size() + 2);
	output.push_back('"');
	for (char character : value) {
		if (character == '"') output.push_back('"');
		output.push_back(character);
	}
	output.push_back('"');
	return output;
}

inline bool serialize_export(const disasm_view::workspace_context_t& context,
	const crypto_scanner::workspace_scan_snapshot_t& snapshot, bool csv,
	const std::shared_ptr<std::atomic<bool>>& cancellation, std::string& output,
	std::string& error)
{
	if (snapshot.results.size() > max_export_results) {
		error = "Crypto export exceeds the 100000-result limit";
		return false;
	}
	try {
	output.clear();
	output.reserve((std::min)(scanner_async_io::max_serialized_bytes,
		snapshot.results.size() * static_cast<std::size_t>(192) + 256));
	if (csv) {
		if (!append_export(output,
			"algorithm,signature,address,module,module_offset,references\n", error)) return false;
	} else {
		const std::string prefix = "{\n  \"binary_id\": " +
			nlohmann::json(context.workspace->identity().binary_id().to_hex()).dump() +
			",\n  \"bin_name\": " + nlohmann::json(context.workspace->identity().bin_name()).dump() +
			",\n  \"results\": [";
		if (!append_export(output, prefix, error)) return false;
	}
	bool first = true;
	for (const auto& hit : snapshot.results) {
		if (scanner_async_io::cancellation_requested(cancellation)) {
			error = "Crypto export cancelled";
			return false;
		}
		if (hit.algorithm.size() > max_export_string || hit.signature_name.size() > max_export_string ||
			hit.module_name.size() > max_export_string) {
			error = "Crypto export contains a string that exceeds 512 bytes";
			return false;
		}
		const auto address = disasm_view::runtime_address(context, hit.address);
		if (!address) continue;
		char address_text[32]{};
		char offset_text[32]{};
		std::snprintf(address_text, sizeof(address_text), "0x%llX",
			static_cast<unsigned long long>(*address));
		std::snprintf(offset_text, sizeof(offset_text), "0x%llX",
			static_cast<unsigned long long>(hit.module_offset));
		std::string row;
		if (csv) {
			row = csv_field(hit.algorithm) + "," + csv_field(hit.signature_name) + "," +
				address_text + "," + csv_field(hit.module_name) + "," + offset_text + "," +
				std::to_string(hit.referencing_functions.size()) + "\n";
		} else {
			nlohmann::json item;
			item["algorithm"] = hit.algorithm;
			item["signature"] = hit.signature_name;
			item["address"] = *address;
			item["module"] = hit.module_name;
			item["module_offset"] = hit.module_offset;
			item["references"] = hit.referencing_functions.size();
			row = (first ? "\n    " : ",\n    ") + item.dump();
			first = false;
		}
		if (!append_export(output, row, error)) return false;
	}
	return csv || append_export(output, first ? "]\n}\n" : "\n  ]\n}\n", error);
	} catch (const std::exception& exception) {
		output.clear();
		error = "Crypto export serialization failed: " + std::string(exception.what());
		return false;
	}
}

inline std::atomic<std::uint64_t> export_task_sequence{1};

inline std::string register_export_task(bool csv, const std::string& target,
	const std::shared_ptr<std::atomic<bool>>& cancellation,
	const std::shared_ptr<std::atomic<std::uint8_t>>& commit_gate)
{
	const std::string id = "scanner.crypto.export." +
		std::to_string(export_task_sequence.fetch_add(1, std::memory_order_acq_rel));
	aida::ui::task_center::task_registration_t registration;
	registration.id = id;
	registration.source = "human";
	registration.owner = "Crypto Scanner";
	registration.owner_view = "view.memory.crypto";
	registration.owner_action = csv ? "scanner.crypto.export.csv" : "scanner.crypto.export.json";
	registration.target = target;
	registration.label = csv ? "Export crypto results as CSV" : "Export crypto results as JSON";
	registration.stage = "Queued";
	registration.progress = -1.0f;
	registration.cancellation_is_safe = true;
	registration.callbacks.cancel = [cancellation, commit_gate] {
		std::uint8_t expected_gate = scanner_async_io::operation_reversible;
		if (!commit_gate->compare_exchange_strong(expected_gate,
			scanner_async_io::operation_cancelled, std::memory_order_acq_rel,
			std::memory_order_acquire)) return false;
		bool expected = false;
		return cancellation->compare_exchange_strong(expected, true, std::memory_order_acq_rel);
	};
	registration.callbacks.focus = [] {
		focus_view("view.memory.crypto");
	};
	return aida::ui::task_center::register_task(std::move(registration)) ? id : std::string();
}

inline void finish_export_task(const std::string& id, export_terminal_t terminal,
	const std::string& stage, const std::string& summary)
{
	if (id.empty()) return;
	auto state = aida::ui::task_center::task_state_t::failed;
	if (terminal == export_terminal_t::succeeded)
		state = aida::ui::task_center::task_state_t::completed;
	else if (terminal == export_terminal_t::cancelled || terminal == export_terminal_t::stale)
		state = aida::ui::task_center::task_state_t::cancelled;
	static_cast<void>(aida::ui::task_center::update_task(id, state, 1.0f, stage, summary));
}

inline void export_results(const disasm_view::workspace_context_t& context,
	const std::shared_ptr<state_t>& state,
	std::shared_ptr<const crypto_scanner::workspace_scan_snapshot_t> snapshot,
	std::string path, bool csv)
{
	if (!context.workspace || !context.publication || !state || !snapshot) return;
	bool expected = false;
	if (!state->export_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
	const std::uint64_t serial = state->export_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->last_export_csv = csv;
		state->last_export_path = path;
		state->export_status = {export_terminal_t::queued, "Queued immutable crypto export", path};
	}
	const auto workspace = context.workspace;
	const std::string binary_id = workspace->identity().binary_id().to_hex();
	const std::uint64_t workspace_generation = workspace->generation();
	const std::uint64_t publication_generation = context.publication->generation;
	const auto process = workspace->identity().process();
	const std::uint32_t pid = process ? process->pid : 0;
	auto cancellation = std::make_shared<std::atomic<bool>>(false);
	auto commit_gate = std::make_shared<std::atomic<std::uint8_t>>(
		scanner_async_io::operation_reversible);
	const std::string task_id = register_export_task(csv, binary_id, cancellation, commit_gate);
	if (task_id.empty()) {
		state->export_pending.store(false, std::memory_order_release);
		set_export_status(state, export_terminal_t::failed,
			"Task Center rejected crypto export ownership", path);
		return;
	}
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "scanner.crypto";
	submission.label = csv ? "scanner.crypto.export.csv" : "scanner.crypto.export.json";
	submission.thread_class = "bounded_task";
	submission.domain = aida::infra::executor::domain_t::feature_worker;
	submission.priority = 2;
	submission.target_pid = pid;
	submission.generation = publication_generation;
	submission.cancel_hook = [cancellation, commit_gate] {
		std::uint8_t expected_gate = scanner_async_io::operation_reversible;
		if (commit_gate->compare_exchange_strong(expected_gate, scanner_async_io::operation_cancelled,
			std::memory_order_acq_rel, std::memory_order_acquire))
			cancellation->store(true, std::memory_order_release);
	};
	submission.body = [context, workspace, state, snapshot = std::move(snapshot), path = std::move(path),
		csv, cancellation, task_id, binary_id, workspace_generation, publication_generation,
		pid, serial, commit_gate]() mutable {
		static_cast<void>(aida::ui::task_center::update_task(task_id,
			aida::ui::task_center::task_state_t::running, 0.1f, "Serializing bounded crypto results"));
		static_cast<void>(aida::ui_thread::post([state, serial] {
			if (state->export_serial.load(std::memory_order_acquire) == serial &&
				state->export_pending.load(std::memory_order_acquire))
				set_export_status(state, export_terminal_t::running,
					"Serializing bounded crypto results");
		}, "scanner.crypto", "publish_export_running", "worker_progress"));
		std::string output;
		std::string error;
		bool serialized = serialize_export(context, *snapshot, csv, cancellation, output, error);
		if (serialized && path.empty()) {
			char* appdata = nullptr;
			std::size_t length = 0;
			_dupenv_s(&appdata, &length, "APPDATA");
			if (appdata) {
				path = (std::filesystem::path(appdata) / "AiDA" / "Standalone" /
					("crypto_export_" + binary_id.substr(0, 16) + (csv ? ".csv" : ".json"))).string();
				free(appdata);
			}
			if (path.empty()) { serialized = false; error = "APPDATA is unavailable for crypto export"; }
		}
		auto current = [workspace, state, snapshot, binary_id, workspace_generation,
			publication_generation, pid] {
			return export_workspace_matches(workspace, state, snapshot, binary_id,
				workspace_generation, publication_generation, pid);
		};
		scanner_async_io::result_t write;
		if (serialized && current())
			write = scanner_async_io::atomic_replace(path, output, true, cancellation, current, commit_gate);
		else if (serialized)
			write.error = "Crypto workspace, target, publication, or snapshot changed";
		const bool cancelled = scanner_async_io::cancellation_requested(cancellation) || write.cancelled;
		const bool stale = !cancelled && !current();
		const bool success = serialized && write.success && !stale;
		if (!success && error.empty()) error = write.error;
		const export_terminal_t terminal = success ? export_terminal_t::succeeded :
			cancelled ? export_terminal_t::cancelled : stale ? export_terminal_t::stale :
			export_terminal_t::failed;
		auto publish = [state, serial, terminal, error = std::move(error), path, task_id]() mutable {
			if (state->export_serial.load(std::memory_order_acquire) != serial) return;
			set_export_status(state, terminal,
				terminal == export_terminal_t::succeeded ? "Crypto export committed atomically" : error, path);
			state->export_pending.store(false, std::memory_order_release);
			finish_export_task(task_id, terminal,
				terminal == export_terminal_t::succeeded ? "Crypto export complete" : "Crypto export failed",
				terminal == export_terminal_t::succeeded ? path : error);
		};
		if (!aida::ui_thread::post(std::move(publish), "scanner.crypto", "publish_export", "worker_completion")) {
			state->export_pending.store(false, std::memory_order_release);
			state->export_dispatch_failed.store(true, std::memory_order_release);
			finish_export_task(task_id, export_terminal_t::failed, "UI publication rejected",
				"Crypto export completion was not published");
		}
	};
	const auto submitted = aida::infra::executor::submit(std::move(submission));
	if (!submitted.submitted) {
		state->export_pending.store(false, std::memory_order_release);
		set_export_status(state, export_terminal_t::failed,
			"Worker queue rejected crypto export: " + submitted.reject_reason, state->last_export_path);
		finish_export_task(task_id, export_terminal_t::failed, "Worker queue rejected", submitted.reject_reason);
	}
}

}

}
