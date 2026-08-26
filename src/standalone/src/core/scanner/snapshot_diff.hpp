#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <shlobj.h>

#include "standalone_driver.hpp"
#include "../helpers/diag_log.hpp"
#include "../infra/executor.hpp"
#include "scanner_task_center.hpp"

namespace snapshot_diff {

struct memory_region_t {
	uint64_t             base = 0;
	uint64_t             size = 0;
	uint32_t             protect = 0;
	std::vector<uint8_t> data;
};

struct snapshot_t {
	uint64_t                   id = 0;
	std::string                name;
	uint32_t                   pid = 0;
	int64_t                    timestamp = 0;
	std::vector<memory_region_t> regions;
	uint64_t                   total_bytes = 0;
};

enum class change_type_t : int {
	unknown = 0,
	pointer_changed,
	float_changed,
	counter_incremented,
	counter_decremented,
	string_modified,
	zeroed_out,
	byte_flip,
};

struct changed_region_t {
	uint64_t             address = 0;
	uint32_t             size = 0;
	std::vector<uint8_t> old_data;
	std::vector<uint8_t> new_data;
	change_type_t        type = change_type_t::unknown;
	std::string          module_name;
};

struct diff_result_t {
	std::string                  snap_a_name;
	std::string                  snap_b_name;
	std::vector<changed_region_t> changes;
	uint64_t                     total_changed_bytes = 0;
	std::size_t                  changed_page_count = 0;
	bool                         truncated = false;
};

struct state_t {
	std::vector<std::shared_ptr<const snapshot_t>> snapshots;
	uint64_t                    snap_a_id = 0;
	uint64_t                    snap_b_id = 0;
	std::shared_ptr<const diff_result_t> published_diff = std::make_shared<const diff_result_t>();
	std::mutex                  mutex;
	std::atomic<bool>           capturing{false};
	std::atomic<bool>           comparing{false};
	std::atomic<bool>           loading{false};
	std::atomic<float>          progress{0.f};
	std::atomic<bool>           cancel{false};
	std::atomic<uint64_t>       operation_generation{1};
	int                         snap_counter = 0;
	std::atomic<uint64_t>       next_snap_id{1};
	std::string                 last_error;
};

inline state_t g_state;

inline std::string last_error()
{
	std::lock_guard<std::mutex> lock(g_state.mutex);
	return g_state.last_error;
}

inline void set_last_error(std::string error)
{
	std::lock_guard<std::mutex> lock(g_state.mutex);
	g_state.last_error = std::move(error);
}

namespace detail {

inline constexpr std::uint64_t maximum_snapshot_bytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t maximum_region_bytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t maximum_snapshot_regions = 4096;
inline constexpr std::size_t maximum_diff_ranges = 250000;
inline constexpr std::uint32_t maximum_snapshot_name_bytes = 128;
inline constexpr std::uint64_t maximum_snapshot_file_bytes = maximum_snapshot_bytes +
	static_cast<std::uint64_t>(maximum_snapshot_regions) * 20ULL +
	maximum_snapshot_name_bytes + 32ULL;

inline std::filesystem::path snapshot_dir()
{
	wchar_t* appdata = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
		auto p = std::filesystem::path(appdata) / L"AiDA" / L"Standalone" / L"snapshots";
		CoTaskMemFree(appdata);
		return p;
	}
	return std::filesystem::current_path() / "snapshots";
}

inline change_type_t classify_change(const uint8_t* old_data, const uint8_t* new_data, uint32_t size)
{
	bool all_zero_new = true;
	for (uint32_t i = 0; i < size; ++i)
		if (new_data[i] != 0) { all_zero_new = false; break; }
	if (all_zero_new) return change_type_t::zeroed_out;

	if (size == 4) {
		float old_f, new_f;
		std::memcpy(&old_f, old_data, 4);
		std::memcpy(&new_f, new_data, 4);
		if (std::isfinite(old_f) && std::isfinite(new_f) &&
		    std::abs(old_f) < 1e12f && std::abs(new_f) < 1e12f)
			return change_type_t::float_changed;

		int32_t old_i, new_i;
		std::memcpy(&old_i, old_data, 4);
		std::memcpy(&new_i, new_data, 4);
		if (new_i == old_i + 1) return change_type_t::counter_incremented;
		if (new_i == old_i - 1) return change_type_t::counter_decremented;
	}

	if (size == 8) {
		uint64_t old_val, new_val;
		std::memcpy(&old_val, old_data, 8);
		std::memcpy(&new_val, new_data, 8);
		if (old_val > 0x10000 && new_val > 0x10000 &&
		    old_val < 0x00007FFFFFFFFFFF && new_val < 0x00007FFFFFFFFFFF)
			return change_type_t::pointer_changed;
	}

	if (size >= 4) {
		bool is_string = true;
		for (uint32_t i = 0; i < size; ++i) {
			if (old_data[i] == 0) break;
			if (old_data[i] < 0x20 || old_data[i] > 0x7E) { is_string = false; break; }
		}
		if (is_string) return change_type_t::string_modified;
	}

	if (size == 1) return change_type_t::byte_flip;

	return change_type_t::unknown;
}

inline const char* change_type_name(change_type_t t)
{
	switch (t) {
	case change_type_t::pointer_changed:     return "Pointer";
	case change_type_t::float_changed:       return "Float";
	case change_type_t::counter_incremented: return "Counter++";
	case change_type_t::counter_decremented: return "Counter--";
	case change_type_t::string_modified:     return "String";
	case change_type_t::zeroed_out:          return "Zeroed";
	case change_type_t::byte_flip:           return "Byte";
	default:                                 return "Unknown";
	}
}

inline bool save_snapshot(const snapshot_t& snap, std::string& error)
{
	error.clear();
	if (snap.regions.size() > maximum_snapshot_regions ||
		snap.total_bytes > maximum_snapshot_bytes) {
		error = "save_snapshot: snapshot exceeds the bounded persistence limit";
		return false;
	}
	auto dir = snapshot_dir();
	std::error_code filesystem_error;
	std::filesystem::create_directories(dir, filesystem_error);
	if (filesystem_error) {
		error = "save_snapshot: cannot create snapshot directory: " + filesystem_error.message();
		return false;
	}

	char fname[128];
	const auto unique_tick = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	snprintf(fname, sizeof(fname), "snapshot_%lld_%llu_%llu.bin",
		static_cast<long long>(snap.timestamp),
		static_cast<unsigned long long>(snap.id),
		static_cast<unsigned long long>(unique_tick));
	auto path = dir / fname;
	auto temporary = path;
	temporary += ".aida-tmp";
	std::filesystem::remove(temporary, filesystem_error);
	filesystem_error.clear();

	std::ofstream ofs(temporary, std::ios::binary | std::ios::trunc);
	if (!ofs.is_open()) {
		error = "save_snapshot: failed to create temporary snapshot";
		return false;
	}

	uint32_t name_len = static_cast<uint32_t>((std::min)(snap.name.size(),
		static_cast<std::size_t>(maximum_snapshot_name_bytes)));
	ofs.write(reinterpret_cast<const char*>(&name_len), 4);
	ofs.write(snap.name.data(), name_len);
	ofs.write(reinterpret_cast<const char*>(&snap.pid), 4);
	ofs.write(reinterpret_cast<const char*>(&snap.timestamp), 8);
	uint32_t region_count = static_cast<uint32_t>(snap.regions.size());
	ofs.write(reinterpret_cast<const char*>(&region_count), 4);

	for (auto& r : snap.regions) {
		if (r.data.size() > maximum_region_bytes) {
			error = "save_snapshot: region exceeds the bounded persistence limit";
			break;
		}
		ofs.write(reinterpret_cast<const char*>(&r.base), 8);
		uint64_t rsize = r.data.size();
		ofs.write(reinterpret_cast<const char*>(&rsize), 8);
		ofs.write(reinterpret_cast<const char*>(&r.protect), 4);
		if (rsize > 0)
			ofs.write(reinterpret_cast<const char*>(r.data.data()), static_cast<std::streamsize>(rsize));
		if (!ofs) {
			error = "save_snapshot: snapshot write failed or was partial";
			break;
		}
	}
	ofs.flush();
	if (!ofs && error.empty()) error = "save_snapshot: snapshot flush failed";
	ofs.close();
	if (!error.empty()) {
		std::filesystem::remove(temporary, filesystem_error);
		return false;
	}
	const auto encoded_size = std::filesystem::file_size(temporary, filesystem_error);
	if (filesystem_error || encoded_size > maximum_snapshot_file_bytes) {
		error = "save_snapshot: persisted snapshot size verification failed";
		std::filesystem::remove(temporary, filesystem_error);
		return false;
	}
	std::filesystem::rename(temporary, path, filesystem_error);
	if (filesystem_error) {
		error = "save_snapshot: atomic commit failed: " + filesystem_error.message();
		std::filesystem::remove(temporary, filesystem_error);
		return false;
	}
	return true;
}

inline bool load_snapshot(const std::string& path, snapshot_t& out, std::string& error)
{
	error.clear();
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs.is_open()) {
		error = "load_snapshot: failed to open file";
		return false;
	}
	ifs.seekg(0, std::ios::end);
	const auto encoded_size = ifs.tellg();
	if (encoded_size < 0 || static_cast<std::uint64_t>(encoded_size) > maximum_snapshot_file_bytes) {
		error = "load_snapshot: file exceeds the bounded snapshot limit";
		return false;
	}
	ifs.seekg(0, std::ios::beg);

	uint32_t name_len = 0;
	if (!ifs.read(reinterpret_cast<char*>(&name_len), 4)) {
		error = "load_snapshot: failed reading name length";
		return false;
	}
	if (name_len > maximum_snapshot_name_bytes) {
		error = "load_snapshot: implausible name length";
		return false;
	}

	std::string name;
	name.resize(name_len);
	if (name_len > 0) {
		if (!ifs.read(name.data(), static_cast<std::streamsize>(name_len))) {
			error = "load_snapshot: failed reading name";
			return false;
		}
	}

	uint32_t pid = 0;
	int64_t  ts = 0;
	uint32_t region_count = 0;
	if (!ifs.read(reinterpret_cast<char*>(&pid), 4) ||
	    !ifs.read(reinterpret_cast<char*>(&ts), 8) ||
	    !ifs.read(reinterpret_cast<char*>(&region_count), 4)) {
		error = "load_snapshot: failed reading header";
		return false;
	}

	if (region_count > maximum_snapshot_regions) {
		error = "load_snapshot: implausible region count";
		return false;
	}

	out = snapshot_t{};
	out.name = std::move(name);
	out.pid = pid;
	out.timestamp = ts;
	out.regions.reserve(region_count);

	for (uint32_t i = 0; i < region_count; ++i) {
		memory_region_t r;
		uint64_t rsize = 0;
		if (!ifs.read(reinterpret_cast<char*>(&r.base), 8) ||
		    !ifs.read(reinterpret_cast<char*>(&rsize), 8) ||
		    !ifs.read(reinterpret_cast<char*>(&r.protect), 4)) {
			error = "load_snapshot: failed reading region header";
			return false;
		}

		if (rsize > maximum_region_bytes || out.total_bytes > maximum_snapshot_bytes - rsize) {
			error = "load_snapshot: implausible region size";
			return false;
		}

		r.size = rsize;
		if (rsize > 0) {
			r.data.resize(static_cast<std::size_t>(rsize));
			if (!ifs.read(reinterpret_cast<char*>(r.data.data()), static_cast<std::streamsize>(rsize))) {
				error = "load_snapshot: failed reading region data";
				return false;
			}
		}

		out.total_bytes += rsize;
		out.regions.push_back(std::move(r));
	}
	if (ifs.peek() != std::char_traits<char>::eof()) {
		error = "load_snapshot: unexpected trailing payload";
		return false;
	}

	return true;
}

}

inline void take_snapshot(const std::string& name = "")
{
	if (g_state.capturing.load()) {
		diag::log_tagged("snapshot_diff", "take_snapshot refused already_capturing");
		return;
	}
	if (!driver_bridge::is_loaded() || driver_bridge::attached_pid() == 0) {
		diag::log_tagged_fmt("snapshot_diff", "take_snapshot refused not_attached driver_loaded=%d pid=%u",
			static_cast<int>(driver_bridge::is_loaded()), driver_bridge::attached_pid());
		set_last_error("take_snapshot: no process attached");
		return;
	}

	g_state.capturing.store(true);
	g_state.cancel.store(false);
	g_state.progress.store(0.f);
	const std::uint64_t operation_generation =
		g_state.operation_generation.fetch_add(1, std::memory_order_acq_rel) + 1;

	std::string snap_name = name;
	if (snap_name.empty()) {
		++g_state.snap_counter;
		char buf[32];
		snprintf(buf, sizeof(buf), "Snap%d", g_state.snap_counter);
		snap_name = buf;
	}

	diag::log_tagged_fmt("snapshot_diff", "take_snapshot start name='%s' pid=%u",
		snap_name.c_str(), driver_bridge::attached_pid());

	aida::infra::executor::submission_t sub;
	sub.owner_subsystem = "scanner";
	sub.label = "scanner.snapshot_take";
	sub.thread_class = "scanner_snapshot";
	sub.domain = aida::infra::executor::domain_t::long_running;
	sub.priority = 2;
	sub.target_pid = driver_bridge::attached_pid();
	sub.generation = operation_generation;
	sub.body = [snap_name, operation_generation]() {
		auto t_start = std::chrono::steady_clock::now();
		snapshot_t snap;
		snap.id = g_state.next_snap_id.fetch_add(1);
		snap.name = snap_name;
		snap.pid = driver_bridge::attached_pid();
		snap.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();

		auto regions = driver_bridge::enumerate_memory_regions(4096);

		std::vector<driver_bridge::memory_region_t> readable;
		for (auto& r : regions) {
			if (r.state != 0x1000) continue;
			if (r.protect & 0x100) continue;
			uint32_t p = r.protect & 0xFF;
			if (p == 0x01 || p == 0x00) continue;
			if (r.size > 0x10000000) continue;
			readable.push_back(r);
		}

		uint64_t total = 0;
		for (auto& r : readable) total += r.size;
		if (total == 0) total = 1;
		uint64_t done = 0;

		bool bounded = false;
		for (auto& r : readable) {
			if (g_state.cancel.load()) break;
			if (snap.total_bytes >= detail::maximum_snapshot_bytes) {
				bounded = true;
				break;
			}

			memory_region_t sr;
			sr.base = r.base;
			sr.protect = r.protect;
			const std::uint64_t remaining = detail::maximum_snapshot_bytes - snap.total_bytes;
			const std::uint64_t requested = (std::min)({r.size,
				detail::maximum_region_bytes, remaining});
			bounded = bounded || requested < r.size;

			if (requested > 0 && driver_bridge::read_memory(r.base,
				static_cast<std::size_t>(requested), sr.data) && !sr.data.empty()) {
				sr.size = sr.data.size();
				snap.total_bytes += sr.data.size();
				snap.regions.push_back(std::move(sr));
			}

			done += r.size;
			g_state.progress.store(static_cast<float>(done) / static_cast<float>(total));
		}
		if (g_state.cancel.load(std::memory_order_acquire) ||
			g_state.operation_generation.load(std::memory_order_acquire) != operation_generation) {
			g_state.progress.store(1.f);
			g_state.capturing.store(false);
			return;
		}

		std::string persistence_error;
		const bool persisted = detail::save_snapshot(snap, persistence_error);
		if (!persisted) {
			std::lock_guard<std::mutex> lock(g_state.mutex);
			g_state.last_error = persistence_error;
		}

		std::size_t region_count = snap.regions.size();
		uint64_t total_bytes = snap.total_bytes;
		{
			std::lock_guard<std::mutex> lk(g_state.mutex);
			if (g_state.snapshots.size() >= 10) {
			uint64_t evicted_id = g_state.snapshots.front()->id;
				g_state.snapshots.erase(g_state.snapshots.begin());
				if (g_state.snap_a_id == evicted_id) g_state.snap_a_id = 0;
				if (g_state.snap_b_id == evicted_id) g_state.snap_b_id = 0;
				diag::log_tagged_fmt("snapshot_diff", "take_snapshot evicted_oldest id=%llu",
					static_cast<unsigned long long>(evicted_id));
			}
			g_state.snapshots.push_back(std::make_shared<const snapshot_t>(std::move(snap)));
		}

		auto t_end = std::chrono::steady_clock::now();
		uint64_t dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
		diag::log_tagged_fmt("snapshot_diff", "take_snapshot done name='%s' regions=%zu bytes=%llu duration_ms=%llu cancelled=%d",
			snap_name.c_str(), region_count,
			static_cast<unsigned long long>(total_bytes),
			static_cast<unsigned long long>(dur_ms),
			static_cast<int>(g_state.cancel.load()));
		if (!persisted)
			diag::log_tagged_fmt("snapshot_diff", "take_snapshot persistence_failed err='%s'",
				persistence_error.c_str());
		if (bounded)
			diag::log_tagged_fmt("snapshot_diff", "take_snapshot bounded bytes=%llu limit=%llu",
				static_cast<unsigned long long>(total_bytes),
				static_cast<unsigned long long>(detail::maximum_snapshot_bytes));

		g_state.progress.store(1.f);
		g_state.capturing.store(false);
	};
	const auto submitted = aida::infra::executor::submit(std::move(sub));
	if (!submitted.submitted) {
		diag::log_tagged("snapshot_diff", "take_snapshot worker_queue_rejected");
		set_last_error("take_snapshot: worker queue rejected the task");
		g_state.progress.store(1.f);
		g_state.capturing.store(false);
		return;
	}
	scanner_task_center::register_executor_task(submitted,
		"view.memory.snapshot_diff", "memory.capture_snapshot", "Capture memory snapshot",
		driver_bridge::attached_pid(), true, []() {
			g_state.cancel.store(true, std::memory_order_release);
			return true;
		});
}

inline void load_from_disk(const std::string& path)
{
	if (g_state.loading.load() || g_state.capturing.load()) {
		diag::log_tagged_fmt("snapshot_diff", "load_from_disk refused busy loading=%d capturing=%d",
			static_cast<int>(g_state.loading.load()),
			static_cast<int>(g_state.capturing.load()));
		return;
	}

	g_state.loading.store(true);
	g_state.cancel.store(false);
	g_state.progress.store(0.f);
	const std::uint64_t operation_generation =
		g_state.operation_generation.fetch_add(1, std::memory_order_acq_rel) + 1;

	std::string fallback_name;
	{
		++g_state.snap_counter;
		char buf[32];
		snprintf(buf, sizeof(buf), "Snap%d", g_state.snap_counter);
		fallback_name = buf;
	}

	diag::log_tagged_fmt("snapshot_diff", "load_from_disk start path='%s'", path.c_str());

	std::string path_copy = path;
	aida::infra::executor::submission_t sub;
	sub.owner_subsystem = "scanner";
	sub.label = "scanner.snapshot_load";
	sub.thread_class = "scanner_snapshot";
	sub.domain = aida::infra::executor::domain_t::feature_worker;
	sub.priority = 3;
	sub.generation = operation_generation;
	sub.body = [path_copy, fallback_name, operation_generation]() {
		auto t_start = std::chrono::steady_clock::now();
		snapshot_t snap;
		std::string load_error;
		if (!detail::load_snapshot(path_copy, snap, load_error)) {
			{
				std::lock_guard<std::mutex> lock(g_state.mutex);
				g_state.last_error = load_error;
			}
			diag::log_tagged_fmt("snapshot_diff", "load_from_disk failed path='%s' err='%s'",
				path_copy.c_str(), load_error.c_str());
			g_state.progress.store(1.f);
			g_state.loading.store(false);
			return;
		}
		if (g_state.cancel.load(std::memory_order_acquire) ||
			g_state.operation_generation.load(std::memory_order_acquire) != operation_generation) {
			g_state.progress.store(1.f);
			g_state.loading.store(false);
			return;
		}

		snap.id = g_state.next_snap_id.fetch_add(1);
		if (snap.name.empty()) {
			snap.name = fallback_name;
		}

		std::size_t region_count = snap.regions.size();
		uint64_t total_bytes = snap.total_bytes;
		std::string snap_name = snap.name;
		{
			std::lock_guard<std::mutex> lk(g_state.mutex);
			if (g_state.snapshots.size() >= 10) {
				uint64_t evicted_id = g_state.snapshots.front()->id;
				g_state.snapshots.erase(g_state.snapshots.begin());
				if (g_state.snap_a_id == evicted_id) g_state.snap_a_id = 0;
				if (g_state.snap_b_id == evicted_id) g_state.snap_b_id = 0;
				diag::log_tagged_fmt("snapshot_diff", "load_from_disk evicted_oldest id=%llu",
					static_cast<unsigned long long>(evicted_id));
			}
			g_state.snapshots.push_back(std::make_shared<const snapshot_t>(std::move(snap)));
		}

		auto t_end = std::chrono::steady_clock::now();
		uint64_t dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
		diag::log_tagged_fmt("snapshot_diff", "load_from_disk done path='%s' name='%s' regions=%zu bytes=%llu duration_ms=%llu",
			path_copy.c_str(), snap_name.c_str(), region_count,
			static_cast<unsigned long long>(total_bytes),
			static_cast<unsigned long long>(dur_ms));

		g_state.progress.store(1.f);
		g_state.loading.store(false);
	};
	const auto submitted = aida::infra::executor::submit(std::move(sub));
	if (!submitted.submitted) {
		diag::log_tagged("snapshot_diff", "load_from_disk worker_queue_rejected");
		set_last_error("load_from_disk: worker queue rejected the task");
		g_state.progress.store(1.f);
		g_state.loading.store(false);
		return;
	}
	scanner_task_center::register_executor_task(submitted,
		"view.memory.snapshot_diff", "memory.load_snapshot", "Load memory snapshot",
		driver_bridge::attached_pid());
}

inline void compare_snapshots(uint64_t id_a, uint64_t id_b)
{
	if (g_state.comparing.load()) {
		diag::log_tagged("snapshot_diff", "compare_snapshots refused already_comparing");
		return;
	}
	if (id_a == 0 || id_b == 0 || id_a == id_b) {
		diag::log_tagged_fmt("snapshot_diff", "compare_snapshots refused invalid_ids a=%llu b=%llu",
			static_cast<unsigned long long>(id_a),
			static_cast<unsigned long long>(id_b));
		return;
	}

	diag::log_tagged_fmt("snapshot_diff", "compare_snapshots start a=%llu b=%llu",
		static_cast<unsigned long long>(id_a),
		static_cast<unsigned long long>(id_b));

	g_state.comparing.store(true);
	g_state.cancel.store(false);
	g_state.progress.store(0.f);
	const std::uint64_t operation_generation =
		g_state.operation_generation.fetch_add(1, std::memory_order_acq_rel) + 1;

	aida::infra::executor::submission_t sub;
	sub.owner_subsystem = "scanner";
	sub.label = "scanner.snapshot_compare";
	sub.thread_class = "scanner_snapshot_compare";
	sub.domain = aida::infra::executor::domain_t::long_running;
	sub.priority = 2;
	sub.target_pid = driver_bridge::attached_pid();
	sub.generation = operation_generation;
	sub.body = [id_a, id_b, operation_generation]() {
		try {
		auto t_start = std::chrono::steady_clock::now();
		diff_result_t result;

		std::shared_ptr<const snapshot_t> snap_a;
		std::shared_ptr<const snapshot_t> snap_b;
		{
			std::lock_guard<std::mutex> lk(g_state.mutex);
			for (const auto& s : g_state.snapshots) {
				if (s->id == id_a) snap_a = s;
				if (s->id == id_b) snap_b = s;
			}
			if (!snap_a || !snap_b) {
				g_state.last_error = (!snap_a && !snap_b)
					? "compare_snapshots: both snapshots evicted"
					: (!snap_a
						? "compare_snapshots: snapshot A evicted"
						: "compare_snapshots: snapshot B evicted");
				g_state.comparing.store(false);
				return;
			}
		}

		result.snap_a_name = snap_a->name;
		result.snap_b_name = snap_b->name;

		std::unordered_map<uint64_t, std::size_t> b_map;
		for (std::size_t i = 0; i < snap_b->regions.size(); ++i)
			b_map[snap_b->regions[i].base] = i;

		auto modules = driver_bridge::enumerate_modules();
		std::sort(modules.begin(), modules.end(),
			[](const auto& left, const auto& right) { return left.base < right.base; });

		uint64_t total = snap_a->regions.size();
		if (total == 0) total = 1;
		uint64_t done = 0;

		for (const auto& ra : snap_a->regions) {
			if (g_state.cancel.load()) break;

			auto it = b_map.find(ra.base);
			if (it == b_map.end()) {
				++done;
				g_state.progress.store(static_cast<float>(done) / static_cast<float>(total));
				continue;
			}

			const auto& rb = snap_b->regions[it->second];
			uint64_t cmp_size = (std::min)(ra.size, rb.size);
			uint64_t min_data = (std::min)(ra.data.size(), rb.data.size());
			if (min_data < cmp_size) cmp_size = min_data;

			uint64_t i = 0;
			while (i < cmp_size) {
				if ((i & 0xFFFFF) == 0 && g_state.cancel.load()) break;
				if (ra.data[i] == rb.data[i]) { ++i; continue; }

				uint64_t start = i;
				while (i < cmp_size && ra.data[i] != rb.data[i] && (i - start) < 64)
					++i;

				changed_region_t cr;
				cr.address = ra.base + start;
				cr.size = static_cast<uint32_t>(i - start);
				cr.old_data.assign(ra.data.begin() + start, ra.data.begin() + i);
				cr.new_data.assign(rb.data.begin() + start, rb.data.begin() + i);
				cr.type = detail::classify_change(cr.old_data.data(), cr.new_data.data(), cr.size);

				const auto module_after = std::upper_bound(modules.begin(), modules.end(), cr.address,
					[](std::uint64_t address, const auto& module) { return address < module.base; });
				if (module_after != modules.begin()) {
					const auto& module = *std::prev(module_after);
					if (cr.address >= module.base && cr.address - module.base < module.size)
						cr.module_name = module.name;
				}

				const std::uint32_t changed_size = cr.size;
				result.changes.push_back(std::move(cr));
				result.total_changed_bytes += changed_size;
				if (result.changes.size() >= detail::maximum_diff_ranges) {
					result.truncated = true;
					break;
				}
			}

			++result.changed_page_count;
			++done;
			g_state.progress.store(static_cast<float>(done) / static_cast<float>(total));
			if (result.truncated) break;
		}

		std::size_t changes_count = result.changes.size();
		uint64_t changed_bytes = result.total_changed_bytes;
		std::size_t changed_pages = result.changed_page_count;
		if (g_state.cancel.load(std::memory_order_acquire) ||
			g_state.operation_generation.load(std::memory_order_acquire) != operation_generation) {
			g_state.progress.store(1.f);
			g_state.comparing.store(false);
			return;
		}
		{
			std::lock_guard<std::mutex> lk(g_state.mutex);
			g_state.published_diff = std::make_shared<const diff_result_t>(std::move(result));
		}

		auto t_end = std::chrono::steady_clock::now();
		uint64_t dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
		diag::log_tagged_fmt("snapshot_diff", "compare_snapshots done a=%llu b=%llu changes=%zu changed_pages=%zu changed_bytes=%llu duration_ms=%llu cancelled=%d",
			static_cast<unsigned long long>(id_a),
			static_cast<unsigned long long>(id_b),
			changes_count, changed_pages,
			static_cast<unsigned long long>(changed_bytes),
			static_cast<unsigned long long>(dur_ms),
			static_cast<int>(g_state.cancel.load()));

		g_state.progress.store(1.f);
		g_state.comparing.store(false);
		} catch (const std::exception& ex) {
			diag::log_tagged_fmt("snapshot_diff", "compare_snapshots worker_exception err='%s'", ex.what());
			set_last_error(ex.what());
			g_state.progress.store(1.f);
			g_state.comparing.store(false);
		} catch (...) {
			diag::log_tagged("snapshot_diff", "compare_snapshots worker_exception err='<unknown>'");
			set_last_error("compare_snapshots: worker threw an unknown exception");
			g_state.progress.store(1.f);
			g_state.comparing.store(false);
		}
	};
	const auto submitted = aida::infra::executor::submit(std::move(sub));
	if (!submitted.submitted) {
		diag::log_tagged("snapshot_diff", "compare_snapshots worker_queue_rejected");
		set_last_error("compare_snapshots: worker queue rejected the task");
		g_state.progress.store(1.f);
		g_state.comparing.store(false);
		return;
	}
	scanner_task_center::register_executor_task(submitted,
		"view.memory.snapshot_diff", "memory.compare_snapshots", "Compare memory snapshots",
		driver_bridge::attached_pid(), true, []() {
			g_state.cancel.store(true, std::memory_order_release);
			return true;
		});
}

inline void clear_snapshots()
{
	diag::log_tagged("snapshot_diff", "clear_snapshots signalled");
	g_state.cancel.store(true);
	g_state.operation_generation.fetch_add(1, std::memory_order_acq_rel);
	std::lock_guard<std::mutex> lk(g_state.mutex);
	std::size_t had = g_state.snapshots.size();
	g_state.snapshots.clear();
	g_state.published_diff = std::make_shared<const diff_result_t>();
	g_state.snap_a_id = 0;
	g_state.snap_b_id = 0;
	g_state.snap_counter = 0;
	diag::log_tagged_fmt("snapshot_diff", "clear_snapshots cleared=%zu", had);
}

}

