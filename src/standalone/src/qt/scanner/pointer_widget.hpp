#pragma once

#include <QWidget>

#include <memory>
#include <optional>

#include "core/scanner/pointer_scanner.hpp"

class QCheckBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QProgressBar;
class QSpinBox;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaNotice;
class AidaStateView;
class AidaViewHeader;
}

namespace aida::qt::scanner {

class ChainDiagramWidget;
class PointerChainModel;

class PointerScannerWidget : public QWidget {
	Q_OBJECT
public:
	explicit PointerScannerWidget(QWidget* parent = nullptr);
	~PointerScannerWidget() override;

protected:
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	void poll_engine();
	void refresh_presentation();
	void refresh_staged_card();
	void on_chain_context(const QPoint& global_pos, int row, int origin);
	void show_chain_menu(const pointer_scanner::pointer_chain_t& chain,
		const QPoint& global_pos, int origin);
	void select_chain(int row);

	PointerChainModel* model_ = nullptr;
	QTableView* table_ = nullptr;
	ChainDiagramWidget* diagram_ = nullptr;
	QLabel* detail_info_ = nullptr;
	QWidget* detail_panel_ = nullptr;
	widgets::AidaViewHeader* view_header_ = nullptr;
	QLineEdit* address_edit_ = nullptr;
	QSpinBox* depth_spin_ = nullptr;
	QSpinBox* offset_spin_ = nullptr;
	QSpinBox* struct_spin_ = nullptr;
	QCheckBox* negative_check_ = nullptr;
	QCheckBox* static_check_ = nullptr;
	QWidget* staged_card_ = nullptr;
	QLabel* staged_status_ = nullptr;
	QLabel* staged_identity_ = nullptr;
	widgets::AidaButton* staged_apply_ = nullptr;
	widgets::AidaButton* build_button_ = nullptr;
	widgets::AidaButton* scan_button_ = nullptr;
	widgets::AidaButton* validate_button_ = nullptr;
	widgets::AidaButton* clear_results_button_ = nullptr;
	widgets::AidaButton* clear_map_button_ = nullptr;
	QLabel* map_progress_label_ = nullptr;
	QProgressBar* map_progress_ = nullptr;
	QLabel* scan_progress_label_ = nullptr;
	QProgressBar* scan_progress_ = nullptr;
	QLabel* validate_progress_label_ = nullptr;
	QProgressBar* validate_progress_ = nullptr;
	widgets::AidaNotice* capability_notice_ = nullptr;
	widgets::AidaStateView* empty_hint_ = nullptr;
	QTimer* timer_ = nullptr;
	std::uint64_t observed_result_generation_ = 0;
	int selected_row_ = -1;
	bool controls_syncing_ = false;
};

}
