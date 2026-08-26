#include "test_lab_controller.hpp"

#include "../test_lab_format.hpp"
#include "../test_all_features.hpp"

#include "../../infra/executor.hpp"
#include "../../../helpers/diag_log.hpp"
#include "../../../../../driver/comm.h"

#include <QTimer>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

namespace aida::qt::testlab {

namespace {

	constexpr std::size_t k_log_tail_max_lines = 96;
	constexpr int k_poll_interval_ms = 100;

	struct full_test_scope_t {
		const char* source = nullptr;
		bool active = false;

		explicit full_test_scope_t(const char* s) : source(s), active(true) {
			test_all_features::begin_test_guard(source);
		}

		~full_test_scope_t() {
			if (active)
				test_all_features::end_test_guard(source);
		}

		full_test_scope_t(const full_test_scope_t&) = delete;
		full_test_scope_t& operator=(const full_test_scope_t&) = delete;
	};

	const char* run_all_log_path() {
		static const std::string path = []() {
			char buf[MAX_PATH] = {};
			if (diag::build_log_path("aida_test_results.log", buf, sizeof(buf)))
				return std::string(buf);
			return std::string();
		}();
		return path.c_str();
	}

	void populate_safe_defaults(test_lab::state_t& s) {
		s.pid = static_cast<std::uint32_t>(GetCurrentProcessId());
		s.tid = static_cast<std::uint32_t>(GetCurrentThreadId());
		s.addr = 0;
		s.size = 64;
		s.u32_a = 0;
		s.u32_b = 0;
		s.u64_a = 0;
		s.buf.clear();
		s.text_a = "ntdll.dll";
		s.text_b = "";
		s.user = nullptr;
	}

	void format_local_timestamp(char* out, std::size_t cap) {
		SYSTEMTIME st;
		GetLocalTime(&st);
		std::snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
			static_cast<unsigned>(st.wYear),
			static_cast<unsigned>(st.wMonth),
			static_cast<unsigned>(st.wDay),
			static_cast<unsigned>(st.wHour),
			static_cast<unsigned>(st.wMinute),
			static_cast<unsigned>(st.wSecond),
			static_cast<unsigned>(st.wMilliseconds));
	}

	const char* driver_name(test_lab::driver_e d) {
		switch (d) {
			case test_lab::driver_e::whoswho:  return "whoswho";
			case test_lab::driver_e::driverless: return "driverless";
		}
		return "unknown";
	}

	void flush_run_all_log(HANDLE hFile) {
		if (hFile != INVALID_HANDLE_VALUE)
			FlushFileBuffers(hFile);
	}

	HANDLE open_log_for_append() {
		return CreateFileA(
			run_all_log_path(),
			FILE_APPEND_DATA | SYNCHRONIZE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
	}

	bool name_starts_with(const char* name, const char* prefix) {
		if (name == nullptr || prefix == nullptr) return false;
		std::size_t i = 0;
		while (prefix[i] != '\0') {
			if (name[i] == '\0' || name[i] != prefix[i]) return false;
			++i;
		}
		return true;
	}

	struct run_all_cache_t {
		std::uint64_t dtb = 0;
		std::uint64_t base = 0;
		std::uint64_t alloc_addr = 0;
		bool sandbox_self_registered = false;
	};

	std::uint64_t resolve_ntdll_base() {
		HMODULE h = GetModuleHandleW(L"ntdll.dll");
		if (h == nullptr) return 0;
		return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(h));
	}

	void prime_run_all_cache(run_all_cache_t& cache) {
		cache.dtb = 0;
		cache.base = 0;
		cache.alloc_addr = 0;
		cache.sandbox_self_registered = false;
		if (!device || !device->is_connected()) return;
		std::uint32_t self_pid = static_cast<std::uint32_t>(GetCurrentProcessId());
		device->set_process_id(self_pid);
		device->solve_dtb();
		cache.dtb = device->get_dtb();
		cache.base = device->get_base_address();
		std::uint64_t alloc_va = device->allocate_memory(0x1000);
		if (alloc_va != 0) cache.alloc_addr = alloc_va;
		diag::log_tagged_fmt("testlab",
			"run-all cache primed: pid=%u dtb=0x%016llX base=0x%016llX alloc_addr=0x%016llX",
			static_cast<unsigned>(self_pid),
			static_cast<unsigned long long>(cache.dtb),
			static_cast<unsigned long long>(cache.base),
			static_cast<unsigned long long>(cache.alloc_addr));
	}

	void apply_smart_defaults(const test_lab::feature_t& f,
		test_lab::state_t& s,
		run_all_cache_t& cache)
	{
		const char* name = f.name;
		if (name == nullptr) return;

		if (name_starts_with(name, "PHYS") ||
			name_starts_with(name, "V2P"))
		{
			s.u64_a = cache.dtb;
			if (s.addr == 0) {
				if (cache.base != 0) {
					s.addr = cache.base;
				} else {
					HMODULE h_self = GetModuleHandleW(nullptr);
					if (h_self != nullptr) {
						s.addr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(h_self));
					}
				}
			}
			if (s.size == 0) s.size = 256;
			return;
		}
		if (name_starts_with(name, "DPIN")) {
			s.u64_a = cache.dtb;
			return;
		}
		if (name_starts_with(name, "MEX")) {
			s.u64_a = cache.dtb;
			std::uint64_t mex_base = resolve_ntdll_base();
			if (mex_base == 0) mex_base = cache.base;
			if (mex_base == 0) {
				HMODULE h_self = GetModuleHandleW(nullptr);
				if (h_self != nullptr) {
					mex_base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(h_self));
				}
			}
			if (mex_base != 0) s.addr = mex_base;
			if (s.text_a.empty() || s.text_a == "ntdll.dll") s.text_a = "ntdll.dll";
			s.text_b = "NtClose";
			return;
		}
		if (name_starts_with(name, "FM")) {
			if (cache.alloc_addr != 0) {
				s.addr = cache.alloc_addr;
			}
			s.size = 64;
			return;
		}
		if (name_starts_with(name, "PMOD")) {
			s.text_a = "tag1|0|6|80|0|DEAD|BEEF";
			s.u32_a = 0;
			return;
		}
		if (name_starts_with(name, "PM")) {
			if (cache.alloc_addr != 0) {
				s.addr = cache.alloc_addr;
			}
			s.size = 64;
			s.u32_a = 0x04u;
			return;
		}
		if (name_starts_with(name, "PRED") || name_starts_with(name, "CKIL")) {
			s.text_a = "tcp://127.0.0.1:80";
			s.text_b = "tcp://127.0.0.1:443";
			s.u32_a = 0;
			return;
		}
		if (name_starts_with(name, "DNSS")) {
			s.text_a = "test.example.com";
			s.text_b = "127.0.0.1";
			s.u32_a = 0;
			return;
		}
		if (name_starts_with(name, "PCEX")) {
			char tmp[MAX_PATH];
			GetTempPathA(MAX_PATH, tmp);
			s.text_a = std::string(tmp) + "aida_test_capture.pcap";
			return;
		}
		if (name_starts_with(name, "CR")) {
			s.u32_a = 0;
			return;
		}
		if (name_starts_with(name, "STRM")) {
			s.u64_a = (static_cast<std::uint64_t>(80u) << 16) | static_cast<std::uint64_t>(443u);
			return;
		}
		if (name_starts_with(name, "PSBX")) {
			return;
		}
		if (name_starts_with(name, "USBX")) {
			if (!cache.sandbox_self_registered && device && device->is_connected()) {
				std::uint64_t denials = 0;
				if (device->protect_sandbox_pid(s.pid, 0, &denials)) {
					cache.sandbox_self_registered = true;
				}
			}
			return;
		}
		if (name_starts_with(name, "NLOG")) {
			if (!cache.sandbox_self_registered && device && device->is_connected()) {
				std::uint64_t denials = 0;
				if (device->protect_sandbox_pid(s.pid, 0, &denials)) {
					cache.sandbox_self_registered = true;
				}
			}
			s.u32_a = 1u;
			return;
		}
	}

	bool result_fields_equal(const test_lab::result_t& a, const test_lab::result_t& b) {
		if (a.state.load(std::memory_order_acquire) != b.state.load(std::memory_order_acquire)) return false;
		if (a.outcome != b.outcome || a.ok != b.ok || a.skipped != b.skipped) return false;
		if (a.ntstatus != b.ntstatus || a.bytes_returned != b.bytes_returned) return false;
		if (a.elapsed_us != b.elapsed_us || a.error != b.error) return false;
		if (a.raw != b.raw) return false;
		if (a.parsed.size() != b.parsed.size()) return false;
		for (std::size_t i = 0; i < a.parsed.size(); ++i) {
			if (a.parsed[i].label != b.parsed[i].label || a.parsed[i].value != b.parsed[i].value) return false;
		}
		return true;
	}

	void copy_result_fields(const test_lab::result_t& src, test_lab::result_t& dst) {
		dst.state.store(src.state.load(std::memory_order_acquire), std::memory_order_release);
		dst.outcome = src.outcome;
		dst.ok = src.ok;
		dst.skipped = src.skipped;
		dst.ntstatus = src.ntstatus;
		dst.bytes_returned = src.bytes_returned;
		dst.elapsed_us = src.elapsed_us;
		dst.error = src.error;
		dst.raw = src.raw;
		dst.parsed = src.parsed;
	}

}

TestLabController* TestLabController::instance() {
	static TestLabController* g_instance = nullptr;
	if (g_instance == nullptr)
		g_instance = new TestLabController();
	return g_instance;
}

TestLabController::TestLabController(QObject* parent) : QObject(parent) {
	poll_timer_ = new QTimer(this);
	poll_timer_->setInterval(k_poll_interval_ms);
	poll_timer_->setTimerType(Qt::CoarseTimer);
	connect(poll_timer_, &QTimer::timeout, this, [this]() { pollSnapshots(); });
	poll_timer_->start();
	pollSnapshots();
}

const test_lab::feature_t* TestLabController::featureAt(int index) const {
	const auto& features = test_lab::all_features();
	if (index < 0 || index >= static_cast<int>(features.size())) return nullptr;
	return &features[static_cast<std::size_t>(index)];
}

std::string TestLabController::runAllLogPath() const {
	return run_all_log_path();
}

std::uint64_t TestLabController::nowMs() const {
	return static_cast<std::uint64_t>(GetTickCount64());
}

void TestLabController::logRenderLockBusy(const char* site, const char* lock_name) {
	static std::atomic<unsigned long long> s_last_busy_log_ms{0};
	const unsigned long long now = GetTickCount64();
	unsigned long long last = s_last_busy_log_ms.load(std::memory_order_acquire);
	if (now - last >= 1000ULL && s_last_busy_log_ms.compare_exchange_strong(last, now, std::memory_order_acq_rel)) {
		diag::log_tagged_critical_fmt("test_lab_render",
			"lock_busy site=%s lock=%s frame=%llu tid=%lu safe_run_active=%d current=%d total=%d ok=%d fail=%d skipped=%d full_test=%d",
			site ? site : "<null>",
			lock_name ? lock_name : "<null>",
			static_cast<unsigned long long>(snapshot_counter_.load(std::memory_order_acquire)),
			static_cast<unsigned long>(GetCurrentThreadId()),
			run_all_active_.load(std::memory_order_acquire) ? 1 : 0,
			run_all_current_.load(std::memory_order_acquire),
			run_all_total_.load(std::memory_order_acquire),
			run_all_ok_.load(std::memory_order_acquire),
			run_all_fail_.load(std::memory_order_acquire),
			run_all_skipped_.load(std::memory_order_acquire),
			test_all_features::is_running() ? 1 : 0);
	}
}

void TestLabController::ensureFeatureSummarySizeLocked(std::size_t feature_count) {
	if (feature_summaries_.size() != feature_count)
		feature_summaries_.assign(feature_count, feature_run_summary_t{});
}

void TestLabController::resetFeatureSummaries() {
	std::lock_guard<std::mutex> lk(feature_summary_mtx_);
	feature_summaries_.assign(test_lab::all_features().size(), feature_run_summary_t{});
}

void TestLabController::updateFeatureSummaryStart(std::size_t feature_index, std::uint64_t log_index) {
	std::lock_guard<std::mutex> lk(feature_summary_mtx_);
	const auto& features = test_lab::all_features();
	ensureFeatureSummarySizeLocked(features.size());
	if (feature_index >= feature_summaries_.size()) return;
	feature_run_summary_t& s = feature_summaries_[feature_index];
	s.state = test_lab::run_state_e::running;
	s.outcome = test_lab::outcome_e::not_run;
	s.ok = false;
	s.skipped = false;
	s.ntstatus = 0;
	s.bytes_returned = 0;
	s.elapsed_us = 0;
	s.error.clear();
	s.started_ms = nowMs();
	s.finished_ms = 0;
	if (log_index != 0) s.log_line_index = log_index;
}

void TestLabController::updateFeatureSummarySkip(std::size_t feature_index, const char* reason, std::uint64_t log_index) {
	std::lock_guard<std::mutex> lk(feature_summary_mtx_);
	const auto& features = test_lab::all_features();
	ensureFeatureSummarySizeLocked(features.size());
	if (feature_index >= feature_summaries_.size()) return;
	feature_run_summary_t& s = feature_summaries_[feature_index];
	if (s.started_ms == 0) s.started_ms = nowMs();
	s.state = test_lab::run_state_e::complete;
	s.outcome = test_lab::outcome_e::not_run;
	s.ok = false;
	s.skipped = true;
	s.ntstatus = 0;
	s.bytes_returned = 0;
	s.elapsed_us = 0;
	s.error = reason != nullptr ? reason : "skipped";
	s.finished_ms = nowMs();
	if (log_index != 0) s.log_line_index = log_index;
}

void TestLabController::updateFeatureSummaryResult(std::size_t feature_index, const test_lab::result_t& r, std::uint64_t log_index) {
	std::lock_guard<std::mutex> lk(feature_summary_mtx_);
	const auto& features = test_lab::all_features();
	ensureFeatureSummarySizeLocked(features.size());
	if (feature_index >= feature_summaries_.size()) return;
	feature_run_summary_t& s = feature_summaries_[feature_index];
	if (s.started_ms == 0) s.started_ms = nowMs();
	s.state = test_lab::run_state_e::complete;
	s.outcome = r.outcome;
	s.ok = r.ok;
	s.skipped = r.skipped;
	s.ntstatus = r.ntstatus;
	s.bytes_returned = r.bytes_returned;
	s.elapsed_us = r.elapsed_us;
	s.error = r.error;
	s.finished_ms = nowMs();
	if (log_index != 0) s.log_line_index = log_index;
}

bool TestLabController::tryCopyFeatureSummaries(const char* site, std::vector<feature_run_summary_t>& out) {
	std::unique_lock<std::mutex> lk(feature_summary_mtx_, std::try_to_lock);
	if (!lk.owns_lock()) {
		logRenderLockBusy(site, "g_feature_summary_mtx");
		return false;
	}
	ensureFeatureSummarySizeLocked(test_lab::all_features().size());
	out = feature_summaries_;
	return true;
}

std::uint64_t TestLabController::appendLogTail(const std::string& text) {
	std::lock_guard<std::mutex> lk(log_tail_mtx_);
	std::uint64_t first_index = 0;
	std::size_t pos = 0;
	while (pos <= text.size()) {
		std::size_t nl = text.find('\n', pos);
		std::size_t end = (nl == std::string::npos) ? text.size() : nl;
		std::string line = text.substr(pos, end - pos);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.size() > 900) {
			line.resize(897);
			line.append("...");
		}
		log_tail_line_t item;
		item.index = log_tail_next_index_++;
		item.text = std::move(line);
		if (first_index == 0) first_index = item.index;
		log_tail_.push_back(std::move(item));
		while (log_tail_.size() > k_log_tail_max_lines)
			log_tail_.pop_front();
		if (nl == std::string::npos) break;
		pos = nl + 1;
		if (pos == text.size()) break;
	}
	return first_index;
}

bool TestLabController::tryCopyLogTail(const char* site, std::vector<log_tail_line_t>& out) {
	std::unique_lock<std::mutex> lk(log_tail_mtx_, std::try_to_lock);
	if (!lk.owns_lock()) {
		logRenderLockBusy(site, "g_log_tail_mtx");
		return false;
	}
	out.assign(log_tail_.begin(), log_tail_.end());
	return true;
}

bool TestLabController::tryCopyResultSummary(const char* site, test_lab::run_state_e& state,
	test_lab::outcome_e& outcome, bool& ok, bool& skipped) {
	std::unique_lock<std::mutex> lk(result_mtx_, std::try_to_lock);
	if (!lk.owns_lock()) {
		logRenderLockBusy(site, "g_result_mtx");
		state = test_lab::run_state_e::idle;
		outcome = test_lab::outcome_e::not_run;
		ok = false;
		skipped = false;
		return false;
	}
	std::shared_ptr<test_lab::result_t> snap = result_;
	if (!snap) {
		state = test_lab::run_state_e::idle;
		outcome = test_lab::outcome_e::not_run;
		ok = false;
		skipped = false;
		return true;
	}
	state = snap->state.load(std::memory_order_acquire);
	outcome = snap->outcome;
	ok = snap->ok;
	skipped = snap->skipped;
	return true;
}

bool TestLabController::tryCopyResultFull(const char* site, test_lab::result_t& out) {
	std::unique_lock<std::mutex> lk(result_mtx_, std::try_to_lock);
	if (!lk.owns_lock()) {
		logRenderLockBusy(site, "g_result_mtx");
		out.state.store(test_lab::run_state_e::idle, std::memory_order_release);
		out.outcome = test_lab::outcome_e::not_run;
		out.ok = false;
		out.skipped = false;
		out.ntstatus = 0;
		out.bytes_returned = 0;
		out.elapsed_us = 0;
		out.error.clear();
		out.raw.clear();
		out.parsed.clear();
		return false;
	}
	std::shared_ptr<test_lab::result_t> snap = result_;
	if (!snap) {
		out.state.store(test_lab::run_state_e::idle, std::memory_order_release);
		out.outcome = test_lab::outcome_e::not_run;
		out.ok = false;
		out.skipped = false;
		out.ntstatus = 0;
		out.bytes_returned = 0;
		out.elapsed_us = 0;
		out.error.clear();
		out.raw.clear();
		out.parsed.clear();
		return true;
	}
	copy_result_fields(*snap, out);
	return true;
}

bool TestLabController::tryReplaceResult(const char* site, const std::shared_ptr<test_lab::result_t>& result) {
	std::unique_lock<std::mutex> lk(result_mtx_, std::try_to_lock);
	if (!lk.owns_lock()) {
		logRenderLockBusy(site, "g_result_mtx");
		return false;
	}
	result_ = result;
	return true;
}

namespace {

	std::uint64_t append_log_line(TestLabController* self, HANDLE hFile, const std::string& line, bool force_flush = false) {
		const std::uint64_t line_index = self->appendLogTail(line);
		if (hFile == INVALID_HANDLE_VALUE) return line_index;
		static std::mutex s_log_mtx;
		static std::uint64_t s_last_flush_ms = 0;
		static std::uint32_t s_bytes_since_flush = 0;
		const bool important =
			line.find(" -- FAIL") != std::string::npos ||
			line.find("FATAL") != std::string::npos ||
			line.find("CRASH") != std::string::npos ||
			line.find("BSOD") != std::string::npos ||
			line.find("Run All complete") != std::string::npos;
		std::lock_guard<std::mutex> lk(s_log_mtx);
		DWORD wrote = 0;
		WriteFile(hFile, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
		s_bytes_since_flush += wrote;
		const std::uint64_t now = static_cast<std::uint64_t>(GetTickCount64());
		if (force_flush || important || s_last_flush_ms == 0 || s_bytes_since_flush >= 65536u || now - s_last_flush_ms >= 1000u) {
			FlushFileBuffers(hFile);
			s_last_flush_ms = now;
			s_bytes_since_flush = 0;
		}
		return line_index;
	}

	std::uint64_t append_log_starting(TestLabController* self, HANDLE hFile,
		const test_lab::feature_t& f,
		const test_lab::state_t& s)
	{
		char ts[40];
		format_local_timestamp(ts, sizeof(ts));
		std::string line;
		line.reserve(256);
		line.append("[").append(ts).append("] [").append(driver_name(f.driver)).append("] ");
		line.append(f.category != nullptr ? f.category : "?").append("/");
		line.append(f.name != nullptr ? f.name : "?");
		line.append(" -- STARTING\n");
		char tmp[160];
		std::snprintf(tmp, sizeof(tmp), "    state: pid=%u tid=%u addr=0x%llX size=%u u32_a=%u\n",
			static_cast<unsigned>(s.pid),
			static_cast<unsigned>(s.tid),
			static_cast<unsigned long long>(s.addr),
			static_cast<unsigned>(s.size),
			static_cast<unsigned>(s.u32_a));
		line.append(tmp);
		return append_log_line(self, hFile, line);
	}

	std::uint64_t append_log_skip(TestLabController* self, HANDLE hFile, const test_lab::feature_t& f, const char* reason) {
		char ts[40];
		format_local_timestamp(ts, sizeof(ts));
		std::string line;
		line.reserve(256);
		line.append("[").append(ts).append("] [").append(driver_name(f.driver)).append("] ");
		line.append(f.category != nullptr ? f.category : "?").append("/");
		line.append(f.name != nullptr ? f.name : "?");
		line.append(" -- SKIPPED (").append(reason != nullptr ? reason : "no reason").append(")\n");
		return append_log_line(self, hFile, line);
	}

	std::uint64_t append_log_result(TestLabController* self, HANDLE hFile,
		const test_lab::feature_t& f,
		const test_lab::state_t& s,
		const test_lab::result_t& r,
		std::uint64_t elapsed_us)
	{
		char ts[40];
		format_local_timestamp(ts, sizeof(ts));
		std::string line;
		line.reserve(1024);
		line.append("[").append(ts).append("] [").append(driver_name(f.driver)).append("] ");
		line.append(f.category != nullptr ? f.category : "?").append("/");
		line.append(f.name != nullptr ? f.name : "?");
		line.append(" -- ").append(test_lab_format::testlab_outcome_name(
			test_lab::effective_outcome(r, f.driver == test_lab::driver_e::driverless)));
		char tmp[64];
		std::snprintf(tmp, sizeof(tmp), " ntstatus=%s bytes=%u elapsed_us=%llu\n",
			test_lab_format::ntstatus_to_string(r.ntstatus),
			static_cast<unsigned>(r.bytes_returned),
			static_cast<unsigned long long>(elapsed_us));
		line.append(tmp);

		std::snprintf(tmp, sizeof(tmp), "    state: pid=%u tid=%u addr=0x%llX size=%u u32_a=%u\n",
			static_cast<unsigned>(s.pid),
			static_cast<unsigned>(s.tid),
			static_cast<unsigned long long>(s.addr),
			static_cast<unsigned>(s.size),
			static_cast<unsigned>(s.u32_a));
		line.append(tmp);

		if ((!r.ok || r.skipped) && !r.error.empty()) {
			line.append("    error: ").append(r.error).append("\n");
		}
		for (const auto& p : r.parsed) {
			line.append("    ").append(p.label).append(": ").append(p.value).append("\n");
		}
		if (!r.raw.empty()) {
			std::size_t limit = r.raw.size();
			if (limit > 64) limit = 64;
			line.append("    raw[0..");
			std::snprintf(tmp, sizeof(tmp), "%zu]: ", limit);
			line.append(tmp);
			for (std::size_t i = 0; i < limit; ++i) {
				std::snprintf(tmp, sizeof(tmp), "%02X ", static_cast<unsigned>(r.raw[i]));
				line.append(tmp);
			}
			line.append("\n");
		}
		return append_log_line(self, hFile, line);
	}

}

void TestLabController::selectFeature(int idx) {
	if (idx == selected_idx_) return;
	selected_idx_ = idx;
	tryReplaceResult("left_pane_select_reset", std::make_shared<test_lab::result_t>());
	Q_EMIT selectionChanged(idx);
	pollSnapshots();
}

void TestLabController::clearResult() {
	tryReplaceResult("action_row_clear", std::make_shared<test_lab::result_t>());
	pollSnapshots();
}

void TestLabController::runSelectedFeature() {
	const test_lab::feature_t* f = featureAt(selected_idx_);
	if (f == nullptr) return;
	runFnPostWithFeature(*f, selected_idx_);
	pollSnapshots();
}

void TestLabController::startRunAllSafe() {
	startRunAllSafeImpl();
	pollSnapshots();
}

void TestLabController::notifyFromWorker() {
	QMetaObject::invokeMethod(this, [this]() { pollSnapshots(); }, Qt::QueuedConnection);
}

void TestLabController::runFnPostWithFeature(const test_lab::feature_t& f, int feature_idx) {
	test_lab::state_t snapshot = state_;
	std::shared_ptr<test_lab::result_t> new_result = std::make_shared<test_lab::result_t>();
	new_result->state.store(test_lab::run_state_e::running, std::memory_order_release);
	if (!tryReplaceResult("single_feature_start", new_result)) {
		new_result->ok = false;
		new_result->outcome = test_lab::outcome_e::failed;
		new_result->error = "result lock busy";
		new_result->state.store(test_lab::run_state_e::complete, std::memory_order_release);
		if (feature_idx >= 0)
			updateFeatureSummaryResult(static_cast<std::size_t>(feature_idx), *new_result, 0);
		return;
	}
	if (feature_idx >= 0)
		updateFeatureSummaryStart(static_cast<std::size_t>(feature_idx), 0);
	test_lab::feature_t feature_copy = f;
	aida::infra::executor::submission_t submission;
	submission.owner_subsystem = "test_lab_view";
	submission.label = "test_lab_view.single_feature";
	submission.thread_class = "testlab_feature";
	submission.domain = aida::infra::executor::domain_t::feature_worker;
	submission.priority = 2;
	submission.diagnostic_id = feature_copy.name ? feature_copy.name : "unnamed";
	submission.failure_policy = "reject_not_started";
	submission.body = [this, feature_copy, snapshot, new_result, feature_idx]() mutable {
		full_test_scope_t full_test_scope("test_lab_view_single_feature");
		if (feature_copy.run == nullptr) {
			new_result->ok = false;
			new_result->outcome = test_lab::outcome_e::failed;
			new_result->error = "no run function";
			test_lab_format::testlab_diag_log_skip(feature_copy, "no run function");
			new_result->state.store(test_lab::run_state_e::complete, std::memory_order_release);
			if (feature_idx >= 0)
				updateFeatureSummaryResult(static_cast<std::size_t>(feature_idx), *new_result, 0);
			notifyFromWorker();
			return;
		}
		test_lab_format::testlab_diag_log_entry(feature_copy, snapshot);
		auto t0 = std::chrono::steady_clock::now();
		test_lab::result_t local;
		local.state.store(test_lab::run_state_e::running, std::memory_order_release);
		feature_copy.run(snapshot, local);
		if (feature_copy.driver != test_lab::driver_e::driverless) test_lab::normalize_legacy_result(local);
		auto t1 = std::chrono::steady_clock::now();
		std::uint64_t elapsed_us = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
		if (local.elapsed_us == 0) local.elapsed_us = elapsed_us;
		test_lab_format::testlab_diag_log_exit(feature_copy, local, local.elapsed_us);
		{
			std::lock_guard<std::mutex> lk(result_mtx_);
			new_result->ok = local.ok;
			new_result->outcome = local.outcome;
			new_result->skipped = local.skipped;
			new_result->ntstatus = local.ntstatus;
			new_result->bytes_returned = local.bytes_returned;
			new_result->elapsed_us = local.elapsed_us;
			new_result->error = std::move(local.error);
			new_result->raw = std::move(local.raw);
			new_result->parsed = std::move(local.parsed);
		}
		new_result->state.store(test_lab::run_state_e::complete, std::memory_order_release);
		if (feature_idx >= 0)
			updateFeatureSummaryResult(static_cast<std::size_t>(feature_idx), *new_result, 0);
		notifyFromWorker();
	};
	if (!aida::infra::executor::submit(std::move(submission)).submitted) {
		new_result->ok = false;
		new_result->outcome = test_lab::outcome_e::failed;
		new_result->error = "executor unavailable";
		new_result->state.store(test_lab::run_state_e::complete, std::memory_order_release);
		if (feature_idx >= 0)
			updateFeatureSummaryResult(static_cast<std::size_t>(feature_idx), *new_result, 0);
		diag::log_tagged_fmt("test_lab", "single_feature executor submit failed name=%s",
			feature_copy.name ? feature_copy.name : "?");
	}
}

void TestLabController::startRunAllSafeImpl() {
	bool expected = false;
	if (!run_all_active_.compare_exchange_strong(expected, true)) return;
	run_all_current_.store(0);
	run_all_ok_.store(0);
	run_all_fail_.store(0);
	run_all_skipped_.store(0);
	resetFeatureSummaries();
	{
		std::unique_lock<std::mutex> lk(run_all_status_mtx_, std::try_to_lock);
		if (lk.owns_lock()) {
			run_all_status_line_ = "starting...";
			run_all_current_name_.clear();
		} else {
			logRenderLockBusy("start_run_all_safe_status", "g_run_all_status_mtx");
		}
	}

	bool posted = false;
	try {
		aida::infra::executor::submission_t submission;
		submission.owner_subsystem = "test_lab_view";
		submission.label = "test_lab_view.run_all_safe";
		submission.thread_class = "testlab_view_run_all";
		submission.domain = aida::infra::executor::domain_t::long_running;
		submission.priority = 2;
		submission.failure_policy = "reject_not_started";
		submission.body = [this]() {
		full_test_scope_t full_test_scope("test_lab_view_run_all");
		const auto& features = test_lab::all_features();
		run_all_total_.store(static_cast<int>(features.size()));

		HANDLE hFile = open_log_for_append();
		if (hFile != INVALID_HANDLE_VALUE) {
			char ts[40];
			format_local_timestamp(ts, sizeof(ts));
			char header[256];
			std::snprintf(header, sizeof(header),
				"\n========================================\n"
				"[%s] Run All Safe Tests started (total=%d)\n"
				"========================================\n",
				ts, static_cast<int>(features.size()));
			append_log_line(this, hFile, std::string(header), true);
		}

		run_all_cache_t cache;
		prime_run_all_cache(cache);

		for (std::size_t i = 0; i < features.size(); ++i) {
			const auto& f = features[i];
			run_all_current_.store(static_cast<int>(i + 1));
			{
				std::lock_guard<std::mutex> lk(run_all_status_mtx_);
				run_all_current_name_ = (f.name != nullptr ? f.name : "?");
			}
			updateFeatureSummaryStart(i, 0);

			const char* destructive_reason = test_lab::destructive_guard_reason(f.category, f.name);
			if (destructive_reason != nullptr) {
				run_all_skipped_.fetch_add(1);
				std::string reason = std::string("destructive guard: ") + destructive_reason;
				std::uint64_t log_index = append_log_skip(this, hFile, f, reason.c_str());
				updateFeatureSummarySkip(i, reason.c_str(), log_index);
				test_lab_format::testlab_diag_log_skip(f, reason.c_str());
				continue;
			}
			if (f.run == nullptr) {
				run_all_skipped_.fetch_add(1);
				std::uint64_t log_index = append_log_skip(this, hFile, f, "no run function");
				updateFeatureSummarySkip(i, "no run function", log_index);
				test_lab_format::testlab_diag_log_skip(f, "no run function");
				continue;
			}

			test_lab::state_t s;
			populate_safe_defaults(s);
			apply_smart_defaults(f, s, cache);
			test_lab::result_t r;
			std::uint64_t start_log_index = append_log_starting(this, hFile, f, s);
			updateFeatureSummaryStart(i, start_log_index);
			test_lab_format::testlab_diag_log_entry(f, s);
			auto t0 = std::chrono::steady_clock::now();
			f.run(s, r);
			if (f.driver != test_lab::driver_e::driverless) test_lab::normalize_legacy_result(r);
			auto t1 = std::chrono::steady_clock::now();
			std::uint64_t elapsed_us = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
			if (r.elapsed_us == 0) r.elapsed_us = elapsed_us;

			if (r.skipped) run_all_skipped_.fetch_add(1);
			else if (r.ok) run_all_ok_.fetch_add(1);
			else           run_all_fail_.fetch_add(1);

			std::uint64_t result_log_index = append_log_result(this, hFile, f, s, r, r.elapsed_us);
			updateFeatureSummaryResult(i, r, result_log_index);
			test_lab_format::testlab_diag_log_exit(f, r, r.elapsed_us);
		}

		if (hFile != INVALID_HANDLE_VALUE) {
			char ts2[40];
			format_local_timestamp(ts2, sizeof(ts2));
			char footer[256];
			std::snprintf(footer, sizeof(footer),
				"[%s] Run All complete: ok=%d fail=%d skipped=%d total=%d\n"
				"========================================\n\n",
				ts2,
				run_all_ok_.load(),
				run_all_fail_.load(),
				run_all_skipped_.load(),
				run_all_total_.load());
			append_log_line(this, hFile, std::string(footer), true);
			flush_run_all_log(hFile);
			CloseHandle(hFile);
		}

		{
			std::lock_guard<std::mutex> lk(run_all_status_mtx_);
			char buf[160];
			std::snprintf(buf, sizeof(buf),
				"done: ok=%d fail=%d skipped=%d (log on Desktop)",
				run_all_ok_.load(),
				run_all_fail_.load(),
				run_all_skipped_.load());
			run_all_status_line_ = buf;
			run_all_current_name_.clear();
		}
		run_all_active_.store(false);
		notifyFromWorker();
		};
		posted = aida::infra::executor::submit(std::move(submission)).submitted;
	} catch (const std::exception& ex) {
		diag::log_tagged_fmt("test_lab", "run_all_safe executor submit exception: %s", ex.what());
	} catch (...) {
		diag::log_tagged("test_lab", "run_all_safe executor submit unknown exception");
	}
	if (!posted) {
		run_all_active_.store(false, std::memory_order_release);
		{
			std::unique_lock<std::mutex> lk(run_all_status_mtx_, std::try_to_lock);
			if (lk.owns_lock()) {
				run_all_status_line_ = "start failed: executor unavailable";
				run_all_current_name_.clear();
			} else {
				logRenderLockBusy("start_run_all_safe_post_failed_status", "g_run_all_status_mtx");
			}
		}
		diag::log_tagged("test_lab", "run_all_safe executor submit failed");
	}
}

void TestLabController::pollSnapshots() {
	if (poll_active_) return;
	poll_active_ = true;
	snapshot_counter_.fetch_add(1, std::memory_order_acq_rel);

	std::vector<feature_run_summary_t> summaries;
	if (tryCopyFeatureSummaries("poll_summary_snapshot", summaries)) {
		summaries_busy_ = false;
		if (cached_summaries_ != summaries) {
			cached_summaries_ = std::move(summaries);
			Q_EMIT featuresChanged();
		}
	} else {
		summaries_busy_ = true;
	}

	std::vector<log_tail_line_t> tail;
	if (tryCopyLogTail("poll_log_tail", tail)) {
		tail_busy_ = false;
		const std::uint64_t old_last = cached_tail_.empty() ? 0 : cached_tail_.back().index;
		const std::uint64_t new_last = tail.empty() ? 0 : tail.back().index;
		const std::uint64_t new_first = tail.empty() ? 0 : tail.front().index;
		const std::uint64_t old_first = cached_tail_.empty() ? 0 : cached_tail_.front().index;
		if (new_last != old_last || new_first != old_first) {
			cached_tail_ = std::move(tail);
			Q_EMIT logTailChanged();
		}
	} else {
		tail_busy_ = true;
	}

	test_lab::result_t fresh_result;
	if (tryCopyResultFull("poll_result_snapshot", fresh_result)) {
		result_busy_ = false;
		if (!result_fields_equal(cached_result_, fresh_result)) {
			copy_result_fields(fresh_result, cached_result_);
			Q_EMIT resultChanged();
		}
	} else {
		result_busy_ = true;
	}

	run_all_status_t rs;
	rs.active = run_all_active_.load(std::memory_order_acquire);
	rs.current = run_all_current_.load(std::memory_order_acquire);
	rs.total = run_all_total_.load(std::memory_order_acquire);
	rs.ok = run_all_ok_.load(std::memory_order_acquire);
	rs.fail = run_all_fail_.load(std::memory_order_acquire);
	rs.skipped = run_all_skipped_.load(std::memory_order_acquire);
	{
		std::unique_lock<std::mutex> lk(run_all_status_mtx_, std::try_to_lock);
		if (lk.owns_lock()) {
			rs.status_line = run_all_status_line_;
			rs.current_name = run_all_current_name_;
		} else {
			logRenderLockBusy("poll_run_all_status", "g_run_all_status_mtx");
			rs.status_line = cached_run_all_.status_line;
			rs.current_name = cached_run_all_.current_name;
		}
	}
	if (cached_run_all_ != rs) {
		cached_run_all_ = std::move(rs);
		Q_EMIT runAllChanged();
	}

	poll_active_ = false;
}

}
