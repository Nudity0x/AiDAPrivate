#pragma once

#include "test_lab_controller.hpp"

#include <QAbstractItemModel>
#include <QAbstractTableModel>
#include <QStyledItemDelegate>
#include <QString>
#include <QWidget>

#include <vector>

class QAction;
class QComboBox;
class QEvent;
class QFormLayout;
class QGroupBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTableView;
class QTreeView;

namespace aida::qt::widgets {
class AidaBadge;
class AidaNotice;
class AidaStateView;
}

namespace aida::qt::testlab {

class TestLabFeatureModel : public QAbstractItemModel {
	Q_OBJECT
public:
	enum ItemRole {
		IsHeaderRole = Qt::UserRole + 1,
		FeatureIndexRole,
		RunStateRole,
		OutcomeRole,
		OkRole,
		SkippedRole,
		DriverRole,
		CountsTextRole,
	};

	enum FilterMode {
		FilterAll = 0,
		FilterFailed,
		FilterPassed,
		FilterSkipped,
		FilterRunning,
		FilterPending,
	};

	explicit TestLabFeatureModel(QObject* parent = nullptr);

	void rebuild();
	void updateSummaries(const std::vector<TestLabController::feature_run_summary_t>& summaries);

	void setStatusFilter(FilterMode mode);
	FilterMode statusFilter() const noexcept { return filter_; }

	int featureIndexFor(const QModelIndex& index) const;
	QModelIndex indexForFeature(int feature_index) const;

	QModelIndex index(int row, int column, const QModelIndex& parent) const override;
	QModelIndex parent(const QModelIndex& child) const override;
	int rowCount(const QModelIndex& parent) const override;
	int columnCount(const QModelIndex& parent) const override;
	QVariant data(const QModelIndex& index, int role) const override;
	void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
	Qt::ItemFlags flags(const QModelIndex& index) const override;

private:
	struct category_t {
		std::string name;
		std::vector<int> features;
		int pass = 0;
		int fail = 0;
		int skip = 0;
		int running = 0;
	};

	QString tooltipForFeature(int feature_index) const;
	QString countsText(const category_t& cat) const;
	bool matchesFilter(int feature_index) const;

	std::vector<category_t> categories_;
	std::vector<std::pair<int, int>> position_by_feature_;
	std::vector<TestLabController::feature_run_summary_t> summaries_;
	FilterMode filter_ = FilterAll;
};

class TestLabRowDelegate : public QStyledItemDelegate {
	Q_OBJECT
public:
	explicit TestLabRowDelegate(QObject* parent = nullptr);

	void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
	QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

class TestLabParsedModel : public QAbstractTableModel {
	Q_OBJECT
public:
	explicit TestLabParsedModel(QObject* parent = nullptr);

	void setFields(const std::vector<test_lab::parsed_field_t>& fields);
	const std::vector<test_lab::parsed_field_t>& fields() const noexcept { return fields_; }

	int rowCount(const QModelIndex& parent) const override;
	int columnCount(const QModelIndex& parent) const override;
	QVariant data(const QModelIndex& index, int role) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
	void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

private:
	std::vector<test_lab::parsed_field_t> fields_;
};

class TestLabWidget : public QWidget {
	Q_OBJECT
public:
	explicit TestLabWidget(TestLabController* controller, QWidget* parent = nullptr);
	~TestLabWidget() override = default;

	TestLabController* controller() const noexcept { return controller_; }

	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	void rebuildInputs();
	void scheduleRebuildInputs();
	void refreshTopBar();
	void refreshResult();
	void refreshEvidence();
	void applySelectionFromController();
	void syncTreeSelection();
	void refreshTreeEmptyState();
	void updateRunAllStatusText();
	void openLogFolder();

	TestLabController* controller_ = nullptr;

	QPushButton* run_all_button_ = nullptr;
	QLabel* run_all_status_ = nullptr;
	QString run_all_status_full_;
	QProgressBar* run_all_progress_ = nullptr;

	QComboBox* filter_combo_ = nullptr;
	TestLabFeatureModel* tree_model_ = nullptr;
	TestLabRowDelegate* tree_delegate_ = nullptr;
	QTreeView* tree_ = nullptr;
	QStackedWidget* tree_stack_ = nullptr;
	widgets::AidaStateView* tree_empty_view_ = nullptr;

	QLabel* name_label_ = nullptr;
	QLabel* summary_label_ = nullptr;
	QGroupBox* inputs_group_ = nullptr;
	QWidget* inputs_body_ = nullptr;
	QFormLayout* inputs_form_ = nullptr;
	QPushButton* run_button_ = nullptr;
	QPushButton* clear_button_ = nullptr;
	QWidget* action_row_ = nullptr;
	QAction* run_action_ = nullptr;
	QAction* run_all_action_ = nullptr;
	QAction* clear_action_ = nullptr;
	widgets::AidaStateView* no_selection_view_ = nullptr;

	QGroupBox* result_group_ = nullptr;
	QWidget* chip_row_ = nullptr;
	widgets::AidaBadge* chip_status_ = nullptr;
	widgets::AidaBadge* chip_ntstatus_ = nullptr;
	widgets::AidaBadge* chip_driver_ = nullptr;
	widgets::AidaBadge* chip_bytes_ = nullptr;
	widgets::AidaBadge* chip_elapsed_ = nullptr;
	widgets::AidaNotice* cached_notice_ = nullptr;
	widgets::AidaNotice* busy_notice_ = nullptr;
	QStackedWidget* result_stack_ = nullptr;
	widgets::AidaStateView* result_empty_view_ = nullptr;
	widgets::AidaStateView* result_running_view_ = nullptr;
	QWidget* result_complete_ = nullptr;
	QLabel* error_label_ = nullptr;
	QGroupBox* raw_group_ = nullptr;
	QPlainTextEdit* raw_edit_ = nullptr;
	widgets::AidaStateView* raw_empty_view_ = nullptr;
	QStackedWidget* raw_stack_ = nullptr;
	QGroupBox* parsed_group_ = nullptr;
	QTableView* parsed_table_ = nullptr;
	TestLabParsedModel* parsed_model_ = nullptr;
	widgets::AidaStateView* parsed_empty_view_ = nullptr;
	QStackedWidget* parsed_stack_ = nullptr;

	QLabel* log_path_label_ = nullptr;
	widgets::AidaNotice* tail_busy_notice_ = nullptr;
	QPlainTextEdit* tail_edit_ = nullptr;
	std::uint64_t tail_last_shown_index_ = 0;

	bool rebuild_pending_ = false;
};

QWidget* createTestLabWidget(QWidget* parent = nullptr);

}
