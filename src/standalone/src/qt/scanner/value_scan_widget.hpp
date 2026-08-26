#pragma once

#include <QWidget>

#include <QHash>

#include "qt/scanner/scan_commands.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSplitter;
class QStackedLayout;

class QAction;

namespace aida::qt::widgets {
class AidaNotice;
class AidaStateView;
}

namespace aida::qt::scanner {

class AddressListView;
class MemoryInteractionBridge;
class ScanResultsView;

class ValueScanWidget : public QWidget {
	Q_OBJECT
public:
	explicit ValueScanWidget(QWidget* parent = nullptr);
	~ValueScanWidget() override;

protected:
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;

private:
	void refresh_state();
	void refresh_action_buttons();
	void update_value_fields();
	void bind_action(QPushButton* button, const QString& action_id);

	ScanResultsView* results_view_ = nullptr;
	AddressListView* address_view_ = nullptr;
	MemoryInteractionBridge* results_bridge_ = nullptr;
	MemoryInteractionBridge* address_bridge_ = nullptr;
	QComboBox* type_combo_ = nullptr;
	QComboBox* mode_combo_ = nullptr;
	QLineEdit* value_edit_ = nullptr;
	QLabel* to_label_ = nullptr;
	QLineEdit* value_edit2_ = nullptr;
	QCheckBox* hex_check_ = nullptr;
	QPushButton* source_pill_ = nullptr;
	QPushButton* first_button_ = nullptr;
	QPushButton* next_button_ = nullptr;
	QPushButton* stop_button_ = nullptr;
	QPushButton* undo_button_ = nullptr;
	QPushButton* new_button_ = nullptr;
	QLabel* count_label_ = nullptr;
	QProgressBar* progress_ = nullptr;
	widgets::AidaNotice* callout_ = nullptr;
	QLabel* address_count_label_ = nullptr;
	QCheckBox* auto_refresh_check_ = nullptr;
	QSplitter* splitter_ = nullptr;
	QStackedLayout* results_stack_ = nullptr;
	QStackedLayout* address_stack_ = nullptr;
	widgets::AidaStateView* results_empty_ = nullptr;
	widgets::AidaStateView* address_empty_ = nullptr;
	QHash<QPushButton*, scan_command_t> fallback_commands_;
	bool refreshing_controls_ = false;
};

class ValueScanResultsDockWidget : public QWidget {
	Q_OBJECT
public:
	explicit ValueScanResultsDockWidget(QWidget* parent = nullptr);

protected:
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;

private:
	void refresh_state();

	ScanResultsView* view_ = nullptr;
	MemoryInteractionBridge* bridge_ = nullptr;
	QWidget* status_strip_ = nullptr;
	QLabel* progress_label_ = nullptr;
	QPushButton* stop_button_ = nullptr;
	QWidget* overlay_ = nullptr;
	widgets::AidaStateView* empty_view_ = nullptr;
	QStackedLayout* stack_ = nullptr;
};

class AddressListDockWidget : public QWidget {
	Q_OBJECT
public:
	explicit AddressListDockWidget(QWidget* parent = nullptr);

protected:
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;

private:
	void refresh_state();

	AddressListView* view_ = nullptr;
	MemoryInteractionBridge* bridge_ = nullptr;
	QWidget* overlay_ = nullptr;
	widgets::AidaStateView* empty_view_ = nullptr;
	QStackedLayout* stack_ = nullptr;
};

}
