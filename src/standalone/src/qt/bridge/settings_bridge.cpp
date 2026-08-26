#include "qt/bridge/settings_bridge.hpp"

#include <QByteArray>
#include <QMetaType>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include <cstring>
#include <exception>
#include <utility>

#include "core/settings/standalone_settings.hpp"
#include "helpers/diag_log.hpp"

namespace aida::qt::bridge {

namespace {

constexpr int k_status_poll_interval_ms = 500;

struct section_def_t {
    QtSettingsBridge::section_t id;
    const char* name;
    std::function<std::function<void(settings_sa_t&)>(const settings_sa_t&)> capture;
};

using section_t = QtSettingsBridge::section_t;

const section_def_t k_sections[] = {
    {section_t::providers, "providers", [](const settings_sa_t& s) {
        return [provider_profiles = s.provider_profiles,
                active_provider_profile_id = s.active_provider_profile_id,
                api_provider = s.api_provider,
                default_provider_id = s.default_provider_id,
                default_model_id = s.default_model_id,
                small_model_provider_id = s.small_model_provider_id,
                small_model_id = s.small_model_id,
                default_agent_name = s.default_agent_name,
                preferred_model_per_provider = s.preferred_model_per_provider,
                provider_base_url_overrides = s.provider_base_url_overrides,
                provider_headers_overrides = s.provider_headers_overrides,
                gemini_api_key = s.gemini_api_key,
                gemini_model_name = s.gemini_model_name,
                gemini_base_url = s.gemini_base_url,
                openai_api_key = s.openai_api_key,
                openai_model_name = s.openai_model_name,
                openai_base_url = s.openai_base_url,
                openrouter_api_key = s.openrouter_api_key,
                openrouter_model_name = s.openrouter_model_name,
                anthropic_api_key = s.anthropic_api_key,
                anthropic_model_name = s.anthropic_model_name,
                anthropic_base_url = s.anthropic_base_url,
                local_llm_base_url = s.local_llm_base_url,
                local_llm_model_name = s.local_llm_model_name,
                local_llm_api_key = s.local_llm_api_key](settings_sa_t& t) mutable {
            t.provider_profiles = std::move(provider_profiles);
            t.active_provider_profile_id = std::move(active_provider_profile_id);
            t.api_provider = std::move(api_provider);
            t.default_provider_id = std::move(default_provider_id);
            t.default_model_id = std::move(default_model_id);
            t.small_model_provider_id = std::move(small_model_provider_id);
            t.small_model_id = std::move(small_model_id);
            t.default_agent_name = std::move(default_agent_name);
            t.preferred_model_per_provider = std::move(preferred_model_per_provider);
            t.provider_base_url_overrides = std::move(provider_base_url_overrides);
            t.provider_headers_overrides = std::move(provider_headers_overrides);
            t.gemini_api_key = std::move(gemini_api_key);
            t.gemini_model_name = std::move(gemini_model_name);
            t.gemini_base_url = std::move(gemini_base_url);
            t.openai_api_key = std::move(openai_api_key);
            t.openai_model_name = std::move(openai_model_name);
            t.openai_base_url = std::move(openai_base_url);
            t.openrouter_api_key = std::move(openrouter_api_key);
            t.openrouter_model_name = std::move(openrouter_model_name);
            t.anthropic_api_key = std::move(anthropic_api_key);
            t.anthropic_model_name = std::move(anthropic_model_name);
            t.anthropic_base_url = std::move(anthropic_base_url);
            t.local_llm_base_url = std::move(local_llm_base_url);
            t.local_llm_model_name = std::move(local_llm_model_name);
            t.local_llm_api_key = std::move(local_llm_api_key);
        };
    }},
    {section_t::chat, "chat", [](const settings_sa_t& s) {
        return [temperature = s.temperature,
                enable_reasoning = s.enable_reasoning,
                reasoning_budget = s.reasoning_budget,
                reasoning_effort = s.reasoning_effort,
                prompt_caching = s.prompt_caching,
                max_agentic_rounds = s.max_agentic_rounds,
                chat_font_size = s.chat_font_size,
                chat_density = s.chat_density,
                chat_show_timestamps = s.chat_show_timestamps,
                chat_show_tokens = s.chat_show_tokens,
                condense_threshold = s.condense_threshold,
                condense_buffer = s.condense_buffer](settings_sa_t& t) mutable {
            t.temperature = temperature;
            t.enable_reasoning = enable_reasoning;
            t.reasoning_budget = reasoning_budget;
            t.reasoning_effort = std::move(reasoning_effort);
            t.prompt_caching = prompt_caching;
            t.max_agentic_rounds = max_agentic_rounds;
            t.chat_font_size = chat_font_size;
            t.chat_density = chat_density;
            t.chat_show_timestamps = chat_show_timestamps;
            t.chat_show_tokens = chat_show_tokens;
            t.condense_threshold = condense_threshold;
            t.condense_buffer = condense_buffer;
        };
    }},
    {section_t::editor, "editor", [](const settings_sa_t& s) {
        return [editor_tab_size = s.editor_tab_size,
                editor_font_size = s.editor_font_size,
                editor_auto_complete = s.editor_auto_complete,
                editor_line_numbers = s.editor_line_numbers,
                editor_highlight_line = s.editor_highlight_line,
                editor_word_wrap = s.editor_word_wrap,
                editor_minimap = s.editor_minimap,
                editor_bracket_match = s.editor_bracket_match,
                ghost_text_enabled = s.ghost_text_enabled,
                ghost_text_model = s.ghost_text_model,
                ghost_text_provider_id = s.ghost_text_provider_id,
                ghost_text_debounce_ms = s.ghost_text_debounce_ms,
                auto_save_enabled = s.auto_save_enabled,
                auto_save_interval_s = s.auto_save_interval_s,
                keybinding_overrides_json = s.keybinding_overrides_json](settings_sa_t& t) mutable {
            t.editor_tab_size = editor_tab_size;
            t.editor_font_size = editor_font_size;
            t.editor_auto_complete = editor_auto_complete;
            t.editor_line_numbers = editor_line_numbers;
            t.editor_highlight_line = editor_highlight_line;
            t.editor_word_wrap = editor_word_wrap;
            t.editor_minimap = editor_minimap;
            t.editor_bracket_match = editor_bracket_match;
            t.ghost_text_enabled = ghost_text_enabled;
            t.ghost_text_model = std::move(ghost_text_model);
            t.ghost_text_provider_id = std::move(ghost_text_provider_id);
            t.ghost_text_debounce_ms = ghost_text_debounce_ms;
            t.auto_save_enabled = auto_save_enabled;
            t.auto_save_interval_s = auto_save_interval_s;
            t.keybinding_overrides_json = std::move(keybinding_overrides_json);
        };
    }},
    {section_t::terminal, "terminal", [](const settings_sa_t& s) {
        return [terminal_shell = s.terminal_shell,
                terminal_scrollback = s.terminal_scrollback,
                terminal_profile_id = s.terminal_profile_id,
                terminal_default_cwd = s.terminal_default_cwd,
                terminal_restore_sessions = s.terminal_restore_sessions,
                terminal_sessions_json = s.terminal_sessions_json](settings_sa_t& t) mutable {
            t.terminal_shell = std::move(terminal_shell);
            t.terminal_scrollback = terminal_scrollback;
            t.terminal_profile_id = std::move(terminal_profile_id);
            t.terminal_default_cwd = std::move(terminal_default_cwd);
            t.terminal_restore_sessions = terminal_restore_sessions;
            t.terminal_sessions_json = std::move(terminal_sessions_json);
        };
    }},
    {section_t::security_approvals, "security_approvals", [](const settings_sa_t& s) {
        return [tool_auto_approve = s.tool_auto_approve,
                tool_always_allow = s.tool_always_allow,
                tool_always_deny = s.tool_always_deny,
                force_xml_tools = s.force_xml_tools,
                auto_approve_read = s.auto_approve_read,
                auto_approve_write = s.auto_approve_write,
                auto_approve_execute = s.auto_approve_execute,
                auto_approve_mcp = s.auto_approve_mcp,
                auto_approve_mode_switch = s.auto_approve_mode_switch,
                auto_approve_subtask = s.auto_approve_subtask,
                auto_approve_max_requests = s.auto_approve_max_requests,
                auto_approve_max_cost = s.auto_approve_max_cost,
                auto_approve_allowed_commands = s.auto_approve_allowed_commands,
                aidaignore_path = s.aidaignore_path](settings_sa_t& t) mutable {
            t.tool_auto_approve = tool_auto_approve;
            t.tool_always_allow = std::move(tool_always_allow);
            t.tool_always_deny = std::move(tool_always_deny);
            t.force_xml_tools = force_xml_tools;
            t.auto_approve_read = auto_approve_read;
            t.auto_approve_write = auto_approve_write;
            t.auto_approve_execute = auto_approve_execute;
            t.auto_approve_mcp = auto_approve_mcp;
            t.auto_approve_mode_switch = auto_approve_mode_switch;
            t.auto_approve_subtask = auto_approve_subtask;
            t.auto_approve_max_requests = auto_approve_max_requests;
            t.auto_approve_max_cost = auto_approve_max_cost;
            t.auto_approve_allowed_commands = std::move(auto_approve_allowed_commands);
            t.aidaignore_path = std::move(aidaignore_path);
        };
    }},
    {section_t::sandbox, "sandbox", [](const settings_sa_t& s) {
        return [sandbox = s.sandbox](settings_sa_t& t) mutable {
            t.sandbox = std::move(sandbox);
        };
    }},
    {section_t::mcp, "mcp", [](const settings_sa_t& s) {
        return [mcp_port = s.mcp_port,
                mcp_enabled = s.mcp_enabled,
                mcp_client_servers = s.mcp_client_servers,
                marketplace_installed_json = s.marketplace_installed_json](settings_sa_t& t) mutable {
            t.mcp_port = mcp_port;
            t.mcp_enabled = mcp_enabled;
            t.mcp_client_servers = std::move(mcp_client_servers);
            t.marketplace_installed_json = std::move(marketplace_installed_json);
        };
    }},
    {section_t::symbols, "symbols", [](const settings_sa_t& s) {
        return [pdb_search_paths = s.pdb_search_paths,
                symbol_cache_dir = s.symbol_cache_dir,
                symbol_auto_download = s.symbol_auto_download,
                symbol_server_url = s.symbol_server_url](settings_sa_t& t) mutable {
            t.pdb_search_paths = std::move(pdb_search_paths);
            t.symbol_cache_dir = std::move(symbol_cache_dir);
            t.symbol_auto_download = symbol_auto_download;
            t.symbol_server_url = std::move(symbol_server_url);
        };
    }},
    {section_t::ui_theme, "ui_theme", [](const settings_sa_t& s) {
        return [active_theme_idx = s.active_theme_idx,
                active_custom_theme_idx = s.active_custom_theme_idx,
                theme_icon_index = s.theme_icon_index,
                custom_icon_path = s.custom_icon_path,
                custom_themes_json = s.custom_themes_json,
                ui_density = s.ui_density,
                ui_reduced_motion = s.ui_reduced_motion,
                ui_diagnostics_mode = s.ui_diagnostics_mode,
                ui_table_preferences_json = s.ui_table_preferences_json,
                ui_filter_preferences_json = s.ui_filter_preferences_json,
                activity_bar_visible = s.activity_bar_visible,
                first_run_completed = s.first_run_completed,
                debugger_definitions_json = s.debugger_definitions_json,
                memory_scanner_state_json = s.memory_scanner_state_json,
                recent_workspaces_json = s.recent_workspaces_json,
                programming_tasks_json = s.programming_tasks_json,
                programming_selected_task_id = s.programming_selected_task_id](settings_sa_t& t) mutable {
            t.active_theme_idx = active_theme_idx;
            t.active_custom_theme_idx = active_custom_theme_idx;
            t.theme_icon_index = theme_icon_index;
            t.custom_icon_path = std::move(custom_icon_path);
            t.custom_themes_json = std::move(custom_themes_json);
            t.ui_density = ui_density;
            t.ui_reduced_motion = ui_reduced_motion;
            t.ui_diagnostics_mode = ui_diagnostics_mode;
            t.ui_table_preferences_json = std::move(ui_table_preferences_json);
            t.ui_filter_preferences_json = std::move(ui_filter_preferences_json);
            t.activity_bar_visible = activity_bar_visible;
            t.first_run_completed = first_run_completed;
            t.debugger_definitions_json = std::move(debugger_definitions_json);
            t.memory_scanner_state_json = std::move(memory_scanner_state_json);
            t.recent_workspaces_json = std::move(recent_workspaces_json);
            t.programming_tasks_json = std::move(programming_tasks_json);
            t.programming_selected_task_id = std::move(programming_selected_task_id);
        };
    }},
};

const section_def_t& section_def(section_t id) {
    for (const auto& def : k_sections) {
        if (def.id == id)
            return def;
    }
    return k_sections[0];
}

bool status_differs(const aida::settings_persistence::status_t& lhs,
                    const aida::settings_persistence::status_t& rhs) noexcept {
    return lhs.generation != rhs.generation ||
        lhs.committed_generation != rhs.committed_generation ||
        lhs.pending != rhs.pending || lhs.failed != rhs.failed ||
        lhs.stage != rhs.stage || lhs.error != rhs.error;
}

}

QtSettingsBridge::QtSettingsBridge(QObject* parent) : QObject(parent) {
    qRegisterMetaType<aida::settings_persistence::status_t>(
        "aida::settings_persistence::status_t");
    latest_ = aida::settings_persistence::status();
    status_timer_ = new QTimer(this);
    status_timer_->setInterval(k_status_poll_interval_ms);
    connect(status_timer_, &QTimer::timeout, this, [this] {
        const auto current = aida::settings_persistence::status();
        if (status_differs(current, latest_)) {
            latest_ = current;
            Q_EMIT statusChanged(latest_);
        }
    });
    status_timer_->start();
}

QtSettingsBridge::~QtSettingsBridge() = default;

const settings_sa_t& QtSettingsBridge::settings() const noexcept {
    return g_sa_settings;
}

bool QtSettingsBridge::mutate(section_t section,
                              const std::function<void(settings_sa_t&)>& fn,
                              const char* label) {
    if (!fn)
        return false;
    if (QThread::currentThread() != thread()) {
        diag::log_tagged_critical_fmt("qt_settings",
            "settings_mutate_rejected_off_gui section=%s label=%s",
            section_def(section).name, label ? label : "");
        return false;
    }
    const auto& def = section_def(section);
    auto restore = def.capture(g_sa_settings);
    try {
        fn(g_sa_settings);
    } catch (const std::exception& exception) {
        restore(g_sa_settings);
        diag::log_tagged_critical_fmt("qt_settings",
            "settings_mutate_exception section=%s label=%s error=%s",
            def.name, label ? label : "", exception.what());
        Q_EMIT saveRejected(QString::fromLatin1(def.name),
                          QString::fromUtf8(exception.what()));
        return false;
    } catch (...) {
        restore(g_sa_settings);
        diag::log_tagged_critical_fmt("qt_settings",
            "settings_mutate_exception section=%s label=%s error=unknown",
            def.name, label ? label : "");
        Q_EMIT saveRejected(QString::fromLatin1(def.name),
                          QStringLiteral("The settings mutation raised an unknown exception"));
        return false;
    }
    const auto result = aida::settings_persistence::request_save(g_sa_settings);
    if (!aida::settings_persistence::accepted(result)) {
        restore(g_sa_settings);
        const auto status = aida::settings_persistence::status();
        const QString detail = status.error.empty()
            ? QStringLiteral("The settings service rejected the save request")
            : QString::fromStdString(status.error);
        diag::log_tagged_fmt("qt_settings",
            "settings_save_rejected section=%s label=%s detail=%s",
            def.name, label ? label : "", status.error.c_str());
        Q_EMIT saveRejected(QString::fromLatin1(def.name), detail);
        return false;
    }
    return true;
}

bool QtSettingsBridge::is_forced(const char* field_name) noexcept {
    if (!field_name)
        return false;
    static constexpr const char* k_forced[] = {
        "editor_line_numbers",
        "editor_word_wrap",
        "editor_minimap",
        "editor_bracket_match",
        "editor_highlight_line",
        "editor_auto_complete",
        "ghost_text_enabled",
        "auto_save_enabled",
    };
    for (const char* name : k_forced) {
        if (std::strcmp(field_name, name) == 0)
            return true;
    }
    return false;
}

bool QtSettingsBridge::is_secret_field(const char* field_name) noexcept {
    if (!field_name || !*field_name)
        return false;
    const std::string_view name(field_name);
    if (name.find("api_key") != std::string_view::npos)
        return true;
    if (name.find("secret") != std::string_view::npos)
        return true;
    if (name.size() >= 6 && name.substr(name.size() - 6) == "_token")
        return true;
    return name == "vertex_key_file";
}

bool QtSettingsBridge::value_is_secret(const std::string& value) noexcept {
    return value.rfind("dpapi1:", 0) == 0 || value.rfind("enc1:", 0) == 0;
}

QSettings& QtSettingsBridge::qt_settings() {
    if (!qt_settings_) {
        qt_settings_ = std::make_unique<QSettings>(QSettings::IniFormat,
            QSettings::UserScope, QStringLiteral("AiDA"),
            QStringLiteral("AiDAStandalone"));
        diag::log_tagged_fmt("qt_settings", "qt_settings_open file=%s",
            qt_settings_->fileName().toUtf8().constData());
    }
    return *qt_settings_;
}

void QtSettingsBridge::save_qt_state(const char* key, const QByteArray& value) {
    if (!key || !*key)
        return;
    if (value.startsWith("dpapi1:") || value.startsWith("enc1:")) {
        diag::log_tagged_critical_fmt("qt_settings",
            "qt_state_secret_write_refused key=%s", key);
        return;
    }
    qt_settings().setValue(QStringLiteral("geometry/") + QString::fromLatin1(key), value);
}

QByteArray QtSettingsBridge::qt_state(const char* key) const {
    if (!key || !*key || !qt_settings_)
        return {};
    return qt_settings_->value(QStringLiteral("geometry/") + QString::fromLatin1(key))
        .toByteArray();
}

QString QtSettingsBridge::qt_settings_path() const {
    return qt_settings_ ? qt_settings_->fileName() : QString{};
}

}
