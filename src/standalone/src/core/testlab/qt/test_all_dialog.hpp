#pragma once

#include "qt/bridge/aida_dialog.hpp"

#include <cstdint>

class QCloseEvent;
class QEvent;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace aida::qt::testlab {

class TestAllController;

class TestAllDialog : public aida::qt::bridge::AidaDialog {
	Q_OBJECT
public:
	explicit TestAllDialog(TestAllController* controller, QWidget* parent = nullptr);
	~TestAllDialog() override = default;

	void reject() override;

protected:
	void closeEvent(QCloseEvent* event) override;
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	void refreshFromController();
	void rebuildLogText();
	void updatePhaseLabel();

	TestAllController* controller_ = nullptr;
	QPushButton* start_button_ = nullptr;
	QPushButton* cancel_button_ = nullptr;
	QLabel* phase_label_ = nullptr;
	QLabel* target_label_ = nullptr;
	QLabel* total_label_ = nullptr;
	QLabel* passed_label_ = nullptr;
	QLabel* failed_label_ = nullptr;
	QLabel* skipped_label_ = nullptr;
	QProgressBar* progress_ = nullptr;
	QLabel* log_caption_ = nullptr;
	QPlainTextEdit* log_ = nullptr;
	QLabel* log_path_label_ = nullptr;
	QLabel* target_log_path_label_ = nullptr;
	std::uint64_t applied_log_version_ = 0;
};

TestAllDialog* createTestAllDialog(TestAllController* controller, QWidget* parent = nullptr);

}
