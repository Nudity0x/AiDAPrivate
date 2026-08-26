#include "qt/debugger/seh_pane.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QTableView>
#include <QVBoxLayout>

#include "core/debugger/seh_view.hpp"
#include "core/runtime/standalone_driver.hpp"

#include "qt/debugger/debugger_models.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_pill.hpp"

namespace aida::qt::debugger {

SehPane::SehPane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.seh"));
    setOwnerViewId("view.debug.seh");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral(
            "Attach to a process to walk the active thread's SEH chain."));
    setEmptyContentText(QStringLiteral("No SEH entries"),
        QStringLiteral(
            "The active thread's SEH chain is empty or could not be proven; "
            "Refresh re-walks the chain."));
    setLoadingText(QStringLiteral("Walking the SEH chain"),
        QStringLiteral(
            "The engine is resolving the active thread's TEB and walking the "
            "exception list."));
    setErrorText(QStringLiteral("SEH refresh failed"),
        QStringLiteral(
            "The SEH chain walk failed; press Refresh to retry."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    bar_layout->setSpacing(tokens.toolbar.group_gap);
    refresh_button_ = new widgets::AidaButton(QStringLiteral("Refresh"), bar);
    refresh_button_->setObjectName(QStringLiteral("aida.view.debug.seh.refresh"));
    refresh_button_->setKind(widgets::AidaButton::Kind::Secondary);
    refresh_button_->setToolTip(QStringLiteral(
        "Re-walk the active thread's SEH chain"));
    connect(refresh_button_, &widgets::AidaButton::clicked, this, [] {
        seh_view::refresh();
    });
    bar_layout->addWidget(refresh_button_);
    scope_pill_ = new widgets::AidaPill(QStringLiteral("SEH per active thread"),
        widgets::AidaSemantic::Info, bar);
    scope_pill_->setObjectName(QStringLiteral("aida.view.debug.seh.scope"));
    scope_pill_->setToolTip(QStringLiteral(
        "Refresh reads the chain for the active debugger thread."));
    bar_layout->addWidget(scope_pill_);
    auto* event_pill = new widgets::AidaPill(QStringLiteral(
        "Debug-event break unavailable"), widgets::AidaSemantic::Warning, bar);
    event_pill->setObjectName(QStringLiteral("aida.view.debug.seh.event_pill"));
    event_pill->setToolTip(QStringLiteral(
        "driver_bridge does not expose a debug-event subscription in this "
        "build."));
    bar_layout->addWidget(event_pill);
    bar_layout->addStretch(1);
    setToolBar(bar);

    model_ = new SehModel(this);
    view_ = new QTableView(this);
    view_->setObjectName(QStringLiteral("aida.view.debug.seh.table"));
    wireTable(view_, model_);

    diagnostics_label_ = new QLabel(this);
    diagnostics_label_->setObjectName(
        QStringLiteral("aida.view.debug.seh.diagnostics"));
    diagnostics_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    diagnostics_label_->setWordWrap(true);
    diagnostics_label_->setText(QStringLiteral(
        "No debug-event channel is exposed. Use a hardware execute breakpoint "
        "on a handler address."));

    auto* body = new QWidget(this);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(tokens.spacing.xs);
    body_layout->addWidget(view_, 1);
    body_layout->addWidget(diagnostics_label_, 0);
    setContent(body);
}

void SehPane::onShown() {
    if (driver_bridge::attached_pid() != 0)
        seh_view::refresh();
    pollModel();
}

void SehPane::onSessionTick() {
    pollModel();
    updateOverlayState();
}

bool SehPane::hasContentRows() const {
    return model_ && model_->rowCount() > 0;
}

bool SehPane::isContentLoading() const {
    return seh_view::g_ui.refreshing.load(std::memory_order_acquire);
}

bool SehPane::contentError(QString* detail) const {
    if (last_error_.isEmpty())
        return false;
    if (detail)
        *detail = last_error_;
    return true;
}

void SehPane::pollModel() {
    std::shared_ptr<const std::vector<seh_view::seh_entry_t>> entries;
    seh_view::seh_diagnostics_t diagnostics{};
    std::uint64_t generation = 0;
    std::string error;
    if (!seh_view::snapshot_state(entries, diagnostics, generation, error))
        return;

    const QString error_text = QString::fromStdString(error);
    if (error_text != last_error_) {
        last_error_ = error_text;
        updateOverlayState();
    }

    refresh_button_->setEnabled(!seh_view::g_ui.refreshing.load(
        std::memory_order_acquire));
    refresh_button_->setLoading(seh_view::g_ui.refreshing.load(
        std::memory_order_acquire));

    model_->applySeh(entries, generation);

    quint64 signature = 0;
    signature ^= diagnostics.chain_entries;
    signature ^= static_cast<quint64>(diagnostics.teb_va);
    signature ^= static_cast<quint64>(diagnostics.raw_exception_list) << 1;
    if (signature != last_diag_signature_ || !error.empty()) {
        last_diag_signature_ = signature;
        QStringList lines;
        if (!error.empty()) {
            lines << QString::fromStdString(error);
        }
        if (generation != 0) {
            lines << QStringLiteral("PID %1 TID %2 | chain depth %3")
                .arg(diagnostics.target_pid)
                .arg(diagnostics.active_tid)
                .arg(diagnostics.chain_entries);
            lines << QStringLiteral(
                "TEB 0x%1 | exception list 0x%2 | TEB query %3")
                .arg(static_cast<qulonglong>(diagnostics.teb_va), 0, 16)
                .arg(static_cast<qulonglong>(diagnostics.raw_exception_list),
                    0, 16)
                .arg(diagnostics.teb_query_ok
                    ? QStringLiteral("ok") : QStringLiteral("failed"));
            if (!diagnostics.chain_stop_reason.empty())
                lines << QStringLiteral("Chain stop: %1").arg(
                    QString::fromStdString(diagnostics.chain_stop_reason));
            if (!diagnostics.empty_reason.empty() &&
                diagnostics.chain_entries == 0)
                lines << QStringLiteral("Empty reason: %1").arg(
                    QString::fromStdString(diagnostics.empty_reason));
            if (diagnostics.stack_scan_attempted)
                lines << QStringLiteral(
                    "Stack scan: %1 candidates, read %2 bytes")
                    .arg(diagnostics.stack_scan_candidates)
                    .arg(static_cast<qulonglong>(diagnostics.stack_scan_bytes));
        }
        if (!lines.isEmpty())
            diagnostics_label_->setText(lines.join(QStringLiteral("\n")));
    }
}

}
