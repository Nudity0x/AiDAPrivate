#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>

#include "core/scanner/snapshot_diff.hpp"

class QLabel;
class QProgressBar;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaNotice;
class AidaStateView;
}

namespace aida::qt::scanner {

class DiffDetailWidget;
class DiffTableModel;
class SnapshotTimelineWidget;

class SnapshotDiffWidget : public QWidget {
	Q_OBJECT
public:
	explicit SnapshotDiffWidget(QWidget* parent = nullptr);
	~SnapshotDiffWidget() override;

protected:
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	void poll_engine();
	void refresh_presentation();
	void refresh_detail();
	void on_diff_context(const QPoint& global_pos, int row, int origin);
	void show_diff_menu(const snapshot_diff::changed_region_t& change,
		const QPoint& global_pos, int origin);

	widgets::AidaButton* take_button_ = nullptr;
	widgets::AidaButton* compare_button_ = nullptr;
	widgets::AidaButton* clear_button_ = nullptr;
	widgets::AidaButton* load_button_ = nullptr;
	QProgressBar* progress_ = nullptr;
	QLabel* count_label_ = nullptr;
	widgets::AidaNotice* capability_notice_ = nullptr;
	widgets::AidaNotice* error_notice_ = nullptr;
	QLabel* truncated_label_ = nullptr;
	SnapshotTimelineWidget* timeline_ = nullptr;
	DiffTableModel* model_ = nullptr;
	QTableView* table_ = nullptr;
	DiffDetailWidget* detail_ = nullptr;
	widgets::AidaStateView* empty_hint_ = nullptr;
	QTimer* timer_ = nullptr;
	std::shared_ptr<const snapshot_diff::diff_result_t> adopted_diff_;
	bool controls_syncing_ = false;
};

}
