#pragma once

#include <QWidget>

#include <memory>

#include "core/disasm/disasm_view.hpp"
#include "core/scanner/aob_generator.hpp"
#include "qt/scanner/aob_controller.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QSplitter;
class QTabBar;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaNotice;
class AidaStateView;
}

namespace aida::qt::scanner {

class AobByteGridWidget;
class AobSavedModel;

class AobGeneratorWidget : public QWidget {
	Q_OBJECT
public:
	explicit AobGeneratorWidget(QWidget* parent = nullptr);
	~AobGeneratorWidget() override;

protected:
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	void poll_engine();
	void refresh_presentation();
	void refresh_signature_card();
	void on_saved_context(const QPoint& global_pos, int row, int origin);
	void show_saved_menu(const aob_generator::signature_t& signature,
		const disasm_view::workspace_context_t& context,
		const std::shared_ptr<aob_view_state_t>& view_state,
		const QPoint& global_pos, int origin);
	QString current_format_text() const;

	std::shared_ptr<aob_generator::state_t> generator_;
	std::shared_ptr<aob_view_state_t> view_state_;
	QTimer* timer_ = nullptr;
	QLineEdit* address_edit_ = nullptr;
	QLineEdit* name_edit_ = nullptr;
	QSpinBox* count_spin_ = nullptr;
	QCheckBox* auto_wildcard_ = nullptr;
	QCheckBox* validate_uniqueness_ = nullptr;
	widgets::AidaButton* generate_button_ = nullptr;
	widgets::AidaButton* regenerate_button_ = nullptr;
	widgets::AidaButton* save_button_ = nullptr;
	widgets::AidaButton* optimize_button_ = nullptr;
	widgets::AidaButton* copy_format_ = nullptr;
	widgets::AidaButton* copy_signature_ = nullptr;
	widgets::AidaButton* copy_yara_ = nullptr;
	widgets::AidaButton* export_json_ = nullptr;
	widgets::AidaButton* export_yara_ = nullptr;
	widgets::AidaButton* export_header_ = nullptr;
	widgets::AidaButton* compare_button_ = nullptr;
	widgets::AidaButton* save_disk_ = nullptr;
	widgets::AidaButton* load_disk_ = nullptr;
	QLabel* batch_badge_ = nullptr;
	widgets::AidaNotice* capability_notice_ = nullptr;
	widgets::AidaNotice* error_notice_ = nullptr;
	QWidget* signature_card_ = nullptr;
	QLabel* signature_info_ = nullptr;
	QLabel* grade_label_ = nullptr;
	AobByteGridWidget* byte_grid_ = nullptr;
	QTabBar* format_tabs_ = nullptr;
	QLineEdit* pattern_text_ = nullptr;
	QLabel* operation_status_ = nullptr;
	widgets::AidaButton* retry_button_ = nullptr;
	AobSavedModel* saved_model_ = nullptr;
	QTableView* saved_table_ = nullptr;
	QWidget* saved_panel_ = nullptr;
	QWidget* form_panel_ = nullptr;
	widgets::AidaStateView* saved_empty_ = nullptr;
	std::uint64_t observed_catalog_generation_ = 0;
	bool controls_syncing_ = false;
};

}
