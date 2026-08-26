#include "qt/scanner/snapshot_widget.hpp"

#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>

#include "core/runtime/standalone_driver.hpp"
#include "core/ai/entity_evidence_handoff.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/editor/hex_view.hpp"
#include "core/scanner/memory_scanner.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"

#include "qt/bridge/clipboard.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/scanner/diff_detail_widget.hpp"
#include "qt/scanner/diff_table_model.hpp"
#include "qt/scanner/scanner_context_menus.hpp"
#include "qt/scanner/scanner_controller.hpp"
#include "qt/scanner/scanner_palette.hpp"
#include "qt/scanner/snapshot_controller.hpp"
#include "qt/scanner/snapshot_timeline_widget.hpp"
#include "qt/scanner/scan_hub_controller.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::scanner {

SnapshotDiffWidget::SnapshotDiffWidget(QWidget* parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("aida.view.memory.snapshots"));
	const auto& tokens = theme::tokens();
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	auto* toolbar = new QFrame(this);
	toolbar->setObjectName(QStringLiteral("aida.view.memory.snapshots.toolbar"));
	toolbar->setProperty("aidaRole", QStringLiteral("toolbar"));
	auto* bar = new QHBoxLayout(toolbar);
	bar->setContentsMargins(tokens.toolbar.padding_x, tokens.toolbar.padding_y,
		tokens.toolbar.padding_x, tokens.toolbar.padding_y);
	bar->setSpacing(tokens.toolbar.group_gap);
	auto* title = new QLabel(QStringLiteral("Snapshot Diff"), toolbar);
	title->setObjectName(QStringLiteral("aida.view.memory.snapshots.title"));
	bar->addWidget(title);
	take_button_ = new widgets::AidaButton(QStringLiteral("Take Snapshot"), toolbar);
	take_button_->setObjectName(QStringLiteral("aida.view.memory.snapshots.take"));
	take_button_->setKind(widgets::AidaButton::Kind::Primary);
	take_button_->setToolTip(QStringLiteral(
		"Capture the target's writable memory into a named snapshot"));
	bar->addWidget(take_button_);
	compare_button_ = new widgets::AidaButton(QStringLiteral("Compare"), toolbar);
	compare_button_->setObjectName(QStringLiteral("aida.view.memory.snapshots.compare"));
	compare_button_->setKind(widgets::AidaButton::Kind::Primary);
	compare_button_->setToolTip(QStringLiteral(
		"Diff the A and B snapshots picked on the timeline"));
	bar->addWidget(compare_button_);
	clear_button_ = new widgets::AidaButton(QStringLiteral("Clear All"), toolbar);
	clear_button_->setObjectName(QStringLiteral("aida.view.memory.snapshots.clear"));
	clear_button_->setKind(widgets::AidaButton::Kind::Destructive);
	clear_button_->setToolTip(QStringLiteral("Discard every snapshot in this session"));
	bar->addWidget(clear_button_);
	load_button_ = new widgets::AidaButton(QStringLiteral("Load"), toolbar);
	load_button_->setObjectName(QStringLiteral("aida.view.memory.snapshots.load"));
	load_button_->setToolTip(QStringLiteral("Load a snapshot file from disk"));
	bar->addWidget(load_button_);
	bar->addStretch(1);
	count_label_ = new QLabel(toolbar);
	count_label_->setObjectName(QStringLiteral("aida.view.memory.snapshots.count"));
	count_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
	bar->addWidget(count_label_);
	progress_ = new QProgressBar(toolbar);
	progress_->setObjectName(QStringLiteral("aida.view.memory.snapshots.progress"));
	progress_->setRange(0, 100);
	progress_->setFixedWidth(16 * mono_cell_width());
	progress_->setTextVisible(false);
	progress_->setVisible(false);
	bar->addWidget(progress_);
	layout->addWidget(toolbar);

	capability_notice_ = new widgets::AidaNotice(QStringLiteral("Live attach required"),
		QStringLiteral("Take/Compare need a live attach. Load existing snapshot files from disk to inspect them."),
		widgets::AidaSemantic::Info, this);
	capability_notice_->setObjectName(
		QStringLiteral("aida.view.memory.snapshots.capability_notice"));
	capability_notice_->setVisible(false);
	layout->addWidget(capability_notice_);

	error_notice_ = new widgets::AidaNotice(QString(), QString(),
		widgets::AidaSemantic::Error, this);
	error_notice_->setObjectName(QStringLiteral("aida.view.memory.snapshots.error_notice"));
	error_notice_->setVisible(false);
	layout->addWidget(error_notice_);

	timeline_ = new SnapshotTimelineWidget(this);
	layout->addWidget(timeline_);

	truncated_label_ = new QLabel(QStringLiteral(
		"Bounded to first 250,000 change ranges"), this);
	truncated_label_->setObjectName(QStringLiteral("aida.view.memory.snapshots.truncated"));
	truncated_label_->setProperty("aidaVariant", QStringLiteral("warning"));
	truncated_label_->setContentsMargins(0, tokens.spacing.xxs, tokens.spacing.md,
		tokens.spacing.xxs);
	truncated_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	truncated_label_->setVisible(false);
	layout->addWidget(truncated_label_);

	model_ = new DiffTableModel(this);
	table_ = new QTableView(this);
	table_->setObjectName(QStringLiteral("aida.view.memory.snapshots.table"));
	table_->verticalHeader()->setVisible(false);
	table_->verticalHeader()->setDefaultSectionSize(tokens.table.compact_row_h);
	table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	auto* header = table_->horizontalHeader();
	header->setSectionResizeMode(QHeaderView::Interactive);
	header->setStretchLastSection(true);
	header->setSectionsClickable(false);
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

	empty_hint_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QString(), QString(), this);
	empty_hint_->setObjectName(QStringLiteral("aida.view.memory.snapshots.empty"));
	layout->addWidget(empty_hint_, 1);

	detail_ = new DiffDetailWidget(this);
	detail_->setVisible(false);
	layout->addWidget(detail_);

	connect(take_button_, &widgets::AidaButton::clicked, this, [] {
		SnapshotDiffController::instance().take_snapshot();
	});
	connect(compare_button_, &widgets::AidaButton::clicked, this, [] {
		auto& state = snapshot_diff::g_state;
		std::uint64_t a = 0;
		std::uint64_t b = 0;
		{
			std::lock_guard<std::mutex> lock(state.mutex);
			a = state.snap_a_id;
			b = state.snap_b_id;
		}
		SnapshotDiffController::instance().compare(a, b);
	});
	connect(clear_button_, &widgets::AidaButton::clicked, this, [] {
		SnapshotDiffController::instance().clear();
	});
	connect(load_button_, &widgets::AidaButton::clicked, this, [this] {
		const QString picked = QFileDialog::getOpenFileName(this,
			QStringLiteral("Load Snapshot"),
			QString::fromStdString(snapshot_diff::detail::snapshot_dir().string()),
			QStringLiteral("Snapshot (*.bin);;All files (*.*)"));
		if (!picked.isEmpty())
			SnapshotDiffController::instance().load(picked.toStdString());
	});
	connect(timeline_, &SnapshotTimelineWidget::selectionChanged, this,
		[this] { refresh_presentation(); });
	connect(table_, &QTableView::clicked, this, [this](const QModelIndex& index) {
		if (!index.isValid())
			return;
		SnapshotDiffController::instance().set_selected_change(index.row());
		refresh_detail();
	});
	connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
		if (!index.isValid())
			return;
		SnapshotDiffController::instance().set_selected_change(index.row());
		refresh_detail();
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
			on_diff_context(global, index.isValid() ? index.row() : -1, 0);
		});
	connect(&SnapshotDiffController::instance(), &SnapshotDiffController::stateChanged,
		this, [this] { poll_engine(); });

	timer_ = new QTimer(this);
	timer_->setInterval(100);
	connect(timer_, &QTimer::timeout, this, [this] { poll_engine(); });
	poll_engine();
}

SnapshotDiffWidget::~SnapshotDiffWidget() = default;

bool SnapshotDiffWidget::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == table_ && forward_table_menu_key(watched, event, table_,
			[this](const QPoint& global_pos, int row, int origin) {
				on_diff_context(global_pos, row, origin);
			}))
		return true;
	return QWidget::eventFilter(watched, event);
}

void SnapshotDiffWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	ScanHubController::instance().note_page_shown();
	poll_engine();
	timer_->start();
}

void SnapshotDiffWidget::hideEvent(QHideEvent* event)
{
	timer_->stop();
	QWidget::hideEvent(event);
}

void SnapshotDiffWidget::poll_engine()
{
	auto& state = snapshot_diff::g_state;
	const auto published = SnapshotDiffController::instance().published_diff();
	if (published != adopted_diff_) {
		adopted_diff_ = published;
		model_->adopt(published);
		SnapshotDiffController::instance().set_selected_change(-1);
		detail_->clear_change();
	}
	timeline_->set_comparing(state.comparing.load(std::memory_order_acquire));
	timeline_->refresh_from_engine();
	refresh_presentation();
}

void SnapshotDiffWidget::refresh_presentation()
{
	controls_syncing_ = true;
	auto& state = snapshot_diff::g_state;
	const bool taking = state.capturing.load();
	const bool comparing = state.comparing.load();
	const bool loading = state.loading.load();
	const bool busy = taking || comparing || loading;
	const bool live_attach = driver_bridge::is_loaded() &&
		driver_bridge::attached_pid() != 0;
	std::size_t snapshot_count = 0;
	std::uint64_t selected_a = 0;
	std::uint64_t selected_b = 0;
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		snapshot_count = state.snapshots.size();
		selected_a = state.snap_a_id;
		selected_b = state.snap_b_id;
	}

	take_button_->setText(taking ? QStringLiteral("Capturing...")
		: QStringLiteral("Take Snapshot"));
	take_button_->setLoading(taking);
	take_button_->setEnabled(!busy && live_attach);
	compare_button_->setText(comparing ? QStringLiteral("Comparing...")
		: QStringLiteral("Compare"));
	compare_button_->setLoading(comparing);
	const bool can_compare = !busy && snapshot_count >= 2 &&
		selected_a != 0 && selected_b != 0 && selected_a != selected_b;
	compare_button_->setEnabled(can_compare);
	clear_button_->setEnabled(!busy && snapshot_count != 0);
	load_button_->setText(loading ? QStringLiteral("Loading...")
		: QStringLiteral("Load"));
	load_button_->setLoading(loading);
	load_button_->setEnabled(!busy);

	if (busy) {
		progress_->setVisible(true);
		progress_->setValue(static_cast<int>(
			state.progress.load() * 100.f));
		count_label_->setVisible(false);
	} else {
		progress_->setVisible(false);
		count_label_->setVisible(true);
		count_label_->setText(QStringLiteral("%1 snapshot%2")
			.arg(snapshot_count).arg(snapshot_count == 1 ? "" : "s"));
	}

	capability_notice_->setVisible(!live_attach);
	const QString last_error =
		QString::fromStdString(snapshot_diff::last_error());
	error_notice_->setMessage(last_error);
	error_notice_->setVisible(!last_error.isEmpty());

	const bool empty = model_->size() == 0;
	if (empty && snapshot_count == 0) {
		empty_hint_->setTitle(QStringLiteral("No snapshots yet"));
		empty_hint_->setMessage(QStringLiteral(
			"Capture two snapshots of the target process, then compare them to see what changed."));
	} else if (empty) {
		empty_hint_->setTitle(QStringLiteral("Nothing compared yet"));
		empty_hint_->setMessage(QStringLiteral(
			"Pick A and B. Click two markers on the timeline above and press Compare."));
	}
	table_->setVisible(!empty);
	empty_hint_->setVisible(empty);
	truncated_label_->setVisible(model_->truncated());
	refresh_detail();
	controls_syncing_ = false;
}

void SnapshotDiffWidget::refresh_detail()
{
	auto& controller = SnapshotDiffController::instance();
	const int selected = controller.selected_change();
	const auto* change = model_->row_at(selected);
	if (change) {
		detail_->set_change(*change);
		detail_->setVisible(true);
	} else {
		detail_->clear_change();
		detail_->setVisible(false);
	}
}

void SnapshotDiffWidget::on_diff_context(const QPoint& global_pos, int row, int origin)
{
	if (row >= 0) {
		SnapshotDiffController::instance().set_selected_change(row);
		refresh_detail();
	}
	const auto* change =
		model_->row_at(SnapshotDiffController::instance().selected_change());
	if (change)
		show_diff_menu(*change, global_pos, origin);
}

void SnapshotDiffWidget::show_diff_menu(
	const snapshot_diff::changed_region_t& change, const QPoint& global_pos, int origin)
{
	const auto diff = adopted_diff_;
	if (!diff)
		return;
	const auto workspace = disasm_view::capture_selected_workspace();
	const std::uint64_t workspace_generation =
		workspace.workspace ? workspace.workspace->generation() : 0;
	const std::uint32_t target_pid = driver_bridge::is_loaded()
		? driver_bridge::attached_pid() : 0;
	const std::uint64_t address = change.address;
	const std::uint32_t size = change.size;
	aida::ui::application_ui::retained_entity_context_t retained;
	retained.owner_id = "memory.snapshot.diff";
	retained.entity_id = std::to_string(address) + ":" + std::to_string(size);
	retained.entity_generation = workspace_generation;
	retained.active_view = aida::ui::stable_view_id_t("view.memory.snapshots");
	retained.validate_identity = [diff, address, size]() {
		if (SnapshotDiffController::instance().published_diff() != diff)
			return aida::ui::capability_state_t::unavailable(
				"The published snapshot diff changed; reopen the menu.");
		const bool exists = std::any_of(diff->changes.begin(), diff->changes.end(),
			[&](const auto& item) { return item.address == address && item.size == size; });
		return exists ? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(
				"The selected change is no longer part of the published diff.");
	};
	auto add = [&](const char* id, bool enabled, const char* reason, auto invoke) {
		retained.actions.push_back({id, enabled ? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(reason), invoke});
	};
	add("memory.entity.copy_address", address != 0, "The change has no address.",
		[address]() {
			char text[24]{};
			std::snprintf(text, sizeof(text), "0x%016llX",
				static_cast<unsigned long long>(address));
			clipboard::set_text(QString::fromLatin1(text));
			diag::log_tagged("scan_audit", "[scan_audit] snapshot_diff ctx copy_address");
			return aida::ui::action_handler_result_t::completed();
		});
	add("memory.entity.open_disassembly", workspace.workspace != nullptr,
		"Open a binary workspace before jumping to disassembly.", [address, workspace]() {
			if (auto* host = SnapshotDiffController::instance().host())
				static_cast<void>(host->open_or_focus(
					registry::stable_view_id_t("document.disassembly")));
			disasm_view::goto_address(address, workspace);
			diag::log_tagged("scan_audit", "[scan_audit] snapshot_diff ctx open_disasm");
			return aida::ui::action_handler_result_t::completed();
		});
	add("memory.entity.open_hex", target_pid != 0,
		"Attach to the target process first.", [workspace, address, size]() {
			if (hex_view::request_live_memory(workspace, address,
					size != 0 ? size : 256))
				if (auto* host = SnapshotDiffController::instance().host())
					static_cast<void>(host->open_or_focus(
						registry::stable_view_id_t("document.hex")));
			diag::log_tagged("scan_audit", "[scan_audit] snapshot_diff ctx open_hex");
			return aida::ui::action_handler_result_t::completed();
		});
	add("memory.result.add_address", target_pid != 0,
		"Attach to the target process first.", [address, size]() {
			const auto type = size <= 1 ? memory_scanner::value_type_t::byte_val
				: size <= 2 ? memory_scanner::value_type_t::int16_val
				: size <= 4 ? memory_scanner::value_type_t::int32_val
				: size <= 8 ? memory_scanner::value_type_t::int64_val
				: memory_scanner::value_type_t::byte_array;
			memory_scanner::add_address(address, "Snapshot diff change", type);
			ScannerController::instance().refresh_from_engine();
			diag::log_tagged("scan_audit", "[scan_audit] snapshot_diff ctx add_address");
			return aida::ui::action_handler_result_t::completed();
		});
	char address_text[24]{};
	std::snprintf(address_text, sizeof(address_text), "0x%016llX",
		static_cast<unsigned long long>(address));
	aida::automation_ui::entity_evidence::snapshot_t evidence;
	evidence.workspace_id = target_pid != 0 ? "pid:" + std::to_string(target_pid)
		: (workspace.workspace
			? workspace.workspace->identity().binary_id().to_hex() : std::string());
	evidence.source_view_id = "view.memory.snapshots";
	evidence.source_kind = "snapshot_diff_change";
	evidence.entity_id = retained.entity_id;
	evidence.display_label = std::string("Change at ") + address_text;
	evidence.excerpt = "PID: " + std::to_string(target_pid) +
		"\nAddress: " + address_text +
		"\nSize: " + std::to_string(size) +
		"\nType: " + snapshot_diff::detail::change_type_name(change.type) +
		"\nModule: " + change.module_name;
	evidence.address = address;
	evidence.revision = workspace_generation;
	evidence.generation = workspace_generation;
	evidence.sensitive = target_pid != 0;
	evidence.return_to_source = [diff, address, size](std::string& reason) {
		if (SnapshotDiffController::instance().published_diff() != diff) {
			reason = "The published snapshot diff changed; capture the change again.";
			return false;
		}
		const auto found = std::find_if(diff->changes.begin(), diff->changes.end(),
			[&](const auto& item) { return item.address == address && item.size == size; });
		if (found == diff->changes.end()) {
			reason = "The retained snapshot change is no longer published; capture it again.";
			return false;
		}
		SnapshotDiffController::instance().set_selected_change(static_cast<int>(
			std::distance(diff->changes.begin(), found)));
		if (auto* host = SnapshotDiffController::instance().host())
			static_cast<void>(host->open_or_focus(
				registry::stable_view_id_t("view.memory.snapshots")));
		reason.clear();
		return true;
	};
	aida::automation_ui::entity_evidence::append_actions(retained, std::move(evidence),
		address != 0 ? aida::ui::capability_state_t::available()
			: aida::ui::capability_state_t::unavailable(
				"The retained snapshot change has no address."));
	documents::show_retained_entity_menu(std::move(retained),
		origin == 1 ? aida::ui::context_menu_open_origin_t::menu_key
			: origin == 2 ? aida::ui::context_menu_open_origin_t::shift_f10
			: aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}
