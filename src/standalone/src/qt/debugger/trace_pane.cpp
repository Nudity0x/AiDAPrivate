#include "qt/debugger/trace_pane.hpp"

#include <QCheckBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableView>
#include <QTimer>

#include <algorithm>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_view.hpp"
#include "core/infra/executor.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/ui/toast_notification.hpp"
#include "core/ui/ui_thread_dispatcher.hpp"

#include "qt/debugger/debugger_models.hpp"
#include "qt/debugger/debugger_mutation_queue.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_pill.hpp"

namespace aida::qt::debugger {

TracePane::TracePane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.trace"));
    setOwnerViewId("view.debug.trace");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral("Attach to a process to record an execution trace."));
    setEmptyContentText(QStringLiteral("No trace records"),
        QStringLiteral(
            "Start Trace records executed instructions; they stream in here "
            "while the target runs."));
    setLoadingText(QStringLiteral("Recording trace"),
        QStringLiteral(
            "Trace is recording; executed instructions stream in while the "
            "target runs."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    bar_layout->setSpacing(tokens.toolbar.group_gap);
    filter_edit_ = new widgets::AidaLineEdit(
        QStringLiteral("Filter trace (mnemonic or hex address)..."), bar);
    filter_edit_->setObjectName(QStringLiteral("aida.view.debug.trace.filter"));
    filter_edit_->setMaxLength(95);
    bar_layout->addWidget(filter_edit_, 1);
    freeze_check_ = new QCheckBox(QStringLiteral("Freeze view"), bar);
    freeze_check_->setObjectName(QStringLiteral("aida.view.debug.trace.freeze"));
    freeze_check_->setToolTip(QStringLiteral(
        "Stop applying new trace snapshots to the table (recording continues)"));
    bar_layout->addWidget(freeze_check_);
    trace_button_ = new widgets::AidaButton(QStringLiteral("Start Trace"), bar);
    trace_button_->setObjectName(QStringLiteral("aida.view.debug.trace.toggle"));
    trace_button_->setKind(widgets::AidaButton::Kind::Primary);
    trace_button_->setToolTip(QStringLiteral(
        "Record executed instructions into the trace log"));
    connect(trace_button_, &widgets::AidaButton::clicked, this,
        &TracePane::toggleTrace);
    bar_layout->addWidget(trace_button_);
    clear_button_ = new widgets::AidaButton(QStringLiteral("Clear"), bar);
    clear_button_->setObjectName(QStringLiteral("aida.view.debug.trace.clear"));
    clear_button_->setKind(widgets::AidaButton::Kind::Secondary);
    clear_button_->setToolTip(QStringLiteral(
        "Discard every recorded trace record"));
    connect(clear_button_, &widgets::AidaButton::clicked, this,
        &TracePane::clearTrace);
    bar_layout->addWidget(clear_button_);
    export_button_ = new widgets::AidaButton(QStringLiteral("Export"), bar);
    export_button_->setObjectName(QStringLiteral("aida.view.debug.trace.export"));
    export_button_->setKind(widgets::AidaButton::Kind::Secondary);
    export_button_->setToolTip(QStringLiteral(
        "Export the trace log to a CSV file in the background"));
    connect(export_button_, &widgets::AidaButton::clicked, this,
        &TracePane::exportTrace);
    bar_layout->addWidget(export_button_);
    dropped_label_ = new QLabel(bar);
    dropped_label_->setObjectName(QStringLiteral("aida.view.debug.trace.dropped"));
    dropped_label_->setProperty("aidaVariant", QStringLiteral("warning"));
    dropped_label_->setVisible(false);
    bar_layout->addWidget(dropped_label_);
    bar_layout->addStretch(1);
    rec_pill_ = new widgets::AidaPill(QStringLiteral("STOPPED"),
        widgets::AidaSemantic::Neutral, bar);
    rec_pill_->setObjectName(QStringLiteral("aida.view.debug.trace.rec"));
    bar_layout->addWidget(rec_pill_);
    setToolBar(bar);

    model_ = new TraceModel(this);
    view_ = new QTableView(this);
    view_->setObjectName(QStringLiteral("aida.view.debug.trace.table"));
    wireTable(view_, model_);
    setContent(view_);

    connect(filter_edit_, &QLineEdit::textChanged, this, [this] {
        model_->setFilter(filter_edit_->text());
    });

    timer_ = new QTimer(this);
    timer_->setInterval(100);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &TracePane::poll);
}

void TracePane::onShown() {
    timer_->start();
    poll();
}

void TracePane::onHidden() {
    timer_->stop();
}

bool TracePane::hasContentRows() const {
    return model_ && (model_->rowCount() > 0 ||
        (filter_edit_ && !filter_edit_->text().isEmpty()));
}

bool TracePane::isContentLoading() const {
    return debugger_engine::g_state.tracing.load(std::memory_order_acquire);
}

void TracePane::poll() {
    auto& st = debugger_engine::g_state;
    const bool tracing = st.tracing.load(std::memory_order_acquire);
    trace_button_->setText(tracing ? QStringLiteral("Stop Trace")
                                   : QStringLiteral("Start Trace"));
    trace_button_->setToolTip(tracing
        ? QStringLiteral("Stop the running trace recording")
        : QStringLiteral("Record executed instructions into the trace log"));
    trace_button_->setKind(tracing ? widgets::AidaButton::Kind::Destructive
                                   : widgets::AidaButton::Kind::Primary);
    trace_button_->setEnabled(tracing ||
        driver_bridge::attached_pid() != 0);
    rec_pill_->setText(tracing ? QStringLiteral("REC")
                               : QStringLiteral("STOPPED"));
    rec_pill_->setKind(tracing ? widgets::AidaSemantic::Error
                               : widgets::AidaSemantic::Neutral);
    rec_pill_->setLeadingDotVisible(tracing);
    const std::uint64_t dropped =
        st.trace_dropped.load(std::memory_order_acquire);
    dropped_label_->setVisible(dropped != 0);
    if (dropped != 0)
        dropped_label_->setText(QStringLiteral("Backpressure: %1 dropped")
            .arg(static_cast<qulonglong>(dropped)));

    if (freeze_check_->isChecked())
        return;
    const auto snapshot = snapshots_.poll(st.trace_mutex, st.trace_log,
        st.trace_generation, "trace");
    if (snapshot.refreshed)
        model_->applyTrace(snapshot.items, snapshot.generation);
}

void TracePane::toggleTrace() {
    const bool tracing = debugger_engine::g_state.tracing.load(
        std::memory_order_acquire);
    const auto context = debugger_interaction::capture(
        debugger_interaction::kind_t::trace_record);
    DebuggerMutationQueue::instance().queueMutation(
        tracing ? "Stop trace" : "Start trace",
        tracing ? "debugger.trace_stop" : "debugger.trace_start", context,
        [tracing]() {
            debugger_view::mutation_result_t result;
            if (tracing) {
                debugger_engine::stop_trace();
                result.ok = result.verified =
                    !debugger_engine::g_state.tracing.load(
                        std::memory_order_acquire);
            } else {
                result.ok = debugger_engine::start_trace();
                result.verified = result.ok &&
                    debugger_engine::g_state.tracing.load(
                        std::memory_order_acquire);
            }
            return result;
        }, false);
}

void TracePane::clearTrace() {
    auto& st = debugger_engine::g_state;
    std::size_t before = 0;
    std::unique_lock<std::mutex> trace_lock(st.trace_mutex, std::try_to_lock);
    if (trace_lock.owns_lock()) {
        before = st.trace_log.size();
        st.trace_log.clear();
        st.trace_generation.fetch_add(1, std::memory_order_release);
        st.trace_dropped.store(0, std::memory_order_release);
        trace_lock.unlock();
        diag::log_tagged_fmt("trace", "trace_clear removed=%zu", before);
        diag::log_tagged("dbg_audit", "[dbg_audit] trace clear ok=1");
    } else {
        toast_notification::push("Trace is updating; retry Clear in a moment.",
            toast_notification::toast_type_t::warning);
    }
}

void TracePane::exportTrace() {
    bool trace_empty = false;
    {
        auto& st = debugger_engine::g_state;
        if (st.trace_mutex.try_lock()) {
            trace_empty = st.trace_log.empty();
            st.trace_mutex.unlock();
        }
    }
    if (trace_empty) {
        toast_notification::push("Trace is empty.",
            toast_notification::toast_type_t::warning);
        diag::log_tagged("dbg_audit",
            "[dbg_audit] trace export fail reason=empty");
        return;
    }
    const QString destination = QFileDialog::getSaveFileName(this,
        QStringLiteral("Export Trace"), QStringLiteral("trace.csv"),
        QStringLiteral("CSV (*.csv);;Text (*.txt);;All files (*.*)"));
    if (destination.isEmpty())
        return;
    const std::string path = destination.toStdString();
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "debugger.trace";
    submission.label = "trace.export";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.cancel_hook = [cancelled]() {
        cancelled->store(true, std::memory_order_release);
    };
    auto export_result = std::make_shared<debugger_view::mutation_result_t>();
    submission.body = [path, cancelled, export_result]() {
        std::size_t written = 0;
        try {
            std::vector<debugger_engine::trace_record_t> trace_copy;
            {
                std::lock_guard<std::mutex> lock(
                    debugger_engine::g_state.trace_mutex);
                const auto& source = debugger_engine::g_state.trace_log;
                if (source.size() > 50000U)
                    export_result->detail =
                        "Trace exceeds the 50,000-record export bound.";
                else {
                    std::size_t text_bytes = 0;
                    for (const auto& record : source) {
                        text_bytes += record.disasm_text.size();
                        if (text_bytes > 64U * 1024U * 1024U) {
                            export_result->detail =
                                "Trace text exceeds the 64 MiB export bound.";
                            break;
                        }
                    }
                    if (export_result->detail.empty())
                        trace_copy = source;
                }
            }
            if (cancelled->load(std::memory_order_acquire))
                export_result->detail = "Trace export cancelled.";
            if (export_result->detail.empty()) {
                std::string csv;
                csv.reserve((std::min)(
                    static_cast<std::size_t>(64U * 1024U * 1024U),
                    trace_copy.size() * static_cast<std::size_t>(160U) +
                        static_cast<std::size_t>(64U)));
                csv.append("index,address,rip,rax,rcx,rdx,rsp,disasm\n");
                for (const auto& record : trace_copy) {
                    if ((written & 0x3ffU) == 0U &&
                        cancelled->load(std::memory_order_acquire)) {
                        export_result->detail = "Trace export cancelled.";
                        break;
                    }
                    char line[512];
                    std::snprintf(line, sizeof(line),
                        "%d,0x%016llX,0x%016llX,0x%016llX,0x%016llX,0x%016llX,0x%016llX,",
                        record.index,
                        static_cast<unsigned long long>(record.address),
                        static_cast<unsigned long long>(record.regs.rip),
                        static_cast<unsigned long long>(record.regs.rax),
                        static_cast<unsigned long long>(record.regs.rcx),
                        static_cast<unsigned long long>(record.regs.rdx),
                        static_cast<unsigned long long>(record.regs.rsp));
                    csv.append(line);
                    for (const char character : record.disasm_text)
                        csv.push_back(character == ',' || character == '"' ||
                            character == '\n' || character == '\r' ? ' '
                                : character);
                    csv.push_back('\n');
                    if (csv.size() > 128U * 1024U * 1024U) {
                        export_result->detail =
                            "Encoded trace exceeds the 128 MiB export bound.";
                        break;
                    }
                    ++written;
                }
                if (export_result->detail.empty())
                    export_result->ok = export_result->verified =
                        debugger_view::write_file_atomic_exact(path,
                            csv.data(), csv.size(), export_result->detail);
            }
        } catch (const std::exception& exception) {
            export_result->detail =
                std::string("Trace export failed: ") + exception.what();
        } catch (...) {
            export_result->detail =
                "Trace export failed with an unknown error.";
        }
        if (!export_result->verified && export_result->detail.empty())
            export_result->detail = "Trace export failed.";
        diag::log_tagged_critical_fmt("trace",
            "trace_export count=%zu ok=%d path='%s'", written,
            export_result->verified ? 1 : 0, path.c_str());
        const bool posted = aida::ui_thread::post([export_result]() {
            toast_notification::push(
                export_result->verified ? "Trace export completed."
                                        : export_result->detail,
                export_result->verified
                    ? toast_notification::toast_type_t::success
                    : toast_notification::toast_type_t::error);
        }, "debugger", "trace_export_completion", "worker_completion");
        if (!posted)
            throw std::runtime_error(
                "Trace-export completion could not be published to the UI thread");
        if (!export_result->verified)
            throw std::runtime_error(export_result->detail);
    };
    const auto result = debugger_view::submit_owned_debugger_task(
        std::move(submission), "view.debug.trace", "debugger.trace_export",
        "Export debugger trace", true);
    if (!result.submitted)
        toast_notification::push("Trace export could not be queued: " +
            result.reject_reason, toast_notification::toast_type_t::error);
    else
        toast_notification::push("Trace export queued in Background Tasks.",
            toast_notification::toast_type_t::info);
}

}
