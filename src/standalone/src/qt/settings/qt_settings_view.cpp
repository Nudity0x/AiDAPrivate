#include "qt/settings/qt_settings_view.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include "core/settings/settings_persistence_service.hpp"
#include "helpers/diag_log.hpp"
#include "qt/ai/qt_ai_chat_view.hpp"
#include "qt/ai/qt_ai_domain.hpp"
#include "qt/auth/qt_auth_view.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/settings/qt_settings_editor_page.hpp"
#include "qt/settings/qt_settings_mcp_page.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::settings {

namespace {

constexpr int k_tab_accounts = 0;
constexpr int k_tab_agents = 1;
constexpr int k_tab_skills = 2;
constexpr int k_tab_mcp_servers = 3;
constexpr int k_tab_editor_theme = 4;

void repolish_variant(QWidget* widget) {
    if (!widget)
        return;
    if (QStyle* style = widget->style()) {
        style->unpolish(widget);
        style->polish(widget);
    }
}

QPointer<AidaSettingsView>& active_instance_slot() {
    static QPointer<AidaSettingsView> instance;
    return instance;
}

}

void AidaSettingsView::buildUi() {
    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.spacing.md, t.spacing.sm, t.spacing.md, t.spacing.sm);
    root->setSpacing(t.spacing.sm);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("Settings"), this);
    title->setObjectName(QStringLiteral("aida.view.settings.title"));
    title->setFont(theme::fonts::h1());
    header->addWidget(title, 1);
    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.view.settings.status"));
    status_label_->setFont(theme::fonts::caption());
    status_label_->setProperty("aidaVariant", "neutral");
    status_label_->setVisible(false);
    header->addWidget(status_label_, 0, Qt::AlignVCenter | Qt::AlignRight);
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(t.spacing.sm);
    sidebar_ = new QListWidget(this);
    sidebar_->setObjectName(QStringLiteral("aida.view.settings.sidebar"));
    sidebar_->addItem(QStringLiteral("Accounts"));
    sidebar_->addItem(QStringLiteral("Agents"));
    sidebar_->addItem(QStringLiteral("Skills"));
    sidebar_->addItem(QStringLiteral("MCP Servers"));
    sidebar_->addItem(QStringLiteral("Editor"));
    sidebar_->setTextElideMode(Qt::ElideRight);
    sidebar_->setMaximumWidth(t.row.property_label_w);
    sidebar_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    for (int row = 0; row < sidebar_->count(); ++row)
        sidebar_->item(row)->setToolTip(sidebar_->item(row)->text());
    sidebar_->setCurrentRow(k_tab_accounts);
    body->addWidget(sidebar_);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("aida.view.settings.pages"));
    accounts_page_ = new auth::AidaAuthView(pages_);
    pages_->addWidget(accounts_page_);

    auto* agents_page = new QWidget(pages_);
    auto* agents_layout = new QVBoxLayout(agents_page);
    agents_layout->setContentsMargins(0, 0, 0, 0);
    agents_layout->setSpacing(t.spacing.sm);
    auto* agents_hint = new QLabel(QStringLiteral(
        "Agents are managed in their own dock view."), agents_page);
    agents_hint->setObjectName(QStringLiteral("aida.view.settings.agentsHint"));
    agents_hint->setFont(theme::fonts::caption());
    agents_hint->setProperty("aidaVariant", "secondary");
    agents_hint->setWordWrap(true);
    auto* open_agents = new QPushButton(QStringLiteral("Open Agents"), agents_page);
    open_agents->setObjectName(QStringLiteral("aida.view.settings.openAgents"));
    open_agents->setToolTip(QStringLiteral("Open the Agents dock view"));
    agents_layout->addWidget(agents_hint);
    agents_layout->addWidget(open_agents, 0, Qt::AlignLeft);
    agents_layout->addStretch(1);
    pages_->addWidget(agents_page);
    connect(open_agents, &QPushButton::clicked, this,
            [] { ai::open_ai_view("view.ai.agents"); });

    auto* skills_page = new QWidget(pages_);
    auto* skills_layout = new QVBoxLayout(skills_page);
    skills_layout->setContentsMargins(0, 0, 0, 0);
    skills_layout->setSpacing(t.spacing.sm);
    auto* skills_hint = new QLabel(QStringLiteral(
        "Skills are managed in their own dock view."), skills_page);
    skills_hint->setObjectName(QStringLiteral("aida.view.settings.skillsHint"));
    skills_hint->setFont(theme::fonts::caption());
    skills_hint->setProperty("aidaVariant", "secondary");
    skills_hint->setWordWrap(true);
    auto* open_skills = new QPushButton(QStringLiteral("Open Skills"), skills_page);
    open_skills->setObjectName(QStringLiteral("aida.view.settings.openSkills"));
    open_skills->setToolTip(QStringLiteral("Open the Skills dock view"));
    skills_layout->addWidget(skills_hint);
    skills_layout->addWidget(open_skills, 0, Qt::AlignLeft);
    skills_layout->addStretch(1);
    pages_->addWidget(skills_page);
    connect(open_skills, &QPushButton::clicked, this,
            [] { ai::open_ai_view("view.ai.skills"); });

    mcp_page_ = new AidaSettingsMcpPage(pages_);
    pages_->addWidget(mcp_page_);
    editor_page_ = new AidaSettingsEditorPage(pages_);
    pages_->addWidget(editor_page_);

    body->addWidget(pages_, 1);
    root->addLayout(body, 1);

    connect(sidebar_, &QListWidget::currentRowChanged, pages_,
            &QStackedWidget::setCurrentIndex);

    status_timer_ = new QTimer(this);
    status_timer_->setInterval(500);
    connect(status_timer_, &QTimer::timeout, this,
            &AidaSettingsView::refreshPersistenceStatus);
}

AidaSettingsView::AidaSettingsView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.settings"));
    buildUi();
    active_instance_slot() = this;
}

AidaSettingsView::~AidaSettingsView() {
    if (active_instance_slot() == this)
        active_instance_slot().clear();
}

AidaSettingsView* AidaSettingsView::activeInstance() {
    return active_instance_slot().data();
}

void AidaSettingsView::setActiveTab(int tab_index) {
    if (tab_index < 0 || tab_index >= pages_->count())
        return;
    sidebar_->setCurrentRow(tab_index);
}

void AidaSettingsView::openToProvider(const QString& provider_id) {
    setActiveTab(k_tab_accounts);
    accounts_page_->focusProvider(provider_id);
}

void AidaSettingsView::refreshPersistenceStatus() {
    const auto status = aida::settings_persistence::status();
    const char* variant = nullptr;
    if (status.failed) {
        status_label_->setText(QStringLiteral("Settings save failed"));
        status_label_->setToolTip(QString::fromStdString(
            status.error.empty() ? status.stage : status.error));
        variant = "error";
    } else if (status.pending) {
        status_label_->setText(QStringLiteral("Saving settings..."));
        status_label_->setToolTip(QString::fromStdString(status.stage));
        variant = "warning";
    }
    if (variant == nullptr) {
        status_label_->setVisible(false);
        return;
    }
    if (status_label_->property("aidaVariant").toString() !=
            QLatin1String(variant)) {
        status_label_->setProperty("aidaVariant", variant);
        repolish_variant(status_label_);
    }
    status_label_->setVisible(true);
}

void AidaSettingsView::applyCompactMode(bool compact) {
    if (compact_ == compact)
        return;
    compact_ = compact;
    const auto& t = theme::tokens();
    if (compact) {
        sidebar_->setMaximumWidth(t.shell.activity_bar_w > 0
            ? static_cast<int>(t.shell.activity_bar_w) : t.control.height_lg * 2);
        for (int row = 0; row < sidebar_->count(); ++row) {
            QListWidgetItem* item = sidebar_->item(row);
            item->setToolTip(item->text());
            item->setData(Qt::UserRole, item->text());
            item->setText(item->text().left(1));
            item->setTextAlignment(Qt::AlignCenter);
        }
    } else {
        sidebar_->setMaximumWidth(t.row.property_label_w);
        for (int row = 0; row < sidebar_->count(); ++row) {
            QListWidgetItem* item = sidebar_->item(row);
            item->setText(item->data(Qt::UserRole).toString());
            item->setToolTip(item->text());
            item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
}

void AidaSettingsView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const auto& t = theme::tokens();
    const int threshold = t.row.property_label_w + static_cast<int>(t.shell.min_panel_w)
        + t.spacing.section;
    applyCompactMode(width() < threshold);
}

void AidaSettingsView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshPersistenceStatus();
    if (!status_timer_->isActive())
        status_timer_->start();
}

void AidaSettingsView::hideEvent(QHideEvent* event) {
    status_timer_->stop();
    QWidget::hideEvent(event);
}

void install_settings_domain(docking::AidaDockHost* host) {
    if (!host)
        return;
    const auto result = host->install_view_factory(
        registry::stable_view_id_t("view.settings"),
        [](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
            return new AidaSettingsView(parent);
        });
    if (!result.ok())
        diag::log_tagged_fmt("qt_settings",
            "view_factory_install_failed view=%s status=%d detail=%s",
            "view.settings", static_cast<int>(result.status), result.detail.c_str());
    QObject::connect(&ai::AidaChatController::instance(),
        &ai::AidaChatController::openSettingsForProviderRequested,
        &ai::AidaChatController::instance(), [](const QString& provider_id) {
            ai::open_ai_view("view.settings");
            if (auto* view = AidaSettingsView::activeInstance())
                view->openToProvider(provider_id);
        });
    diag::log_tagged("qt_settings", "settings_domain_installed");
}

}
