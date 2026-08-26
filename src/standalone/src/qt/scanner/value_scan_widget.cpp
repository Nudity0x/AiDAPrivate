#include "qt/scanner/value_scan_widget.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStackedLayout>
#include <QVBoxLayout>

#include <algorithm>

#include "core/disasm/function_index.hpp"
#include "core/runtime/standalone_driver.hpp"

#include "qt/bridge/action_bridge.hpp"
#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/overlays/aida_no_target_overlay.hpp"
#include "qt/scanner/address_list_model.hpp"
#include "qt/scanner/address_list_view.hpp"
#include "qt/scanner/memory_interaction_bridge.hpp"
#include "qt/scanner/scan_results_model.hpp"
#include "qt/scanner/scan_results_view.hpp"
#include "qt/scanner/scan_hub_controller.hpp"
#include "qt/scanner/scanner_context_menus.hpp"
#include "qt/scanner/scanner_controller.hpp"
#include "qt/scanner/scanner_palette.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::scanner {

ValueScanWidget::ValueScanWidget(QWidget* parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("aida.view.memory.value_scan"));
	bridge::InteractionContextProvider::attach_scope(this,
		QStringLiteral("scope.view.memory.value_scan"),
		aida::ui::focus_scope_kind_t::widget);
	auto& controller = ScannerController::instance();
	const auto& tokens = theme::tokens();
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	auto* toolbar = new QFrame(this);
	toolbar->setObjectName(QStringLiteral("aida.view.memory.value_scan.toolbar"));
	toolbar->setProperty("aidaRole", QStringLiteral("toolbar"));
	auto* bar = new QHBoxLayout(toolbar);
	bar->setContentsMargins(tokens.toolbar.padding_x, tokens.toolbar.padding_y,
		tokens.toolbar.padding_x, tokens.toolbar.padding_y);
	bar->setSpacing(tokens.toolbar.group_gap);

	const int cell = mono_cell_width();
	type_combo_ = new QComboBox(toolbar);
	type_combo_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.type_combo"));
	type_combo_->setToolTip(QStringLiteral("Value type to scan for"));
	for (int i = 0; i < static_cast<int>(memory_scanner::value_type_t::COUNT); ++i)
		type_combo_->addItem(QString::fromLatin1(memory_scanner::value_type_name(
			static_cast<memory_scanner::value_type_t>(i))), i);
	bar->addWidget(type_combo_);

	mode_combo_ = new QComboBox(toolbar);
	mode_combo_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.mode_combo"));
	mode_combo_->setToolTip(QStringLiteral("Scan comparison mode"));
	for (int i = 0; i < static_cast<int>(memory_scanner::scan_mode_t::COUNT); ++i)
		mode_combo_->addItem(QString::fromLatin1(memory_scanner::scan_mode_name(
			static_cast<memory_scanner::scan_mode_t>(i))), i);
	bar->addWidget(mode_combo_);

	value_edit_ = new QLineEdit(toolbar);
	value_edit_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.value_edit"));
	value_edit_->setPlaceholderText(QStringLiteral("value"));
	value_edit_->setMaxLength(255);
	value_edit_->setMinimumWidth(2 * tokens.table.cell_pad_x + 18 * cell);
	value_edit_->setFont(theme::fonts::codeRegular());
	value_edit_->setToolTip(QStringLiteral("Value to find (decimal, or hex with HEX checked)"));
	bar->addWidget(value_edit_);

	to_label_ = new QLabel(QStringLiteral("to"), toolbar);
	to_label_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.to_label"));
	to_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
	bar->addWidget(to_label_);
	value_edit2_ = new QLineEdit(toolbar);
	value_edit2_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.value_edit_max"));
	value_edit2_->setPlaceholderText(QStringLiteral("max"));
	value_edit2_->setMaxLength(63);
	value_edit2_->setMinimumWidth(2 * tokens.table.cell_pad_x + 12 * cell);
	value_edit2_->setFont(theme::fonts::codeRegular());
	value_edit2_->setToolTip(QStringLiteral("Upper bound for the Between scan mode"));
	bar->addWidget(value_edit2_);

	hex_check_ = new QCheckBox(QStringLiteral("HEX"), toolbar);
	hex_check_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.hex_check"));
	hex_check_->setToolTip(QStringLiteral("Interpret the value fields as hexadecimal"));
	bar->addWidget(hex_check_);

	source_pill_ = new QPushButton(toolbar);
	source_pill_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.source_pill"));
	source_pill_->setCheckable(true);
	source_pill_->setToolTip(QStringLiteral(
		"Switch the next scan between the attached process and static PE image"));
	bar->addWidget(source_pill_);

	first_button_ = new QPushButton(toolbar);
	first_button_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.first_scan"));
	next_button_ = new QPushButton(toolbar);
	next_button_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.next_scan"));
	stop_button_ = new QPushButton(toolbar);
	stop_button_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.stop_scan"));
	undo_button_ = new QPushButton(toolbar);
	undo_button_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.undo_scan"));
	new_button_ = new QPushButton(toolbar);
	new_button_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.new_scan"));
	bar->addWidget(first_button_);
	bar->addWidget(next_button_);
	bar->addWidget(stop_button_);
	bar->addWidget(undo_button_);
	bar->addWidget(new_button_);

	bar->addStretch(1);
	count_label_ = new QLabel(toolbar);
	count_label_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.count"));
	count_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
	bar->addWidget(count_label_);
	progress_ = new QProgressBar(toolbar);
	progress_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.progress"));
	progress_->setRange(0, 100);
	progress_->setFixedWidth(16 * cell);
	progress_->setVisible(false);
	bar->addWidget(progress_);
	layout->addWidget(toolbar);

	callout_ = new widgets::AidaNotice(QString(), QString(),
		widgets::AidaSemantic::Warning, this);
	callout_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.callout"));
	callout_->setVisible(false);
	layout->addWidget(callout_);

	splitter_ = new QSplitter(Qt::Vertical, this);
	splitter_->setObjectName(QStringLiteral("aida.view.memory.value_scan.splitter"));
	splitter_->setOpaqueResize(true);
	splitter_->setChildrenCollapsible(false);

	auto* results_container = new QWidget(splitter_);
	results_stack_ = new QStackedLayout(results_container);
	results_stack_->setContentsMargins(0, 0, 0, 0);
	results_stack_->setStackingMode(QStackedLayout::StackOne);
	results_view_ = new ScanResultsView(results_container);
	results_bridge_ = new MemoryInteractionBridge(this,
		QStringLiteral("view.memory.value_scan"),
		memory_interaction::kind_t::scan_result);
	results_bridge_->set_runtime_source([] {
		return ScannerController::instance().runtime_snapshot();
	});
	results_view_->bind(controller.results_model(), results_bridge_,
		QStringLiteral("view.memory.value_scan"));
	results_stack_->addWidget(results_view_);
	results_empty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QStringLiteral("Ready to scan"), QString(), results_container);
	results_empty_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.results_empty"));
	results_empty_->setActionLabel(QStringLiteral("Run First Scan"));
	connect(results_empty_, &widgets::AidaStateView::actionTriggered, this, [] {
		ScannerController::instance().execute_command(scan_command_t::first_scan);
	});
	results_stack_->addWidget(results_empty_);
	splitter_->addWidget(results_container);

	auto* address_pane = new QWidget(splitter_);
	auto* address_layout = new QVBoxLayout(address_pane);
	address_layout->setContentsMargins(0, 0, 0, 0);
	address_layout->setSpacing(0);
	auto* address_header = new QFrame(address_pane);
	address_header->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.address_header"));
	address_header->setProperty("aidaRole", QStringLiteral("header"));
	address_header->setFixedHeight(tokens.control.height_md);
	auto* header_layout = new QHBoxLayout(address_header);
	header_layout->setContentsMargins(tokens.spacing.md, 0,
		tokens.spacing.md, 0);
	header_layout->setSpacing(tokens.spacing.sm);
	auto* title = new QLabel(QStringLiteral("Address List"), address_header);
	title->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.address_title"));
	header_layout->addWidget(title);
	address_count_label_ = new QLabel(address_header);
	address_count_label_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.address_count"));
	address_count_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
	header_layout->addWidget(address_count_label_);
	header_layout->addStretch(1);
	auto* add_button = new QPushButton(QStringLiteral("Add Address"), address_header);
	add_button->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.add_address"));
	header_layout->addWidget(add_button);
	auto_refresh_check_ = new QCheckBox(QStringLiteral("Auto-refresh"), address_header);
	auto_refresh_check_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.auto_refresh"));
	auto_refresh_check_->setToolTip(QStringLiteral(
		"Automatically refresh retained address values"));
	header_layout->addWidget(auto_refresh_check_);
	address_layout->addWidget(address_header);
	auto* address_container = new QWidget(address_pane);
	address_stack_ = new QStackedLayout(address_container);
	address_stack_->setContentsMargins(0, 0, 0, 0);
	address_stack_->setStackingMode(QStackedLayout::StackOne);
	address_view_ = new AddressListView(address_container);
	address_bridge_ = new MemoryInteractionBridge(this,
		QStringLiteral("view.memory.value_scan"),
		memory_interaction::kind_t::address_entry);
	address_bridge_->set_runtime_source([] {
		return ScannerController::instance().runtime_snapshot();
	});
	address_view_->bind(controller.address_model(), address_bridge_,
		QStringLiteral("view.memory.value_scan"));
	address_stack_->addWidget(address_view_);
	address_empty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QStringLiteral("Address list is empty"),
		QStringLiteral("Double-click a scan result or use Add Address to retain one."),
		address_container);
	address_empty_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan.address_empty"));
	address_empty_->setActionLabel(QStringLiteral("Add Address"));
	connect(address_empty_, &widgets::AidaStateView::actionTriggered, this,
		[add_button] { add_button->click(); });
	address_stack_->addWidget(address_empty_);
	address_layout->addWidget(address_container, 1);
	splitter_->addWidget(address_pane);
	splitter_->setStretchFactor(0, 6);
	splitter_->setStretchFactor(1, 4);
	layout->addWidget(splitter_, 1);

	connect(type_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (!refreshing_controls_)
			ScannerController::instance().set_scan_value_type(index);
	});
	connect(mode_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (!refreshing_controls_) {
			ScannerController::instance().set_scan_mode(index);
			update_value_fields();
		}
	});
	const auto value_edited = [this] {
		if (!refreshing_controls_)
			ScannerController::instance().set_value_text(
				value_edit_->text(), value_edit2_->text());
	};
	connect(value_edit_, &QLineEdit::textChanged, this,
		[this, value_edited](const QString&) { value_edited(); });
	connect(value_edit2_, &QLineEdit::textChanged, this,
		[this, value_edited](const QString&) { value_edited(); });
	connect(hex_check_, &QCheckBox::toggled, this, [this](bool checked) {
		if (!refreshing_controls_)
			ScannerController::instance().set_hex_input(checked);
	});
	connect(source_pill_, &QPushButton::clicked, this, [this](bool checked) {
		ScannerController::instance().set_prefer_static_source(checked);
	});
	connect(add_button, &QPushButton::clicked, this, [this] {
		auto& controller = ScannerController::instance();
		auto& state = memory_scanner::g_state;
		int value_type = 0;
		{
			std::lock_guard<std::mutex> lock(state.results_mutex);
			value_type = static_cast<int>(state.config.value_type);
		}
		controller.open_add_dialog(0, value_type, this);
	});
	connect(auto_refresh_check_, &QCheckBox::toggled, this, [](bool checked) {
		ScannerController::instance().set_auto_refresh(checked);
	});

	bind_action(first_button_, QStringLiteral("memory.first_scan"));
	bind_action(next_button_, QStringLiteral("memory.next_scan"));
	bind_action(stop_button_, QStringLiteral("memory.stop_scan"));
	bind_action(undo_button_, QStringLiteral("memory.undo_scan"));
	bind_action(new_button_, QStringLiteral("memory.new_scan"));

	connect(results_view_, &ScanResultsView::sortRequested, this,
		[](int logical) {
			ScannerController::instance().sort_column_clicked(logical);
		});
	connect(results_view_, &ScanResultsView::rowActivated, this,
		[this](int source_row) {
			auto* model = ScannerController::instance().results_model();
			const auto* result = model->result_at_source(source_row);
			if (!result)
				return;
			auto& state = memory_scanner::g_state;
			int value_type = 0;
			{
				std::lock_guard<std::mutex> lock(state.results_mutex);
				value_type = static_cast<int>(state.config.value_type);
			}
			ScannerController::instance().open_add_dialog(result->address,
				value_type, this);
		});
	connect(results_view_, &ScanResultsView::contextMenuRequested, this,
		[this](const QPoint& global_pos, int source_row, int origin) {
			auto& controller = ScannerController::instance();
			const auto runtime = controller.runtime_snapshot();
			auto context = results_bridge_->capture_row(
				controller.results_model()->view_row_for_source(source_row));
			if (context.kind == memory_interaction::kind_t::none)
				return;
			show_result_context_menu(context, runtime,
				QStringLiteral("view.memory.value_scan"),
				origin == 1 ? aida::ui::context_menu_open_origin_t::menu_key
					: origin == 2 ? aida::ui::context_menu_open_origin_t::shift_f10
					: aida::ui::context_menu_open_origin_t::pointer, global_pos,
				results_view_);
		});
	connect(address_view_, &AddressListView::freezeToggleRequested, this,
		[](int row) { ScannerController::instance().freeze_toggled(row); });
	connect(address_view_, &AddressListView::editDescriptionRequested, this,
		[this](int row) {
			ScannerController::instance().open_edit_description_dialog(row, this);
		});
	connect(address_view_, &AddressListView::changeTypeRequested, this,
		[this](int row) {
			ScannerController::instance().open_change_type_dialog(row, this);
		});
	connect(address_view_, &AddressListView::changeValueRequested, this,
		[this](int row) {
			ScannerController::instance().open_change_value_dialog(row, this);
		});
	connect(address_view_, &AddressListView::removeRequested, this,
		[](const std::vector<int>& rows) {
			ScannerController::instance().remove_addresses(rows);
		});
	connect(address_view_, &AddressListView::contextMenuRequested, this,
		[this](const QPoint& global_pos, int row, int origin) {
			auto& controller = ScannerController::instance();
			const auto runtime = controller.runtime_snapshot();
			auto context = address_bridge_->capture_row(row);
			if (context.kind == memory_interaction::kind_t::none)
				return;
			show_address_context_menu(context, runtime,
				QStringLiteral("view.memory.value_scan"),
				origin == 1 ? aida::ui::context_menu_open_origin_t::menu_key
					: origin == 2 ? aida::ui::context_menu_open_origin_t::shift_f10
					: aida::ui::context_menu_open_origin_t::pointer, global_pos,
				address_view_);
		});

	connect(&controller, &ScannerController::stateChanged, this,
		[this] { refresh_state(); });
	connect(&controller, &ScannerController::scanProgressed, this,
		[this](double progress, const QString&) {
			progress_->setValue(static_cast<int>(progress * 100.0));
		});
	connect(&controller, &ScannerController::scanStateEdge, results_bridge_,
		&MemoryInteractionBridge::synchronize_now);
	connect(&controller, &ScannerController::scanStateEdge, address_bridge_,
		&MemoryInteractionBridge::synchronize_now);
	connect(&controller, &ScannerController::persistedConfigReplayed, this,
		[this] { refresh_state(); });

	refresh_state();
}

ValueScanWidget::~ValueScanWidget() = default;

void ValueScanWidget::bind_action(QPushButton* button, const QString& action_id)
{
	auto* actions = ScannerController::instance().actions();
	QAction* action = actions
		? actions->surface_action(action_id,
			aida::ui::action_invocation_source_t::toolbar, button)
		: nullptr;
	if (action) {
		button->setText(action->text());
		button->setToolTip(action->toolTip());
		button->setEnabled(action->isEnabled());
		connect(action, &QAction::changed, button, [button, action] {
			button->setEnabled(action->isEnabled());
			button->setToolTip(action->toolTip());
		});
		connect(button, &QPushButton::clicked, action, &QAction::trigger);
		return;
	}
	const auto command = action_id == QStringLiteral("memory.first_scan")
		? scan_command_t::first_scan
		: action_id == QStringLiteral("memory.next_scan")
		? scan_command_t::next_scan
		: action_id == QStringLiteral("memory.stop_scan")
		? scan_command_t::stop_scan
		: action_id == QStringLiteral("memory.undo_scan")
		? scan_command_t::undo_scan
		: scan_command_t::new_scan;
	button->setText(command == scan_command_t::first_scan ? QStringLiteral("First Scan")
		: command == scan_command_t::next_scan ? QStringLiteral("Next Scan")
		: command == scan_command_t::stop_scan ? QStringLiteral("Stop Scan")
		: command == scan_command_t::undo_scan ? QStringLiteral("Undo Scan")
		: QStringLiteral("New Scan"));
	fallback_commands_.insert(button, command);
	connect(button, &QPushButton::clicked, this, [command] {
		ScannerController::instance().execute_command(command);
	});
}

void ValueScanWidget::update_value_fields()
{
	auto& state = memory_scanner::g_state;
	memory_scanner::scan_mode_t mode;
	{
		std::lock_guard<std::mutex> lock(state.results_mutex);
		mode = state.config.scan_mode;
	}
	const bool needs_value = mode != memory_scanner::scan_mode_t::changed &&
		mode != memory_scanner::scan_mode_t::unchanged &&
		mode != memory_scanner::scan_mode_t::increased &&
		mode != memory_scanner::scan_mode_t::decreased &&
		mode != memory_scanner::scan_mode_t::unknown_initial;
	const bool between = mode == memory_scanner::scan_mode_t::value_between;
	value_edit_->setVisible(needs_value);
	to_label_->setVisible(needs_value && between);
	value_edit2_->setVisible(needs_value && between);
}

void ValueScanWidget::refresh_state()
{
	refreshing_controls_ = true;
	auto& controller = ScannerController::instance();
	auto& state = memory_scanner::g_state;
	memory_scanner::value_type_t value_type;
	memory_scanner::scan_mode_t scan_mode;
	bool hex_input = false;
	bool has_initial_scan = false;
	std::size_t total_found = 0;
	{
		std::lock_guard<std::mutex> lock(state.results_mutex);
		value_type = state.config.value_type;
		scan_mode = state.config.scan_mode;
		hex_input = state.config.hex_input;
		has_initial_scan = state.has_initial_scan;
		total_found = state.total_found;
	}
	const bool scanning = state.scanning.load(std::memory_order_acquire);
	const bool attached = driver_bridge::is_loaded() &&
		driver_bridge::attached_pid() != 0;
	const bool static_pe = function_index::detail::static_pe_active();

	type_combo_->setCurrentIndex(static_cast<int>(value_type));
	mode_combo_->setCurrentIndex(static_cast<int>(scan_mode));
	if (value_edit_->text() != controller.value_text())
		value_edit_->setText(controller.value_text());
	if (value_edit2_->text() != controller.value_text2())
		value_edit2_->setText(controller.value_text2());
	hex_check_->setChecked(hex_input);

	const bool static_selected = controller.prefer_static_source() || !attached;
	source_pill_->setText(static_selected ? QStringLiteral("Static binary")
		: QStringLiteral("Live process"));
	source_pill_->setChecked(static_selected);
	source_pill_->setVisible(static_pe);
	source_pill_->setEnabled(attached && !has_initial_scan && !scanning);

	first_button_->setVisible(!scanning && !has_initial_scan);
	next_button_->setVisible(!scanning && has_initial_scan);
	stop_button_->setVisible(scanning);

	count_label_->setText(QStringLiteral("%1 found").arg(total_found));
	progress_->setVisible(scanning);

	if (!attached) {
		callout_->setTitle(static_pe
			? QStringLiteral("Static binary loaded")
			: QStringLiteral("No live process attached"));
		callout_->setMessage(static_pe
			? QStringLiteral("Value scans and watch mutations require a live process attach.")
			: QStringLiteral("Value scan needs a live process. Attach a target from the Process Attach panel."));
	}
	callout_->setVisible(!attached);

	const std::size_t result_rows =
		static_cast<std::size_t>(controller.results_model()->rowCount());
	if (result_rows == 0) {
		const auto first_capability =
			controller.command_capability(scan_command_t::first_scan);
		if (!attached && !static_pe) {
			results_empty_->setTitle(QStringLiteral("No memory target"));
			results_empty_->setMessage(QStringLiteral(
				"Attach a running process or open a binary to scan its memory."));
			results_empty_->setActionLabel(QString());
		} else if (!has_initial_scan) {
			results_empty_->setTitle(QStringLiteral("Ready to scan"));
			results_empty_->setMessage(QStringLiteral(
				"Pick a value type and comparison above, then run the first scan."));
			results_empty_->setActionLabel(first_capability.enabled
				? QStringLiteral("Run First Scan") : QString());
		} else {
			results_empty_->setTitle(QStringLiteral("No matching addresses"));
			results_empty_->setMessage(scanning
				? QStringLiteral("Scanning memory...")
				: QStringLiteral("The last scan matched nothing. Adjust the value or start a New Scan."));
			results_empty_->setActionLabel(QString());
		}
	}
	results_stack_->setCurrentWidget(result_rows == 0
		? static_cast<QWidget*>(results_empty_) : static_cast<QWidget*>(results_view_));

	const std::size_t address_count =
		controller.address_model()->entry_count();
	address_count_label_->setText(QStringLiteral("%1 items").arg(address_count));
	address_stack_->setCurrentWidget(address_count == 0
		? static_cast<QWidget*>(address_empty_) : static_cast<QWidget*>(address_view_));
	if (auto_refresh_check_->isChecked() != controller.auto_refresh())
		auto_refresh_check_->setChecked(controller.auto_refresh());
	refreshing_controls_ = false;
	update_value_fields();
	refresh_action_buttons();
}

void ValueScanWidget::refresh_action_buttons()
{
	auto& controller = ScannerController::instance();
	controller.refresh_actions();
	for (auto it = fallback_commands_.constBegin();
		it != fallback_commands_.constEnd(); ++it) {
		const auto capability = controller.command_capability(it.value());
		it.key()->setEnabled(capability.enabled);
		it.key()->setToolTip(capability.enabled ? QString()
			: QString::fromStdString(capability.disabled_reason));
	}
	results_view_->sync_sort_indicator(
		static_cast<int>(controller.sort_field()), controller.sort_descending());
}

void ValueScanWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	ScanHubController::instance().note_page_shown();
	ScannerController::instance().register_visible_view();
	results_bridge_->set_visible(true);
	address_bridge_->set_visible(true);
	refresh_state();
}

void ValueScanWidget::hideEvent(QHideEvent* event)
{
	results_bridge_->set_visible(false);
	address_bridge_->set_visible(false);
	ScannerController::instance().unregister_visible_view();
	QWidget::hideEvent(event);
}

ValueScanResultsDockWidget::ValueScanResultsDockWidget(QWidget* parent)
	: QWidget(parent)
{
	setObjectName(QStringLiteral("aida.view.memory.value_scan_results"));
	auto& controller = ScannerController::instance();
	const auto& tokens = theme::tokens();
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	status_strip_ = new QFrame(this);
	status_strip_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan_results.status_strip"));
	status_strip_->setProperty("aidaRole", QStringLiteral("statusbar"));
	status_strip_->setFixedHeight(tokens.toolbar.height);
	auto* strip_layout = new QHBoxLayout(status_strip_);
	strip_layout->setContentsMargins(tokens.status_bar.padding_x, 0,
		tokens.status_bar.padding_x, 0);
	strip_layout->setSpacing(tokens.status_bar.item_gap);
	stop_button_ = new QPushButton(QStringLiteral("Stop"), status_strip_);
	stop_button_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan_results.stop"));
	stop_button_->setToolTip(QStringLiteral("Cancel the running memory scan"));
	strip_layout->addWidget(stop_button_);
	progress_label_ = new QLabel(status_strip_);
	progress_label_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan_results.progress_label"));
	progress_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
	strip_layout->addWidget(progress_label_, 1);
	layout->addWidget(status_strip_);

	stack_ = new QStackedLayout();
	stack_->setStackingMode(QStackedLayout::StackOne);
	view_ = new ScanResultsView(this);
	bridge_ = new MemoryInteractionBridge(this,
		QStringLiteral("view.memory.value_scan_results"),
		memory_interaction::kind_t::scan_result);
	bridge_->set_runtime_source([] {
		return ScannerController::instance().runtime_snapshot();
	});
	view_->bind(controller.results_model(), bridge_,
		QStringLiteral("view.memory.value_scan_results"));
	stack_->addWidget(view_);
	overlays::AidaNoTargetConfig overlay_config;
	overlay_config.glyph = overlays::AidaGlyph::Memory;
	overlay_config.title = QStringLiteral("No memory target available");
	overlay_config.subtitle = QStringLiteral(
		"Attach to a running process for live scans, or open a binary for static value discovery.");
	overlay_config.stable_id = QStringLiteral("no_target.memory.value_scan_results");
	overlay_ = new overlays::AidaNoTargetOverlay(overlay_config, this);
	stack_->addWidget(overlay_);
	empty_view_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QStringLiteral("No scan results"),
		QStringLiteral("Run a first scan from the Value Scan page to populate this table."),
		this);
	empty_view_->setObjectName(
		QStringLiteral("aida.view.memory.value_scan_results.empty"));
	stack_->addWidget(empty_view_);
	layout->addLayout(stack_, 1);

	connect(stop_button_, &QPushButton::clicked, this, [] {
		ScannerController::instance().execute_command(scan_command_t::stop_scan);
	});
	connect(view_, &ScanResultsView::sortRequested, this, [](int logical) {
		ScannerController::instance().sort_column_clicked(logical);
	});
	connect(view_, &ScanResultsView::contextMenuRequested, this,
		[this](const QPoint& global_pos, int source_row, int origin) {
			auto& controller = ScannerController::instance();
			const auto runtime = controller.runtime_snapshot();
			auto context = bridge_->capture_row(
				controller.results_model()->view_row_for_source(source_row));
			if (context.kind == memory_interaction::kind_t::none)
				return;
			show_result_context_menu(context, runtime,
				QStringLiteral("view.memory.value_scan_results"),
				origin == 1 ? aida::ui::context_menu_open_origin_t::menu_key
					: origin == 2 ? aida::ui::context_menu_open_origin_t::shift_f10
					: aida::ui::context_menu_open_origin_t::pointer, global_pos, view_);
		});
	connect(view_, &ScanResultsView::rowActivated, this,
		[this](int source_row) {
			auto* model = ScannerController::instance().results_model();
			const auto* result = model->result_at_source(source_row);
			if (!result)
				return;
			auto& state = memory_scanner::g_state;
			int value_type = 0;
			{
				std::lock_guard<std::mutex> lock(state.results_mutex);
				value_type = static_cast<int>(state.config.value_type);
			}
			ScannerController::instance().open_add_dialog(result->address,
				value_type, this);
		});
	connect(&controller, &ScannerController::stateChanged, this,
			[this] { refresh_state(); });
	connect(&controller, &ScannerController::scanProgressed, this,
			[this](double progress, const QString&) {
				progress_label_->setText(QStringLiteral("Scanning memory... %1%")
					.arg(static_cast<int>(progress * 100.0)));
			});
	connect(controller.results_model(), &QAbstractItemModel::modelReset, this,
			[this] { refresh_state(); });
	connect(&controller, &ScannerController::scanStateEdge, bridge_,
		&MemoryInteractionBridge::synchronize_now);
	refresh_state();
}

void ValueScanResultsDockWidget::refresh_state()
{
	auto& state = memory_scanner::g_state;
	const bool scanning = state.scanning.load(std::memory_order_acquire);
	const bool attached = driver_bridge::is_loaded() &&
		driver_bridge::attached_pid() != 0;
	const bool static_pe = function_index::detail::static_pe_active();
	const bool empty = ScannerController::instance().results_model()->rowCount() == 0;
	status_strip_->setVisible(scanning);
	if (scanning) {
		const int progress = static_cast<int>((std::clamp)(
			state.scan_progress.load(std::memory_order_acquire), 0.f, 1.f) * 100.f);
		progress_label_->setText(QStringLiteral("Scanning memory... %1%").arg(progress));
	}
	if (!empty)
		stack_->setCurrentWidget(view_);
	else if (!attached && !static_pe)
		stack_->setCurrentWidget(overlay_);
	else
		stack_->setCurrentWidget(empty_view_);
	auto& controller = ScannerController::instance();
	view_->sync_sort_indicator(static_cast<int>(controller.sort_field()),
		controller.sort_descending());
}

void ValueScanResultsDockWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	ScannerController::instance().register_visible_view();
	bridge_->set_visible(true);
	refresh_state();
}

void ValueScanResultsDockWidget::hideEvent(QHideEvent* event)
{
	bridge_->set_visible(false);
	ScannerController::instance().unregister_visible_view();
	QWidget::hideEvent(event);
}

AddressListDockWidget::AddressListDockWidget(QWidget* parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("aida.view.memory.address_list"));
	auto& controller = ScannerController::instance();
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	stack_ = new QStackedLayout();
	stack_->setStackingMode(QStackedLayout::StackOne);
	view_ = new AddressListView(this);
	bridge_ = new MemoryInteractionBridge(this,
		QStringLiteral("view.memory.address_list"),
		memory_interaction::kind_t::address_entry);
	bridge_->set_runtime_source([] {
		return ScannerController::instance().runtime_snapshot();
	});
	view_->bind(controller.address_model(), bridge_,
		QStringLiteral("view.memory.address_list"));
	stack_->addWidget(view_);
	overlays::AidaNoTargetConfig overlay_config;
	overlay_config.glyph = overlays::AidaGlyph::Memory;
	overlay_config.title = QStringLiteral("No live memory target attached");
	overlay_config.subtitle = QStringLiteral(
		"Attach to a running process or launch a target to build and edit a live address list.");
	overlay_config.stable_id = QStringLiteral("no_target.memory.address_list");
	overlay_ = new overlays::AidaNoTargetOverlay(overlay_config, this);
	stack_->addWidget(overlay_);
	empty_view_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QStringLiteral("Address list is empty"),
		QStringLiteral("Add scan results from the Value Scan page to watch and edit live values."),
		this);
	empty_view_->setObjectName(
		QStringLiteral("aida.view.memory.address_list.empty"));
	stack_->addWidget(empty_view_);
	layout->addLayout(stack_, 1);

	connect(view_, &AddressListView::freezeToggleRequested, this,
		[](int row) { ScannerController::instance().freeze_toggled(row); });
	connect(view_, &AddressListView::editDescriptionRequested, this,
		[this](int row) {
			ScannerController::instance().open_edit_description_dialog(row, this);
		});
	connect(view_, &AddressListView::changeTypeRequested, this,
		[this](int row) {
			ScannerController::instance().open_change_type_dialog(row, this);
		});
	connect(view_, &AddressListView::changeValueRequested, this,
		[this](int row) {
			ScannerController::instance().open_change_value_dialog(row, this);
		});
	connect(view_, &AddressListView::removeRequested, this,
		[](const std::vector<int>& rows) {
			ScannerController::instance().remove_addresses(rows);
		});
	connect(view_, &AddressListView::contextMenuRequested, this,
		[this](const QPoint& global_pos, int row, int origin) {
			auto& controller = ScannerController::instance();
			const auto runtime = controller.runtime_snapshot();
			auto context = bridge_->capture_row(row);
			if (context.kind == memory_interaction::kind_t::none)
				return;
			show_address_context_menu(context, runtime,
				QStringLiteral("view.memory.address_list"),
				origin == 1 ? aida::ui::context_menu_open_origin_t::menu_key
					: origin == 2 ? aida::ui::context_menu_open_origin_t::shift_f10
					: aida::ui::context_menu_open_origin_t::pointer, global_pos, view_);
		});
	connect(&controller, &ScannerController::stateChanged, this,
			[this] { refresh_state(); });
	connect(&controller, &ScannerController::addressListChanged, this,
			[this] { refresh_state(); });
	connect(&controller, &ScannerController::scanStateEdge, bridge_,
		&MemoryInteractionBridge::synchronize_now);
	refresh_state();
}

void AddressListDockWidget::refresh_state()
{
	const bool attached = driver_bridge::is_loaded() &&
		driver_bridge::attached_pid() != 0;
	const bool empty =
		ScannerController::instance().address_model()->rowCount() == 0;
	if (!empty)
		stack_->setCurrentWidget(view_);
	else if (!attached)
		stack_->setCurrentWidget(overlay_);
	else
		stack_->setCurrentWidget(empty_view_);
}

void AddressListDockWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	ScannerController::instance().register_visible_view();
	bridge_->set_visible(true);
	refresh_state();
}

void AddressListDockWidget::hideEvent(QHideEvent* event)
{
	bridge_->set_visible(false);
	ScannerController::instance().unregister_visible_view();
	QWidget::hideEvent(event);
}

}
