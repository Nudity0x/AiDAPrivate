#pragma once

#include <QWidget>

#include <memory>

#include "core/disasm/disasm_view.hpp"
#include "core/scanner/crypto_workspace_scan.hpp"

class QComboBox;
class QHeaderView;
class QLabel;
class QLineEdit;
class QProgressBar;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaBadge;
class AidaButton;
class AidaNotice;
class AidaStateView;
}

namespace aida::qt::scanner {

class CryptoHitsModel;

class CryptoScannerWidget : public QWidget {
	Q_OBJECT
public:
	explicit CryptoScannerWidget(QWidget* parent = nullptr);
	~CryptoScannerWidget() override;

protected:
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	void poll_engine();
	void refresh_presentation();
	void issue_filter();
	void on_hit_context(const QPoint& global_pos, int row, int origin);
	void show_hit_menu(const crypto_scanner::crypto_hit_t& hit,
		const disasm_view::workspace_context_t& context,
		const std::shared_ptr<crypto_workspace_scan::state_t>& state,
		const QPoint& global_pos, int origin);

	std::shared_ptr<crypto_workspace_scan::state_t> state_;
	std::shared_ptr<const crypto_scanner::workspace_scan_snapshot_t> snapshot_;
	QTimer* poll_timer_ = nullptr;
	QTimer* filter_debounce_ = nullptr;
	widgets::AidaButton* scan_button_ = nullptr;
	widgets::AidaButton* scan_file_button_ = nullptr;
	widgets::AidaButton* entropy_button_ = nullptr;
	widgets::AidaButton* export_json_ = nullptr;
	widgets::AidaButton* export_csv_ = nullptr;
	QProgressBar* progress_ = nullptr;
	widgets::AidaNotice* error_notice_ = nullptr;
	widgets::AidaNotice* export_notice_ = nullptr;
	widgets::AidaButton* retry_button_ = nullptr;
	widgets::AidaNotice* capability_notice_ = nullptr;
	QLineEdit* search_edit_ = nullptr;
	QComboBox* category_combo_ = nullptr;
	widgets::AidaBadge* chip_hits_ = nullptr;
	widgets::AidaBadge* chip_cipher_ = nullptr;
	widgets::AidaBadge* chip_hash_ = nullptr;
	widgets::AidaBadge* chip_refs_ = nullptr;
	CryptoHitsModel* model_ = nullptr;
	QTableView* table_ = nullptr;
	widgets::AidaStateView* empty_view_ = nullptr;
	QLabel* status_label_ = nullptr;
	int sort_column_ = -1;
	bool sort_ascending_ = true;
	bool controls_syncing_ = false;
};

}
