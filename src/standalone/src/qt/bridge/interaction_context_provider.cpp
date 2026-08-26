#include "qt/bridge/interaction_context_provider.hpp"

#include <QApplication>
#include <QVariant>
#include <QWidget>

#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"

namespace aida::qt::bridge {

InteractionContextProvider::InteractionContextProvider(QObject* parent)
    : QObject(parent) {
    aida::ui::application_ui::set_interaction_context_source(
        [this] { return base_context(); });
    if (qApp) {
        connect(qApp, &QApplication::focusChanged, this,
                [this](QWidget*, QWidget*) {
                    bump_generation();
                    Q_EMIT contextChanged();
                });
    } else {
        diag::log_tagged_critical("qt_context",
            "interaction_context_provider constructed before QApplication");
    }
}

InteractionContextProvider::~InteractionContextProvider() {
    aida::ui::application_ui::set_interaction_context_source(nullptr);
}

aida::ui::interaction_context_t InteractionContextProvider::base_context() const {
    aida::ui::interaction_context_t context;
    context.modal_active = QApplication::activeModalWidget() != nullptr;
    rebuild_focus_path(context);
    context.text_input_active = text_input_active();
    if (active_view_hook_) {
        auto active = active_view_hook_();
        if (!active.first.empty()) {
            context.active_view = aida::ui::stable_view_id_t(std::move(active.first));
            context.active_view_instance =
                aida::ui::stable_view_instance_key_t(std::move(active.second));
        }
    }
    return context;
}

aida::ui::interaction_context_t InteractionContextProvider::current() const {
    return aida::ui::application_ui::current_interaction_context();
}

std::uint64_t InteractionContextProvider::generation() const {
    return aida::ui::application_ui::interaction_generation();
}

void InteractionContextProvider::bump_generation() {
    aida::ui::application_ui::bump_interaction_generation();
}

bool InteractionContextProvider::text_input_active() const {
    QWidget* focus = QApplication::focusWidget();
    if (!focus)
        return false;
    if (focus->testAttribute(Qt::WA_InputMethodEnabled))
        return true;
    return focus->property(k_text_input_property).toBool();
}

bool InteractionContextProvider::modal_active() const {
    return QApplication::activeModalWidget() != nullptr;
}

void InteractionContextProvider::set_active_view_hook(
    std::function<std::pair<std::string, std::string>()> hook) {
    active_view_hook_ = std::move(hook);
}

void InteractionContextProvider::attach_scope(QWidget* widget,
                                              const QString& scope_id,
                                              aida::ui::focus_scope_kind_t kind) {
    if (!widget)
        return;
    widget->setProperty(k_scope_id_property, scope_id);
    widget->setProperty(k_scope_kind_property,
                        QString::fromLatin1(scope_kind_token(kind)));
}

void InteractionContextProvider::mark_text_input(QWidget* widget,
                                                 bool accepts_text) {
    if (!widget)
        return;
    widget->setProperty(k_text_input_property, accepts_text);
}

const char* InteractionContextProvider::scope_kind_token(
    aida::ui::focus_scope_kind_t kind) noexcept {
    switch (kind) {
    case aida::ui::focus_scope_kind_t::global: return "global";
    case aida::ui::focus_scope_kind_t::domain: return "domain";
    case aida::ui::focus_scope_kind_t::document: return "document";
    case aida::ui::focus_scope_kind_t::widget: return "widget";
    case aida::ui::focus_scope_kind_t::table: return "table";
    case aida::ui::focus_scope_kind_t::tree: return "tree";
    case aida::ui::focus_scope_kind_t::canvas: return "canvas";
    case aida::ui::focus_scope_kind_t::text_editor: return "text_editor";
    case aida::ui::focus_scope_kind_t::modal: return "modal";
    }
    return "widget";
}

std::optional<aida::ui::focus_scope_kind_t>
InteractionContextProvider::scope_kind_from_token(const QString& token) noexcept {
    using aida::ui::focus_scope_kind_t;
    if (token == QLatin1String("global")) return focus_scope_kind_t::global;
    if (token == QLatin1String("domain")) return focus_scope_kind_t::domain;
    if (token == QLatin1String("document")) return focus_scope_kind_t::document;
    if (token == QLatin1String("widget")) return focus_scope_kind_t::widget;
    if (token == QLatin1String("table")) return focus_scope_kind_t::table;
    if (token == QLatin1String("tree")) return focus_scope_kind_t::tree;
    if (token == QLatin1String("canvas")) return focus_scope_kind_t::canvas;
    if (token == QLatin1String("text_editor")) return focus_scope_kind_t::text_editor;
    if (token == QLatin1String("modal")) return focus_scope_kind_t::modal;
    return std::nullopt;
}

void InteractionContextProvider::rebuild_focus_path(
    aida::ui::interaction_context_t& context) const {
    context.focus_path.clear();
    QWidget* widget = QApplication::focusWidget();
    while (widget) {
        const QVariant scope_id = widget->property(k_scope_id_property);
        if (scope_id.isValid()) {
            const QString id = scope_id.toString();
            if (!id.isEmpty()) {
                const QVariant kind_value = widget->property(k_scope_kind_property);
                const auto kind = scope_kind_from_token(kind_value.toString())
                    .value_or(aida::ui::focus_scope_kind_t::widget);
                context.focus_path.push_back(
                    {aida::ui::stable_scope_id_t(id.toStdString()), kind});
            }
        }
        widget = widget->parentWidget();
    }
}

}
