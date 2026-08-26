#include "qt/scanner/pointer_controller.hpp"

#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>

#include "core/disasm/disasm_view.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/ui/task_center.hpp"
#include "core/ui/ui_thread_dispatcher.hpp"
#include "helpers/diag_log.hpp"

#include "qt/docking/dock_host.hpp"

namespace aida::qt::scanner {

namespace {

std::uint64_t mix_identity(std::uint64_t hash, std::uint64_t value) {
	hash ^= value;
	return hash * 1099511628211ULL;
}

std::uint64_t mix_identity(std::uint64_t hash, const std::string& value) {
	for (const char character : value)
		hash = mix_identity(hash, static_cast<unsigned char>(character));
	return hash;
}

std::uint64_t chain_identity_value(const pointer_scanner::pointer_chain_t& chain) {
	std::uint64_t hash = 1469598103934665603ULL;
	hash = mix_identity(hash, static_cast<std::uint64_t>(chain.module_index + 1));
	hash = mix_identity(hash, chain.module_name);
	hash = mix_identity(hash, chain.base_offset);
	hash = mix_identity(hash, chain.is_static ? 1ULL : 0ULL);
	for (const auto offset : chain.offsets)
		hash = mix_identity(hash, static_cast<std::uint64_t>(offset));
	return hash;
}

bool checked_apply_offset(std::uint64_t address, std::int64_t offset,
	std::uint64_t& output) {
	const std::uint64_t magnitude = offset < 0
		? static_cast<std::uint64_t>(-(offset + 1)) + 1
		: static_cast<std::uint64_t>(offset);
	if (offset < 0) {
		if (address < magnitude)
			return false;
		output = address - magnitude;
		return true;
	}
	if (address > (std::numeric_limits<std::uint64_t>::max)() - magnitude)
		return false;
	output = address + magnitude;
	return true;
}

std::uint64_t map_fingerprint() {
	auto& state = pointer_scanner::g_state;
	std::lock_guard<std::mutex> lock(state.map_mutex);
	std::uint64_t hash = 1469598103934665603ULL;
	hash = mix_identity(hash, state.last_map_diagnostics.pid);
	hash = mix_identity(hash, state.last_map_diagnostics.duration_ms);
	hash = mix_identity(hash, static_cast<std::uint64_t>(state.map_entry_count));
	for (const auto& module : state.cached_modules) {
		hash = mix_identity(hash, module.base);
		hash = mix_identity(hash, module.size);
		hash = mix_identity(hash, module.name);
	}
	return hash;
}

std::uint64_t result_fingerprint() {
	auto& state = pointer_scanner::g_state;
	std::lock_guard<std::mutex> lock(state.results_mutex);
	std::uint64_t hash = 1469598103934665603ULL;
	hash = mix_identity(hash, static_cast<std::uint64_t>(state.results.size()));
	if (!state.results.empty()) {
		hash = mix_identity(hash, chain_identity_value(state.results.front()));
		hash = mix_identity(hash, chain_identity_value(state.results.back()));
	}
	return hash;
}

}

PointerScanController& PointerScanController::instance()
{
	static PointerScanController* controller = new PointerScanController();
	return *controller;
}

PointerScanController::PointerScanController(QObject* parent) : QObject(parent) {}

void PointerScanController::install(docking::AidaDockHost* host)
{
	host_ = host;
}

std::uint32_t PointerScanController::attached_target_pid()
{
	if (!driver_bridge::is_loaded())
		return 0;
	return driver_bridge::attached_pid();
}

std::string PointerScanController::chain_identity_key(
	const pointer_scanner::pointer_chain_t& chain)
{
	char encoded[24]{};
	std::snprintf(encoded, sizeof(encoded), "%016llX",
		static_cast<unsigned long long>(chain_identity_value(chain)));
	return encoded;
}

std::string PointerScanController::format_offset(std::int64_t offset)
{
	const std::uint64_t magnitude = offset < 0
		? static_cast<std::uint64_t>(-(offset + 1)) + 1
		: static_cast<std::uint64_t>(offset);
	char buf[32];
	if (offset >= 0) snprintf(buf, sizeof(buf), "+0x%llX", static_cast<unsigned long long>(magnitude));
	else          snprintf(buf, sizeof(buf), "-0x%llX", static_cast<unsigned long long>(magnitude));
	return buf;
}

std::optional<std::uint64_t> PointerScanController::chain_base_address(
	const pointer_scanner::pointer_chain_t& chain)
{
	auto& state = pointer_scanner::g_state;
	std::lock_guard<std::mutex> lock(state.map_mutex);
	if (!chain.is_static)
		return chain.base_offset == 0 ? std::optional<std::uint64_t>{} : chain.base_offset;
	if (chain.module_index < 0 ||
		static_cast<std::size_t>(chain.module_index) >= state.cached_modules.size())
		return {};
	const auto module_base = state.cached_modules[static_cast<std::size_t>(chain.module_index)].base;
	if (module_base > (std::numeric_limits<std::uint64_t>::max)() - chain.base_offset)
		return {};
	return module_base + chain.base_offset;
}

std::string PointerScanController::cached_module_name(int module_index)
{
	auto& state = pointer_scanner::g_state;
	std::lock_guard<std::mutex> lock(state.map_mutex);
	if (module_index < 0 ||
		static_cast<std::size_t>(module_index) >= state.cached_modules.size())
		return {};
	return state.cached_modules[static_cast<std::size_t>(module_index)].name;
}

void PointerScanController::observe_generations()
{
	auto& state = pointer_scanner::g_state;
	const bool building = state.map_building.load(std::memory_order_acquire);
	const bool scanning = state.scanning.load(std::memory_order_acquire);
	const std::uint64_t map_value = map_fingerprint();
	const std::uint64_t result_value = result_fingerprint();
	bool changed = false;
	if (map_fingerprint_ != map_value || (observed_map_building_ && !building)) {
		map_fingerprint_ = map_value;
		++map_generation_;
		resolutions_.current_map_generation.store(map_generation_,
			std::memory_order_release);
		changed = true;
	}
	if (result_fingerprint_ != result_value || (observed_scanning_ && !scanning)) {
		result_fingerprint_ = result_value;
		++result_generation_;
		resolutions_.current_result_generation.store(result_generation_,
			std::memory_order_release);
		changed = true;
	}
	observed_map_building_ = building;
	observed_scanning_ = scanning;
	if (changed)
		Q_EMIT stateChanged();
}

void PointerScanController::prune_resolution_cache_locked()
{
	constexpr std::size_t maximum_entries = 256;
	while (resolutions_.entries.size() > maximum_entries) {
		auto oldest = resolutions_.entries.end();
		for (auto iterator = resolutions_.entries.begin();
			iterator != resolutions_.entries.end(); ++iterator) {
			if (iterator->second.status == resolution_status_t::queued ||
				iterator->second.status == resolution_status_t::running)
				continue;
			if (oldest == resolutions_.entries.end() ||
				iterator->second.touch < oldest->second.touch)
				oldest = iterator;
		}
		if (oldest == resolutions_.entries.end())
			break;
		resolutions_.entries.erase(oldest);
	}
}

bool PointerScanController::request_chain_resolution(
	const disasm_view::workspace_context_t& context,
	const pointer_scanner::pointer_chain_t& chain)
{
	if (!context.workspace || !context.workspace->identity().process() || chain.offsets.size() > 64)
		return false;
	const auto base = chain_base_address(chain);
	if (!base)
		return false;
	const std::uint32_t pid = context.workspace->identity().process()->pid;
	const std::uint64_t workspace_generation = context.workspace->generation();
	const std::uint64_t map_generation = map_generation_;
	const std::uint64_t result_generation = result_generation_;
	const std::string key = chain_identity_key(chain);
	const std::uint64_t serial = resolutions_.serial.fetch_add(1,
		std::memory_order_acq_rel) + 1;
	auto cancellation = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> lock(resolutions_.mutex);
		auto existing = resolutions_.entries.find(key);
		if (existing != resolutions_.entries.end() && existing->second.pid == pid &&
			existing->second.workspace_generation == workspace_generation &&
			existing->second.map_generation == map_generation &&
			existing->second.result_generation == result_generation &&
			(existing->second.status == resolution_status_t::queued ||
			 existing->second.status == resolution_status_t::running ||
			 existing->second.status == resolution_status_t::ready)) {
			existing->second.touch = resolutions_.touch.fetch_add(1,
				std::memory_order_acq_rel) + 1;
			return true;
		}
		if (existing != resolutions_.entries.end() && existing->second.cancellation)
			existing->second.cancellation->store(true, std::memory_order_release);
		auto& entry = resolutions_.entries[key];
		entry = {};
		entry.status = resolution_status_t::queued;
		entry.pid = pid;
		entry.workspace_generation = workspace_generation;
		entry.map_generation = map_generation;
		entry.result_generation = result_generation;
		entry.serial = serial;
		entry.touch = resolutions_.touch.fetch_add(1, std::memory_order_acq_rel) + 1;
		entry.cancellation = cancellation;
		prune_resolution_cache_locked();
	}
	const std::string task_id = "pointer.resolve." + key + "." + std::to_string(serial);
	aida::ui::task_center::task_registration_t registration;
	registration.id = task_id;
	registration.source = "pointer_scanner";
	registration.owner = "Pointer Scanner";
	registration.owner_view = "view.memory.pointers";
	registration.owner_action = "Resolve pointer chain";
	registration.target = "PID " + std::to_string(pid);
	registration.label = "Resolve pointer chain";
	registration.stage = "Queued exact dereference sequence";
	registration.affected_entity = pointer_scanner::chain_to_string(chain);
	registration.cancellation_is_safe = true;
	registration.callbacks.cancel = [cancellation] {
		bool expected = false;
		return cancellation->compare_exchange_strong(expected, true,
			std::memory_order_acq_rel);
	};
	if (!aida::ui::task_center::register_task(std::move(registration))) {
		std::lock_guard<std::mutex> lock(resolutions_.mutex);
		auto found = resolutions_.entries.find(key);
		if (found != resolutions_.entries.end() && found->second.serial == serial) {
			found->second.status = resolution_status_t::failed;
			found->second.error = "Task Center rejected ownership of the pointer resolution";
		}
		return false;
	}
	auto workspace = context.workspace;
	auto offsets = chain.offsets;
	auto addresses = std::make_shared<std::vector<std::uint64_t>>();
	auto error = std::make_shared<std::string>();
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "pointer_scanner";
	submission.label = "pointer.resolve_chain";
	submission.thread_class = "bounded_task";
	submission.domain = aida::infra::executor::domain_t::feature_worker;
	submission.priority = 4;
	submission.target_pid = pid;
	submission.generation = workspace_generation;
	submission.cancel_hook = [cancellation] {
		cancellation->store(true, std::memory_order_release);
	};
	submission.body = [this, workspace, offsets = std::move(offsets), addresses, error,
		cancellation, task_id, key, base = *base, pid, workspace_generation,
		map_generation, result_generation, serial]() {
		{
			std::lock_guard<std::mutex> lock(resolutions_.mutex);
			auto found = resolutions_.entries.find(key);
			if (found != resolutions_.entries.end() && found->second.serial == serial)
				found->second.status = resolution_status_t::running;
		}
		static_cast<void>(aida::ui::task_center::update_task(task_id,
			aida::ui::task_center::task_state_t::running, 0.05f,
			"Resolving pointer hops"));
		addresses->reserve(offsets.size() + 1);
		addresses->push_back(base);
		std::uint64_t current = base;
		for (std::size_t index = 0; index < offsets.size(); ++index) {
			if (cancellation->load(std::memory_order_acquire)) {
				*error = "Pointer resolution was cancelled";
				break;
			}
			const auto process = workspace ? workspace->identity().process() : std::nullopt;
			if (!workspace || workspace->closing() || workspace->closed() ||
				workspace->generation() != workspace_generation || !process || process->pid != pid) {
				*error = "Workspace or target changed before the next pointer hop";
				break;
			}
			std::uint64_t pointer = 0;
			std::vector<std::uint8_t> bytes;
			if (!driver_bridge::read_memory_for(pid, current, sizeof(pointer), bytes) ||
				bytes.size() != sizeof(pointer)) {
				*error = "Unreadable pointer at hop " + std::to_string(index + 1) +
					" (exact 8-byte read failed)";
				break;
			}
			std::memcpy(&pointer, bytes.data(), sizeof(pointer));
			if (!checked_apply_offset(pointer, offsets[index], current) || current == 0) {
				*error = "Pointer arithmetic overflow or null result at hop " +
					std::to_string(index + 1);
				break;
			}
			addresses->push_back(current);
		}
		if (cancellation->load(std::memory_order_acquire) && error->empty())
			*error = "Pointer resolution was cancelled";
		auto publish = [this, workspace, addresses, error, cancellation, task_id, key, pid,
			workspace_generation, map_generation, result_generation, serial]() {
			const auto process = workspace ? workspace->identity().process() : std::nullopt;
			const bool current = workspace && !workspace->closing() && !workspace->closed() &&
				workspace->generation() == workspace_generation && process && process->pid == pid &&
				resolutions_.current_map_generation.load(std::memory_order_acquire) == map_generation &&
				resolutions_.current_result_generation.load(std::memory_order_acquire) == result_generation;
			std::lock_guard<std::mutex> lock(resolutions_.mutex);
			auto found = resolutions_.entries.find(key);
			if (found == resolutions_.entries.end() || found->second.serial != serial)
				return;
			if (!current) {
				found->second.status = resolution_status_t::stale;
				found->second.error = "Target, map, results, or workspace generation changed";
				static_cast<void>(aida::ui::task_center::update_task(task_id,
					aida::ui::task_center::task_state_t::cancelled, 1.0f,
					"Discarded stale pointer resolution", found->second.error));
				return;
			}
			found->second.addresses = std::move(*addresses);
			found->second.error = *error;
			if (cancellation->load(std::memory_order_acquire))
				found->second.status = resolution_status_t::cancelled;
			else if (!error->empty())
				found->second.status = resolution_status_t::failed;
			else
				found->second.status = resolution_status_t::ready;
			static_cast<void>(aida::ui::task_center::update_task(task_id,
				found->second.status == resolution_status_t::ready
					? aida::ui::task_center::task_state_t::completed
					: found->second.status == resolution_status_t::cancelled
						? aida::ui::task_center::task_state_t::cancelled
						: aida::ui::task_center::task_state_t::failed,
				1.0f, found->second.status == resolution_status_t::ready
					? "Pointer chain resolved" : "Pointer chain did not resolve",
				found->second.status == resolution_status_t::ready
					? std::to_string(found->second.addresses.size() - 1) + " hops"
					: found->second.error));
		};
		if (!aida::ui_thread::post(std::move(publish), "pointer_scanner",
				"publish_pointer_resolution", "worker_completion")) {
			std::lock_guard<std::mutex> lock(resolutions_.mutex);
			auto found = resolutions_.entries.find(key);
			if (found != resolutions_.entries.end() && found->second.serial == serial) {
				found->second.status = resolution_status_t::failed;
				found->second.error = "UI dispatcher rejected pointer-resolution publication";
			}
			static_cast<void>(aida::ui::task_center::update_task(task_id,
				aida::ui::task_center::task_state_t::failed, 1.0f,
				"UI publication rejected", "The resolved chain was not published"));
		}
	};
	const auto submitted = aida::infra::executor::submit(std::move(submission));
	if (!submitted.submitted) {
		std::lock_guard<std::mutex> lock(resolutions_.mutex);
		auto found = resolutions_.entries.find(key);
		if (found != resolutions_.entries.end() && found->second.serial == serial) {
			found->second.status = resolution_status_t::failed;
			found->second.error = "Worker queue rejected pointer resolution: " + submitted.reject_reason;
		}
		static_cast<void>(aida::ui::task_center::update_task(task_id,
			aida::ui::task_center::task_state_t::failed, 1.0f,
			"Worker queue rejected", submitted.reject_reason));
		return false;
	}
	return true;
}

std::optional<std::uint64_t> PointerScanController::resolved_step_address(
	const pointer_scanner::pointer_chain_t& chain, int step,
	resolution_status_t* status, std::string* error)
{
	if (step < 0)
		return {};
	const std::string key = chain_identity_key(chain);
	std::lock_guard<std::mutex> lock(resolutions_.mutex);
	auto found = resolutions_.entries.find(key);
	if (found == resolutions_.entries.end()) {
		if (status) *status = resolution_status_t::idle;
		return {};
	}
	found->second.touch = resolutions_.touch.fetch_add(1, std::memory_order_acq_rel) + 1;
	if (status) *status = found->second.status;
	if (error) *error = found->second.error;
	if ((found->second.status != resolution_status_t::ready &&
		 found->second.status != resolution_status_t::failed) ||
		static_cast<std::size_t>(step) >= found->second.addresses.size())
		return {};
	return found->second.addresses[static_cast<std::size_t>(step)];
}

}
