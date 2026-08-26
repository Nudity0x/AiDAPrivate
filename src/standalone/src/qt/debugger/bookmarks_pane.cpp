#include "qt/debugger/bookmarks_pane.hpp"

#include <QHBoxLayout>
#include <QTableView>
#include <QTimer>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_view.hpp"
#include "core/ui/toast_notification.hpp"

#include "qt/debugger/debugger_models.hpp"
#include "qt/debugger/debugger_mutation_queue.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_line_edit.hpp"

namespace aida::qt::debugger {

BookmarksPane::BookmarksPane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.bookmarks"));
    setOwnerViewId("view.debug.bookmarks");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral(
            "Attach to a process to bookmark addresses in the debugger "
            "workspace."));
    setEmptyContentText(QStringLiteral("No bookmarks"),
        QStringLiteral(
            "Bookmark an address above to keep it highlighted across the "
            "disassembly and hex views."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    bar_layout->setSpacing(tokens.toolbar.group_gap);
    address_edit_ = new widgets::AidaLineEdit(QStringLiteral("0x... address"),
        bar);
    address_edit_->setObjectName(
        QStringLiteral("aida.view.debug.bookmarks.address"));
    address_edit_->setMaxLength(23);
    address_edit_->setToolTip(QStringLiteral(
        "Hexadecimal address to bookmark (e.g. 0x140001234); Enter adds the "
        "bookmark"));
    connect(address_edit_, &QLineEdit::returnPressed, this,
        &BookmarksPane::addBookmark);
    bar_layout->addWidget(address_edit_, 2);
    label_edit_ = new widgets::AidaLineEdit(QStringLiteral("label (optional)"),
        bar);
    label_edit_->setObjectName(QStringLiteral("aida.view.debug.bookmarks.label"));
    label_edit_->setMaxLength(63);
    label_edit_->setToolTip(QStringLiteral(
        "Optional label shown next to the bookmarked address"));
    connect(label_edit_, &QLineEdit::returnPressed, this,
        &BookmarksPane::addBookmark);
    bar_layout->addWidget(label_edit_, 3);
    add_button_ = new widgets::AidaButton(QStringLiteral("Add Bookmark"), bar);
    add_button_->setObjectName(QStringLiteral("aida.view.debug.bookmarks.add"));
    add_button_->setKind(widgets::AidaButton::Kind::Primary);
    add_button_->setToolTip(QStringLiteral(
        "Bookmark the entered address with an optional label"));
    connect(add_button_, &widgets::AidaButton::clicked, this,
        &BookmarksPane::addBookmark);
    bar_layout->addWidget(add_button_);
    bar_layout->addStretch(1);
    setToolBar(bar);

    model_ = new BookmarksModel(this);
    view_ = new QTableView(this);
    view_->setObjectName(QStringLiteral("aida.view.debug.bookmarks.table"));
    wireTable(view_, model_);
    setContent(view_);

    connect(view_, &QAbstractItemView::activated, this,
        [this](const QModelIndex&) { jumpToSelected(); });

    timer_ = new QTimer(this);
    timer_->setInterval(250);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &BookmarksPane::poll);
}

void BookmarksPane::onShown() {
    timer_->start();
    poll();
}

void BookmarksPane::onHidden() {
    timer_->stop();
}

bool BookmarksPane::hasContentRows() const {
    return model_ && model_->rowCount() > 0;
}

void BookmarksPane::poll() {
    auto& st = debugger_engine::g_state;
    std::unique_lock<std::mutex> lock(st.anno_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    if (st.bookmarks.size() > 65536U || st.labels.size() > 65536U)
        return;
    std::vector<std::uint64_t> bookmarks = st.bookmarks;
    std::map<std::uint64_t, std::string> labels;
    for (const auto& kv : st.labels)
        labels.emplace(kv.first, kv.second.text);
    lock.unlock();
    std::uint64_t signature = 1469598103934665603ULL;
    const auto mix = [&signature](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            signature ^= static_cast<std::uint8_t>(value >> shift);
            signature *= 1099511628211ULL;
        }
    };
    mix(static_cast<std::uint64_t>(bookmarks.size()));
    for (const auto address : bookmarks)
        mix(address);
    mix(static_cast<std::uint64_t>(labels.size()));
    for (const auto& kv : labels) {
        mix(kv.first);
        for (const char c : kv.second)
            mix(static_cast<unsigned char>(c));
    }
    if (signature == last_signature_)
        return;
    last_signature_ = signature;
    const auto selected = capture_selected_row_ids(*model_,
        view_->selectionModel());
    const quint64 focus = view_->currentIndex().isValid()
        ? model_->rowId(view_->currentIndex().row()) : 0;
    model_->applyBookmarks(std::move(bookmarks), std::move(labels));
    restore_selected_row_ids(*model_, view_, selected, focus);
}

void BookmarksPane::addBookmark() {
    const std::uint64_t address =
        debugger_view::parse_hex_address(address_edit_->text().toStdString());
    diag::log_tagged_critical_fmt("bookmarks",
        "bookmark_add_request raw='%s' parsed_addr=0x%llx label='%s'",
        address_edit_->text().toStdString().c_str(),
        static_cast<unsigned long long>(address),
        label_edit_->text().toStdString().c_str());
    if (address == 0) {
        toast_notification::push(
            "Enter a hexadecimal address (e.g. 0x140001234).",
            toast_notification::toast_type_t::warning);
        return;
    }
    const std::string label = label_edit_->text().toStdString();
    const auto context = debugger_interaction::capture(
        debugger_interaction::kind_t::bookmark, address, 0, -1, 0, 0, label);
    const bool queued = DebuggerMutationQueue::instance().queueMutation(
        "Add bookmark", "debugger.bookmark_add", context,
        [address, label]() {
            debugger_view::mutation_result_t result;
            debugger_engine::toggle_bookmark(address);
            if (!label.empty())
                debugger_engine::set_label(address, label);
            result.ok = result.verified = true;
            return result;
        }, false);
    if (queued) {
        address_edit_->clear();
        label_edit_->clear();
    }
}

void BookmarksPane::jumpToSelected() {
    const auto index = view_->currentIndex();
    if (!index.isValid())
        return;
    const auto context = model_->contextForRow(index.row());
    if (context.address != 0)
        debugger_view::jump_to_disasm(context.address);
}

}
