#include "qt/scanner/crypto_widget.hpp"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include "core/ai/entity_evidence_handoff.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"

#include "qt/bridge/clipboard.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/scanner/crypto_controller.hpp"
#include "qt/scanner/crypto_hits_model.hpp"
#include "qt/scanner/crypto_reference_dialog.hpp"
#include "qt/scanner/scan_hub_controller.hpp"
#include "qt/scanner/scanner_context_menus.hpp"
#include "qt/scanner/scanner_palette.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_badge.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::scanner {

CryptoScannerWidget::CryptoScannerWidget(QWidget* parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("aida.view.memory.crypto"));
	const auto& tokens = theme::tokens();
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	auto* toolbar = new QFrame(this);
	toolbar->setObjectName(QStringLiteral("aida.view.memory.crypto.toolbar"));
	toolbar->setProperty("aidaRole", QStringLiteral("toolbar"));
	auto* bar = new QHBoxLayout(toolbar);
	bar->setContentsMargins(tokens.toolbar.padding_x, tokens.toolbar.padding_y,
		tokens.toolbar.padding_x, tokens.toolbar.padding_y);
	bar->setSpacing(tokens.toolbar.group_gap);
	auto* title = new QLabel(QStringLiteral("Crypto Scanner"), toolbar);
	title->setObjectName(QStringLiteral("aida.view.memory.crypto.title"));
	bar->addWidget(title);
	scan_button_ = new widgets::AidaButton(QStringLiteral("Scan Process"), toolbar);
	scan_button_->setObjectName(QStringLiteral("aida.view.memory.crypto.scan_process"));
	scan_button_->setKind(widgets::AidaButton::Kind::Primary);
	scan_button_->setToolTip(QStringLiteral("Scan the attached process for cryptographic constants"));
	bar->addWidget(scan_button_);
	scan_file_button_ = new widgets::AidaButton(QStringLiteral("Scan File"), toolbar);
	scan_file_button_->setObjectName(QStringLiteral("aida.view.memory.crypto.scan_file"));
	scan_file_button_->setToolTip(QStringLiteral("Scan the loaded binary for cryptographic constants"));
	bar->addWidget(scan_file_button_);
	entropy_button_ = new widgets::AidaButton(QStringLiteral("Entropy"), toolbar);
	entropy_button_->setObjectName(QStringLiteral("aida.view.memory.crypto.entropy"));
	entropy_button_->setToolTip(QStringLiteral("Map high-entropy regions in the attached process"));
	bar->addWidget(entropy_button_);
	export_json_ = new widgets::AidaButton(QStringLiteral("JSON"), toolbar);
	export_json_->setObjectName(QStringLiteral("aida.view.memory.crypto.export_json"));
	export_json_->setKind(widgets::AidaButton::Kind::Ghost);
	export_json_->setToolTip(QStringLiteral("Export the scan results as JSON"));
	bar->addWidget(export_json_);
	export_csv_ = new widgets::AidaButton(QStringLiteral("CSV"), toolbar);
	export_csv_->setObjectName(QStringLiteral("aida.view.memory.crypto.export_csv"));
	export_csv_->setKind(widgets::AidaButton::Kind::Ghost);
	export_csv_->setToolTip(QStringLiteral("Export the scan results as CSV"));
	bar->addWidget(export_csv_);
	bar->addStretch(1);
	progress_ = new QProgressBar(toolbar);
	progress_->setObjectName(QStringLiteral("aida.view.memory.crypto.progress"));
	progress_->setRange(0, 100);
	progress_->setFixedWidth(16 * mono_cell_width());
	progress_->setTextVisible(false);
	progress_->setVisible(false);
	bar->addWidget(progress_);
	layout->addWidget(toolbar);

	error_notice_ = new widgets::AidaNotice(QString(), QString(),
		widgets::AidaSemantic::Error, this);
	error_notice_->setObjectName(QStringLiteral("aida.view.memory.crypto.error_notice"));
	error_notice_->setVisible(false);
	layout->addWidget(error_notice_);

	auto* export_row = new QWidget(this);
	export_row->setObjectName(QStringLiteral("aida.view.memory.crypto.export_row"));
	auto* export_layout = new QHBoxLayout(export_row);
	export_layout->setContentsMargins(tokens.spacing.lg, 0, tokens.spacing.lg, 0);
	export_notice_ = new widgets::AidaNotice(QString(), QString(),
		widgets::AidaSemantic::Info, export_row);
	export_notice_->setObjectName(QStringLiteral("aida.view.memory.crypto.export_notice"));
	export_layout->addWidget(export_notice_, 1);
	retry_button_ = new widgets::AidaButton(QStringLiteral("Retry"), export_row);
	retry_button_->setObjectName(QStringLiteral("aida.view.memory.crypto.retry"));
	retry_button_->setVisible(false);
	export_layout->addWidget(retry_button_);
	export_row->setVisible(false);
	layout->addWidget(export_row);

	capability_notice_ = new widgets::AidaNotice(QStringLiteral("No scan target"),
		QStringLiteral("Scan needs either a live attach or a loaded PE."),
		widgets::AidaSemantic::Warning, this);
	capability_notice_->setObjectName(
		QStringLiteral("aida.view.memory.crypto.capability_notice"));
	capability_notice_->setVisible(false);
	layout->addWidget(capability_notice_);

	auto* filter_strip = new QWidget(this);
	filter_strip->setObjectName(QStringLiteral("aida.view.memory.crypto.filter_strip"));
	auto* filter_layout = new QHBoxLayout(filter_strip);
	filter_layout->setContentsMargins(tokens.spacing.lg, tokens.spacing.xs,
		tokens.spacing.lg, tokens.spacing.xs);
	filter_layout->setSpacing(tokens.spacing.sm);
	search_edit_ = new QLineEdit(filter_strip);
	search_edit_->setObjectName(QStringLiteral("aida.view.memory.crypto.search"));
	search_edit_->setPlaceholderText(
		QStringLiteral("Filter algorithm, signature, module..."));
	search_edit_->setMaxLength(127);
	search_edit_->setMinimumWidth(30 * mono_cell_width());
	filter_layout->addWidget(search_edit_);
	category_combo_ = new QComboBox(filter_strip);
	category_combo_->setObjectName(QStringLiteral("aida.view.memory.crypto.category"));
	category_combo_->setToolTip(QStringLiteral("Restrict results to one crypto category"));
	const char* categories[] = {"All", "Symmetric", "Hash", "Stream Cipher",
		"Block Cipher", "Checksum", "Encoding", "Asymmetric"};
	for (const char* name : categories)
		category_combo_->addItem(QString::fromLatin1(name));
	filter_layout->addWidget(category_combo_);
	chip_hits_ = new widgets::AidaBadge(filter_strip);
	chip_hits_->setObjectName(QStringLiteral("aida.view.memory.crypto.chip_hits"));
	chip_cipher_ = new widgets::AidaBadge(filter_strip);
	chip_cipher_->setObjectName(QStringLiteral("aida.view.memory.crypto.chip_cipher"));
	chip_hash_ = new widgets::AidaBadge(filter_strip);
	chip_hash_->setObjectName(QStringLiteral("aida.view.memory.crypto.chip_hash"));
	chip_refs_ = new widgets::AidaBadge(filter_strip);
	chip_refs_->setObjectName(QStringLiteral("aida.view.memory.crypto.chip_refs"));
	filter_layout->addWidget(chip_hits_);
	filter_layout->addWidget(chip_cipher_);
	filter_layout->addWidget(chip_hash_);
	filter_layout->addWidget(chip_refs_);
	filter_layout->addStretch(1);
	layout->addWidget(filter_strip);

	model_ = new CryptoHitsModel(this);
	table_ = new QTableView(this);
	table_->setObjectName(QStringLiteral("aida.view.memory.crypto.table"));
	table_->verticalHeader()->setVisible(false);
	table_->verticalHeader()->setDefaultSectionSize(tokens.table.row_h);
	table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	auto* header = table_->horizontalHeader();
	header->setSectionResizeMode(QHeaderView::Interactive);
	header->setStretchLastSection(true);
	header->setSectionsClickable(true);
	header->setMinimumHeight(tokens.table.header_h);
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
	layout->addWidget(table_, 1);

	empty_view_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QStringLiteral("No crypto results"), QString(), this);
	empty_view_->setObjectName(QStringLiteral("aida.view.memory.crypto.empty"));
	empty_view_->setVisible(false);
	layout->addWidget(empty_view_, 1);

	status_label_ = new QLabel(this);
	status_label_->setObjectName(QStringLiteral("aida.view.memory.crypto.status"));
	status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
	status_label_->setContentsMargins(tokens.spacing.lg, tokens.spacing.xs,
		tokens.spacing.lg, tokens.spacing.xs);
	layout->addWidget(status_label_);

	connect(scan_button_, &widgets::AidaButton::clicked, this, [this] {
		const auto context = disasm_view::capture_selected_workspace();
		if (!state_)
			return;
		if (state_->scanning.load(std::memory_order_acquire)) {
			CryptoController::instance().cancel_scan(state_);
			return;
		}
		CryptoController::instance().start_scan(context, state_, false);
		diag::log_tagged("scan_audit", "[scan_audit] crypto_scanner scan_process");
	});
	connect(scan_file_button_, &widgets::AidaButton::clicked, this, [this] {
		if (!state_)
			return;
		CryptoController::instance().start_scan(
			disasm_view::capture_selected_workspace(), state_, false);
		diag::log_tagged("scan_audit", "[scan_audit] crypto_scanner scan_file");
	});
	connect(entropy_button_, &widgets::AidaButton::clicked, this, [this] {
		if (!state_)
			return;
		CryptoController::instance().start_scan(
			disasm_view::capture_selected_workspace(), state_, true);
		diag::log_tagged("scan_audit", "[scan_audit] crypto_scanner scan_entropy");
	});
	connect(export_json_, &widgets::AidaButton::clicked, this, [this] {
		if (state_ && snapshot_)
			CryptoController::instance().export_results(
				disasm_view::capture_selected_workspace(), state_, snapshot_, {}, false);
	});
	connect(export_csv_, &widgets::AidaButton::clicked, this, [this] {
		if (state_ && snapshot_)
			CryptoController::instance().export_results(
				disasm_view::capture_selected_workspace(), state_, snapshot_, {}, true);
	});
	connect(retry_button_, &widgets::AidaButton::clicked, this, [this] {
		if (!state_)
			return;
		bool csv = false;
		std::string path;
		{
			std::lock_guard<std::mutex> lock(state_->mutex);
			csv = state_->last_export_csv;
			path = state_->last_export_path;
		}
		CryptoController::instance().export_results(
			disasm_view::capture_selected_workspace(), state_, snapshot_,
			std::move(path), csv);
	});
	connect(search_edit_, &QLineEdit::textChanged, this, [this](const QString&) {
		filter_debounce_->start();
	});
	connect(category_combo_, &QComboBox::currentIndexChanged, this,
		[this](int) { issue_filter(); });
	connect(header, &QHeaderView::sectionClicked, this, [this](int logical) {
		if (logical < 0 || logical >= CryptoHitsModel::column_category)
			return;
		if (sort_column_ == logical)
			sort_ascending_ = !sort_ascending_;
		else {
			sort_column_ = logical;
			sort_ascending_ = true;
		}
		auto* table_header = table_->horizontalHeader();
		table_header->setSortIndicatorShown(true);
		table_header->setSortIndicator(sort_column_,
			sort_ascending_ ? Qt::AscendingOrder : Qt::DescendingOrder);
		issue_filter();
	});
	const auto activate_hit = [this](const QModelIndex& index) {
		const auto* hit = model_->row_at(index.row());
		if (!hit)
			return;
		const auto context = disasm_view::capture_selected_workspace();
		if (auto* host = CryptoController::instance().host())
			static_cast<void>(host->open_or_focus(
				registry::stable_view_id_t("document.disassembly")));
		disasm_view::goto_address(hit->address, context);
	};
	connect(table_, &QTableView::activated, this, activate_hit);
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
			on_hit_context(global, index.isValid() ? index.row() : -1, 0);
		});
	connect(&CryptoController::instance(), &CryptoController::stateChanged, this,
		[this] { refresh_presentation(); });

	filter_debounce_ = new QTimer(this);
	filter_debounce_->setSingleShot(true);
	filter_debounce_->setInterval(150);
	connect(filter_debounce_, &QTimer::timeout, this, [this] { issue_filter(); });

	poll_timer_ = new QTimer(this);
	poll_timer_->setInterval(100);
	connect(poll_timer_, &QTimer::timeout, this, [this] { poll_engine(); });
	poll_engine();
}

CryptoScannerWidget::~CryptoScannerWidget() = default;

bool CryptoScannerWidget::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == table_ && forward_table_menu_key(watched, event, table_,
			[this](const QPoint& global_pos, int row, int origin) {
				on_hit_context(global_pos, row, origin);
			}))
		return true;
	return QWidget::eventFilter(watched, event);
}

void CryptoScannerWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	ScanHubController::instance().note_page_shown();
	poll_engine();
	poll_timer_->start();
}

void CryptoScannerWidget::hideEvent(QHideEvent* event)
{
	poll_timer_->stop();
	QWidget::hideEvent(event);
}

void CryptoScannerWidget::issue_filter()
{
	if (!state_ || !snapshot_)
		return;
	const auto context = disasm_view::capture_selected_workspace();
	CryptoController::instance().request_filter(context, state_, snapshot_,
		search_edit_->text().toStdString(), category_combo_->currentIndex() - 1,
		sort_column_, sort_ascending_);
}

void CryptoScannerWidget::poll_engine()
{
	const auto context = disasm_view::capture_selected_workspace();
	state_ = CryptoController::instance().state_for(context);
	if (!state_) {
		refresh_presentation();
		return;
	}
	CryptoController::instance().reconcile(state_);
	std::shared_ptr<const crypto_scanner::workspace_scan_snapshot_t> snapshot;
	std::shared_ptr<const std::vector<crypto_scanner::crypto_hit_t>> filtered;
	{
		std::lock_guard<std::mutex> lock(state_->mutex);
		snapshot = state_->snapshot;
		filtered = state_->filtered_results;
	}
	if (snapshot != snapshot_) {
		snapshot_ = snapshot;
		issue_filter();
	}
	model_->adopt(filtered);
	if (state_->open_reference_chooser) {
		state_->open_reference_chooser = false;
		auto* dialog = new CryptoReferenceDialog(context,
			state_->reference_choices, state_->reference_generation, this);
		if (state_->reference_focus_pending) {
			state_->reference_focus_pending = false;
		}
		dialog->open();
	}
	refresh_presentation();
}

void CryptoScannerWidget::refresh_presentation()
{
	controls_syncing_ = true;
	if (!state_) {
		capability_notice_->setTitle(QStringLiteral("No analysis target"));
		capability_notice_->setMessage(QStringLiteral(
			"Open a binary or attach a live target before scanning."));
		capability_notice_->setVisible(true);
		empty_view_->setTitle(QStringLiteral("No analysis target"));
		empty_view_->setMessage(QStringLiteral(
			"Open a binary or attach a live target before scanning."));
		table_->setVisible(false);
		empty_view_->setVisible(true);
		controls_syncing_ = false;
		return;
	}
	const auto context = disasm_view::capture_selected_workspace();
	const bool attached_now = context.workspace && context.workspace->target_kind() ==
		aida::analysis::target_kind_t::live_snapshot;
	const bool pe_loaded = context.workspace && context.workspace->target_kind() ==
		aida::analysis::target_kind_t::static_file && static_cast<bool>(context.image);
	const bool scanning = state_->scanning.load(std::memory_order_acquire);
	const bool export_pending = state_->export_pending.load(std::memory_order_acquire);

	capability_notice_->setTitle(QStringLiteral("No scan target"));
	capability_notice_->setMessage(QStringLiteral(
		"Scan needs either a live attach or a loaded PE."));
	capability_notice_->setVisible(!attached_now && !pe_loaded);

	scan_button_->setText(scanning ? QStringLiteral("Cancel")
		: QStringLiteral("Scan Process"));
	scan_button_->setKind(scanning ? widgets::AidaButton::Kind::Destructive
		: widgets::AidaButton::Kind::Primary);
	scan_button_->setEnabled(scanning || attached_now);
	scan_file_button_->setEnabled(!scanning && pe_loaded);
	entropy_button_->setEnabled(!scanning && attached_now);
	export_json_->setEnabled(!export_pending);
	export_csv_->setEnabled(!export_pending);

	if (scanning) {
		progress_->setVisible(true);
		progress_->setValue(static_cast<int>(
			state_->progress.load(std::memory_order_acquire) * 100.f));
	} else {
		progress_->setVisible(false);
	}

	std::string scan_error;
	crypto_workspace_scan::export_status_t export_status;
	{
		std::lock_guard<std::mutex> lock(state_->mutex);
		scan_error = state_->last_error;
		export_status = state_->export_status;
	}
	error_notice_->setMessage(QString::fromStdString(scan_error));
	error_notice_->setVisible(!scan_error.empty());
	const bool export_visible =
		export_status.terminal != crypto_workspace_scan::export_terminal_t::idle;
	export_notice_->setMessage(QString::fromStdString(export_status.message));
	const bool failed =
		export_status.terminal == crypto_workspace_scan::export_terminal_t::failed ||
		export_status.terminal == crypto_workspace_scan::export_terminal_t::stale ||
		export_status.terminal == crypto_workspace_scan::export_terminal_t::cancelled;
	export_notice_->setKind(failed ? widgets::AidaSemantic::Error
		: widgets::AidaSemantic::Info);
	retry_button_->setVisible(failed && !export_pending);
	export_notice_->parentWidget()->setVisible(export_visible);

	chip_hits_->setText(QStringLiteral("hits · %1")
		.arg(state_->total_hits.load(std::memory_order_acquire)));
	chip_cipher_->setText(QStringLiteral("ciph · %1")
		.arg(state_->cipher_hits.load(std::memory_order_acquire)));
	chip_hash_->setText(QStringLiteral("hash · %1")
		.arg(state_->hash_hits.load(std::memory_order_acquire)));
	chip_refs_->setText(QStringLiteral("refs · %1")
		.arg(state_->referenced_hits.load(std::memory_order_acquire)));

	std::size_t entropy_count = snapshot_ ? snapshot_->entropy_map.size() : 0;
	status_label_->setText(entropy_count > 0
		? QStringLiteral("%1 results  ·  %2 entropy regions")
			.arg(model_->size()).arg(entropy_count)
		: QStringLiteral("%1 results").arg(model_->size()));

	const bool empty = model_->size() == 0;
	if (empty) {
		if (!snapshot_) {
			empty_view_->setTitle(QStringLiteral("No crypto scan yet"));
			empty_view_->setMessage(QStringLiteral(
				"Scan the attached process or the loaded file to hunt cryptographic constants."));
		} else {
			empty_view_->setTitle(QStringLiteral("No hits"));
			empty_view_->setMessage(scanning
				? QStringLiteral("Scanning for cryptographic constants...")
				: QStringLiteral("Nothing matched. Adjust the filter or scan a different target."));
		}
	}
	table_->setVisible(!empty);
	empty_view_->setVisible(empty);
	controls_syncing_ = false;
}

void CryptoScannerWidget::on_hit_context(const QPoint& global_pos, int row, int origin)
{
	if (!state_ || row < 0)
		return;
	const auto* hit = model_->row_at(row);
	if (!hit)
		return;
	show_hit_menu(*hit, disasm_view::capture_selected_workspace(), state_,
		global_pos, origin);
}

void CryptoScannerWidget::show_hit_menu(const crypto_scanner::crypto_hit_t& hit,
	const disasm_view::workspace_context_t& context,
	const std::shared_ptr<crypto_workspace_scan::state_t>& state,
	const QPoint& global_pos, int origin)
{
	if (!context.workspace || !snapshot_)
		return;
	const auto scan_snapshot = snapshot_;
	const auto workspace_hit = std::find_if(scan_snapshot->results.begin(),
		scan_snapshot->results.end(), [&](const auto& item) {
			const auto runtime = disasm_view::runtime_address(context, item.address);
			return runtime && *runtime == hit.address &&
				item.algorithm == hit.algorithm &&
				item.signature_name == hit.signature_name;
		});
	const bool hit_identity_available = workspace_hit != scan_snapshot->results.end();
	const auto hit_address_identity = hit_identity_available
		? workspace_hit->address : aida::analysis::address_t{};
	const auto workspace = context.workspace;
	const auto generation = context.publication ? context.publication->generation : 0;
	const auto target_pid = driver_bridge::attached_pid();
	aida::ui::application_ui::retained_entity_context_t retained;
	retained.owner_id = "memory.crypto.hit";
	retained.entity_id = hit.algorithm + "@" + std::to_string(hit.address);
	retained.entity_generation = generation;
	retained.active_view = aida::ui::stable_view_id_t("view.memory.crypto");
	retained.validate_identity = [workspace, generation, hit, scan_snapshot, target_pid,
		hit_identity_available, hit_address_identity]() {
		if (!workspace) return aida::ui::capability_state_t::unavailable("The crypto workspace was closed.");
		if (driver_bridge::attached_pid() != target_pid)
			return aida::ui::capability_state_t::unavailable("The crypto scan target process changed; reopen the menu.");
		const auto publication = workspace->analysis_publication();
		if (!publication || publication->generation != generation)
			return aida::ui::capability_state_t::unavailable("The analysis publication changed; reopen the menu.");
		if (!hit_identity_available)
			return aida::ui::capability_state_t::unavailable("The selected crypto hit identity is no longer available.");
		const bool current = std::any_of(scan_snapshot->results.begin(), scan_snapshot->results.end(), [&](const auto& item) {
			return item.address == hit_address_identity && item.algorithm == hit.algorithm &&
				item.signature_name == hit.signature_name;
		});
		return current ? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable("The selected crypto hit changed or was removed.");
	};
	auto add = [&](const char* id, auto invoke) {
		retained.actions.push_back({id, aida::ui::capability_state_t::available(), invoke});
	};
	add("memory.entity.open_disassembly", [hit, context]() {
		if (auto* host = CryptoController::instance().host())
			static_cast<void>(host->open_or_focus(
				registry::stable_view_id_t("document.disassembly")));
		disasm_view::goto_address(hit.address, context);
		diag::log_tagged("scan_audit", "[scan_audit] crypto_scanner ctx open_disasm");
		return aida::ui::action_handler_result_t::completed();
	});
	add("memory.entity.open_hex", [hit, context]() {
		crypto_workspace_scan::detail::open_hit_in_hex(context, hit.address);
		if (auto* host = CryptoController::instance().host())
			static_cast<void>(host->open_or_focus(
				registry::stable_view_id_t("document.hex")));
		diag::log_tagged("scan_audit", "[scan_audit] crypto_scanner ctx open_hex");
		return aida::ui::action_handler_result_t::completed();
	});
	if (!hit.referencing_functions.empty()) {
		const bool bounded = hit.referencing_functions.size() <=
			crypto_workspace_scan::k_max_crypto_reference_publication;
		retained.actions.push_back({"memory.crypto.show_references", bounded
			? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(
				"The reference publication exceeds the 4 MiB scanner-module bound."),
			[state, hit, generation]() {
			state->reference_choices.clear();
			state->reference_choices.reserve(hit.referencing_functions.size());
			std::unordered_set<std::uint64_t> seen;
			seen.reserve(hit.referencing_functions.size());
			for (const auto address : hit.referencing_functions)
				if (seen.insert(address).second)
					state->reference_choices.push_back(address);
			state->reference_generation = generation;
			state->reference_selected = 0;
			state->reference_focus_pending = true;
			state->open_reference_chooser = true;
			return aida::ui::action_handler_result_t::completed();
		}});
	}
	add("memory.entity.copy_address", [hit]() {
		char address[32]{};
		std::snprintf(address, sizeof(address), "0x%llX", static_cast<unsigned long long>(hit.address));
		clipboard::set_text(QString::fromLatin1(address));
		diag::log_tagged("scan_audit", "[scan_audit] crypto_scanner ctx copy_address");
		return aida::ui::action_handler_result_t::completed();
	});
	add("memory.crypto.copy_algorithm", [hit]() {
		clipboard::set_text(QString::fromStdString(hit.algorithm));
		diag::log_tagged("scan_audit", "[scan_audit] crypto_scanner ctx copy_algo");
		return aida::ui::action_handler_result_t::completed();
	});
	char evidence_address[24]{};
	std::snprintf(evidence_address, sizeof(evidence_address), "0x%016llX",
		static_cast<unsigned long long>(hit.address));
	aida::automation_ui::entity_evidence::snapshot_t evidence;
	evidence.workspace_id = target_pid != 0 ? "pid:" + std::to_string(target_pid)
		: workspace->identity().binary_id().to_hex();
	evidence.source_view_id = "view.memory.crypto";
	evidence.source_kind = "crypto_scan_hit";
	evidence.entity_id = retained.entity_id;
	evidence.display_label = hit.algorithm + " / " + hit.signature_name;
	evidence.excerpt = "PID: " + std::to_string(target_pid) +
		"\nPublication generation: " + std::to_string(generation) +
		"\nAlgorithm: " + hit.algorithm + "\nSignature: " + hit.signature_name +
		"\nCategory: " + crypto_scanner::category_name(hit.category) +
		"\nAddress: " + evidence_address + "\nModule: " + hit.module_name +
		"\nModule offset: " + std::to_string(hit.module_offset) +
		"\nReference count: " + std::to_string(hit.referencing_functions.size());
	evidence.address = hit.address;
	evidence.revision = generation;
	evidence.generation = generation;
	evidence.sensitive = target_pid != 0;
	const auto evidence_hit_address = hit.address;
	const auto evidence_hit_algorithm = hit.algorithm;
	const auto evidence_hit_signature = hit.signature_name;
	const auto evidence_hit_module = hit.module_name;
	const auto evidence_hit_module_offset = hit.module_offset;
	const auto evidence_hit_reference_count = hit.referencing_functions.size();
	evidence.return_to_source = [workspace, generation, evidence_hit_address,
		evidence_hit_algorithm, evidence_hit_signature, evidence_hit_module,
		evidence_hit_module_offset, evidence_hit_reference_count, scan_snapshot,
		state, hit_identity_available, hit_address_identity](std::string& reason) {
		const auto publication = workspace ? workspace->analysis_publication() : nullptr;
		if (!publication || publication->generation != generation || !hit_identity_available ||
			!std::any_of(scan_snapshot->results.begin(), scan_snapshot->results.end(),
				[&](const auto& item) {
					return item.address == hit_address_identity &&
						item.algorithm == evidence_hit_algorithm &&
						item.signature_name == evidence_hit_signature &&
						item.module_name == evidence_hit_module &&
						item.module_offset == evidence_hit_module_offset &&
						item.referencing_functions.size() == evidence_hit_reference_count;
				})) {
			reason = "The crypto publication or retained hit changed; capture it again.";
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->context_address = evidence_hit_address;
			state->context_algorithm = evidence_hit_algorithm;
			state->context_signature = evidence_hit_signature;
			state->ctx_hit_idx = -1;
			if (state->filtered_results) {
				const auto found = std::find_if(state->filtered_results->begin(),
					state->filtered_results->end(), [&](const auto& item) {
						return item.address == evidence_hit_address &&
							item.algorithm == evidence_hit_algorithm &&
							item.signature_name == evidence_hit_signature;
					});
				if (found != state->filtered_results->end())
					state->ctx_hit_idx = static_cast<int>(std::distance(
						state->filtered_results->begin(), found));
			}
		}
		if (auto* host = CryptoController::instance().host())
			static_cast<void>(host->open_or_focus(
				registry::stable_view_id_t("view.memory.crypto")));
		reason.clear();
		return true;
	};
	aida::automation_ui::entity_evidence::append_actions(retained, std::move(evidence));
	documents::show_retained_entity_menu(std::move(retained),
		origin == 1 ? aida::ui::context_menu_open_origin_t::menu_key
			: origin == 2 ? aida::ui::context_menu_open_origin_t::shift_f10
			: aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}
