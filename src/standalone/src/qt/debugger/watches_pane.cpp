#include "qt/debugger/watches_pane.hpp"

#include <QHBoxLayout>
#include <QTableView>
#include <QTimer>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_view.hpp"
#include "core/ui/application_ui_runtime.hpp"

#include "qt/debugger/debugger_action_bridge.hpp"
#include "qt/debugger/debugger_models.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_line_edit.hpp"

namespace aida::qt::debugger {

WatchesPane::WatchesPane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.watches"));
    setOwnerViewId("view.debug.watches");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral(
            "Attach to a process to evaluate register/memory watch expressions "
            "live."));
    setEmptyContentText(QStringLiteral("No watches defined"),
        QStringLiteral(
            "Add a register or memory expression above (e.g. RAX, [RBX+0x10]) "
            "to evaluate it at every stop."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    bar_layout->setSpacing(tokens.toolbar.group_gap);
    expression_edit_ = new widgets::AidaLineEdit(
        QStringLiteral("watch: RAX, [RBX+0x10], or 0x..."), bar);
    expression_edit_->setObjectName(
        QStringLiteral("aida.view.debug.watches.expression"));
    expression_edit_->setMaxLength(95);
    bar_layout->addWidget(expression_edit_, 1);
    add_button_ = new widgets::AidaButton(QStringLiteral("Add"), bar);
    add_button_->setObjectName(QStringLiteral("aida.view.debug.watches.add"));
    add_button_->setKind(widgets::AidaButton::Kind::Primary);
    add_button_->setToolTip(QStringLiteral(
        "Add the entered register or memory expression"));
    connect(add_button_, &widgets::AidaButton::clicked, this,
        &WatchesPane::addWatch);
    connect(expression_edit_, &QLineEdit::returnPressed, this,
        &WatchesPane::addWatch);
    add_button_->setEnabled(false);
    connect(expression_edit_, &QLineEdit::textChanged, this, [this] {
        add_button_->setEnabled(
            !expression_edit_->text().trimmed().isEmpty());
    });
    expression_edit_->setToolTip(QStringLiteral(
        "Watch expression: a register (RAX), a dereference ([RBX+0x10]), "
        "or a hex address (0x...)"));
    bar_layout->addWidget(add_button_);
    refresh_button_ = new widgets::AidaButton(QStringLiteral("Refresh"), bar);
    refresh_button_->setObjectName(
        QStringLiteral("aida.view.debug.watches.refresh"));
    refresh_button_->setKind(widgets::AidaButton::Kind::Secondary);
    refresh_button_->setToolTip(QStringLiteral(
        "Re-evaluate every watch expression against the current target state"));
    connect(refresh_button_, &widgets::AidaButton::clicked, this,
        &WatchesPane::refreshWatches);
    bar_layout->addWidget(refresh_button_);
    setToolBar(bar);

    model_ = new WatchesModel(this);
    view_ = new QTableView(this);
    view_->setObjectName(QStringLiteral("aida.view.debug.watches.table"));
    wireTable(view_, model_);
    setContent(view_);
    connect(view_, &QAbstractItemView::activated, this, [this](const QModelIndex& index) {
        const auto context = model_->contextForRow(index.row());
        if (context.address != 0)
            debugger_view::jump_to_hex(context.address, 256);
    });

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(250);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, [this] {
        auto& st = debugger_engine::g_state;
        model_->setRegisters(debugger_engine::cached_registers());
        const auto snapshot = snapshots_.poll(st.watch_mutex, st.watches,
            st.watches_generation, "watches");
        if (snapshot.refreshed)
            model_->applySnapshot(snapshot.items, snapshot.generation);
        const auto presentation = aida::ui::application_ui::present_action(
            "debugger.watch.refresh_all");
        refresh_button_->setEnabled(presentation.enabled);
        refresh_button_->setToolTip(presentation.enabled
            ? QStringLiteral(
                "Re-evaluate every watch expression against the current target state")
            : QString::fromStdString(presentation.disabled_reason.empty()
                ? "Watch refresh is unavailable in the current debugger state."
                : presentation.disabled_reason));
    });
    connect(&DebuggerActionBridge::instance(),
        &DebuggerActionBridge::watchExpressionStaged, this,
        &WatchesPane::stageExpression);
}

void WatchesPane::onShown() {
    poll_timer_->start();
    const QString staged =
        DebuggerActionBridge::instance().consumeWatchExpression();
    if (!staged.isEmpty())
        stageExpression(staged);
}

void WatchesPane::onHidden() {
    poll_timer_->stop();
}

bool WatchesPane::hasContentRows() const {
    return model_ && model_->rowCount() > 0;
}

void WatchesPane::onSessionStateChanged(int, quint32, quint64 stopGeneration) {
    updateOverlayState();
    if (stopGeneration != last_stop_generation_) {
        last_stop_generation_ = stopGeneration;
        refreshWatches();
    }
}

void WatchesPane::stageExpression(const QString& expression) {
    expression_edit_->setText(expression.left(95));
    expression_edit_->setFocus(Qt::OtherFocusReason);
}

void WatchesPane::addWatch() {
    const QString expression = expression_edit_->text().trimmed();
    if (expression.isEmpty())
        return;
    diag::log_tagged_critical_fmt("watches", "watch_add expr='%s'",
        expression.toStdString().c_str());
    const int index =
        debugger_engine::add_watch(expression.toStdString());
    if (index >= 0)
        expression_edit_->clear();
}

void WatchesPane::refreshWatches() {
    diag::log_tagged_fmt("watches", "watch_refresh_all_request");
    static_cast<void>(aida::ui::application_ui::execute_action(
        "debugger.watch.refresh_all",
        aida::ui::action_invocation_source_t::toolbar));
}

}
