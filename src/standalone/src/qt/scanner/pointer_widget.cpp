#include "qt/scanner/pointer_widget.hpp"

#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QSpinBox>
#include <QSplitter>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/ai/entity_evidence_handoff.hpp"
#include "core/debugger/debugger_view.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/disasm/function_index.hpp"
#include "core/editor/hex_view.hpp"
#include "core/scanner/memory_interaction_context.hpp"
#include "core/scanner/memory_scanner.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"

#include "qt/bridge/clipboard.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/scanner/pointer_chain_model.hpp"
#include "qt/scanner/pointer_controller.hpp"
#include "qt/scanner/chain_diagram_widget.hpp"
#include "qt/scanner/scan_hub_controller.hpp"
#include "qt/scanner/scanner_context_menus.hpp"
#include "qt/scanner/scanner_controller.hpp"
#include "qt/scanner/scanner_palette.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_headers.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::scanner {

PointerScannerWidget::PointerScannerWidget(QWidget* parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("aida.view.memory.pointers"));
	const auto& tokens = theme::tokens();
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	view_header_ = new widgets::AidaViewHeader(
		QStringLiteral("Pointer Chain Scanner"), QString(), this);
	view_header_->setObjectName(QStringLiteral("aida.view.memory.pointers.header"));
	layout->addWidget(view_header_);

	auto* splitter = new QSplitter(Qt::Horizontal, this);
	splitter->setObjectName(QStringLiteral("aida.view.memory.pointers.splitter"));
	splitter->setOpaqueResize(true);
	splitter->setChildrenCollapsible(false);

	auto* config = new QWidget(splitter);
	config->setObjectName(QStringLiteral("aida.view.memory.pointers.config"));
	const QFontMetricsF body_metrics(theme::fonts::body());
	const int em = static_cast<int>(body_metrics.horizontalAdvance(
		QLatin1Char('M')) + 0.5);
	config->setMinimumWidth(22 * em);
	config->setMaximumWidth(28 * em);
	auto* form = new QFormLayout(config);
	form->setContentsMargins(tokens.spacing.lg, tokens.spacing.lg,
		tokens.spacing.lg, tokens.spacing.lg);
	form->setSpacing(tokens.spacing.sm);

	capability_notice_ = new widgets::AidaNotice(QStringLiteral("No live target"),
		QStringLiteral("Pointer chain scanning requires a live attach."),
		widgets::AidaSemantic::Warning, config);
	capability_notice_->setObjectName(
		QStringLiteral("aida.view.memory.pointers.capability_notice"));
	capability_notice_->setVisible(false);
	form->addRow(capability_notice_);

	form->addRow(new QLabel(QStringLiteral("Configuration"), config));
	address_edit_ = new QLineEdit(config);
	address_edit_->setObjectName(QStringLiteral("aida.view.memory.pointers.address"));
	address_edit_->setPlaceholderText(QStringLiteral("e.g. 7FF60012A440"));
	address_edit_->setMaxLength(19);
	address_edit_->setToolTip(QStringLiteral(
		"Hexadecimal address the pointer chains must resolve to"));
	address_edit_->setFont(theme::fonts::codeRegular());
	form->addRow(QStringLiteral("Target Address"), address_edit_);

	staged_card_ = new QFrame(config);
	staged_card_->setObjectName(QStringLiteral("aida.view.memory.pointers.staged"));
	staged_card_->setProperty("aidaRole", QStringLiteral("panel"));
	auto* staged_layout = new QVBoxLayout(staged_card_);
	staged_layout->setContentsMargins(tokens.spacing.md, tokens.spacing.sm,
		tokens.spacing.md, tokens.spacing.sm);
	staged_layout->setSpacing(tokens.spacing.xs);
	staged_status_ = new QLabel(staged_card_);
	staged_status_->setObjectName(QStringLiteral("aida.view.memory.pointers.staged_status"));
	staged_status_->setWordWrap(true);
	staged_layout->addWidget(staged_status_);
	staged_identity_ = new QLabel(staged_card_);
	staged_identity_->setObjectName(
		QStringLiteral("aida.view.memory.pointers.staged_identity"));
	staged_identity_->setFont(theme::fonts::codeRegular());
	staged_layout->addWidget(staged_identity_);
	auto* staged_buttons = new QHBoxLayout();
	staged_apply_ = new widgets::AidaButton(QStringLiteral("Use Target"), staged_card_);
	staged_apply_->setObjectName(QStringLiteral("aida.view.memory.pointers.staged_apply"));
	staged_apply_->setKind(widgets::AidaButton::Kind::Primary);
	auto* staged_recheck = new widgets::AidaButton(QStringLiteral("Recheck"), staged_card_);
	staged_recheck->setObjectName(QStringLiteral("aida.view.memory.pointers.staged_recheck"));
	auto* staged_dismiss = new widgets::AidaButton(QStringLiteral("Dismiss"), staged_card_);
	staged_dismiss->setObjectName(QStringLiteral("aida.view.memory.pointers.staged_dismiss"));
	staged_dismiss->setKind(widgets::AidaButton::Kind::Ghost);
	staged_buttons->addWidget(staged_apply_);
	staged_buttons->addWidget(staged_recheck);
	staged_buttons->addWidget(staged_dismiss);
	staged_buttons->addStretch(1);
	staged_layout->addLayout(staged_buttons);
	form->addRow(staged_card_);
	staged_card_->setVisible(false);

	depth_spin_ = new QSpinBox(config);
	depth_spin_->setObjectName(QStringLiteral("aida.view.memory.pointers.depth"));
	depth_spin_->setRange(1, 7);
	form->addRow(QStringLiteral("Max Depth"), depth_spin_);
	offset_spin_ = new QSpinBox(config);
	offset_spin_->setObjectName(QStringLiteral("aida.view.memory.pointers.offset"));
	offset_spin_->setRange(64, 16384);
	form->addRow(QStringLiteral("Max Offset"), offset_spin_);
	struct_spin_ = new QSpinBox(config);
	struct_spin_->setObjectName(QStringLiteral("aida.view.memory.pointers.struct_size"));
	struct_spin_->setRange(64, 16384);
	form->addRow(QStringLiteral("Struct Size"), struct_spin_);
	negative_check_ = new QCheckBox(QStringLiteral("Negative Offsets"), config);
	negative_check_->setObjectName(QStringLiteral("aida.view.memory.pointers.negative"));
	negative_check_->setToolTip(QStringLiteral("Allow chains through negative offsets"));
	form->addRow(negative_check_);
	static_check_ = new QCheckBox(QStringLiteral("Static Bases Only"), config);
	static_check_->setObjectName(QStringLiteral("aida.view.memory.pointers.static_only"));
	static_check_->setToolTip(QStringLiteral("Restrict base candidates to module-static addresses"));
	form->addRow(static_check_);

	map_progress_label_ = new QLabel(QStringLiteral("Building reverse map..."), config);
	map_progress_label_->setObjectName(
		QStringLiteral("aida.view.memory.pointers.map_progress_label"));
	map_progress_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
	map_progress_ = new QProgressBar(config);
	map_progress_->setObjectName(QStringLiteral("aida.view.memory.pointers.map_progress"));
	map_progress_->setRange(0, 100);
	map_progress_->setTextVisible(false);
	form->addRow(map_progress_label_);
	form->addRow(map_progress_);
	build_button_ = new widgets::AidaButton(QStringLiteral("Build Pointer Map"), config);
	build_button_->setObjectName(QStringLiteral("aida.view.memory.pointers.build"));
	build_button_->setKind(widgets::AidaButton::Kind::Primary);
	build_button_->setToolTip(QStringLiteral(
		"Enumerate every pointer in the target before scanning chains"));
	form->addRow(build_button_);

	scan_progress_label_ = new QLabel(QStringLiteral("Scanning chains..."), config);
	scan_progress_label_->setObjectName(
		QStringLiteral("aida.view.memory.pointers.scan_progress_label"));
	scan_progress_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
	scan_progress_ = new QProgressBar(config);
	scan_progress_->setObjectName(QStringLiteral("aida.view.memory.pointers.scan_progress"));
	scan_progress_->setRange(0, 100);
	scan_progress_->setTextVisible(false);
	form->addRow(scan_progress_label_);
	form->addRow(scan_progress_);
	scan_button_ = new widgets::AidaButton(QStringLiteral("Scan Chains"), config);
	scan_button_->setObjectName(QStringLiteral("aida.view.memory.pointers.scan"));
	scan_button_->setKind(widgets::AidaButton::Kind::Primary);
	scan_button_->setToolTip(QStringLiteral(
		"Find pointer chains from the map to the target address"));
	form->addRow(scan_button_);

	validate_progress_label_ = new QLabel(QStringLiteral("Validating chains..."), config);
	validate_progress_label_->setObjectName(
		QStringLiteral("aida.view.memory.pointers.validate_progress_label"));
	validate_progress_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
	validate_progress_ = new QProgressBar(config);
	validate_progress_->setObjectName(
		QStringLiteral("aida.view.memory.pointers.validate_progress"));
	validate_progress_->setRange(0, 100);
	validate_progress_->setTextVisible(false);
	form->addRow(validate_progress_label_);
	form->addRow(validate_progress_);
	validate_button_ = new widgets::AidaButton(QStringLiteral("Validate All"), config);
	validate_button_->setObjectName(QStringLiteral("aida.view.memory.pointers.validate"));
	validate_button_->setToolTip(QStringLiteral(
		"Re-read every chain against the live target and keep the ones that still resolve"));
	form->addRow(validate_button_);
	clear_results_button_ = new widgets::AidaButton(QStringLiteral("Clear Results"), config);
	clear_results_button_->setObjectName(
		QStringLiteral("aida.view.memory.pointers.clear_results"));
	clear_results_button_->setKind(widgets::AidaButton::Kind::Ghost);
	form->addRow(clear_results_button_);
	clear_map_button_ = new widgets::AidaButton(QStringLiteral("Clear Map"), config);
	clear_map_button_->setObjectName(QStringLiteral("aida.view.memory.pointers.clear_map"));
	clear_map_button_->setKind(widgets::AidaButton::Kind::Ghost);
	form->addRow(clear_map_button_);
	splitter->addWidget(config);

	auto* right = new QWidget(splitter);
	auto* right_layout = new QVBoxLayout(right);
	right_layout->setContentsMargins(0, 0, 0, 0);
	right_layout->setSpacing(0);
	model_ = new PointerChainModel(right);
	table_ = new QTableView(right);
	table_->setObjectName(QStringLiteral("aida.view.memory.pointers.table"));
	table_->verticalHeader()->setVisible(false);
	table_->verticalHeader()->setDefaultSectionSize(tokens.table.row_h);
	table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	auto* table_header = table_->horizontalHeader();
	table_header->setSectionResizeMode(QHeaderView::Interactive);
	table_header->setStretchLastSection(true);
	table_header->setSectionsClickable(false);
	table_header->setMinimumHeight(tokens.table.header_h);
	table_->setShowGrid(false);
	table_->setWordWrap(false);
	table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_->setSelectionMode(QAbstractItemView::SingleSelection);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->setAlternatingRowColors(true);
	table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
	table_->setContextMenuPolicy(Qt::CustomContextMenu);
	table_->setModel(model_);
	table_->installEventFilter(this);
	right_layout->addWidget(table_, 1);
	empty_hint_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QString(), QString(), right);
	empty_hint_->setObjectName(QStringLiteral("aida.view.memory.pointers.empty"));
	right_layout->addWidget(empty_hint_, 1);

	detail_panel_ = new QFrame(right);
	detail_panel_->setObjectName(QStringLiteral("aida.view.memory.pointers.detail"));
	detail_panel_->setProperty("aidaRole", QStringLiteral("panel"));
	detail_panel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
	auto* detail_layout = new QVBoxLayout(detail_panel_);
	detail_layout->setContentsMargins(tokens.spacing.lg, tokens.spacing.sm,
		tokens.spacing.lg, tokens.spacing.sm);
	detail_layout->setSpacing(tokens.spacing.xs);
	auto* detail_header = new QHBoxLayout();
	auto* detail_title = new QLabel(QStringLiteral("Chain Detail"), detail_panel_);
	detail_title->setObjectName(QStringLiteral("aida.view.memory.pointers.detail_title"));
	detail_header->addWidget(detail_title);
	detail_header->addStretch(1);
	auto* copy_chain = new widgets::AidaButton(QStringLiteral("Copy Chain"), detail_panel_);
	copy_chain->setObjectName(QStringLiteral("aida.view.memory.pointers.copy_chain"));
	copy_chain->setToolTip(QStringLiteral("Copy the offset chain text"));
	auto* copy_cpp = new widgets::AidaButton(QStringLiteral("Copy C++"), detail_panel_);
	copy_cpp->setObjectName(QStringLiteral("aida.view.memory.pointers.copy_cpp"));
	copy_cpp->setToolTip(QStringLiteral("Copy a C++ dereference expression"));
	auto* goto_base = new widgets::AidaButton(QStringLiteral("Goto Base"), detail_panel_);
	goto_base->setObjectName(QStringLiteral("aida.view.memory.pointers.goto_base"));
	goto_base->setKind(widgets::AidaButton::Kind::Primary);
	goto_base->setToolTip(QStringLiteral("Open the chain base in the disassembly"));
	detail_header->addWidget(copy_chain);
	detail_header->addWidget(copy_cpp);
	detail_header->addWidget(goto_base);
	detail_layout->addLayout(detail_header);
	detail_info_ = new QLabel(detail_panel_);
	detail_info_->setObjectName(QStringLiteral("aida.view.memory.pointers.detail_info"));
	detail_info_->setProperty("aidaVariant", QStringLiteral("secondary"));
	detail_layout->addWidget(detail_info_);
	diagram_ = new ChainDiagramWidget(detail_panel_);
	detail_layout->addWidget(diagram_, 1);
	right_layout->addWidget(detail_panel_);
	splitter->addWidget(right);
	splitter->setStretchFactor(0, 0);
	splitter->setStretchFactor(1, 1);
	layout->addWidget(splitter, 1);

	connect(address_edit_, &QLineEdit::textChanged, this, [](const QString& text) {
		auto& state = pointer_scanner::g_state;
		const std::string value = text.toStdString();
		std::strncpy(state.addr_buf, value.c_str(), sizeof(state.addr_buf) - 1);
		state.addr_buf[sizeof(state.addr_buf) - 1] = '\0';
	});
	connect(depth_spin_, &QSpinBox::valueChanged, this, [](int value) {
		pointer_scanner::g_state.config.max_depth = value;
	});
	connect(offset_spin_, &QSpinBox::valueChanged, this, [](int value) {
		pointer_scanner::g_state.config.max_offset = value;
	});
	connect(struct_spin_, &QSpinBox::valueChanged, this, [](int value) {
		pointer_scanner::g_state.config.struct_size = value;
	});
	connect(negative_check_, &QCheckBox::toggled, this, [](bool checked) {
		pointer_scanner::g_state.config.negative_offsets = checked;
	});
	connect(static_check_, &QCheckBox::toggled, this, [](bool checked) {
		pointer_scanner::g_state.config.only_static_bases = checked;
	});
	connect(build_button_, &widgets::AidaButton::clicked, this, [this] {
		if (pointer_scanner::g_state.map_building.load()) {
			pointer_scanner::cancel_all();
			return;
		}
		diag::log_tagged("scan_audit", "[scan_audit] pointer_scanner build_reverse_map");
		pointer_scanner::build_reverse_map();
		poll_engine();
	});
	connect(scan_button_, &widgets::AidaButton::clicked, this, [this] {
		auto& state = pointer_scanner::g_state;
		if (state.scanning.load()) {
			state.scan_cancel.store(true);
			return;
		}
		const char* p = state.addr_buf;
		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
		state.config.target_address = strtoull(p, nullptr, 16);
		diag::log_tagged("scan_audit", "[scan_audit] pointer_scanner scan_chains");
		pointer_scanner::start_scan();
		poll_engine();
	});
	connect(validate_button_, &widgets::AidaButton::clicked, this, [this] {
		diag::log_tagged("scan_audit", "[scan_audit] pointer_scanner validate_all");
		pointer_scanner::validate_all_results();
		poll_engine();
	});
	connect(clear_results_button_, &widgets::AidaButton::clicked, this, [this] {
		pointer_scanner::clear_results();
		diagram_->clear_chain();
		poll_engine();
	});
	connect(clear_map_button_, &widgets::AidaButton::clicked, this, [this] {
		pointer_scanner::clear_map();
		poll_engine();
	});
	connect(staged_apply_, &widgets::AidaButton::clicked, this, [this] {
		if (!pointer_scanner::g_staged_target.context)
			return;
		const auto staged = *pointer_scanner::g_staged_target.context;
		std::string validation_error;
		if (!staged.validate(validation_error)) {
			pointer_scanner::g_staged_target.stale = true;
			pointer_scanner::g_staged_target.status = validation_error.empty()
				? "The retained pointer target source is stale." : validation_error;
		} else {
			auto& state = pointer_scanner::g_state;
			std::snprintf(state.addr_buf, sizeof(state.addr_buf), "%016llX",
				static_cast<unsigned long long>(staged.address));
			state.config.target_address = staged.address;
			pointer_scanner::g_staged_target.status =
				"Target address accepted; start the scan when ready.";
			pointer_scanner::g_staged_target.context.reset();
			address_edit_->setText(QString::fromLatin1(state.addr_buf));
		}
		refresh_staged_card();
	});
	connect(staged_recheck, &widgets::AidaButton::clicked, this, [this] {
		if (!pointer_scanner::g_staged_target.context)
			return;
		const auto staged = *pointer_scanner::g_staged_target.context;
		std::string validation_error;
		pointer_scanner::g_staged_target.stale = !staged.validate(validation_error);
		pointer_scanner::g_staged_target.status = pointer_scanner::g_staged_target.stale
			? (validation_error.empty() ? "The retained pointer target source is stale." : validation_error)
			: "Exact source current at last check. Review before replacing the target address.";
		refresh_staged_card();
	});
	connect(staged_dismiss, &widgets::AidaButton::clicked, this, [this] {
		pointer_scanner::g_staged_target.context.reset();
		refresh_staged_card();
	});
	connect(table_, &QTableView::clicked, this, [this](const QModelIndex& index) {
		if (index.isValid())
			select_chain(index.row());
	});
	connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
		if (index.isValid())
			select_chain(index.row());
	});
	connect(table_, &QTableView::customContextMenuRequested, this,
		[this](const QPoint& pos) {
			QModelIndex index = table_->indexAt(pos);
			QPoint global = table_->viewport()->mapToGlobal(pos);
			if (!index.isValid()) {
				index = table_->currentIndex();
				if (index.isValid())
					global = table_->viewport()->mapToGlobal(
						table_->visualRect(index).center());
			}
			on_chain_context(global, index.isValid() ? index.row() : -1, 0);
		});
	connect(diagram_, &ChainDiagramWidget::stepActivated, this,
		[](int, std::uint64_t address) {
			const auto context = disasm_view::capture_selected_workspace();
			if (hex_view::request_live_memory(context, address, 256))
				if (auto* host = PointerScanController::instance().host())
					static_cast<void>(host->open_or_focus(
						registry::stable_view_id_t("document.hex")));
		});
	connect(diagram_, &ChainDiagramWidget::resolutionRequested, this,
		[this](int) {
			const auto* chain = model_->row_at(selected_row_);
			if (chain)
				PointerScanController::instance().request_chain_resolution(
					disasm_view::capture_selected_workspace(), *chain);
		});
	connect(copy_chain, &widgets::AidaButton::clicked, this, [this] {
		const auto* chain = model_->row_at(selected_row_);
		if (chain)
			clipboard::set_text(QString::fromStdString(
				pointer_scanner::chain_to_string(*chain)));
	});
	connect(copy_cpp, &widgets::AidaButton::clicked, this, [this] {
		const auto* chain = model_->row_at(selected_row_);
		if (chain)
			clipboard::set_text(QString::fromStdString(
				pointer_scanner::export_chain_cpp(*chain)));
	});
	connect(goto_base, &widgets::AidaButton::clicked, this, [this] {
		const auto* chain = model_->row_at(selected_row_);
		if (!chain)
			return;
		const auto base = PointerScanController::chain_base_address(*chain);
		if (!base || *base == 0)
			return;
		if (auto* host = PointerScanController::instance().host())
			static_cast<void>(host->open_or_focus(
				registry::stable_view_id_t("document.disassembly")));
		disasm_view::goto_address(*base, disasm_view::capture_selected_workspace());
	});
	connect(&PointerScanController::instance(), &PointerScanController::stateChanged,
		this, [this] { poll_engine(); });

	timer_ = new QTimer(this);
	timer_->setInterval(100);
	connect(timer_, &QTimer::timeout, this, [this] { poll_engine(); });
	poll_engine();
}

PointerScannerWidget::~PointerScannerWidget() = default;

bool PointerScannerWidget::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == table_ && forward_table_menu_key(watched, event, table_,
			[this](const QPoint& global_pos, int row, int origin) {
				on_chain_context(global_pos, row, origin);
			}))
		return true;
	return QWidget::eventFilter(watched, event);
}

void PointerScannerWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	ScanHubController::instance().note_page_shown();
	poll_engine();
	timer_->start();
}

void PointerScannerWidget::hideEvent(QHideEvent* event)
{
	timer_->stop();
	QWidget::hideEvent(event);
}

void PointerScannerWidget::select_chain(int row)
{
	selected_row_ = row;
	const auto* chain = model_->row_at(row);
	if (!chain)
		return;
	PointerScanController::instance().request_chain_resolution(
		disasm_view::capture_selected_workspace(), *chain);
	memory_interaction::runtime_t runtime;
	runtime.target_pid = PointerScanController::attached_target_pid();
	runtime.driver_loaded = runtime.target_pid != 0;
	runtime.live_attached = runtime.driver_loaded;
	const std::uint64_t base =
		PointerScanController::chain_base_address(*chain).value_or(0);
	memory_interaction::select(memory_interaction::capture_pointer_chain(runtime,
		base, static_cast<std::uint64_t>(chain->offsets.size()), row,
		pointer_scanner::chain_to_string(*chain)));
	diagram_->set_chain(*chain);
	detail_info_->setText(QStringLiteral("Depth %1  ·  %2  ·  %3")
		.arg(chain->depth)
		.arg(chain->is_static ? QStringLiteral("Static") : QStringLiteral("Dynamic"))
		.arg(chain->validated ? QStringLiteral("Validated")
			: QStringLiteral("Not validated")));
	refresh_presentation();
}

void PointerScannerWidget::poll_engine()
{
	PointerScanController::instance().observe_generations();
	auto& state = pointer_scanner::g_state;
	const std::uint64_t result_generation =
		PointerScanController::instance().result_generation();
	if (result_generation != observed_result_generation_) {
		observed_result_generation_ = result_generation;
		std::vector<pointer_scanner::pointer_chain_t> rows;
		std::size_t total = 0;
		{
			std::lock_guard<std::mutex> lock(state.results_mutex);
			rows = state.results;
			total = state.results.size();
		}
		model_->adopt(std::move(rows));
		if (selected_row_ >= static_cast<int>(model_->size()))
			selected_row_ = -1;
	}
	model_->refresh_validity();
	refresh_staged_card();
	refresh_presentation();
}

void PointerScannerWidget::refresh_staged_card()
{
	const bool staged = pointer_scanner::g_staged_target.context.has_value();
	staged_card_->setVisible(staged);
	if (!staged)
		return;
	if (!pointer_scanner::g_staged_target.stale)
		pointer_scanner::g_staged_target.status =
			"Exact source current at last check. Review before replacing the target address.";
	staged_status_->setText(
		QString::fromStdString(pointer_scanner::g_staged_target.status));
	const auto& staged_context = *pointer_scanner::g_staged_target.context;
	staged_identity_->setText(QStringLiteral("%1  generation %2\n0x%3  PID %4")
		.arg(QString::fromStdString(staged_context.source_view))
		.arg(staged_context.source_generation)
		.arg(staged_context.address, 16, 16, QLatin1Char('0'))
		.arg(staged_context.target_pid));
	staged_apply_->setEnabled(!pointer_scanner::g_staged_target.stale);
}

void PointerScannerWidget::refresh_presentation()
{
	controls_syncing_ = true;
	auto& state = pointer_scanner::g_state;
	const bool live_now = PointerScanController::attached_target_pid() != 0;
	const bool static_pe_now = function_index::detail::static_pe_active();
	capability_notice_->setVisible(!live_now && !static_pe_now);

	std::size_t entries = 0;
	std::size_t chains = 0;
	{
		std::lock_guard<std::mutex> lock(state.map_mutex);
		entries = state.map_entry_count;
	}
	{
		std::lock_guard<std::mutex> lock(state.results_mutex);
		chains = state.results.size();
	}
	view_header_->setSubtitle(QStringLiteral("Map: %1  ·  Chains: %2")
		.arg(entries).arg(chains));

	const bool building = state.map_building.load();
	const bool scanning = state.scanning.load();
	const bool validating = state.validating.load();

	map_progress_label_->setVisible(building);
	map_progress_->setVisible(building);
	if (building)
		map_progress_->setValue(static_cast<int>(state.map_progress.load() * 100.f));
	build_button_->setText(building ? QStringLiteral("Cancel")
		: QStringLiteral("Build Pointer Map"));
	build_button_->setKind(building ? widgets::AidaButton::Kind::Destructive
		: widgets::AidaButton::Kind::Primary);
	build_button_->setEnabled(building || live_now);

	scan_progress_label_->setVisible(scanning);
	scan_progress_->setVisible(scanning);
	if (scanning)
		scan_progress_->setValue(static_cast<int>(state.scan_progress.load() * 100.f));
	const bool can_scan = !building && entries > 0 && live_now;
	scan_button_->setText(scanning ? QStringLiteral("Cancel")
		: QStringLiteral("Scan Chains"));
	scan_button_->setKind(scanning ? widgets::AidaButton::Kind::Destructive
		: widgets::AidaButton::Kind::Primary);
	scan_button_->setEnabled(scanning || can_scan);

	validate_progress_label_->setVisible(validating);
	validate_progress_->setVisible(validating);
	if (validating)
		validate_progress_->setValue(
			static_cast<int>(state.validate_progress.load() * 100.f));
	validate_button_->setVisible(!validating);
	validate_button_->setEnabled(!scanning && !building && !validating && live_now);

	if (address_edit_->text().toStdString() != state.addr_buf)
		address_edit_->setText(QString::fromLatin1(state.addr_buf));
	if (depth_spin_->value() != state.config.max_depth)
		depth_spin_->setValue(state.config.max_depth);
	if (offset_spin_->value() != static_cast<int>(state.config.max_offset))
		offset_spin_->setValue(static_cast<int>(state.config.max_offset));
	if (struct_spin_->value() != static_cast<int>(state.config.struct_size))
		struct_spin_->setValue(static_cast<int>(state.config.struct_size));
	if (negative_check_->isChecked() != state.config.negative_offsets)
		negative_check_->setChecked(state.config.negative_offsets);
	if (static_check_->isChecked() != state.config.only_static_bases)
		static_check_->setChecked(state.config.only_static_bases);

	const bool empty = model_->size() == 0;
	if (empty && (scanning || building)) {
		empty_hint_->setTitle(QStringLiteral("Scanning"));
		empty_hint_->setMessage(QStringLiteral("Scanning the target process..."));
	} else if (empty && entries == 0) {
		empty_hint_->setTitle(QStringLiteral("No pointer map"));
		empty_hint_->setMessage(QStringLiteral(
			"Build the map first. Click \"Build Pointer Map\" to enumerate every pointer in the target."));
	} else if (empty) {
		empty_hint_->setTitle(QStringLiteral("No chains yet"));
		empty_hint_->setMessage(QStringLiteral(
			"Enter a target address and click \"Scan Chains\"."));
	}
	table_->setVisible(!empty);
	empty_hint_->setVisible(empty);
	const bool has_selection = selected_row_ >= 0 &&
		selected_row_ < static_cast<int>(model_->size());
	detail_panel_->setVisible(has_selection);
	controls_syncing_ = false;
}

void PointerScannerWidget::on_chain_context(const QPoint& global_pos, int row,
	int origin)
{
	if (row >= 0)
		select_chain(row);
	const auto* chain = model_->row_at(selected_row_);
	if (chain)
		show_chain_menu(*chain, global_pos, origin);
}

void PointerScannerWidget::show_chain_menu(
	const pointer_scanner::pointer_chain_t& chain, const QPoint& global_pos,
	int origin)
{
	const std::string chain_text = pointer_scanner::chain_to_string(chain);
	const auto base_value = PointerScanController::chain_base_address(chain);
	resolution_status_t resolution_status = resolution_status_t::idle;
	std::string resolution_error;
	const auto resolved_value = PointerScanController::instance().resolved_step_address(
		chain, static_cast<int>(chain.offsets.size()), &resolution_status, &resolution_error);
	const std::uint64_t base = base_value.value_or(0);
	const std::uint64_t resolved = resolved_value.value_or(0);
	const auto workspace = disasm_view::capture_selected_workspace();
	const auto workspace_generation = workspace.workspace
		? workspace.workspace->generation() : 0;
	const auto map_generation = PointerScanController::instance().map_generation();
	const auto result_generation = PointerScanController::instance().result_generation();
	const auto target_pid = PointerScanController::attached_target_pid();
	aida::ui::application_ui::retained_entity_context_t retained;
	retained.owner_id = "memory.pointer.chain";
	retained.entity_id = PointerScanController::chain_identity_key(chain);
	retained.entity_generation = result_generation;
	retained.active_view = aida::ui::stable_view_id_t("view.memory.pointers");
	retained.validate_identity = [chain, chain_text, workspace, workspace_generation,
		map_generation, result_generation, target_pid]() {
		if (!workspace.workspace || workspace.workspace->generation() != workspace_generation)
			return aida::ui::capability_state_t::unavailable("The pointer workspace generation changed.");
		if (PointerScanController::attached_target_pid() != target_pid ||
			PointerScanController::instance().map_generation() != map_generation ||
			PointerScanController::instance().result_generation() != result_generation)
			return aida::ui::capability_state_t::unavailable("The target, pointer map, or results changed.");
		std::lock_guard<std::mutex> lock(pointer_scanner::g_state.results_mutex);
		const bool exists = std::any_of(pointer_scanner::g_state.results.begin(),
			pointer_scanner::g_state.results.end(), [&](const auto& item) {
				return pointer_scanner::chain_to_string(item) == chain_text;
			});
		return exists ? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable("The selected pointer chain is no longer published.");
	};
	const bool current = true;
	auto add = [&](const char* id, bool enabled, const char* reason, auto invoke) {
		retained.actions.push_back({id, enabled ? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(reason), invoke});
	};
	add("memory.pointer.copy_chain", current, "The selected chain is stale.", [chain_text]() {
		clipboard::set_text(QString::fromStdString(chain_text));
		return aida::ui::action_handler_result_t::completed();
	});
	add("memory.pointer.copy_cpp", current, "The selected chain is stale.", [chain]() {
		clipboard::set_text(QString::fromStdString(
			pointer_scanner::export_chain_cpp(chain)));
		return aida::ui::action_handler_result_t::completed();
	});
	add("memory.pointer.copy_base", current && base != 0,
		!current ? "The selected chain is stale." : "The chain has no resolved base address.", [base]() {
		char address[24];
		std::snprintf(address, sizeof(address), "0x%016llX",
			static_cast<unsigned long long>(base));
		clipboard::set_text(QString::fromLatin1(address));
		return aida::ui::action_handler_result_t::completed();
	});
	add("memory.pointer.copy_resolved", current && resolved != 0,
		!current ? "The selected chain is stale." : "The chain has no resolved final address.", [resolved]() {
		char address[24];
		std::snprintf(address, sizeof(address), "0x%016llX",
			static_cast<unsigned long long>(resolved));
		clipboard::set_text(QString::fromLatin1(address));
		return aida::ui::action_handler_result_t::completed();
	});
	add("memory.pointer.open_base_disassembly", current && base != 0,
		!current ? "The selected chain is stale." : "The chain has no resolved base address.", [base, workspace]() {
		disasm_view::goto_address(base, workspace);
		if (auto* host = PointerScanController::instance().host())
			static_cast<void>(host->open_or_focus(
				registry::stable_view_id_t("document.disassembly")));
		return aida::ui::action_handler_result_t::completed();
	});
	add("memory.pointer.open_resolved_hex", current && resolved != 0 && target_pid != 0,
		!current ? "The selected chain is stale." : resolved == 0
			? "The chain has no resolved final address." : "Attach to the target process first.", [workspace, resolved]() {
		if (hex_view::request_live_memory(workspace, resolved, 256))
			if (auto* host = PointerScanController::instance().host())
				static_cast<void>(host->open_or_focus(
					registry::stable_view_id_t("document.hex")));
		return aida::ui::action_handler_result_t::completed();
	});
	add("memory.pointer.add_address", current && resolved != 0 && target_pid != 0,
		!current ? "The selected chain is stale." : resolved == 0
			? "The chain has no resolved final address." : "Attach to the target process first.", [resolved, chain_text]() {
		memory_scanner::add_address(resolved, chain_text,
			memory_scanner::value_type_t::int64_val);
		ScannerController::instance().refresh_from_engine();
		return aida::ui::action_handler_result_t::completed();
	});
	add("memory.pointer.stage_patch", current && resolved != 0 && target_pid != 0,
		!current ? "The selected chain is stale." : resolved == 0
			? "The chain has no resolved final address." : "Attach to the target process first.", [resolved]() {
		std::string error;
		const bool staged = debugger_view::stage_patch_review(resolved, 1,
			"Staged from pointer chain", &error);
		if (staged)
			if (auto* host = PointerScanController::instance().host())
				static_cast<void>(host->open_or_focus(
					registry::stable_view_id_t("view.debug.patches")));
		return staged ? aida::ui::action_handler_result_t::completed()
			: aida::ui::action_handler_result_t::failed(error.empty()
				? "The patch review could not be staged." : error);
	});
	add("memory.pointer.validate", current && target_pid != 0,
		!current ? "The selected chain is stale." : "Attach to the target process first.", [workspace, chain]() {
		PointerScanController::instance().request_chain_resolution(workspace, chain);
		return aida::ui::action_handler_result_t::completed();
	});
	char base_text[24]{};
	char resolved_text[24]{};
	std::snprintf(base_text, sizeof(base_text), "0x%016llX",
		static_cast<unsigned long long>(base));
	std::snprintf(resolved_text, sizeof(resolved_text), "0x%016llX",
		static_cast<unsigned long long>(resolved));
	aida::automation_ui::entity_evidence::snapshot_t evidence;
	evidence.workspace_id = "pid:" + std::to_string(target_pid);
	evidence.source_view_id = "view.memory.pointers";
	evidence.source_kind = "pointer_chain";
	evidence.entity_id = retained.entity_id;
	evidence.display_label = "Pointer chain to " + std::string(resolved_text);
	evidence.excerpt = "PID: " + std::to_string(target_pid) +
		"\nMap generation: " + std::to_string(map_generation) +
		"\nResult generation: " + std::to_string(result_generation) +
		"\nBase: " + base_text + "\nResolved: " + resolved_text +
		"\nChain: " + chain_text;
	evidence.address = resolved != 0 ? resolved : base;
	evidence.revision = map_generation;
	evidence.generation = result_generation;
	evidence.sensitive = true;
	evidence.return_to_source = [chain_text, workspace, workspace_generation,
		map_generation, result_generation, target_pid](std::string& reason) {
		if (!workspace.workspace || workspace.workspace->generation() != workspace_generation ||
			PointerScanController::attached_target_pid() != target_pid ||
			PointerScanController::instance().map_generation() != map_generation ||
			PointerScanController::instance().result_generation() != result_generation) {
			reason = "The target, pointer map, results, or workspace generation changed; capture the chain again.";
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(pointer_scanner::g_state.results_mutex);
			const auto found = std::find_if(pointer_scanner::g_state.results.begin(),
				pointer_scanner::g_state.results.end(), [&](const auto& item) {
					return pointer_scanner::chain_to_string(item) == chain_text;
				});
			if (found == pointer_scanner::g_state.results.end()) {
				reason = "The retained pointer chain is no longer published; capture it again.";
				return false;
			}
			pointer_scanner::g_state.selected_result = static_cast<int>(
				std::distance(pointer_scanner::g_state.results.begin(), found));
		}
		if (auto* host = PointerScanController::instance().host())
			static_cast<void>(host->open_or_focus(
				registry::stable_view_id_t("view.memory.pointers")));
		reason.clear();
		return true;
	};
	aida::automation_ui::entity_evidence::append_actions(retained, std::move(evidence),
		current && target_pid != 0
			? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(!current
				? "The retained pointer chain changed; select it again."
				: "Attach to the retained target process before handing off pointer evidence."));
	documents::show_retained_entity_menu(std::move(retained),
		origin == 1 ? aida::ui::context_menu_open_origin_t::menu_key
			: origin == 2 ? aida::ui::context_menu_open_origin_t::shift_f10
			: aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}
