#include "qt/auth/qt_auth_oauth_dialog.hpp"

#include <QCloseEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedLayout>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <optional>
#include <string>

#include "qt/auth/qt_auth_view.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::auth {

namespace {

void repolish_variant(QWidget* widget) {
    if (!widget)
        return;
    if (QStyle* style = widget->style()) {
        style->unpolish(widget);
        style->polish(widget);
    }
}

enum class flow_phase_t : int {
    idle = 0,
    browser,
    callback,
    session,
    complete,
    error_state
};

const char* flow_phase_label(flow_phase_t phase) {
    switch (phase) {
    case flow_phase_t::idle: return "Preparing";
    case flow_phase_t::browser: return "Browser authentication";
    case flow_phase_t::callback: return "Callback received";
    case flow_phase_t::session: return "Finalizing session";
    case flow_phase_t::complete: return "Connected";
    case flow_phase_t::error_state: return "Authentication failed";
    }
    return "Unknown";
}

double flow_phase_progress(flow_phase_t phase) {
    switch (phase) {
    case flow_phase_t::idle: return 0.05;
    case flow_phase_t::browser: return 0.25;
    case flow_phase_t::callback: return 0.60;
    case flow_phase_t::session: return 0.85;
    case flow_phase_t::complete: return 1.0;
    case flow_phase_t::error_state: return 0.0;
    }
    return 0.0;
}

flow_phase_t derive_phase_codex(const aida::auth::codex::codex_login_state_t* state,
                                bool success_played, bool exchange_in_progress) {
    if (success_played)
        return flow_phase_t::complete;
    if (!state)
        return flow_phase_t::idle;
    const auto value = aida::auth::codex::snapshot(*state);
    if (value.done && !exchange_in_progress && !value.error.empty())
        return flow_phase_t::error_state;
    if (value.done)
        return flow_phase_t::session;
    if (!value.received_code.empty())
        return flow_phase_t::callback;
    return flow_phase_t::browser;
}

flow_phase_t derive_phase_claude(const aida::auth::claude_code::claude_code_login_state_t* state,
                                 bool success_played, bool exchange_in_progress) {
    if (success_played)
        return flow_phase_t::complete;
    if (!state)
        return flow_phase_t::idle;
    const auto value = aida::auth::claude_code::snapshot(*state);
    if (value.done && !exchange_in_progress && !value.error.empty())
        return flow_phase_t::error_state;
    if (value.done)
        return flow_phase_t::session;
    if (!value.received_code.empty())
        return flow_phase_t::callback;
    return flow_phase_t::browser;
}

flow_phase_t derive_phase_copilot(const aida::auth::copilot::copilot_login_state_t* state,
                                  bool success_played, bool poll_in_progress) {
    if (success_played)
        return flow_phase_t::complete;
    if (!state)
        return flow_phase_t::idle;
    const auto value = aida::auth::copilot::snapshot(*state);
    if (value.done && !poll_in_progress && !value.error.empty())
        return flow_phase_t::error_state;
    if (value.done)
        return flow_phase_t::session;
    if (!value.user_code.empty())
        return flow_phase_t::callback;
    return flow_phase_t::browser;
}

QString trimmed(const QString& value) {
    QString out = value;
    while (!out.isEmpty() &&
        (out.back() == QLatin1Char(' ') || out.back() == QLatin1Char('\t') ||
         out.back() == QLatin1Char('\r') || out.back() == QLatin1Char('\n')))
        out.chop(1);
    int first = 0;
    while (first < out.size() &&
        (out.at(first) == QLatin1Char(' ') || out.at(first) == QLatin1Char('\t')))
        ++first;
    if (first != 0)
        out.remove(0, first);
    return out;
}

}

AidaOAuthLoginDialog::AidaOAuthLoginDialog(Provider provider, QWidget* parent)
    : QDialog(parent), provider_(provider) {
    switch (provider) {
    case Provider::Codex: setWindowTitle(QStringLiteral("Sign in with OpenAI")); break;
    case Provider::ClaudeCode: setWindowTitle(QStringLiteral("Sign in with Claude")); break;
    case Provider::Copilot:
        setWindowTitle(QStringLiteral("Sign in with GitHub Copilot"));
        break;
    }
    setWindowModality(Qt::ApplicationModal);
    setObjectName(QStringLiteral("aida.auth.oauthDialog"));
    setProperty("aidaRole", "dialog");
    const auto& t = theme::tokens();
    const int base = t.shell.min_panel_w > 0 ? static_cast<int>(t.shell.min_panel_w) : 96;
    setMinimumSize(base * 4, base * 3);
    resize(base * 5 + t.spacing.section,
        provider == Provider::Copilot ? base * 5 : base * 4 + t.spacing.section);
    buildUi();
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(100);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, &AidaOAuthLoginDialog::pollFlow);
}

AidaOAuthLoginDialog::~AidaOAuthLoginDialog() = default;

void AidaOAuthLoginDialog::buildUi() {
    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.spacing.md, t.spacing.md, t.spacing.md, t.spacing.md);
    root->setSpacing(t.spacing.sm);

    auto* grid = new QFormLayout();
    grid->setRowWrapPolicy(QFormLayout::WrapLongRows);
    grid->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    provider_value_ = new QLabel(this);
    provider_value_->setObjectName(QStringLiteral("aida.auth.oauth.provider"));
    provider_value_->setWordWrap(true);
    flow_value_ = new QLabel(this);
    flow_value_->setObjectName(QStringLiteral("aida.auth.oauth.flow"));
    flow_value_->setWordWrap(true);
    third_label_ = new QLabel(this);
    third_label_->setObjectName(QStringLiteral("aida.auth.oauth.thirdLabel"));
    third_value_ = new QLabel(this);
    third_value_->setObjectName(QStringLiteral("aida.auth.oauth.thirdValue"));
    third_value_->setFont(theme::fonts::codeRegular());
    third_value_->setWordWrap(true);
    third_value_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addRow(QStringLiteral("Provider"), provider_value_);
    grid->addRow(QStringLiteral("Flow"), flow_value_);
    grid->addRow(third_label_, third_value_);
    root->addLayout(grid);

    switch (provider_) {
    case Provider::Codex:
        provider_value_->setText(QStringLiteral("OpenAI Codex"));
        flow_value_->setText(QStringLiteral("ChatGPT OAuth with PKCE"));
        third_label_->setText(QStringLiteral("Callback"));
        third_value_->setText(QStringLiteral("localhost:1455"));
        break;
    case Provider::ClaudeCode:
        provider_value_->setText(QStringLiteral("Claude Code"));
        flow_value_->setText(QStringLiteral("Anthropic OAuth with PKCE"));
        third_label_->setText(QStringLiteral("Scope"));
        third_value_->setText(QStringLiteral("Claude Code provider session"));
        break;
    case Provider::Copilot:
        provider_value_->setText(QStringLiteral("GitHub Copilot"));
        flow_value_->setText(QStringLiteral("GitHub device code"));
        third_label_->setText(QStringLiteral("Device code"));
        break;
    }

    if (provider_ == Provider::Copilot) {
        auto* stack_host = new QWidget(this);
        copilot_stack_ = new QStackedLayout(stack_host);

        auto* setup_page = new QWidget(stack_host);
        auto* setup = new QVBoxLayout(setup_page);
        setup->setContentsMargins(0, 0, 0, 0);
        setup->setSpacing(t.spacing.sm);
        auto* ghe_label = new QLabel(QStringLiteral("GitHub Enterprise URL"), setup_page);
        ghe_label->setObjectName(QStringLiteral("aida.auth.oauth.gheLabel"));
        ghe_label->setFont(theme::fonts::bodyEm());
        ghe_edit_ = new QLineEdit(setup_page);
        ghe_edit_->setObjectName(QStringLiteral("aida.auth.oauth.gheUrl"));
        ghe_edit_->setPlaceholderText(
            QStringLiteral("https://github.your-company.com (optional)"));
        ghe_edit_->setToolTip(QStringLiteral(
            "Absolute HTTPS URL of your GitHub Enterprise instance; leave empty for github.com"));
        ghe_edit_->setText(QString::fromStdString(
            AidaAuthViewModel::instance().copilotEnterpriseUrl()));
        ghe_error_ = new QLabel(setup_page);
        ghe_error_->setObjectName(QStringLiteral("aida.auth.oauth.gheError"));
        ghe_error_->setFont(theme::fonts::caption());
        ghe_error_->setProperty("aidaVariant", "error");
        ghe_error_->setWordWrap(true);
        ghe_error_->setVisible(false);
        auto* setup_hint = new QLabel(QStringLiteral(
            "Leave the URL empty to authenticate with github.com. Credentials remain owned "
            "by the existing encrypted provider store."), setup_page);
        setup_hint->setObjectName(QStringLiteral("aida.auth.oauth.gheHint"));
        setup_hint->setWordWrap(true);
        setup_hint->setFont(theme::fonts::caption());
        setup_hint->setProperty("aidaVariant", "secondary");
        setup->addWidget(ghe_label);
        setup->addWidget(ghe_edit_);
        setup->addWidget(ghe_error_);
        setup->addWidget(setup_hint);
        setup->addStretch(1);
        copilot_stack_->addWidget(setup_page);

        auto* flow_page = new QWidget(stack_host);
        auto* flow = new QVBoxLayout(flow_page);
        flow->setContentsMargins(0, 0, 0, 0);
        flow->setSpacing(t.spacing.sm);
        device_code_label_ = new QLabel(flow_page);
        device_code_label_->setObjectName(QStringLiteral("aida.auth.oauth.deviceCode"));
        device_code_label_->setFont(theme::fonts::codeRegular());
        device_code_label_->setWordWrap(true);
        device_code_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        copy_code_button_ = new QPushButton(QStringLiteral("Copy Device Code"), flow_page);
        copy_code_button_->setObjectName(QStringLiteral("aida.auth.oauth.copyCode"));
        copy_code_button_->setToolTip(QStringLiteral(
            "Copy the current device code to the clipboard"));
        flow->addWidget(device_code_label_);
        flow->addWidget(copy_code_button_, 0, Qt::AlignLeft);
        flow->addStretch(1);
        copilot_stack_->addWidget(flow_page);
        copilot_stack_->setCurrentIndex(0);
        root->addWidget(stack_host, 1);
        connect(copy_code_button_, &QPushButton::clicked, this, [this] {
            const QString code = device_code_label_->text();
            if (code.isEmpty())
                return;
            clipboard::set_text(code);
            chrome::toast_info(QStringLiteral("Code copied"), 2.5);
        });
    } else {
        root->addStretch(1);
    }

    phase_label_ = new QLabel(this);
    phase_label_->setObjectName(QStringLiteral("aida.auth.oauth.phase"));
    phase_label_->setFont(theme::fonts::bodyEm());
    phase_label_->setWordWrap(true);
    phase_bar_ = new QProgressBar(this);
    phase_bar_->setObjectName(QStringLiteral("aida.auth.oauth.phaseBar"));
    phase_bar_->setRange(0, 1000);
    phase_bar_->setTextVisible(true);
    root->addWidget(phase_label_);
    root->addWidget(phase_bar_);

    auto* footer = new QHBoxLayout();
    footer->setSpacing(t.spacing.sm);
    confirm_button_ = new QPushButton(QStringLiteral("Open Browser"), this);
    confirm_button_->setObjectName(QStringLiteral("aida.auth.oauth.confirm"));
    confirm_button_->setProperty("aidaVariant", "primary");
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_button_->setObjectName(QStringLiteral("aida.auth.oauth.cancel"));
    footer->addStretch(1);
    footer->addWidget(confirm_button_);
    footer->addWidget(cancel_button_);
    root->addLayout(footer);

    connect(confirm_button_, &QPushButton::clicked, this,
            &AidaOAuthLoginDialog::onConfirm);
    connect(cancel_button_, &QPushButton::clicked, this, &AidaOAuthLoginDialog::reject);

    refreshPhase();
}

void AidaOAuthLoginDialog::showFor(Provider provider, QWidget* parent) {
    static AidaOAuthLoginDialog* dialogs[3] = {};
    const int index = static_cast<int>(provider);
    if (dialogs[index] == nullptr)
        dialogs[index] = new AidaOAuthLoginDialog(provider, parent);
    AidaOAuthLoginDialog* dialog = dialogs[index];
    if (dialog->isVisible()) {
        dialog->raise();
        dialog->activateWindow();
        return;
    }
    dialog->refreshPhase();
    dialog->open();
}

void AidaOAuthLoginDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (provider_ == Provider::Copilot)
        ghe_edit_->setText(QString::fromStdString(
            AidaAuthViewModel::instance().copilotEnterpriseUrl()));
    poll_timer_->start();
    refreshPhase();
}

void AidaOAuthLoginDialog::hideEvent(QHideEvent* event) {
    poll_timer_->stop();
    QDialog::hideEvent(event);
}

void AidaOAuthLoginDialog::pollFlow() {
    auto& model = AidaAuthViewModel::instance();
    const bool open = provider_ == Provider::Codex ? model.codexFlowOpen()
        : provider_ == Provider::ClaudeCode ? model.claudeFlowOpen()
        : model.copilotFlowOpen();
    if (!open) {
        close();
        return;
    }
    refreshPhase();
}

void AidaOAuthLoginDialog::refreshPhase() {
    auto& model = AidaAuthViewModel::instance();
    flow_phase_t phase = flow_phase_t::idle;
    QString error;
    switch (provider_) {
    case Provider::Codex: {
        auto state = model.codexState();
        phase = derive_phase_codex(state.get(), model.codexSuccessPlayed(),
            model.codexExchangeInFlight());
        if (state) {
            const auto snapshot = aida::auth::codex::snapshot(*state);
            if (!model.codexStartActive())
                auth_url_ = QString::fromStdString(snapshot.auth_url);
            if (snapshot.done && !model.codexExchangeInFlight())
                error = QString::fromStdString(snapshot.error);
        }
        break;
    }
    case Provider::ClaudeCode: {
        auto state = model.claudeCodeState();
        phase = derive_phase_claude(state.get(), model.claudeSuccessPlayed(),
            model.claudeExchangeInFlight());
        if (state) {
            const auto snapshot = aida::auth::claude_code::snapshot(*state);
            if (!model.claudeStartActive())
                auth_url_ = QString::fromStdString(snapshot.auth_url);
            if (snapshot.done && !model.claudeExchangeInFlight())
                error = QString::fromStdString(snapshot.error);
        }
        break;
    }
    case Provider::Copilot: {
        auto state = model.copilotState();
        phase = derive_phase_copilot(state.get(), model.copilotSuccessPlayed(),
            model.copilotPollInFlight());
        const bool flow_started = model.copilotFlowStarted();
        if (copilot_stack_ != nullptr)
            copilot_stack_->setCurrentIndex(flow_started ? 1 : 0);
        if (state && flow_started) {
            const auto snapshot = aida::auth::copilot::snapshot(*state);
            if (!model.copilotStartActive()) {
                device_code_label_->setText(QString::fromStdString(snapshot.user_code));
                verification_uri_ = QString::fromStdString(snapshot.verification_uri);
            }
            if (snapshot.done && !model.copilotPollInFlight())
                error = QString::fromStdString(snapshot.error);
        }
        break;
    }
    }
    complete_ = phase == flow_phase_t::complete;
    const char* variant = "neutral";
    if (!error.isEmpty()) {
        phase_label_->setText(QStringLiteral("%1 - %2")
            .arg(QString::fromLatin1(flow_phase_label(flow_phase_t::error_state)), error));
        phase_bar_->setValue(0);
        variant = "error";
    } else {
        phase_label_->setText(QString::fromLatin1(flow_phase_label(phase)));
        phase_bar_->setValue(static_cast<int>(flow_phase_progress(phase) * 1000.0));
        variant = complete_ ? "success" : "neutral";
    }
    if (phase_label_->property("aidaVariant").toString() != QLatin1String(variant)) {
        phase_label_->setProperty("aidaVariant", variant);
        repolish_variant(phase_label_);
    }
    if (provider_ == Provider::Copilot) {
        const bool flow_started = model.copilotFlowStarted();
        confirm_button_->setText(!flow_started ? QStringLiteral("Start Login")
            : complete_ ? QStringLiteral("Close")
            : QStringLiteral("Open Verification"));
        confirm_button_->setEnabled(!flow_started || complete_ ||
            !verification_uri_.isEmpty());
        cancel_button_->setText(flow_started ? QStringLiteral("Cancel")
                                             : QStringLiteral("Cancel"));
    } else {
        confirm_button_->setText(complete_ ? QStringLiteral("Close")
                                           : QStringLiteral("Open Browser"));
        confirm_button_->setEnabled(complete_ || !auth_url_.isEmpty());
    }
}

void AidaOAuthLoginDialog::onConfirm() {
    auto& model = AidaAuthViewModel::instance();
    if (provider_ == Provider::Copilot && !model.copilotFlowStarted()) {
        const QString enterprise = trimmed(ghe_edit_->text());
        if (!enterprise.isEmpty() &&
            (!enterprise.startsWith(QStringLiteral("https://")) ||
             enterprise.contains(QLatin1Char(' ')) || enterprise.contains(QLatin1Char('\t')) ||
             enterprise.contains(QLatin1Char('\r')) || enterprise.contains(QLatin1Char('\n')))) {
            ghe_error_->setText(QStringLiteral(
                "Use an absolute HTTPS URL without whitespace."));
            ghe_error_->setVisible(true);
            return;
        }
        ghe_error_->setVisible(false);
        const std::string normalized = enterprise.toStdString();
        model.setCopilotEnterpriseUrl(normalized);
        model.startCopilotFlow(normalized.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{normalized});
        refreshPhase();
        return;
    }
    if (complete_) {
        switch (provider_) {
        case Provider::Codex: model.closeCodexFlow(); break;
        case Provider::ClaudeCode: model.closeClaudeCodeFlow(); break;
        case Provider::Copilot: model.closeCopilotFlow(); break;
        }
        accept();
        return;
    }
    const std::string url = provider_ == Provider::Copilot
        ? verification_uri_.toStdString() : auth_url_.toStdString();
    if (!url.empty())
        model.openUrlInBrowser(url);
}

void AidaOAuthLoginDialog::reject() {
    auto& model = AidaAuthViewModel::instance();
    switch (provider_) {
    case Provider::Codex: model.closeCodexFlow(); break;
    case Provider::ClaudeCode: model.closeClaudeCodeFlow(); break;
    case Provider::Copilot: model.closeCopilotFlow(); break;
    }
    if (ghe_edit_ != nullptr)
        ghe_edit_->clear();
    QDialog::reject();
}

void AidaOAuthLoginDialog::closeEvent(QCloseEvent* event) {
    auto& model = AidaAuthViewModel::instance();
    switch (provider_) {
    case Provider::Codex: model.closeCodexFlow(); break;
    case Provider::ClaudeCode: model.closeClaudeCodeFlow(); break;
    case Provider::Copilot: model.closeCopilotFlow(); break;
    }
    if (ghe_edit_ != nullptr)
        ghe_edit_->clear();
    QDialog::closeEvent(event);
}

}
