#include "qt/settings/qt_settings_mcp_page.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>

#include "core/ai/standalone_chat.hpp"
#include "core/mcp/mcp_client.hpp"
#include "core/mcp/mcp_marketplace.hpp"
#include "core/settings/settings_persistence_service.hpp"
#include "core/settings/standalone_settings.hpp"
#include "helpers/diag_log.hpp"
#include "qt/ai/qt_ai_chat_dialogs.hpp"
#include "qt/ai/qt_ai_domain.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_icons.hpp"
#include "qt/theme/aida_tokens.hpp"

extern settings_sa_t g_sa_settings;

namespace aida::qt::settings {

namespace {

void repolish_variant(QWidget* widget) {
    if (!widget)
        return;
    if (QStyle* style = widget->style()) {
        style->unpolish(widget);
        style->polish(widget);
    }
}

std::string trim_copy(const std::string& v) {
    size_t first = 0;
    while (first < v.size() && std::isspace(static_cast<unsigned char>(v[first])))
        ++first;
    size_t last = v.size();
    while (last > first && std::isspace(static_cast<unsigned char>(v[last - 1])))
        --last;
    return v.substr(first, last - first);
}

bool parse_mcp_args(const std::string& text, std::vector<std::string>& out,
                    std::string& error) {
    out.clear();
    error.clear();
    std::string cur;
    bool in_quote = false;
    char quote_char = 0;
    bool escaped = false;
    for (char c : text) {
        if (escaped) {
            cur.push_back(c);
            escaped = false;
            continue;
        }
        if (in_quote) {
            if (c == '\\') {
                escaped = true;
            } else if (c == quote_char) {
                in_quote = false;
                quote_char = 0;
            } else {
                cur.push_back(c);
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            in_quote = true;
            quote_char = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (escaped)
        cur.push_back('\\');
    if (in_quote) {
        error = "Unclosed quote in args";
        return false;
    }
    if (!cur.empty())
        out.push_back(cur);
    return true;
}

std::string quote_argv_preview(const std::string& arg) {
    if (arg.empty())
        return "\"\"";
    bool quote = false;
    for (char c : arg) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\'') {
            quote = true;
            break;
        }
    }
    if (!quote)
        return arg;
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"')
            out += "\\\"";
        else
            out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string make_argv_preview(const std::string& command,
                              const std::vector<std::string>& args) {
    std::string out = quote_argv_preview(command);
    for (const auto& arg : args) {
        out.push_back(' ');
        out += quote_argv_preview(arg);
    }
    return out;
}

bool build_mcp_client_config(const mcp_client_server_t& srv,
                             mcp_client::server_config_t& cfg, std::string& error) {
    error.clear();
    cfg = {};
    cfg.name = trim_copy(srv.name);
    cfg.url = trim_copy(srv.url);
    cfg.api_key = srv.api_key;
    cfg.enabled = srv.enabled;
    cfg.auto_connect = srv.enabled && srv.auto_connect;
    if (cfg.name.empty()) {
        error = "Server name is required";
        return false;
    }
    if (srv.transport == "stdio") {
        cfg.transport = mcp_client::transport_type_t::stdio;
        cfg.command = trim_copy(srv.command);
        if (cfg.command.empty()) {
            error = "Stdio command is required for " + cfg.name;
            return false;
        }
        if (!parse_mcp_args(srv.args, cfg.args, error)) {
            error = cfg.name + ": " + error;
            return false;
        }
    } else {
        cfg.transport = mcp_client::transport_type_t::http_sse;
        if (cfg.url.empty()) {
            error = "URL is required for " + cfg.name;
            return false;
        }
    }
    return true;
}

mcp_client::connection_state_t status_for_server(
    const std::vector<mcp_client::manager_t::server_status_t>& statuses,
    const std::string& name) {
    for (const auto& st : statuses)
        if (st.name == name)
            return st.state;
    return mcp_client::connection_state_t::disconnected;
}

QString connection_label(mcp_client::connection_state_t st) {
    switch (st) {
    case mcp_client::connection_state_t::connected: return QStringLiteral("Connected");
    case mcp_client::connection_state_t::connecting: return QStringLiteral("Connecting");
    case mcp_client::connection_state_t::reconnecting: return QStringLiteral("Reconnecting");
    case mcp_client::connection_state_t::error: return QStringLiteral("Error");
    default: return QStringLiteral("Disconnected");
    }
}

QString oauth_pill_label(mcp_client::oauth_status_t st) {
    switch (st) {
    case mcp_client::oauth_status_t::authenticated: return QStringLiteral("connected");
    case mcp_client::oauth_status_t::not_required: return QStringLiteral("no auth");
    case mcp_client::oauth_status_t::needs_auth: return QStringLiteral("sign in");
    case mcp_client::oauth_status_t::needs_client_registration:
        return QStringLiteral("configure");
    case mcp_client::oauth_status_t::authenticating: return QStringLiteral("auth...");
    case mcp_client::oauth_status_t::failed: return QStringLiteral("auth failed");
    }
    return QStringLiteral("?");
}

bool oauth_needs_action(mcp_client::oauth_status_t st) {
    return st == mcp_client::oauth_status_t::needs_auth ||
        st == mcp_client::oauth_status_t::needs_client_registration ||
        st == mcp_client::oauth_status_t::failed ||
        st == mcp_client::oauth_status_t::authenticating;
}

}

AidaMcpServerDraftModel::AidaMcpServerDraftModel(QObject* parent)
    : QAbstractListModel(parent) {}

int AidaMcpServerDraftModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(draft_.size());
}

QVariant AidaMcpServerDraftModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(draft_.size()))
        return {};
    const auto& srv = draft_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole: {
        QString label = QString::fromStdString(srv.name);
        if (!srv.enabled)
            label += QStringLiteral("  (disabled)");
        return label;
    }
    case Qt::ToolTipRole:
        return QString::fromStdString(srv.transport == "stdio"
            ? srv.command + " " + srv.args : srv.url);
    case Qt::UserRole:
        return QString::fromStdString(srv.name);
    default:
        return {};
    }
}

void AidaMcpServerDraftModel::loadFromSettings() {
    beginResetModel();
    draft_ = g_sa_settings.mcp_client_servers;
    dirty_ = false;
    endResetModel();
}

void AidaMcpServerDraftModel::discardChanges() {
    loadFromSettings();
}

void AidaMcpServerDraftModel::markClean() {
    dirty_ = false;
}

void AidaMcpServerDraftModel::touchRows() {
    if (draft_.empty())
        return;
    Q_EMIT dataChanged(index(0), index(static_cast<int>(draft_.size()) - 1), {});
}

void AidaMcpServerDraftModel::updateRow(int row, const mcp_client_server_t& server) {
    if (row < 0 || row >= static_cast<int>(draft_.size()))
        return;
    draft_[static_cast<std::size_t>(row)] = server;
    dirty_ = true;
    Q_EMIT dataChanged(index(row), index(row), {});
    Q_EMIT draftEdited();
}

int AidaMcpServerDraftModel::appendNewServer() {
    mcp_client_server_t srv;
    std::string base = "New Server";
    std::string candidate = base;
    int suffix = 2;
    bool unique = false;
    while (!unique) {
        unique = true;
        for (const auto& existing : draft_) {
            if (existing.name == candidate) {
                unique = false;
                candidate = base + " " + std::to_string(suffix++);
                break;
            }
        }
    }
    srv.name = candidate;
    srv.url = "http://localhost:3001";
    srv.enabled = false;
    srv.auto_connect = false;
    const int row = static_cast<int>(draft_.size());
    beginInsertRows({}, row, row);
    draft_.push_back(std::move(srv));
    dirty_ = true;
    endInsertRows();
    return row;
}

void AidaMcpServerDraftModel::removeRow(int row) {
    if (row < 0 || row >= static_cast<int>(draft_.size()))
        return;
    beginRemoveRows({}, row, row);
    draft_.erase(draft_.begin() + row);
    dirty_ = true;
    endRemoveRows();
}

AidaMcpInstalledSection::AidaMcpInstalledSection(QWidget* parent) : QWidget(parent) {
    const auto& t = theme::tokens();
    setObjectName(QStringLiteral("aida.view.settings.mcp.installed"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, t.spacing.sm, 0, 0);
    root->setSpacing(t.spacing.xs);
    auto* title = new QLabel(QStringLiteral("Marketplace Installed"), this);
    title->setObjectName(QStringLiteral("aida.view.settings.mcp.installed.title"));
    title->setFont(theme::fonts::strong());
    auto* hint = new QLabel(QStringLiteral(
        "Installed marketplace servers stay disabled until explicitly enabled here. "
        "Connecting starts local third-party MCP code and exposes any tools it registers."),
        this);
    hint->setObjectName(QStringLiteral("aida.view.settings.mcp.installed.hint"));
    hint->setWordWrap(true);
    hint->setFont(theme::fonts::caption());
    hint->setProperty("aidaVariant", "secondary");
    root->addWidget(title);
    root->addWidget(hint);
    auto* rows_host = new QWidget(this);
    rows_layout_ = new QVBoxLayout(rows_host);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    rows_layout_->setSpacing(t.spacing.sm);
    root->addWidget(rows_host);
    root->addStretch(1);
    refresh();
}

void AidaMcpInstalledSection::refresh() {
    const auto installed = ::mcp_marketplace::get_installed();
    const auto statuses = get_mcp_client_manager().get_status();
    std::string signature;
    for (const auto& srv : installed) {
        signature += srv.package_name;
        signature += srv.enabled ? '|' : '!';
        signature += srv.auto_connect ? '+' : '-';
        signature += std::to_string(static_cast<int>(
            status_for_server(statuses, srv.package_name)));
        signature += ';';
    }
    if (signature == last_signature_)
        return;
    last_signature_ = signature;
    while (QLayoutItem* item = rows_layout_->takeAt(0)) {
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    if (installed.empty()) {
        auto* empty = new QLabel(QStringLiteral("No marketplace servers installed."), this);
        empty->setObjectName(QStringLiteral("aida.view.settings.mcp.installed.empty"));
        empty->setFont(theme::fonts::caption());
        empty->setProperty("aidaVariant", "secondary");
        empty->setWordWrap(true);
        rows_layout_->addWidget(empty);
        return;
    }
    for (const auto& srv : installed) {
        const auto& t = theme::tokens();
        auto* card = new QWidget(this);
        card->setObjectName(QStringLiteral("aida.view.settings.mcp.installed.card"));
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(t.spacing.sm, t.spacing.xs, t.spacing.sm,
            t.spacing.xs);
        card_layout->setSpacing(t.spacing.xs);

        auto* header = new QHBoxLayout();
        header->setSpacing(t.spacing.sm);
        auto* name = new QLabel(QString::fromStdString(srv.package_name), card);
        name->setFont(theme::fonts::strong());
        header->addWidget(name);
        auto* registry = new QLabel(
            QString::fromStdString(::mcp_marketplace::registry_label(srv.registry)), card);
        registry->setFont(theme::fonts::caption());
        registry->setProperty("aidaVariant", "secondary");
        header->addWidget(registry);
        if (!srv.version.empty()) {
            auto* version = new QLabel(QString::fromStdString(srv.version), card);
            version->setFont(theme::fonts::caption());
            version->setProperty("aidaVariant", "secondary");
            header->addWidget(version);
        }
        auto* enabled = new QLabel(srv.enabled ? QStringLiteral("Enabled")
                                               : QStringLiteral("Disabled"), card);
        enabled->setFont(theme::fonts::caption());
        enabled->setProperty("aidaVariant", srv.enabled ? "success" : "neutral");
        header->addWidget(enabled);
        if (srv.auto_connect) {
            auto* auto_label = new QLabel(QStringLiteral("Auto-connect"), card);
            auto_label->setFont(theme::fonts::caption());
            auto_label->setProperty("aidaVariant", "info");
            header->addWidget(auto_label);
        }
        const auto state = status_for_server(statuses, srv.package_name);
        auto* conn = new QLabel(connection_label(state), card);
        conn->setFont(theme::fonts::caption());
        const char* conn_variant = "neutral";
        switch (state) {
        case mcp_client::connection_state_t::connected: conn_variant = "success"; break;
        case mcp_client::connection_state_t::connecting:
        case mcp_client::connection_state_t::reconnecting: conn_variant = "warning"; break;
        case mcp_client::connection_state_t::error: conn_variant = "error"; break;
        default: conn_variant = "neutral"; break;
        }
        conn->setProperty("aidaVariant", conn_variant);
        header->addWidget(conn);
        header->addStretch(1);
        card_layout->addLayout(header);

        auto* launch = new QLabel(QString::fromStdString(
            ::mcp_marketplace::launch_command_preview(srv)), card);
        launch->setFont(theme::fonts::codeRegular());
        launch->setWordWrap(true);
        launch->setTextInteractionFlags(Qt::TextSelectableByMouse);
        card_layout->addWidget(launch);

        auto* actions = new QHBoxLayout();
        const bool connected = state == mcp_client::connection_state_t::connected;
        if (!srv.enabled) {
            auto* enable = new QPushButton(QStringLiteral("Enable"), card);
            actions->addWidget(enable);
            connect(enable, &QPushButton::clicked, this, [this, name = srv.package_name] {
                if (::mcp_marketplace::set_server_policy(name, true, false)) {
                    chrome::toast_info(QStringLiteral(
                        "Marketplace server enabled with auto-connect off."), 3.5);
                    refresh();
                }
            });
        } else {
            auto* disable = new QPushButton(QStringLiteral("Disable"), card);
            actions->addWidget(disable);
            connect(disable, &QPushButton::clicked, this, [this, name = srv.package_name] {
                if (::mcp_marketplace::set_server_policy(name, false, false)) {
                    chrome::toast_info(QStringLiteral("Marketplace server disabled."), 3.5);
                    refresh();
                }
            });
            if (connected) {
                auto* disconnect = new QPushButton(QStringLiteral("Disconnect"), card);
                actions->addWidget(disconnect);
                connect(disconnect, &QPushButton::clicked, this, [this,
                        name = srv.package_name] {
                    ::mcp_marketplace::deactivate_server(name);
                    refresh();
                });
            } else {
                auto* connect_button = new QPushButton(QStringLiteral("Connect"), card);
                actions->addWidget(connect_button);
                connect(connect_button, &QPushButton::clicked, this, [this, srv] {
                    ::mcp_marketplace::activate_server(srv);
                    refresh();
                });
            }
            auto* auto_toggle = new QPushButton(srv.auto_connect
                ? QStringLiteral("Auto off") : QStringLiteral("Auto on"), card);
            actions->addWidget(auto_toggle);
            connect(auto_toggle, &QPushButton::clicked, this, [this, srv] {
                if (::mcp_marketplace::set_server_policy(srv.package_name, true,
                        !srv.auto_connect)) {
                    chrome::toast_info(srv.auto_connect
                        ? QStringLiteral("Marketplace auto-connect disabled.")
                        : QStringLiteral("Marketplace auto-connect enabled."), 3.5);
                    refresh();
                }
            });
        }
        actions->addStretch(1);
        card_layout->addLayout(actions);
        rows_layout_->addWidget(card);
    }
}

AidaSettingsMcpPage::AidaSettingsMcpPage(QWidget* parent) : QWidget(parent) {
    buildUi();
    model_->loadFromSettings();
    if (model_->rowCount() > 0) {
        selected_row_ = 0;
        list_->setCurrentIndex(model_->index(0));
    }
    refreshFormFromDraft();
}

AidaSettingsMcpPage::~AidaSettingsMcpPage() {
    std::vector<std::string> active_oauth;
    {
        std::lock_guard<std::mutex> lock(oauth_mtx_);
        active_oauth.reserve(oauth_generations_.size());
        for (const auto& entry : oauth_generations_)
            active_oauth.push_back(entry.first);
        oauth_generations_.clear();
    }
    for (const auto& server_name : active_oauth)
        (void)mcp_client::cancel_auth(server_name);
}

void AidaSettingsMcpPage::buildUi() {
    const auto& t = theme::tokens();
    setObjectName(QStringLiteral("aida.view.settings.mcpPage"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.spacing.md, t.spacing.sm, t.spacing.md, t.spacing.sm);
    root->setSpacing(t.spacing.sm);

    auto* title_block = new QVBoxLayout();
    title_block->setSpacing(t.spacing.xxs);
    auto* title = new QLabel(QStringLiteral("External MCP Servers"), this);
    title->setObjectName(QStringLiteral("aida.view.settings.mcp.title"));
    title->setFont(theme::fonts::h1());
    auto* subtitle = new QLabel(QStringLiteral(
        "Configure trusted external servers and review installed marketplace packages."),
        this);
    subtitle->setObjectName(QStringLiteral("aida.view.settings.mcp.subtitle"));
    subtitle->setFont(theme::fonts::caption());
    subtitle->setProperty("aidaVariant", "secondary");
    subtitle->setWordWrap(true);
    title_block->addWidget(title);
    title_block->addWidget(subtitle);
    root->addLayout(title_block);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("aida.view.settings.mcp.splitter"));
    root->addWidget(splitter, 1);

    auto* left = new QWidget(this);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    model_ = new AidaMcpServerDraftModel(this);
    list_ = new QListView(left);
    list_->setObjectName(QStringLiteral("aida.view.settings.mcp.serverList"));
    list_->setModel(model_);
    list_->setUniformItemSizes(true);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    left_layout->addWidget(list_);
    splitter->addWidget(left);

    auto* right = new QWidget(this);
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(t.spacing.sm, t.spacing.xs, t.spacing.sm, t.spacing.xs);
    right_layout->setSpacing(t.spacing.sm);

    auto* detail_title = new QLabel(QStringLiteral("Server Configuration"), right);
    detail_title->setObjectName(QStringLiteral("aida.view.settings.mcp.detailTitle"));
    detail_title->setFont(theme::fonts::strong());
    right_layout->addWidget(detail_title);
    dirty_label_ = new QLabel(right);
    dirty_label_->setObjectName(QStringLiteral("aida.view.settings.mcp.dirtyHint"));
    dirty_label_->setFont(theme::fonts::caption());
    dirty_label_->setProperty("aidaVariant", "secondary");
    dirty_label_->setWordWrap(true);
    right_layout->addWidget(dirty_label_);

    auto* form = new QFormLayout();
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    name_edit_ = new QLineEdit(right);
    name_edit_->setObjectName(QStringLiteral("aida.view.settings.mcp.name"));
    name_edit_->setPlaceholderText(QStringLiteral("Server name"));
    name_edit_->setClearButtonEnabled(true);
    name_edit_->setToolTip(QStringLiteral("Required. Unique display name for this server."));
    form->addRow(QStringLiteral("Name *"), name_edit_);
    transport_combo_ = new QComboBox(right);
    transport_combo_->setObjectName(QStringLiteral("aida.view.settings.mcp.transport"));
    transport_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    transport_combo_->setMinimumContentsLength(10);
    transport_combo_->addItem(QStringLiteral("HTTP/SSE"), QStringLiteral("http_sse"));
    transport_combo_->addItem(QStringLiteral("Stdio"), QStringLiteral("stdio"));
    transport_combo_->setToolTip(QStringLiteral("Connection transport for this server"));
    form->addRow(QStringLiteral("Transport"), transport_combo_);
    url_edit_ = new QLineEdit(right);
    url_edit_->setObjectName(QStringLiteral("aida.view.settings.mcp.url"));
    url_edit_->setPlaceholderText(QStringLiteral("https://server"));
    url_edit_->setClearButtonEnabled(true);
    url_edit_->setToolTip(QStringLiteral("Required for HTTP/SSE transport."));
    form->addRow(QStringLiteral("URL *"), url_edit_);
    key_edit_ = new QLineEdit(right);
    key_edit_->setObjectName(QStringLiteral("aida.view.settings.mcp.apiKey"));
    key_edit_->setEchoMode(QLineEdit::Password);
    key_edit_->setPlaceholderText(QStringLiteral("secret"));
    key_edit_->setClearButtonEnabled(true);
    key_edit_->setToolTip(QStringLiteral("Optional bearer/API key sent to this server"));
    auto* reveal_key = key_edit_->addAction(
        theme::icons::icon(QStringLiteral("padlock")), QLineEdit::TrailingPosition);
    reveal_key->setToolTip(QStringLiteral("Reveal key"));
    form->addRow(QStringLiteral("API Key"), key_edit_);
    command_edit_ = new QLineEdit(right);
    command_edit_->setObjectName(QStringLiteral("aida.view.settings.mcp.command"));
    command_edit_->setPlaceholderText(QStringLiteral("node server.js"));
    command_edit_->setClearButtonEnabled(true);
    command_edit_->setToolTip(QStringLiteral("Required for Stdio transport."));
    form->addRow(QStringLiteral("Command *"), command_edit_);
    args_edit_ = new QLineEdit(right);
    args_edit_->setObjectName(QStringLiteral("aida.view.settings.mcp.args"));
    args_edit_->setPlaceholderText(QStringLiteral("--port 3001"));
    args_edit_->setClearButtonEnabled(true);
    args_edit_->setToolTip(QStringLiteral(
        "Command-line arguments; quote values containing spaces"));
    form->addRow(QStringLiteral("Args"), args_edit_);
    argv_preview_ = new QLabel(right);
    argv_preview_->setObjectName(QStringLiteral("aida.view.settings.mcp.argvPreview"));
    argv_preview_->setFont(theme::fonts::codeRegular());
    argv_preview_->setWordWrap(true);
    argv_preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QStringLiteral("Argv preview"), argv_preview_);
    right_layout->addLayout(form);

    form_error_ = new QLabel(right);
    form_error_->setObjectName(QStringLiteral("aida.view.settings.mcp.formError"));
    form_error_->setFont(theme::fonts::caption());
    form_error_->setProperty("aidaVariant", "error");
    form_error_->setWordWrap(true);
    form_error_->setVisible(false);
    right_layout->addWidget(form_error_);

    auto* toggles = new QHBoxLayout();
    toggles->setSpacing(t.spacing.md);
    enabled_check_ = new QCheckBox(QStringLiteral("Enabled"), right);
    enabled_check_->setObjectName(QStringLiteral("aida.view.settings.mcp.enabled"));
    enabled_check_->setToolTip(QStringLiteral("Include this server in the MCP client set"));
    auto_connect_check_ = new QCheckBox(QStringLiteral("Auto-connect"), right);
    auto_connect_check_->setObjectName(QStringLiteral("aida.view.settings.mcp.autoConnect"));
    auto_connect_check_->setToolTip(QStringLiteral(
        "Connect automatically when the IDE starts (requires Enabled)"));
    toggles->addWidget(enabled_check_);
    toggles->addWidget(auto_connect_check_);
    toggles->addStretch(1);
    right_layout->addLayout(toggles);

    auto* oauth_row = new QHBoxLayout();
    oauth_row->setSpacing(t.spacing.sm);
    oauth_pill_ = new QLabel(right);
    oauth_pill_->setObjectName(QStringLiteral("aida.view.settings.mcp.oauthPill"));
    oauth_pill_->setFont(theme::fonts::caption());
    oauth_button_ = new QPushButton(QStringLiteral("Sign in"), right);
    oauth_button_->setObjectName(QStringLiteral("aida.view.settings.mcp.oauthAction"));
    oauth_row->addWidget(oauth_pill_);
    oauth_row->addWidget(oauth_button_);
    oauth_row->addStretch(1);
    right_layout->addLayout(oauth_row);

    auto* conn_row = new QHBoxLayout();
    conn_row->setSpacing(t.spacing.sm);
    reconnect_button_ = new QPushButton(QStringLiteral("Reconnect"), right);
    reconnect_button_->setObjectName(QStringLiteral("aida.view.settings.mcp.reconnect"));
    reconnect_button_->setToolTip(QStringLiteral("Drop and re-establish this connection"));
    disconnect_button_ = new QPushButton(QStringLiteral("Disconnect"), right);
    disconnect_button_->setObjectName(QStringLiteral("aida.view.settings.mcp.disconnect"));
    disconnect_button_->setToolTip(QStringLiteral("Close the live connection"));
    conn_row->addWidget(reconnect_button_);
    conn_row->addWidget(disconnect_button_);
    conn_row->addStretch(1);
    right_layout->addLayout(conn_row);

    installed_section_ = new AidaMcpInstalledSection(right);
    right_layout->addWidget(installed_section_, 1);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    auto* footer = new QHBoxLayout();
    footer->setSpacing(t.spacing.sm);
    add_button_ = new QPushButton(QStringLiteral("Add Server"), this);
    add_button_->setObjectName(QStringLiteral("aida.view.settings.mcp.add"));
    apply_button_ = new QPushButton(QStringLiteral("Apply Changes"), this);
    apply_button_->setObjectName(QStringLiteral("aida.view.settings.mcp.apply"));
    apply_button_->setProperty("aidaVariant", "primary");
    discard_button_ = new QPushButton(QStringLiteral("Discard Draft"), this);
    discard_button_->setObjectName(QStringLiteral("aida.view.settings.mcp.discard"));
    remove_button_ = new QPushButton(QStringLiteral("Remove Selected"), this);
    remove_button_->setObjectName(QStringLiteral("aida.view.settings.mcp.remove"));
    remove_button_->setProperty("aidaVariant", "destructive");
    marketplace_button_ = new QPushButton(QStringLiteral("Marketplace..."), this);
    marketplace_button_->setObjectName(QStringLiteral("aida.view.settings.mcp.marketplace"));
    footer->addWidget(add_button_);
    footer->addWidget(apply_button_);
    footer->addWidget(discard_button_);
    footer->addWidget(remove_button_);
    footer->addStretch(1);
    footer->addWidget(marketplace_button_);
    root->addLayout(footer);

    status_timer_ = new QTimer(this);
    status_timer_->setInterval(500);

    connect(reveal_key, &QAction::triggered, this, [this] {
        const bool password = key_edit_->echoMode() == QLineEdit::Password;
        key_edit_->setEchoMode(password ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid())
            return;
        if (model_->dirty()) {
            chrome::toast_warning(QStringLiteral(
                "Apply or discard MCP server changes before switching."), 4.0);
            if (selected_row_ >= 0 && selected_row_ < model_->rowCount()) {
                const QSignalBlocker blocker(list_->selectionModel());
                list_->setCurrentIndex(model_->index(selected_row_));
            }
            return;
        }
        selected_row_ = current.row();
        refreshFormFromDraft();
    });
    connect(name_edit_, &QLineEdit::textEdited, this, &AidaSettingsMcpPage::onFieldEdited);
    connect(transport_combo_, &QComboBox::currentIndexChanged, this,
            &AidaSettingsMcpPage::onFieldEdited);
    connect(url_edit_, &QLineEdit::textEdited, this, &AidaSettingsMcpPage::onFieldEdited);
    connect(key_edit_, &QLineEdit::textEdited, this, &AidaSettingsMcpPage::onFieldEdited);
    connect(command_edit_, &QLineEdit::textEdited, this, &AidaSettingsMcpPage::onFieldEdited);
    connect(args_edit_, &QLineEdit::textEdited, this, &AidaSettingsMcpPage::onFieldEdited);
    connect(enabled_check_, &QCheckBox::toggled, this, &AidaSettingsMcpPage::onFieldEdited);
    connect(auto_connect_check_, &QCheckBox::toggled, this,
            &AidaSettingsMcpPage::onFieldEdited);
    connect(add_button_, &QPushButton::clicked, this, [this] {
        commitFormToDraft();
        const int row = model_->appendNewServer();
        if (row >= 0) {
            selected_row_ = row;
            list_->setCurrentIndex(model_->index(row));
        }
        refreshFormFromDraft();
    });
    connect(apply_button_, &QPushButton::clicked, this, &AidaSettingsMcpPage::onApply);
    connect(discard_button_, &QPushButton::clicked, this, [this] {
        model_->discardChanges();
        refreshFormFromDraft();
        chrome::toast_info(QStringLiteral("MCP server draft discarded."), 3.0);
    });
    connect(remove_button_, &QPushButton::clicked, this,
            &AidaSettingsMcpPage::onRemoveSelected);
    connect(marketplace_button_, &QPushButton::clicked, this, [] {
        ai::open_ai_view("view.ai.mcp_marketplace");
    });
    connect(reconnect_button_, &QPushButton::clicked, this, [this] {
        commitFormToDraft();
        const QModelIndex current = list_->currentIndex();
        if (!current.isValid())
            return;
        const auto& draft = model_->draft();
        if (current.row() < 0 || current.row() >= static_cast<int>(draft.size()))
            return;
        mcp_client::server_config_t cfg;
        std::string err;
        auto& mgr = get_mcp_client_manager();
        if (build_mcp_client_config(draft[static_cast<std::size_t>(current.row())],
                cfg, err)) {
            mgr.add_server(cfg);
            mgr.disconnect_server(cfg.name);
            mgr.connect_server(cfg.name);
        } else {
            chrome::toast_error(QString::fromStdString(err), 4.0);
        }
    });
    connect(disconnect_button_, &QPushButton::clicked, this, [this] {
        const QModelIndex current = list_->currentIndex();
        if (!current.isValid())
            return;
        const auto& draft = model_->draft();
        if (current.row() < 0 || current.row() >= static_cast<int>(draft.size()))
            return;
        get_mcp_client_manager().disconnect_server(
            draft[static_cast<std::size_t>(current.row())].name);
    });
    connect(oauth_button_, &QPushButton::clicked, this, [this] {
        const QModelIndex current = list_->currentIndex();
        if (!current.isValid())
            return;
        const auto& draft = model_->draft();
        if (current.row() < 0 || current.row() >= static_cast<int>(draft.size()))
            return;
        const auto& srv = draft[static_cast<std::size_t>(current.row())];
        const auto oauth = mcp_client::auth_status(srv.name);
        if (oauth == mcp_client::oauth_status_t::authenticating)
            onCancelAuth(srv.name);
        else
            onSignIn(srv.name);
    });
    connect(status_timer_, &QTimer::timeout, this,
            &AidaSettingsMcpPage::refreshRowStatuses);
    updateArgvPreview();
}

void AidaSettingsMcpPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshRowStatuses();
    installed_section_->refresh();
    if (!status_timer_->isActive())
        status_timer_->start();
}

void AidaSettingsMcpPage::hideEvent(QHideEvent* event) {
    status_timer_->stop();
    QWidget::hideEvent(event);
}

void AidaSettingsMcpPage::refreshFormFromDraft() {
    loading_form_ = true;
    form_error_->setVisible(false);
    const QModelIndex current = list_->currentIndex();
    const auto& draft = model_->draft();
    if (!current.isValid() || current.row() < 0 ||
        current.row() >= static_cast<int>(draft.size())) {
        name_edit_->clear();
        url_edit_->clear();
        key_edit_->clear();
        command_edit_->clear();
        args_edit_->clear();
        transport_combo_->setCurrentIndex(0);
        enabled_check_->setChecked(false);
        auto_connect_check_->setChecked(false);
        loading_form_ = false;
        updateArgvPreview();
        return;
    }
    const auto& srv = draft[static_cast<std::size_t>(current.row())];
    name_edit_->setText(QString::fromStdString(srv.name));
    url_edit_->setText(QString::fromStdString(srv.url));
    key_edit_->setText(QString::fromStdString(srv.api_key));
    command_edit_->setText(QString::fromStdString(srv.command));
    args_edit_->setText(QString::fromStdString(srv.args));
    transport_combo_->setCurrentIndex(srv.transport == "stdio" ? 1 : 0);
    enabled_check_->setChecked(srv.enabled);
    auto_connect_check_->setChecked(srv.enabled && srv.auto_connect);
    loading_form_ = false;
    updateTransportVisibility();
    updateArgvPreview();
    refreshRowStatuses();
}

void AidaSettingsMcpPage::commitFormToDraft() {
    const QModelIndex current = list_->currentIndex();
    if (!current.isValid())
        return;
    const auto& draft = model_->draft();
    if (current.row() < 0 || current.row() >= static_cast<int>(draft.size()))
        return;
    mcp_client_server_t srv = draft[static_cast<std::size_t>(current.row())];
    srv.name = name_edit_->text().toStdString();
    srv.url = url_edit_->text().toStdString();
    srv.api_key = key_edit_->text().toStdString();
    srv.command = command_edit_->text().toStdString();
    srv.args = args_edit_->text().toStdString();
    srv.transport = transport_combo_->currentData().toString().toStdString();
    srv.enabled = enabled_check_->isChecked();
    srv.auto_connect = enabled_check_->isChecked() && auto_connect_check_->isChecked();
    model_->updateRow(current.row(), srv);
}

void AidaSettingsMcpPage::onFieldEdited() {
    if (loading_form_)
        return;
    if (!enabled_check_->isChecked() && auto_connect_check_->isChecked()) {
        auto_connect_check_->setChecked(false);
    }
    form_error_->setVisible(false);
    updateTransportVisibility();
    updateArgvPreview();
    commitFormToDraft();
    const bool dirty = model_->dirty();
    apply_button_->setEnabled(dirty);
    discard_button_->setEnabled(dirty);
    dirty_label_->setText(dirty
        ? QStringLiteral("Unsaved changes - Apply reconnects configured MCP clients.")
        : QStringLiteral("Edits are staged until Apply."));
    if (dirty_label_->property("aidaVariant").toString() !=
            QLatin1String(dirty ? "warning" : "secondary")) {
        dirty_label_->setProperty("aidaVariant", dirty ? "warning" : "secondary");
        repolish_variant(dirty_label_);
    }
}

void AidaSettingsMcpPage::updateTransportVisibility() {
    const bool stdio = transport_combo_->currentData().toString() == QLatin1String("stdio");
    url_edit_->setVisible(!stdio);
    key_edit_->setVisible(!stdio);
    command_edit_->setVisible(stdio);
    args_edit_->setVisible(stdio);
    argv_preview_->setVisible(stdio);
}

void AidaSettingsMcpPage::updateArgvPreview() {
    if (transport_combo_->currentData().toString() == QLatin1String("stdio")) {
        std::vector<std::string> parsed_args;
        std::string parse_error;
        const bool args_ok = parse_mcp_args(args_edit_->text().toStdString(), parsed_args,
            parse_error);
        if (args_ok) {
            argv_preview_->setText(QString::fromStdString(make_argv_preview(
                command_edit_->text().toStdString(), parsed_args)));
            argv_preview_->setToolTip(QString());
            if (argv_preview_->property("aidaVariant").isValid()) {
                argv_preview_->setProperty("aidaVariant", QVariant());
                repolish_variant(argv_preview_);
            }
        } else {
            argv_preview_->setText(QString::fromStdString(parse_error));
            argv_preview_->setToolTip(QString::fromStdString(parse_error));
            if (argv_preview_->property("aidaVariant").toString() !=
                    QLatin1String("error")) {
                argv_preview_->setProperty("aidaVariant", "error");
                repolish_variant(argv_preview_);
            }
        }
    } else {
        argv_preview_->setText(QString());
    }
}

void AidaSettingsMcpPage::refreshRowStatuses() {
    const QModelIndex current = list_->currentIndex();
    const auto& draft = model_->draft();
    if (!current.isValid() || current.row() < 0 ||
        current.row() >= static_cast<int>(draft.size())) {
        oauth_pill_->clear();
        oauth_button_->setVisible(false);
        return;
    }
    const auto& srv = draft[static_cast<std::size_t>(current.row())];
    const auto oauth = mcp_client::auth_status(srv.name);
    oauth_pill_->setText(oauth_pill_label(oauth));
    const char* variant = "neutral";
    switch (oauth) {
    case mcp_client::oauth_status_t::authenticated: variant = "success"; break;
    case mcp_client::oauth_status_t::needs_auth: variant = "warning"; break;
    case mcp_client::oauth_status_t::authenticating: variant = "info"; break;
    case mcp_client::oauth_status_t::needs_client_registration:
    case mcp_client::oauth_status_t::failed: variant = "error"; break;
    case mcp_client::oauth_status_t::not_required:
    default: variant = "neutral"; break;
    }
    if (oauth_pill_->property("aidaVariant").toString() != QLatin1String(variant)) {
        oauth_pill_->setProperty("aidaVariant", variant);
        repolish_variant(oauth_pill_);
    }
    const bool actionable = oauth_needs_action(oauth);
    oauth_button_->setVisible(actionable);
    if (actionable) {
        oauth_button_->setText(oauth == mcp_client::oauth_status_t::authenticating
            ? QStringLiteral("Cancel") : QStringLiteral("Sign in"));
    }
    if (model_->rowCount() > 0)
        model_->touchRows();
    installed_section_->refresh();
}

std::uint64_t AidaSettingsMcpPage::beginOauthGeneration(const std::string& server_name) {
    std::lock_guard<std::mutex> lock(oauth_mtx_);
    if (oauth_generation_ == (std::numeric_limits<std::uint64_t>::max)())
        return 0;
    const std::uint64_t generation = ++oauth_generation_;
    oauth_generations_[server_name] = generation;
    return generation;
}

bool AidaSettingsMcpPage::completeOauthGeneration(const std::string& server_name,
                                                  std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(oauth_mtx_);
    const auto it = oauth_generations_.find(server_name);
    if (it == oauth_generations_.end() || it->second != generation)
        return false;
    oauth_generations_.erase(it);
    return true;
}

void AidaSettingsMcpPage::cancelOauthGeneration(const std::string& server_name) {
    std::lock_guard<std::mutex> lock(oauth_mtx_);
    oauth_generations_.erase(server_name);
}

void AidaSettingsMcpPage::onSignIn(const std::string& server_name) {
    const std::uint64_t generation = beginOauthGeneration(server_name);
    if (generation == 0) {
        chrome::toast_error(QStringLiteral(
            "MCP auth %1: UI authorization generation exhausted; restart is required")
            .arg(QString::fromStdString(server_name)), 5.0);
        return;
    }
    const bool accepted = mcp_client::trigger_auth_flow(server_name,
        [this, generation](const std::string& nm, mcp_client::oauth_status_t final_status,
                           const std::string& err) {
            const bool current = completeOauthGeneration(nm, generation);
            QMetaObject::invokeMethod(this, [this, nm, final_status, err, current] {
                if (!current)
                    return;
                if (!err.empty())
                    chrome::toast_error(QStringLiteral("MCP auth %1: %2")
                        .arg(QString::fromStdString(nm), QString::fromStdString(err)), 5.0);
                else if (final_status == mcp_client::oauth_status_t::authenticated)
                    chrome::toast_info(QStringLiteral("MCP auth %1: OK")
                        .arg(QString::fromStdString(nm)), 3.5);
                refreshRowStatuses();
            }, Qt::QueuedConnection);
        });
    if (!accepted && completeOauthGeneration(server_name, generation)) {
        chrome::toast_error(QStringLiteral("MCP auth %1: %2")
            .arg(QString::fromStdString(server_name),
                 QString::fromStdString(mcp_client::last_error())), 5.0);
    }
}

void AidaSettingsMcpPage::onCancelAuth(const std::string& server_name) {
    cancelOauthGeneration(server_name);
    const bool cancelled = mcp_client::cancel_auth(server_name);
    if (cancelled) {
        chrome::toast_info(QStringLiteral("MCP auth %1: cancelled")
            .arg(QString::fromStdString(server_name)), 3.5);
    } else {
        chrome::toast_error(QStringLiteral("MCP auth %1: %2")
            .arg(QString::fromStdString(server_name),
                 QString::fromStdString(mcp_client::last_error())), 5.0);
    }
    refreshRowStatuses();
}

void AidaSettingsMcpPage::onApply() {
    commitFormToDraft();
    auto& mgr = get_mcp_client_manager();
    bool ok = true;
    std::string apply_error;
    std::vector<mcp_client::server_config_t> configs;
    const auto& draft = model_->draft();
    configs.reserve(draft.size());
    std::unordered_map<std::string, int> seen_names;
    for (const auto& srv : draft) {
        mcp_client::server_config_t cfg;
        if (!build_mcp_client_config(srv, cfg, apply_error)) {
            ok = false;
            break;
        }
        if (++seen_names[cfg.name] > 1) {
            apply_error = "Duplicate MCP server name: " + cfg.name;
            ok = false;
            break;
        }
        configs.push_back(std::move(cfg));
    }
    if (!ok) {
        form_error_->setText(QString::fromStdString(apply_error));
        form_error_->setVisible(true);
        chrome::toast_error(QString::fromStdString(apply_error), 5.0);
        return;
    }
    form_error_->setVisible(false);
    g_sa_settings.mcp_client_servers = draft;
    mgr.disconnect_all();
    for (const auto& cfg : configs)
        mgr.add_server(cfg);
    mgr.connect_all();
    static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
    model_->markClean();
    apply_button_->setEnabled(false);
    discard_button_->setEnabled(false);
    dirty_label_->setText(QStringLiteral("Edits are staged until Apply."));
    dirty_label_->setProperty("aidaVariant", "secondary");
    repolish_variant(dirty_label_);
    installed_section_->refresh();
    chrome::toast_info(QStringLiteral("MCP server settings applied."), 3.5);
}

void AidaSettingsMcpPage::onRemoveSelected() {
    const QModelIndex current = list_->currentIndex();
    if (!current.isValid())
        return;
    const auto& draft = model_->draft();
    if (current.row() < 0 || current.row() >= static_cast<int>(draft.size()))
        return;
    const int row = current.row();
    const QString name = QString::fromStdString(draft[static_cast<std::size_t>(row)].name);

    aida::qt::ai::aida_confirm_request_t request;
    request.verb = QStringLiteral("Remove");
    request.target = name;
    request.scope = QStringLiteral("The staged MCP server draft entry");
    request.effect = QStringLiteral(
        "Removal is staged as a draft. Apply Changes will disconnect and remove this "
        "server from the MCP client configuration.");
    request.reversibility = QStringLiteral(
        "Discard the draft to recover the staged server before applying.");
    request.prerequisite = QString();
    request.confirm_label = QStringLiteral("Remove");
    request.destructive = true;
    request.confirm_enabled = true;
    aida::qt::ai::AidaConfirmDialog::request(request, this, [this, row] {
        model_->removeRow(row);
        if (model_->rowCount() <= 0) {
            selected_row_ = -1;
            list_->clearSelection();
        } else {
            selected_row_ = (std::min)(row, model_->rowCount() - 1);
            list_->setCurrentIndex(model_->index(selected_row_));
        }
        refreshFormFromDraft();
        apply_button_->setEnabled(model_->dirty());
        discard_button_->setEnabled(model_->dirty());
    });
}

}
