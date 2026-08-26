#include "qt/debugger/strings_pane.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QTableView>
#include <QTimer>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_view.hpp"
#include "core/runtime/standalone_driver.hpp"

#include "qt/debugger/debugger_models.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_line_edit.hpp"

namespace aida::qt::debugger {

StringsPane::StringsPane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.strings"));
    setOwnerViewId("view.debug.strings");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral(
            "Attach to a process to enumerate its printable strings."));
    setEmptyContentText(QStringLiteral("No strings captured"),
        QStringLiteral(
            "Run Scan Strings to harvest printable strings from the target's "
            "memory."));
    setLoadingText(QStringLiteral("Scanning target memory"),
        QStringLiteral(
            "Printable strings stream in as pages are scanned; cancel anytime "
            "from the toolbar."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    bar_layout->setSpacing(tokens.toolbar.group_gap);
    filter_edit_ = new widgets::AidaLineEdit(QStringLiteral("Filter strings..."),
        bar);
    filter_edit_->setObjectName(QStringLiteral("aida.view.debug.strings.filter"));
    filter_edit_->setMaxLength(127);
    bar_layout->addWidget(filter_edit_, 1);
    min_length_spin_ = new QSpinBox(bar);
    min_length_spin_->setObjectName(
        QStringLiteral("aida.view.debug.strings.min_length"));
    min_length_spin_->setRange(2, 64);
    min_length_spin_->setValue(4);
    min_length_spin_->setToolTip(QStringLiteral("Minimum string length"));
    bar_layout->addWidget(min_length_spin_);
    scan_button_ = new widgets::AidaButton(QStringLiteral("Scan Strings"), bar);
    scan_button_->setObjectName(QStringLiteral("aida.view.debug.strings.scan"));
    scan_button_->setKind(widgets::AidaButton::Kind::Primary);
    scan_button_->setToolTip(QStringLiteral(
        "Scan the attached target's memory for printable strings"));
    connect(scan_button_, &widgets::AidaButton::clicked, this,
        &StringsPane::toggleScan);
    bar_layout->addWidget(scan_button_);
    progress_label_ = new QLabel(bar);
    progress_label_->setObjectName(
        QStringLiteral("aida.view.debug.strings.progress"));
    progress_label_->setVisible(false);
    bar_layout->addWidget(progress_label_);
    bar_layout->addStretch(1);
    setToolBar(bar);

    model_ = new StringsModel(this);
    view_ = new QTableView(this);
    view_->setObjectName(QStringLiteral("aida.view.debug.strings.table"));
    wireTable(view_, model_);
    setContent(view_);

    connect(filter_edit_, &QLineEdit::textChanged, this, [this] {
        model_->setFilter(filter_edit_->text());
    });
    connect(view_, &QAbstractItemView::activated, this,
        [this](const QModelIndex&) { jumpToSelectedHex(); });

    timer_ = new QTimer(this);
    timer_->setInterval(250);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &StringsPane::poll);
}

void StringsPane::onShown() {
    timer_->start();
    poll();
}

void StringsPane::onHidden() {
    timer_->stop();
}

bool StringsPane::hasContentRows() const {
    return model_ && (model_->rowCount() > 0 ||
        (filter_edit_ && !filter_edit_->text().isEmpty()));
}

bool StringsPane::isContentLoading() const {
    return debugger_engine::g_state.strings_scanning.load(
        std::memory_order_acquire);
}

void StringsPane::poll() {
    auto& st = debugger_engine::g_state;
    const bool scanning = st.strings_scanning.load(std::memory_order_acquire);
    const bool cancel_pending =
        st.strings_cancel.load(std::memory_order_acquire);
    scan_button_->setText(scanning
        ? (cancel_pending ? QStringLiteral("Cancelling...")
                          : QStringLiteral("Cancel Scan"))
        : QStringLiteral("Scan Strings"));
    scan_button_->setKind(scanning ? widgets::AidaButton::Kind::Destructive
                                   : widgets::AidaButton::Kind::Primary);
    scan_button_->setEnabled(!scanning || !cancel_pending);
    scan_button_->setLoading(scanning && !cancel_pending);

    if (scanning) {
        const quint64 pages =
            st.strings_pages_scanned.load(std::memory_order_acquire);
        const quint64 found =
            st.strings_found_so_far.load(std::memory_order_acquire);
        progress_label_->setText(QStringLiteral(
            "Scanning %1 pages... %2 strings found so far")
            .arg(static_cast<qulonglong>(pages))
            .arg(static_cast<qulonglong>(found)));
        progress_label_->setVisible(true);
    } else {
        progress_label_->setVisible(false);
    }

    const auto snapshot = snapshots_.poll(st.strings_mutex, st.strings,
        st.strings_generation, "strings");
    if (snapshot.refreshed)
        model_->applyStrings(snapshot.items, snapshot.generation);
}

void StringsPane::toggleScan() {
    auto& st = debugger_engine::g_state;
    const bool scanning = st.strings_scanning.load(std::memory_order_acquire);
    if (scanning) {
        diag::log_tagged_critical_fmt("strings",
            "strings_cancel_request pages_so_far=%llu found_so_far=%llu",
            static_cast<unsigned long long>(st.strings_pages_scanned.load()),
            static_cast<unsigned long long>(st.strings_found_so_far.load()));
        debugger_engine::request_strings_cancel();
        return;
    }
    const std::size_t min_length = static_cast<std::size_t>(
        (std::max)(2, min_length_spin_->value()));
    diag::log_tagged_critical_fmt("strings",
        "strings_scan_request min_length=%zu attached_pid=%u", min_length,
        static_cast<unsigned>(driver_bridge::attached_pid()));
    debugger_engine::find_strings_async(min_length);
}

void StringsPane::jumpToSelectedHex() {
    const auto index = view_->currentIndex();
    if (!index.isValid())
        return;
    const auto context = model_->contextForRow(index.row());
    if (context.address == 0)
        return;
    diag::log_tagged_fmt("dbg_view",
        "strings double-click: jump to hex addr=0x%llX value='%.32s'",
        static_cast<unsigned long long>(context.address),
        context.primary_text.c_str());
    debugger_view::jump_to_hex(context.address, 256);
}

}
