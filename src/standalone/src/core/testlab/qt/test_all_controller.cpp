#include "test_all_controller.hpp"

#include "test_all_dialog.hpp"

#include <QTimer>

#include <utility>

namespace aida::qt::testlab {

namespace {

	constexpr int k_poll_interval_ms = 100;
	constexpr std::size_t k_log_tail_max_lines = 512;

}

TestAllController* TestAllController::instance() {
	static TestAllController* g_instance = nullptr;
	if (g_instance == nullptr)
		g_instance = new TestAllController();
	return g_instance;
}

TestAllController::TestAllController(QObject* parent) : QObject(parent) {
	poll_timer_ = new QTimer(this);
	poll_timer_->setInterval(k_poll_interval_ms);
	poll_timer_->setTimerType(Qt::CoarseTimer);
	connect(poll_timer_, &QTimer::timeout, this, [this]() { poll(); });
	connect(this, &TestAllController::visibleChanged, this, [this](bool vis) { syncDialogVisibility(vis); });
	poll_timer_->start();
	poll();
}

TestAllController::~TestAllController() = default;

void TestAllController::setVisible(bool visible) {
	test_all_features::set_overlay_visible(visible);
	const bool now_visible = test_all_features::overlay_visible();
	if (visible_ != now_visible) {
		visible_ = now_visible;
		Q_EMIT visibleChanged(visible_);
	}
}

void TestAllController::requestStart() {
	test_all_features::start_tests();
}

void TestAllController::requestCancel() {
	test_all_features::request_interactive_cancel();
}

void TestAllController::showDialog() {
	setVisible(true);
	if (!dialog_.isNull()) {
		dialog_->raise();
		dialog_->activateWindow();
	}
}

std::string TestAllController::fullTestLogPath() const {
	const char* path = test_all_features::full_test_log_path();
	return path != nullptr ? std::string(path) : std::string();
}

std::string TestAllController::fullTestTargetLogPath() const {
	const char* path = test_all_features::full_test_target_log_path();
	return path != nullptr ? std::string(path) : std::string();
}

void TestAllController::syncDialogVisibility(bool visible) {
	if (visible) {
		if (dialog_.isNull())
			dialog_ = createTestAllDialog(this, nullptr);
		if (!dialog_->isVisible())
			dialog_->open();
	} else if (!dialog_.isNull() && dialog_->isVisible()) {
		dialog_->hide();
	}
}

void TestAllController::poll() {
	if (poll_active_)
		return;
	poll_active_ = true;

	const test_all_features::overlay_perf_snapshot_t perf = test_all_features::overlay_perf_snapshot();
	const test_all_features::overlay_run_snapshot_t run = test_all_features::overlay_run_snapshot();

	char phase_buf[160] = {};
	char step_buf[256] = {};
	std::uint64_t step_start_ms = 0;
	test_all_features::current_phase_and_step(phase_buf, sizeof(phase_buf), step_buf, sizeof(step_buf), &step_start_ms);

	bool tail_changed = false;
	std::size_t tail_total = log_tail_total_;
	std::vector<test_all_features::overlay_log_line_t> tail;
	const bool tail_ok = test_all_features::overlay_log_tail_snapshot(
		k_log_tail_max_lines, log_tail_version_, tail, &tail_total, &tail_changed);
	const bool busy_changed = (!tail_ok) != log_tail_busy_;
	log_tail_busy_ = !tail_ok;
	if (tail_ok) {
		if (tail_changed)
			log_tail_ = std::move(tail);
		log_tail_total_ = tail_total;
	}

	const bool snapshot_changed =
		perf.dirty_version != perf_.dirty_version ||
		tail_changed ||
		busy_changed;

	perf_ = perf;
	run_ = run;
	phase_ = phase_buf;
	step_ = step_buf;
	step_start_ms_ = step_start_ms;

	if (snapshot_changed)
		Q_EMIT snapshotChanged();

	const bool now_visible = test_all_features::overlay_visible();
	if (visible_ != now_visible) {
		visible_ = now_visible;
		Q_EMIT visibleChanged(visible_);
	}

	poll_active_ = false;
}

}
