#include "qt/scanner/aob_widget.hpp"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QSplitter>
#include <QTabBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/ai/entity_evidence_handoff.hpp"
#include "core/editor/hex_view.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"

#include "qt/bridge/clipboard.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/scanner/aob_byte_grid_widget.hpp"
#include "qt/scanner/aob_saved_model.hpp"
#include "qt/scanner/scan_hub_controller.hpp"
#include "qt/scanner/scanner_context_menus.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::scanner {

namespace {

widgets::AidaSemantic grade_semantic(float quality_score)
{
	if (quality_score >= 0.85f) return widgets::AidaSemantic::Success;
	if (quality_score >= 0.7f)  return widgets::AidaSemantic::Info;
	if (quality_score >= 0.5f)  return widgets::AidaSemantic::Warning;
	return widgets::AidaSemantic::Error;
}

QString format_for_tab(const aob_generator::signature_t& sig, int tab)
{
	switch (tab) {
	case 0:  return QString::fromStdString(aob_generator::format_signature(sig));
	case 1:  return QString::fromStdString(aob_generator::format_ida_signature(sig));
	case 2:  return QString::fromStdString(aob_generator::format_code_signature(sig));
	case 3:  return QString::fromStdString(aob_generator::format_x64dbg_signature(sig));
	default: return QString::fromStdString(aob_generator::format_signature(sig));
	}
}

bool signature_bytes_equal(const std::vector<aob_generator::aob_byte_t>& lhs,
	const std::vector<aob_generator::aob_byte_t>& rhs)
{
	return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(),
		[](const auto& left, const auto& right) {
			return left.value == right.value && left.wildcard == right.wildcard;
		});
}

}

AobGeneratorWidget::AobGeneratorWidget(QWidget* parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("aida.view.memory.aob"));
	const auto& tokens = theme::tokens();
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	auto* splitter = new QSplitter(Qt::Horizontal, this);
	splitter->setObjectName(QStringLiteral("aida.view.memory.aob.splitter"));
	splitter->setOpaqueResize(true);
	splitter->setChildrenCollapsible(false);

	form_panel_ = new QWidget(splitter);
	form_panel_->setObjectName(QStringLiteral("aida.view.memory.aob.form"));
	auto* form = new QVBoxLayout(form_panel_);
	form->setContentsMargins(tokens.panel.padding, tokens.panel.padding,
		tokens.panel.padding, tokens.panel.padding);
	form->setSpacing(tokens.spacing.sm);
	auto* header = new QLabel(QStringLiteral("AOB Signature Generator"), form_panel_);
	header->setObjectName(QStringLiteral("aida.view.memory.aob.title"));
	form->addWidget(header);

	capability_notice_ = new widgets::AidaNotice(QStringLiteral("No data source"),
		QStringLiteral("Generate needs a live process attach or an open PE."),
		widgets::AidaSemantic::Warning, form_panel_);
	capability_notice_->setObjectName(
		QStringLiteral("aida.view.memory.aob.capability_notice"));
	capability_notice_->setVisible(false);
	form->addWidget(capability_notice_);

	auto* row1 = new QHBoxLayout();
	address_edit_ = new QLineEdit(form_panel_);
	address_edit_->setObjectName(QStringLiteral("aida.view.memory.aob.address"));
	address_edit_->setPlaceholderText(QStringLiteral("Address (hex)"));
	address_edit_->setFont(theme::fonts::codeRegular());
	address_edit_->setToolTip(QStringLiteral(
		"Instruction address to lift the pattern from (hex; defaults to the selected instruction)"));
	address_edit_->setMaxLength(static_cast<int>(sizeof(aob_generator::state_t::address_input)) - 1);
	row1->addWidget(address_edit_, 1);
	name_edit_ = new QLineEdit(form_panel_);
	name_edit_->setObjectName(QStringLiteral("aida.view.memory.aob.name"));
	name_edit_->setPlaceholderText(QStringLiteral("Signature name"));
	name_edit_->setMaxLength(static_cast<int>(sizeof(aob_generator::state_t::name_input)) - 1);
	row1->addWidget(name_edit_, 1);
	count_spin_ = new QSpinBox(form_panel_);
	count_spin_->setObjectName(QStringLiteral("aida.view.memory.aob.instruction_count"));
	count_spin_->setRange(1, 128);
	count_spin_->setToolTip(QStringLiteral("Instructions covered by the signature"));
	row1->addWidget(count_spin_);
	form->addLayout(row1);

	auto* row2 = new QHBoxLayout();
	auto_wildcard_ = new QCheckBox(QStringLiteral("Auto-wildcard"), form_panel_);
	auto_wildcard_->setObjectName(QStringLiteral("aida.view.memory.aob.auto_wildcard"));
	auto_wildcard_->setToolTip(QStringLiteral("Wildcard bytes that differ between candidate encodings"));
	row2->addWidget(auto_wildcard_);
	validate_uniqueness_ = new QCheckBox(QStringLiteral("Validate uniqueness"), form_panel_);
	validate_uniqueness_->setObjectName(QStringLiteral("aida.view.memory.aob.validate_uniqueness"));
	validate_uniqueness_->setToolTip(QStringLiteral("Rescan the module to confirm the pattern matches once"));
	row2->addWidget(validate_uniqueness_);
	row2->addStretch(1);
	form->addLayout(row2);

	auto* row3 = new QHBoxLayout();
	generate_button_ = new widgets::AidaButton(QStringLiteral("Generate"), form_panel_);
	generate_button_->setObjectName(QStringLiteral("aida.view.memory.aob.generate"));
	generate_button_->setKind(widgets::AidaButton::Kind::Primary);
	row3->addWidget(generate_button_);
	regenerate_button_ = new widgets::AidaButton(QStringLiteral("Regenerate"), form_panel_);
	regenerate_button_->setObjectName(QStringLiteral("aida.view.memory.aob.regenerate"));
	row3->addWidget(regenerate_button_);
	save_button_ = new widgets::AidaButton(QStringLiteral("Save"), form_panel_);
	save_button_->setObjectName(QStringLiteral("aida.view.memory.aob.save"));
	row3->addWidget(save_button_);
	optimize_button_ = new widgets::AidaButton(QStringLiteral("Optimize"), form_panel_);
	optimize_button_->setObjectName(QStringLiteral("aida.view.memory.aob.optimize"));
	optimize_button_->setToolTip(QStringLiteral(
		"Trim the pattern against the attached process until it stays unique"));
	row3->addWidget(optimize_button_);
	batch_badge_ = new QLabel(form_panel_);
	batch_badge_->setObjectName(QStringLiteral("aida.view.memory.aob.batch_badge"));
	batch_badge_->setProperty("aidaVariant", QStringLiteral("accent"));
	batch_badge_->setVisible(false);
	row3->addWidget(batch_badge_);
	row3->addStretch(1);
	form->addLayout(row3);

	error_notice_ = new widgets::AidaNotice(QStringLiteral("Last error:"), QString(),
		widgets::AidaSemantic::Error, form_panel_);
	error_notice_->setObjectName(QStringLiteral("aida.view.memory.aob.error_notice"));
	error_notice_->setVisible(false);
	form->addWidget(error_notice_);

	signature_card_ = new QFrame(form_panel_);
	signature_card_->setObjectName(QStringLiteral("aida.view.memory.aob.signature_card"));
	signature_card_->setProperty("aidaRole", QStringLiteral("panel"));
	auto* card_layout = new QHBoxLayout(signature_card_);
	card_layout->setContentsMargins(tokens.spacing.md, tokens.spacing.sm,
		tokens.spacing.md, tokens.spacing.sm);
	signature_info_ = new QLabel(signature_card_);
	signature_info_->setObjectName(QStringLiteral("aida.view.memory.aob.signature_info"));
	card_layout->addWidget(signature_info_, 1);
	grade_label_ = new QLabel(signature_card_);
	grade_label_->setObjectName(QStringLiteral("aida.view.memory.aob.grade"));
	card_layout->addWidget(grade_label_);
	form->addWidget(signature_card_);

	byte_grid_ = new AobByteGridWidget(form_panel_);
	form->addWidget(byte_grid_);

	format_tabs_ = new QTabBar(form_panel_);
	format_tabs_->setObjectName(QStringLiteral("aida.view.memory.aob.format_tabs"));
	format_tabs_->setDocumentMode(true);
	format_tabs_->setExpanding(false);
	format_tabs_->addTab(QStringLiteral("Standard"));
	format_tabs_->addTab(QStringLiteral("IDA"));
	format_tabs_->addTab(QStringLiteral("Code"));
	format_tabs_->addTab(QStringLiteral("x64dbg"));
	format_tabs_->setTabToolTip(0, QStringLiteral("AA BB ?? byte pattern"));
	format_tabs_->setTabToolTip(1, QStringLiteral("IDA-style pattern"));
	format_tabs_->setTabToolTip(2, QStringLiteral("C/C++ byte array"));
	format_tabs_->setTabToolTip(3, QStringLiteral("x64dbg pattern"));
	form->addWidget(format_tabs_);
	pattern_text_ = new QLineEdit(form_panel_);
	pattern_text_->setObjectName(QStringLiteral("aida.view.memory.aob.pattern"));
	pattern_text_->setReadOnly(true);
	pattern_text_->setFont(theme::fonts::codeRegular());
	form->addWidget(pattern_text_);

	auto* copy_row = new QHBoxLayout();
	copy_format_ = new widgets::AidaButton(
		QStringLiteral("Copy as Standard"), form_panel_);
	copy_format_->setObjectName(QStringLiteral("aida.view.memory.aob.copy_format"));
	copy_format_->setToolTip(QStringLiteral("Copy the pattern in the selected format"));
	copy_signature_ = new widgets::AidaButton(QStringLiteral("Copy signature"), form_panel_);
	copy_signature_->setObjectName(QStringLiteral("aida.view.memory.aob.copy_signature"));
	copy_signature_->setKind(widgets::AidaButton::Kind::Primary);
	copy_signature_->setToolTip(QStringLiteral("Copy the standard byte pattern"));
	copy_yara_ = new widgets::AidaButton(QStringLiteral("Copy YARA"), form_panel_);
	copy_yara_->setObjectName(QStringLiteral("aida.view.memory.aob.copy_yara"));
	copy_yara_->setToolTip(QStringLiteral("Copy the pattern as a YARA rule"));
	copy_row->addWidget(copy_format_);
	copy_row->addWidget(copy_signature_);
	copy_row->addWidget(copy_yara_);
	copy_row->addStretch(1);
	form->addLayout(copy_row);

	auto* ops_row = new QHBoxLayout();
	export_json_ = new widgets::AidaButton(QStringLiteral("Export JSON"), form_panel_);
	export_json_->setObjectName(QStringLiteral("aida.view.memory.aob.export_json"));
	export_json_->setKind(widgets::AidaButton::Kind::Ghost);
	export_yara_ = new widgets::AidaButton(QStringLiteral("Export YARA"), form_panel_);
	export_yara_->setObjectName(QStringLiteral("aida.view.memory.aob.export_yara"));
	export_yara_->setKind(widgets::AidaButton::Kind::Ghost);
	export_header_ = new widgets::AidaButton(QStringLiteral("Export Header"), form_panel_);
	export_header_->setObjectName(QStringLiteral("aida.view.memory.aob.export_header"));
	export_header_->setKind(widgets::AidaButton::Kind::Ghost);
	compare_button_ = new widgets::AidaButton(QStringLiteral("Compare"), form_panel_);
	compare_button_->setObjectName(QStringLiteral("aida.view.memory.aob.compare"));
	compare_button_->setToolTip(QStringLiteral(
		"Re-scan the attached process and check which saved patterns still match"));
	save_disk_ = new widgets::AidaButton(QStringLiteral("Save Disk"), form_panel_);
	save_disk_->setObjectName(QStringLiteral("aida.view.memory.aob.save_disk"));
	save_disk_->setKind(widgets::AidaButton::Kind::Ghost);
	load_disk_ = new widgets::AidaButton(QStringLiteral("Load Disk"), form_panel_);
	load_disk_->setObjectName(QStringLiteral("aida.view.memory.aob.load_disk"));
	load_disk_->setKind(widgets::AidaButton::Kind::Ghost);
	ops_row->addWidget(export_json_);
	ops_row->addWidget(export_yara_);
	ops_row->addWidget(export_header_);
	ops_row->addWidget(compare_button_);
	ops_row->addWidget(save_disk_);
	ops_row->addWidget(load_disk_);
	ops_row->addStretch(1);
	form->addLayout(ops_row);

	auto* status_row = new QHBoxLayout();
	operation_status_ = new QLabel(form_panel_);
	operation_status_->setObjectName(QStringLiteral("aida.view.memory.aob.operation_status"));
	operation_status_->setProperty("aidaVariant", QStringLiteral("secondary"));
	status_row->addWidget(operation_status_, 1);
	retry_button_ = new widgets::AidaButton(QStringLiteral("Retry"), form_panel_);
	retry_button_->setObjectName(QStringLiteral("aida.view.memory.aob.retry"));
	retry_button_->setVisible(false);
	status_row->addWidget(retry_button_);
	form->addLayout(status_row);
	form->addStretch(1);
	splitter->addWidget(form_panel_);

	saved_panel_ = new QWidget(splitter);
	saved_panel_->setObjectName(QStringLiteral("aida.view.memory.aob.saved"));
	auto* saved_layout = new QVBoxLayout(saved_panel_);
	saved_layout->setContentsMargins(tokens.panel.padding, tokens.panel.padding,
		tokens.panel.padding, tokens.panel.padding);
	saved_layout->setSpacing(tokens.spacing.sm);
	auto* saved_title = new QLabel(QStringLiteral("Saved Signatures"), saved_panel_);
	saved_title->setObjectName(QStringLiteral("aida.view.memory.aob.saved_title"));
	saved_layout->addWidget(saved_title);
	saved_model_ = new AobSavedModel(saved_panel_);
	saved_table_ = new QTableView(saved_panel_);
	saved_table_->setObjectName(QStringLiteral("aida.view.memory.aob.saved_table"));
	saved_table_->verticalHeader()->setVisible(false);
	saved_table_->verticalHeader()->setDefaultSectionSize(tokens.table.row_h);
	saved_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	saved_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
	saved_table_->horizontalHeader()->setStretchLastSection(true);
	saved_table_->horizontalHeader()->setMinimumHeight(tokens.table.header_h);
	saved_table_->setShowGrid(false);
	saved_table_->setWordWrap(false);
	saved_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	saved_table_->setSelectionMode(QAbstractItemView::SingleSelection);
	saved_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	saved_table_->setAlternatingRowColors(true);
	saved_table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
	saved_table_->setContextMenuPolicy(Qt::CustomContextMenu);
	saved_table_->setModel(saved_model_);
	saved_table_->installEventFilter(this);
	saved_empty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QStringLiteral("Nothing saved yet"),
		QStringLiteral("Generated signatures appear here once you click Save."),
		saved_panel_);
	saved_empty_->setObjectName(QStringLiteral("aida.view.memory.aob.saved_empty"));
	saved_layout->addWidget(saved_empty_, 1);
	saved_layout->addWidget(saved_table_, 1);
	splitter->addWidget(saved_panel_);
	splitter->setStretchFactor(0, 55);
	splitter->setStretchFactor(1, 45);
	layout->addWidget(splitter, 1);

	connect(address_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
		if (controls_syncing_ || !generator_) return;
		const std::string value = text.toStdString();
		std::strncpy(generator_->address_input, value.c_str(),
			sizeof(generator_->address_input) - 1);
		generator_->address_input[sizeof(generator_->address_input) - 1] = '\0';
	});
	connect(name_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
		if (controls_syncing_ || !generator_) return;
		const std::string value = text.toStdString();
		std::strncpy(generator_->name_input, value.c_str(),
			sizeof(generator_->name_input) - 1);
		generator_->name_input[sizeof(generator_->name_input) - 1] = '\0';
	});
	connect(count_spin_, &QSpinBox::valueChanged, this, [this](int value) {
		if (controls_syncing_ || !generator_) return;
		generator_->instruction_count = value;
	});
	connect(auto_wildcard_, &QCheckBox::toggled, this, [this](bool checked) {
		if (controls_syncing_ || !generator_) return;
		generator_->auto_wildcard = checked;
	});
	connect(validate_uniqueness_, &QCheckBox::toggled, this, [this](bool checked) {
		if (controls_syncing_ || !generator_) return;
		generator_->validate_uniqueness = checked;
	});
	connect(generate_button_, &widgets::AidaButton::clicked, this, [this] {
		AobController::instance().request_generate(
			disasm_view::capture_selected_workspace());
		poll_engine();
	});
	connect(regenerate_button_, &widgets::AidaButton::clicked, this, [this] {
		AobController::instance().request_regenerate(
			disasm_view::capture_selected_workspace());
		poll_engine();
	});
	connect(save_button_, &widgets::AidaButton::clicked, this, [this] {
		AobController::instance().request_save_current(
			disasm_view::capture_selected_workspace());
	});
	connect(optimize_button_, &widgets::AidaButton::clicked, this, [this] {
		AobController::instance().request_optimize(
			disasm_view::capture_selected_workspace());
		poll_engine();
	});
	connect(export_json_, &widgets::AidaButton::clicked, this, [this] {
		AobController::instance().request_export(
			disasm_view::capture_selected_workspace(),
			aob_generator::export_format_t::json, {});
	});
	connect(export_yara_, &widgets::AidaButton::clicked, this, [this] {
		AobController::instance().request_export(
			disasm_view::capture_selected_workspace(),
			aob_generator::export_format_t::yara, {});
	});
	connect(export_header_, &widgets::AidaButton::clicked, this, [this] {
		AobController::instance().request_export(
			disasm_view::capture_selected_workspace(),
			aob_generator::export_format_t::header, {});
	});
	connect(compare_button_, &widgets::AidaButton::clicked, this, [this] {
		diag::log_tagged("scan_audit", "[scan_audit] aob compare invoked");
		AobController::instance().request_comparison(
			disasm_view::capture_selected_workspace());
	});
	connect(save_disk_, &widgets::AidaButton::clicked, this, [this] {
		AobController::instance().request_catalog(
			disasm_view::capture_selected_workspace(), true);
	});
	connect(load_disk_, &widgets::AidaButton::clicked, this, [this] {
		AobController::instance().request_catalog(
			disasm_view::capture_selected_workspace(), false);
	});
	connect(copy_format_, &widgets::AidaButton::clicked, this, [this] {
		clipboard::set_text(current_format_text());
	});
	connect(copy_signature_, &widgets::AidaButton::clicked, this, [this] {
		if (!generator_) return;
		aob_generator::signature_t current;
		{
			std::lock_guard<std::mutex> lk(generator_->mutex);
			current = generator_->current;
		}
		clipboard::set_text(QString::fromStdString(
			aob_generator::format_signature(current)));
	});
	connect(copy_yara_, &widgets::AidaButton::clicked, this, [this] {
		if (!generator_) return;
		aob_generator::signature_t current;
		{
			std::lock_guard<std::mutex> lk(generator_->mutex);
			current = generator_->current;
		}
		clipboard::set_text(QString::fromStdString(
			aob_generator::format_yara_rule(current)));
	});
	connect(format_tabs_, &QTabBar::currentChanged, this, [this](int index) {
		if (view_state_)
			view_state_->active_format = index;
		copy_format_->setText(QStringLiteral("Copy as %1")
			.arg(format_tabs_->tabText(index)));
		pattern_text_->setText(current_format_text());
	});
	connect(retry_button_, &widgets::AidaButton::clicked, this, [this] {
		if (!view_state_)
			return;
		aob_operation_status_t export_status;
		aob_operation_status_t catalog_status;
		aob_operation_status_t comparison_status;
		{
			std::lock_guard<std::mutex> lock(view_state_->operation_mutex);
			export_status = view_state_->export_status;
			catalog_status = view_state_->catalog_status;
			comparison_status = view_state_->comparison_status;
		}
		const auto context = disasm_view::capture_selected_workspace();
		if (comparison_status.terminal != aob_terminal_t::idle)
			AobController::instance().request_comparison(context);
		else if (catalog_status.terminal != aob_terminal_t::idle)
			AobController::instance().request_catalog(context,
				view_state_->last_catalog_save);
		else
			AobController::instance().request_export(context,
				view_state_->last_export_format, view_state_->last_export_path);
	});
	const auto toggle_saved = [this](const QModelIndex& index) {
		if (!view_state_ || !index.isValid()) return;
		const int previous = view_state_->selected_saved;
		const int row = index.row();
		const auto* signature = saved_model_->row_at(row);
		if (!signature) return;
		if (previous == row)
			AobController::instance().select_saved(view_state_, -1, 0, {});
		else
			AobController::instance().select_saved(view_state_, row, signature->address,
				signature->name);
		refresh_presentation();
	};
	const auto activate_saved = [this](const QModelIndex& index) {
		if (!view_state_ || !index.isValid()) return;
		const int row = index.row();
		if (view_state_->selected_saved == row) return;
		const auto* signature = saved_model_->row_at(row);
		if (!signature) return;
		AobController::instance().select_saved(view_state_, row, signature->address,
			signature->name);
		refresh_presentation();
	};
	connect(saved_table_, &QTableView::clicked, this, toggle_saved);
	connect(saved_table_, &QTableView::activated, this, activate_saved);
	connect(saved_table_, &QTableView::customContextMenuRequested, this,
		[this](const QPoint& pos) {
			QModelIndex index = saved_table_->indexAt(pos);
			QPoint global = saved_table_->viewport()->mapToGlobal(pos);
			if (!index.isValid()) {
				index = saved_table_->currentIndex();
				if (index.isValid())
					global = saved_table_->viewport()->mapToGlobal(
						saved_table_->visualRect(index).center());
			}
			on_saved_context(global, index.isValid() ? index.row() : -1, 0);
		});
	connect(&AobController::instance(), &AobController::stateChanged, this,
		[this] { refresh_presentation(); });

	timer_ = new QTimer(this);
	timer_->setInterval(66);
	connect(timer_, &QTimer::timeout, this, [this] { poll_engine(); });
	poll_engine();
}

AobGeneratorWidget::~AobGeneratorWidget() = default;

bool AobGeneratorWidget::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == saved_table_ && forward_table_menu_key(watched, event,
			saved_table_, [this](const QPoint& global_pos, int row, int origin) {
				on_saved_context(global_pos, row, origin);
			}))
		return true;
	return QWidget::eventFilter(watched, event);
}

void AobGeneratorWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	ScanHubController::instance().note_page_shown();
	poll_engine();
	timer_->start();
}

void AobGeneratorWidget::hideEvent(QHideEvent* event)
{
	timer_->stop();
	QWidget::hideEvent(event);
}

void AobGeneratorWidget::poll_engine()
{
	const auto context = disasm_view::capture_selected_workspace();
	generator_ = aob_generator::state_for(context);
	view_state_ = AobController::instance().view_state_for(context);
	if (!generator_ || !view_state_) {
		refresh_presentation();
		return;
	}
	AobController::instance().poll(generator_, view_state_);
	if (generator_->show_no_address_modal) {
		generator_->show_no_address_modal = false;
		std::string message;
		{
			std::lock_guard<std::mutex> lk(generator_->mutex);
			message = generator_->last_error.empty()
				? std::string("No address selected - click an instruction first.")
				: generator_->last_error;
		}
		chrome::toast_error(QString::fromStdString(message), 6.0);
	}
	if (view_state_->render_catalog_generation != observed_catalog_generation_) {
		observed_catalog_generation_ = view_state_->render_catalog_generation;
		saved_model_->adopt(view_state_->render_catalog);
	}
	refresh_presentation();
}

QString AobGeneratorWidget::current_format_text() const
{
	if (!generator_)
		return {};
	aob_generator::signature_t current;
	{
		std::lock_guard<std::mutex> lk(generator_->mutex);
		current = generator_->current;
	}
	if (current.bytes.empty())
		return {};
	return format_for_tab(current,
		view_state_ ? view_state_->active_format : 0);
}

void AobGeneratorWidget::refresh_presentation()
{
	controls_syncing_ = true;
	if (!generator_ || !view_state_) {
		capability_notice_->setTitle(QStringLiteral("No analysis target"));
		capability_notice_->setMessage(QStringLiteral(
			"Open a binary or attach a live target to generate signatures."));
		capability_notice_->setVisible(true);
		form_panel_->setEnabled(false);
		controls_syncing_ = false;
		return;
	}
	form_panel_->setEnabled(true);
	const auto context = disasm_view::capture_selected_workspace();
	const bool live = context.workspace && context.workspace->target_kind() ==
		aida::analysis::target_kind_t::live_snapshot;
	const bool pe = context.workspace && context.workspace->target_kind() ==
		aida::analysis::target_kind_t::static_file && static_cast<bool>(context.image);
	capability_notice_->setTitle(QStringLiteral("No data source"));
	capability_notice_->setMessage(QStringLiteral(
		"Generate needs a live process attach or an open PE."));
	capability_notice_->setVisible(!live && !pe);

	const bool generating = generator_->generating.load();
	generate_button_->setEnabled(!generating);
	generate_button_->setLoading(generating);
	regenerate_button_->setEnabled(!generating);
	const auto process = live ? context.workspace->identity().process() : std::nullopt;
	const std::uint32_t live_pid = process ? process->pid : 0;
	const bool attached_live = driver_bridge::is_loaded() && live_pid != 0;
	optimize_button_->setEnabled(attached_live);
	const bool batch_running = generator_->batch_generating.load();
	if (batch_running) {
		batch_badge_->setText(QStringLiteral("Batch %1/%2")
			.arg(generator_->batch_done.load())
			.arg(generator_->batch_total.load()));
	}
	batch_badge_->setVisible(batch_running);

	if (address_edit_->text().toStdString() != generator_->address_input)
		address_edit_->setText(QString::fromLatin1(generator_->address_input));
	if (name_edit_->text().toStdString() != generator_->name_input)
		name_edit_->setText(QString::fromLatin1(generator_->name_input));
	if (count_spin_->value() != generator_->instruction_count)
		count_spin_->setValue(generator_->instruction_count);
	if (auto_wildcard_->isChecked() != generator_->auto_wildcard)
		auto_wildcard_->setChecked(generator_->auto_wildcard);
	if (validate_uniqueness_->isChecked() != generator_->validate_uniqueness)
		validate_uniqueness_->setChecked(generator_->validate_uniqueness);

	std::string error_copy;
	{
		std::lock_guard<std::mutex> lk(generator_->mutex);
		error_copy = generator_->last_error;
	}
	error_notice_->setMessage(QString::fromStdString(error_copy));
	error_notice_->setVisible(!error_copy.empty());

	export_json_->setEnabled(!view_state_->export_pending.load(std::memory_order_acquire));
	export_yara_->setEnabled(!view_state_->export_pending.load(std::memory_order_acquire));
	export_header_->setEnabled(!view_state_->export_pending.load(std::memory_order_acquire));
	compare_button_->setEnabled(attached_live &&
		!view_state_->comparison_pending.load(std::memory_order_acquire));
	save_disk_->setEnabled(!view_state_->catalog_pending.load(std::memory_order_acquire));
	load_disk_->setEnabled(!view_state_->catalog_pending.load(std::memory_order_acquire));

	aob_operation_status_t export_status;
	aob_operation_status_t catalog_status;
	aob_operation_status_t comparison_status;
	{
		std::lock_guard<std::mutex> lock(view_state_->operation_mutex);
		export_status = view_state_->export_status;
		catalog_status = view_state_->catalog_status;
		comparison_status = view_state_->comparison_status;
	}
	const aob_operation_status_t* visible_status =
		comparison_status.terminal != aob_terminal_t::idle ? &comparison_status :
		catalog_status.terminal != aob_terminal_t::idle ? &catalog_status :
		export_status.terminal != aob_terminal_t::idle ? &export_status : nullptr;
	operation_status_->setText(visible_status
		? QString::fromStdString(visible_status->message) : QString());
	const bool retryable = visible_status &&
		(visible_status->terminal == aob_terminal_t::failed ||
			visible_status->terminal == aob_terminal_t::cancelled ||
			visible_status->terminal == aob_terminal_t::stale);
	retry_button_->setVisible(retryable);

	refresh_signature_card();
	saved_empty_->setVisible(saved_model_->size() == 0);
	saved_table_->setVisible(saved_model_->size() != 0);
	controls_syncing_ = false;
}

void AobGeneratorWidget::refresh_signature_card()
{
	aob_generator::signature_t current;
	{
		std::lock_guard<std::mutex> lk(generator_->mutex);
		current = generator_->current;
	}
	const bool has_signature = !current.bytes.empty();
	signature_card_->setVisible(has_signature);
	byte_grid_->setVisible(has_signature);
	format_tabs_->setVisible(has_signature);
	pattern_text_->setVisible(has_signature);
	save_button_->setEnabled(has_signature);
	copy_format_->setEnabled(has_signature);
	copy_signature_->setEnabled(has_signature);
	copy_yara_->setEnabled(has_signature);
	if (!has_signature) {
		byte_grid_->clear();
		pattern_text_->clear();
		return;
	}
	signature_info_->setText(QStringLiteral("0x%1  |  %2  |  %3 bytes  |  %4%")
		.arg(QStringLiteral("%1").arg(current.address, 16, 16, QLatin1Char('0')).toUpper())
		.arg(current.module_name.empty() ? QStringLiteral("<unknown>")
			: QString::fromStdString(current.module_name))
		.arg(current.bytes.size())
		.arg(static_cast<int>(current.quality_score * 100.f)));
	const QString grade = QStringLiteral("Grade: %1")
		.arg(QString::fromLatin1(aob_generator::score_grade(current.quality_score)));
	grade_label_->setText(grade);
	const char* grade_state = widgets::semantic_state_name(
		grade_semantic(current.quality_score));
	if (grade_label_->property("aidaState").toString() !=
			QLatin1String(grade_state)) {
		grade_label_->setProperty("aidaState", QString::fromLatin1(grade_state));
		theme::stylesheet::repolish(grade_label_);
	}
	byte_grid_->set_bytes(current.bytes);
	pattern_text_->setText(current_format_text());
}

void AobGeneratorWidget::on_saved_context(const QPoint& global_pos, int row, int origin)
{
	if (!view_state_ || !generator_)
		return;
	if (row >= 0) {
		const auto* signature = saved_model_->row_at(row);
		if (signature)
			AobController::instance().select_saved(view_state_, row, signature->address,
				signature->name);
	}
	const int selected = view_state_->selected_saved;
	const auto* signature = saved_model_->row_at(selected);
	if (!signature)
		return;
	show_saved_menu(*signature, disasm_view::capture_selected_workspace(),
		view_state_, global_pos, origin);
}

void AobGeneratorWidget::show_saved_menu(
	const aob_generator::signature_t& signature,
	const disasm_view::workspace_context_t& context,
	const std::shared_ptr<aob_view_state_t>& view_state,
	const QPoint& global_pos, int origin)
{
	if (!context.workspace)
		return;
	const auto workspace = context.workspace;
	const auto generation = context.publication ? context.publication->generation : 0;
	const auto generator_state = generator_;
	aida::ui::application_ui::retained_entity_context_t retained;
	retained.owner_id = "memory.aob.saved";
	retained.entity_id = signature.name + "@" + std::to_string(signature.address);
	retained.entity_generation = generation;
	retained.active_view = aida::ui::stable_view_id_t("view.memory.aob");
	retained.validate_identity = [workspace, generator_state, generation, signature]() {
		if (!workspace) return aida::ui::capability_state_t::unavailable("The AOB workspace was closed.");
		const auto publication = workspace->analysis_publication();
		if (!publication || publication->generation != generation)
			return aida::ui::capability_state_t::unavailable("The analysis publication changed; reopen the menu.");
		std::lock_guard<std::mutex> lock(generator_state->mutex);
		const bool current = std::any_of(generator_state->saved_signatures.begin(),
			generator_state->saved_signatures.end(), [&](const auto& item) {
				return item.address == signature.address && item.name == signature.name &&
					signature_bytes_equal(item.bytes, signature.bytes);
			});
		return current ? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable("The selected signature changed or was removed.");
	};
	auto add = [&](const char* id, bool enabled, const char* reason, auto invoke) {
		retained.actions.push_back({id, enabled ? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(reason), invoke});
	};
	add("memory.entity.open_disassembly", signature.address != 0,
		"The saved signature has no mapped address.", [signature, context]() {
			AobController::instance().host()->open_or_focus(
				registry::stable_view_id_t("document.disassembly"));
			disasm_view::goto_address(signature.address, context);
			diag::log_tagged("scan_audit", "[scan_audit] aob saved ctx open_disasm");
			return aida::ui::action_handler_result_t::completed();
		});
	add("memory.entity.open_hex", signature.address != 0,
		"The saved signature has no mapped address.", [signature, context]() {
			if (context.workspace->target_kind() == aida::analysis::target_kind_t::live_snapshot)
				hex_view::request_live_memory(context, signature.address, 256);
			else
				hex_view::activate(context);
			AobController::instance().host()->open_or_focus(
				registry::stable_view_id_t("document.hex"));
			diag::log_tagged("scan_audit", "[scan_audit] aob saved ctx open_hex");
			return aida::ui::action_handler_result_t::completed();
		});
	add("memory.aob.copy_pattern", !signature.bytes.empty(),
		"The saved signature has no retained pattern bytes.", [signature]() {
			clipboard::set_text(QString::fromStdString(
				aob_generator::format_signature(signature)));
			diag::log_tagged("scan_audit", "[scan_audit] aob saved ctx copy_pattern");
			return aida::ui::action_handler_result_t::completed();
		});
	add("memory.aob.copy_ida_pattern", !signature.bytes.empty(),
		"The saved signature has no retained pattern bytes.", [signature]() {
			clipboard::set_text(QString::fromStdString(
				aob_generator::format_ida_signature(signature)));
			diag::log_tagged("scan_audit", "[scan_audit] aob saved ctx copy_ida");
			return aida::ui::action_handler_result_t::completed();
		});
	add("memory.entity.copy_address", signature.address != 0,
		"The saved signature has no mapped address.", [signature]() {
			char address[24]{};
			std::snprintf(address, sizeof(address), "0x%llX", static_cast<unsigned long long>(signature.address));
			clipboard::set_text(QString::fromLatin1(address));
			diag::log_tagged("scan_audit", "[scan_audit] aob saved ctx copy_address");
			return aida::ui::action_handler_result_t::completed();
		});
	char evidence_address[24]{};
	std::snprintf(evidence_address, sizeof(evidence_address), "0x%016llX",
		static_cast<unsigned long long>(signature.address));
	constexpr std::size_t k_evidence_pattern_bytes = 1024U;
	const std::size_t evidence_byte_count = (std::min)(signature.bytes.size(),
		k_evidence_pattern_bytes);
	std::string evidence_pattern;
	evidence_pattern.reserve(evidence_byte_count * 3U);
	char encoded_byte[4]{};
	for (std::size_t index = 0; index < evidence_byte_count; ++index) {
		if (index != 0) evidence_pattern.push_back(' ');
		if (signature.bytes[index].wildcard) evidence_pattern += "??";
		else {
			std::snprintf(encoded_byte, sizeof(encoded_byte), "%02X",
				signature.bytes[index].value);
			evidence_pattern += encoded_byte;
		}
	}
	std::uint64_t signature_identity_hash = 1469598103934665603ULL;
	for (const auto& byte : signature.bytes) {
		signature_identity_hash ^= byte.value;
		signature_identity_hash *= 1099511628211ULL;
		signature_identity_hash ^= byte.wildcard ? 1U : 0U;
		signature_identity_hash *= 1099511628211ULL;
	}
	const auto signature_id = signature.id;
	const auto signature_address = signature.address;
	const auto signature_name = signature.name;
	aida::automation_ui::entity_evidence::snapshot_t evidence;
	evidence.workspace_id = workspace->identity().binary_id().to_hex();
	evidence.source_view_id = "view.memory.aob";
	evidence.source_kind = "aob_signature";
	evidence.entity_id = retained.entity_id;
	evidence.display_label = signature.name;
	evidence.excerpt = "Name: " + signature.name + "\nAddress: " +
		evidence_address + "\nPattern: " + evidence_pattern +
		"\nByte count: " + std::to_string(signature.bytes.size()) +
		"\nUniqueness count: " + std::to_string(signature.uniqueness_count);
	evidence.address = signature.address;
	evidence.revision = generation;
	evidence.generation = generation;
	evidence.truncated = evidence_byte_count != signature.bytes.size();
	evidence.return_to_source = [workspace, generator_state, view_state,
		generation, signature_id, signature_address, signature_name,
		signature_identity_hash](std::string& reason) {
		const auto publication = workspace ? workspace->analysis_publication() : nullptr;
		if (!publication || publication->generation != generation) {
			reason = "The AOB analysis publication changed; capture the signature again.";
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(generator_state->mutex);
			const auto found = std::find_if(generator_state->saved_signatures.begin(),
				generator_state->saved_signatures.end(), [&](const auto& item) {
					if (item.id != signature_id || item.address != signature_address ||
						item.name != signature_name) return false;
					std::uint64_t current_hash = 1469598103934665603ULL;
					for (const auto& byte : item.bytes) {
						current_hash ^= byte.value;
						current_hash *= 1099511628211ULL;
						current_hash ^= byte.wildcard ? 1U : 0U;
						current_hash *= 1099511628211ULL;
					}
					return current_hash == signature_identity_hash;
				});
			if (found == generator_state->saved_signatures.end()) {
				reason = "The retained AOB signature changed or was removed; capture it again.";
				return false;
			}
			AobController::instance().select_saved(view_state, static_cast<int>(
				std::distance(generator_state->saved_signatures.begin(), found)),
				signature_address, signature_name);
		}
		if (auto* host = AobController::instance().host())
			static_cast<void>(host->open_or_focus(
				registry::stable_view_id_t("view.memory.aob")));
		reason.clear();
		return true;
	};
	aida::automation_ui::entity_evidence::append_actions(retained,
		std::move(evidence), !signature.bytes.empty()
			? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(
				"The retained AOB signature has no pattern bytes."));
	documents::show_retained_entity_menu(std::move(retained),
		origin == 1 ? aida::ui::context_menu_open_origin_t::menu_key
			: origin == 2 ? aida::ui::context_menu_open_origin_t::shift_f10
			: aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}
