#include "qt/debugger/breakpoints_pane.hpp"

#include <QHeaderView>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_engine.hpp"
#include "core/debugger/debugger_view.hpp"
#include "core/ui/toast_notification.hpp"

#include "qt/debugger/debugger_action_bridge.hpp"
#include "qt/debugger/debugger_mutation_queue.hpp"
#include "qt/debugger/debugger_models.hpp"
#include "qt/debugger/debugger_session_controller.hpp"
#include "qt/debugger/debugger_snapshot_store.hpp"
#include "qt/debugger/dialogs/breakpoint_edit_dialog.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_line_edit.hpp"

namespace aida::qt::debugger {

BreakpointsPane::BreakpointsPane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.breakpoints"));
    setOwnerViewId("view.debug.breakpoints");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral(
            "Attach to a process to set and review its breakpoints."));
    setEmptyContentText(QStringLiteral("No breakpoints set"),
        QStringLiteral(
            "Add a software or hardware breakpoint above, or stage one from "
            "the disassembly view."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    bar_layout->setSpacing(tokens.toolbar.group_gap);

    address_edit_ = new widgets::AidaLineEdit(
        QStringLiteral("0x... breakpoint address"), bar);
    address_edit_->setObjectName(
        QStringLiteral("aida.view.debug.breakpoints.address"));
    address_edit_->setMaxLength(23);
    address_edit_->setToolTip(QStringLiteral(
        "Hexadecimal breakpoint address (e.g. 0x140001234); Enter adds a "
        "software breakpoint"));
    connect(address_edit_, &QLineEdit::returnPressed, this,
        [this] { addBreakpoint(false); });
    bar_layout->addWidget(address_edit_, 1);

    add_sw_button_ = new widgets::AidaButton(QStringLiteral("Add SW"), bar);
    add_sw_button_->setObjectName(
        QStringLiteral("aida.view.debug.breakpoints.add_sw"));
    add_sw_button_->setKind(widgets::AidaButton::Kind::Primary);
    add_sw_button_->setToolTip(QStringLiteral(
        "Queue a software (INT3) breakpoint at the entered address"));
    connect(add_sw_button_, &widgets::AidaButton::clicked, this,
        [this] { addBreakpoint(false); });
    bar_layout->addWidget(add_sw_button_);

    add_hw_button_ = new widgets::AidaButton(QStringLiteral("Add HW"), bar);
    add_hw_button_->setObjectName(
        QStringLiteral("aida.view.debug.breakpoints.add_hw"));
    add_hw_button_->setKind(widgets::AidaButton::Kind::Secondary);
    add_hw_button_->setToolTip(QStringLiteral(
        "Queue a hardware execute breakpoint at the entered address"));
    connect(add_hw_button_, &widgets::AidaButton::clicked, this,
        [this] { addBreakpoint(true); });
    bar_layout->addWidget(add_hw_button_);

    clear_button_ = new widgets::AidaButton(QStringLiteral("Clear"), bar);
    clear_button_->setObjectName(
        QStringLiteral("aida.view.debug.breakpoints.clear"));
    clear_button_->setKind(widgets::AidaButton::Kind::Destructive);
    clear_button_->setToolTip(QStringLiteral(
        "Review removal of every breakpoint definition"));
    connect(clear_button_, &widgets::AidaButton::clicked, this, [this] {
        const auto context = debugger_interaction::capture(
            debugger_interaction::kind_t::breakpoint);
        DebuggerMutationQueue::instance().queueMutation(
            "Clear all breakpoints", "debugger.breakpoint_clear_all", context,
            []() {
                debugger_view::mutation_result_t result;
                const bool cleared = debugger_engine::clear_all_breakpoints();
                result.ok = result.verified = cleared &&
                    debugger_engine::snapshot_breakpoints().empty();
                if (!result.ok)
                    result.detail =
                        "Clear all breakpoints did not verify: " +
                        debugger_engine::last_error();
                return result;
            });
    });
    bar_layout->addWidget(clear_button_);
    setToolBar(bar);

    model_ = new BreakpointsModel(this);
    view_ = new QTableView(this);
    view_->setObjectName(QStringLiteral("aida.view.debug.breakpoints.table"));
    wireTable(view_, model_);
    view_->setItemDelegateForColumn(BreakpointsModel::State,
        new StatePillDelegate(view_));
    setContent(view_);

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(250);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, [this] {
        if (staged_ && !debugger_interaction::is_current(staged_context_)) {
            clearStaged(true);
            toast_notification::push(
                "The reviewed breakpoint target or debugger stop changed; "
                "stage the address again.",
                toast_notification::toast_type_t::warning);
        }
        auto& st = debugger_engine::g_state;
        const auto snapshot = snapshots_.poll(st.bp_mutex, st.breakpoints,
            st.breakpoints_generation, "breakpoints");
        if (snapshot.refreshed)
            applySnapshotPreservingSelection(snapshot.items, snapshot.generation);
    });
    connect(view_, &QAbstractItemView::activated, this,
        [this](const QModelIndex&) { editSelected(); });
    connect(&DebuggerActionBridge::instance(),
        &DebuggerActionBridge::breakpointAddressStaged, this,
        [this](quint64 address, bool hardwareExecute) {
            std::uint64_t staged_address = 0;
            bool hw = false;
            debugger_interaction::context_t context;
            if (DebuggerActionBridge::instance().consumeBreakpointStage(
                    staged_address, hw, context) && staged_address == address)
                stageAddress(staged_address, hardwareExecute, context);
        });
}

void BreakpointsPane::onShown() {
    poll_timer_->start();
    std::uint64_t staged_address = 0;
    bool hw = false;
    debugger_interaction::context_t context;
    if (DebuggerActionBridge::instance().consumeBreakpointStage(
            staged_address, hw, context))
        stageAddress(staged_address, hw, context);
    auto& st = debugger_engine::g_state;
    const auto snapshot = snapshots_.poll(st.bp_mutex, st.breakpoints,
        st.breakpoints_generation, "breakpoints");
    if (snapshot.refreshed)
        applySnapshotPreservingSelection(snapshot.items, snapshot.generation);
}

void BreakpointsPane::onHidden() {
    poll_timer_->stop();
}

void BreakpointsPane::onSessionTick() {
    DebuggerPaneBase::onSessionTick();
    updateOverlayState();
}

bool BreakpointsPane::hasContentRows() const {
    return model_ && model_->rowCount() > 0;
}

void BreakpointsPane::applySnapshotPreservingSelection(
    BreakpointsModel::const_rows_ptr rows, quint64 generation) {
    const auto selected = capture_selected_row_ids(*model_,
        view_->selectionModel());
    const quint64 focus = view_->currentIndex().isValid()
        ? model_->rowId(view_->currentIndex().row()) : 0;
    model_->applySnapshot(std::move(rows), generation);
    restore_selected_row_ids(*model_, view_, selected, focus);
}

void BreakpointsPane::stageAddress(
    std::uint64_t address, bool hardwareExecute,
    const debugger_interaction::context_t& context) {
    staged_ = true;
    staged_hardware_ = hardwareExecute;
    staged_context_ = context;
    address_edit_->setText(QString::asprintf("%llX",
        static_cast<unsigned long long>(address)));
}

void BreakpointsPane::clearStaged(bool clearAddress) {
    staged_ = false;
    staged_hardware_ = false;
    staged_context_ = {};
    if (clearAddress)
        address_edit_->clear();
}

void BreakpointsPane::addBreakpoint(bool hardwareExecute) {
    const std::uint64_t address =
        debugger_view::parse_hex_address(address_edit_->text().toStdString());
    diag::log_tagged_critical_fmt("bp",
        "bp_add_%s_request raw='%s' parsed_addr=0x%llx",
        hardwareExecute ? "hw" : "sw",
        address_edit_->text().toStdString().c_str(),
        static_cast<unsigned long long>(address));
    if (address == 0) {
        toast_notification::push(
            "Enter a hexadecimal address (e.g. 0x140001234).",
            toast_notification::toast_type_t::warning);
        return;
    }
    debugger_interaction::context_t context;
    std::string error;
    if (!staged_) {
        context = debugger_interaction::capture(
            debugger_interaction::kind_t::breakpoint, address);
    } else {
        if (staged_hardware_ != hardwareExecute)
            error = "The staged breakpoint mode changed; stage the definition again.";
        else if (staged_context_.address != address)
            error = "The staged breakpoint address was edited; stage the definition again.";
        else if (!debugger_interaction::is_current(staged_context_))
            error = "The staged breakpoint target or debugger stop changed; stage the definition again.";
        if (!error.empty()) {
            clearStaged(true);
            toast_notification::push(error,
                toast_notification::toast_type_t::warning);
            return;
        }
        context = staged_context_;
    }
    const bool software_enabled = !staged_ || !staged_hardware_;
    const bool hardware_enabled = !staged_ || staged_hardware_;
    if (hardwareExecute && !hardware_enabled) {
        toast_notification::push(
            "The retained reviewed handoff requires a software breakpoint",
            toast_notification::toast_type_t::warning);
        return;
    }
    if (!hardwareExecute && !software_enabled) {
        toast_notification::push(
            "The retained reviewed handoff requires a hardware execute breakpoint",
            toast_notification::toast_type_t::warning);
        return;
    }
    const bool queued = DebuggerMutationQueue::instance().queueMutation(
        hardwareExecute ? "Add hardware execute breakpoint"
                        : "Add software breakpoint",
        hardwareExecute ? "debugger.breakpoint_add_hardware"
                        : "debugger.breakpoint_add_software",
        context, [address, hardwareExecute]() {
            debugger_view::mutation_result_t result;
            result.ok = result.verified = debugger_engine::add_breakpoint(
                address,
                hardwareExecute
                    ? debugger_engine::bp_type_t::hardware_execute
                    : debugger_engine::bp_type_t::software,
                "", "", 1) >= 0;
            if (!result.ok)
                result.detail = hardwareExecute
                    ? "Add hardware breakpoint failed: " +
                        debugger_engine::last_error()
                    : "Add software breakpoint failed: " +
                        debugger_engine::last_error();
            return result;
        });
    if (queued)
        clearStaged(true);
}

void BreakpointsPane::editSelected() {
    const auto index = view_->currentIndex();
    if (!index.isValid())
        return;
    const auto context = model_->contextForRow(index.row());
    if (context.kind == debugger_interaction::kind_t::none)
        return;
    BreakpointEditDialog::openFor(context, context.index,
        debugger_view::breakpoint_editor_focus_t::condition, this);
}

}
