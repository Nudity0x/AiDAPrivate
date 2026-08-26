#pragma once

#include "../test_lab.hpp"

#include <QObject>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class QTimer;

namespace aida::qt::testlab {

class TestLabController : public QObject {
	Q_OBJECT
public:
	struct log_tail_line_t {
		std::uint64_t index = 0;
		std::string text;
	};

	struct feature_run_summary_t {
		test_lab::run_state_e state = test_lab::run_state_e::idle;
		test_lab::outcome_e outcome = test_lab::outcome_e::not_run;
		bool ok = false;
		bool skipped = false;
		std::int32_t ntstatus = 0;
		std::uint32_t bytes_returned = 0;
		std::uint64_t elapsed_us = 0;
		std::string error;
		std::uint64_t started_ms = 0;
		std::uint64_t finished_ms = 0;
		std::uint64_t log_line_index = 0;

		bool operator==(const feature_run_summary_t& o) const noexcept {
			return state == o.state && outcome == o.outcome && ok == o.ok && skipped == o.skipped &&
				ntstatus == o.ntstatus && bytes_returned == o.bytes_returned &&
				elapsed_us == o.elapsed_us && error == o.error &&
				started_ms == o.started_ms && finished_ms == o.finished_ms &&
				log_line_index == o.log_line_index;
		}
		bool operator!=(const feature_run_summary_t& o) const noexcept { return !(*this == o); }
	};

	struct run_all_status_t {
		bool active = false;
		int current = 0;
		int total = 0;
		int ok = 0;
		int fail = 0;
		int skipped = 0;
		std::string status_line;
		std::string current_name;

		bool operator==(const run_all_status_t& o) const noexcept {
			return active == o.active && current == o.current && total == o.total &&
				ok == o.ok && fail == o.fail && skipped == o.skipped &&
				status_line == o.status_line && current_name == o.current_name;
		}
		bool operator!=(const run_all_status_t& o) const noexcept { return !(*this == o); }
	};

	static TestLabController* instance();

	TestLabController(const TestLabController&) = delete;
	TestLabController& operator=(const TestLabController&) = delete;

	const test_lab::feature_t* featureAt(int index) const;

	test_lab::state_t& inputState() noexcept { return state_; }

	int selectedFeature() const noexcept { return selected_idx_; }
	void selectFeature(int idx);
	void runSelectedFeature();
	void clearResult();
	void startRunAllSafe();

	std::string runAllLogPath() const;

	std::uint64_t appendLogTail(const std::string& text);

	const std::vector<feature_run_summary_t>& cachedSummaries() const noexcept { return cached_summaries_; }
	const std::vector<log_tail_line_t>& cachedLogTail() const noexcept { return cached_tail_; }
	const test_lab::result_t& cachedResult() const noexcept { return cached_result_; }
	const run_all_status_t& cachedRunAll() const noexcept { return cached_run_all_; }
	bool summariesBusy() const noexcept { return summaries_busy_; }
	bool resultBusy() const noexcept { return result_busy_; }
	bool logTailBusy() const noexcept { return tail_busy_; }

Q_SIGNALS:
	void featuresChanged();
	void resultChanged();
	void logTailChanged();
	void runAllChanged();
	void selectionChanged(int idx);

private:
	explicit TestLabController(QObject* parent = nullptr);
	~TestLabController() override = default;

	void pollSnapshots();
	void notifyFromWorker();

	std::uint64_t nowMs() const;
	void logRenderLockBusy(const char* site, const char* lock_name);

	void resetFeatureSummaries();
	void ensureFeatureSummarySizeLocked(std::size_t feature_count);
	void updateFeatureSummaryStart(std::size_t feature_index, std::uint64_t log_index);
	void updateFeatureSummarySkip(std::size_t feature_index, const char* reason, std::uint64_t log_index);
	void updateFeatureSummaryResult(std::size_t feature_index, const test_lab::result_t& r, std::uint64_t log_index);

	bool tryCopyFeatureSummaries(const char* site, std::vector<feature_run_summary_t>& out);
	bool tryCopyLogTail(const char* site, std::vector<log_tail_line_t>& out);
	bool tryCopyResultSummary(const char* site, test_lab::run_state_e& state,
		test_lab::outcome_e& outcome, bool& ok, bool& skipped);
	bool tryCopyResultFull(const char* site, test_lab::result_t& out);
	bool tryReplaceResult(const char* site, const std::shared_ptr<test_lab::result_t>& result);

	void runFnPostWithFeature(const test_lab::feature_t& f, int feature_idx);
	void startRunAllSafeImpl();

	test_lab::state_t                   state_;
	int                                 selected_idx_ = -1;

	std::shared_ptr<test_lab::result_t> result_ = std::make_shared<test_lab::result_t>();
	std::mutex                          result_mtx_;

	std::atomic<bool> run_all_active_{ false };
	std::atomic<int>  run_all_current_{ 0 };
	std::atomic<int>  run_all_total_{ 0 };
	std::atomic<int>  run_all_ok_{ 0 };
	std::atomic<int>  run_all_fail_{ 0 };
	std::atomic<int>  run_all_skipped_{ 0 };
	std::mutex        run_all_status_mtx_;
	std::string       run_all_status_line_;
	std::string       run_all_current_name_;

	std::mutex                 log_tail_mtx_;
	std::deque<log_tail_line_t> log_tail_;
	std::uint64_t              log_tail_next_index_ = 1;

	std::mutex                          feature_summary_mtx_;
	std::vector<feature_run_summary_t>  feature_summaries_;

	std::atomic<std::uint64_t> snapshot_counter_{ 0 };

	QTimer* poll_timer_ = nullptr;

	std::vector<feature_run_summary_t> cached_summaries_;
	std::vector<log_tail_line_t>       cached_tail_;
	test_lab::result_t                 cached_result_;
	run_all_status_t                   cached_run_all_;
	bool                               summaries_busy_ = false;
	bool                               result_busy_ = false;
	bool                               tail_busy_ = false;
	bool                               poll_active_ = false;
};

}
