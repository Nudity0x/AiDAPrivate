#include "qt/overlays/aida_no_target_overlay.hpp"

#include <QVBoxLayout>

#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"

namespace aida::qt::overlays {

namespace {

AidaEmptyStateAction action_from_presentation(
    const char* id,
    const aida::ui::application_ui::action_presentation_t& presentation, int kind)
{
    AidaEmptyStateAction action;
    action.id = QString::fromLatin1(id);
    action.label = QString::fromStdString(presentation.label);
    action.kind = kind;
    action.disabled = !presentation.enabled;
    action.tooltip = QString::fromStdString(presentation.enabled
        ? presentation.description : presentation.disabled_reason);
    return action;
}

}

AidaNoTargetOverlay::AidaNoTargetOverlay(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.no_target_overlay"));
    state_ = new AidaEmptyState(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(state_);
    connect(state_, &AidaEmptyState::actionTriggered, this,
            [](const QString& id) {
        const char* action_id = nullptr;
        if (id == QLatin1String("open_file")) {
            diag::log_tagged_critical("file_dialog", "no_target_overlay.open_binary_clicked");
            action_id = "tools.load_binary";
        } else if (id == QLatin1String("attach_process")) {
            diag::log_tagged_critical("file_dialog", "no_target_overlay.attach_clicked");
            action_id = "tools.attach_process";
        } else if (id == QLatin1String("run_target")) {
            diag::log_tagged_critical("file_dialog", "no_target_overlay.run_clicked");
            action_id = "debugger.launch";
        }
        if (action_id)
            static_cast<void>(aida::ui::application_ui::execute_action(
                action_id, aida::ui::action_invocation_source_t::toolbar));
    });
}

AidaNoTargetOverlay::AidaNoTargetOverlay(const AidaNoTargetConfig& config, QWidget* parent)
    : AidaNoTargetOverlay(parent)
{
    setConfig(config);
}

QString AidaNoTargetOverlay::stateIdForGlyph(AidaGlyph glyph)
{
    switch (glyph) {
    case AidaGlyph::Cpu:     return QStringLiteral("no_target.analysis");
    case AidaGlyph::Shield:  return QStringLiteral("no_target.debugger");
    case AidaGlyph::Search:  return QStringLiteral("no_target.scanner");
    case AidaGlyph::Network: return QStringLiteral("no_target.network");
    case AidaGlyph::Memory:  return QStringLiteral("no_target.memory");
    default:                 return QStringLiteral("no_target.binary");
    }
}

void AidaNoTargetOverlay::setConfig(const AidaNoTargetConfig& config)
{
    config_ = config;
    rebuild();
}

void AidaNoTargetOverlay::refreshCapabilities()
{
    rebuild();
}

void AidaNoTargetOverlay::rebuild()
{
    using aida::ui::application_ui::present_action;
    const auto open_action = present_action("tools.load_binary");
    const auto attach_action = present_action("tools.attach_process");
    const auto run_action = present_action("debugger.launch");

    AidaEmptyStateConfig config;
    config.glyph = config_.glyph;
    config.title = config_.title;
    config.body = config_.subtitle;
    config.footer = config_.hint.isEmpty()
        ? QStringLiteral("Drag any .exe, .dll, or .sys into this window, then send evidence to the AI Assistant.")
        : config_.hint;
    config.actions.push_back(action_from_presentation("open_file", open_action, 0));
    config.actions.push_back(action_from_presentation("attach_process", attach_action, 1));
    config.actions.push_back(action_from_presentation("run_target", run_action, 1));
    if (state_)
        state_->setConfig(config);
}

}
