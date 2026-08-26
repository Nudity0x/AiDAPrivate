#include "qt/bridge/action_bridge.hpp"

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_icons.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::bridge {

namespace {

constexpr int k_invalidation_interval_ms = 250;
constexpr int k_confirmation_reevaluate_interval_ms = 250;

bool dangerous_effects(aida::ui::action_effect_t effects) noexcept {
    using aida::ui::action_effect_t;
    const aida::ui::action_effect_t mask = action_effect_t::destructive |
        action_effect_t::security_sensitive | action_effect_t::live_process |
        action_effect_t::debugger_execution | action_effect_t::memory_mutation;
    return aida::ui::any(effects & mask);
}

bool high_consequence_effects(aida::ui::action_effect_t effects) noexcept {
    using aida::ui::action_effect_t;
    const aida::ui::action_effect_t mask = action_effect_t::destructive |
        action_effect_t::security_sensitive | action_effect_t::live_process |
        action_effect_t::file_system | action_effect_t::network_activity |
        action_effect_t::memory_mutation | action_effect_t::debugger_execution;
    return aida::ui::any(effects & mask);
}

QString source_tag(aida::ui::action_invocation_source_t source) {
    using aida::ui::action_invocation_source_t;
    switch (source) {
    case action_invocation_source_t::application_menu: return QStringLiteral("application_menu");
    case action_invocation_source_t::activity_bar: return QStringLiteral("activity_bar");
    case action_invocation_source_t::toolbar: return QStringLiteral("toolbar");
    case action_invocation_source_t::command_palette: return QStringLiteral("command_palette");
    case action_invocation_source_t::context_menu: return QStringLiteral("context_menu");
    case action_invocation_source_t::shortcut: return QStringLiteral("shortcut");
    case action_invocation_source_t::accessibility: return QStringLiteral("accessibility");
    }
    return QStringLiteral("unknown");
}

}

ConfirmationController::ConfirmationController(QObject* parent)
    : QObject(parent) {}

void ConfirmationController::clear_state() {
    state_ = state_t{};
}

bool ConfirmationController::queue(
    const QString& action_id, aida::ui::action_invocation_source_t source,
    const aida::ui::action_execution_result_t& result,
    const aida::ui::interaction_context_t& context) {
    using namespace aida::ui;
    if (result.status != action_execution_status_t::confirmation_required &&
        result.status != action_execution_status_t::review_required)
        return false;
    if (state_.active)
        cancel();
    auto& registry = application_ui::action_registry();
    const auto id = stable_action_id_t(action_id.toStdString());
    const auto* descriptor = registry.find(id);
    if (!descriptor)
        return false;
    clear_state();
    state_.active = true;
    state_.action = id.value();
    state_.label = descriptor->label;
    state_.description = descriptor->description;
    state_.consequence = result.consequence_summary;
    state_.source = source;
    state_.context = context;
    state_.retained_present = false;
    if (const auto* retained =
            context.payload.get<application_ui::retained_entity_runtime_context_t>()) {
        state_.retained_context.retained = retained->context();
        state_.retained_context.external = nullptr;
        state_.context.payload = typed_context_ref_t::from(
            stable_context_type_id_t("context.retained.entity"),
            state_.retained_context);
        state_.retained_present = true;
    }
    QWidget* parent = QApplication::activeWindow();
    auto* dialog = new ActionConfirmationDialog(this, parent);
    if (!state_.active) {
        dialog->deleteLater();
        return true;
    }
    dialog_ = dialog;
    connect(dialog, &QDialog::finished, this, [this](int) {
        if (state_.active)
            cancel();
    });
    Q_EMIT confirmation_opened(QString::fromStdString(state_.action));
    dialog->open();
    return true;
}

void ConfirmationController::cancel() {
    using namespace aida::ui;
    if (!state_.active)
        return;
    const std::string pending_id = state_.action;
    auto& registry = application_ui::action_registry();
    const auto* descriptor = registry.find(stable_action_id_t(pending_id));
    const auto cleanup = descriptor
        ? descriptor->cancel_confirmation
        : action_confirmation_cancel_fn_t{};
    clear_state();
    if (dialog_)
        dialog_->close();
    try {
        if (cleanup)
            cleanup();
    } catch (const std::exception& exception) {
        Q_EMIT diagnostic_raised(QStringLiteral("diagnostic.ui.confirmation.cleanup"),
            QString::fromStdString(pending_id),
            QStringLiteral("Action confirmation cleanup failed"),
            QString::fromUtf8(exception.what()));
    } catch (...) {
        Q_EMIT diagnostic_raised(QStringLiteral("diagnostic.ui.confirmation.cleanup"),
            QString::fromStdString(pending_id),
            QStringLiteral("Action confirmation cleanup failed"),
            QStringLiteral("The cleanup callback raised an unknown exception"));
    }
    Q_EMIT confirmation_closed(QString::fromStdString(pending_id));
}

void ConfirmationController::confirm_now() {
    using namespace aida::ui;
    if (!state_.active)
        return;
    auto& registry = application_ui::action_registry();
    const auto id = stable_action_id_t(state_.action);
    const auto* descriptor = registry.find(id);
    const auto state = registry.evaluate(id, state_.context);
    if (!descriptor || !state.capability.enabled) {
        publish_unavailable_and_close(descriptor != nullptr, state.capability);
        return;
    }
    const auto pending_source = state_.source;
    action_invocation_t invocation{state_.context};
    invocation.source = pending_source;
    invocation.invocation_id = application_ui::allocate_invocation_id();
    invocation.review_completed = true;
    invocation.confirmation_granted = true;
    const auto result = registry.execute(id, invocation);
    if (result.executed()) {
        clear_state();
        if (dialog_)
            dialog_->close();
    } else {
        cancel();
    }
    application_ui::publish_action_execution_failure(id.c_str(), result, pending_source);
}

void ConfirmationController::capability_lost_now() {
    using namespace aida::ui;
    if (!state_.active)
        return;
    auto& registry = application_ui::action_registry();
    const auto id = stable_action_id_t(state_.action);
    const auto* descriptor = registry.find(id);
    const auto state = descriptor
        ? registry.evaluate(id, state_.context)
        : action_state_t{};
    if (descriptor && state.capability.enabled)
        return;
    publish_unavailable_and_close(descriptor != nullptr, state.capability);
}

void ConfirmationController::publish_unavailable_and_close(
    bool descriptor_present, const aida::ui::capability_state_t& capability) {
    using namespace aida::ui;
    const QString pending_id = QString::fromStdString(state_.action);
    const auto pending_source = state_.source;
    action_execution_result_t unavailable;
    unavailable.status = action_execution_status_t::unavailable;
    unavailable.action = stable_action_id_t(state_.action);
    unavailable.message = !descriptor_present
        ? "The retained action is no longer registered"
        : capability.disabled_reason.empty()
            ? "The retained action became unavailable before confirmation"
            : capability.disabled_reason;
    cancel();
    application_ui::publish_action_execution_failure(
        unavailable.action.c_str(), unavailable, pending_source);
    diag::log_tagged_fmt("qt_action_bridge",
        "confirmation_capability_lost action=%s source=%s",
        pending_id.toUtf8().constData(),
        source_tag(pending_source).toUtf8().constData());
}

ActionConfirmationDialog::ActionConfirmationDialog(
    ConfirmationController* controller, QWidget* parent)
    : AidaDialog(parent), controller_(controller) {
    setObjectName(QStringLiteral("aida.action.confirmation"));
    setWindowTitle(QStringLiteral("Confirm Action"));
    setModal(true);
    setProperty("aidaSeverity", QStringLiteral("normal"));

    const auto& t = aida::qt::theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(t.panel.padding, t.panel.padding,
        t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);
    auto* title_label = new QLabel(this);
    title_label->setObjectName(QStringLiteral("aida.action.confirmation.title"));
    title_label->setFont(aida::qt::theme::fonts::strong());
    title_label->setWordWrap(true);
    title_label->setText(QString::fromStdString(controller_->state_.label));
    layout->addWidget(title_label);

    description_label_ = new QLabel(this);
    description_label_->setObjectName(
        QStringLiteral("aida.action.confirmation.description"));
    description_label_->setWordWrap(true);
    description_label_->setText(QString::fromStdString(controller_->state_.description));
    description_label_->setVisible(!controller_->state_.description.empty());
    layout->addWidget(description_label_);

    consequence_label_ = new QLabel(this);
    consequence_label_->setObjectName(
        QStringLiteral("aida.action.confirmation.consequence"));
    consequence_label_->setWordWrap(true);
    if (!controller_->state_.consequence.empty()) {
        consequence_label_->setText(
            QStringLiteral("Consequence: %1").arg(
                QString::fromStdString(controller_->state_.consequence)));
    } else {
        consequence_label_->setVisible(false);
    }
    layout->addWidget(consequence_label_);

    unavailable_label_ = new QLabel(this);
    unavailable_label_->setObjectName(
        QStringLiteral("aida.action.confirmation.unavailable"));
    unavailable_label_->setWordWrap(true);
    unavailable_label_->setProperty("aidaVariant", QStringLiteral("warning"));
    unavailable_label_->setVisible(false);
    layout->addWidget(unavailable_label_);

    layout->addStretch(1);

    auto* buttons = new QDialogButtonBox(this);
    confirm_button_ = buttons->addButton(QStringLiteral("Confirm"),
                                         QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    auto& registry = aida::ui::application_ui::action_registry();
    const auto* descriptor = registry.find(
        aida::ui::stable_action_id_t(controller_->state_.action));
    if (descriptor && high_consequence_effects(descriptor->consequence.effects)) {
        setProperty("aidaSeverity", QStringLiteral("high"));
        confirm_button_->setProperty("aidaVariant", QStringLiteral("destructive"));
        if (!controller_->state_.consequence.empty())
            consequence_label_->setProperty("aidaVariant", QStringLiteral("warning"));
    }

    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (finishing_)
            return;
        controller_->confirm_now();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        if (finishing_)
            return;
        finishing_ = true;
        controller_->cancel();
        reject();
    });
    connect(this, &QDialog::finished, this, &QDialog::deleteLater);

    reevaluate_timer_ = new QTimer(this);
    reevaluate_timer_->setInterval(k_confirmation_reevaluate_interval_ms);
    connect(reevaluate_timer_, &QTimer::timeout, this,
            &ActionConfirmationDialog::reevaluate);
    reevaluate_timer_->start();
    reevaluate();
}

void ActionConfirmationDialog::reevaluate() {
    if (!controller_->state_.active)
        return;
    auto& registry = aida::ui::application_ui::action_registry();
    const auto id = aida::ui::stable_action_id_t(controller_->state_.action);
    const auto* descriptor = registry.find(id);
    const auto state = descriptor
        ? registry.evaluate(id, controller_->state_.context)
        : aida::ui::action_state_t{};
    if (!descriptor || !state.capability.enabled) {
        finishing_ = true;
        controller_->capability_lost_now();
        return;
    }
    apply_capability(state, true);
}

void ActionConfirmationDialog::apply_capability(
    const aida::ui::action_state_t& state, bool descriptor_present) {
    confirm_button_->setEnabled(descriptor_present && state.capability.enabled);
    const bool show_unavailable = descriptor_present && !state.capability.enabled &&
        !state.capability.disabled_reason.empty();
    unavailable_label_->setVisible(show_unavailable);
    if (show_unavailable) {
        unavailable_label_->setText(QStringLiteral("Unavailable: %1").arg(
            QString::fromStdString(state.capability.disabled_reason)));
    }
}

ActionBridge::ActionBridge(InteractionContextProvider* context, QObject* parent)
    : QObject(parent), context_(context) {
    confirmations_ = new ConfirmationController(this);
    invalidation_timer_ = new QTimer(this);
    invalidation_timer_->setInterval(k_invalidation_interval_ms);
    connect(invalidation_timer_, &QTimer::timeout, this, [this] {
        QWidget* window = QApplication::activeWindow();
        if (window && window->isVisible())
            ensure_current();
    });
    invalidation_timer_->start();
}

QAction* ActionBridge::hydrate(
    const aida::ui::application_action_descriptor_t& descriptor) {
    auto* action = new QAction(this);
    const QString id = QString::fromStdString(descriptor.id.value());
    action->setObjectName(QStringLiteral("aida.") + id);
    action->setText(QString::fromStdString(descriptor.label));
    action->setToolTip(QString::fromStdString(descriptor.description));
    action->setStatusTip(QString::fromStdString(descriptor.description));
    action->setData(id);
    action->setCheckable(descriptor.checked != nullptr);
    if (dangerous_effects(descriptor.consequence.effects))
        action->setAutoRepeat(false);
    if (!descriptor.icon_semantic.empty()) {
        const QIcon icon = aida::qt::theme::icons::icon(
            QString::fromStdString(descriptor.icon_semantic));
        if (!icon.isNull())
            action->setIcon(icon);
    }
    connect(action, &QAction::triggered, this, [this, id](bool) {
        dispatch(id, aida::ui::action_invocation_source_t::application_menu);
    });
    return action;
}

void ActionBridge::rebuild() {
    auto& registry = aida::ui::application_ui::action_registry();
    actions_.clear();
    registry.for_each([this](const aida::ui::application_action_descriptor_t& descriptor) {
        QAction* action = hydrate(descriptor);
        actions_.insert(QString::fromStdString(descriptor.id.value()), action);
    });
    built_revision_ = registry.revision();
    built_ = true;
    Q_EMIT actions_rebuilt();
    diag::log_tagged_fmt("qt_action_bridge", "actions_rebuilt count=%llu revision=%llu",
        static_cast<unsigned long long>(actions_.size()),
        static_cast<unsigned long long>(built_revision_));
}

void ActionBridge::ensure_current() {
    auto& registry = aida::ui::application_ui::action_registry();
    const std::uint64_t revision = registry.revision();
    if (!built_ || revision != built_revision_)
        rebuild();
}

QAction* ActionBridge::action(const QString& id) {
    ensure_current();
    return actions_.value(id, nullptr).data();
}

QAction* ActionBridge::menu_action(const QString& id, const QString& label_override,
                                   const QString& shortcut_hint_override, QMenu* menu) {
    ensure_current();
    QAction* canonical = actions_.value(id, nullptr).data();
    if (!canonical)
        return nullptr;
    const QString hint = !shortcut_hint_override.isEmpty()
        ? shortcut_hint_override
        : shortcut_hint(id, context_->current());
    if (label_override.isEmpty() && hint.isEmpty()) {
        refresh(canonical);
        return canonical;
    }
    QString label = label_override.isEmpty() ? canonical->text() : label_override;
    if (!hint.isEmpty())
        label += u'\t' + hint;
    auto* child = new QAction(label, menu);
    child->setObjectName(canonical->objectName() + QStringLiteral(".menu"));
    child->setData(id);
    child->setCheckable(canonical->isCheckable());
    const QIcon canonical_icon = canonical->icon();
    if (!canonical_icon.isNull())
        child->setIcon(canonical_icon);
    child->setAutoRepeat(canonical->autoRepeat());
    child->setShortcutVisibleInContextMenu(true);
    connect(child, &QAction::triggered, this, [this, id](bool) {
        dispatch(id, aida::ui::action_invocation_source_t::application_menu);
    });
    refresh(child);
    return child;
}

QAction* ActionBridge::surface_action(
    const QString& id, aida::ui::action_invocation_source_t source, QObject* parent) {
    ensure_current();
    QAction* canonical = actions_.value(id, nullptr).data();
    if (!canonical)
        return nullptr;
    auto* child = new QAction(canonical->text(), parent);
    child->setObjectName(canonical->objectName() + QStringLiteral(".surface"));
    child->setData(id);
    child->setCheckable(canonical->isCheckable());
    const QIcon canonical_icon = canonical->icon();
    if (!canonical_icon.isNull())
        child->setIcon(canonical_icon);
    child->setAutoRepeat(canonical->autoRepeat());
    child->setToolTip(canonical->toolTip());
    child->setStatusTip(canonical->statusTip());
    connect(child, &QAction::triggered, this, [this, id, source](bool) {
        dispatch(id, source);
    });
    refresh(child);
    return child;
}

void ActionBridge::refresh(QAction* action) const {
    if (!action)
        return;
    const QString id = action->data().toString();
    auto& registry = aida::ui::application_ui::action_registry();
    const auto* descriptor = registry.find(
        aida::ui::stable_action_id_t(id.toStdString()));
    if (!descriptor) {
        action->setEnabled(false);
        action->setVisible(false);
        return;
    }
    const auto state = registry.evaluate(descriptor->id, context_->current());
    action->setEnabled(state.capability.enabled);
    action->setVisible(state.capability.visible);
    QString tooltip = QString::fromStdString(descriptor->description);
    bool mixed = false;
    if (descriptor->checked) {
        switch (state.check_state) {
        case aida::ui::action_check_state_t::checked:
            action->setChecked(true);
            break;
        case aida::ui::action_check_state_t::mixed:
            action->setChecked(true);
            mixed = true;
            break;
        default:
            action->setChecked(false);
            break;
        }
    }
    if (!state.capability.enabled && !state.capability.disabled_reason.empty()) {
        tooltip += QStringLiteral("\nUnavailable: %1").arg(
            QString::fromStdString(state.capability.disabled_reason));
    }
    if (mixed)
        tooltip += QStringLiteral(" (mixed)");
    action->setToolTip(tooltip);
}

void ActionBridge::dispatch(const QString& id,
                            aida::ui::action_invocation_source_t source) {
    dispatch(id, source, context_->current());
}

void ActionBridge::dispatch(const QString& id,
                            aida::ui::action_invocation_source_t source,
                            const aida::ui::interaction_context_t& context) {
    auto& registry = aida::ui::application_ui::action_registry();
    aida::ui::action_invocation_t invocation{context};
    invocation.source = source;
    invocation.invocation_id = aida::ui::application_ui::allocate_invocation_id();
    auto result = registry.execute(
        aida::ui::stable_action_id_t(id.toStdString()), invocation);
    finalize(id, result, source, invocation.context);
}

void ActionBridge::finalize(const QString& id,
                            const aida::ui::action_execution_result_t& result,
                            aida::ui::action_invocation_source_t source,
                            const aida::ui::interaction_context_t& context) {
    if (confirmations_->queue(id, source, result, context))
        return;
    aida::ui::application_ui::publish_action_execution_failure(
        id.toStdString().c_str(), result, source);
}

QString ActionBridge::shortcut_hint(
    const QString& id, const aida::ui::interaction_context_t& context) const {
    auto& resolver = aida::ui::application_ui::shortcut_resolver();
    return QString::fromStdString(resolver.effective_hint(
        aida::ui::stable_action_id_t(id.toStdString()), context));
}

}
