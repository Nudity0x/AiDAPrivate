#include "qt/ai/qt_ai_chat_pills.hpp"

#include <QAction>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScreen>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "core/ai/agent_registry.hpp"
#include "core/ai/provider_catalog.hpp"
#include "core/ai/skills.hpp"
#include "core/ai/standalone_chat.hpp"
#include "core/auth/auth_store.hpp"
#include "core/infra/event_bus.hpp"
#include "core/mcp/mcp_client.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "qt/ai/qt_ai_domain.hpp"
#include "qt/ai/qt_chat_inject.hpp"
#include "qt/bridge/settings_bridge.hpp"
#include "qt/chrome/aida_chrome_composer.hpp"
#include "qt/theme/aida_tokens.hpp"

extern settings_sa_t g_sa_settings;

namespace aida::qt::ai {

namespace {

bridge::QtSettingsBridge* settingsBridge() {
    auto* composer = chrome::chromeComposer();
    return composer ? composer->settings() : nullptr;
}

QString compose_provider_display_name(const std::string& provider_id) {
    if (provider_id.empty())
        return {};
    const auto* prov = aida::provider::catalog::get_provider(provider_id);
    if (prov != nullptr && !prov->name.empty())
        return QString::fromStdString(prov->name);
    return QString::fromStdString(provider_id);
}

QString compose_model_label_for(const std::string& provider_id, const std::string& model_id) {
    if (provider_id.empty() || model_id.empty())
        return QStringLiteral("Select model");
    const auto* model = aida::provider::catalog::get_model(provider_id, model_id);
    const QString m_disp = (model != nullptr && !model->name.empty())
        ? QString::fromStdString(model->name) : QString::fromStdString(model_id);
    const QString p_disp = compose_provider_display_name(provider_id);
    if (p_disp.isEmpty() || p_disp == m_disp)
        return m_disp;
    return p_disp + QStringLiteral("  -  ") + m_disp;
}

QString format_cost_brief(double in_per_million, double out_per_million) {
    if (in_per_million <= 0.0 && out_per_million <= 0.0)
        return {};
    return QStringLiteral("$%1 / $%2").arg(in_per_million, 0, 'f', 2)
        .arg(out_per_million, 0, 'f', 2);
}

QString format_context_brief(std::int64_t ctx) {
    if (ctx <= 0)
        return {};
    if (ctx >= 1000000)
        return QStringLiteral("%1M ctx").arg(static_cast<double>(ctx) / 1000000.0, 0, 'f', 1);
    if (ctx >= 1000)
        return QStringLiteral("%1K ctx").arg(ctx / 1000);
    return QStringLiteral("%1 ctx").arg(ctx);
}

bool provider_is_authenticated(const std::string& provider_id) {
    if (provider_id.empty())
        return false;
    aida::auth::auth_info_t info;
    if (!aida::auth::store::get(provider_id, info) || info.kind == aida::auth::auth_kind_t::none)
        return false;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    if (info.kind == aida::auth::auth_kind_t::oauth) {
        if (info.expires_unix > 0 && info.expires_unix <= now)
            return false;
        return !info.access.empty();
    }
    if (info.kind == aida::auth::auth_kind_t::api)
        return !info.api_key.empty();
    if (info.kind == aida::auth::auth_kind_t::wellknown)
        return !info.wellknown_token.empty();
    return false;
}

} // namespace

AidaPillPopup::AidaPillPopup(QWidget* anchor, QWidget* content)
    : QDialog(anchor, Qt::Popup | Qt::FramelessWindowHint), anchor_(anchor) {
    setObjectName(QStringLiteral("aida.ai.pill.popup"));
    setProperty("aidaRole", QStringLiteral("dialog"));
    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(t.panel.padding, t.panel.padding,
                               t.panel.padding, t.panel.padding);
    layout->addWidget(content);
    setLayout(layout);
}

void AidaPillPopup::openUnderAnchor() {
    if (!anchor_)
        return;
    const auto& t = theme::tokens();
    const QRect anchor_rect(anchor_->mapToGlobal(QPoint(0, 0)), anchor_->size());
    QScreen* screen = anchor_->screen();
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry()
                                   : QRect(anchor_rect.topLeft(), QSize(1920, 1080));
    adjustSize();
    const QSize hint = sizeHint().expandedTo(QSize(320, 140))
        .boundedTo(QSize((std::max)(available.width() - t.spacing.lg, 200),
                         (std::max)(available.height() - t.spacing.lg, 120)));
    resize(hint);
    const int gap = t.spacing.xs;
    const int space_below = available.bottom() - anchor_rect.bottom() - gap - t.spacing.sm;
    const int space_above = anchor_rect.top() - gap - t.spacing.sm - available.top();
    const bool lower_half = anchor_rect.center().y() > available.center().y();
    const bool flip_up = space_below < hint.height() ||
        (lower_half && space_above >= hint.height());
    int y = flip_up ? anchor_rect.top() - gap - hint.height()
                    : anchor_rect.bottom() + gap;
    y = (std::max)(available.top() + t.spacing.xs,
                   (std::min)(y, available.bottom() - hint.height() - t.spacing.xs));
    int x = anchor_rect.left();
    x = (std::max)(available.left() + t.spacing.xs,
                   (std::min)(x, available.right() - hint.width() - t.spacing.xs));
    move(x, y);
    show();
    raise();
}

AidaModelPill::AidaModelPill(QWidget* parent) : QToolButton(parent) {
    setObjectName(QStringLiteral("aida.ai.pill.model"));
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setAutoRaise(false);
    setCursor(Qt::PointingHandCursor);
    connect(this, &QToolButton::clicked, this, &AidaModelPill::openPopup);
    refresh();
}

QString AidaModelPill::providerId() const {
    return QString::fromStdString(
        g_sa_settings.ai_runtime_snapshot().selected_provider_id());
}

QString AidaModelPill::modelId() const {
    return QString::fromStdString(
        g_sa_settings.ai_runtime_snapshot().selected_model_id());
}

bool AidaModelPill::hasSelection() const {
    return !providerId().isEmpty() && !modelId().isEmpty();
}

bool AidaModelPill::authed() const {
    return hasSelection() && provider_is_authenticated(providerId().toStdString());
}

QString AidaModelPill::pillLabel() const {
    return compose_model_label_for(providerId().toStdString(), modelId().toStdString());
}

QString AidaModelPill::tooltipText() const {
    const QString provider = providerId().isEmpty() ? QStringLiteral("none") : providerId();
    const QString model = modelId().isEmpty() ? QStringLiteral("none") : modelId();
    const char* auth_label = !hasSelection() ? "no selection"
        : authed() ? "authenticated" : "not signed in";
    return QStringLiteral("Provider: %1\nModel: %2\nAuth: %3\nClick to change")
        .arg(provider, model, QString::fromLatin1(auth_label));
}

void AidaModelPill::refresh() {
    const QString label = pillLabel();
    const int max_w = theme::scale_logical(theme::tokens().shell.min_panel_w, 2.0);
    setText(fontMetrics().elidedText(label, Qt::ElideRight, max_w));
    setToolTip(tooltipText());
}

bool AidaModelPill::event(QEvent* event) {
    if (event->type() == QEvent::Show || event->type() == QEvent::WindowActivate)
        refresh();
    return QToolButton::event(event);
}

void AidaModelPill::openPopup() {
    const auto settings = g_sa_settings.ai_runtime_snapshot();
    const std::string current_provider = settings.selected_provider_id();
    const std::string current_model = settings.selected_model_id();

    const auto& providers = aida::provider::catalog::list_providers();
    std::vector<const aida::provider::provider_info_t*> selectable;
    selectable.reserve(providers.size());
    for (const auto& p : providers) {
        if (p.model_ids.empty())
            continue;
        bool has_active_model = false;
        for (const auto& mid : p.model_ids) {
            const auto* m = aida::provider::catalog::get_model(p.id, mid);
            if (m == nullptr)
                continue;
            if (m->status == aida::provider::model_info_t::status_t::deprecated)
                continue;
            has_active_model = true;
            break;
        }
        if (has_active_model)
            selectable.push_back(&p);
    }
    std::vector<const aida::provider::provider_info_t*> authenticated;
    for (const auto* p : selectable) {
        if (provider_is_authenticated(p->id))
            authenticated.push_back(p);
    }

    std::string active_provider = current_provider;
    bool active_present = false;
    for (const auto* p : authenticated) {
        if (p->id == active_provider) {
            active_present = true;
            break;
        }
    }
    if (!active_present)
        active_provider = authenticated.front()->id;

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::tokens().spacing.sm);
    auto* provider_strip = new QHBoxLayout();
    provider_strip->setSpacing(theme::tokens().spacing.sm);
    auto* list = new QListWidget(content);
    list->setObjectName(QStringLiteral("aida.ai.pill.model_list"));
    auto* filter = new QLineEdit(content);
    filter->setObjectName(QStringLiteral("aida.ai.pill.model_filter"));
    filter->setPlaceholderText(QStringLiteral("Search models..."));
    filter->setClearButtonEnabled(true);

    const aida::provider::provider_info_t* active_p = nullptr;
    for (const auto* p : selectable) {
        if (p->id == active_provider) {
            active_p = p;
            break;
        }
    }

    auto* popup = new AidaPillPopup(this, content);
    popup->setAttribute(Qt::WA_DeleteOnClose);

    if (authenticated.empty()) {
        auto* row = new QWidget(content);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        auto* label = new QLabel(QStringLiteral("No providers signed in yet"), row);
        label->setProperty("aidaVariant", QStringLiteral("secondary"));
        auto* sign_in = new QToolButton(row);
        sign_in->setObjectName(QStringLiteral("aida.ai.pill.sign_in"));
        sign_in->setText(QStringLiteral("Sign in a provider"));
        QObject::connect(sign_in, &QToolButton::clicked, content, [this, popup] {
            popup->close();
            Q_EMIT openSettingsForProvider(QString());
        });
        row_layout->addWidget(label, 1);
        row_layout->addWidget(sign_in);
        layout->addWidget(row);
        popup->openUnderAnchor();
        return;
    }

    auto reload_models = [list, filter, active_p, current_provider, current_model, popup]() {
        list->clear();
        if (!active_p)
            return;
        const QString needle = filter->text().toLower();
        for (const auto& mid : active_p->model_ids) {
            const auto* m = aida::provider::catalog::get_model(active_p->id, mid);
            if (!m || m->status == aida::provider::model_info_t::status_t::deprecated)
                continue;
            if (!needle.isEmpty() &&
                !QString::fromStdString(m->name).toLower().contains(needle) &&
                !QString::fromStdString(m->id).toLower().contains(needle))
                continue;
            QString meta;
            const QString ctx = format_context_brief(m->limit.context);
            const QString cost = format_cost_brief(m->cost.input_per_million,
                                                   m->cost.output_per_million);
            if (!ctx.isEmpty())
                meta = ctx;
            if (!cost.isEmpty())
                meta += (meta.isEmpty() ? QString() : QStringLiteral("  ")) + cost;
            const QString label = meta.isEmpty()
                ? QString::fromStdString(m->name)
                : QStringLiteral("%1    %2").arg(QString::fromStdString(m->name), meta);
            auto* item = new QListWidgetItem(label, list);
            QString tip = QString::fromStdString(m->name);
            if (!meta.isEmpty())
                tip += QStringLiteral("\n%1").arg(meta);
            if (m->capabilities.reasoning)
                tip += QStringLiteral("\nreasoning");
            item->setToolTip(tip);
            item->setData(Qt::UserRole, QString::fromStdString(m->id));
            if (current_provider == active_p->id && current_model == m->id)
                item->setSelected(true);
            list->addItem(item);
        }
    };
    reload_models();

    for (const auto* p : authenticated) {
        const QString label = QString::fromStdString(p->name.empty() ? p->id : p->name);
        auto* chip = new QToolButton(content);
        chip->setObjectName(QStringLiteral("aida.ai.pill.provider_chip.%1")
            .arg(QString::fromStdString(p->id)));
        chip->setText(label);
        chip->setCheckable(true);
        chip->setChecked(p->id == active_provider);
        chip->setToolTip(QStringLiteral("%1\nauthenticated").arg(label));
        const std::string pid = p->id;
        QObject::connect(chip, &QToolButton::clicked, content,
                         [pid, popup, this](bool) {
            auto& domain = ai_domain();
            Q_UNUSED(domain);
            if (auto* bridge = settingsBridge())
                bridge->mutate(bridge::QtSettingsBridge::section_t::providers,
                               [pid](settings_sa_t& s) {
                        s.default_provider_id = pid;
                    }, "model_pill_provider");
            popup->close();
        });
        provider_strip->addWidget(chip);
    }
    layout->addLayout(provider_strip);
    layout->addWidget(filter);
    layout->addWidget(list, 1);

    QObject::connect(filter, &QLineEdit::textChanged, content,
                     [reload_models](const QString&) { reload_models(); });
    QObject::connect(list, &QListWidget::itemActivated, content,
                     [active_p, popup, this](QListWidgetItem* item) {
        if (!active_p || !item)
            return;
        const std::string mid = item->data(Qt::UserRole).toString().toStdString();
        const std::string pid = active_p->id;
        const bool saved = settingsBridge()
            ? settingsBridge()->mutate(
                  bridge::QtSettingsBridge::section_t::providers,
                  [pid, mid](settings_sa_t& s) {
                      s.preferred_model_per_provider[pid] = mid;
                      s.set_selection(pid, mid);
                      if (auto* prof = s.get_active_profile()) {
                          prof->model = mid;
                          s.sync_legacy_fields_from_active_profile();
                      }
                  }, "model_pill_model")
            : false;
        if (saved) {
            aida::events::model_changed_t evt;
            evt.session_id.clear();
            evt.provider_id = pid;
            evt.model_id = mid;
            aida::events::publish(aida::events::event_model_changed, evt);
            refresh();
            popup->close();
        }
    });
    QObject::connect(list, &QListWidget::itemClicked, content,
                     [list](QListWidgetItem* item) {
        list->setCurrentItem(item);
        Q_EMIT list->itemActivated(item);
    });

    filter->setFocus(Qt::PopupFocusReason);
    popup->openUnderAnchor();
}

AidaAgentPill::AidaAgentPill(QWidget* parent) : QToolButton(parent) {
    setObjectName(QStringLiteral("aida.ai.pill.agent"));
    setCursor(Qt::PointingHandCursor);
    connect(this, &QToolButton::clicked, this, [] {
        (void)aida::ui::application_ui::execute_action(
            "ai.agent_picker.toggle", aida::ui::action_invocation_source_t::toolbar);
    });
    refresh();
}

QString AidaAgentPill::pillLabel() const {
    const std::string active = aida::agent::active_agent_name();
    if (active == "plan")
        return QStringLiteral("PLAN");
    if (active == "build")
        return QStringLiteral("BUILD");
    return active.empty() ? QStringLiteral("agent") : QString::fromStdString(active);
}

QString AidaAgentPill::tooltipText() const {
    const auto picker = aida::ui::application_ui::present_action("ai.agent_picker.toggle");
    const auto mode = aida::ui::application_ui::present_action("ai.agent_mode.toggle_plan_build");
    const QString picker_hint = picker.shortcut.empty()
        ? QStringLiteral("unbound") : QString::fromStdString(picker.shortcut);
    const QString mode_hint = mode.shortcut.empty()
        ? QStringLiteral("unbound") : QString::fromStdString(mode.shortcut);
    return QStringLiteral("Active agent: %1\nClick to switch  |  %2 to toggle plan/build  |  %3 to open picker")
        .arg(QString::fromStdString(aida::agent::active_agent_name()), mode_hint, picker_hint);
}

void AidaAgentPill::refresh() {
    setText(pillLabel());
    setToolTip(tooltipText());
}

bool AidaAgentPill::event(QEvent* event) {
    if (event->type() == QEvent::Show || event->type() == QEvent::WindowActivate)
        refresh();
    return QToolButton::event(event);
}

AidaSkillsPill::AidaSkillsPill(QWidget* parent) : QToolButton(parent) {
    setObjectName(QStringLiteral("aida.ai.pill.skills"));
    setText(QStringLiteral("Skills"));
    setToolTip(QStringLiteral("Insert a /skill command"));
    setCursor(Qt::PointingHandCursor);
    connect(this, &QToolButton::clicked, this, &AidaSkillsPill::openPopup);
}

bool AidaSkillsPill::event(QEvent* event) {
    return QToolButton::event(event);
}

void AidaSkillsPill::openPopup() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::tokens().spacing.sm);
    auto* filter = new QLineEdit(content);
    filter->setObjectName(QStringLiteral("aida.ai.pill.skill_filter"));
    filter->setPlaceholderText(QStringLiteral("Search skills..."));
    filter->setClearButtonEnabled(true);
    auto* list = new QListWidget(content);
    list->setObjectName(QStringLiteral("aida.ai.pill.skill_list"));
    layout->addWidget(filter);
    layout->addWidget(list, 1);

    auto* popup = new AidaPillPopup(this, content);
    popup->setAttribute(Qt::WA_DeleteOnClose);

    const std::string active_agent = aida::agent::active_agent_name();
    auto reload = [list, filter, active_agent, popup]() {
        list->clear();
        const QString needle = filter->text().toLower();
        const auto skills_all = aida::skills::all();
        for (const auto& sk : skills_all) {
            if (!aida::skills::is_enabled(sk.name))
                continue;
            if (!sk.agent_slugs.empty()) {
                bool ok = false;
                for (const auto& slug : sk.agent_slugs) {
                    if (slug == active_agent) {
                        ok = true;
                        break;
                    }
                }
                if (!ok)
                    continue;
            }
            if (!needle.isEmpty() &&
                !QString::fromStdString(sk.name).toLower().contains(needle) &&
                !QString::fromStdString(sk.description).toLower().contains(needle))
                continue;
            QString desc = QString::fromStdString(sk.description);
            if (desc.size() > 72) {
                desc.truncate(69);
                desc += QStringLiteral("...");
            }
            auto* item = new QListWidgetItem(
                desc.isEmpty() ? QStringLiteral("/") + QString::fromStdString(sk.name)
                               : QStringLiteral("/%1    %2").arg(
                                     QString::fromStdString(sk.name), desc),
                list);
            item->setToolTip(QStringLiteral("/%1\n%2").arg(
                QString::fromStdString(sk.name), QString::fromStdString(sk.description)));
            item->setData(Qt::UserRole, QString::fromStdString(sk.name));
            list->addItem(item);
        }
        if (list->count() == 0) {
            auto* empty = new QListWidgetItem(needle.isEmpty()
                ? QStringLiteral("No skills available for this agent")
                : QStringLiteral("No matching skills"), list);
            empty->setFlags(Qt::NoItemFlags);
            list->addItem(empty);
        }
    };
    reload();

    QObject::connect(filter, &QLineEdit::textChanged, content,
                     [reload](const QString&) { reload(); });
    auto pick = [list, popup](QListWidgetItem* item) {
        if (!item || !(item->flags() & Qt::ItemIsSelectable))
            return;
        const QString name = item->data(Qt::UserRole).toString();
        if (name.isEmpty())
            return;
        AidaChatInjectBridge::instance().post(
            (QStringLiteral("/") + name + QStringLiteral(" ")).toStdString());
        popup->close();
    };
    QObject::connect(list, &QListWidget::itemActivated, content, pick);
    QObject::connect(list, &QListWidget::itemClicked, content, pick);

    filter->setFocus(Qt::PopupFocusReason);
    popup->openUnderAnchor();
}

AidaMcpPill::AidaMcpPill(QWidget* parent) : QToolButton(parent) {
    setObjectName(QStringLiteral("aida.ai.pill.mcp"));
    setCursor(Qt::PointingHandCursor);
    connect(this, &QToolButton::clicked, this, &AidaMcpPill::openPopup);
    refresh();
}

QString AidaMcpPill::pillLabel() const {
    const auto statuses = get_mcp_client_manager().get_status();
    std::size_t connected = 0;
    for (const auto& s : statuses) {
        if (s.state == mcp_client::connection_state_t::connected)
            ++connected;
    }
    if (statuses.empty())
        return QStringLiteral("MCP");
    return QStringLiteral("MCP %1/%2").arg(connected).arg(statuses.size());
}

QString AidaMcpPill::tooltipText() const {
    const auto statuses = get_mcp_client_manager().get_status();
    std::size_t connected = 0;
    std::size_t tools_count = 0;
    for (const auto& s : statuses) {
        if (s.state == mcp_client::connection_state_t::connected) {
            ++connected;
            tools_count += s.tool_count;
        }
    }
    return QStringLiteral("MCP servers connected: %1 / %2\nTotal remote tools: %3\nClick to view details")
        .arg(connected).arg(statuses.size()).arg(tools_count);
}

void AidaMcpPill::refresh() {
    setText(pillLabel());
    setToolTip(tooltipText());
}

bool AidaMcpPill::event(QEvent* event) {
    if (event->type() == QEvent::Show || event->type() == QEvent::WindowActivate)
        refresh();
    return QToolButton::event(event);
}

void AidaMcpPill::openPopup() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::tokens().spacing.xs);
    auto* title = new QLabel(QStringLiteral("MCP servers"), content);
    title->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(title);
    auto* list = new QListWidget(content);
    list->setObjectName(QStringLiteral("aida.ai.pill.mcp_list"));
    layout->addWidget(list, 1);

    const auto statuses = get_mcp_client_manager().get_status();
    if (statuses.empty()) {
        auto* item = new QListWidgetItem(QStringLiteral("No MCP servers configured"), list);
        item->setFlags(Qt::NoItemFlags);
        list->addItem(item);
    } else {
        for (const auto& s : statuses) {
            const char* state_label = "?";
            switch (s.state) {
            case mcp_client::connection_state_t::connected: state_label = "online"; break;
            case mcp_client::connection_state_t::connecting: state_label = "connecting"; break;
            case mcp_client::connection_state_t::reconnecting: state_label = "reconnecting"; break;
            case mcp_client::connection_state_t::disconnected: state_label = "offline"; break;
            case mcp_client::connection_state_t::error: state_label = "error"; break;
            default: break;
            }
            auto* item = new QListWidgetItem(
                QStringLiteral("%1  |  %2  |  %3 tools")
                    .arg(QString::fromStdString(s.name),
                         QString::fromLatin1(state_label),
                         QString::number(s.tool_count)),
                list);
            item->setToolTip(QStringLiteral("%1 (%2)").arg(
                QString::fromStdString(s.name), QString::fromLatin1(state_label)));
            item->setFlags(Qt::NoItemFlags);
            list->addItem(item);
        }
    }

    auto* popup = new AidaPillPopup(this, content);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->openUnderAnchor();
}

}
