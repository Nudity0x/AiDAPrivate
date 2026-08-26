#include "qt/analysis/qt_fuzzer_view.hpp"

#include <QCheckBox>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/runtime/standalone_driver.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/shell_host_contract.hpp"
#include "helpers/diag_log.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_fuzzer_canvas.hpp"
#include "qt/analysis/qt_fuzzer_model.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::analysis {

QtFuzzerView::QtFuzzerView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.analysis.fuzzer"));
    const auto& tokens = theme::tokens();
    setMinimumWidth(5 * static_cast<int>(tokens.shell.min_panel_w) +
        tokens.control.height_lg);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto& fz = fuzzer_engine::g_state;

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.fuzzer.toolbar"));
    auto* toolbar_layout = new QVBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.spacing.xs);
    const QFontMetricsF ui_metrics(font());
    const auto field_w = [&tokens](const QFontMetricsF& metrics,
                               const QString& sample) {
        return static_cast<int>(metrics.horizontalAdvance(sample)) +
            2 * tokens.table.cell_pad_x + tokens.spacing.lg;
    };
    auto* address_row = new QHBoxLayout();
    addr_edit_ = new widgets::AidaLineEdit(QStringLiteral("Target address"), toolbar);
    addr_edit_->setObjectName(QStringLiteral("aida.fuzzer.addr"));
    addr_edit_->setToolTip(QStringLiteral(
        "Fuzzing entry point address, in hexadecimal"));
    addr_edit_->setText(QString::fromLatin1(fz.addr_input));
    address_row->addWidget(addr_edit_);
    end_edit_ = new widgets::AidaLineEdit(QStringLiteral("End address"), toolbar);
    end_edit_->setObjectName(QStringLiteral("aida.fuzzer.end"));
    end_edit_->setToolTip(QStringLiteral(
        "Optional end address bounding the fuzzed range, in hexadecimal"));
    end_edit_->setText(QString::fromLatin1(fz.end_addr_input));
    address_row->addWidget(end_edit_);
    input_edit_ = new widgets::AidaLineEdit(QStringLiteral("Input address"), toolbar);
    input_edit_->setObjectName(QStringLiteral("aida.fuzzer.input"));
    input_edit_->setToolTip(QStringLiteral(
        "Address of the mutation input buffer, in hexadecimal"));
    input_edit_->setText(QString::fromLatin1(fz.input_addr));
    address_row->addWidget(input_edit_);
    input_size_edit_ = new widgets::AidaLineEdit(QStringLiteral("size"), toolbar);
    input_size_edit_->setObjectName(QStringLiteral("aida.fuzzer.input_size"));
    input_size_edit_->setToolTip(QStringLiteral(
        "Size of the mutation input buffer in bytes"));
    input_size_edit_->setMinimumWidth(
        field_w(ui_metrics, QStringLiteral("4294967295")));
    input_size_edit_->setText(QString::fromLatin1(fz.input_size_str));
    address_row->addWidget(input_size_edit_);
    max_iter_edit_ = new widgets::AidaLineEdit(QStringLiteral("max iter"), toolbar);
    max_iter_edit_->setObjectName(QStringLiteral("aida.fuzzer.max_iter"));
    max_iter_edit_->setToolTip(QStringLiteral(
        "Maximum number of fuzzing iterations"));
    max_iter_edit_->setMinimumWidth(
        field_w(ui_metrics, QStringLiteral("4294967295")));
    max_iter_edit_->setText(QString::fromLatin1(fz.max_iter_str));
    address_row->addWidget(max_iter_edit_);
    toolbar_layout->addLayout(address_row);

    auto* strategy_row = new QHBoxLayout();
    static const char* k_strategy_labels[6] = {
        "Bit", "Byte", "Arith", "Interesting", "Havoc", "Splice"
    };
    for (int i = 0; i < 6; ++i) {
        strategy_checks_[i] = new QCheckBox(QString::fromLatin1(k_strategy_labels[i]),
            toolbar);
        strategy_checks_[i]->setChecked(fz.config.strategies[i]);
        connect(strategy_checks_[i], &QCheckBox::checkStateChanged, this,
            [this, i](Qt::CheckState state) {
            fuzzer_engine::g_state.config.strategies[i] =
                state == Qt::Checked;
        });
        strategy_row->addWidget(strategy_checks_[i]);
    }
    strategy_row->addStretch(1);
    toolbar_layout->addLayout(strategy_row);

    auto* button_row = new QHBoxLayout();
    start_button_ = new QPushButton(QStringLiteral("Start Fuzzing"), toolbar);
    start_button_->setObjectName(QStringLiteral("aida.fuzzer.start"));
    start_button_->setToolTip(QStringLiteral(
        "Start fuzzing the target with the configured strategies"));
    stop_button_ = new QPushButton(QStringLiteral("Stop"), toolbar);
    stop_button_->setObjectName(QStringLiteral("aida.fuzzer.stop"));
    stop_button_->setToolTip(QStringLiteral("Stop the active fuzzing run"));
    export_button_ = new QPushButton(QStringLiteral("Export"), toolbar);
    export_button_->setObjectName(QStringLiteral("aida.fuzzer.export"));
    export_button_->setToolTip(QStringLiteral(
        "Export the unique-crash catalog to disk"));
    import_button_ = new QPushButton(QStringLiteral("Import"), toolbar);
    import_button_->setObjectName(QStringLiteral("aida.fuzzer.import"));
    import_button_->setToolTip(QStringLiteral(
        "Import a previously exported crash catalog"));
    reset_button_ = new QPushButton(QStringLiteral("Reset"), toolbar);
    reset_button_->setObjectName(QStringLiteral("aida.fuzzer.reset"));
    reset_button_->setToolTip(QStringLiteral(
        "Clear all fuzzer state and the retained crash catalog"));
    button_row->addWidget(start_button_);
    button_row->addWidget(stop_button_);
    button_row->addWidget(export_button_);
    button_row->addWidget(import_button_);
    button_row->addWidget(reset_button_);
    button_row->addStretch(1);
    status_pill_ = new QLabel(QStringLiteral("Idle"), toolbar);
    status_pill_->setObjectName(QStringLiteral("aida.fuzzer.status"));
    status_pill_->setProperty("aidaVariant", QStringLiteral("neutral"));
    button_row->addWidget(status_pill_);
    toolbar_layout->addLayout(button_row);
    layout->addWidget(toolbar);

    auto* stats_grid = new QFrame(this);
    stats_grid->setObjectName(QStringLiteral("aida.fuzzer.stats"));
    auto* grid = new QHBoxLayout(stats_grid);
    grid->setContentsMargins(tokens.toolbar.padding_x, tokens.toolbar.padding_y,
        tokens.toolbar.padding_x, tokens.toolbar.padding_y);
    grid->setSpacing(tokens.spacing.sm);
    static const char* k_stat_labels[8] = {
        "Executions", "Speed", "Crashes", "Unique",
        "Edges", "New Cov", "Corpus", "Elapsed"
    };
    for (int i = 0; i < 8; ++i) {
        auto* cell = new QFrame(stats_grid);
        auto* cell_layout = new QVBoxLayout(cell);
        cell_layout->setContentsMargins(tokens.spacing.sm, tokens.spacing.xs,
            tokens.spacing.sm, tokens.spacing.xs);
        auto* name = new QLabel(QString::fromLatin1(k_stat_labels[i]), cell);
        name->setProperty("aidaVariant", QStringLiteral("secondary"));
        auto* value = new QLabel(QStringLiteral("0"), cell);
        cell_layout->addWidget(name);
        cell_layout->addWidget(value);
        grid->addWidget(cell);
        stat_labels_[i] = value;
    }
    layout->addWidget(stats_grid);

    canvas_ = new QtFuzzerCanvas(this);
    layout->addWidget(canvas_);

    auto* crashes_header = new QHBoxLayout();
    auto* crashes_title = new QLabel(QStringLiteral("Unique Crashes"), this);
    crashes_title->setObjectName(QStringLiteral("aida.fuzzer.crashes_title"));
    crashes_header->addWidget(crashes_title);
    truncated_badge_ = new QLabel(QStringLiteral("Catalog bounded"), this);
    truncated_badge_->setObjectName(QStringLiteral("aida.fuzzer.truncated_badge"));
    truncated_badge_->setProperty("aidaVariant", QStringLiteral("warning"));
    truncated_badge_->setToolTip(QStringLiteral(
        "Additional unique crashes were counted but not retained after the bounded catalog reached its memory limit."));
    truncated_badge_->setVisible(false);
    crashes_header->addWidget(truncated_badge_);
    crashes_header->addStretch(1);
    layout->addLayout(crashes_header);

    model_ = new QtFuzzerCrashModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.fuzzer.table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(tokens.table.row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* horizontal = table_->horizontalHeader();
    using Column = QtFuzzerCrashModel::Column;
    const QFontMetricsF code_metrics(theme::fonts::codeRegular());
    const auto with_cell_pad = [&tokens](int content) {
        return content + 2 * tokens.table.cell_pad_x + tokens.spacing.xs;
    };
    horizontal->setSectionResizeMode(static_cast<int>(Column::index), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::index),
        with_cell_pad(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("#999")))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::score), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::score),
        with_cell_pad(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("CRITICAL")))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::type), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::type),
        with_cell_pad(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Invalid Instruction")))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::address),
        QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::address),
        with_cell_pad(static_cast<int>(code_metrics.horizontalAdvance(
            QStringLiteral("0xDDDDDDDDDDDDDDDD")))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::instruction),
        QHeaderView::Stretch);
    horizontal->setSectionResizeMode(static_cast<int>(Column::description),
        QHeaderView::Stretch);
    horizontal->setStretchLastSection(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.fuzzer.state_view"));
    state_view_->setState(widgets::AidaStateView::State::Empty);
    state_view_->setTitle(QStringLiteral("Configure target and start fuzzing"));
    state_view_->setMessage(QStringLiteral(
        "Enter a target address and input region above, then press Start Fuzzing."));
    layout->addWidget(state_view_, 1);
    layout->addWidget(table_, 1);

    detail_panel_ = new QFrame(this);
    detail_panel_->setObjectName(QStringLiteral("aida.fuzzer.detail"));
    detail_panel_->setProperty("aidaRole", QStringLiteral("panel"));
    auto* detail_layout = new QFormLayout(detail_panel_);
    detail_title_ = new QLabel(detail_panel_);
    detail_title_->setObjectName(QStringLiteral("aida.fuzzer.detail.title"));
    detail_instruction_ = new QLabel(detail_panel_);
    detail_instruction_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_instruction_->setWordWrap(true);
    detail_fault_ = new QLabel(detail_panel_);
    detail_fault_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_fault_->setWordWrap(true);
    detail_registers_ = new QLabel(detail_panel_);
    detail_registers_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_registers_->setWordWrap(true);
    detail_mutation_ = new QLabel(detail_panel_);
    detail_mutation_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_mutation_->setWordWrap(true);
    detail_input_ = new QLabel(detail_panel_);
    detail_input_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_input_->setWordWrap(true);
    detail_minimized_ = new QLabel(detail_panel_);
    detail_minimized_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_minimized_->setWordWrap(true);
    detail_ai_ = new QLabel(detail_panel_);
    detail_ai_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_ai_->setWordWrap(true);
    detail_layout->addRow(detail_title_);
    detail_layout->addRow(QStringLiteral("Instruction"), detail_instruction_);
    detail_layout->addRow(QStringLiteral("Fault / RIP"), detail_fault_);
    detail_layout->addRow(QStringLiteral("Registers"), detail_registers_);
    detail_layout->addRow(QStringLiteral("Mutation"), detail_mutation_);
    detail_layout->addRow(QStringLiteral("Input"), detail_input_);
    detail_layout->addRow(QStringLiteral("Minimized"), detail_minimized_);
    detail_layout->addRow(QStringLiteral("AI Analysis"), detail_ai_);
    auto* detail_buttons = new QHBoxLayout();
    analyze_button_ = new QPushButton(QStringLiteral("AI Analyze"), detail_panel_);
    analyze_button_->setObjectName(QStringLiteral("aida.fuzzer.analyze"));
    analyze_button_->setToolTip(QStringLiteral(
        "Send the selected crash to the AI provider for analysis"));
    minimize_button_ = new QPushButton(QStringLiteral("Minimize"), detail_panel_);
    minimize_button_->setObjectName(QStringLiteral("aida.fuzzer.minimize"));
    minimize_button_->setToolTip(QStringLiteral(
        "Shrink the selected crash's reproducer input"));
    detail_buttons->addWidget(analyze_button_);
    detail_buttons->addWidget(minimize_button_);
    detail_buttons->addStretch(1);
    detail_layout->addRow(detail_buttons);
    detail_panel_->setVisible(false);
    layout->addWidget(detail_panel_);

    timer_ = new QTimer(this);
    timer_->setInterval(66);
    connect(timer_, &QTimer::timeout, this, [this] { pollEngine(); });

    connect(start_button_, &QPushButton::clicked, this, [this] { startFuzzing(); });
    connect(stop_button_, &QPushButton::clicked, this, [] {
        diag::log_tagged("fuzzer", "fuzz_stop_requested");
        fuzzer_engine::stop_fuzzing();
    });
    connect(export_button_, &QPushButton::clicked, this, [] {
        diag::log_tagged("fuzzer", "export_requested");
        fuzzer_engine::export_crashes();
    });
    connect(import_button_, &QPushButton::clicked, this, [] {
        diag::log_tagged("fuzzer", "import_requested");
        fuzzer_engine::import_crashes();
    });
    connect(reset_button_, &QPushButton::clicked, this, [this] { resetState(); });
    connect(table_, &QTableView::clicked, this, [this](const QModelIndex& index) {
        if (!index.isValid()) return;
        const bool same = selected_crash_ == index.row();
        selected_crash_ = same ? -1 : index.row();
        const auto* crash = model_->rowAt(index.row());
        selected_crash_hash_ = crash && !same ? crash->crash_hash : 0;
        updateDetailPanel();
    });
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = table_->indexAt(pos);
        if (index.isValid())
            showCrashMenu(table_->viewport()->mapToGlobal(pos), index.row());
    });
    connect(analyze_button_, &QPushButton::clicked, this, [this] {
        if (fuzzer_engine::g_state.analyzing_crash.load()) return;
        const auto* crash = model_->rowAt(selected_crash_);
        if (!crash) return;
        diag::log_tagged_fmt("fuzz_view",
            "ai_analyze_clicked crash_idx=%d type=%s addr=0x%llX",
            selected_crash_,
            fuzzer_engine::crash_type_name(crash->type),
            static_cast<unsigned long long>(crash->instruction_address));
        fuzzer_engine::ai_analyze_crash(selected_crash_, crash->crash_hash);
    });
    connect(minimize_button_, &QPushButton::clicked, this, [this] {
        if (fuzzer_engine::g_state.minimizing.load()) return;
        const auto* crash = model_->rowAt(selected_crash_);
        if (!crash) return;
        diag::log_tagged_fmt("fuzz_view",
            "minimize_clicked crash_idx=%d input_size=%zu",
            selected_crash_, crash->input.size());
        fuzzer_engine::minimize_crash(selected_crash_, crash->crash_hash);
    });

    refreshPresentation();
}

void QtFuzzerView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    timer_->start();
}

void QtFuzzerView::hideEvent(QHideEvent* event) {
    timer_->stop();
    QWidget::hideEvent(event);
}

void QtFuzzerView::startFuzzing() {
    auto& fz = fuzzer_engine::g_state;
    auto& cfg = fz.config;
    const auto write_input = [](char* target, std::size_t capacity,
                                const std::string& text) {
        std::strncpy(target, text.c_str(), capacity - 1);
        target[capacity - 1] = '\0';
    };
    write_input(fz.addr_input, sizeof(fz.addr_input),
        addr_edit_->text().toStdString());
    write_input(fz.end_addr_input, sizeof(fz.end_addr_input),
        end_edit_->text().toStdString());
    write_input(fz.input_addr, sizeof(fz.input_addr),
        input_edit_->text().toStdString());
    write_input(fz.input_size_str, sizeof(fz.input_size_str),
        input_size_edit_->text().toStdString());
    write_input(fz.max_iter_str, sizeof(fz.max_iter_str),
        max_iter_edit_->text().toStdString());
    if (fz.addr_input[0])
        cfg.target_address = std::strtoull(fz.addr_input, nullptr, 16);
    if (fz.end_addr_input[0])
        cfg.end_address = std::strtoull(fz.end_addr_input, nullptr, 16);
    if (fz.input_addr[0])
        cfg.input_address = std::strtoull(fz.input_addr, nullptr, 16);
    if (fz.input_size_str[0])
        cfg.input_size = static_cast<int>(std::strtol(fz.input_size_str, nullptr, 0));
    if (fz.max_iter_str[0])
        cfg.max_iterations =
            static_cast<std::uint32_t>(std::strtoul(fz.max_iter_str, nullptr, 0));
    if (cfg.input_size <= 0) cfg.input_size = 256;
    if (cfg.max_iterations == 0) cfg.max_iterations = 10000;

    std::uint32_t pid = 0;
    std::uint32_t tid = 0;
    pid = driver_bridge::attached_pid();
    if (pid != 0) {
        auto threads = driver_bridge::enumerate_threads();
        if (!threads.empty()) tid = threads[0].tid;
    }
    cfg.pid = pid;
    cfg.tid = tid;

    const bool ok_target = cfg.target_address != 0;
    const bool ok_attached = pid != 0;
    auto& bridge = QtAnalysisBridge::instance();
    if (!ok_target) {
        diag::log_tagged_fmt("fuzzer",
            "start_reject reason=no_target_address buf='%s'", fz.addr_input);
        bridge.toastWarning(QStringLiteral("Enter a target address (hex)"), 3.0);
    } else if (!ok_attached) {
        diag::log_tagged("fuzzer", "start_reject reason=no_process_attached");
        bridge.toastWarning(
            QStringLiteral("Attach to a process before starting fuzzer"), 3.0);
    } else {
        diag::log_tagged_fmt("fuzzer",
            "fuzz_start pid=%u tid=%u target=0x%llX end=0x%llX input=0x%llX input_size=%d iters=%u",
            pid, tid,
            static_cast<unsigned long long>(cfg.target_address),
            static_cast<unsigned long long>(cfg.end_address),
            static_cast<unsigned long long>(cfg.input_address),
            cfg.input_size, cfg.max_iterations);
        fuzzer_engine::start_fuzzing();
    }
}

void QtFuzzerView::resetState() {
    diag::log_tagged("fuzzer", "[analysis_audit] view_reset_request");
    if (fuzzer_engine::reset_state()) {
        selected_crash_ = -1;
        selected_crash_hash_ = 0;
        last_unique_crashes_ = 0;
        for (int i = 0; i < 8; ++i) stat_values_[i] = 0.0;
        updateDetailPanel();
        QtAnalysisBridge::instance().toastInfo(
            QStringLiteral("Fuzzer state cleared"), 2.0);
    } else {
        QtAnalysisBridge::instance().toastWarning(
            QStringLiteral("Cannot reset while fuzzer is busy"), 2.5);
    }
}

void QtFuzzerView::pollEngine() {
    auto& fz = fuzzer_engine::g_state;
    const bool running = fz.running.load();
    const auto snapshot = fuzzer_engine::capture_render_snapshot();
    static const fuzzer_engine::render_snapshot_t empty_snapshot;
    const auto& published = snapshot ? *snapshot : empty_snapshot;
    const auto& stats = published.stats;
    const auto& crashes = published.unique_crashes;

    if (crash_snapshot_generation_ != published.generation) {
        crash_snapshot_generation_ = published.generation;
        if (selected_crash_hash_ != 0) {
            const auto selected = std::find_if(crashes.begin(), crashes.end(),
                [&](const fuzzer_engine::crash_info_t& crash) {
                    return crash.crash_hash == selected_crash_hash_;
                });
            if (selected == crashes.end()) {
                selected_crash_ = -1;
                selected_crash_hash_ = 0;
            } else {
                selected_crash_ =
                    static_cast<int>(std::distance(crashes.begin(), selected));
            }
            updateDetailPanel();
        }
        model_->setSnapshot(snapshot);
    }

    updateStats(stats);
    canvas_->setSnapshot(snapshot, running);
    if (running && !theme::AidaMotion::reducedMotion()) {
        const qreal period =
            2.0 * static_cast<qreal>(theme::tokens().motion.hero);
        scan_phase_ += static_cast<qreal>(timer_->interval()) / period;
        if (scan_phase_ > 1.0) scan_phase_ -= 1.0;
        canvas_->setScanPhase(scan_phase_);
    }

    truncated_badge_->setVisible(published.crash_catalog_truncated);
    status_pill_->setText(running ? QStringLiteral("Running") : QStringLiteral("Idle"));
    const char* pill_variant = running ? "accent" : "neutral";
    if (status_pill_->property("aidaVariant").toString() !=
            QLatin1String(pill_variant)) {
        status_pill_->setProperty("aidaVariant", QString::fromLatin1(pill_variant));
        theme::stylesheet::repolish(status_pill_);
    }

    start_button_->setVisible(!running);
    stop_button_->setVisible(running);
    const bool persistence_busy =
        fz.exporting_crashes.load() || fz.importing_crashes.load();
    export_button_->setEnabled(!persistence_busy);
    import_button_->setEnabled(!persistence_busy);
    reset_button_->setEnabled(!running && !fz.minimizing.load() &&
        !fz.analyzing_crash.load() && !persistence_busy);
    refreshPresentation();
}

void QtFuzzerView::updateStats(const fuzzer_engine::fuzz_stats_t& stats) {
    const qreal targets[8] = {
        static_cast<qreal>(stats.total_executions),
        static_cast<qreal>(stats.executions_per_second),
        static_cast<qreal>(stats.total_crashes),
        static_cast<qreal>(stats.total_unique_crashes),
        static_cast<qreal>(stats.edge_coverage),
        static_cast<qreal>(stats.new_coverage_finds),
        static_cast<qreal>(stats.corpus_size),
        stats.elapsed_seconds
    };
    for (int i = 0; i < 8; ++i)
        stat_values_[i] = targets[i];
    stat_labels_[0]->setText(QString::number(stats.total_executions));
    stat_labels_[1]->setText(QStringLiteral("%1/s").arg(stats.executions_per_second));
    stat_labels_[2]->setText(QString::number(stats.total_crashes));
    stat_labels_[3]->setText(QString::number(stats.total_unique_crashes));
    stat_labels_[4]->setText(QString::number(stats.edge_coverage));
    stat_labels_[5]->setText(QString::number(stats.new_coverage_finds));
    stat_labels_[6]->setText(QString::number(stats.corpus_size));
    stat_labels_[7]->setText(QStringLiteral("%1s").arg(stats.elapsed_seconds, 0, 'f', 1));
}

void QtFuzzerView::updateDetailPanel() {
    const auto* crash = selected_crash_ >= 0 ? model_->rowAt(selected_crash_) : nullptr;
    detail_panel_->setVisible(crash != nullptr);
    if (!crash) return;
    detail_title_->setText(QStringLiteral("Crash #%1   %2   [%3]")
        .arg(selected_crash_ + 1)
        .arg(QString::fromLatin1(fuzzer_engine::crash_type_name(crash->type)))
        .arg(QString::fromLatin1(fuzzer_engine::exploit_score_name(crash->score))));
    detail_instruction_->setText(crash->crashing_instruction.empty()
        ? QStringLiteral("-")
        : QString::fromStdString(crash->crashing_instruction));
    detail_fault_->setText(QStringLiteral("Fault 0x%1   RIP 0x%2")
        .arg(crash->fault_address, 0, 16).arg(crash->rip, 0, 16));
    detail_registers_->setText(QStringLiteral(
        "RAX=%1 RBX=%2 RCX=%3 RDX=%4\nRSP=%5 RBP=%6 RSI=%7 RDI=%8\n"
        "RIP=%9 R8 =%10 R9 =%11 R10=%12\nR11=%13 R12=%14 R13=%15 R14=%16 R15=%17")
        .arg(crash->rax, 16, 16, QLatin1Char('0'))
        .arg(crash->rbx, 16, 16, QLatin1Char('0'))
        .arg(crash->rcx, 16, 16, QLatin1Char('0'))
        .arg(crash->rdx, 16, 16, QLatin1Char('0'))
        .arg(crash->rsp, 16, 16, QLatin1Char('0'))
        .arg(crash->rbp, 16, 16, QLatin1Char('0'))
        .arg(crash->rsi, 16, 16, QLatin1Char('0'))
        .arg(crash->rdi, 16, 16, QLatin1Char('0'))
        .arg(crash->rip, 16, 16, QLatin1Char('0'))
        .arg(crash->r8, 16, 16, QLatin1Char('0'))
        .arg(crash->r9, 16, 16, QLatin1Char('0'))
        .arg(crash->r10, 16, 16, QLatin1Char('0'))
        .arg(crash->r11, 16, 16, QLatin1Char('0'))
        .arg(crash->r12, 16, 16, QLatin1Char('0'))
        .arg(crash->r13, 16, 16, QLatin1Char('0'))
        .arg(crash->r14, 16, 16, QLatin1Char('0'))
        .arg(crash->r15, 16, 16, QLatin1Char('0')));
    const char* mutation_strategy = "unknown";
    switch (crash->mutation.strategy) {
    case fuzzer_engine::mutation_strategy_t::bit_flip: mutation_strategy = "bit_flip"; break;
    case fuzzer_engine::mutation_strategy_t::byte_flip: mutation_strategy = "byte_flip"; break;
    case fuzzer_engine::mutation_strategy_t::arithmetic: mutation_strategy = "arithmetic"; break;
    case fuzzer_engine::mutation_strategy_t::interesting_values:
        mutation_strategy = "interesting"; break;
    case fuzzer_engine::mutation_strategy_t::havoc: mutation_strategy = "havoc"; break;
    case fuzzer_engine::mutation_strategy_t::splice: mutation_strategy = "splice"; break;
    default: break;
    }
    detail_mutation_->setText(QStringLiteral("Mutation %1   offset=%2   size=%3")
        .arg(QString::fromLatin1(mutation_strategy))
        .arg(crash->mutation.offset).arg(crash->mutation.size));
    if (!crash->input.empty()) {
        QString hex = QStringLiteral("Input ");
        const std::size_t shown = (std::min)(crash->input.size(), std::size_t{32});
        for (std::size_t i = 0; i < shown; ++i)
            hex += QStringLiteral("%1 ").arg(crash->input[i], 2, 16, QLatin1Char('0'));
        if (crash->input.size() > 32) hex += QStringLiteral("...");
        detail_input_->setText(hex);
    } else {
        detail_input_->setText(QString());
    }
    if (crash->is_minimized) {
        QString text = QStringLiteral("Minimized ");
        const std::size_t shown =
            (std::min)(crash->minimized_input.size(), std::size_t{32});
        for (std::size_t i = 0; i < shown; ++i)
            text += QStringLiteral("%1 ").arg(crash->minimized_input[i], 2, 16,
                QLatin1Char('0'));
        if (crash->minimized_input.size() > 32) text += QStringLiteral("...");
        text += QStringLiteral(" (%1 bytes)").arg(crash->minimized_input.size());
        detail_minimized_->setText(text);
    } else {
        detail_minimized_->setText(QString());
    }
    detail_ai_->setText(crash->ai_analysis.empty()
        ? QString() : QString::fromStdString(crash->ai_analysis));
    analyze_button_->setEnabled(!fuzzer_engine::g_state.analyzing_crash.load());
    minimize_button_->setEnabled(!fuzzer_engine::g_state.minimizing.load() &&
        !crash->input.empty());
}

void QtFuzzerView::refreshPresentation() {
    const bool empty = !fuzzer_engine::g_state.active &&
        !fuzzer_engine::g_state.running.load() && model_->rowCount() == 0;
    state_view_->setVisible(empty);
    table_->setVisible(!empty);
}

void QtFuzzerView::showCrashMenu(const QPoint& global_pos, int view_row) {
    const auto* crash = model_->rowAt(view_row);
    if (!crash) return;
    selected_crash_ = view_row;
    selected_crash_hash_ = crash->crash_hash;
    updateDetailPanel();
    auto& fz = fuzzer_engine::g_state;
    const std::uint64_t crash_hash = crash->crash_hash;
    const std::uint64_t generation = model_->snapshot_generation();
    aida::ui::application_ui::retained_entity_context_t retained;
    retained.owner_id = "analysis.fuzzer.crash";
    retained.entity_id = std::to_string(crash_hash);
    retained.entity_generation = generation;
    retained.active_view = aida::ui::stable_view_id_t("view.analysis.fuzzer");
    retained.validate_identity = [crash_hash, generation] {
        const auto current = fuzzer_engine::capture_render_snapshot();
        if (!current || current->generation != generation)
            return aida::ui::capability_state_t::unavailable(
                "The crash publication changed; select the crash again");
        const auto found = std::find_if(current->unique_crashes.begin(),
            current->unique_crashes.end(), [crash_hash](const auto& item) {
                return item.crash_hash == crash_hash;
            });
        return found != current->unique_crashes.end()
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The retained crash no longer exists in this publication");
    };
    const auto add_action = [&retained](const char* id, bool enabled,
        const char* reason, auto invoke) {
        aida::ui::application_ui::retained_entity_action_t action;
        action.action_id = id;
        action.capability = enabled
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(reason);
        action.invoke = std::move(invoke);
        retained.actions.push_back(std::move(action));
    };
    const int index = view_row;
    add_action("analysis.fuzzer.crash.ai_analyze",
        !fz.analyzing_crash.load(std::memory_order_acquire),
        "Another crash is already being analyzed", [index, crash_hash] {
            fuzzer_engine::ai_analyze_crash(index, crash_hash);
            return aida::ui::action_handler_result_t::completed();
        });
    add_action("analysis.fuzzer.crash.minimize",
        !fz.minimizing.load(std::memory_order_acquire) && !crash->input.empty(),
        crash->input.empty() ? "The retained crash has no reproducer input"
            : "Another crash minimization is already running", [index, crash_hash] {
            fuzzer_engine::minimize_crash(index, crash_hash);
            return aida::ui::action_handler_result_t::completed();
        });
    const std::uint64_t instruction_address = crash->instruction_address;
    add_action("analysis.fuzzer.crash.copy_instruction_address", true, "",
        [instruction_address] {
            char text[32]{};
            std::snprintf(text, sizeof(text), "0x%llX",
                static_cast<unsigned long long>(instruction_address));
            clipboard::set_text(QString::fromLatin1(text));
            return aida::ui::action_handler_result_t::completed();
        });
    add_action("analysis.fuzzer.crash.copy_hash", true, "", [crash_hash] {
        char text[32]{};
        std::snprintf(text, sizeof(text), "%016llX",
            static_cast<unsigned long long>(crash_hash));
        clipboard::set_text(QString::fromLatin1(text));
        return aida::ui::action_handler_result_t::completed();
    });
    const std::string description = crash->description;
    add_action("analysis.fuzzer.crash.copy_description", true, "", [description] {
        clipboard::set_text(QString::fromStdString(description));
        return aida::ui::action_handler_result_t::completed();
    });
    const std::size_t retained_input_size =
        (std::min)(crash->input.size(), std::size_t{64u * 1024u});
    const std::vector<std::uint8_t> input(crash->input.begin(),
        crash->input.begin() + static_cast<std::ptrdiff_t>(retained_input_size));
    add_action("analysis.fuzzer.crash.copy_input_hex", !input.empty(),
        "The retained crash has no reproducer input", [input] {
            QString hex;
            hex.reserve(static_cast<int>(input.size() * 2u));
            for (const auto byte : input)
                hex += QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0'));
            clipboard::set_text(hex);
            return aida::ui::action_handler_result_t::completed();
        });
    QtAnalysisBridge::instance().showRetainedMenu(retained,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}
