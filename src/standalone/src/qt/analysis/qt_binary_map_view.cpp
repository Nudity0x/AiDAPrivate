#include "qt/analysis/qt_binary_map_view.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "helpers/diag_log.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/infra/event_bus.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/shell_host_contract.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_binary_map_canvas.hpp"
#include "qt/analysis/qt_binary_map_dialogs.hpp"
#include "qt/analysis/qt_binary_map_heatmap.hpp"
#include "qt/analysis/qt_binary_map_list_model.hpp"
#include "qt/analysis/qt_binary_map_shared.hpp"
#include "qt/analysis/qt_binary_map_strip.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_search_field.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::analysis {

QtBinaryMapView::QtBinaryMapView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.analysis.binary_map"));
    const auto& t = theme::tokens();
    setMinimumWidth(5 * static_cast<int>(t.shell.min_panel_w) + t.control.height_lg);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.binary_map.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(t.toolbar.padding_x, t.toolbar.padding_y,
        t.toolbar.padding_x, t.toolbar.padding_y);
    toolbar_layout->setSpacing(t.toolbar.group_gap);
    title_label_ = new QLabel(QStringLiteral("Binary Map"), toolbar);
    title_label_->setObjectName(QStringLiteral("aida.binary_map.title"));
    toolbar_layout->addWidget(title_label_);
    subtitle_label_ = new QLabel(toolbar);
    subtitle_label_->setObjectName(QStringLiteral("aida.binary_map.subtitle"));
    subtitle_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    toolbar_layout->addWidget(subtitle_label_);
    toolbar_layout->addStretch(1);
    mode_combo_ = new QComboBox(toolbar);
    mode_combo_->setObjectName(QStringLiteral("aida.binary_map.mode"));
    mode_combo_->setToolTip(QStringLiteral(
        "Choose static PE, live process, or merged map content"));
    mode_combo_->addItems({QStringLiteral("Auto"), QStringLiteral("Static"),
        QStringLiteral("Live")});
    toolbar_layout->addWidget(mode_combo_);
    refresh_button_ = new QPushButton(QStringLiteral("Refresh"), toolbar);
    refresh_button_->setObjectName(QStringLiteral("aida.binary_map.refresh"));
    refresh_button_->setToolTip(QStringLiteral(
        "Rebuild the map from the target's current state"));
    toolbar_layout->addWidget(refresh_button_);
    chat_button_ = new QPushButton(QStringLiteral("To chat"), toolbar);
    chat_button_->setObjectName(QStringLiteral("aida.binary_map.to_chat"));
    chat_button_->setToolTip(QStringLiteral(
        "Append the rendered binary map to the AI chat input"));
    toolbar_layout->addWidget(chat_button_);
    copy_button_ = new QPushButton(QStringLiteral("Copy"), toolbar);
    copy_button_->setObjectName(QStringLiteral("aida.binary_map.copy"));
    copy_button_->setToolTip(QStringLiteral(
        "Copy the rendered binary map to the clipboard"));
    toolbar_layout->addWidget(copy_button_);
    export_button_ = new QPushButton(QStringLiteral("Export"), toolbar);
    export_button_->setObjectName(QStringLiteral("aida.binary_map.export"));
    export_button_->setToolTip(QStringLiteral(
        "Export the current map snapshot to a file"));
    toolbar_layout->addWidget(export_button_);
    layout->addWidget(toolbar);

    filter_ = new widgets::AidaSearchField(
        QStringLiteral("Filter sections, regions, modules, imports..."), this);
    filter_->setObjectName(QStringLiteral("aida.binary_map.filter"));
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);

    splitter_ = new QSplitter(this);
    splitter_->setObjectName(QStringLiteral("aida.binary_map.splitter"));
    splitter_->setChildrenCollapsible(false);
    left_host_ = new QWidget(this);
    left_host_->setObjectName(QStringLiteral("aida.binary_map.canvas_host"));
    auto* left_layout = new QVBoxLayout(left_host_);
    left_layout->setContentsMargins(t.spacing.xs, t.spacing.xs,
        t.spacing.xs, t.spacing.xs);
    left_layout->setSpacing(t.spacing.xs);
    section_strip_ = new QtSectionStripWidget(left_host_);
    left_layout->addWidget(section_strip_);
    canvas_ = new QtAddressSpaceCanvas(left_host_);
    left_layout->addWidget(canvas_, 1);
    heatmap_ = new QtFunctionHeatmapWidget(left_host_);
    left_layout->addWidget(heatmap_, 1);
    legend_label_ = new QLabel(QStringLiteral(
        "image   mapped   private   stack   heap   guard   reserved"), left_host_);
    legend_label_->setObjectName(QStringLiteral("aida.binary_map.legend"));
    legend_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    left_layout->addWidget(legend_label_);
    live_stats_ = new QLabel(left_host_);
    live_stats_->setObjectName(QStringLiteral("aida.binary_map.live_stats"));
    live_stats_->setProperty("aidaVariant", QStringLiteral("secondary"));
    left_layout->addWidget(live_stats_);
    splitter_->addWidget(left_host_);
    list_model_ = new QtBinaryMapListModel(this);
    list_ = new QTableView(this);
    list_->setObjectName(QStringLiteral("aida.binary_map.list"));
    list_->verticalHeader()->setVisible(false);
    list_->horizontalHeader()->setVisible(false);
    list_->horizontalHeader()->setStretchLastSection(true);
    list_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    list_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    list_->setSelectionBehavior(QAbstractItemView::SelectRows);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    list_->setShowGrid(false);
    list_->setAlternatingRowColors(false);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setModel(list_model_);
    splitter_->addWidget(list_);
    splitter_->setStretchFactor(0, 58);
    splitter_->setStretchFactor(1, 42);
    layout->addWidget(splitter_, 1);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.binary_map.state_view"));
    state_view_->setState(widgets::AidaStateView::State::Empty);
    state_view_->setTitle(QStringLiteral("No binary or process"));
    state_view_->setMessage(QStringLiteral(
        "Open a file or attach to a process to inspect its binary map."));
    layout->addWidget(state_view_, 1);

    timer_ = new QTimer(this);
    timer_->setInterval(66);
    connect(timer_, &QTimer::timeout, this, [this] { pollRefreshFlags(); });

    connect(mode_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!state_) return;
        const auto pref = static_cast<qt_binary_map_display_mode_t>(index);
        const auto previous = state_->mode_pref;
        state_->mode_pref = pref;
        diag::log_tagged_fmt("binary_map", "mode_toggle prev=%d new=%d",
            static_cast<int>(previous), static_cast<int>(pref));
        state_->refresh_requested.store(true);
        state_->live_refresh_requested.store(true);
    });
    connect(refresh_button_, &QPushButton::clicked, this, [this] {
        if (!state_) return;
        diag::log_tagged_fmt("binary_map", "toolbar refresh_clicked mode_pref=%d",
            static_cast<int>(state_->mode_pref));
        state_->refresh_requested.store(true);
        state_->live_refresh_requested.store(true);
    });
    connect(chat_button_, &QPushButton::clicked, this, [this] { sendToChat(); });
    connect(copy_button_, &QPushButton::clicked, this, [this] {
        if (!state_) return;
        const auto payload = std::atomic_load_explicit(&state_->rendered_text,
            std::memory_order_acquire);
        clipboard::set_text(payload
            ? QString::fromStdString(*payload) : QString());
        QtAnalysisBridge::instance().toastInfo(
            QStringLiteral("Binary map copied to clipboard"), 3.0);
    });
    connect(export_button_, &QPushButton::clicked, this, [this] { exportSnapshot(); });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (!state_) return;
        state_->filter = text;
        state_->filter_lower = bm_to_lower_copy(text.toStdString());
        rebuildList();
    });
    connect(section_strip_, &QtSectionStripWidget::jumpToAddress, this,
            [this](quint64 va) {
        if (state_) bm_jump_to_address(*state_, va);
    });
    connect(section_strip_, &QtSectionStripWidget::openHex, this,
            [this](quint64 va, std::size_t size) {
        if (state_) bm_jump_to_hex(*state_, va, size);
    });
    connect(canvas_, &QtAddressSpaceCanvas::regionSelected, this,
            [this](quint64 base) {
        if (state_) state_->live_selected_base.store(base);
    });
    connect(canvas_, &QtAddressSpaceCanvas::regionDoubleClicked, this,
            [this](quint64 base) {
        if (state_) bm_jump_to_address(*state_, base);
    });
    connect(heatmap_, &QtFunctionHeatmapWidget::functionClicked, this,
            [this](quint64 va) {
        if (state_) state_->selected_va.store(va);
    });
    connect(heatmap_, &QtFunctionHeatmapWidget::functionDoubleClicked, this,
            [this](quint64 va) {
        if (state_) bm_jump_to_address(*state_, va);
    });
    connect(heatmap_, &QtFunctionHeatmapWidget::functionMenuRequested, this,
            [this](quint64 va, const QPoint& global_pos) {
        if (!state_ || va == 0) return;
        const auto map_snapshot = std::atomic_load_explicit(&state_->map,
            std::memory_order_acquire);
        const aida::binary_map::map_function_t* function = nullptr;
        if (map_snapshot) {
            for (const auto& fn : map_snapshot->functions) {
                if (fn.va == va) {
                    function = &fn;
                    break;
                }
            }
        }
        if (!function) return;
        const auto fn = *function;
        state_->selected_va.store(va);
        aida::ui::application_ui::retained_entity_context_t retained;
        retained.owner_id = "analysis.binary_map.heat_function";
        retained.entity_id = std::to_string(va) + ":" + fn.name;
        retained.entity_generation =
            state_->workspace_generation.load(std::memory_order_acquire);
        retained.active_view = aida::ui::stable_view_id_t("view.analysis.binary_map");
        auto* state = state_.get();
        const auto generation = retained.entity_generation;
        retained.validate_identity = [state, va, generation] {
            return state->workspace_generation.load(std::memory_order_acquire) ==
                    generation &&
                state->selected_va.load() == va
                ? aida::ui::capability_state_t::available()
                : aida::ui::capability_state_t::unavailable(
                    "The Binary Map workspace or selected function changed");
        };
        const auto add = [&retained](const char* id, auto invoke) {
            aida::ui::application_ui::retained_entity_action_t action;
            action.action_id = id;
            action.capability = aida::ui::capability_state_t::available();
            action.invoke = std::move(invoke);
            retained.actions.push_back(std::move(action));
        };
        add("analysis.binary_map.function.follow_disassembly", [state, va] {
            bm_jump_to_address(*state, va);
            return aida::ui::action_handler_result_t::completed();
        });
        add("analysis.binary_map.function.open_hex", [state, va] {
            bm_jump_to_hex(*state, va, 0x400);
            return aida::ui::action_handler_result_t::completed();
        });
        QtAnalysisBridge::instance().showRetainedMenu(retained,
            aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
    });
    connect(list_, &QTableView::clicked, this, [this](const QModelIndex& index) {
        if (!state_ || !index.isValid()) return;
        const auto* row = list_model_->rowAt(index.row());
        if (!row) return;
        if (row->kind == QtBinaryMapListModel::row_kind_t::group_header ||
            row->kind == QtBinaryMapListModel::row_kind_t::import_dll) {
            auto& groups = state_->collapsed_groups;
            if (groups.count(row->group_key)) groups.erase(row->group_key);
            else groups.insert(row->group_key);
            diag::log_tagged_fmt("binary_map", "group_toggle key='%s'",
                row->group_key.c_str());
            rebuildList();
            return;
        }
        if (row->kind == QtBinaryMapListModel::row_kind_t::region) {
            state_->live_selected_base.store(row->region.base);
            return;
        }
        if (row->kind == QtBinaryMapListModel::row_kind_t::function) {
            state_->selected_va.store(row->function.va);
            return;
        }
        if (row->kind == QtBinaryMapListModel::row_kind_t::global) {
            state_->selected_entity_id = "global:" + std::to_string(row->global.va);
            bm_jump_to_hex(*state_, row->global.va, 0x200);
            return;
        }
        if (row->kind == QtBinaryMapListModel::row_kind_t::export_entry) {
            state_->selected_entity_id = "export:" + row->export_name;
            if (row->export_va != 0) {
                bm_jump_to_address(*state_, row->export_va);
            } else {
                clipboard::set_text(QString::fromStdString(row->export_name));
                QtAnalysisBridge::instance().toastInfo(
                    QStringLiteral("Export name copied (no VA resolved)"), 2.5);
            }
            return;
        }
        if (row->kind == QtBinaryMapListModel::row_kind_t::import_function) {
            const std::string clip = row->dll + "!" + row->function_name;
            clipboard::set_text(QString::fromStdString(clip));
            QtAnalysisBridge::instance().toastInfo(
                QStringLiteral("Import symbol copied"), 2.0);
        }
    });
    connect(list_, &QTableView::activated, this, [this](const QModelIndex& index) {
        if (!state_ || !index.isValid()) return;
        const auto* row = list_model_->rowAt(index.row());
        if (!row) return;
        switch (row->kind) {
        case QtBinaryMapListModel::row_kind_t::region:
            bm_jump_to_address(*state_, row->region.base); break;
        case QtBinaryMapListModel::row_kind_t::module:
            bm_jump_to_address(*state_, row->module.base); break;
        case QtBinaryMapListModel::row_kind_t::section:
            bm_jump_to_hex(*state_, row->section.va,
                bm_hex_request_size(row->section.size)); break;
        case QtBinaryMapListModel::row_kind_t::function:
            bm_jump_to_address(*state_, row->function.va); break;
        case QtBinaryMapListModel::row_kind_t::global:
            bm_jump_to_address(*state_, row->global.va); break;
        default: break;
        }
    });
    connect(list_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = list_->indexAt(pos);
        if (index.isValid())
            showRowMenu(list_->viewport()->mapToGlobal(pos), index.row());
    });
    list_->installEventFilter(this);

    connect(&QtAnalysisBridge::instance(), &QtAnalysisBridge::activeContextChanged,
            this, [this](QtWorkspaceContext* context) { rebindContext(context); });
    rebindContext(QtAnalysisBridge::instance().activeContext());
}

void QtBinaryMapView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    timer_->start();
    if (state_ && !state_->auto_refreshed_once) {
        const auto mode = bm_resolve_active_mode(*state_, state_->mode_pref);
        const bool want_static = mode == qt_binary_map_active_mode_t::pe_static ||
            mode == qt_binary_map_active_mode_t::merged;
        const bool want_live = mode == qt_binary_map_active_mode_t::live_process ||
            mode == qt_binary_map_active_mode_t::merged;
        if (want_static && !state_->refreshing.load())
            state_->refresh_requested.store(true);
        if (want_live && !state_->live_refreshing.load() &&
            bm_live_available(*state_))
            state_->live_refresh_requested.store(true);
        state_->auto_refreshed_once = true;
    }
}

void QtBinaryMapView::hideEvent(QHideEvent* event) {
    timer_->stop();
    QWidget::hideEvent(event);
}

bool QtBinaryMapView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == list_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Menu ||
            (key->key() == Qt::Key_F10 &&
                key->modifiers().testFlag(Qt::ShiftModifier))) {
            const auto current = list_->currentIndex();
            if (current.isValid())
                showRowMenu(list_->viewport()->mapToGlobal(
                    list_->visualRect(current).center()), current.row());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QtBinaryMapView::rebindContext(QtWorkspaceContext* context) {
    if (context_ == context) return;
    context_ = context;
    state_ = context ? context->binaryMapState : nullptr;
    if (state_) {
        if (!state_->initialized) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->initialized) {
                state_->opts.max_functions = 200;
                state_->opts.max_globals = 60;
                state_->opts.max_callees_per_function = 5;
                state_->opts.max_chars = 16384;
                state_->opts.include_imports = true;
                state_->opts.include_exports = true;
                state_->canvas_zoom = 1.f;
                state_->canvas_offset_norm = 0.0;
                state_->initialized = true;
                diag::log_tagged_fmt("binary_map",
                    "view_initialize max_functions=%d max_globals=%d max_chars=%zu include_imp=%d include_exp=%d",
                    state_->opts.max_functions, state_->opts.max_globals,
                    state_->opts.max_chars, state_->opts.include_imports ? 1 : 0,
                    state_->opts.include_exports ? 1 : 0);
            }
        }
        state_->workspace_generation.store(
            context->workspace().lock()
                ? context->workspace().lock()->generation() : 0,
            std::memory_order_release);
        mode_combo_->setCurrentIndex(static_cast<int>(state_->mode_pref));
        if (!state_->filter.isEmpty() && filter_->text() != state_->filter)
            filter_->setText(state_->filter);
        if (!state_->filter.isEmpty()) state_->filter_lower =
            bm_to_lower_copy(state_->filter.toStdString());
    }
    rebuildList();
    refreshPresentation();
}

void QtBinaryMapView::pollRefreshFlags() {
    if (!state_) return;
    auto& s = *state_;
    const auto mode = bm_resolve_active_mode(s, s.mode_pref);
    const int prev_mode = s.active_mode_atomic.exchange(static_cast<int>(mode));
    if (prev_mode != static_cast<int>(mode)) {
        diag::log_tagged_fmt("binary_map",
            "mode_switch from=%d to=%d pref=%d live_available=%d static_available=%d",
            prev_mode, static_cast<int>(mode), static_cast<int>(s.mode_pref),
            bm_live_available(s) ? 1 : 0, bm_static_available(s) ? 1 : 0);
    }
    const bool want_static = mode == qt_binary_map_active_mode_t::pe_static ||
        mode == qt_binary_map_active_mode_t::merged;
    const bool want_live = mode == qt_binary_map_active_mode_t::live_process ||
        mode == qt_binary_map_active_mode_t::merged;
    if (s.refresh_requested.exchange(false) && want_static)
        bm_perform_refresh(state_);
    if (s.live_refresh_requested.exchange(false) && want_live)
        bm_perform_live_refresh(state_);
    if (s.refresh_after_pin_requested) {
        s.refresh_after_pin_requested = false;
        s.refresh_requested.store(true);
    }
    // Live-drift trigger (07 sec. 6.1): refresh when the attached PID changed or a
    // live workspace has no cached regions.
    if (want_live && bm_live_available(s) && !s.live_refreshing.load()) {
        const auto workspace = s.workspace.lock();
        const auto process = workspace ? workspace->identity().process()
            : std::optional<aida::analysis::process_identity_t>{};
        const std::uint32_t live_pid_attached = process ? process->pid : 0;
        std::uint32_t cached_pid = 0;
        std::size_t cached_regions = 0;
        const auto cached_live = std::atomic_load_explicit(&s.live,
            std::memory_order_acquire);
        if (cached_live) {
            cached_pid = cached_live->pid;
            cached_regions = cached_live->regions.size();
        }
        if (cached_pid != live_pid_attached ||
            (live_pid_attached != 0 && cached_regions == 0)) {
            diag::log_tagged_fmt("binary_map",
                "live_auto_refresh_trigger cached_pid=%u attached_pid=%u cached_regions=%zu",
                cached_pid, live_pid_attached, cached_regions);
            s.live_refresh_requested.store(true);
        }
    }
    // Adopt published snapshots into the canvases + list when they change.
    const auto map = std::atomic_load_explicit(&s.map, std::memory_order_acquire);
    const auto live = std::atomic_load_explicit(&s.live, std::memory_order_acquire);
    if (map != last_map_ || live != last_live_) {
        last_map_ = map;
        last_live_ = live;
        section_strip_->setSections(map);
        canvas_->setContent(live, s.live_selected_base.load());
        heatmap_->setFunctions(map, s.selected_va.load());
        rebuildList();
        refreshPresentation();
    }
    canvas_->setContent(live, s.live_selected_base.load());
}

void QtBinaryMapView::rebuildList() {
    if (!state_) {
        list_model_->rebuild(*state_, nullptr, nullptr,
            qt_binary_map_active_mode_t::none);
        return;
    }
    const auto map = std::atomic_load_explicit(&state_->map,
        std::memory_order_acquire);
    const auto live = std::atomic_load_explicit(&state_->live,
        std::memory_order_acquire);
    list_model_->rebuild(*state_, map, live,
        bm_resolve_active_mode(*state_, state_->mode_pref));
}

void QtBinaryMapView::refreshPresentation() {
    if (!state_ || state_->workspace.expired()) {
        state_view_->setVisible(true);
        splitter_->setVisible(false);
        return;
    }
    const auto mode = bm_resolve_active_mode(*state_, state_->mode_pref);
    state_view_->setVisible(false);
    splitter_->setVisible(true);
    const char* mode_text = mode == qt_binary_map_active_mode_t::merged
        ? "merged: static PE + live process"
        : mode == qt_binary_map_active_mode_t::live_process ? "live process"
        : mode == qt_binary_map_active_mode_t::pe_static ? "static PE"
        : "no target";
    const auto map = std::atomic_load_explicit(&state_->map,
        std::memory_order_acquire);
    const auto live = std::atomic_load_explicit(&state_->live,
        std::memory_order_acquire);
    QString subtitle = QString::fromLatin1(mode_text);
    if (live && (mode == qt_binary_map_active_mode_t::live_process ||
            mode == qt_binary_map_active_mode_t::merged)) {
        subtitle = live->pid != 0
            ? QStringLiteral("%1  -  PID %2  %3  regions %4")
                .arg(QString::fromLatin1(mode_text)).arg(live->pid)
                .arg(QString::fromStdString(live->process_name))
                .arg(live->regions.size())
            : QStringLiteral("%1  -  no active process")
                .arg(QString::fromLatin1(mode_text));
    } else if (mode == qt_binary_map_active_mode_t::pe_static && map) {
        subtitle = QStringLiteral("%1  -  %2  %3  base 0x%4  %5")
            .arg(QString::fromLatin1(mode_text))
            .arg(map->module_name.empty() ? QStringLiteral("(generating)")
                : QString::fromStdString(map->module_name))
            .arg(QString::fromStdString(map->format))
            .arg(map->image_base, 0, 16)
            .arg(QString::fromStdString(bm_format_size_human(map->image_size)));
    }
    subtitle_label_->setText(subtitle);
    const bool refreshing = state_->refreshing.load() ||
        state_->live_refreshing.load();
    refresh_button_->setEnabled(!refreshing);
    refresh_button_->setText(refreshing ? QStringLiteral("Refreshing")
        : QStringLiteral("Refresh"));
    const bool exporting = state_->export_pending.load(std::memory_order_acquire);
    export_button_->setEnabled(!exporting);
    export_button_->setText(exporting ? QStringLiteral("Exporting")
        : QStringLiteral("Export"));
    if (live) {
        live_stats_->setText(QStringLiteral(
            "REGIONS %1   MODULES %2   THREADS %3   COMMITTED %4   RESERVED %5   RWX %6")
            .arg(live->regions.size()).arg(live->modules.size())
            .arg(live->threads.size())
            .arg(QString::fromStdString(bm_format_size_human(live->total_committed)))
            .arg(QString::fromStdString(bm_format_size_human(live->total_reserved)))
            .arg(live->rwx_count));
    } else {
        live_stats_->clear();
    }
}

void QtBinaryMapView::sendToChat() {
    if (!state_) return;
    const auto rendered = std::atomic_load_explicit(&state_->rendered_text,
        std::memory_order_acquire);
    std::string payload = rendered ? *rendered : std::string{};
    const auto mode = bm_resolve_active_mode(*state_, state_->mode_pref);
    const auto live = std::atomic_load_explicit(&state_->live,
        std::memory_order_acquire);
    if (mode == qt_binary_map_active_mode_t::live_process && live) {
        std::string live_payload = "Live memory map:\n";
        std::uint64_t committed_total = 0;
        std::uint32_t rwx_total = 0;
        for (const auto& r : live->regions) {
            if (r.is_committed) committed_total += r.size;
            const bool exec = (r.protect & 0xF0) != 0;
            const std::uint32_t low = r.protect & 0xFF;
            const bool write = (low == 0x04) || (low == 0x08) || (low == 0x40) ||
                (low == 0x80);
            if (exec && write) ++rwx_total;
        }
        const std::string committed_str = bm_format_size_human(committed_total);
        char header[200];
        std::snprintf(header, sizeof(header),
            "PID %u %s  regions=%zu  committed=%s  RWX=%u\n",
            static_cast<unsigned>(live->pid), live->process_name.c_str(),
            live->regions.size(), committed_str.c_str(),
            static_cast<unsigned>(rwx_total));
        live_payload += header;
        for (std::size_t i = 0; i < live->regions.size() && i < 64; ++i)
            live_payload += bm_make_region_chat_payload(live->regions[i]);
        payload = live_payload;
    }
    diag::log_tagged_fmt("binary_map", "toolbar to_chat bytes=%zu", payload.size());
    if (payload.empty()) {
        QtAnalysisBridge::instance().toastWarning(
            QStringLiteral("Binary map is empty; refresh first"), 3.0);
    } else {
        // S10: the chat injection contract replaces the g_chat_buf write.
        QtAnalysisBridge::instance().injectToChatText(
            QString::fromStdString(payload));
    }
}

void QtBinaryMapView::exportSnapshot() {
    if (!state_) return;
    const auto mode = bm_resolve_active_mode(*state_, state_->mode_pref);
    const auto live = std::atomic_load_explicit(&state_->live,
        std::memory_order_acquire);
    if (mode == qt_binary_map_active_mode_t::live_process ||
        mode == qt_binary_map_active_mode_t::merged) {
        char default_name[64]{};
        std::snprintf(default_name, sizeof(default_name), "memory_map_pid%u.json",
            static_cast<unsigned>(live ? live->pid : 0));
        const auto path = dialogs::save_file(this, QStringLiteral("Export Memory Map"),
            "JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0", QStringLiteral("json"),
            QString::fromLatin1(default_name));
        if (!path || path->empty()) return;
        if (!bm_queue_snapshot_export(state_, *path, "Export live memory map", live,
                {}))
            QtAnalysisBridge::instance().toastError(QStringLiteral(
                "The memory-map export could not be queued; see Task Center"), 3.0);
    } else {
        const auto payload = std::atomic_load_explicit(&state_->rendered_text,
            std::memory_order_acquire);
        const auto path = dialogs::save_file(this, QStringLiteral("Export Binary Map"),
            "Text (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0", QStringLiteral("txt"),
            QStringLiteral("binary_map.txt"));
        if (!path || path->empty()) return;
        if (!bm_queue_snapshot_export(state_, *path, "Export static Binary Map", {},
                payload))
            QtAnalysisBridge::instance().toastError(QStringLiteral(
                "The Binary Map export could not be queued; see Task Center"), 3.0);
    }
}

void show_binary_map_menu(QtBinaryMapView* view, QWidget* parent,
    const QPoint& global_pos, int view_row);

void QtBinaryMapView::showRowMenu(const QPoint& global_pos, int view_row) {
    // Full region/module/section/function/global/import/export retained menus
    // are implemented in qt_binary_map_menus.cpp.
    show_binary_map_menu(this, this, global_pos, view_row);
}

}
