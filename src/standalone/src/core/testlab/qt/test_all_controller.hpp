#pragma once

#include "../test_all_features.hpp"

#include <QObject>
#include <QPointer>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class QTimer;

namespace aida::qt::testlab {

class TestAllDialog;

class TestAllController : public QObject {
	Q_OBJECT
public:
	static TestAllController* instance();

	TestAllController(const TestAllController&) = delete;
	TestAllController& operator=(const TestAllController&) = delete;

	bool visible() const noexcept { return visible_; }
	void setVisible(bool visible);

	void requestStart();
	void requestCancel();
	void showDialog();

	bool running() const noexcept { return run_.running; }
	const test_all_features::overlay_perf_snapshot_t& perfSnapshot() const noexcept { return perf_; }
	const test_all_features::overlay_run_snapshot_t& runSnapshot() const noexcept { return run_; }
	const std::string& phaseLabel() const noexcept { return phase_; }
	const std::string& stepLabel() const noexcept { return step_; }
	std::uint64_t stepStartMs() const noexcept { return step_start_ms_; }
	const std::vector<test_all_features::overlay_log_line_t>& logTail() const noexcept { return log_tail_; }
	std::uint64_t logTailVersion() const noexcept { return log_tail_version_; }
	std::size_t logTailTotal() const noexcept { return log_tail_total_; }
	bool logTailBusy() const noexcept { return log_tail_busy_; }
	std::string fullTestLogPath() const;
	std::string fullTestTargetLogPath() const;

Q_SIGNALS:
	void visibleChanged(bool visible);
	void snapshotChanged();

private:
	explicit TestAllController(QObject* parent = nullptr);
	~TestAllController() override;

	void poll();
	void syncDialogVisibility(bool visible);

	bool visible_ = false;
	bool log_tail_busy_ = false;
	bool poll_active_ = false;
	std::uint64_t log_tail_version_ = 0;
	std::size_t log_tail_total_ = 0;
	std::string phase_;
	std::string step_;
	std::uint64_t step_start_ms_ = 0;
	test_all_features::overlay_perf_snapshot_t perf_;
	test_all_features::overlay_run_snapshot_t run_;
	std::vector<test_all_features::overlay_log_line_t> log_tail_;
	QTimer* poll_timer_ = nullptr;
	QPointer<TestAllDialog> dialog_;
};

}
