#include "qt/scanner/snapshot_controller.hpp"

#include "helpers/diag_log.hpp"

namespace aida::qt::scanner {

SnapshotDiffController& SnapshotDiffController::instance()
{
	static SnapshotDiffController* controller = new SnapshotDiffController();
	return *controller;
}

SnapshotDiffController::SnapshotDiffController(QObject* parent) : QObject(parent) {}

void SnapshotDiffController::install(docking::AidaDockHost* host)
{
	host_ = host;
}

void SnapshotDiffController::take_snapshot()
{
	diag::log_tagged("scan_audit", "[scan_audit] snapshot_diff take_snapshot");
	snapshot_diff::take_snapshot();
	Q_EMIT stateChanged();
}

void SnapshotDiffController::compare(std::uint64_t id_a, std::uint64_t id_b)
{
	diag::log_tagged("scan_audit", "[scan_audit] snapshot_diff compare");
	snapshot_diff::compare_snapshots(id_a, id_b);
	set_selected_change(-1);
	Q_EMIT stateChanged();
}

void SnapshotDiffController::clear()
{
	snapshot_diff::clear_snapshots();
	set_selected_change(-1);
	Q_EMIT stateChanged();
}

void SnapshotDiffController::load(const std::string& path)
{
	snapshot_diff::load_from_disk(path);
	Q_EMIT stateChanged();
}

void SnapshotDiffController::set_selected_change(int row)
{
	if (selected_change_ == row)
		return;
	selected_change_ = row;
	Q_EMIT stateChanged();
}

std::shared_ptr<const snapshot_diff::diff_result_t>
SnapshotDiffController::published_diff() const
{
	std::lock_guard<std::mutex> lock(snapshot_diff::g_state.mutex);
	return snapshot_diff::g_state.published_diff;
}

std::size_t SnapshotDiffController::snapshot_count() const
{
	std::lock_guard<std::mutex> lock(snapshot_diff::g_state.mutex);
	return snapshot_diff::g_state.snapshots.size();
}

}
