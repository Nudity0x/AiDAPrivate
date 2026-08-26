#include "qt/debugger/patches_pane.hpp"

#include <QHBoxLayout>
#include <QTableView>
#include <QTimer>

#include "core/analysis/code_patcher.hpp"
#include "core/debugger/debugger_view.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/toast_notification.hpp"

#include "qt/debugger/debugger_action_bridge.hpp"
#include "qt/debugger/debugger_models.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_notice.hpp"

namespace aida::qt::debugger {

PatchesPane::PatchesPane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.patches"));
    setOwnerViewId("view.debug.patches");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral(
            "Attach to a process to stage and apply live-memory patches."));
    setEmptyContentText(QStringLiteral("No patches staged"),
        QStringLiteral(
            "Stage a byte patch with Stage Patch... above, or from a selected "
            "range in the hex view."));
    setErrorText(QStringLiteral("Patch list publication failed"),
        QStringLiteral(
            "The authoritative patch state was preserved, but the immutable "
            "UI snapshot could not be refreshed."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    bar_layout->setSpacing(tokens.toolbar.group_gap);

    stage_button_ = new widgets::AidaButton(QStringLiteral("Stage Patch..."),
        bar);
    stage_button_->setObjectName(QStringLiteral("aida.view.debug.patches.stage"));
    stage_button_->setKind(widgets::AidaButton::Kind::Primary);
    stage_button_->setToolTip(QStringLiteral(
        "Review a new runtime byte patch"));
    connect(stage_button_, &widgets::AidaButton::clicked, this,
        [this] { runPanelCommand(0); });
    bar_layout->addWidget(stage_button_);
    caves_button_ = new widgets::AidaButton(QStringLiteral("Find Code Caves..."),
        bar);
    caves_button_->setObjectName(QStringLiteral("aida.view.debug.patches.caves"));
    caves_button_->setKind(widgets::AidaButton::Kind::Secondary);
    caves_button_->setToolTip(QStringLiteral(
        "Find bounded code caves in the attached module"));
    connect(caves_button_, &widgets::AidaButton::clicked, this,
        [this] { runPanelCommand(1); });
    bar_layout->addWidget(caves_button_);
    revert_all_button_ = new widgets::AidaButton(
        QStringLiteral("Revert All Patches..."), bar);
    revert_all_button_->setObjectName(
        QStringLiteral("aida.view.debug.patches.revert_all"));
    revert_all_button_->setKind(widgets::AidaButton::Kind::Destructive);
    revert_all_button_->setToolTip(QStringLiteral(
        "Review restoring every active runtime patch"));
    connect(revert_all_button_, &widgets::AidaButton::clicked, this,
        [this] { runPanelCommand(2); });
    bar_layout->addWidget(revert_all_button_);
    save_button_ = new widgets::AidaButton(QStringLiteral("Save Patchset..."),
        bar);
    save_button_->setObjectName(QStringLiteral("aida.view.debug.patches.save"));
    save_button_->setKind(widgets::AidaButton::Kind::Secondary);
    save_button_->setToolTip(QStringLiteral(
        "Save the current patch set to a file"));
    connect(save_button_, &widgets::AidaButton::clicked, this,
        [this] { runPanelCommand(3); });
    bar_layout->addWidget(save_button_);
    bar_layout->addStretch(1);
    setToolBar(bar);

    publication_notice_ = new widgets::AidaNotice(QStringLiteral(
        "Patch list publication unavailable"), QStringLiteral(
        "The authoritative patch state was preserved, but the immutable UI "
        "snapshot could not be refreshed. Patch actions remain disabled until "
        "a later publication succeeds."),
        widgets::AidaSemantic::Warning, this);
    publication_notice_->setObjectName(
        QStringLiteral("aida.view.debug.patches.publication_notice"));
    publication_notice_->setVisible(false);

    model_ = new PatchesModel(this);
    view_ = new QTableView(this);
    view_->setObjectName(QStringLiteral("aida.view.debug.patches.table"));
    wireTable(view_, model_);

    auto* body = new QWidget(this);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(tokens.spacing.xs);
    body_layout->addWidget(publication_notice_);
    body_layout->addWidget(view_, 1);
    setContent(body);

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(250);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, &PatchesPane::pollModel);
    connect(&DebuggerActionBridge::instance(),
        &DebuggerActionBridge::patchRowFocusRequested, this,
        [this](int index) {
            if (index >= 0 && index < model_->rowCount())
                view_->setCurrentIndex(model_->index(index, 0));
        });
}

void PatchesPane::onShown() {
    poll_timer_->start();
    const int focus = DebuggerActionBridge::instance().consumePatchRowFocus();
    if (focus >= 0 && focus < model_->rowCount())
        view_->setCurrentIndex(model_->index(focus, 0));
    pollModel();
}

void PatchesPane::onHidden() {
    poll_timer_->stop();
}

void PatchesPane::onSessionTick() {
    pollModel();
    updateOverlayState();
}

bool PatchesPane::hasContentRows() const {
    return model_ && model_->rowCount() > 0;
}

bool PatchesPane::contentError(QString* detail) const {
    if (code_patcher::g_state.publication_failure_generation.load(
            std::memory_order_acquire) == 0)
        return false;
    if (detail) {
        *detail = QStringLiteral(
            "The authoritative patch state was preserved, but the immutable "
            "UI snapshot could not be refreshed. Patch actions remain "
            "disabled until a later publication succeeds.");
    }
    return true;
}

void PatchesPane::pollModel() {
    const auto snapshot = code_patcher::published_snapshot();
    const std::uint64_t generation = snapshot ? snapshot->generation : 0;
    const std::uint64_t failure_generation =
        code_patcher::g_state.publication_failure_generation.load(
            std::memory_order_acquire);
    publication_notice_->setVisible(!snapshot || failure_generation != 0);
    if (snapshot && generation != last_generation_) {
        last_generation_ = generation;
        const auto selected = capture_selected_row_ids(*model_,
            view_->selectionModel());
        const quint64 focus = view_->currentIndex().isValid()
            ? model_->rowId(view_->currentIndex().row()) : 0;
        std::vector<PatchesModel::row_t> rows;
        rows.reserve(snapshot->rows.size());
        for (const auto& row : snapshot->rows) {
            PatchesModel::row_t out;
            out.address = row.address;
            out.patched_size = row.patched_size;
            out.original = QString::fromStdString(row.original);
            out.patched = QString::fromStdString(row.patched);
            out.description = QString::fromStdString(row.description);
            out.active = row.active;
            rows.push_back(std::move(out));
        }
        model_->applyRows(std::move(rows), generation, snapshot->total_count);
        restore_selected_row_ids(*model_, view_, selected, focus);
    }

    const auto refreshGate = [this](widgets::AidaButton* button,
                                    debugger_view::patch_panel_command_t
                                        command,
                                    const QString& defaultTip) {
        const auto capability = debugger_view::patch_panel_capability(command);
        button->setEnabled(capability.enabled);
        button->setToolTip(capability.enabled
            ? defaultTip
            : QString::fromLatin1(capability.disabled_reason
                ? capability.disabled_reason
                : "The Patches panel action is unavailable"));
    };
    refreshGate(stage_button_, debugger_view::patch_panel_command_t::stage,
        QStringLiteral("Review a new runtime byte patch"));
    refreshGate(caves_button_,
        debugger_view::patch_panel_command_t::find_code_caves,
        QStringLiteral("Find bounded code caves in the attached module"));
    refreshGate(revert_all_button_,
        debugger_view::patch_panel_command_t::revert_all,
        QStringLiteral("Review restoring every active runtime patch"));
    refreshGate(save_button_,
        debugger_view::patch_panel_command_t::save_patchset,
        QStringLiteral("Save the current patch set to a file"));
}

void PatchesPane::runPanelCommand(int command) {
    std::string error;
    const auto mapped = command == 0
        ? debugger_view::patch_panel_command_t::stage
        : command == 1 ? debugger_view::patch_panel_command_t::find_code_caves
        : command == 2 ? debugger_view::patch_panel_command_t::revert_all
            : debugger_view::patch_panel_command_t::save_patchset;
    if (!debugger_view::execute_patch_panel_command(mapped, &error) &&
        !error.empty())
        notify_panel_error(error);
}

void PatchesPane::notify_panel_error(const std::string& error) {
    toast_notification::push(error, toast_notification::toast_type_t::error);
}

}
