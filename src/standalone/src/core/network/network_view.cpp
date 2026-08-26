#include "network_view.hpp"
#include "../infra/executor.hpp"
#include "../infra/taskflow_runtime.hpp"
#include "executor_status.hpp"
#include "../ui/task_center.hpp"
#include "../ui/application_ui_runtime.hpp"

#include "../ai/standalone_chat.hpp"
#include "standalone_driver.hpp"

#include "protocol_parser.hpp"
#include "mitm_proxy.hpp"
#include "flow_serializer.hpp"
#include "cert_pin_bypass.hpp"
#include "cert_generator.hpp"
#include "ssl_keylog.hpp"

#include "toast_notification.hpp"
#include "../ui/ui_thread_dispatcher.hpp"
#include "../helpers/diag_log.hpp"
#include "../session/analysis_session.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/pcap_serialize.hpp"
#include "qt/network/decoder/decoder_pane.hpp"
#include "qt/network/fuzzer/fuzzer_controller.hpp"
#include "qt/network/websocket/ws_frame_store.hpp"
#include "qt/net/http_text_utils.hpp"

#include "burp/burp_module.hpp"
#include "burp/site_map.hpp"
#include "burp/scope.hpp"
#include "burp/cookie_jar.hpp"
#include "burp/issue.hpp"
#include "burp/intruder_view.hpp"
#include "burp/sequencer_view.hpp"
#include "burp/comparer.hpp"
#include "burp/match_replace_view.hpp"
#include "burp/session_handler_view.hpp"
#include "qt/net/qt_api_view.hpp"
#include "qt/net/qt_ws_editor_view.hpp"
#include "qt/net/qt_h2_editor_view.hpp"
#include "qt/net/qt_scanner_view.hpp"
#include "qt/net/qt_browser_launcher_view.hpp"
#include "burp/burp_logger_view.hpp"
#include "burp/browser_launch.hpp"
#include "burp/offensive/api_security_engine.hpp"
#include "burp/offensive/auth_attack_engine.hpp"
#include "burp/offensive/business_logic_engine.hpp"
#include "burp/offensive/client_attack_engine.hpp"
#include "burp/offensive/fuzzing_engine.hpp"
#include "burp/offensive/js_analysis_engine.hpp"
#include "burp/offensive/recon_engine.hpp"
#include "burp/offensive/server_attack_engine.hpp"
#include "burp/offensive/sqli_engine.hpp"
#include "burp/offensive/xss_engine.hpp"
#include "intercept/cert_profile_manager.hpp"
#include "intercept/diagnostics.hpp"
#include "intercept/instrumentation_provider.hpp"
#include "intercept/script_handoff.hpp"


#include "../workbench/workbench_shell_integration.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <charconv>
#include <climits>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <system_error>
#include <utility>
#include <vector>

namespace network_view {

using aida::qt::net::format_ip;
using aida::qt::net::protocol_name;
using aida::qt::net::tcp_state_name;
using aida::qt::net::format_bytes;
using aida::qt::net::format_rate;
using aida::qt::net::format_timestamp;
using aida::qt::net::filter_text_match;
using aida::qt::net::payload_display_text;
using aida::qt::net::capture_row_info_text;
using aida::qt::net::k_network_export_limit;
using aida::qt::net::network_now_ms;

static constexpr std::size_t k_max_repeater_entries = 128;

enum class network_exchange_action_t : std::uint8_t {
    repeater,
    fuzzer,
    intruder,
    scanner,
    comparer,
    compare_request_response,
    session_handling,
    cookies,
    match_replace,
    decoder,
    sequencer,
    camoufox,
    copy_url,
    copy_method,
    copy_status,
    copy_request,
    copy_response,
    copy_headers,
    copy_body,
    copy_artifact,
    scope_include,
    scope_exclude,
    save_export,
    create_issue,
    chat,
    agent,
    replay_live,
    remove
};

struct network_exchange_action_descriptor_t {
    network_exchange_action_t action;
    const char* id;
};

struct exchange_context_runtime_t {
    artifact_identity_t primary;
    artifact_identity_t related;
    bool primary_current = false;
    std::string unavailable_reason;
};

static void reset_common_exchange_actions();

static std::atomic<std::uint64_t> s_repeater_artifact_sequence{1};
static std::unordered_map<std::uint64_t, artifact_kind_t>
    s_repeater_selected_artifact_kinds;
static std::atomic<std::uint64_t> s_network_operation_sequence{1};

struct operational_review_binding_t {
    bool prepared = false;
    operational_command_t command = operational_command_t::capture_start;
    std::size_t retained_count = 0;
    filter_entry_t retained_rule;
    std::vector<std::uint32_t> retained_rule_ids;
    std::vector<std::uint64_t> retained_exchange_ids;
    ssl_keylog::retained_set_token retained_keylog_token;
    int filter_action = 0;
    int filter_direction = 0;
    int filter_protocol = 0;
    std::string filter_pid;
    std::string filter_port;
    std::string filter_ip;
};

static operational_review_binding_t s_operational_review;

static bool post_network_ui_completion(std::function<void()> completion) {
    if (!completion)
        return false;
    return aida::ui_thread::post(std::move(completion),
        "network", "ui_completion", "worker_result");
}

static open_view_handler_t s_open_view_handler;

void set_open_view_handler(open_view_handler_t handler) {
    s_open_view_handler = std::move(handler);
}

std::string open_view(const char* view_id) {
    if (!view_id || view_id[0] == '\0')
        return "The view identity is empty";
    if (s_open_view_handler)
        return s_open_view_handler(view_id);
    return "The view host is unavailable";
}

static exchange_context_display_fn s_exchange_context_display;

void set_exchange_context_display(exchange_context_display_fn fn) {
    s_exchange_context_display = std::move(fn);
}

static exchange_review_display_fn s_exchange_review_display;

void set_exchange_review_display(exchange_review_display_fn fn) {
    s_exchange_review_display = std::move(fn);
}

static exchange_remove_receipt_fn s_exchange_remove_receipt_display;

void set_exchange_remove_receipt_display(exchange_remove_receipt_fn fn) {
    s_exchange_remove_receipt_display = std::move(fn);
}

using save_file_dialog_fn = std::function<std::string(
    const char* title, const char* filter_pairs, const char* default_extension,
    const std::string& initial_name)>;
static save_file_dialog_fn s_save_file_dialog;

void set_save_file_dialog_handler(save_file_dialog_fn fn) {
    s_save_file_dialog = std::move(fn);
}

static std::function<void(const std::string&)> s_clipboard_text_handler;

void set_clipboard_text_handler(std::function<void(const std::string&)> fn) {
    s_clipboard_text_handler = std::move(fn);
}

static void network_clipboard_set(const std::string& text) {
    if (s_clipboard_text_handler)
        s_clipboard_text_handler(text);
}

static std::string register_network_operation(const char* action, const char* label,
                                              const char* owner_view, std::string target) {
    const std::string id = "network.operation." +
        std::to_string(s_network_operation_sequence.fetch_add(1, std::memory_order_acq_rel));
    aida::ui::task_center::task_registration_t registration;
    registration.id = id;
    registration.source = "human";
    registration.owner = "network";
    registration.owner_view = owner_view ? owner_view : "view.network.connections";
    registration.owner_action = action ? action : "network.operation";
    registration.target = std::move(target);
    registration.label = label ? label : "Network operation";
    registration.stage = "Queued";
    registration.progress = -1.0f;
    registration.cancellation_is_safe = false;
    registration.callbacks.focus = [view = registration.owner_view] {
        (void)open_view(view.c_str());
    };
    if (!aida::ui::task_center::register_task(std::move(registration)))
        return {};
    return id;
}

static void finish_network_operation(const std::string& id, bool success,
                                     std::string stage, std::string summary) {
    if (id.empty())
        return;
    (void)aida::ui::task_center::update_task(
        id,
        success ? aida::ui::task_center::task_state_t::completed
                : aida::ui::task_center::task_state_t::failed,
        1.0f, std::move(stage), std::move(summary));
}

std::uint64_t artifact_content_hash(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const std::uint8_t value : bytes) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<std::uint64_t>(bytes.size());
    hash *= 1099511628211ULL;
    return hash == 0 ? 1 : hash;
}

static std::uint64_t artifact_hash(const std::vector<std::uint8_t>& bytes) {
    return artifact_content_hash(bytes);
}

static std::uint64_t artifact_hash(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : text) {
        const auto value = static_cast<unsigned char>(character);
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<std::uint64_t>(text.size());
    hash *= 1099511628211ULL;
    return hash == 0 ? 1 : hash;
}

artifact_identity_t exchange_artifact_identity(const mitm_proxy::http_exchange& exchange,
                                                       artifact_kind_t kind) {
    artifact_identity_t identity;
    identity.kind = kind;
    identity.source_id = exchange.id;
    identity.timestamp = exchange.timestamp;
    identity.target_host = exchange.target_host;
    identity.target_port = exchange.target_port;
    identity.use_tls = exchange.is_tls;
    identity.parent_id = "network.exchange." + std::to_string(exchange.id);
    const bool response = kind == artifact_kind_t::response;
    identity.id = identity.parent_id + (response ? ".response" : ".request");
    identity.source_view_id = kind == artifact_kind_t::intercept_request
        ? "view.network.intercept" : "view.network.proxy";
    identity.label = std::string(response ? "Response #" : "Request #") + std::to_string(exchange.id);
    const auto& bytes = response ? exchange.raw_response : exchange.raw_request;
    identity.content_size = bytes.size();
    identity.content_hash = artifact_hash(bytes);
    return identity;
}

static artifact_identity_t repeater_artifact_identity(const repeater_entry_t& entry,
                                                       artifact_kind_t kind) {
    artifact_identity_t identity;
    const bool response = kind == artifact_kind_t::repeater_response;
    identity.kind = kind;
    identity.source_id = entry.id;
    identity.timestamp = response ? entry.response_timestamp : 0;
    identity.revision = entry.request_revision;
    identity.target_host = entry.host;
    identity.target_port = entry.port;
    identity.use_tls = entry.use_tls;
    identity.parent_id = entry.source_artifact_id;
    identity.session_id = entry.source_session_id;
    identity.id = "network.repeater." + std::to_string(entry.id) +
        (response ? ".response." + std::to_string(entry.response_timestamp)
                  : ".request." + std::to_string(entry.request_revision));
    identity.source_view_id = "view.network.repeater";
    identity.label = std::string(response ? "Repeater response #" : "Repeater request #") +
        std::to_string(entry.id);
    const std::string& text = response ? entry.raw_response : entry.raw_request;
    identity.content_size = text.size();
    const std::uint64_t retained_hash = response
        ? entry.response_hash : entry.request_hash;
    identity.content_hash = text.empty() ? 0
        : retained_hash != 0 ? retained_hash : artifact_hash(std::string_view(text));
    return identity;
}

struct network_selection_publication_t {
    std::weak_ptr<aida::analysis::analysis_workspace_t> workspace;
    std::uint64_t workspace_generation = 0;
    aida::workbench::document_id_t document;
    std::string entity_key;
    std::string source_view_id;
    std::string analysis_session_id;
    std::string binary_id;
};

static network_selection_publication_t s_network_selection_publication;

struct network_artifact_workspace_binding_t {
    std::string analysis_session_id;
    std::string binary_id;
};

static std::unordered_map<std::string, network_artifact_workspace_binding_t>
    s_network_artifact_workspace_bindings;
static constexpr std::size_t k_max_network_artifact_workspace_bindings = 4096;

static std::string network_analysis_session_id(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace) {
    if (!workspace)
        return {};
    const auto count = analysis_session::session_count();
    for (std::size_t index = 0; index < count; ++index) {
        const auto session = analysis_session::session_handle_at(index);
        if (session && session->workspace == workspace)
            return session->id;
    }
    return {};
}

static std::string network_artifact_binding_key(const artifact_identity_t& identity) {
    std::string key;
    key.reserve(identity.source_view_id.size() + identity.parent_id.size() + identity.id.size() + 48);
    key.append(identity.source_view_id);
    key.push_back('|');
    key.append(identity.session_id);
    key.push_back('|');
    key.append(identity.parent_id.empty() ? identity.id : identity.parent_id);
    key.push_back('|');
    key.append(std::to_string(identity.source_id));
    key.push_back('|');
    key.append(std::to_string(
        identity.kind == artifact_kind_t::repeater_request ||
        identity.kind == artifact_kind_t::repeater_response
            ? 0 : identity.timestamp));
    return key;
}

static bool bind_network_artifact_workspace(
    const artifact_identity_t& identity,
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    std::string& session_id,
    std::string& binary_id) {
    if (!workspace)
        return false;
    session_id = network_analysis_session_id(workspace);
    binary_id = workspace->identity().binary_id().to_hex();
    if (session_id.empty() || binary_id.empty())
        return false;
    const std::string key = network_artifact_binding_key(identity);
    auto found = s_network_artifact_workspace_bindings.find(key);
    if (found != s_network_artifact_workspace_bindings.end())
        return found->second.analysis_session_id == session_id &&
            found->second.binary_id == binary_id;
    if (s_network_artifact_workspace_bindings.size() >=
        k_max_network_artifact_workspace_bindings)
        return false;
    s_network_artifact_workspace_bindings.emplace(key,
        network_artifact_workspace_binding_t{session_id, binary_id});
    return true;
}

static std::string network_selection_entity_key(const artifact_identity_t& identity) {
    std::string key = "network.artifact|";
    const auto append_component = [&](std::string_view value) {
        key.append(std::to_string(value.size()));
        key.push_back(':');
        key.append(value);
        key.push_back('|');
    };
    append_component(identity.source_view_id);
    append_component(identity.id);
    append_component(identity.session_id);
    key.append(std::to_string(static_cast<unsigned>(identity.kind)));
    key.push_back('|');
    key.append(std::to_string(identity.source_id));
    key.push_back('|');
    key.append(std::to_string(identity.timestamp));
    key.push_back('|');
    key.append(std::to_string(identity.revision));
    key.push_back('|');
    key.append(std::to_string(identity.content_hash));
    key.push_back('|');
    key.append(std::to_string(identity.content_size));
    key.push_back('|');
    key.append(std::to_string(identity.target_port));
    key.push_back('|');
    key.push_back(identity.use_tls ? '1' : '0');
    key.push_back('|');
    key.push_back(identity.raw_protocol ? '1' : '0');
    if (key.size() <= aida::workbench::k_max_document_key_bytes)
        return key;
    return "network.artifact.hash|" + std::to_string(artifact_hash(key));
}

static bool publish_workbench_network_selection(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    const aida::workbench::selection_context_t& selection,
    aida::workbench::document_id_t requested_document,
    aida::workbench::document_id_t& published_document) {
    if (!workspace || workspace->closing() || workspace->closed())
        return false;
    aida::workbench::document_local_cursor_t cursor;
    aida::workbench::workbench_shell_workspace_context_t output;
    auto& runtime = aida::workbench::workbench_shell_runtime_t::instance();
    const auto result = requested_document.value != 0
        ? runtime.publish_document_selection(workspace, requested_document, selection, cursor,
            aida::workbench::navigation_origin_t::user, output)
        : runtime.publish_selection(workspace, selection, cursor,
            aida::workbench::navigation_origin_t::user, output);
    if (!result)
        return false;
    if (requested_document.value != 0) {
        published_document = requested_document;
        return true;
    }
    const auto focused = std::find_if(output.persistence.views.begin(),
        output.persistence.views.end(), [](const auto& view) { return view.focused; });
    if (focused == output.persistence.views.end() || focused->document.value == 0)
        return false;
    published_document = focused->document;
    return true;
}

static bool document_workbench_selection_matches(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    aida::workbench::document_id_t document_id,
    std::string_view entity_key) {
    if (!workspace || document_id.value == 0 || entity_key.empty())
        return false;
    aida::workbench::workbench_shell_workspace_context_t context;
    if (!aida::workbench::workbench_shell_runtime_t::instance()
            .workspace_context(workspace, context))
        return false;
    const auto document = std::find_if(context.persistence.documents.begin(),
        context.persistence.documents.end(), [&](const auto& candidate) {
            return candidate.id == document_id;
        });
    return document != context.persistence.documents.end() &&
        document->local_state.selection.kind == aida::workbench::selection_kind_t::entity &&
        document->local_state.selection.entity_key == entity_key;
}

static void clear_owned_network_selection() {
    auto publication = std::move(s_network_selection_publication);
    s_network_selection_publication = {};
    auto workspace = publication.workspace.lock();
    if (!workspace || workspace->closing() || workspace->closed() ||
        publication.document.value == 0 ||
        !document_workbench_selection_matches(
            workspace, publication.document, publication.entity_key))
        return;
    aida::workbench::selection_context_t empty;
    aida::workbench::document_id_t ignored;
    static_cast<void>(publish_workbench_network_selection(
        workspace, empty, publication.document, ignored));
}

void clear_stale_network_selection(std::string_view source_view_id) {
    if (s_network_selection_publication.source_view_id != source_view_id)
        return;
    clear_owned_network_selection();
}

void publish_network_selection(const artifact_identity_t& identity, bool force) {
    if (!identity.valid() || identity.source_view_id.empty()) {
        clear_owned_network_selection();
        return;
    }
    auto workspace = analysis_session::active_workspace();
    if (!workspace || workspace->closing() || workspace->closed()) {
        clear_owned_network_selection();
        return;
    }
    std::string analysis_session_id;
    std::string binary_id;
    if (!bind_network_artifact_workspace(
            identity, workspace, analysis_session_id, binary_id)) {
        clear_owned_network_selection();
        return;
    }
    const auto generation = workspace->generation();
    const std::string entity_key = network_selection_entity_key(identity);
    auto previous_workspace = s_network_selection_publication.workspace.lock();
    const bool same_publication = previous_workspace.get() == workspace.get() &&
        s_network_selection_publication.workspace_generation == generation &&
        s_network_selection_publication.analysis_session_id == analysis_session_id &&
        s_network_selection_publication.binary_id == binary_id &&
        s_network_selection_publication.entity_key == entity_key &&
        s_network_selection_publication.document.value != 0;
    if (same_publication && !force)
        return;
    const aida::workbench::document_id_t requested_document = same_publication
        ? s_network_selection_publication.document : aida::workbench::document_id_t{};
    if (!same_publication && s_network_selection_publication.document.value != 0)
        clear_owned_network_selection();
    aida::workbench::selection_context_t selection;
    selection.kind = aida::workbench::selection_kind_t::entity;
    selection.entity_key = entity_key;
    aida::workbench::document_id_t published_document;
    if (!publish_workbench_network_selection(
            workspace, selection, requested_document, published_document)) {
        clear_owned_network_selection();
        return;
    }
    s_network_selection_publication.workspace = workspace;
    s_network_selection_publication.workspace_generation = generation;
    s_network_selection_publication.document = published_document;
    s_network_selection_publication.entity_key = entity_key;
    s_network_selection_publication.source_view_id = identity.source_view_id;
    s_network_selection_publication.analysis_session_id = std::move(analysis_session_id);
    s_network_selection_publication.binary_id = std::move(binary_id);
}

struct repeater_artifact_publication_t {
    std::vector<std::shared_ptr<const artifact_snapshot_t>> requests;
};

static std::shared_ptr<const repeater_artifact_publication_t>
    s_repeater_artifact_publication;
static std::atomic<bool> s_repeater_artifact_publication_ready{false};

static void publish_repeater_request_artifacts(const state_t& state) noexcept {
    const auto previous = std::atomic_load_explicit(
        &s_repeater_artifact_publication, std::memory_order_acquire);
    s_repeater_artifact_publication_ready.store(false, std::memory_order_release);
    std::atomic_store_explicit(&s_repeater_artifact_publication,
        std::shared_ptr<const repeater_artifact_publication_t>{},
        std::memory_order_release);
    try {
        auto next = std::make_shared<repeater_artifact_publication_t>();
        next->requests.reserve(state.repeater_entries.size());
        std::unordered_map<std::uint64_t,
            std::shared_ptr<const artifact_snapshot_t>> previous_by_source;
        if (previous) {
            previous_by_source.reserve(previous->requests.size());
            for (const auto& candidate : previous->requests) {
                if (candidate)
                    previous_by_source.emplace(
                        candidate->identity.source_id, candidate);
            }
        }
        for (const auto& entry : state.repeater_entries) {
            if (!entry || entry->id == 0 || entry->raw_request.empty())
                continue;
            const artifact_identity_t identity = repeater_artifact_identity(
                *entry, artifact_kind_t::repeater_request);
            if (!identity.valid())
                continue;
            std::shared_ptr<const artifact_snapshot_t> retained;
            const auto previous_found = previous_by_source.find(identity.source_id);
            if (previous_found != previous_by_source.end()) {
                const auto& candidate = previous_found->second;
                if (candidate && candidate->identity.id == identity.id &&
                    candidate->identity.revision == identity.revision &&
                    candidate->identity.content_size == identity.content_size &&
                    candidate->identity.content_hash == identity.content_hash &&
                    candidate->identity.target_host == identity.target_host &&
                    candidate->identity.target_port == identity.target_port &&
                    candidate->identity.use_tls == identity.use_tls &&
                    candidate->identity.parent_id == identity.parent_id)
                    retained = candidate;
            }
            if (!retained) {
                auto snapshot = std::make_shared<artifact_snapshot_t>();
                snapshot->identity = identity;
                snapshot->bytes.assign(entry->raw_request.begin(), entry->raw_request.end());
                if (snapshot->identity.content_size != snapshot->bytes.size() ||
                    snapshot->identity.content_hash != artifact_hash(snapshot->bytes))
                    continue;
                retained = std::move(snapshot);
            }
            next->requests.push_back(std::move(retained));
        }
        std::shared_ptr<const repeater_artifact_publication_t> immutable =
            std::move(next);
        std::atomic_store_explicit(&s_repeater_artifact_publication,
            std::move(immutable), std::memory_order_release);
        s_repeater_artifact_publication_ready.store(true, std::memory_order_release);
    } catch (...) {
    }
}

std::uint64_t repeater_next_entry_id() {
    return s_repeater_artifact_sequence.fetch_add(1, std::memory_order_relaxed);
}

void repeater_publish_request_artifacts() {
    publish_repeater_request_artifacts(g_state);
}

artifact_identity_t repeater_entry_identity(const repeater_entry_t& entry,
                                            artifact_kind_t kind) {
    return repeater_artifact_identity(entry, kind);
}

static artifact_identity_t websocket_artifact_identity(const state_t::ws_frame_entry_t& frame) {
    artifact_identity_t identity;
    identity.kind = artifact_kind_t::websocket_frame;
    identity.id = "network.websocket." + std::to_string(frame.exchange_id) + "." +
        std::to_string(frame.timestamp) + (frame.is_outbound ? ".out" : ".in");
    identity.parent_id = "network.exchange." + std::to_string(frame.exchange_id);
    identity.source_view_id = "view.network.websocket";
    identity.source_id = frame.exchange_id;
    identity.timestamp = frame.timestamp;
    identity.content_size = frame.payload.size();
    identity.content_hash = artifact_hash(frame.payload);
    identity.label = std::string(frame.is_outbound ? "Outbound" : "Inbound") +
        " WebSocket frame";
    identity.target_host = frame.host;
    identity.target_port = frame.port;
    return identity;
}

template <typename Fn>
static bool post_network_task(const char* name,
                              aida::infra::executor::domain_t domain,
                              const char* thread_class,
                              Fn&& fn,
                              bool register_with_task_center = true) {
    try {
        std::string task_name = name ? name : "?";
        std::function<void()> task(std::forward<Fn>(fn));
        aida::infra::executor::submission_t sub;
        sub.owner_subsystem = "network.view";
        sub.label = name ? name : "network.task";
        sub.thread_class = thread_class ? thread_class : "bounded_task";
        sub.domain = domain;
        sub.priority = 3;
        sub.body = [task_name, task = std::move(task), domain]() mutable {
            diag::log_tagged_fmt("network",
                "executor_task_enter name=%s domain=%s tid=%lu",
                task_name.c_str(),
                aida::infra::executor::domain_name(domain),
                static_cast<unsigned long>(GetCurrentThreadId()));
            task();
            diag::log_tagged_fmt("network",
                "executor_task_exit name=%s domain=%s tid=%lu",
                task_name.c_str(),
                aida::infra::executor::domain_name(domain),
                static_cast<unsigned long>(GetCurrentThreadId()));
        };
        auto submit_result = aida::infra::executor::submit(std::move(sub));
        bool ok = submit_result.submitted;
        if (ok && submit_result.task_id != 0 && register_with_task_center) {
            const std::string task_label = [&task_name] {
                std::string label;
                label.reserve(task_name.size());
                bool capitalize = true;
                for (const char character : task_name) {
                    if (character == '_' || character == '.') {
                        label.push_back(' ');
                        capitalize = true;
                    } else {
                        label.push_back(capitalize
                            ? static_cast<char>(std::toupper(static_cast<unsigned char>(character)))
                            : character);
                        capitalize = false;
                    }
                }
                return label.empty() ? std::string("Network task") : label;
            }();
            std::string owner_view = "view.network.connections";
            if (task_name.find("capture") != std::string::npos) owner_view = "view.network.capture";
            else if (task_name.find("repeater") != std::string::npos) owner_view = "view.network.repeater";
            else if (task_name.find("fuzzer") != std::string::npos) owner_view = "view.network.fuzzer";
            else if (task_name.find("offensive") != std::string::npos) owner_view = "view.network.offensive";
            else if (task_name.find("pcap") != std::string::npos) owner_view = "view.network.pcap";
            else if (task_name.find("har") != std::string::npos) owner_view = "view.network.proxy";
            else if (task_name.find("script") != std::string::npos) owner_view = "view.network.scripting";
            aida::ui::task_center::task_registration_t registration;
            registration.owner = "network";
            registration.owner_view = owner_view;
            registration.owner_action = task_name;
            registration.label = task_label;
            registration.stage = "Queued";
            registration.cancellation_is_safe = false;
            registration.callbacks.focus = [owner_view] {
                (void)open_view(owner_view.c_str());
            };
            (void)aida::ui::task_center::register_executor_job(submit_result.task_id, std::move(registration));
        }
        diag::log_tagged_fmt("network", "executor_post name=%s domain=%s ok=%d reject=%s",
            name ? name : "?",
            aida::infra::executor::domain_name(domain),
            ok ? 1 : 0,
            submit_result.reject_reason.empty() ? "<none>" : submit_result.reject_reason.c_str());
        return ok;
    } catch (const std::exception& e) {
        diag::log_tagged_fmt("network", "executor_post_cpp_exception name=%s what=%s",
            name ? name : "?", e.what());
        return false;
    } catch (...) {
        diag::log_tagged_fmt("network", "executor_post_unknown_exception name=%s",
            name ? name : "?");
        return false;
    }
}

static bool initialize_executor_for_network() {
    try {
        const auto snap = aida::infra::taskflow_runtime::active_snapshot(0);
        const auto work = aida::network::executor_status::work_stats();
        const auto service = aida::network::executor_status::service_stats();
        const auto critical = aida::network::executor_status::critical_stats();
        const bool accepting = snap.accepting && !snap.shutting_down;
        diag::log_tagged_fmt("network",
            "executor_runtime_status accepting=%d shutting_down=%d total_active=%u executor_work_pending=%llu executor_service_pending=%llu executor_critical_pending=%llu",
            snap.accepting ? 1 : 0,
            snap.shutting_down ? 1 : 0,
            static_cast<unsigned>(snap.total_active),
            static_cast<unsigned long long>(work.pending),
            static_cast<unsigned long long>(service.pending),
            static_cast<unsigned long long>(critical.pending));
        return accepting;
    } catch (const std::exception& e) {
        diag::log_tagged_fmt("network", "executor_runtime_status_cpp_exception what=%s", e.what());
        return false;
    } catch (...) {
        diag::log_tagged("network", "executor_runtime_status_unknown_exception");
        return false;
    }
}

static std::atomic<bool> s_cert_diagnostics_pending{false};
static std::atomic<bool> s_cert_handoff_pending{false};
static std::atomic<std::uint64_t> s_cert_diagnostics_serial{0};
static std::atomic<std::uint64_t> s_cert_handoff_serial{0};
static cert_diagnostics_sink_t s_cert_diagnostics_sink;
static cert_handoff_sink_t s_cert_handoff_sink;

void set_cert_diagnostics_sink(cert_diagnostics_sink_t sink) {
    s_cert_diagnostics_sink = std::move(sink);
}

void set_cert_handoff_sink(cert_handoff_sink_t sink) {
    s_cert_handoff_sink = std::move(sink);
}

bool cert_diagnostics_pending() {
    return s_cert_diagnostics_pending.load(std::memory_order_acquire);
}

bool cert_handoff_pending() {
    return s_cert_handoff_pending.load(std::memory_order_acquire);
}

static std::string cert_diag_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

static bool cert_diag_has_any(const std::string& value, std::initializer_list<const char*> needles) {
    const std::string lowered = cert_diag_lower(value);
    for (const char* needle : needles) {
        if (lowered.find(needle) != std::string::npos) return true;
    }
    return false;
}

static void cert_diag_apply_proxy_observations(cert_intercept::diagnostic_context_t& context) {
    auto observations = mitm_proxy::get_tls_observations(64);
    for (const auto& obs : observations) {
        std::string evidence = std::string(mitm_proxy::to_string(obs.kind)) + " host=" + obs.target_host;
        if (!obs.sni.empty()) evidence += " sni=" + obs.sni;
        if (!obs.alpn.empty()) evidence += " alpn=" + obs.alpn;
        if (!obs.detail.empty()) evidence += " detail=" + obs.detail;
        switch (obs.kind) {
        case mitm_proxy::tls_observation_kind_t::http_tls:
            context.interception_observed = true;
            break;
        case mitm_proxy::tls_observation_kind_t::sni_authority_mismatch:
            context.hostname_san_mismatch_observed = true;
            context.interception_still_failing = true;
            context.observation_evidence.push_back(std::move(evidence));
            break;
        case mitm_proxy::tls_observation_kind_t::client_handshake_failed:
            if (cert_diag_has_any(obs.detail, {"certificate", "unknown ca", "bad certificate", "required", "alert"})) {
                context.browser_trust_policy_or_ct_block = true;
                context.interception_still_failing = true;
                context.observation_evidence.push_back(std::move(evidence));
            }
            break;
        case mitm_proxy::tls_observation_kind_t::upstream_handshake_failed:
            if (cert_diag_has_any(obs.detail, {"certificate required", "bad certificate", "handshake failure", "alert certificate"})) {
                context.mutual_tls_requested = true;
                context.interception_still_failing = true;
                context.observation_evidence.push_back(std::move(evidence));
            }
            break;
        case mitm_proxy::tls_observation_kind_t::non_http_tls:
            context.non_http_tls_observed = true;
            context.interception_still_failing = true;
            context.observation_evidence.push_back(std::move(evidence));
            break;
        default:
            break;
        }
    }
}

static bool driver_feature_ready(const char* feature, int iter = -1) {
    bool drv_ok = driver_bridge::using_kernel_driver();
    if (!drv_ok && (iter < 0 || iter <= 3 || (iter % 60) == 0)) {
        diag::log_tagged_fmt("network", "%s_driver_gate drv_ok=%d iter=%d",
            feature ? feature : "network",
            drv_ok ? 1 : 0,
            iter);
    }
    if (!drv_ok)
        return false;
    return true;
}

static connection_snapshot_sink_t s_connection_snapshot_sink;
static capture_batch_sink_t s_capture_batch_sink;
static dns_batch_sink_t s_dns_batch_sink;
static bandwidth_snapshot_sink_t s_bandwidth_snapshot_sink;
static filters_changed_sink_t s_filters_changed_sink;

void set_connection_snapshot_sink(connection_snapshot_sink_t sink) {
    s_connection_snapshot_sink = std::move(sink);
}
void set_capture_batch_sink(capture_batch_sink_t sink) {
    s_capture_batch_sink = std::move(sink);
}
void set_dns_batch_sink(dns_batch_sink_t sink) {
    s_dns_batch_sink = std::move(sink);
}
void set_bandwidth_snapshot_sink(bandwidth_snapshot_sink_t sink) {
    s_bandwidth_snapshot_sink = std::move(sink);
}
void set_filters_changed_sink(filters_changed_sink_t sink) {
    s_filters_changed_sink = std::move(sink);
}

std::shared_ptr<const std::vector<packet_entry_t>> capture_packets_snapshot() {
    std::lock_guard<std::mutex> lock(g_state.cap_mutex);
    return std::make_shared<const std::vector<packet_entry_t>>(
        g_state.captured_packets.begin(), g_state.captured_packets.end());
}

static std::mutex s_capture_control_mutex;
static std::string s_capture_control_status;

static void set_capture_control_status(const char* text) {
    std::lock_guard<std::mutex> lock(s_capture_control_mutex);
    s_capture_control_status = text ? text : "";
}

std::string capture_control_status_text() {
    std::lock_guard<std::mutex> lock(s_capture_control_mutex);
    return s_capture_control_status;
}

std::size_t capture_buffered_count() {
    std::lock_guard<std::mutex> lock(g_state.cap_mutex);
    return g_state.captured_packets.size();
}

std::shared_ptr<const std::vector<dns_entry_t>> dns_entries_snapshot() {
    std::lock_guard<std::mutex> lock(g_state.dns_mutex);
    return std::make_shared<const std::vector<dns_entry_t>>(
        g_state.dns_entries.begin(), g_state.dns_entries.end());
}


static void connection_poll_thread(state_t& state) {
    diag::log_tagged_fmt("network", "connection_poll_thread_started auto_refresh=%d filter_pid=%u filter_proto=%u",
        state.conn_auto_refresh_enabled.load(std::memory_order_acquire) ? 1 : 0, state.conn_filter_pid, state.conn_filter_protocol);
    int poll_iter = 0;
    while (state.conn_polling.load()) {
        bool drv_ok = driver_feature_ready("connection_poll", poll_iter);
        ++poll_iter;
        const bool pane_visible = state.conn_pane_visible.load(std::memory_order_acquire);
        const bool auto_refresh = state.conn_auto_refresh_enabled.load(std::memory_order_acquire);
        if (drv_ok && auto_refresh && pane_visible) {
            auto raw_conns = driver_bridge::enumerate_connections(
                state.conn_filter_pid, state.conn_filter_protocol);

            auto entries = std::make_shared<std::vector<connection_entry_t>>();
            entries->reserve(raw_conns.size());
            for (auto& c : raw_conns) {
                connection_entry_t e;
                e.pid = c.pid;
                e.protocol = c.protocol;
                e.state = c.state;
                e.local_port = c.local_port;
                e.remote_port = c.remote_port;
                e.address_family = c.address_family;
                memcpy(e.local_addr, c.local_addr, 16);
                memcpy(e.remote_addr, c.remote_addr, 16);
                entries->push_back(std::move(e));
            }

            const size_t count = entries->size();
            if (s_connection_snapshot_sink)
                s_connection_snapshot_sink(std::move(entries));
            if (poll_iter <= 3 || (poll_iter % 60) == 0) {
                diag::log_tagged_fmt("network", "connection_poll iter=%d drv_ok=1 count=%zu", poll_iter, count);
            }
        } else if (poll_iter <= 3 || (poll_iter % 60) == 0) {
            diag::log_tagged_fmt("network", "connection_poll iter=%d drv_ok=%d auto_refresh=%d pane_visible=%d skipped",
                poll_iter, drv_ok ? 1 : 0, auto_refresh ? 1 : 0, pane_visible ? 1 : 0);
        }

        std::unique_lock<std::mutex> lk(state.conn_cv_mutex);
        state.conn_cv.wait_for(lk, std::chrono::milliseconds(1000), [&state]() {
            return !state.conn_polling.load();
        });
    }
    diag::log_tagged("network", "connection_poll_thread_exited");
}

static void capture_poll_thread(state_t& state) {
    diag::log_tagged("network", "capture_poll_thread_started");
    state.cap_thread_alive.store(true);
    int poll_iter = 0;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(state.cap_cv_mutex);
            state.cap_cv.wait(lk, [&state]() {
                return state.cap_polling.load() || !state.cap_thread_alive.load();
            });
        }
        if (!state.cap_thread_alive.load())
            break;
        poll_iter = 0;
        diag::log_tagged("network", "capture_poll_loop_armed");
        while (state.cap_polling.load()) {
            bool drv_ok = driver_feature_ready("capture_poll", poll_iter);
            if (poll_iter < 5 || (poll_iter % 100) == 0) {
                diag::log_tagged_fmt("network", "capture_poll iter=%d drv_ok=%d", poll_iter, drv_ok ? 1 : 0);
            }
            ++poll_iter;
            if (drv_ok) {
                auto raw_packets = driver_bridge::get_captured_packets(64);

                if (!raw_packets.empty()) {
                    const size_t batch_n = raw_packets.size();
                    auto batch = std::make_shared<std::vector<packet_entry_t>>();
                    batch->reserve(batch_n);
                    std::size_t trimmed_from_front = 0;
                    {
                        std::lock_guard<std::mutex> lock(state.cap_mutex);
                        for (auto& p : raw_packets) {
                            packet_entry_t entry;
                            entry.timestamp = p.timestamp;
                            entry.pid = p.pid;
                            entry.protocol = static_cast<uint8_t>(p.protocol);
                            entry.direction = static_cast<uint8_t>(p.direction);
                            entry.src_port = static_cast<uint16_t>(p.local_port);
                            entry.dst_port = static_cast<uint16_t>(p.remote_port);
                            memcpy(entry.src_addr, p.local_addr, 16);
                            memcpy(entry.dst_addr, p.remote_addr, 16);
                            entry.payload_size = p.payload_size;
                            entry.payload = p.payload;

                            auto det = protocol_parser::detect_protocol(
                                p.payload.data(), p.payload.size(),
                                static_cast<uint16_t>(p.local_port), static_cast<uint16_t>(p.remote_port),
                                p.protocol);
                            entry.protocol_label = det.label;
                            entry.summary = det.summary;

                            batch->push_back(std::move(entry));
                        }
                        for (const auto& entry : *batch) {
                            state.captured_packets.push_back(entry);
                            while (state.captured_packets.size() > state.cap_max_packets) {
                                state.captured_packets.pop_front();
                                ++trimmed_from_front;
                            }
                        }
                    }
                    if (poll_iter <= 5 || (poll_iter % 50) == 0) {
                        diag::log_tagged_fmt("network", "capture_poll_batch packets=%zu trimmed_from_front=%zu",
                            batch_n, trimmed_from_front);
                    }
                    if (s_capture_batch_sink)
                        s_capture_batch_sink(std::move(batch), trimmed_from_front);
                }
            }

            for (int i = 0; i < 10 && state.cap_polling.load(); i++)
                Sleep(10);
        }
        diag::log_tagged_fmt("network", "capture_poll_loop_idle iter=%d", poll_iter);
    }
    diag::log_tagged("network", "capture_poll_thread_exited");
}

static void dns_poll_thread(state_t& state) {
    diag::log_tagged("network", "dns_poll_thread_started");
    state.dns_thread_alive.store(true);
    int poll_iter = 0;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(state.dns_cv_mutex);
            state.dns_cv.wait(lk, [&state]() {
                return state.dns_polling.load() || !state.dns_thread_alive.load();
            });
        }
        if (!state.dns_thread_alive.load())
            break;
        poll_iter = 0;
        diag::log_tagged("network", "dns_poll_loop_armed");
        while (state.dns_polling.load()) {
            bool drv_ok = driver_feature_ready("dns_poll", poll_iter);
            if (poll_iter < 5 || (poll_iter % 100) == 0) {
                diag::log_tagged_fmt("network", "dns_poll iter=%d drv_ok=%d filter_pid=%u",
                    poll_iter, drv_ok ? 1 : 0, state.dns_filter_pid);
            }
            ++poll_iter;
            if (drv_ok) {
                auto raw_dns = driver_bridge::get_dns_queries(state.dns_filter_pid);

                if (!raw_dns.empty()) {
                    auto batch = std::make_shared<std::vector<dns_entry_t>>();
                    batch->reserve(raw_dns.size());
                    std::size_t trimmed_from_front = 0;
                    {
                        std::lock_guard<std::mutex> lock(state.dns_mutex);
                        for (auto& d : raw_dns) {
                            bool duplicate = false;
                            const auto recent_count = static_cast<std::ptrdiff_t>((std::min)(
                                static_cast<std::size_t>(256), state.dns_entries.size()));
                            for (auto it = state.dns_entries.rbegin();
                                 it != state.dns_entries.rend() && it != state.dns_entries.rbegin() + recent_count;
                                 ++it) {
                                if (it->timestamp == d.timestamp && it->domain == d.domain && it->pid == d.pid) {
                                    duplicate = true;
                                    break;
                                }
                            }
                            if (!duplicate) {
                                dns_entry_t e;
                                e.timestamp = d.timestamp;
                                e.pid = d.pid;
                                e.query_type = static_cast<uint16_t>(d.query_type);
                                e.domain = d.domain;
                                e.resolved_addr = format_ip(d.resolved_addr, 2);
                                e.response_code = d.response_code;
                                e.ttl = d.ttl;
                                state.dns_entries.push_back(e);
                                batch->push_back(std::move(e));
                            }
                        }
                        while (state.dns_entries.size() > state.dns_max_entries) {
                            state.dns_entries.pop_front();
                            ++trimmed_from_front;
                        }
                    }
                    if (!batch->empty()) {
                        diag::log_tagged_fmt("network", "dns_poll_batch raw=%zu added=%zu total=%zu",
                            raw_dns.size(), batch->size(),
                            trimmed_from_front);
                        if (s_dns_batch_sink)
                            s_dns_batch_sink(std::move(batch), trimmed_from_front);
                    }
                }
            }

            for (int i = 0; i < 50 && state.dns_polling.load(); i++)
                Sleep(10);
        }
        diag::log_tagged_fmt("network", "dns_poll_loop_idle iter=%d", poll_iter);
    }
    diag::log_tagged("network", "dns_poll_thread_exited");
}

static void bandwidth_poll_thread(state_t& state) {
    diag::log_tagged("network", "bandwidth_poll_thread_started");
    state.bw_thread_alive.store(true);
    int poll_iter = 0;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(state.bw_cv_mutex);
            state.bw_cv.wait(lk, [&state]() {
                return state.bw_polling.load() || !state.bw_thread_alive.load();
            });
        }
        if (!state.bw_thread_alive.load())
            break;
        poll_iter = 0;
        diag::log_tagged("network", "bandwidth_poll_loop_armed");
        while (state.bw_polling.load()) {
            ++poll_iter;
            if (driver_bridge::using_kernel_driver()) {
                auto raw_bw = driver_bridge::get_bw_per_process();
                if (poll_iter <= 3 || (poll_iter % 60) == 0) {
                    diag::log_tagged_fmt("network", "bandwidth_poll iter=%d processes=%zu", poll_iter, raw_bw.size());
                }

            std::vector<bw_entry_t> old_entries;
            {
                std::lock_guard<std::mutex> lock(state.bw_mutex);
                old_entries = state.bw_entries;
            }

            std::vector<bw_entry_t> entries;
            entries.reserve(raw_bw.size());
            for (auto& b : raw_bw) {
                bw_entry_t e;
                e.pid = b.pid;
                e.bytes_in = b.bytes_recv;
                e.bytes_out = b.bytes_sent;
                e.rate_in = 0.f;
                e.rate_out = 0.f;

                for (auto& old : old_entries) {
                    if (old.pid == b.pid) {
                        if (old.bytes_in > 0 || old.bytes_out > 0) {
                            float dt = 0.5f;
                            e.rate_in = static_cast<float>(b.bytes_recv > old.bytes_in ? b.bytes_recv - old.bytes_in : 0) / dt;
                            e.rate_out = static_cast<float>(b.bytes_sent > old.bytes_out ? b.bytes_sent - old.bytes_out : 0) / dt;
                        }
                        memcpy(e.rate_history, old.rate_history, sizeof(e.rate_history));
                        e.history_index = old.history_index;
                        break;
                    }
                }

                e.rate_history[e.history_index % 64] = e.rate_in + e.rate_out;
                e.history_index++;

                entries.push_back(std::move(e));
            }

            bool substantively_unchanged = old_entries.size() == entries.size();
            if (substantively_unchanged) {
                for (const auto& entry : entries) {
                    bool matched_zero = false;
                    for (const auto& old : old_entries) {
                        if (old.pid == entry.pid) {
                            matched_zero = old.bytes_in == entry.bytes_in &&
                                old.bytes_out == entry.bytes_out &&
                                old.rate_in == 0.f && old.rate_out == 0.f;
                            break;
                        }
                    }
                    if (!matched_zero) {
                        substantively_unchanged = false;
                        break;
                    }
                }
            }

            std::shared_ptr<const std::vector<bw_entry_t>> bandwidth_snapshot;
            {
                std::lock_guard<std::mutex> lock(state.bw_mutex);
                state.bw_entries = std::move(entries);
                if (!substantively_unchanged)
                    bandwidth_snapshot = std::make_shared<const std::vector<bw_entry_t>>(state.bw_entries);
            }
            if (bandwidth_snapshot && s_bandwidth_snapshot_sink)
                s_bandwidth_snapshot_sink(std::move(bandwidth_snapshot));
            }


            for (int i = 0; i < 50 && state.bw_polling.load(); i++)
                Sleep(10);
        }
        diag::log_tagged_fmt("network", "bandwidth_poll_loop_idle iter=%d", poll_iter);
    }
    diag::log_tagged("network", "bandwidth_poll_thread_exited");
}


static bool start_connection_worker(state_t& state) {
    if (!state.conn_thread_done.load(std::memory_order_acquire))
        return true;
    state.conn_polling.store(true);
    state.conn_thread_done.store(false, std::memory_order_release);
    if (post_network_task("connection_poll", aida::infra::executor::domain_t::feature_worker, "bounded_task", []() {
            try {
                connection_poll_thread(g_state);
            } catch (const std::exception& e) {
                diag::log_tagged_fmt("network", "connection_poll_cpp_exception what=%s", e.what());
            } catch (...) {
                diag::log_tagged("network", "connection_poll_unknown_exception");
            }
            g_state.conn_thread_done.store(true, std::memory_order_release);
        })) {
        return true;
    }
    state.conn_polling.store(false);
    state.conn_thread_done.store(true, std::memory_order_release);
    diag::log_tagged("network", "connection_worker_post_failed");
    return false;
}

static bool start_capture_worker(state_t& state) {
    if (!state.cap_thread_done.load(std::memory_order_acquire) &&
        state.cap_thread_alive.load(std::memory_order_acquire)) {
        return true;
    }
    state.cap_thread_alive.store(true, std::memory_order_release);
    state.cap_thread_done.store(false, std::memory_order_release);
    if (post_network_task("capture_poll", aida::infra::executor::domain_t::feature_worker, "bounded_task", []() {
            try {
                capture_poll_thread(g_state);
            } catch (const std::exception& e) {
                diag::log_tagged_fmt("network", "capture_poll_cpp_exception what=%s", e.what());
            } catch (...) {
                diag::log_tagged("network", "capture_poll_unknown_exception");
            }
            g_state.cap_thread_done.store(true, std::memory_order_release);
        })) {
        return true;
    }
    state.cap_thread_alive.store(false, std::memory_order_release);
    state.cap_thread_done.store(true, std::memory_order_release);
    diag::log_tagged("network", "capture_worker_post_failed");
    return false;
}

static bool start_dns_worker(state_t& state) {
    if (!state.dns_thread_done.load(std::memory_order_acquire) &&
        state.dns_thread_alive.load(std::memory_order_acquire)) {
        return true;
    }
    state.dns_thread_alive.store(true, std::memory_order_release);
    state.dns_thread_done.store(false, std::memory_order_release);
    if (post_network_task("dns_poll", aida::infra::executor::domain_t::feature_worker, "bounded_task", []() {
            try {
                dns_poll_thread(g_state);
            } catch (const std::exception& e) {
                diag::log_tagged_fmt("network", "dns_poll_cpp_exception what=%s", e.what());
            } catch (...) {
                diag::log_tagged("network", "dns_poll_unknown_exception");
            }
            g_state.dns_thread_done.store(true, std::memory_order_release);
        })) {
        return true;
    }
    state.dns_thread_alive.store(false, std::memory_order_release);
    state.dns_thread_done.store(true, std::memory_order_release);
    diag::log_tagged("network", "dns_worker_post_failed");
    return false;
}

static bool start_bandwidth_worker(state_t& state) {
    if (!state.bw_thread_done.load(std::memory_order_acquire) &&
        state.bw_thread_alive.load(std::memory_order_acquire)) {
        return true;
    }
    state.bw_thread_alive.store(true, std::memory_order_release);
    state.bw_thread_done.store(false, std::memory_order_release);
    if (post_network_task("bandwidth_poll", aida::infra::executor::domain_t::feature_worker, "bounded_task", []() {
            try {
                bandwidth_poll_thread(g_state);
            } catch (const std::exception& e) {
                diag::log_tagged_fmt("network", "bandwidth_poll_cpp_exception what=%s", e.what());
            } catch (...) {
                diag::log_tagged("network", "bandwidth_poll_unknown_exception");
            }
            g_state.bw_thread_done.store(true, std::memory_order_release);
        })) {
        return true;
    }
    state.bw_thread_alive.store(false, std::memory_order_release);
    state.bw_thread_done.store(true, std::memory_order_release);
    diag::log_tagged("network", "bandwidth_worker_post_failed");
    return false;
}

static void publish_initial_snapshots(state_t& state) {
    publish_repeater_request_artifacts(state);
}

static std::atomic<bool> s_driver_available_snapshot{false};
static std::atomic<bool> s_driver_available_snapshot_pending{false};
static std::atomic<std::uint64_t> s_driver_available_snapshot_requested_ms{0};

void request_driver_available_snapshot(bool force) {
    const std::uint64_t now = network_now_ms();
    const std::uint64_t last = s_driver_available_snapshot_requested_ms.load(std::memory_order_acquire);
    if (!force && last != 0 && now >= last && now - last < 500)
        return;
    bool expected = false;
    if (!s_driver_available_snapshot_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    s_driver_available_snapshot_requested_ms.store(now, std::memory_order_release);
    const bool posted = post_network_task(
        "driver_availability_snapshot", aida::infra::executor::domain_t::diagnostics, "bounded_task",
        []() {
            bool available = false;
            try {
                available = driver_bridge::using_kernel_driver();
            } catch (...) {
            }
            s_driver_available_snapshot.store(available, std::memory_order_release);
            s_driver_available_snapshot_pending.store(false, std::memory_order_release);
        }, false);
    if (!posted)
        s_driver_available_snapshot_pending.store(false, std::memory_order_release);
}

bool driver_available_snapshot() {
    return s_driver_available_snapshot.load(std::memory_order_acquire);
}

void initialize() {
    reset_common_exchange_actions();
    g_state.active = true;
    publish_initial_snapshots(g_state);
    diag::log_tagged("network", "initialize_begin");
    diag::log_tagged("net_audit", "[net_audit] network_view initialize begin");

    bool executor_ready = initialize_executor_for_network();

    mitm_proxy::set_ws_frame_callback([](const mitm_proxy::ws_frame_observed_t& frame) {
        state_t::ws_frame_entry_t entry;
        entry.timestamp = frame.timestamp;
        entry.exchange_id = frame.exchange_id;
        entry.host = frame.host;
        entry.port = frame.port;
        entry.is_outbound = frame.is_outbound;
        entry.is_text = frame.is_text;
        entry.opcode = frame.opcode;
        entry.payload = frame.payload;
        if (frame.is_text && !frame.payload.empty()) {
            size_t preview_len = frame.payload.size() < 96 ? frame.payload.size() : 96;
            entry.preview.assign(frame.payload.begin(), frame.payload.begin() + static_cast<ptrdiff_t>(preview_len));
            for (auto& ch : entry.preview) {
                unsigned char uc = static_cast<unsigned char>(ch);
                if (uc < 32 || uc == 127) ch = '.';
            }
        } else if (!frame.payload.empty()) {
            char buf[16];
            entry.preview.clear();
            size_t cap = frame.payload.size() < 16 ? frame.payload.size() : 16;
            for (size_t bi = 0; bi < cap; ++bi) {
                snprintf(buf, sizeof(buf), bi == 0 ? "%02X" : " %02X",
                    static_cast<unsigned>(frame.payload[bi]));
                entry.preview += buf;
            }
            if (frame.payload.size() > cap) entry.preview += " ...";
        }
        aida::qt::net::WsFrameStore::instance().append(std::move(entry));
    });
    diag::log_tagged("net_audit", "[net_audit] websocket ws_frame_callback installed");

    if (executor_ready) {
        start_connection_worker(g_state);
        start_capture_worker(g_state);
        start_dns_worker(g_state);
        start_bandwidth_worker(g_state);
        aida::qt::net::fuzzer_controller_start();
    } else {
        g_state.conn_polling.store(false);
        g_state.conn_thread_done.store(true, std::memory_order_release);
        g_state.cap_thread_alive.store(false, std::memory_order_release);
        g_state.cap_thread_done.store(true, std::memory_order_release);
        g_state.dns_thread_alive.store(false, std::memory_order_release);
        g_state.dns_thread_done.store(true, std::memory_order_release);
        g_state.bw_thread_alive.store(false, std::memory_order_release);
        g_state.bw_thread_done.store(true, std::memory_order_release);
        g_state.fuzz_thread_alive.store(false, std::memory_order_release);
        g_state.fuzz_thread_done.store(true, std::memory_order_release);
        diag::log_tagged("network", "initialize_continuing_without_poll_workers");
    }

    try {
        diag::log_tagged("network", "burp_initialize_begin");
        bool burp_ok = aida::burp::initialize();
        diag::log_tagged_fmt("network", "burp_initialize_result ok=%d", burp_ok ? 1 : 0);
    } catch (const std::exception& e) {
        diag::log_tagged_fmt("network", "burp_initialize_cpp_exception what=%s", e.what());
    } catch (...) {
        diag::log_tagged("network", "burp_initialize_unknown_exception");
    }

    diag::log_tagged("network", "initialize_complete");
}

void shutdown() {
    reset_common_exchange_actions();
    diag::log_tagged("network", "shutdown_begin");
    diag::log_tagged("net_audit", "[net_audit] network_view shutdown begin");
    mitm_proxy::set_ws_frame_callback(nullptr);
    g_state.conn_polling.store(false);
    g_state.conn_cv.notify_all();
    g_state.bw_polling.store(false);
    g_state.bw_thread_alive.store(false);
    g_state.bw_cv.notify_all();

    g_state.cap_polling.store(false);
    g_state.cap_running.store(false, std::memory_order_release);
    g_state.cap_start_pending.store(false, std::memory_order_release);
    g_state.cap_stop_pending.store(false, std::memory_order_release);
    g_state.cap_thread_alive.store(false);
    g_state.cap_cv.notify_all();

    g_state.dns_polling.store(false);
    g_state.dns_thread_alive.store(false);
    g_state.dns_cv.notify_all();


    auto wait_done = [](const char* name, const std::atomic<bool>& done_flag) {
        const uint64_t begin = static_cast<uint64_t>(GetTickCount64());
        while (!done_flag.load(std::memory_order_acquire)) {
            const uint64_t elapsed = static_cast<uint64_t>(GetTickCount64()) - begin;
            if (elapsed >= 2500) {
                diag::log_tagged_fmt("network", "shutdown_wait_timeout worker=%s elapsed_ms=%llu",
                    name ? name : "<unnamed>",
                    static_cast<unsigned long long>(elapsed));
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        diag::log_tagged_fmt("network", "shutdown_wait_done worker=%s elapsed_ms=%llu",
            name ? name : "<unnamed>",
            static_cast<unsigned long long>(static_cast<uint64_t>(GetTickCount64()) - begin));
    };
    auto wait_done_before_dependency_teardown = [](const char* name,
                                                   const std::atomic<bool>& done_flag) {
        const uint64_t begin = static_cast<uint64_t>(GetTickCount64());
        uint64_t next_report = 2500;
        while (!done_flag.load(std::memory_order_acquire)) {
            const uint64_t elapsed = static_cast<uint64_t>(GetTickCount64()) - begin;
            if (elapsed >= next_report) {
                diag::log_tagged_fmt("network",
                    "shutdown_dependency_drain_pending worker=%s elapsed_ms=%llu",
                    name ? name : "<unnamed>",
                    static_cast<unsigned long long>(elapsed));
                next_report = elapsed + 2500;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        diag::log_tagged_fmt("network",
            "shutdown_dependency_drain_done worker=%s elapsed_ms=%llu",
            name ? name : "<unnamed>",
            static_cast<unsigned long long>(static_cast<uint64_t>(GetTickCount64()) - begin));
    };
    wait_done("conn", g_state.conn_thread_done);
    wait_done("capture", g_state.cap_thread_done);
    wait_done("dns", g_state.dns_thread_done);
    wait_done("bandwidth", g_state.bw_thread_done);
    aida::qt::net::fuzzer_controller_shutdown();

    aida::burp::shutdown();

    mitm_proxy::stop();
    ssl_keylog::stop_watching();
    g_state.active = false;
    diag::log_tagged("network", "shutdown_complete");
}


template <typename RawConnections>
static std::vector<connection_entry_t> convert_connection_entries(RawConnections&& raw_connections) {
    std::vector<connection_entry_t> entries;
    entries.reserve(raw_connections.size());
    for (auto& connection : raw_connections) {
        connection_entry_t entry;
        entry.pid = connection.pid;
        entry.protocol = connection.protocol;
        entry.state = connection.state;
        entry.local_port = connection.local_port;
        entry.remote_port = connection.remote_port;
        entry.address_family = connection.address_family;
        std::memcpy(entry.local_addr, connection.local_addr, sizeof(entry.local_addr));
        std::memcpy(entry.remote_addr, connection.remote_addr, sizeof(entry.remote_addr));
        entries.push_back(std::move(entry));
    }
    return entries;
}

void request_connection_refresh() {
    state_t& state = g_state;
    bool expected = false;
    if (!state.conn_refresh_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = state.conn_refresh_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::uint32_t filter_pid = state.conn_filter_pid;
    const std::uint8_t filter_protocol = state.conn_filter_protocol;
    const std::string task_id = register_network_operation(
        "network.connections.refresh", "Refresh network connections",
        "view.network.connections", "driver connection table");
    const bool posted = post_network_task(
        "connection_refresh", aida::infra::executor::domain_t::feature_worker, "bounded_task",
        [serial, filter_pid, filter_protocol, task_id]() {
            bool success = false;
            std::string error;
            std::vector<connection_entry_t> entries;
            try {
                if (!driver_feature_ready("connection_refresh")) {
                    error = "Driver connection enumeration is unavailable";
                } else {
                    auto raw = driver_bridge::enumerate_connections(filter_pid, filter_protocol);
                    entries = convert_connection_entries(std::move(raw));
                    success = true;
                }
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Connection enumeration failed";
            }
            if (success && g_state.conn_refresh_serial.load(std::memory_order_acquire) == serial &&
                s_connection_snapshot_sink) {
                s_connection_snapshot_sink(
                    std::make_shared<const std::vector<connection_entry_t>>(entries));
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? std::to_string(entries.size()) + " connections published" : error);
            post_network_ui_completion([serial, success, error = std::move(error)]() {
                if (g_state.conn_refresh_serial.load(std::memory_order_acquire) != serial)
                    return;
                g_state.conn_refresh_pending.store(false, std::memory_order_release);
                if (!success)
                    toast_notification::push(error.empty() ? "Connection refresh failed" : error,
                        toast_notification::toast_type_t::error);
            });
        }, false);
    if (!posted) {
        state.conn_refresh_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected connection refresh");
    }
}

static void request_capture_start(state_t& state) {
    bool expected = false;
    if (!state.cap_start_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        diag::log_tagged("network", "start_capture_ignored_already_pending");
        return;
    }
    set_capture_control_status("Starting capture...");
    uint32_t filter_pid = state.cap_filter_pid;
    uint32_t filter_port = static_cast<uint32_t>(state.cap_filter_port);
    uint32_t filter_protocol = state.cap_filter_protocol;
    const bool driver_ok = s_driver_available_snapshot.load(std::memory_order_acquire);
    if (!driver_ok) {
        set_capture_control_status("Capture unavailable until the kernel driver is ready");
        state.cap_start_pending.store(false, std::memory_order_release);
        return;
    }
    bool poll_ready = start_capture_worker(state);
    diag::log_tagged_fmt("network",
        "start_capture_requested filter_pid=%u filter_port=%u filter_proto=%u drv_ok=%d poll_ready=%d cap_thread_done=%d cap_thread_alive=%d",
        filter_pid, filter_port, filter_protocol,
        driver_ok ? 1 : 0,
        poll_ready ? 1 : 0,
        state.cap_thread_done.load(std::memory_order_acquire) ? 1 : 0,
        state.cap_thread_alive.load(std::memory_order_acquire) ? 1 : 0);
    if (!poll_ready) {
        set_capture_control_status("Capture worker unavailable");
        state.cap_start_pending.store(false, std::memory_order_release);
        return;
    }

    if (!post_network_task("capture_start_control", aida::infra::executor::domain_t::feature_worker, "bounded_task", [filter_pid, filter_port, filter_protocol]() {
            ULONGLONG t0 = GetTickCount64();
            bool ok = false;
            try {
                ok = driver_feature_ready("start_capture_async") &&
                     driver_bridge::start_capture(filter_pid, filter_port, filter_protocol, nullptr);
            } catch (const std::exception& e) {
                diag::log_tagged_fmt("network", "start_capture_cpp_exception what=%s", e.what());
            } catch (...) {
                diag::log_tagged("network", "start_capture_unknown_exception");
            }
            ULONGLONG elapsed = GetTickCount64() - t0;
            if (ok) {
                g_state.cap_running.store(true, std::memory_order_release);
                g_state.cap_polling.store(true, std::memory_order_release);
                g_state.cap_cv.notify_all();
                set_capture_control_status("Capture running");
                diag::log_tagged_fmt("network", "start_capture_ok async elapsed_ms=%llu poll_thread_signaled=%d",
                    static_cast<unsigned long long>(elapsed),
                    g_state.cap_thread_alive.load(std::memory_order_acquire) ? 1 : 0);
                diag::log_tagged("net_audit",
                    "[net_audit] capture started ok");
            } else {
                set_capture_control_status("Capture start failed");
                diag::log_tagged_fmt("network", "start_capture_failed async elapsed_ms=%llu kernel_mode=%d",
                    static_cast<unsigned long long>(elapsed),
                    driver_bridge::using_kernel_driver() ? 1 : 0);
                diag::log_tagged("net_audit",
                    "[net_audit] capture start FAILED driver call returned false");
            }
            g_state.cap_start_pending.store(false, std::memory_order_release);
        })) {
        set_capture_control_status("Capture start queue failed");
        state.cap_start_pending.store(false, std::memory_order_release);
    }
}

static void request_capture_stop(state_t& state) {
    bool expected = false;
    if (!state.cap_stop_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        diag::log_tagged("network", "stop_capture_ignored_already_pending");
        return;
    }
    set_capture_control_status("Stopping capture...");
    diag::log_tagged("network", "stop_capture_requested");
    if (!post_network_task("capture_stop_control", aida::infra::executor::domain_t::feature_worker, "bounded_task", []() {
            ULONGLONG t0 = GetTickCount64();
            bool ok = false;
            try {
                ok = driver_bridge::stop_capture();
            } catch (const std::exception& e) {
                diag::log_tagged_fmt("network", "stop_capture_cpp_exception what=%s", e.what());
            } catch (...) {
                diag::log_tagged("network", "stop_capture_unknown_exception");
            }
            ULONGLONG elapsed = GetTickCount64() - t0;
            g_state.cap_running.store(false, std::memory_order_release);
            g_state.cap_polling.store(false, std::memory_order_release);
            set_capture_control_status(ok ? "Capture stopped" : "Capture stop failed");
            diag::log_tagged_fmt("network", "stop_capture_complete ok=%d elapsed_ms=%llu",
                ok ? 1 : 0,
                static_cast<unsigned long long>(elapsed));
            diag::log_tagged("net_audit",
                ok ? "[net_audit] capture stopped by user" : "[net_audit] capture stop FAILED driver call returned false");
            g_state.cap_stop_pending.store(false, std::memory_order_release);
        })) {
        set_capture_control_status("Capture stop queue failed");
        state.cap_stop_pending.store(false, std::memory_order_release);
    }
}


template <typename RawDns>
static std::size_t merge_dns_entries(state_t& state, RawDns&& raw_dns,
                                     std::vector<dns_entry_t>* added_out = nullptr,
                                     std::size_t* trimmed_out = nullptr) {
    std::size_t added = 0;
    std::size_t trimmed = 0;
    std::lock_guard<std::mutex> lock(state.dns_mutex);
    for (auto& query : raw_dns) {
        bool duplicate = false;
        const auto recent_count = static_cast<std::ptrdiff_t>((std::min)(
            static_cast<std::size_t>(256), state.dns_entries.size()));
        for (auto it = state.dns_entries.rbegin();
             it != state.dns_entries.rend() && it != state.dns_entries.rbegin() + recent_count; ++it) {
            if (it->timestamp == query.timestamp && it->domain == query.domain && it->pid == query.pid) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;
        dns_entry_t entry;
        entry.timestamp = query.timestamp;
        entry.pid = query.pid;
        entry.query_type = static_cast<std::uint16_t>(query.query_type);
        entry.domain = query.domain;
        entry.resolved_addr = format_ip(query.resolved_addr, 2);
        entry.response_code = query.response_code;
        entry.ttl = query.ttl;
        state.dns_entries.push_back(entry);
        if (added_out)
            added_out->push_back(std::move(entry));
        ++added;
    }
    while (state.dns_entries.size() > state.dns_max_entries) {
        state.dns_entries.pop_front();
        ++trimmed;
    }
    if (trimmed_out)
        *trimmed_out = trimmed;
    return added;
}

void request_dns_refresh() {
    state_t& state = g_state;
    bool expected = false;
    if (!state.dns_refresh_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = state.dns_refresh_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::uint32_t filter_pid = state.dns_filter_pid;
    const std::string task_id = register_network_operation(
        "network.dns.refresh", "Refresh DNS observations", "view.network.dns",
        filter_pid == 0 ? "all processes" : "PID " + std::to_string(filter_pid));
    const bool posted = post_network_task(
        "dns_refresh", aida::infra::executor::domain_t::feature_worker, "bounded_task",
        [serial, filter_pid, task_id]() {
            bool success = false;
            std::size_t raw_count = 0;
            std::size_t added = 0;
            std::string error;
            try {
                if (!driver_feature_ready("dns_refresh")) {
                    error = "Driver DNS observations are unavailable";
                } else {
                    auto raw = driver_bridge::get_dns_queries(filter_pid);
                    raw_count = raw.size();
                    if (g_state.dns_refresh_serial.load(std::memory_order_acquire) == serial) {
                        std::vector<dns_entry_t> added_entries;
                        std::size_t trimmed = 0;
                        added = merge_dns_entries(g_state, std::move(raw), &added_entries, &trimmed);
                        if (!added_entries.empty() && s_dns_batch_sink) {
                            s_dns_batch_sink(
                                std::make_shared<const std::vector<dns_entry_t>>(
                                    std::move(added_entries)),
                                trimmed);
                        }
                    }
                    success = g_state.dns_refresh_serial.load(std::memory_order_acquire) == serial;
                    if (!success)
                        error = "DNS refresh was superseded";
                }
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "DNS refresh failed";
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? std::to_string(added) + " new of " + std::to_string(raw_count) + " observations" : error);
            post_network_ui_completion([serial, success, error = std::move(error)]() {
                if (g_state.dns_refresh_serial.load(std::memory_order_acquire) != serial)
                    return;
                g_state.dns_refresh_pending.store(false, std::memory_order_release);
                if (!success)
                    toast_notification::push(error.empty() ? "DNS refresh failed" : error,
                        toast_notification::toast_type_t::error);
            });
        }, false);
    if (!posted) {
        state.dns_refresh_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected DNS refresh");
    }
}

static std::shared_ptr<const proxy_runtime_snapshot_t> s_proxy_runtime_snapshot;
static std::atomic<bool> s_proxy_snapshot_pending{false};
static std::atomic<std::uint64_t> s_proxy_snapshot_requested_ms{0};
static std::atomic<bool> s_proxy_operation_pending{false};
static std::atomic<std::uint64_t> s_proxy_operation_serial{0};
static proxy_snapshot_sink_t s_proxy_snapshot_sink;

void set_proxy_snapshot_sink(proxy_snapshot_sink_t sink) {
    s_proxy_snapshot_sink = std::move(sink);
}

std::shared_ptr<const proxy_runtime_snapshot_t> proxy_runtime_snapshot() {
    return std::atomic_load_explicit(&s_proxy_runtime_snapshot, std::memory_order_acquire);
}

bool proxy_operation_pending() {
    return s_proxy_operation_pending.load(std::memory_order_acquire);
}

void request_proxy_runtime_snapshot(bool force) {
    const std::uint64_t now = network_now_ms();
    const std::uint64_t last = s_proxy_snapshot_requested_ms.load(std::memory_order_acquire);
    if (!force && last != 0 && now >= last && now - last < 350)
        return;
    bool expected = false;
    if (!s_proxy_snapshot_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    s_proxy_snapshot_requested_ms.store(now, std::memory_order_release);
    const bool posted = post_network_task(
        "proxy_snapshot", aida::infra::executor::domain_t::diagnostics, "bounded_task",
        []() {
            try {
                auto snapshot = std::make_shared<proxy_runtime_snapshot_t>();
                snapshot->stats = mitm_proxy::get_stats();
                snapshot->history = mitm_proxy::get_history(4096);
                snapshot->ca_ready = cert_generator::is_ready();
                if (snapshot->ca_ready) {
                    const auto& root = cert_generator::get_root_ca();
                    snapshot->ca_installed = cert_generator::is_root_ca_installed(root);
                    snapshot->spki_prefix = aida::burp::browser::spki_hash_prefix(
                        cert_generator::spki_sha256_base64(root));
                }
                const auto browsers = aida::burp::browser::list_running();
                snapshot->controlled_browser_running = std::any_of(browsers.begin(), browsers.end(),
                    [](const auto& browser) { return browser.running; });
                snapshot->bypass_active = cert_pin_bypass::is_bypass_active();
                if (snapshot->bypass_active)
                    snapshot->bypass_count = cert_pin_bypass::get_active_bypasses().size();
                std::atomic_store_explicit(&s_proxy_runtime_snapshot,
                    std::shared_ptr<const proxy_runtime_snapshot_t>(snapshot),
                    std::memory_order_release);
                if (s_proxy_snapshot_sink)
                    s_proxy_snapshot_sink(std::move(snapshot));
            } catch (...) {
            }
            s_proxy_snapshot_pending.store(false, std::memory_order_release);
        }, false);
    if (!posted)
        s_proxy_snapshot_pending.store(false, std::memory_order_release);
}

static void request_proxy_control(state_t& state, bool start) {
    bool expected = false;
    if (!s_proxy_operation_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = s_proxy_operation_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    mitm_proxy::proxy_config config;
    config.bind_addr = state.proxy_bind_addr;
    config.bind_port = static_cast<std::uint16_t>((std::max)(1, (std::min)(state.proxy_port, 65535)));
    config.decode_tls = state.proxy_decode_tls;
    const std::string target = config.bind_addr + ":" + std::to_string(config.bind_port) +
        (config.decode_tls ? " TLS interception" : " plaintext only");
    const std::string task_id = register_network_operation(
        start ? "network.proxy.start" : "network.proxy.stop",
        start ? "Start interception proxy" : "Stop interception proxy",
        "view.network.proxy", target);
    const bool posted = post_network_task(
        start ? "proxy_start" : "proxy_stop",
        start ? aida::infra::executor::domain_t::long_running
              : aida::infra::executor::domain_t::feature_worker,
        start ? "long_running" : "bounded_task",
        [serial, start, config = std::move(config), task_id]() {
            bool success = true;
            std::string error;
            try {
                if (start)
                    success = mitm_proxy::start(config);
                else
                    mitm_proxy::stop();
                if (success != mitm_proxy::is_running()) {
                    if (!start && !mitm_proxy::is_running())
                        success = true;
                    else if (start)
                        success = false;
                }
                if (!success)
                    error = start ? "Proxy did not enter the running state" : "Proxy did not stop";
            } catch (const std::exception& exception) {
                success = false;
                error = exception.what();
            } catch (...) {
                success = false;
                error = start ? "Proxy start failed" : "Proxy stop failed";
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? (start ? "Proxy is running" : "Proxy is stopped") : error);
            post_network_ui_completion([serial, success, error = std::move(error)]() {
                if (s_proxy_operation_serial.load(std::memory_order_acquire) != serial)
                    return;
                if (!success)
                    toast_notification::push(error.empty() ? "Proxy control failed" : error,
                        toast_notification::toast_type_t::error);
                s_proxy_operation_pending.store(false, std::memory_order_release);
                request_proxy_runtime_snapshot(true);
            });
        }, false);
    if (!posted) {
        s_proxy_operation_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected proxy control");
    }
}

static void request_proxy_history_clear(std::size_t reviewed_count,
                                        std::vector<std::uint64_t> reviewed_ids) {
    bool expected = false;
    if (!s_proxy_operation_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = s_proxy_operation_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string task_id = register_network_operation(
        "network.proxy.history.clear", "Clear proxy history", "view.network.proxy",
        std::to_string(reviewed_count) + " reviewed exchanges");
    const bool posted = post_network_task(
        "proxy_history_clear", aida::infra::executor::domain_t::diagnostics, "bounded_task",
        [serial, reviewed_count, reviewed_ids = std::move(reviewed_ids), task_id]() {
            bool success = true;
            std::string error;
            try {
                success = mitm_proxy::clear_history_if_exact(reviewed_ids);
                if (!success) {
                    error = "Proxy history changed after confirmation; review the current retained exchanges again";
                }
            } catch (const std::exception& exception) {
                success = false;
                error = exception.what();
            } catch (...) {
                success = false;
                error = "Proxy history clear failed";
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? std::to_string(reviewed_count) + " exchanges cleared" : error);
            post_network_ui_completion([serial, success, error = std::move(error)]() {
                if (s_proxy_operation_serial.load(std::memory_order_acquire) != serial)
                    return;
                if (!success)
                    toast_notification::push(error.empty() ? "Proxy history clear failed" : error,
                        toast_notification::toast_type_t::error);
                s_proxy_operation_pending.store(false, std::memory_order_release);
                request_proxy_runtime_snapshot(true);
            });
        }, false);
    if (!posted) {
        s_proxy_operation_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected proxy history clear");
    }
}

static void request_ca_trust_repair() {
    bool expected = false;
    if (!s_proxy_operation_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = s_proxy_operation_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string task_id = register_network_operation(
        "network.proxy.ca_trust_repair", "Repair AiDA interception CA trust", "view.network.proxy",
        "current user trust store");
    const bool posted = post_network_task(
        "proxy_ca_repair", aida::infra::executor::domain_t::diagnostics, "bounded_task",
        [serial, task_id]() {
            bool success = false;
            std::string error;
            try {
                success = cert_generator::initialize();
                if (success && cert_generator::is_ready())
                    success = cert_generator::install_root_ca(cert_generator::get_root_ca());
                if (!success)
                    error = "AiDA CA trust repair failed";
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "AiDA CA trust repair failed";
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? "AiDA CA is installed in the current user trust store" : error);
            post_network_ui_completion([serial, success, error = std::move(error)]() {
                if (s_proxy_operation_serial.load(std::memory_order_acquire) != serial)
                    return;
                toast_notification::push(success ? "AiDA CA trust repaired."
                    : (error.empty() ? "AiDA CA trust repair failed." : error),
                    success ? toast_notification::toast_type_t::success : toast_notification::toast_type_t::error);
                s_proxy_operation_pending.store(false, std::memory_order_release);
                request_proxy_runtime_snapshot(true);
            });
        }, false);
    if (!posted) {
        s_proxy_operation_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected CA trust repair");
    }
}

void request_legacy_bypass_revert(std::size_t reviewed_count) {
    bool expected = false;
    if (!s_proxy_operation_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = s_proxy_operation_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string task_id = register_network_operation(
        "network.proxy.legacy_bypass.revert", "Revert legacy certificate bypass patches",
        "view.network.proxy", std::to_string(reviewed_count) + " live patches");
    const bool posted = post_network_task(
        "proxy_legacy_bypass_revert", aida::infra::executor::domain_t::feature_worker, "bounded_task",
        [serial, reviewed_count, task_id]() {
            bool success = false;
            int reverted = 0;
            std::string error;
            try {
                reverted = cert_pin_bypass::revert_all_bypasses();
                success = reverted >= 0 && !cert_pin_bypass::is_bypass_active();
                if (!success)
                    error = "One or more legacy certificate patches remain active";
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Legacy certificate patch reversion failed";
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? std::to_string(reverted) + " of " + std::to_string(reviewed_count) + " patches reverted" : error);
            post_network_ui_completion([serial, success, error = std::move(error)]() {
                if (s_proxy_operation_serial.load(std::memory_order_acquire) != serial)
                    return;
                if (!success)
                    toast_notification::push(error.empty() ? "Legacy certificate patch reversion failed" : error,
                        toast_notification::toast_type_t::error);
                s_proxy_operation_pending.store(false, std::memory_order_release);
                request_proxy_runtime_snapshot(true);
            });
        }, false);
    if (!posted) {
        s_proxy_operation_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected legacy bypass reversion");
    }
}

void request_certificate_diagnostics(std::uint32_t target_pid,
                                            cert_intercept::diagnostic_context_t context) {
    bool expected = false;
    if (!s_cert_diagnostics_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = s_cert_diagnostics_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string task_id = register_network_operation(
        "network.proxy.certificate_diagnostics", "Diagnose target TLS interception",
        "view.network.proxy", "PID " + std::to_string(target_pid));
    const bool posted = post_network_task(
        "proxy_certificate_diagnostics", aida::infra::executor::domain_t::diagnostics, "bounded_task",
        [serial, target_pid, context = std::move(context), task_id]() mutable {
            bool success = false;
            cert_intercept::process_diagnostics_t report;
            std::vector<cert_intercept::provider_status_t> providers;
            std::string error;
            try {
                cert_diag_apply_proxy_observations(context);
                report = cert_intercept::diagnose_process(target_pid, context);
                providers = cert_intercept::provider_registry_t::instance().evaluate(target_pid, report);
                success = true;
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Target TLS diagnostics failed";
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? cert_intercept::to_string(report.primary) + " - " + report.summary : error);
            post_network_ui_completion([serial, success, report = std::move(report),
                                   providers = std::move(providers), error = std::move(error)]() mutable {
                if (s_cert_diagnostics_serial.load(std::memory_order_acquire) != serial)
                    return;
                s_cert_diagnostics_pending.store(false, std::memory_order_release);
                if (s_cert_diagnostics_sink) {
                    s_cert_diagnostics_sink(success, std::move(report), std::move(providers),
                        success ? cert_intercept::to_string(report.primary) + " - " + report.summary
                                : (error.empty() ? "Target TLS diagnostics failed" : error));
                }
            });
        }, false);
    if (!posted) {
        s_cert_diagnostics_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected target TLS diagnostics");
        if (s_cert_diagnostics_sink)
            s_cert_diagnostics_sink(false, cert_intercept::process_diagnostics_t{}, {},
                "Executor rejected target TLS diagnostics");
    }
}

void request_certificate_handoff(cert_intercept::process_diagnostics_t report,
                                        std::vector<cert_intercept::provider_status_t> providers,
                                        std::string proxy_endpoint) {
    bool expected = false;
    if (!s_cert_handoff_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = s_cert_handoff_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string target_label = report.process_name.empty() ? "target" : report.process_name;
    const std::string task_id = register_network_operation(
        "network.proxy.certificate_handoff", "Generate TLS interception handoff",
        "view.network.proxy", target_label);
    const bool posted = post_network_task(
        "proxy_certificate_handoff", aida::infra::executor::domain_t::diagnostics, "bounded_task",
        [serial, report = std::move(report), providers = std::move(providers),
         proxy_endpoint = std::move(proxy_endpoint), target_label, task_id]() mutable {
            bool success = false;
            std::string status;
            try {
                cert_intercept::profiles::public_ca_export_t exported;
                if (cert_generator::initialize() && cert_generator::is_ready())
                    exported = cert_intercept::profiles::export_public_ca_files(cert_generator::get_root_ca());
                if (!exported.ok) {
                    status = exported.error.empty() ? "ca_export_failed" : exported.error;
                } else {
                    cert_intercept::handoff_request_t request;
                    request.diagnostics = std::move(report);
                    request.provider_statuses = std::move(providers);
                    request.target_label = target_label;
                    request.proxy_endpoint = proxy_endpoint;
                    request.ca_cert_pem_path = exported.pem_path.u8string();
                    request.ca_cert_der_path = exported.der_path.u8string();
                    auto handoff = cert_intercept::generate_handoff(request);
                    success = handoff.ok;
                    status = success ? handoff.metadata_path.u8string() : handoff.error;
                }
            } catch (const std::exception& exception) {
                status = exception.what();
            } catch (...) {
                status = "TLS interception handoff generation failed";
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed", status);
            post_network_ui_completion([serial, success, status = std::move(status)]() mutable {
                if (s_cert_handoff_serial.load(std::memory_order_acquire) != serial)
                    return;
                s_cert_handoff_pending.store(false, std::memory_order_release);
                if (s_cert_handoff_sink) {
                    s_cert_handoff_sink(success, status.empty()
                        ? (success ? "Handoff generated" : "TLS interception handoff generation failed")
                        : status);
                }
            });
        }, false);
    if (!posted) {
        s_cert_handoff_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected TLS interception handoff generation");
        if (s_cert_handoff_sink)
            s_cert_handoff_sink(false, "Executor rejected TLS interception handoff generation");
    }
}

static std::string driver_failure_text(const char* fallback) {
    const std::string detail = driver_bridge::last_error();
    return detail.empty() ? (fallback ? fallback : "Driver operation failed") : detail;
}

static void request_filter_add(state_t& state) {
    bool expected = false;
    if (!state.filter_mutation_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed_pid = state.nf_pid[0] ? std::strtoul(state.nf_pid, &end, 10) : 0;
    if (errno != 0 || (state.nf_pid[0] && (!end || *end != '\0')) || parsed_pid > UINT32_MAX) {
        state.filter_mutation_pending.store(false, std::memory_order_release);
        toast_notification::push("PID must be an unsigned 32-bit integer", toast_notification::toast_type_t::error);
        return;
    }
    end = nullptr;
    errno = 0;
    const unsigned long parsed_port = state.nf_port[0] ? std::strtoul(state.nf_port, &end, 10) : 0;
    if (errno != 0 || (state.nf_port[0] && (!end || *end != '\0')) || parsed_port > UINT16_MAX) {
        state.filter_mutation_pending.store(false, std::memory_order_release);
        toast_notification::push("Port must be between 0 and 65535", toast_notification::toast_type_t::error);
        return;
    }
    std::array<std::uint8_t, 16> ip_bytes{};
    const std::string ip_text(state.nf_ip);
    if (!ip_text.empty() && inet_pton(AF_INET, ip_text.c_str(), ip_bytes.data()) != 1) {
        state.filter_mutation_pending.store(false, std::memory_order_release);
        toast_notification::push("Filter IP address is not valid IPv4", toast_notification::toast_type_t::error);
        return;
    }
    const std::uint32_t action = static_cast<std::uint32_t>(state.nf_action);
    const std::uint32_t direction = static_cast<std::uint32_t>(state.nf_direction);
    const std::uint32_t protocol = static_cast<std::uint32_t>(state.nf_protocol);
    const std::uint32_t pid = static_cast<std::uint32_t>(parsed_pid);
    const std::uint16_t port = static_cast<std::uint16_t>(parsed_port);
    const std::uint64_t serial = state.filter_mutation_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string target = "action=" + std::to_string(action) + " direction=" +
        std::to_string(direction) + " protocol=" + std::to_string(protocol) +
        " pid=" + std::to_string(pid) + " port=" + std::to_string(port) +
        (ip_text.empty() ? std::string() : " ip=" + ip_text);
    const std::string task_id = register_network_operation(
        "network.filters.add", "Add network filter rule", "view.network.filters", target);
    const bool posted = post_network_task(
        "filter_add", aida::infra::executor::domain_t::feature_worker, "bounded_task",
        [serial, action, direction, protocol, pid, port, ip_bytes, ip_text, task_id]() {
            std::uint32_t rule_id = 0;
            bool success = false;
            std::string error;
            try {
                success = driver_feature_ready("filter_add") && driver_bridge::add_filter_rule(
                    action, direction, protocol, pid, port, ip_bytes.data(), nullptr, &rule_id);
                if (!success)
                    error = driver_failure_text("Failed to add filter rule");
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Failed to add filter rule";
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? "Rule #" + std::to_string(rule_id) + " applied" : error);
            post_network_ui_completion([serial, success, rule_id, action, direction, protocol, pid, port,
                                   ip_text, error = std::move(error)]() {
                if (g_state.filter_mutation_serial.load(std::memory_order_acquire) != serial)
                    return;
                if (success) {
                    filter_entry_t entry;
                    entry.rule_id = rule_id;
                    entry.action = static_cast<std::uint8_t>(action);
                    entry.direction = static_cast<std::uint8_t>(direction);
                    entry.protocol = static_cast<std::uint8_t>(protocol);
                    entry.pid = pid;
                    entry.port = port;
                    entry.ip_addr = ip_text;
                    entry.active = true;
                    g_state.filters.push_back(std::move(entry));
                    toast_notification::push("Filter rule added", toast_notification::toast_type_t::info);
                } else {
                    toast_notification::push(error.empty() ? "Failed to add filter rule" : error,
                        toast_notification::toast_type_t::error);
                }
                g_state.filter_mutation_pending.store(false, std::memory_order_release);
                if (s_filters_changed_sink)
                    s_filters_changed_sink();
            });
        }, false);
    if (!posted) {
        state.filter_mutation_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected filter mutation");
    }
}

static void request_filter_clear(state_t& state) {
    bool expected = false;
    if (!state.filter_mutation_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = state.filter_mutation_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::size_t reviewed_count = state.filters.size();
    const std::string task_id = register_network_operation(
        "network.filters.clear", "Clear all network filter rules", "view.network.filters",
        std::to_string(reviewed_count) + " reviewed rules");
    const bool posted = post_network_task(
        "filter_clear", aida::infra::executor::domain_t::feature_worker, "bounded_task",
        [serial, reviewed_count, task_id]() {
            bool success = false;
            std::string error;
            try {
                success = driver_feature_ready("filter_clear") && driver_bridge::clear_filter_rules();
                if (!success)
                    error = driver_failure_text("Failed to clear filter rules");
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Failed to clear filter rules";
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? std::to_string(reviewed_count) + " rules cleared" : error);
            post_network_ui_completion([serial, success, error = std::move(error)]() {
                if (g_state.filter_mutation_serial.load(std::memory_order_acquire) != serial)
                    return;
                if (success) {
                    g_state.filters.clear();
                    g_state.filter_selected = -1;
                } else
                    toast_notification::push(error.empty() ? "Failed to clear filter rules" : error,
                        toast_notification::toast_type_t::error);
                g_state.filter_mutation_pending.store(false, std::memory_order_release);
                if (s_filters_changed_sink)
                    s_filters_changed_sink();
            });
        }, false);
    if (!posted) {
        state.filter_mutation_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected filter mutation");
    }
}

static void request_filter_remove(state_t& state, std::uint32_t rule_id) {
    bool expected = false;
    if (!state.filter_mutation_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = state.filter_mutation_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string task_id = register_network_operation(
        "network.filters.remove_selected", "Remove network filter rule", "view.network.filters",
        "rule #" + std::to_string(rule_id));
    const bool posted = post_network_task(
        "filter_remove", aida::infra::executor::domain_t::feature_worker, "bounded_task",
        [serial, rule_id, task_id]() {
            bool success = false;
            std::string error;
            try {
                success = driver_feature_ready("filter_remove") && driver_bridge::remove_filter_rule(rule_id);
                if (!success)
                    error = driver_failure_text("Failed to remove filter rule");
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Failed to remove filter rule";
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? "Rule #" + std::to_string(rule_id) + " removed" : error);
            post_network_ui_completion([serial, rule_id, success, error = std::move(error)]() {
                if (g_state.filter_mutation_serial.load(std::memory_order_acquire) != serial)
                    return;
                if (success) {
                    g_state.filters.erase(std::remove_if(g_state.filters.begin(), g_state.filters.end(),
                        [rule_id](const filter_entry_t& entry) { return entry.rule_id == rule_id; }),
                        g_state.filters.end());
                    g_state.filter_selected = -1;
                } else {
                    toast_notification::push(error.empty() ? "Failed to remove filter rule" : error,
                        toast_notification::toast_type_t::error);
                }
                g_state.filter_mutation_pending.store(false, std::memory_order_release);
                if (s_filters_changed_sink)
                    s_filters_changed_sink();
            });
        }, false);
    if (!posted) {
        state.filter_mutation_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected filter mutation");
    }
}

void request_bandwidth_control(bool start) {
    state_t& state = g_state;
    bool expected = false;
    if (!state.bw_control_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = state.bw_control_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string task_id = register_network_operation(
        start ? "network.bandwidth.start" : "network.bandwidth.stop",
        start ? "Start bandwidth monitor" : "Stop bandwidth monitor",
        "view.network.bandwidth", "kernel bandwidth telemetry");
    const bool posted = post_network_task(
        start ? "bandwidth_start" : "bandwidth_stop",
        aida::infra::executor::domain_t::feature_worker, "bounded_task",
        [serial, start, task_id]() {
            bool success = false;
            std::string error;
            try {
                success = driver_feature_ready(start ? "bandwidth_start" : "bandwidth_stop") &&
                    driver_bridge::bw_monitor_op(start ? 0U : 1U);
                if (!success)
                    error = driver_failure_text(start ? "Failed to start bandwidth monitor"
                                                      : "Failed to stop bandwidth monitor");
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = start ? "Failed to start bandwidth monitor" : "Failed to stop bandwidth monitor";
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? (start ? "Bandwidth telemetry started" : "Bandwidth telemetry stopped") : error);
            post_network_ui_completion([serial, start, success, error = std::move(error)]() {
                if (g_state.bw_control_serial.load(std::memory_order_acquire) != serial)
                    return;
                if (success) {
                    g_state.bw_monitoring = start;
                    g_state.bw_polling.store(start, std::memory_order_release);
                    if (start)
                        g_state.bw_cv.notify_one();
                } else {
                    toast_notification::push(error.empty()
                        ? (start ? "Failed to start bandwidth monitor" : "Failed to stop bandwidth monitor")
                        : error, toast_notification::toast_type_t::error);
                }
                g_state.bw_control_pending.store(false, std::memory_order_release);
            });
        }, false);
    if (!posted) {
        state.bw_control_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected bandwidth control");
    }
}

bool start_pcap_export() {
    state_t& state = g_state;
    bool expected = false;
    if (!state.pcap_writing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;
    state.pcap_written_count.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> status_lock(state.pcap_error_mutex);
        state.pcap_last_error.clear();
        state.pcap_last_path.clear();
    }
    const auto export_packets = capture_packets_snapshot();
    const std::string path(state.pcap_path);
    const auto filter_pid = state.pcap_filter_pid;
    const auto filter_proto = state.pcap_filter_protocol;
    const std::string task_id = register_network_operation(
        "network.pcap.export", "Export packet capture", "view.network.pcap",
        path + " pid=" + std::to_string(filter_pid) + " protocol=" + std::to_string(filter_proto));
    diag::log_tagged_fmt("network", "pcap_export_clicked path='%s' filter_pid=%u filter_proto=%u source_packets=%zu",
        path.c_str(), filter_pid, filter_proto, export_packets ? export_packets->size() : 0);
    diag::log_tagged("net_audit", ("[net_audit] pcap export start path='" + path + "'").c_str());
    const bool posted = post_network_task("pcap_export", aida::infra::executor::domain_t::diagnostics, "bounded_task",
        [packets = export_packets, path, filter_pid, filter_proto, task_id]() {
            bool success = false;
            std::uint32_t count = 0;
            std::string error;
            std::vector<std::uint8_t> bytes;
            if (!packets) {
                error = "The capture snapshot is no longer available";
            } else {
                success = aida::qt::net::serialize_pcap(*packets, filter_pid, filter_proto, bytes, count, error);
            }
            if (success)
                success = aida::qt::net::atomic_write_export(path, bytes.data(), bytes.size(), error);
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? std::to_string(count) + " packets written atomically to " + path : error);
            post_network_ui_completion([success, count, path, error = std::move(error)]() {
                {
                    std::lock_guard<std::mutex> status_lock(g_state.pcap_error_mutex);
                    g_state.pcap_last_error = success ? std::string() : error;
                    g_state.pcap_last_path = success ? path : std::string();
                }
                g_state.pcap_written_count.store(success ? count : 0, std::memory_order_release);
                g_state.pcap_writing.store(false, std::memory_order_release);
            });
        }, false);
    if (!posted) {
        std::lock_guard<std::mutex> elock(state.pcap_error_mutex);
        state.pcap_last_error = "Executor rejected PCAP export";
        state.pcap_writing.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", state.pcap_last_error);
        return false;
    }
    return true;
}

bool start_har_export(const std::string& har_path) {
    state_t& state = g_state;
    bool expected = false;
    if (!state.har_writing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;
    state.har_written_count.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> status_lock(state.har_status_mutex);
        state.har_last_error.clear();
        state.har_last_path.clear();
    }
    const std::string task_id = register_network_operation(
        "network.har.export", "Export proxy history as HAR", "view.network.proxy", har_path);
    diag::log_tagged_fmt("network", "har_export_clicked path='%s'", har_path.c_str());
    diag::log_tagged("net_audit",
        ("[net_audit] HAR export path='" + har_path + "'").c_str());
    const bool posted = post_network_task(
        "har_export", aida::infra::executor::domain_t::diagnostics, "bounded_task",
        [har_path, task_id]() {
            bool success = false;
            std::size_t count = 0;
            std::string serialized;
            std::string error;
            try {
                auto history = mitm_proxy::get_history(4096);
                count = history.size();
                success = aida::qt::net::serialize_har_bounded(history, serialized, error);
                if (success)
                    success = aida::qt::net::atomic_write_export(har_path,
                        reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size(), error);
            } catch (const std::exception& exception) {
                success = false;
                error = exception.what();
            } catch (...) {
                success = false;
                error = "HAR export failed";
            }
            finish_network_operation(task_id, success, success ? "Completed" : "Failed",
                success ? std::to_string(count) + " exchanges written atomically to " + har_path : error);
            post_network_ui_completion([success, count, har_path, error = std::move(error)]() {
                {
                    std::lock_guard<std::mutex> status_lock(g_state.har_status_mutex);
                    g_state.har_last_error = success ? std::string() : error;
                    g_state.har_last_path = success ? har_path : std::string();
                }
                g_state.har_written_count.store(success ? static_cast<std::uint32_t>(count) : 0,
                                                std::memory_order_release);
                g_state.har_writing.store(false, std::memory_order_release);
            });
        }, false);
    if (!posted) {
        state.har_writing.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> status_lock(state.har_status_mutex);
            state.har_last_error = "Executor rejected HAR export";
        }
        finish_network_operation(task_id, false, "Rejected", "Executor rejected HAR export");
        return false;
    }
    return true;
}

static std::shared_ptr<const intercept_runtime_snapshot_t> s_intercept_runtime_snapshot;
static std::atomic<bool> s_intercept_snapshot_pending{false};
static std::atomic<std::uint64_t> s_intercept_snapshot_requested_ms{0};
static std::atomic<std::uint64_t> s_intercept_snapshot_generation{0};
static std::atomic<bool> s_intercept_operation_pending{false};
static std::atomic<std::uint64_t> s_intercept_operation_serial{0};
static intercept_drop_review_t s_intercept_drop_review;
static std::uint64_t s_intercept_selected_exchange_id = 0;
static intercept_modified_draft_t s_intercept_modified_draft;
static intercept_drop_review_display_fn s_intercept_drop_review_display;
static intercept_snapshot_sink_t s_intercept_snapshot_sink;

struct intercept_editor_mirror_t {
    bool pretty_dirty = false;
    bool oversized = false;
    bool binary = false;
    std::string error;
};
static intercept_editor_mirror_t s_intercept_editor_mirror;

void set_intercept_drop_review_display(intercept_drop_review_display_fn fn) {
    s_intercept_drop_review_display = std::move(fn);
}

void set_intercept_snapshot_sink(intercept_snapshot_sink_t sink) {
    s_intercept_snapshot_sink = std::move(sink);
}

std::shared_ptr<const intercept_runtime_snapshot_t> intercept_runtime_snapshot() {
    return std::atomic_load_explicit(&s_intercept_runtime_snapshot, std::memory_order_acquire);
}

bool intercept_operation_pending() {
    return s_intercept_operation_pending.load(std::memory_order_acquire);
}

void set_intercept_selected_exchange(std::uint64_t exchange_id) {
    s_intercept_selected_exchange_id = exchange_id;
}

intercept_modified_draft_t intercept_modified_draft() {
    return s_intercept_modified_draft;
}

void set_intercept_modified_draft_text(std::string raw_request) {
    s_intercept_modified_draft.raw_request = std::move(raw_request);
    s_intercept_modified_draft.content_hash = artifact_hash(std::string_view(
        s_intercept_modified_draft.raw_request));
}

void set_intercept_editor_state(bool pretty_dirty, bool oversized, bool binary,
                                std::string error) {
    s_intercept_editor_mirror.pretty_dirty = pretty_dirty;
    s_intercept_editor_mirror.oversized = oversized;
    s_intercept_editor_mirror.binary = binary;
    s_intercept_editor_mirror.error = std::move(error);
}

intercept_target_identity_t intercept_target_identity(
    const intercept_runtime_snapshot_t& publication,
    const mitm_proxy::http_exchange& exchange) {
    return {publication.generation, exchange.id, exchange.timestamp,
        artifact_hash(exchange.raw_request), exchange.raw_request.size()};
}

static bool intercept_exchange_matches(const mitm_proxy::http_exchange& exchange,
                                       const intercept_target_identity_t& target) {
    return target.valid() && exchange.id == target.exchange_id &&
        exchange.timestamp == target.timestamp &&
        exchange.raw_request.size() == target.content_size &&
        artifact_hash(exchange.raw_request) == target.content_hash;
}

static bool intercept_target_matches(const intercept_target_identity_t& lhs,
                                     const intercept_target_identity_t& rhs,
                                     bool include_generation) {
    return lhs.exchange_id == rhs.exchange_id && lhs.timestamp == rhs.timestamp &&
        lhs.content_hash == rhs.content_hash && lhs.content_size == rhs.content_size &&
        (!include_generation || lhs.publication_generation == rhs.publication_generation);
}

static int utf8_decode_codepoint(const char* cursor, const char* end,
                                 unsigned int& codepoint) {
    const auto lead = static_cast<unsigned char>(*cursor);
    const std::size_t remaining = static_cast<std::size_t>(end - cursor);
    if (lead < 0x80U) {
        codepoint = lead;
        return 1;
    }
    std::size_t length = 0;
    unsigned int value = 0;
    unsigned int minimum = 0;
    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2;
        value = lead & 0x1FU;
        minimum = 0x80U;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
        length = 3;
        value = lead & 0x0FU;
        minimum = 0x800U;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
        length = 4;
        value = lead & 0x07U;
        minimum = 0x10000U;
    } else {
        return 0;
    }
    if (remaining < length)
        return 0;
    for (std::size_t i = 1; i < length; ++i) {
        const auto continuation = static_cast<unsigned char>(cursor[i]);
        if ((continuation & 0xC0U) != 0x80U)
            return 0;
        value = (value << 6) | (continuation & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU))
        return 0;
    codepoint = value;
    return static_cast<int>(length);
}

bool intercept_editor_compatible(const std::vector<std::uint8_t>& bytes,
                                        std::string& unavailable_reason) {
    if (bytes.empty()) {
        unavailable_reason = "The retained request is empty and cannot be edited or forwarded.";
        return false;
    }
    if (bytes.size() >= k_intercept_editor_capacity) {
        unavailable_reason = "The retained request exceeds the 65535-byte reviewed editor limit; use a binary-safe external workflow.";
        return false;
    }
    const char* cursor = reinterpret_cast<const char*>(bytes.data());
    const char* const end = cursor + bytes.size();
    while (cursor < end) {
        unsigned int codepoint = 0;
        const int consumed = utf8_decode_codepoint(cursor, end, codepoint);
        if (consumed <= 0 || cursor + consumed > end) {
            unavailable_reason = "The retained request contains invalid UTF-8 or binary bytes that the text editor cannot represent safely.";
            return false;
        }
        if ((codepoint < 0x20U && codepoint != '\r' && codepoint != '\n' && codepoint != '\t') ||
            codepoint == 0x7FU) {
            unavailable_reason = "The retained request contains binary control bytes that the text editor cannot represent safely.";
            return false;
        }
        cursor += consumed;
    }
    unavailable_reason.clear();
    return true;
}

bool refresh_intercept_modified_draft(std::string& unavailable_reason) {
    if (!s_intercept_modified_draft.loaded || !s_intercept_modified_draft.editable) {
        unavailable_reason = s_intercept_modified_draft.unavailable_reason.empty()
            ? "The selected request has no editable retained draft."
            : s_intercept_modified_draft.unavailable_reason;
        return false;
    }
    if (s_intercept_modified_draft.raw_request.size() >= k_intercept_editor_capacity) {
        unavailable_reason = "The modified request exceeded its bounded editor capacity.";
        s_intercept_modified_draft.unavailable_reason = unavailable_reason;
        return false;
    }
    if (s_intercept_modified_draft.raw_request.empty()) {
        unavailable_reason = "The modified request is empty and cannot be forwarded.";
        s_intercept_modified_draft.unavailable_reason = unavailable_reason;
        return false;
    }
    s_intercept_modified_draft.content_hash = artifact_hash(std::string_view(
        s_intercept_modified_draft.raw_request));
    s_intercept_modified_draft.unavailable_reason.clear();
    unavailable_reason.clear();
    return true;
}

void retain_intercept_modified_draft(
    const intercept_runtime_snapshot_t& publication,
    const mitm_proxy::http_exchange& exchange) {
    const auto target = intercept_target_identity(publication, exchange);
    if (s_intercept_modified_draft.loaded &&
        intercept_target_matches(s_intercept_modified_draft.source, target, false)) {
        s_intercept_modified_draft.source.publication_generation = publication.generation;
        return;
    }
    s_intercept_modified_draft = {};
    s_intercept_modified_draft.loaded = true;
    s_intercept_modified_draft.source = target;
    s_intercept_modified_draft.editable = intercept_editor_compatible(
        exchange.raw_request, s_intercept_modified_draft.unavailable_reason);
    if (!s_intercept_modified_draft.editable)
        return;
    s_intercept_modified_draft.raw_request.assign(
        exchange.raw_request.begin(), exchange.raw_request.end());
    s_intercept_modified_draft.content_hash = artifact_hash(exchange.raw_request);
}

static void publish_intercept_runtime_snapshot() {
    auto snapshot = std::make_shared<intercept_runtime_snapshot_t>();
    snapshot->generation = s_intercept_snapshot_generation.fetch_add(
        1, std::memory_order_acq_rel) + 1;
    snapshot->running = mitm_proxy::is_running();
    snapshot->enabled = mitm_proxy::is_intercept_enabled();
    snapshot->held = mitm_proxy::get_held_exchanges();
    std::atomic_store_explicit(&s_intercept_runtime_snapshot,
        std::shared_ptr<const intercept_runtime_snapshot_t>(snapshot),
        std::memory_order_release);
    if (s_intercept_snapshot_sink)
        s_intercept_snapshot_sink(std::move(snapshot));
}

void request_intercept_runtime_snapshot(bool force) {
    const std::uint64_t now = network_now_ms();
    const std::uint64_t last = s_intercept_snapshot_requested_ms.load(std::memory_order_acquire);
    if (!force && last != 0 && now >= last && now - last < 200)
        return;
    bool expected = false;
    if (!s_intercept_snapshot_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    s_intercept_snapshot_requested_ms.store(now, std::memory_order_release);
    const bool posted = post_network_task(
        "intercept_snapshot", aida::infra::executor::domain_t::diagnostics, "bounded_task",
        []() {
            try {
                publish_intercept_runtime_snapshot();
            } catch (...) {
            }
            s_intercept_snapshot_pending.store(false, std::memory_order_release);
        }, false);
    if (!posted)
        s_intercept_snapshot_pending.store(false, std::memory_order_release);
}

static bool request_intercept_operation(intercept_operation_t operation, bool enabled,
                                        intercept_target_identity_t target,
                                        std::vector<std::uint8_t> modified_request,
                                        std::uint64_t modified_content_hash,
                                        std::size_t reviewed_count,
                                        std::shared_ptr<const intercept_runtime_snapshot_t>
                                            reviewed_publication = {}) {
    bool expected = false;
    if (!s_intercept_operation_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;
    const std::uint64_t serial = s_intercept_operation_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const char* action = "network.intercept.forward_selected";
    const char* label = "Forward held exchange";
    std::string target_summary = target.exchange_id == 0
        ? std::to_string(reviewed_count) + " held exchanges"
        : "exchange " + std::to_string(target.exchange_id);
    switch (operation) {
    case intercept_operation_t::set_enabled:
        action = "network.intercept.toggle_enabled";
        label = enabled ? "Enable request interception" : "Disable request interception";
        target_summary = enabled ? "interception enabled" : "interception disabled";
        break;
    case intercept_operation_t::forward_all:
        action = "network.intercept.forward_all";
        label = "Forward all held exchanges";
        break;
    case intercept_operation_t::drop_all:
        action = "network.intercept.drop_all";
        label = "Drop all held exchanges";
        break;
    case intercept_operation_t::drop_one:
        action = "network.intercept.drop_selected";
        label = "Drop held exchange";
        break;
    case intercept_operation_t::forward_modified:
        action = "network.intercept.forward_modified";
        label = "Forward modified exchange";
        break;
    default:
        break;
    }
    const std::string task_id = register_network_operation(
        action, label, "view.network.intercept", target_summary);
    const bool posted = post_network_task(
        "intercept_mutation", aida::infra::executor::domain_t::feature_worker, "bounded_task",
        [serial, operation, enabled, target, modified_request = std::move(modified_request),
         modified_content_hash, reviewed_count,
         reviewed_publication = std::move(reviewed_publication), task_id]() {
            bool success = false;
            std::string error;
            try {
                std::vector<mitm_proxy::http_exchange> live_held;
                const auto validate_selected = [&]() -> const mitm_proxy::http_exchange* {
                    if (!reviewed_publication || !target.valid() ||
                        target.publication_generation != reviewed_publication->generation) {
                        error = "The exact reviewed Intercept publication is unavailable";
                        return nullptr;
                    }
                    const auto reviewed = std::find_if(reviewed_publication->held.begin(),
                        reviewed_publication->held.end(), [&](const auto& exchange) {
                            return intercept_exchange_matches(exchange, target);
                        });
                    if (reviewed == reviewed_publication->held.end()) {
                        error = "The reviewed held exchange identity does not match its immutable publication";
                        return nullptr;
                    }
                    live_held = mitm_proxy::get_held_exchanges();
                    const auto live = std::find_if(live_held.begin(), live_held.end(),
                        [&](const auto& exchange) {
                            return intercept_exchange_matches(exchange, target);
                        });
                    if (live == live_held.end()) {
                        error = "The held exchange changed after review; select and review it again";
                        return nullptr;
                    }
                    return &*reviewed;
                };
                const auto validate_all = [&]() {
                    if (!reviewed_publication || reviewed_publication->generation == 0 ||
                        reviewed_count == 0 ||
                        reviewed_count != reviewed_publication->held.size()) {
                        error = "The exact reviewed Intercept publication is unavailable";
                        return false;
                    }
                    live_held = mitm_proxy::get_held_exchanges();
                    for (const auto& reviewed : reviewed_publication->held) {
                        const auto reviewed_target = intercept_target_identity(
                            *reviewed_publication, reviewed);
                        const bool current = std::any_of(live_held.begin(), live_held.end(),
                            [&](const auto& exchange) {
                                return intercept_exchange_matches(exchange, reviewed_target);
                            });
                        if (!current) {
                            error = "A held exchange changed after review; review the current publication again";
                            return false;
                        }
                    }
                    return true;
                };
                switch (operation) {
                case intercept_operation_t::set_enabled:
                    mitm_proxy::set_intercept_enabled(enabled);
                    success = mitm_proxy::is_intercept_enabled() == enabled;
                    break;
                case intercept_operation_t::forward_all:
                    if (!validate_all()) break;
                    for (const auto& exchange : reviewed_publication->held)
                        mitm_proxy::forward_exchange(exchange.id);
                    success = true;
                    break;
                case intercept_operation_t::drop_all:
                    if (!validate_all()) break;
                    for (const auto& exchange : reviewed_publication->held)
                        mitm_proxy::drop_exchange(exchange.id);
                    success = true;
                    break;
                case intercept_operation_t::forward_one:
                    if (!validate_selected()) break;
                    mitm_proxy::forward_exchange(target.exchange_id);
                    success = true;
                    break;
                case intercept_operation_t::drop_one:
                    if (!validate_selected()) break;
                    mitm_proxy::drop_exchange(target.exchange_id);
                    success = true;
                    break;
                case intercept_operation_t::forward_modified: {
                    const auto* reviewed = validate_selected();
                    if (!reviewed) break;
                    if (modified_request.empty() ||
                        modified_request.size() >= k_intercept_editor_capacity ||
                        modified_content_hash == 0 ||
                        artifact_hash(modified_request) != modified_content_hash) {
                        error = "The modified request draft changed after review or exceeded its bounded size";
                        break;
                    }
                    artifact_identity_t canonical_source;
                    if (!validate_reviewed_request(
                            exchange_artifact_identity(*reviewed,
                                artifact_kind_t::intercept_request),
                            modified_request, canonical_source, error))
                        break;
                    mitm_proxy::forward_modified(target.exchange_id, modified_request);
                    success = true;
                    break;
                }
                }
                publish_intercept_runtime_snapshot();
                const auto snapshot = std::atomic_load_explicit(&s_intercept_runtime_snapshot, std::memory_order_acquire);
                if (success && target.exchange_id != 0 && snapshot) {
                    success = std::none_of(snapshot->held.begin(), snapshot->held.end(),
                        [target](const auto& exchange) {
                            return exchange.id == target.exchange_id;
                        });
                } else if (success && reviewed_publication && snapshot &&
                    (operation == intercept_operation_t::forward_all ||
                     operation == intercept_operation_t::drop_all)) {
                    std::vector<std::uint64_t> remaining_ids;
                    remaining_ids.reserve(snapshot->held.size());
                    for (const auto& exchange : snapshot->held)
                        remaining_ids.push_back(exchange.id);
                    std::sort(remaining_ids.begin(), remaining_ids.end());
                    success = std::none_of(reviewed_publication->held.begin(),
                        reviewed_publication->held.end(), [&remaining_ids](const auto& exchange) {
                            return std::binary_search(remaining_ids.begin(), remaining_ids.end(),
                                exchange.id);
                        });
                }
                if (!success && error.empty())
                    error = "The requested intercept state could not be verified";
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Intercept operation failed";
            }
            const std::string detail = success
                ? (reviewed_count > 0 ? std::to_string(reviewed_count) + " exchanges processed" : "Completed")
                : error;
            finish_network_operation(task_id, success, success ? "Completed" : "Failed", detail);
            post_network_ui_completion([serial, success, error = std::move(error)]() {
                if (s_intercept_operation_serial.load(std::memory_order_acquire) != serial)
                    return;
                if (!success)
                    toast_notification::push(error.empty() ? "Intercept operation failed" : error,
                        toast_notification::toast_type_t::error);
                s_intercept_operation_pending.store(false, std::memory_order_release);
                request_intercept_runtime_snapshot(true);
            });
        }, false);
    if (!posted) {
        s_intercept_operation_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected intercept operation");
        return false;
    }
    return true;
}

static bool selected_intercept_command(intercept_command_t command) {
    return command == intercept_command_t::forward_selected ||
        command == intercept_command_t::drop_selected ||
        command == intercept_command_t::forward_modified;
}

static intercept_command_capability_t intercept_command_capability_for(
    intercept_command_t command,
    const std::shared_ptr<const intercept_runtime_snapshot_t>& publication,
    const intercept_target_identity_t& target) {
    if (!publication)
        return {false, "The immutable Intercept publication is still loading"};
    if (!publication->running)
        return {false, "Start the MITM proxy before using Intercept commands"};
    if (s_intercept_operation_pending.load(std::memory_order_acquire))
        return {false, "Wait for the current Intercept operation to finish"};
    if (publication->held.empty())
        return {false, "No held exchanges are available"};
    if (selected_intercept_command(command)) {
        if (!target.valid())
            return {false, "Select a held exchange first"};
        const bool reviewed_exists = target.publication_generation == publication->generation &&
            std::any_of(publication->held.begin(), publication->held.end(),
            [&](const auto& exchange) {
                return intercept_exchange_matches(exchange, target);
            });
        if (!reviewed_exists)
            return {false, "The reviewed held exchange changed; select it again"};
        const auto current = std::atomic_load_explicit(
            &s_intercept_runtime_snapshot, std::memory_order_acquire);
        const bool selected_current = current && current->running &&
            std::any_of(current->held.begin(), current->held.end(),
                [&](const auto& exchange) {
                    return intercept_exchange_matches(exchange, target);
                });
        if (!selected_current)
            return {false, "The selected held exchange changed; select it again"};
        if (command == intercept_command_t::forward_modified) {
            if (!s_intercept_modified_draft.loaded ||
                !intercept_target_matches(
                    s_intercept_modified_draft.source, target, false))
                return {false, "The selected exchange has no retained modified draft"};
            std::string draft_reason;
            if (!refresh_intercept_modified_draft(draft_reason))
                return {false, s_intercept_modified_draft.unavailable_reason.empty()
                    ? "The retained modified draft is unavailable"
                    : s_intercept_modified_draft.unavailable_reason};
            if (s_intercept_editor_mirror.pretty_dirty)
                return {false, "Apply or discard the pending Pretty edits before forwarding raw bytes"};
            if (s_intercept_editor_mirror.oversized || s_intercept_editor_mirror.binary ||
                !s_intercept_editor_mirror.error.empty())
                return {false, s_intercept_editor_mirror.error.empty()
                    ? "The retained modified request is not safely editable"
                    : s_intercept_editor_mirror.error};
        }
    }
    return {true, {}};
}

intercept_command_capability_t intercept_command_capability(intercept_command_t command) {
    const auto publication = std::atomic_load_explicit(
        &s_intercept_runtime_snapshot, std::memory_order_acquire);
    intercept_target_identity_t target;
    if (publication && selected_intercept_command(command)) {
        const auto selected = std::find_if(publication->held.begin(), publication->held.end(),
            [](const auto& exchange) {
                return exchange.id == s_intercept_selected_exchange_id;
            });
        if (selected != publication->held.end())
            target = intercept_target_identity(*publication, *selected);
    }
    return intercept_command_capability_for(command, publication, target);
}

static bool execute_reviewed_intercept_command(
    intercept_command_t command,
    const std::shared_ptr<const intercept_runtime_snapshot_t>& publication,
    const intercept_target_identity_t& target,
    std::string* error) {
    const auto capability = intercept_command_capability_for(command, publication, target);
    if (!capability.enabled) {
        if (error) *error = capability.disabled_reason.empty()
            ? "The Intercept command is unavailable"
            : capability.disabled_reason;
        return false;
    }
    switch (command) {
    case intercept_command_t::forward_selected:
        if (!request_intercept_operation(intercept_operation_t::forward_one, false,
                target, {}, 0, 1, publication)) {
            if (error) *error = "The Intercept executor rejected the selected forward operation";
            return false;
        }
        break;
    case intercept_command_t::drop_selected: {
        const std::string open_error = open_view("view.network.intercept");
        if (!open_error.empty()) {
            if (error) *error = open_error;
            return false;
        }
        s_intercept_drop_review = {true, false, target, 1, publication};
        if (s_intercept_drop_review_display)
            s_intercept_drop_review_display(s_intercept_drop_review);
        break;
    }
    case intercept_command_t::forward_modified: {
        std::string draft_reason;
        if (!refresh_intercept_modified_draft(draft_reason) ||
            !intercept_target_matches(s_intercept_modified_draft.source, target, false)) {
            if (error) *error = draft_reason.empty()
                ? "The retained modified draft no longer matches the selected exchange"
                : draft_reason;
            return false;
        }
        const auto reviewed = std::find_if(publication->held.begin(), publication->held.end(),
            [&](const auto& exchange) {
                return intercept_exchange_matches(exchange, target);
            });
        if (reviewed == publication->held.end()) {
            if (error) *error = "The selected exchange changed before modified forwarding";
            return false;
        }
        std::vector<std::uint8_t> modified_request(
            s_intercept_modified_draft.raw_request.begin(),
            s_intercept_modified_draft.raw_request.end());
        artifact_identity_t canonical_source;
        if (!validate_reviewed_request(
                exchange_artifact_identity(*reviewed, artifact_kind_t::intercept_request),
                modified_request, canonical_source, draft_reason)) {
            if (error) *error = draft_reason.empty()
                ? "The modified request failed reviewed HTTP validation" : draft_reason;
            return false;
        }
        if (!request_intercept_operation(intercept_operation_t::forward_modified, false,
                target, std::move(modified_request),
                s_intercept_modified_draft.content_hash, 1, publication)) {
            if (error) *error = "The Intercept executor rejected the modified forward operation";
            return false;
        }
        break;
    }
    case intercept_command_t::forward_all:
        if (!request_intercept_operation(intercept_operation_t::forward_all, false,
                {}, {}, 0, publication->held.size(), publication)) {
            if (error) *error = "The Intercept executor rejected the forward-all operation";
            return false;
        }
        break;
    case intercept_command_t::drop_all: {
        const std::string open_error = open_view("view.network.intercept");
        if (!open_error.empty()) {
            if (error) *error = open_error;
            return false;
        }
        s_intercept_drop_review = {true, true, {}, publication->held.size(), publication};
        if (s_intercept_drop_review_display)
            s_intercept_drop_review_display(s_intercept_drop_review);
        break;
    }
    }
    if (error) error->clear();
    return true;
}

bool execute_intercept_command(intercept_command_t command, std::string* error) {
    const auto publication = std::atomic_load_explicit(
        &s_intercept_runtime_snapshot, std::memory_order_acquire);
    intercept_target_identity_t target;
    if (publication && selected_intercept_command(command)) {
        const auto selected = std::find_if(publication->held.begin(), publication->held.end(),
            [](const auto& exchange) {
                return exchange.id == s_intercept_selected_exchange_id;
            });
        if (selected != publication->held.end())
            target = intercept_target_identity(*publication, *selected);
    }
    return execute_reviewed_intercept_command(command, publication, target, error);
}

bool confirm_intercept_drop_review() {
    const bool retained_target = s_intercept_drop_review.reviewed_publication &&
        s_intercept_drop_review.reviewed_count != 0 &&
        (s_intercept_drop_review.all ||
            s_intercept_drop_review.target.valid());
    if (!retained_target)
        return false;
    const bool accepted = request_intercept_operation(
        s_intercept_drop_review.all ? intercept_operation_t::drop_all
                                    : intercept_operation_t::drop_one,
        false, s_intercept_drop_review.target, {}, 0,
        s_intercept_drop_review.reviewed_count,
        s_intercept_drop_review.reviewed_publication);
    if (accepted)
        s_intercept_drop_review = {};
    return accepted;
}

void cancel_intercept_drop_review() noexcept {
    s_intercept_drop_review = {};
}

enum class keylog_operation_t {
    launch_and_watch,
    watch_file,
    stop_watching,
    clear_entries
};

static std::shared_ptr<const keylog_runtime_snapshot_t> s_keylog_runtime_snapshot;
static std::atomic<bool> s_keylog_snapshot_pending{false};
static std::atomic<std::uint64_t> s_keylog_snapshot_requested_ms{0};
static std::atomic<bool> s_keylog_operation_pending{false};
static std::atomic<std::uint64_t> s_keylog_operation_serial{0};
static keylog_snapshot_sink_t s_keylog_snapshot_sink;

void set_keylog_snapshot_sink(keylog_snapshot_sink_t sink) {
    s_keylog_snapshot_sink = std::move(sink);
}

std::shared_ptr<const keylog_runtime_snapshot_t> keylog_runtime_snapshot() {
    return std::atomic_load_explicit(&s_keylog_runtime_snapshot, std::memory_order_acquire);
}

bool keylog_operation_pending() {
    return s_keylog_operation_pending.load(std::memory_order_acquire);
}

static void publish_keylog_runtime_snapshot(const std::string& known_path = {}) {
    auto snapshot = std::make_shared<keylog_runtime_snapshot_t>();
    snapshot->watching = ssl_keylog::is_watching();
    snapshot->entries = ssl_keylog::get_entries(500);
    snapshot->entry_count = ssl_keylog::entry_count();
    if (!known_path.empty()) {
        snapshot->path = known_path;
    } else {
        const auto previous = std::atomic_load_explicit(&s_keylog_runtime_snapshot, std::memory_order_acquire);
        if (previous)
            snapshot->path = previous->path;
    }
    if (!snapshot->watching)
        snapshot->path.clear();
    std::atomic_store_explicit(&s_keylog_runtime_snapshot,
        std::shared_ptr<const keylog_runtime_snapshot_t>(snapshot),
        std::memory_order_release);
    if (s_keylog_snapshot_sink)
        s_keylog_snapshot_sink(std::move(snapshot));
}

void request_keylog_runtime_snapshot(bool force) {
    const std::uint64_t now = network_now_ms();
    const std::uint64_t last = s_keylog_snapshot_requested_ms.load(std::memory_order_acquire);
    if (!force && last != 0 && now >= last && now - last < 250)
        return;
    bool expected = false;
    if (!s_keylog_snapshot_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    s_keylog_snapshot_requested_ms.store(now, std::memory_order_release);
    const bool posted = post_network_task(
        "keylog_snapshot", aida::infra::executor::domain_t::diagnostics, "bounded_task",
        []() {
            try {
                publish_keylog_runtime_snapshot();
            } catch (...) {
            }
            s_keylog_snapshot_pending.store(false, std::memory_order_release);
        }, false);
    if (!posted)
        s_keylog_snapshot_pending.store(false, std::memory_order_release);
}

static void request_keylog_operation(keylog_operation_t operation, std::string path,
                                     std::string arguments, std::size_t reviewed_count,
                                     ssl_keylog::retained_set_token reviewed_token = {}) {
    bool expected = false;
    if (!s_keylog_operation_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = s_keylog_operation_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const char* action = "network.keylog.watch";
    const char* label = "Watch TLS keylog file";
    std::string target = path;
    switch (operation) {
    case keylog_operation_t::launch_and_watch:
        action = "network.keylog.launch";
        label = "Launch target with TLS key logging";
        break;
    case keylog_operation_t::stop_watching:
        action = "network.keylog.stop";
        label = "Stop TLS keylog watcher";
        break;
    case keylog_operation_t::clear_entries:
        action = "network.keylog.clear";
        label = "Clear captured TLS keys";
        target = std::to_string(reviewed_count) + " captured keys";
        break;
    default:
        break;
    }
    const std::string task_id = register_network_operation(action, label, "view.network.keylog", target);
    const bool posted = post_network_task(
        "keylog_mutation",
        operation == keylog_operation_t::launch_and_watch
            ? aida::infra::executor::domain_t::external_tool
            : aida::infra::executor::domain_t::feature_worker,
        "bounded_task",
        [serial, operation, path = std::move(path), arguments = std::move(arguments),
         reviewed_count, reviewed_token, task_id]() mutable {
            bool success = false;
            std::string effective_path = path;
            std::string error;
            std::uint32_t launched_pid = 0;
            try {
                switch (operation) {
                case keylog_operation_t::launch_and_watch: {
                    auto result = ssl_keylog::launch_with_keylog(path, arguments);
                    success = result.success;
                    launched_pid = result.pid;
                    effective_path = result.keylog_path;
                    if (success) {
                        ssl_keylog::start_watching(effective_path);
                        success = ssl_keylog::is_watching();
                    }
                    if (!success)
                        error = result.error.empty() ? "TLS keylog watcher did not start" : result.error;
                    break;
                }
                case keylog_operation_t::watch_file:
                    ssl_keylog::start_watching(path);
                    success = ssl_keylog::is_watching();
                    if (!success) error = "TLS keylog watcher did not start";
                    break;
                case keylog_operation_t::stop_watching:
                    ssl_keylog::stop_watching();
                    success = !ssl_keylog::is_watching();
                    if (!success) error = "TLS keylog watcher did not stop";
                    break;
                case keylog_operation_t::clear_entries:
                    if (reviewed_token.count != reviewed_count ||
                        !ssl_keylog::clear_entries_if_exact(reviewed_token)) {
                        error = "TLS secrets changed after confirmation; review the current retained set again";
                    } else {
                        success = ssl_keylog::entry_count() == 0;
                        if (!success) error = "Captured TLS keys remained after clear";
                    }
                    break;
                }
                publish_keylog_runtime_snapshot(success ? effective_path : std::string());
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "TLS keylog operation failed";
            }
            std::string detail;
            if (success && operation == keylog_operation_t::launch_and_watch)
                detail = "PID " + std::to_string(launched_pid) + "; watching " + effective_path;
            else if (success && operation == keylog_operation_t::clear_entries)
                detail = std::to_string(reviewed_count) + " captured keys cleared";
            else
                detail = success ? "Completed" : error;
            finish_network_operation(task_id, success, success ? "Completed" : "Failed", detail);
            post_network_ui_completion([serial, operation, success, effective_path = std::move(effective_path),
                                   error = std::move(error)]() mutable {
                if (s_keylog_operation_serial.load(std::memory_order_acquire) != serial)
                    return;
                if (success && operation == keylog_operation_t::launch_and_watch)
                    toast_notification::push("Process launched; TLS keylog watcher is active",
                        toast_notification::toast_type_t::info);
                else if (!success)
                    toast_notification::push(error.empty() ? "TLS keylog operation failed" : error,
                        toast_notification::toast_type_t::error);
                s_keylog_operation_pending.store(false, std::memory_order_release);
                request_keylog_runtime_snapshot(true);
            });
        }, false);
    if (!posted) {
        s_keylog_operation_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected", "Executor rejected TLS keylog operation");
    }
}

bool filter_mutation_pending() {
    return g_state.filter_mutation_pending.load(std::memory_order_acquire);
}

std::string filter_draft_error() {
    const state_t& state = g_state;
    char* end = nullptr;
    errno = 0;
    const unsigned long pid = state.nf_pid[0] ? std::strtoul(state.nf_pid, &end, 10) : 0;
    if (errno != 0 || (state.nf_pid[0] && (!end || *end != '\0')) || pid > UINT32_MAX)
        return "PID must be an unsigned 32-bit integer";
    end = nullptr;
    errno = 0;
    const unsigned long port = state.nf_port[0] ? std::strtoul(state.nf_port, &end, 10) : 0;
    if (errno != 0 || (state.nf_port[0] && (!end || *end != '\0')) || port > UINT16_MAX)
        return "Port must be between 0 and 65535";
    std::array<std::uint8_t, 16> bytes{};
    if (state.nf_ip[0] && inet_pton(AF_INET, state.nf_ip, bytes.data()) != 1)
        return "Filter IP address is not valid IPv4";
    if (state.nf_action < 0 || state.nf_action > 1)
        return "Choose Block or Allow for the filter action";
    if (state.nf_direction < 0 || state.nf_direction > 2)
        return "Choose In, Out, or Both for the filter direction";
    if (state.nf_protocol != 0 && state.nf_protocol != 6 && state.nf_protocol != 17)
        return "Choose Any, TCP, or UDP for the filter protocol";
    return {};
}

static std::string filter_rule_summary(const filter_entry_t& rule) {
    std::string summary = "rule #" + std::to_string(rule.rule_id) + " " +
        (rule.action == 0 ? "BLOCK" : "ALLOW") + " " +
        (rule.direction == 0 ? "IN" : rule.direction == 1 ? "OUT" : "BOTH") + " " +
        (rule.protocol == 6 ? "TCP" : rule.protocol == 17 ? "UDP" : "ANY") +
        " PID " + std::to_string(rule.pid) + " port " + std::to_string(rule.port);
    if (!rule.ip_addr.empty()) summary.append(" IP ").append(rule.ip_addr);
    return summary;
}

static bool command_requires_confirmation(operational_command_t command) noexcept {
    return command == operational_command_t::proxy_history_clear ||
        command == operational_command_t::proxy_ca_trust_repair ||
        command == operational_command_t::filter_add ||
        command == operational_command_t::filter_remove_selected ||
        command == operational_command_t::filter_clear ||
        command == operational_command_t::keylog_clear;
}

operational_command_capability_t operational_command_capability(
    operational_command_t command) {
    operational_command_capability_t capability;
    capability.checked = false;
    if (!g_state.active) {
        capability.disabled_reason = "The Network runtime is not initialized";
        return capability;
    }
    const bool capture_pending = g_state.cap_start_pending.load(std::memory_order_acquire) ||
        g_state.cap_stop_pending.load(std::memory_order_acquire);
    const bool capture_running = g_state.cap_running.load(std::memory_order_acquire);
    const bool proxy_pending = s_proxy_operation_pending.load(std::memory_order_acquire);
    const bool filter_pending = g_state.filter_mutation_pending.load(std::memory_order_acquire);
    const bool keylog_pending = s_keylog_operation_pending.load(std::memory_order_acquire);
    const auto proxy = std::atomic_load_explicit(&s_proxy_runtime_snapshot, std::memory_order_acquire);
    const auto intercept = std::atomic_load_explicit(&s_intercept_runtime_snapshot, std::memory_order_acquire);
    const auto keylog = std::atomic_load_explicit(&s_keylog_runtime_snapshot, std::memory_order_acquire);
    switch (command) {
    case operational_command_t::capture_start:
        request_driver_available_snapshot();
        if (capture_pending) capability.disabled_reason = "A capture state change is already in progress";
        else if (capture_running) capability.disabled_reason = "Packet capture is already running";
        else if (!s_driver_available_snapshot.load(std::memory_order_acquire))
            capability.disabled_reason = "Driver-backed packet capture is unavailable";
        else capability.enabled = true;
        capability.target_summary = "PID " + std::to_string(g_state.cap_filter_pid) +
            ", port " + std::to_string(g_state.cap_filter_port) +
            ", protocol " + std::to_string(g_state.cap_filter_protocol);
        break;
    case operational_command_t::capture_stop:
        if (capture_pending) capability.disabled_reason = "A capture state change is already in progress";
        else if (!capture_running) capability.disabled_reason = "Packet capture is not running";
        else capability.enabled = true;
        capability.target_summary = "the active driver-backed packet capture";
        break;
    case operational_command_t::proxy_start:
        if (proxy_pending) capability.disabled_reason = "A Proxy state change is already in progress";
        else if (mitm_proxy::is_running()) capability.disabled_reason = "The interception Proxy is already running";
        else if (g_state.proxy_bind_addr[0] == '\0') capability.disabled_reason = "Enter a Proxy bind address first";
        else if (g_state.proxy_port < 1 || g_state.proxy_port > 65535) capability.disabled_reason = "Proxy port must be between 1 and 65535";
        else capability.enabled = true;
        capability.target_summary = std::string(g_state.proxy_bind_addr) + ":" +
            std::to_string(g_state.proxy_port) +
            (g_state.proxy_decode_tls ? " with TLS interception" : " without TLS interception");
        break;
    case operational_command_t::proxy_stop:
        if (proxy_pending) capability.disabled_reason = "A Proxy state change is already in progress";
        else if (!mitm_proxy::is_running()) capability.disabled_reason = "The interception Proxy is not running";
        else capability.enabled = true;
        capability.target_summary = "the active interception Proxy listener";
        break;
    case operational_command_t::proxy_history_clear:
        request_proxy_runtime_snapshot();
        if (proxy_pending) capability.disabled_reason = "A Proxy operation is already in progress";
        else if (!proxy) capability.disabled_reason = "Proxy history is still loading";
        else if (proxy->history.empty()) capability.disabled_reason = "Proxy history is already empty";
        else capability.enabled = true;
        capability.target_summary = proxy
            ? std::to_string(proxy->history.size()) + " retained exchanges, including requests, responses, annotations, and evidence"
            : "the retained Proxy history";
        break;
    case operational_command_t::proxy_ca_trust_repair:
        request_proxy_runtime_snapshot();
        if (proxy_pending) capability.disabled_reason = "A Proxy operation is already in progress";
        else if (!proxy) capability.disabled_reason = "CA trust status is still loading";
        else if (proxy->ca_installed) capability.disabled_reason = "The AiDA interception CA is already trusted for the current user";
        else capability.enabled = true;
        capability.target_summary = "the current-user root trust store for controlled Camoufox interception";
        break;
    case operational_command_t::filter_add: {
        request_driver_available_snapshot();
        const std::string validation = filter_draft_error();
        if (filter_pending) capability.disabled_reason = "A network filter mutation is already in progress";
        else if (!s_driver_available_snapshot.load(std::memory_order_acquire))
            capability.disabled_reason = "Driver-backed filter control is unavailable";
        else if (!validation.empty()) capability.disabled_reason = validation;
        else capability.enabled = true;
        capability.target_summary = std::string(g_state.nf_action == 0 ? "BLOCK" : "ALLOW") + " " +
            (g_state.nf_direction == 0 ? "IN" : g_state.nf_direction == 1 ? "OUT" : "BOTH") + " " +
            (g_state.nf_protocol == 6 ? "TCP" : g_state.nf_protocol == 17 ? "UDP" : "ANY") +
            " PID " + (g_state.nf_pid[0] ? g_state.nf_pid : "0") + " port " +
            (g_state.nf_port[0] ? g_state.nf_port : "0") +
            (g_state.nf_ip[0] ? std::string(" IP ") + g_state.nf_ip : std::string());
        break;
    }
    case operational_command_t::filter_remove_selected:
        request_driver_available_snapshot();
        if (filter_pending) capability.disabled_reason = "A network filter mutation is already in progress";
        else if (!s_driver_available_snapshot.load(std::memory_order_acquire))
            capability.disabled_reason = "Driver-backed filter control is unavailable";
        else if (g_state.filter_selected < 0 ||
                 g_state.filter_selected >= static_cast<int>(g_state.filters.size()))
            capability.disabled_reason = "Select a retained network filter rule first";
        else capability.enabled = true;
        capability.target_summary = capability.enabled
            ? filter_rule_summary(g_state.filters[static_cast<std::size_t>(g_state.filter_selected)])
            : "the selected retained network filter rule";
        break;
    case operational_command_t::filter_clear:
        request_driver_available_snapshot();
        if (filter_pending) capability.disabled_reason = "A network filter mutation is already in progress";
        else if (!s_driver_available_snapshot.load(std::memory_order_acquire))
            capability.disabled_reason = "Driver-backed filter control is unavailable";
        else if (g_state.filters.empty()) capability.disabled_reason = "There are no retained network filter rules to clear";
        else capability.enabled = true;
        capability.target_summary = std::to_string(g_state.filters.size()) + " retained live kernel traffic rules";
        break;
    case operational_command_t::intercept_toggle:
        request_intercept_runtime_snapshot();
        if (s_intercept_operation_pending.load(std::memory_order_acquire))
            capability.disabled_reason = "An Intercept operation is already in progress";
        else if (!intercept) capability.disabled_reason = "Intercept state is still loading";
        else capability.enabled = true;
        capability.checked = intercept && intercept->enabled;
        capability.target_summary = capability.checked
            ? "disable request interception while preserving held exchanges"
            : "enable request interception for new Proxy exchanges";
        break;
    case operational_command_t::keylog_launch:
        request_keylog_runtime_snapshot();
        if (keylog_pending) capability.disabled_reason = "A TLS keylog operation is already in progress";
        else if (keylog && keylog->watching) capability.disabled_reason = "A TLS keylog source is already being watched";
        else if (g_state.kl_exe_path[0] == '\0') capability.disabled_reason = "Choose an executable to launch first";
        else capability.enabled = true;
        capability.target_summary = std::string(g_state.kl_exe_path) +
            (g_state.kl_args[0] ? std::string(" ") + g_state.kl_args : std::string());
        break;
    case operational_command_t::keylog_watch:
        request_keylog_runtime_snapshot();
        if (keylog_pending) capability.disabled_reason = "A TLS keylog operation is already in progress";
        else if (keylog && keylog->watching) capability.disabled_reason = "A TLS keylog source is already being watched";
        else if (g_state.kl_watch_path[0] == '\0') capability.disabled_reason = "Choose or enter a TLS keylog file first";
        else capability.enabled = true;
        capability.target_summary = g_state.kl_watch_path;
        break;
    case operational_command_t::keylog_stop:
        request_keylog_runtime_snapshot();
        if (keylog_pending) capability.disabled_reason = "A TLS keylog operation is already in progress";
        else if (!keylog || !keylog->watching) capability.disabled_reason = "No TLS keylog source is being watched";
        else capability.enabled = true;
        capability.target_summary = keylog && !keylog->path.empty()
            ? keylog->path : "the active TLS keylog source";
        break;
    case operational_command_t::keylog_clear:
        request_keylog_runtime_snapshot();
        {
        const auto token = ssl_keylog::retained_token();
        if (keylog_pending) capability.disabled_reason = "A TLS keylog operation is already in progress";
        else if (token.count == 0) capability.disabled_reason = "There are no captured TLS secrets to clear";
        else capability.enabled = true;
        capability.target_summary = std::to_string(token.count) + " retained TLS secrets";
        }
        break;
    }
    return capability;
}

bool prepare_operational_command_confirmation(operational_command_t command,
                                              std::string* error) {
    if (error) error->clear();
    if (!command_requires_confirmation(command)) {
        if (error) *error = "This Network command does not require confirmation";
        return false;
    }
    const auto capability = operational_command_capability(command);
    if (!capability.enabled) {
        if (error) *error = capability.disabled_reason;
        return false;
    }
    operational_review_binding_t binding;
    binding.prepared = true;
    binding.command = command;
    if (command == operational_command_t::proxy_history_clear) {
        const auto proxy = std::atomic_load_explicit(&s_proxy_runtime_snapshot, std::memory_order_acquire);
        if (!proxy) {
            if (error) *error = "Proxy history changed before review could be prepared";
            return false;
        }
        binding.retained_count = proxy->history.size();
        binding.retained_exchange_ids.reserve(proxy->history.size());
        for (const auto& exchange : proxy->history)
            binding.retained_exchange_ids.push_back(exchange.id);
    } else if (command == operational_command_t::filter_add) {
        binding.filter_action = g_state.nf_action;
        binding.filter_direction = g_state.nf_direction;
        binding.filter_protocol = g_state.nf_protocol;
        binding.filter_pid = g_state.nf_pid;
        binding.filter_port = g_state.nf_port;
        binding.filter_ip = g_state.nf_ip;
    } else if (command == operational_command_t::filter_remove_selected) {
        binding.retained_rule = g_state.filters[static_cast<std::size_t>(g_state.filter_selected)];
    } else if (command == operational_command_t::filter_clear) {
        binding.retained_count = g_state.filters.size();
        binding.retained_rule_ids.reserve(g_state.filters.size());
        for (const auto& rule : g_state.filters)
            binding.retained_rule_ids.push_back(rule.rule_id);
    } else if (command == operational_command_t::keylog_clear) {
        const auto keylog = std::atomic_load_explicit(&s_keylog_runtime_snapshot, std::memory_order_acquire);
        if (!keylog) {
            if (error) *error = "TLS keylog state changed before review could be prepared";
            return false;
        }
        binding.retained_keylog_token = ssl_keylog::retained_token();
        binding.retained_count = binding.retained_keylog_token.count;
        if (binding.retained_count == 0) {
            if (error) *error = "There are no captured TLS secrets to clear";
            return false;
        }
    }
    s_operational_review = std::move(binding);
    return true;
}

void cancel_operational_command_confirmation(operational_command_t command) noexcept {
    if (s_operational_review.prepared && s_operational_review.command == command)
        s_operational_review = {};
}

bool execute_operational_command(operational_command_t command, std::string* error) {
    if (error) error->clear();
    const auto capability = operational_command_capability(command);
    if (!capability.enabled) {
        if (error) *error = capability.disabled_reason;
        return false;
    }
    if (command_requires_confirmation(command) &&
        (!s_operational_review.prepared || s_operational_review.command != command)) {
        if (error) *error = "The reviewed Network target is unavailable; review the command again";
        return false;
    }
    if (command == operational_command_t::proxy_history_clear) {
        const auto proxy = std::atomic_load_explicit(&s_proxy_runtime_snapshot, std::memory_order_acquire);
        std::vector<std::uint64_t> current;
        if (proxy) {
            current.reserve(proxy->history.size());
            for (const auto& exchange : proxy->history) current.push_back(exchange.id);
        }
        if (!proxy || current != s_operational_review.retained_exchange_ids) {
            s_operational_review = {};
            if (error) *error = "Proxy history changed after review; review the current retained exchanges again";
            return false;
        }
        const std::size_t count = s_operational_review.retained_count;
        auto reviewed_ids = std::move(s_operational_review.retained_exchange_ids);
        s_operational_review = {};
        request_proxy_history_clear(count, std::move(reviewed_ids));
        return s_proxy_operation_pending.load(std::memory_order_acquire);
    }
    if (command == operational_command_t::filter_add) {
        const bool unchanged = s_operational_review.filter_action == g_state.nf_action &&
            s_operational_review.filter_direction == g_state.nf_direction &&
            s_operational_review.filter_protocol == g_state.nf_protocol &&
            s_operational_review.filter_pid == g_state.nf_pid &&
            s_operational_review.filter_port == g_state.nf_port &&
            s_operational_review.filter_ip == g_state.nf_ip;
        s_operational_review = {};
        if (!unchanged) {
            if (error) *error = "The network filter draft changed after review; review the current rule again";
            return false;
        }
        request_filter_add(g_state);
        return g_state.filter_mutation_pending.load(std::memory_order_acquire);
    }
    if (command == operational_command_t::filter_remove_selected) {
        const filter_entry_t reviewed = s_operational_review.retained_rule;
        const auto found = std::find_if(g_state.filters.begin(), g_state.filters.end(),
            [&reviewed](const filter_entry_t& rule) {
                return rule.rule_id == reviewed.rule_id && rule.action == reviewed.action &&
                    rule.direction == reviewed.direction && rule.protocol == reviewed.protocol &&
                    rule.pid == reviewed.pid && rule.port == reviewed.port &&
                    rule.ip_addr == reviewed.ip_addr && rule.active == reviewed.active;
            });
        s_operational_review = {};
        if (found == g_state.filters.end()) {
            if (error) *error = "The retained network filter rule changed after review; select it again";
            return false;
        }
        request_filter_remove(g_state, reviewed.rule_id);
        return g_state.filter_mutation_pending.load(std::memory_order_acquire);
    }
    if (command == operational_command_t::filter_clear) {
        std::vector<std::uint32_t> current;
        current.reserve(g_state.filters.size());
        for (const auto& rule : g_state.filters) current.push_back(rule.rule_id);
        if (current != s_operational_review.retained_rule_ids) {
            s_operational_review = {};
            if (error) *error = "The retained network filter set changed after review; review it again";
            return false;
        }
        s_operational_review = {};
        request_filter_clear(g_state);
        return g_state.filter_mutation_pending.load(std::memory_order_acquire);
    }
    if (command == operational_command_t::keylog_clear) {
        const auto current_token = ssl_keylog::retained_token();
        const auto reviewed_token = s_operational_review.retained_keylog_token;
        if (current_token.generation != reviewed_token.generation ||
            current_token.count != reviewed_token.count) {
            s_operational_review = {};
            if (error) *error = "The retained TLS secret set changed after review; review it again";
            return false;
        }
        const std::size_t count = s_operational_review.retained_count;
        s_operational_review = {};
        request_keylog_operation(keylog_operation_t::clear_entries, {}, {}, count,
            reviewed_token);
        return s_keylog_operation_pending.load(std::memory_order_acquire);
    }
    if (command == operational_command_t::proxy_ca_trust_repair) {
        s_operational_review = {};
        request_ca_trust_repair();
        return s_proxy_operation_pending.load(std::memory_order_acquire);
    }
    switch (command) {
    case operational_command_t::capture_start: request_capture_start(g_state); return g_state.cap_start_pending.load(std::memory_order_acquire);
    case operational_command_t::capture_stop: request_capture_stop(g_state); return g_state.cap_stop_pending.load(std::memory_order_acquire);
    case operational_command_t::proxy_start: request_proxy_control(g_state, true); return s_proxy_operation_pending.load(std::memory_order_acquire);
    case operational_command_t::proxy_stop: request_proxy_control(g_state, false); return s_proxy_operation_pending.load(std::memory_order_acquire);
    case operational_command_t::intercept_toggle: {
        const auto intercept = std::atomic_load_explicit(&s_intercept_runtime_snapshot, std::memory_order_acquire);
        if (!intercept) {
            if (error) *error = "Intercept state is no longer retained";
            return false;
        }
        return request_intercept_operation(
            intercept_operation_t::set_enabled, !intercept->enabled,
            {}, {}, 0, 0);
    }
    case operational_command_t::keylog_launch:
        request_keylog_operation(keylog_operation_t::launch_and_watch,
            g_state.kl_exe_path, g_state.kl_args, 0);
        return s_keylog_operation_pending.load(std::memory_order_acquire);
    case operational_command_t::keylog_watch:
        request_keylog_operation(keylog_operation_t::watch_file,
            g_state.kl_watch_path, {}, 0);
        return s_keylog_operation_pending.load(std::memory_order_acquire);
    case operational_command_t::keylog_stop: {
        const auto keylog = std::atomic_load_explicit(&s_keylog_runtime_snapshot, std::memory_order_acquire);
        request_keylog_operation(keylog_operation_t::stop_watching,
            keylog ? keylog->path : std::string(), {}, keylog ? keylog->entry_count : 0);
        return s_keylog_operation_pending.load(std::memory_order_acquire);
    }
    default:
        break;
    }
    if (error) *error = "The Network command could not be dispatched";
    return false;
}

static std::vector<std::uint8_t> sitemap_request_bytes(
    const aida::burp::exchange_observed_t& exchange) {
    std::string raw = exchange.method.empty() ? "GET" : exchange.method;
    raw.push_back(' ');
    raw.append(exchange.path.empty() ? "/" : exchange.path);
    if (!exchange.query.empty()) raw.append("?").append(exchange.query);
    raw.append(" HTTP/1.1\r\n");
    bool has_host = false;
    for (const auto& header : exchange.req_headers) {
        std::string name = header.first;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        has_host = has_host || name == "host";
        raw.append(header.first).append(": ").append(header.second).append("\r\n");
    }
    if (!has_host) raw.append("Host: ").append(exchange.host).append("\r\n");
    raw.append("\r\n");
    raw.append(reinterpret_cast<const char*>(exchange.req_body.data()), exchange.req_body.size());
    return {raw.begin(), raw.end()};
}

static std::vector<std::uint8_t> sitemap_response_bytes(
    const aida::burp::exchange_observed_t& exchange) {
    std::string raw = "HTTP/1.1 " + std::to_string(exchange.status_code) + " " +
        exchange.reason_phrase + "\r\n";
    for (const auto& header : exchange.resp_headers)
        raw.append(header.first).append(": ").append(header.second).append("\r\n");
    raw.append("\r\n");
    raw.append(reinterpret_cast<const char*>(exchange.resp_body.data()), exchange.resp_body.size());
    return {raw.begin(), raw.end()};
}

bool make_sitemap_artifact(std::uint64_t exchange_id, artifact_kind_t kind,
                           artifact_identity_t& identity, std::string& unavailable_reason) {
    if (kind != artifact_kind_t::sitemap_request &&
        kind != artifact_kind_t::sitemap_response) {
        unavailable_reason = "Site Map artifacts must identify a request or response.";
        return false;
    }
    aida::burp::exchange_observed_t exchange;
    if (!aida::burp::sitemap::find_exchange(exchange_id, exchange)) {
        unavailable_reason = "The Site Map exchange is no longer retained.";
        return false;
    }
    const bool response = kind == artifact_kind_t::sitemap_response;
    if (response && exchange.status_code <= 0 && exchange.resp_headers.empty() &&
        exchange.resp_body.empty()) {
        identity = {};
        unavailable_reason = "No response has been retained for this Site Map exchange.";
        return false;
    }
    const auto bytes = response ? sitemap_response_bytes(exchange) : sitemap_request_bytes(exchange);
    identity = {};
    identity.kind = kind;
    identity.source_id = exchange.id;
    identity.timestamp = exchange.timestamp_ms;
    identity.target_host = exchange.host;
    identity.target_port = exchange.port;
    identity.use_tls = exchange.scheme == "https";
    identity.parent_id = "network.sitemap.exchange." + std::to_string(exchange.id);
    identity.id = identity.parent_id + (response ? ".response" : ".request");
    identity.source_view_id = "view.network.sitemap";
    identity.label = std::string(response ? "Site Map response #" : "Site Map request #") +
        std::to_string(exchange.id);
    identity.content_size = bytes.size();
    identity.content_hash = artifact_hash(bytes);
    unavailable_reason.clear();
    return true;
}

bool resolve_artifact(const artifact_identity_t& identity, artifact_snapshot_t& snapshot,
                      std::string& unavailable_reason) {
    snapshot = artifact_snapshot_t{};
    if (!identity.valid()) {
        unavailable_reason = "The network artifact identity is incomplete; select the item again.";
        return false;
    }
    snapshot.identity = identity;
    switch (identity.kind) {
    case artifact_kind_t::request:
    case artifact_kind_t::response:
    case artifact_kind_t::exchange: {
        const auto proxy_snapshot = std::atomic_load_explicit(
            &s_proxy_runtime_snapshot, std::memory_order_acquire);
        if (!proxy_snapshot) {
            unavailable_reason = "The proxy history snapshot is still loading; retry after the view refreshes.";
            return false;
        }
        const auto found = std::find_if(proxy_snapshot->history.begin(), proxy_snapshot->history.end(),
            [&](const mitm_proxy::http_exchange& exchange) {
            return exchange.id == identity.source_id && exchange.timestamp == identity.timestamp;
        });
        if (found == proxy_snapshot->history.end()) {
            unavailable_reason = "The captured exchange is no longer retained; select a current history item.";
            return false;
        }
        snapshot.identity.target_host = found->target_host;
        snapshot.identity.target_port = found->target_port;
        snapshot.identity.use_tls = found->is_tls;
        snapshot.bytes = identity.kind == artifact_kind_t::response ? found->raw_response : found->raw_request;
        break;
    }
    case artifact_kind_t::intercept_request: {
        const auto publication = std::atomic_load_explicit(
            &s_intercept_runtime_snapshot, std::memory_order_acquire);
        if (!publication) {
            unavailable_reason = "The immutable Intercept publication is still loading; select the held request again.";
            return false;
        }
        const auto found = std::find_if(publication->held.begin(), publication->held.end(),
            [&](const mitm_proxy::http_exchange& exchange) {
                return exchange.id == identity.source_id &&
                    exchange.timestamp == identity.timestamp;
            });
        if (found == publication->held.end()) {
            unavailable_reason = "The held request is no longer retained; select a current Intercept row.";
            return false;
        }
        snapshot.identity.target_host = found->target_host;
        snapshot.identity.target_port = found->target_port;
        snapshot.identity.use_tls = found->is_tls;
        snapshot.bytes = found->raw_request;
        break;
    }
    case artifact_kind_t::packet: {
        const auto capture_snapshot = capture_packets_snapshot();
        if (!capture_snapshot) {
            unavailable_reason = "The capture snapshot is still loading; retry after the view refreshes.";
            return false;
        }
        const auto found = std::find_if(capture_snapshot->begin(), capture_snapshot->end(),
            [&](const packet_entry_t& packet) {
                return packet.timestamp == identity.timestamp && packet.payload.size() == identity.content_size;
            });
        if (found == capture_snapshot->end()) {
            unavailable_reason = "The captured packet rolled out of the bounded capture history; select a current packet.";
            return false;
        }
        snapshot.bytes = found->payload;
        break;
    }
    case artifact_kind_t::websocket_frame: {
        if (!aida::qt::net::WsFrameStore::instance().findPayload(
                identity.source_id, identity.timestamp, identity.content_size,
                snapshot.bytes)) {
            unavailable_reason = "The WebSocket frame is no longer retained; select a current frame.";
            return false;
        }
        break;
    }
    case artifact_kind_t::repeater_request: {
        if (!s_repeater_artifact_publication_ready.load(std::memory_order_acquire)) {
            unavailable_reason = "The Repeater request publication is unavailable; select the request again after the view refreshes.";
            return false;
        }
        const auto publication = std::atomic_load_explicit(
            &s_repeater_artifact_publication, std::memory_order_acquire);
        if (!publication) {
            unavailable_reason = "The Repeater request snapshot is still loading; select the request again.";
            return false;
        }
        const auto found = std::find_if(publication->requests.begin(), publication->requests.end(),
            [&](const std::shared_ptr<const artifact_snapshot_t>& retained) {
                return retained && retained->identity.source_id == identity.source_id;
            });
        if (found == publication->requests.end()) {
            unavailable_reason = "The Repeater request is no longer retained; select a current request.";
            return false;
        }
        if ((*found)->identity.revision != identity.revision ||
            (*found)->identity.content_size != identity.content_size ||
            (*found)->identity.content_hash != identity.content_hash ||
            (*found)->identity.id != identity.id) {
            unavailable_reason = "The Repeater request generation changed; reopen actions on the current request.";
            return false;
        }
        snapshot = **found;
        break;
    }
    case artifact_kind_t::repeater_response: {
        const auto found = std::find_if(g_state.repeater_entries.begin(), g_state.repeater_entries.end(),
            [&](const std::shared_ptr<repeater_entry_t>& entry) {
                return entry && entry->id == identity.source_id;
            });
        if (found == g_state.repeater_entries.end()) {
            unavailable_reason = "The Repeater tab was closed; reopen or select another request.";
            return false;
        }
        if ((*found)->request_revision != identity.revision ||
            (*found)->response_timestamp != identity.timestamp) {
            unavailable_reason = "The Repeater request or response generation changed; reopen actions on the current artifact.";
            return false;
        }
        snapshot.identity.target_host = (*found)->host;
        snapshot.identity.target_port = (*found)->port;
        snapshot.identity.use_tls = (*found)->use_tls;
        snapshot.bytes.assign((*found)->raw_response.begin(), (*found)->raw_response.end());
        break;
    }
    case artifact_kind_t::sitemap_request:
    case artifact_kind_t::sitemap_response: {
        aida::burp::exchange_observed_t exchange;
        if (!aida::burp::sitemap::find_exchange(identity.source_id, exchange)) {
            unavailable_reason = "The Site Map exchange is no longer retained.";
            return false;
        }
        if (exchange.timestamp_ms != identity.timestamp) {
            unavailable_reason = "The Site Map exchange generation changed; select the current exchange.";
            return false;
        }
        snapshot.identity.target_host = exchange.host;
        snapshot.identity.target_port = exchange.port;
        snapshot.identity.use_tls = exchange.scheme == "https";
        snapshot.bytes = identity.kind == artifact_kind_t::sitemap_response
            ? sitemap_response_bytes(exchange) : sitemap_request_bytes(exchange);
        break;
    }
    case artifact_kind_t::api_request:
    case artifact_kind_t::api_response: {
        if (!aida::qt::net::QtApiController::resolveRetainedArtifact(identity.source_id, identity.timestamp,
                identity.kind == artifact_kind_t::api_response, snapshot.bytes, unavailable_reason))
            return false;
        if (!aida::qt::net::QtApiController::resolveRetainedEndpoint(identity.source_id,
                identity.timestamp, snapshot.identity.target_host,
                snapshot.identity.target_port, snapshot.identity.use_tls,
                unavailable_reason)) return false;
        break;
    }
    case artifact_kind_t::websocket_editor_frame:
        if (!aida::qt::net::wsResolveRetainedFrame(identity.source_id, identity.revision,
                snapshot.bytes, unavailable_reason)) return false;
        break;
    case artifact_kind_t::http2_request:
    case artifact_kind_t::http2_response:
        if (!aida::qt::net::QtH2EditorController::resolveRetainedArtifact(identity.source_id, identity.timestamp,
                identity.kind == artifact_kind_t::http2_response, snapshot.bytes, unavailable_reason))
            return false;
        break;
    case artifact_kind_t::intruder_response:
        if (!aida::burp::intruder_view::resolve_retained_artifact(identity.source_id,
                identity.revision, identity.timestamp, snapshot.bytes, unavailable_reason))
            return false;
        break;
    case artifact_kind_t::scanner_request:
    case artifact_kind_t::scanner_response:
        if (!aida::qt::net::QtScannerController::resolveRetainedArtifact(identity.source_id,
                identity.timestamp, identity.revision,
                identity.kind == artifact_kind_t::scanner_response,
                snapshot.bytes, unavailable_reason))
            return false;
        if (!aida::qt::net::QtScannerController::resolveRetainedEndpoint(identity.source_id,
                identity.timestamp, snapshot.identity.target_host,
                snapshot.identity.target_port, snapshot.identity.use_tls,
                unavailable_reason)) return false;
        break;
    }
    if (snapshot.bytes.size() != identity.content_size || artifact_hash(snapshot.bytes) != identity.content_hash) {
        unavailable_reason = "The network artifact changed after the action menu opened; review the current bytes and try again.";
        snapshot = artifact_snapshot_t{};
        return false;
    }
    unavailable_reason.clear();
    return true;
}

bool send_artifact_to_repeater(const artifact_identity_t& identity, std::string& unavailable_reason) {
    if (g_state.repeater_entries.size() >= k_max_repeater_entries) {
        unavailable_reason = "Repeater retains at most 128 reviewed tabs; close a tab before opening another.";
        return false;
    }
    if (identity.kind == artifact_kind_t::response ||
        identity.kind == artifact_kind_t::repeater_response ||
        identity.kind == artifact_kind_t::sitemap_response ||
        identity.kind == artifact_kind_t::api_response ||
        identity.kind == artifact_kind_t::http2_response ||
        identity.kind == artifact_kind_t::intruder_response ||
        identity.kind == artifact_kind_t::scanner_response) {
        unavailable_reason = "Responses cannot be replayed as requests; choose the corresponding request artifact.";
        return false;
    }
    if (identity.kind == artifact_kind_t::http2_request) {
        unavailable_reason = "HTTP/2 requests must remain in the HTTP/2 editor so frame and pseudo-header semantics are preserved.";
        return false;
    }
    if (identity.kind != artifact_kind_t::request &&
        identity.kind != artifact_kind_t::intercept_request &&
        identity.kind != artifact_kind_t::exchange &&
        identity.kind != artifact_kind_t::repeater_request &&
        identity.kind != artifact_kind_t::sitemap_request &&
        identity.kind != artifact_kind_t::api_request &&
        identity.kind != artifact_kind_t::scanner_request) {
        unavailable_reason = "Repeater accepts retained HTTP/1 request artifacts only.";
        return false;
    }
    artifact_snapshot_t snapshot;
    if (!resolve_artifact(identity, snapshot, unavailable_reason)) return false;
    if (snapshot.bytes.size() > 65535U) {
        unavailable_reason = "Repeater accepts reviewed requests of at most 65535 bytes.";
        return false;
    }
    if (!intercept_editor_compatible(snapshot.bytes, unavailable_reason))
        return false;
    auto entry = std::make_shared<repeater_entry_t>();
    entry->id = s_repeater_artifact_sequence.fetch_add(1, std::memory_order_relaxed);
    entry->source_artifact_id = identity.id;
    entry->source_session_id = identity.session_id;
    entry->host = identity.target_host;
    entry->port = identity.target_port == 0 ? 443 : identity.target_port;
    entry->use_tls = identity.use_tls;
    entry->raw_request.assign(snapshot.bytes.begin(), snapshot.bytes.end());
    entry->request_hash = artifact_hash(snapshot.bytes);
    g_state.repeater_entries.push_back(std::move(entry));
    publish_repeater_request_artifacts(g_state);
    g_state.repeater_selected = static_cast<int>(g_state.repeater_entries.size()) - 1;
    unavailable_reason.clear();
    (void)open_view("view.network.repeater");
    return true;
}

namespace {

static constexpr char k_line_separator[] = "\r\n";

bool http_token_character(std::uint8_t value) {
    if ((value >= '0' && value <= '9') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z')) return true;
    switch (value) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

bool parse_decimal_size(std::string_view value, std::size_t& result) {
    if (value.empty() || value.size() > 20U) return false;
    result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, 10);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

bool validate_chunked_body(const std::vector<std::uint8_t>& request,
                           std::size_t offset, std::string& reason) {
    while (offset < request.size()) {
        const auto line_end = std::search(request.begin() + static_cast<std::ptrdiff_t>(offset),
            request.end(), std::begin(k_line_separator), std::end(k_line_separator) - 1);
        if (line_end == request.end()) {
            reason = "The chunked request body has an unterminated chunk-size line.";
            return false;
        }
        const std::size_t line_end_offset = static_cast<std::size_t>(line_end - request.begin());
        std::string_view size_line(reinterpret_cast<const char*>(request.data() + offset),
            line_end_offset - offset);
        const auto extension = size_line.find(';');
        if (extension != std::string_view::npos) {
            const auto extension_text = size_line.substr(extension + 1U);
            if (extension_text.empty() ||
                !std::all_of(extension_text.begin(), extension_text.end(), [](unsigned char value) {
                    return value >= 0x20U && value != 0x7fU;
                })) {
                reason = "The chunked request contains an invalid chunk extension.";
                return false;
            }
            size_line = size_line.substr(0, extension);
        }
        if (size_line.empty() || size_line.size() > 16U) {
            reason = "The chunked request body has an invalid chunk size.";
            return false;
        }
        std::uint64_t chunk_size = 0;
        const auto parsed = std::from_chars(size_line.data(),
            size_line.data() + size_line.size(), chunk_size, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != size_line.data() + size_line.size() ||
            chunk_size > 65535U) {
            reason = "The chunked request body has an invalid or oversized chunk.";
            return false;
        }
        offset = line_end_offset + 2U;
        if (chunk_size == 0) {
            if (offset + 2U == request.size() && request[offset] == '\r' &&
                request[offset + 1U] == '\n') return true;
            while (offset < request.size()) {
                const auto trailer_end = std::search(
                    request.begin() + static_cast<std::ptrdiff_t>(offset), request.end(),
                    std::begin(k_line_separator), std::end(k_line_separator) - 1);
                if (trailer_end == request.end()) break;
                const std::size_t trailer_end_offset =
                    static_cast<std::size_t>(trailer_end - request.begin());
                if (trailer_end_offset == offset)
                    return trailer_end_offset + 2U == request.size();
                const auto colon = std::find(request.begin() + static_cast<std::ptrdiff_t>(offset),
                    trailer_end, static_cast<std::uint8_t>(':'));
                if (colon == trailer_end) break;
                for (auto it = request.begin() + static_cast<std::ptrdiff_t>(offset);
                     it != colon; ++it) {
                    if (!http_token_character(*it)) {
                        reason = "The chunked request contains an invalid trailer name.";
                        return false;
                    }
                }
                if (!std::all_of(colon + 1, trailer_end, [](std::uint8_t value) {
                        return value == '\t' || (value >= 0x20U && value != 0x7fU);
                    })) {
                    reason = "The chunked request contains an invalid trailer value.";
                    return false;
                }
                offset = trailer_end_offset + 2U;
            }
            reason = "The chunked request has invalid or trailing data after its final chunk.";
            return false;
        }
        if (chunk_size > request.size() - offset ||
            offset + static_cast<std::size_t>(chunk_size) + 2U > request.size()) {
            reason = "The chunked request body is shorter than its declared chunk size.";
            return false;
        }
        offset += static_cast<std::size_t>(chunk_size);
        if (request[offset] != '\r' || request[offset + 1U] != '\n') {
            reason = "The chunked request body has an invalid chunk terminator.";
            return false;
        }
        offset += 2U;
    }
    reason = "The chunked request body has no final zero-sized chunk.";
    return false;
}

bool validate_http1_request(const std::vector<std::uint8_t>& request,
                            std::string& reason) {
    if (request.empty() || request.size() > 65535U) {
        reason = "A reviewed HTTP/1 request must contain from 1 to 65,535 bytes.";
        return false;
    }
    static constexpr char separator_bytes[] = "\r\n\r\n";
    const auto separator = std::search(request.begin(), request.end(),
        std::begin(separator_bytes), std::end(separator_bytes) - 1);
    if (separator == request.end()) {
        reason = "The reviewed HTTP/1 request has no exact CRLF header/body separator.";
        return false;
    }
    const std::size_t header_end = static_cast<std::size_t>(separator - request.begin());
    for (std::size_t index = 0; index < header_end + 2U; ++index) {
        const std::uint8_t value = request[index];
        if (value == '\n' && (index == 0 || request[index - 1U] != '\r')) {
            reason = "The reviewed HTTP/1 headers contain a bare line-feed.";
            return false;
        }
        if (value == '\r' && (index + 1U >= request.size() || request[index + 1U] != '\n')) {
            reason = "The reviewed HTTP/1 headers contain a bare carriage-return.";
            return false;
        }
        if (value == 0 || (value < 0x20U && value != '\r' && value != '\n' && value != '\t') ||
            value == 0x7fU) {
            reason = "The reviewed HTTP/1 headers contain a prohibited control character.";
            return false;
        }
    }
    const auto first_line_end = std::search(request.begin(), separator,
        std::begin(k_line_separator), std::end(k_line_separator) - 1);
    const auto first_space = std::find(request.begin(), first_line_end,
        static_cast<std::uint8_t>(' '));
    const auto second_space = first_space == first_line_end ? first_line_end :
        std::find(first_space + 1, first_line_end, static_cast<std::uint8_t>(' '));
    if (first_space == request.begin() || first_space == first_line_end ||
        second_space == first_space + 1 || second_space == first_line_end ||
        std::find(second_space + 1, first_line_end, static_cast<std::uint8_t>(' ')) != first_line_end) {
        reason = "The reviewed request line must be METHOD SP target SP HTTP/1.x.";
        return false;
    }
    if (static_cast<std::size_t>(first_space - request.begin()) > 32U ||
        !std::all_of(request.begin(), first_space, http_token_character)) {
        reason = "The reviewed request method is not a bounded HTTP token.";
        return false;
    }
    if (static_cast<std::size_t>(second_space - first_space - 1) > 8192U ||
        !std::all_of(first_space + 1, second_space, [](std::uint8_t value) {
            return value > 0x20U && value != 0x7fU;
        })) {
        reason = "The reviewed request target is empty, oversized, or contains controls.";
        return false;
    }
    const std::string_view version(reinterpret_cast<const char*>(&*(second_space + 1)),
        static_cast<std::size_t>(first_line_end - second_space - 1));
    if (version != "HTTP/1.0" && version != "HTTP/1.1") {
        reason = "The reviewed request must use HTTP/1.0 or HTTP/1.1.";
        return false;
    }

    bool host_present = false;
    bool content_length_present = false;
    std::size_t content_length = 0;
    bool transfer_encoding_present = false;
    bool transfer_chunked = false;
    std::size_t line_begin = static_cast<std::size_t>(first_line_end - request.begin()) + 2U;
    while (line_begin < header_end) {
        const auto line_end = std::search(
            request.begin() + static_cast<std::ptrdiff_t>(line_begin), request.end(),
            std::begin(k_line_separator), std::end(k_line_separator) - 1);
        if (line_end == request.end() || line_end > separator) {
            reason = "The reviewed HTTP/1 header block is malformed.";
            return false;
        }
        const std::size_t line_end_offset = static_cast<std::size_t>(line_end - request.begin());
        if (line_end_offset - line_begin > 8192U || request[line_begin] == ' ' ||
            request[line_begin] == '\t') {
            reason = "Folded, empty, or oversized HTTP headers are not accepted.";
            return false;
        }
        const auto colon = std::find(request.begin() + static_cast<std::ptrdiff_t>(line_begin),
            line_end, static_cast<std::uint8_t>(':'));
        if (colon == line_end || colon == request.begin() + static_cast<std::ptrdiff_t>(line_begin) ||
            static_cast<std::size_t>(colon -
                (request.begin() + static_cast<std::ptrdiff_t>(line_begin))) > 256U ||
            !std::all_of(request.begin() + static_cast<std::ptrdiff_t>(line_begin), colon,
                http_token_character)) {
            reason = "The reviewed request contains an invalid HTTP header name.";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(request.data() + line_begin),
            static_cast<std::size_t>(colon - (request.begin() + static_cast<std::ptrdiff_t>(line_begin))));
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        std::size_t value_begin = static_cast<std::size_t>(colon - request.begin()) + 1U;
        while (value_begin < line_end_offset &&
            (request[value_begin] == ' ' || request[value_begin] == '\t')) ++value_begin;
        std::size_t value_end = line_end_offset;
        while (value_end > value_begin &&
            (request[value_end - 1U] == ' ' || request[value_end - 1U] == '\t')) --value_end;
        const std::string_view value(reinterpret_cast<const char*>(request.data() + value_begin),
            value_end - value_begin);
        if (name == "host") {
            if (host_present || value.empty()) {
                reason = "The reviewed request must contain exactly one non-empty Host header.";
                return false;
            }
            host_present = true;
        }
        if (name == "content-length") {
            std::size_t parsed_length = 0;
            if (content_length_present || !parse_decimal_size(value, parsed_length)) {
                reason = "Content-Length must be one unambiguous bounded decimal value.";
                return false;
            }
            content_length_present = true;
            content_length = parsed_length;
        }
        if (name == "transfer-encoding") {
            std::string lower(value);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char item) {
                return static_cast<char>(std::tolower(item));
            });
            if (transfer_encoding_present || lower != "chunked") {
                reason = "Reviewed requests support only one exact Transfer-Encoding: chunked header.";
                return false;
            }
            transfer_encoding_present = true;
            transfer_chunked = true;
        }
        line_begin = line_end_offset + 2U;
    }
    if (version == "HTTP/1.1" && !host_present) {
        reason = "HTTP/1.1 reviewed requests require a non-empty Host header.";
        return false;
    }
    if (content_length_present && transfer_encoding_present) {
        reason = "A reviewed request cannot combine Content-Length and Transfer-Encoding.";
        return false;
    }
    const std::size_t body_begin = header_end + 4U;
    const std::size_t body_size = request.size() - body_begin;
    if (content_length_present && content_length != body_size) {
        reason = "The request body length does not match Content-Length.";
        return false;
    }
    if (transfer_chunked && !validate_chunked_body(request, body_begin, reason)) return false;
    if (!content_length_present && !transfer_encoding_present && body_size != 0U) {
        reason = "A reviewed request body requires explicit Content-Length or chunked framing.";
        return false;
    }
    reason.clear();
    return true;
}

}

bool validate_reviewed_request(const artifact_identity_t& source,
                               const std::vector<std::uint8_t>& reviewed_request,
                               artifact_identity_t& canonical_source,
                               std::string& unavailable_reason) {
    canonical_source = {};
    artifact_snapshot_t current;
    if (!resolve_artifact(source, current, unavailable_reason)) return false;
    if (current.identity.target_host.empty() || current.identity.target_port == 0 ||
        source.target_host != current.identity.target_host ||
        source.target_port != current.identity.target_port ||
        source.use_tls != current.identity.use_tls) {
        unavailable_reason = "The proposal endpoint does not exactly match the canonical retained artifact endpoint.";
        return false;
    }
    if (!validate_http1_request(reviewed_request, unavailable_reason)) return false;
    canonical_source = current.identity;
    unavailable_reason.clear();
    return true;
}

bool stage_validated_reviewed_request(const artifact_identity_t& source,
                                      const std::vector<std::uint8_t>& reviewed_request,
                                      const std::string& provenance,
                                      artifact_identity_t& staged_identity,
                                      std::string& unavailable_reason) {
    staged_identity = {};
    if (g_state.repeater_entries.size() >= k_max_repeater_entries) {
        unavailable_reason = "Repeater retains at most 128 reviewed tabs; close a tab before staging another.";
        return false;
    }
    if (reviewed_request.empty() || reviewed_request.size() > 65535U) {
        unavailable_reason = "The prevalidated request payload is outside its bounded staging range.";
        return false;
    }
    if (!source.valid() || source.target_host.empty() || source.target_port == 0) {
        unavailable_reason = "The validated request has no canonical retained endpoint.";
        return false;
    }
    if (source.kind != artifact_kind_t::request &&
        source.kind != artifact_kind_t::intercept_request &&
        source.kind != artifact_kind_t::exchange &&
        source.kind != artifact_kind_t::repeater_request &&
        source.kind != artifact_kind_t::sitemap_request &&
        source.kind != artifact_kind_t::api_request &&
        source.kind != artifact_kind_t::scanner_request) {
        unavailable_reason = "Only canonical retained HTTP/1 request artifacts can be staged.";
        return false;
    }
    if (provenance.empty() || provenance.size() > 512U) {
        unavailable_reason = "The reviewed request has no bounded AI proposal provenance.";
        return false;
    }
    artifact_identity_t canonical_source;
    if (!validate_reviewed_request(source, reviewed_request,
            canonical_source, unavailable_reason))
        return false;
    auto entry = std::make_shared<repeater_entry_t>();
    entry->id = s_repeater_artifact_sequence.fetch_add(1, std::memory_order_relaxed);
    entry->source_artifact_id = canonical_source.id;
    entry->source_session_id = canonical_source.session_id;
    entry->host = canonical_source.target_host;
    entry->port = canonical_source.target_port;
    entry->use_tls = canonical_source.use_tls;
    entry->raw_request.assign(reviewed_request.begin(), reviewed_request.end());
    entry->request_hash = artifact_hash(reviewed_request);
    entry->reviewed_source_hash = canonical_source.content_hash;
    entry->review_provenance = provenance;
    entry->reviewed_draft = true;
    staged_identity = repeater_artifact_identity(*entry, artifact_kind_t::repeater_request);
    g_state.repeater_entries.push_back(std::move(entry));
    publish_repeater_request_artifacts(g_state);
    g_state.repeater_selected = static_cast<int>(g_state.repeater_entries.size()) - 1;
    const std::string open_error = open_view("view.network.repeater");
    if (!open_error.empty()) {
        g_state.repeater_entries.pop_back();
        publish_repeater_request_artifacts(g_state);
        g_state.repeater_selected = static_cast<int>(g_state.repeater_entries.size()) - 1;
        staged_identity = {};
        unavailable_reason = open_error;
        return false;
    }
    unavailable_reason.clear();
    return true;
}

bool send_artifact_to_comparer(const artifact_identity_t& identity, std::string& unavailable_reason) {
    artifact_snapshot_t snapshot;
    if (!resolve_artifact(identity, snapshot, unavailable_reason)) return false;
    const std::uint64_t slot = aida::burp::comparer::add_slot_from_bytes(
        identity.label.empty() ? identity.id : identity.label, snapshot.bytes, identity.id);
    if (slot == 0) {
        unavailable_reason = "Comparer rejected the artifact: " + aida::burp::comparer::last_error();
        return false;
    }
    unavailable_reason.clear();
    (void)open_view("view.network.comparer");
    return true;
}

static bool handoff_artifact(const artifact_identity_t& identity, bool agent,
                             std::string& unavailable_reason) {
    artifact_snapshot_t snapshot;
    if (!resolve_artifact(identity, snapshot, unavailable_reason)) return false;
    constexpr std::size_t max_handoff_bytes = 64U * 1024U;
    const std::size_t count = (std::min)(snapshot.bytes.size(), max_handoff_bytes);
    std::string content(reinterpret_cast<const char*>(snapshot.bytes.data()), count);
    aida::automation_ui::evidence_envelope_t envelope;
    envelope.id = "evidence." + identity.id + "." + std::to_string(identity.content_hash);
    envelope.session_id = identity.session_id;
    envelope.source_view_id = identity.source_view_id;
    envelope.source_kind = "network";
    envelope.entity_id = identity.id;
    envelope.display_label = identity.label.empty() ? identity.id : identity.label;
    envelope.return_target = identity.id;
    envelope.excerpt = std::move(content);
    envelope.revision = identity.revision;
    envelope.generation = identity.timestamp;
    envelope.snapshot_hash = identity.content_hash;
    envelope.content_hash = identity.content_hash;
    envelope.truncated = snapshot.bytes.size() > count;
    const std::string evidence_id = aida::automation_ui::register_evidence(std::move(envelope));
    if (evidence_id.empty()) {
        unavailable_reason = "The evidence envelope was rejected because its source identity was incomplete.";
        return false;
    }
    return agent
        ? aida::automation_ui::queue_evidence_for_agent(evidence_id, unavailable_reason)
        : aida::automation_ui::queue_evidence_for_chat(evidence_id, unavailable_reason);
}

bool add_artifact_to_chat(const artifact_identity_t& identity, std::string& unavailable_reason) {
    return handoff_artifact(identity, false, unavailable_reason);
}

bool assign_artifact_to_agent(const artifact_identity_t& identity, std::string& unavailable_reason) {
    return handoff_artifact(identity, true, unavailable_reason);
}

namespace {

const network_exchange_action_descriptor_t k_exchange_actions[] = {
    {network_exchange_action_t::repeater, "network.exchange.repeater"},
    {network_exchange_action_t::fuzzer, "network.exchange.fuzzer"},
    {network_exchange_action_t::intruder, "network.exchange.intruder"},
    {network_exchange_action_t::scanner, "network.exchange.scanner"},
    {network_exchange_action_t::comparer, "network.exchange.comparer"},
    {network_exchange_action_t::compare_request_response, "network.exchange.compare_request_response"},
    {network_exchange_action_t::session_handling, "network.exchange.session_handling"},
    {network_exchange_action_t::cookies, "network.exchange.cookies"},
    {network_exchange_action_t::match_replace, "network.exchange.match_replace"},
    {network_exchange_action_t::decoder, "network.exchange.decoder"},
    {network_exchange_action_t::sequencer, "network.exchange.sequencer"},
    {network_exchange_action_t::camoufox, "network.exchange.camoufox"},
    {network_exchange_action_t::copy_url, "network.exchange.copy_url"},
    {network_exchange_action_t::copy_method, "network.exchange.copy_method"},
    {network_exchange_action_t::copy_status, "network.exchange.copy_status"},
    {network_exchange_action_t::copy_request, "network.exchange.copy_request"},
    {network_exchange_action_t::copy_response, "network.exchange.copy_response"},
    {network_exchange_action_t::copy_headers, "network.exchange.copy_headers"},
    {network_exchange_action_t::copy_body, "network.exchange.copy_body"},
    {network_exchange_action_t::copy_artifact, "network.exchange.copy_artifact"},
    {network_exchange_action_t::scope_include, "network.exchange.scope_include"},
    {network_exchange_action_t::scope_exclude, "network.exchange.scope_exclude"},
    {network_exchange_action_t::save_export, "network.exchange.save_export"},
    {network_exchange_action_t::create_issue, "network.exchange.create_issue"},
    {network_exchange_action_t::chat, "network.exchange.chat"},
    {network_exchange_action_t::agent, "network.exchange.agent"},
    {network_exchange_action_t::replay_live, "network.exchange.replay"},
    {network_exchange_action_t::remove, "network.exchange.remove"}
};

bool response_kind(artifact_kind_t kind) {
    return kind == artifact_kind_t::response ||
        kind == artifact_kind_t::repeater_response ||
        kind == artifact_kind_t::sitemap_response ||
        kind == artifact_kind_t::api_response ||
        kind == artifact_kind_t::http2_response ||
        kind == artifact_kind_t::intruder_response ||
        kind == artifact_kind_t::scanner_response;
}

bool request_kind(artifact_kind_t kind) {
    return kind == artifact_kind_t::request ||
        kind == artifact_kind_t::intercept_request ||
        kind == artifact_kind_t::exchange ||
        kind == artifact_kind_t::repeater_request ||
        kind == artifact_kind_t::sitemap_request ||
        kind == artifact_kind_t::api_request ||
        kind == artifact_kind_t::http2_request ||
        kind == artifact_kind_t::scanner_request;
}

std::string artifact_text(const artifact_snapshot_t& snapshot) {
    return std::string(reinterpret_cast<const char*>(snapshot.bytes.data()), snapshot.bytes.size());
}

std::string clipboard_text(const std::string& value) {
    const bool textual = std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c == '\r' || c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7F);
    });
    if (textual) return value;
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded = "hex:";
    encoded.reserve(4U + value.size() * 2U);
    for (const char byte : value) {
        const auto c = static_cast<unsigned char>(byte);
        encoded.push_back(hex[c >> 4U]);
        encoded.push_back(hex[c & 0x0FU]);
    }
    return encoded;
}

std::string request_method(const std::string& raw) {
    const auto end = raw.find(' ');
    return end == std::string::npos ? std::string() : raw.substr(0, end);
}

std::string request_target(const std::string& raw) {
    const auto first = raw.find(' ');
    if (first == std::string::npos) return {};
    const auto second = raw.find(' ', first + 1U);
    return second == std::string::npos ? std::string() : raw.substr(first + 1U, second - first - 1U);
}

std::string artifact_url(const artifact_identity_t& identity, const std::string& request) {
    if (identity.target_host.empty()) return {};
    const std::string target = request_target(request);
    if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0) return target;
    std::string url = identity.use_tls ? "https://" : "http://";
    url.append(identity.target_host);
    const std::uint16_t default_port = identity.use_tls ? 443 : 80;
    if (identity.target_port != 0 && identity.target_port != default_port)
        url.append(":").append(std::to_string(identity.target_port));
    url.append(target.empty() ? "/" : target);
    return url;
}

std::pair<std::string, std::string> http_headers_body(const std::string& raw) {
    auto split = raw.find("\r\n\r\n");
    std::size_t delimiter = 4;
    if (split == std::string::npos) {
        split = raw.find("\n\n");
        delimiter = 2;
    }
    if (split == std::string::npos) return {raw, {}};
    const auto first_line = raw.find('\n');
    const std::size_t header_begin = first_line == std::string::npos ? 0 : first_line + 1U;
    return {raw.substr(header_begin, split - header_begin), raw.substr(split + delimiter)};
}

std::string curl_quote(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    for (const char c : value) {
        if (c == '"' || c == '\\') result.push_back('\\');
        result.push_back(c);
    }
    result.push_back('"');
    return result;
}

std::string curl_command(const artifact_identity_t& identity, const std::string& request) {
    const std::string method = request_method(request);
    const std::string url = artifact_url(identity, request);
    if (method.empty() || url.empty()) return {};
    std::string result = "curl -i -k -X " + method + " " + curl_quote(url);
    const auto parts = http_headers_body(request);
    std::size_t offset = 0;
    while (offset < parts.first.size()) {
        const auto end = parts.first.find('\n', offset);
        std::string line = parts.first.substr(offset,
            end == std::string::npos ? std::string::npos : end - offset);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (name != "host" && name != "content-length")
                result.append(" -H ").append(curl_quote(line));
        }
        if (end == std::string::npos) break;
        offset = end + 1U;
    }
    if (!parts.second.empty())
        result.append(" --data-binary ").append(curl_quote(parts.second));
    return result;
}

std::string response_status(const std::string& raw) {
    const auto first = raw.find(' ');
    if (first == std::string::npos) return {};
    const auto end = raw.find_first_of("\r\n", first + 1U);
    return raw.substr(first + 1U, end == std::string::npos ? std::string::npos : end - first - 1U);
}

bool snapshot_for(const artifact_identity_t& identity, artifact_snapshot_t& snapshot,
                  std::string& reason) {
    return identity.valid() && resolve_artifact(identity, snapshot, reason);
}

const artifact_identity_t* request_identity(const exchange_context_runtime_t& context) {
    if (request_kind(context.primary.kind)) return &context.primary;
    return request_kind(context.related.kind) ? &context.related : nullptr;
}

const artifact_identity_t* response_identity(const exchange_context_runtime_t& context) {
    if (response_kind(context.primary.kind) && context.primary.valid() &&
        context.primary.content_size != 0) return &context.primary;
    return response_kind(context.related.kind) && context.related.valid() &&
        context.related.content_size != 0 ? &context.related : nullptr;
}

bool matching_http1_pair(const artifact_identity_t& request,
                         const artifact_identity_t& response) {
    return request.valid() && response.valid() && request_kind(request.kind) &&
        response_kind(response.kind) && request.kind != artifact_kind_t::http2_request &&
        response.kind != artifact_kind_t::http2_response && !request.raw_protocol &&
        !response.raw_protocol && !request.parent_id.empty() &&
        request.parent_id == response.parent_id && request.source_view_id == response.source_view_id &&
        request.source_id == response.source_id && request.session_id == response.session_id &&
        request.target_host == response.target_host && request.target_port == response.target_port &&
        request.use_tls == response.use_tls;
}

std::string cookie_request_path(const std::string& raw_request) {
    std::string target = request_target(raw_request);
    const std::size_t scheme = target.find("://");
    if (scheme != std::string::npos) {
        const std::size_t slash = target.find('/', scheme + 3U);
        target = slash == std::string::npos ? "/" : target.substr(slash);
    }
    const std::size_t fragment = target.find('#');
    if (fragment != std::string::npos) target.resize(fragment);
    return target.empty() ? "/" : target;
}

struct exchange_review_state_t {
    exchange_review_kind_t kind = exchange_review_kind_t::none;
    artifact_identity_t primary;
    artifact_identity_t related;
    bool open_requested = false;
    char issue_name[160]{};
    char issue_description[2048]{};
    char issue_remediation[2048]{};
    int issue_severity = static_cast<int>(aida::burp::severity_t::info);
    int issue_confidence = static_cast<int>(aida::burp::confidence_t::firm);
    std::string validation_error;
};

struct exchange_remove_undo_state_t {
    exchange_remove_source_t source = exchange_remove_source_t::none;
    mitm_proxy::http_exchange proxy_exchange;
    std::shared_ptr<repeater_entry_t> repeater_entry;
    std::size_t original_index = 0;
    bool open_requested = false;
    bool operation_pending = false;
    bool restored = false;
    std::string error;
};

static exchange_review_state_t s_exchange_review;
static exchange_remove_undo_state_t s_exchange_remove_undo;
static std::atomic<bool> s_common_exchange_operation_pending{false};

static void present_exchange_review();
static void present_remove_receipt(const std::string& label);
static std::atomic<int> s_remove_undo_completion_status{0};
static std::array<char, 512> s_remove_undo_completion_error{};
static constexpr std::size_t k_issue_evidence_limit = 1024U * 1024U;

static bool proxy_artifact_kind(artifact_kind_t kind) {
    return kind == artifact_kind_t::exchange || kind == artifact_kind_t::request ||
        kind == artifact_kind_t::response;
}

static bool repeater_artifact_kind(artifact_kind_t kind) {
    return kind == artifact_kind_t::repeater_request ||
        kind == artifact_kind_t::repeater_response;
}

static bool proxy_exchange_matches_identity(
    const mitm_proxy::http_exchange& exchange, const artifact_identity_t& identity) {
    if (exchange.id != identity.source_id || exchange.timestamp != identity.timestamp ||
        exchange.target_host != identity.target_host || exchange.target_port != identity.target_port ||
        exchange.is_tls != identity.use_tls)
        return false;
    const auto& bytes = identity.kind == artifact_kind_t::response
        ? exchange.raw_response : exchange.raw_request;
    return bytes.size() == identity.content_size && artifact_hash(bytes) == identity.content_hash;
}

static std::string bounded_export_filename(const artifact_identity_t& identity) {
    std::string name = identity.label.empty() ? "network-artifact" : identity.label;
    for (char& character : name) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (!(std::isalnum(value) || character == '-' || character == '_'))
            character = '_';
    }
    if (name.size() > 96U)
        name.resize(96U);
    if (name.empty())
        name = "network-artifact";
    return name + ".bin";
}

static bool export_reviewed_artifact(const artifact_identity_t& identity,
                                     std::string& reason) {
    const std::string filename = bounded_export_filename(identity);
    static const char k_artifact_filter[] =
        "Network artifact (*.bin)\0*.bin\0"
        "HTTP message (*.http)\0*.http\0"
        "All files (*.*)\0*.*\0\0";
    if (!s_save_file_dialog) {
        reason = "The save-file dialog host is unavailable.";
        return false;
    }
    const std::string destination = s_save_file_dialog(
        "Export reviewed network artifact", k_artifact_filter, "bin", filename);
    if (destination.empty()) {
        reason = "Artifact export was cancelled.";
        return false;
    }
    artifact_snapshot_t snapshot;
    if (!snapshot_for(identity, snapshot, reason))
        return false;
    if (snapshot.bytes.size() > k_network_export_limit) {
        reason = "The reviewed artifact exceeds the 256 MiB export safety limit.";
        return false;
    }
    const std::string task_id = register_network_operation(
        "network.exchange.save_export", "Export reviewed network artifact",
        identity.source_view_id.c_str(), destination);
    const bool posted = post_network_task(
        "network_artifact_export", aida::infra::executor::domain_t::diagnostics,
        "bounded_task",
        [bytes = std::move(snapshot.bytes), destination, task_id]() {
            bool success = false;
            std::string error;
            try {
                success = aida::qt::net::atomic_write_export(destination, bytes.data(), bytes.size(), error);
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Reviewed artifact export failed";
            }
            if (!success && error.empty())
                error = "Reviewed artifact export failed without a destination receipt";
            finish_network_operation(task_id, success,
                success ? "Completed" : "Failed",
                success ? std::to_string(bytes.size()) +
                    " bytes written atomically to " + destination : error);
            post_network_ui_completion([success, destination, error = std::move(error)] {
                toast_notification::push(success
                    ? "Exported reviewed artifact to " + destination
                    : (error.empty() ? "Artifact export failed" : error),
                    success ? toast_notification::toast_type_t::success
                            : toast_notification::toast_type_t::error);
            });
        }, false);
    if (!posted) {
        finish_network_operation(task_id, false, "Rejected",
            "Executor rejected reviewed artifact export");
        reason = "The Network executor rejected the artifact export.";
        return false;
    }
    reason.clear();
    return true;
}

static void stage_exchange_review(exchange_review_kind_t kind,
                                  const exchange_context_runtime_t& context) {
    s_exchange_review = {};
    s_exchange_review.kind = kind;
    s_exchange_review.primary = context.primary;
    s_exchange_review.related = context.related;
    s_exchange_review.open_requested = true;
    if (kind == exchange_review_kind_t::create_issue) {
        std::snprintf(s_exchange_review.issue_name,
            sizeof(s_exchange_review.issue_name), "Manual finding for %s",
            context.primary.label.empty() ? "network artifact"
                                          : context.primary.label.c_str());
        std::snprintf(s_exchange_review.issue_description,
            sizeof(s_exchange_review.issue_description),
            "Reviewed network evidence retained from %s.",
            context.primary.source_view_id.empty() ? "Network"
                                                   : context.primary.source_view_id.c_str());
    }
    present_exchange_review();
}

static bool submit_reviewed_issue(std::string& reason) {
    artifact_snapshot_t primary;
    if (!snapshot_for(s_exchange_review.primary, primary, reason))
        return false;
    artifact_snapshot_t related;
    if (s_exchange_review.related.valid() &&
        !snapshot_for(s_exchange_review.related, related, reason))
        return false;
    if (primary.bytes.size() > k_issue_evidence_limit ||
        related.bytes.size() > k_issue_evidence_limit) {
        reason = "Manual issue evidence is bounded to 1 MiB per retained artifact.";
        return false;
    }
    const artifact_identity_t primary_identity = s_exchange_review.primary;
    const artifact_identity_t related_identity = s_exchange_review.related;
    aida::burp::issue_t issue;
    issue.session_id = primary_identity.session_id;
    issue.type_key = "manual-network-artifact";
    issue.name = s_exchange_review.issue_name;
    issue.description = s_exchange_review.issue_description;
    issue.remediation = s_exchange_review.issue_remediation;
    issue.severity = static_cast<aida::burp::severity_t>(s_exchange_review.issue_severity);
    issue.confidence = static_cast<aida::burp::confidence_t>(s_exchange_review.issue_confidence);
    issue.scheme = primary_identity.use_tls ? "https" : "http";
    issue.host = primary_identity.target_host;
    issue.port = primary_identity.target_port;
    issue.src_exchange_id = primary_identity.source_id;
    issue.seen_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const artifact_identity_t* request = request_kind(primary_identity.kind)
        ? &primary_identity
        : request_kind(related_identity.kind) ? &related_identity : nullptr;
    if (request) {
        const artifact_snapshot_t& request_data = request->id == primary_identity.id
            ? primary : related;
        const std::string raw = artifact_text(request_data);
        issue.path = request_target(raw);
    }
    aida::burp::evidence_t evidence;
    if (request_kind(primary_identity.kind))
        evidence.request_raw = artifact_text(primary);
    else if (response_kind(primary_identity.kind))
        evidence.response_raw = artifact_text(primary);
    if (related_identity.valid()) {
        if (request_kind(related_identity.kind))
            evidence.request_raw = artifact_text(related);
        else if (response_kind(related_identity.kind))
            evidence.response_raw = artifact_text(related);
    }
    evidence.marker = primary_identity.id;
    issue.evidence.push_back(std::move(evidence));
    const std::string owner_view = primary_identity.source_view_id.empty()
        ? "view.network.scanner" : primary_identity.source_view_id;
    const std::string task_id = register_network_operation(
        "network.exchange.create_issue", "Create reviewed Network issue",
        owner_view.c_str(), issue.name);
    const bool posted = post_network_task(
        "network_issue_create", aida::infra::executor::domain_t::diagnostics,
        "bounded_task", [issue = std::move(issue), task_id]() mutable {
            bool retained = false;
            bool persisted = false;
            std::uint64_t issue_id = 0;
            std::string error;
            try {
                issue_id = aida::burp::issue_store::add(std::move(issue));
                retained = issue_id != 0;
                persisted = retained && aida::burp::issue_store::save_to_disk();
                if (!persisted)
                    error = aida::burp::issue_store::last_error();
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Manual Network issue creation failed";
            }
            if (persisted) {
                finish_network_operation(task_id, true, "Completed",
                    "Scanner issue #" + std::to_string(issue_id) +
                    " persisted with reviewed evidence");
            } else if (retained) {
                (void)aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::partial, 1.0f,
                    "Persistence failed",
                    "Scanner issue #" + std::to_string(issue_id) +
                    " remains in memory, but durable save failed: " +
                    (error.empty() ? "unknown storage error" : error));
            } else {
                finish_network_operation(task_id, false, "Failed",
                    error.empty() ? "Issue store rejected the finding" : error);
            }
            post_network_ui_completion([retained, persisted, issue_id,
                                   error = std::move(error)] {
                if (retained) {
                    (void)open_view("view.network.scanner");
                    toast_notification::push(
                        persisted
                            ? "Created Scanner issue #" + std::to_string(issue_id)
                            : "Issue #" + std::to_string(issue_id) +
                                " remains in memory; durable save failed",
                        persisted ? toast_notification::toast_type_t::success
                                  : toast_notification::toast_type_t::warning);
                } else {
                    toast_notification::push(error.empty()
                        ? "Manual Network issue creation failed" : error,
                        toast_notification::toast_type_t::error);
                }
            });
        }, false);
    if (!posted) {
        finish_network_operation(task_id, false, "Rejected",
            "Executor rejected manual issue creation");
        reason = "The Network executor rejected manual issue creation.";
        return false;
    }
    reason.clear();
    return true;
}

static bool submit_reviewed_replay(std::string& reason) {
    const artifact_identity_t request_identity_value =
        request_kind(s_exchange_review.primary.kind)
            ? s_exchange_review.primary : s_exchange_review.related;
    artifact_snapshot_t request;
    if (!snapshot_for(request_identity_value, request, reason))
        return false;
    if (request.bytes.empty() || request.bytes.size() > 65535U ||
        request_identity_value.target_host.empty() ||
        request_identity_value.target_port == 0) {
        reason = "Live replay requires a bounded reviewed HTTP/1 request and verified target.";
        return false;
    }
    bool expected = false;
    if (!s_common_exchange_operation_pending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        reason = "Another reviewed Network artifact operation is still active.";
        return false;
    }
    const std::string target = request_identity_value.target_host + ":" +
        std::to_string(request_identity_value.target_port);
    const std::string task_id = register_network_operation(
        "network.exchange.replay", "Replay reviewed Network request",
        request_identity_value.source_view_id.c_str(), target);
    const bool posted = post_network_task(
        "network_exchange_replay", aida::infra::executor::domain_t::external_tool,
        "bounded_task",
        [identity = request_identity_value, bytes = std::move(request.bytes), task_id]() {
            bool success = false;
            int status = 0;
            std::uint64_t response_id = 0;
            std::string error;
            try {
                auto result = mitm_proxy::repeat_request(
                    identity.target_host, identity.target_port, identity.use_tls, bytes);
                success = result.success;
                status = result.exchange.response.status_code;
                response_id = result.exchange.id;
                error = std::move(result.error);
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Reviewed request replay failed";
            }
            finish_network_operation(task_id, success,
                success ? "Completed" : "Failed",
                success ? "Recorded replay exchange #" + std::to_string(response_id) +
                    " with HTTP status " + std::to_string(status)
                    : (error.empty() ? "The target returned no response" : error));
            const bool completion_queued = post_network_ui_completion(
                [success, response_id, status, error = std::move(error)] {
                s_common_exchange_operation_pending.store(false, std::memory_order_release);
                request_proxy_runtime_snapshot(true);
                toast_notification::push(success
                    ? "Replay recorded as exchange #" + std::to_string(response_id) +
                        " (HTTP " + std::to_string(status) + ")"
                    : (error.empty() ? "Reviewed request replay failed" : error),
                    success ? toast_notification::toast_type_t::success
                            : toast_notification::toast_type_t::error);
            });
            if (!completion_queued)
                s_common_exchange_operation_pending.store(false, std::memory_order_release);
        }, false);
    if (!posted) {
        s_common_exchange_operation_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected",
            "Executor rejected reviewed request replay");
        reason = "The Network executor rejected reviewed request replay.";
        return false;
    }
    reason.clear();
    return true;
}

static bool submit_reviewed_removal(std::string& reason) {
    artifact_snapshot_t current;
    if (!snapshot_for(s_exchange_review.primary, current, reason))
        return false;
    const artifact_identity_t identity = s_exchange_review.primary;
    if (repeater_artifact_kind(identity.kind)) {
        const auto found = std::find_if(g_state.repeater_entries.begin(),
            g_state.repeater_entries.end(), [&](const auto& entry) {
                return entry && entry->id == identity.source_id;
            });
        if (found == g_state.repeater_entries.end()) {
            reason = "The reviewed Repeater tab is no longer retained.";
            return false;
        }
        if ((*found)->in_progress.load(std::memory_order_acquire)) {
            reason = "Wait for the active Repeater send to finish before removing its tab.";
            return false;
        }
        const std::size_t index = static_cast<std::size_t>(
            std::distance(g_state.repeater_entries.begin(), found));
        s_exchange_remove_undo = {};
        s_exchange_remove_undo.source = exchange_remove_source_t::repeater;
        s_exchange_remove_undo.repeater_entry = *found;
        s_exchange_remove_undo.original_index = index;
        s_exchange_remove_undo.open_requested = true;
        const std::string task_id = register_network_operation(
            "network.exchange.remove", "Remove reviewed Repeater tab",
            "view.network.repeater", identity.label);
        g_state.repeater_entries.erase(found);
        s_repeater_selected_artifact_kinds.erase(identity.source_id);
        clear_stale_network_selection("view.network.repeater");
        publish_repeater_request_artifacts(g_state);
        g_state.repeater_selected = g_state.repeater_entries.empty() ? -1
            : static_cast<int>((std::min)(index, g_state.repeater_entries.size() - 1U));
        finish_network_operation(task_id, true, "Completed",
            "Repeater tab removed; recovery is available from the receipt");
        present_remove_receipt(identity.label.empty()
            ? "Repeater tab #" + std::to_string(identity.source_id)
            : identity.label);
        reason.clear();
        return true;
    }
    if (!proxy_artifact_kind(identity.kind) ||
        identity.source_view_id != "view.network.proxy") {
        reason = "This artifact source does not expose a reversible remove operation.";
        return false;
    }
    if (s_proxy_operation_pending.load(std::memory_order_acquire)) {
        reason = "Wait for the active Proxy operation before removing reviewed history.";
        return false;
    }
    bool expected = false;
    if (!s_common_exchange_operation_pending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        reason = "Another reviewed Network artifact operation is still active.";
        return false;
    }
    const std::string task_id = register_network_operation(
        "network.exchange.remove", "Remove reviewed proxy exchange",
        "view.network.proxy", identity.label);
    const bool posted = post_network_task(
        "network_exchange_remove", aida::infra::executor::domain_t::diagnostics,
        "bounded_task", [identity, task_id]() {
            bool success = false;
            std::size_t removed_index = 0;
            std::uint64_t previous_id = 0;
            std::uint64_t next_id = 0;
            mitm_proxy::http_exchange removed;
            std::string error;
            try {
                std::lock_guard<std::mutex> lock(mitm_proxy::g_state.history_mutex);
                auto found = std::find_if(mitm_proxy::g_state.history.begin(),
                    mitm_proxy::g_state.history.end(), [&](const auto& exchange) {
                        return exchange && proxy_exchange_matches_identity(*exchange, identity);
                    });
                if (found == mitm_proxy::g_state.history.end()) {
                    error = "Proxy history changed after review; select the current exchange again";
                } else {
                    removed_index = static_cast<std::size_t>(
                        std::distance(mitm_proxy::g_state.history.begin(), found));
                    if (found != mitm_proxy::g_state.history.begin()) {
                        const auto& previous = *(found - 1);
                        previous_id = previous ? previous->id : 0;
                    }
                    if (found + 1 != mitm_proxy::g_state.history.end()) {
                        const auto& next = *(found + 1);
                        next_id = next ? next->id : 0;
                    }
                    removed = **found;
                    mitm_proxy::g_state.history.erase(found);
                    success = true;
                }
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Reviewed proxy exchange removal failed";
            }
            if (!success) {
                finish_network_operation(task_id, false, "Failed",
                    error.empty() ? "Reviewed proxy exchange removal failed" : error);
                s_common_exchange_operation_pending.store(false, std::memory_order_release);
                post_network_ui_completion([error] {
                    toast_notification::push(error.empty()
                        ? "Reviewed proxy exchange removal failed" : error,
                        toast_notification::toast_type_t::error);
                    request_proxy_runtime_snapshot(true);
                });
                return;
            }
            bool completion_queued = false;
            try {
                completion_queued = post_network_ui_completion(
                    [removed_index, removed]() mutable {
                s_common_exchange_operation_pending.store(false, std::memory_order_release);
                s_exchange_remove_undo = {};
                s_exchange_remove_undo.source = exchange_remove_source_t::proxy;
                const std::string receipt_label = "Proxy exchange #" +
                    std::to_string(removed.id);
                s_exchange_remove_undo.proxy_exchange = std::move(removed);
                s_exchange_remove_undo.original_index = removed_index;
                s_exchange_remove_undo.open_requested = true;
                clear_stale_network_selection("view.network.proxy");
                present_remove_receipt(receipt_label);
                request_proxy_runtime_snapshot(true);
            });
            } catch (...) {
                completion_queued = false;
            }
            if (completion_queued) {
                finish_network_operation(task_id, true, "Completed",
                    "Proxy exchange removed; recovery is available from the receipt");
                return;
            }
            bool rolled_back = false;
            std::string rollback_error;
            try {
                std::lock_guard<std::mutex> lock(mitm_proxy::g_state.history_mutex);
                const bool duplicate = std::any_of(mitm_proxy::g_state.history.begin(),
                    mitm_proxy::g_state.history.end(), [&](const auto& current) {
                        return current && current->id == removed.id;
                    });
                const std::size_t insertion = (std::min)(
                    removed_index, mitm_proxy::g_state.history.size());
                const bool previous_matches = previous_id == 0 ||
                    (insertion > 0 && mitm_proxy::g_state.history[insertion - 1] &&
                     mitm_proxy::g_state.history[insertion - 1]->id == previous_id);
                const bool next_matches = next_id == 0 ||
                    (insertion < mitm_proxy::g_state.history.size() &&
                     mitm_proxy::g_state.history[insertion] &&
                     mitm_proxy::g_state.history[insertion]->id == next_id);
                if (duplicate) {
                    rollback_error = "Removed exchange identity was reused before rollback";
                } else if (!previous_matches || !next_matches) {
                    rollback_error = "Proxy history ordering changed before exact rollback";
                } else if (mitm_proxy::g_state.history.size() >=
                           mitm_proxy::g_state.config.max_history) {
                    rollback_error = "Proxy history reached capacity before exact rollback";
                } else {
                    mitm_proxy::g_state.history.insert(
                        mitm_proxy::g_state.history.begin() +
                            static_cast<std::ptrdiff_t>(insertion),
                        std::make_shared<mitm_proxy::http_exchange>(removed));
                    rolled_back = true;
                }
            } catch (const std::exception& exception) {
                rollback_error = exception.what();
            } catch (...) {
                rollback_error = "Exact proxy removal rollback failed";
            }
            s_common_exchange_operation_pending.store(false, std::memory_order_release);
            if (rolled_back) {
                finish_network_operation(task_id, false, "Reverted",
                    "Removal was rolled back because the recovery receipt could not be published");
            } else {
                (void)aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::partial, 1.0f,
                    "Recovery unavailable",
                    "Proxy exchange was removed, but its recovery receipt could not be published and exact rollback failed: " +
                    (rollback_error.empty() ? "unknown rollback error" : rollback_error));
            }
        }, false);
    if (!posted) {
        s_common_exchange_operation_pending.store(false, std::memory_order_release);
        finish_network_operation(task_id, false, "Rejected",
            "Executor rejected reviewed proxy exchange removal");
        reason = "The Network executor rejected reviewed proxy exchange removal.";
        return false;
    }
    reason.clear();
    return true;
}

static void apply_remove_undo_completion(bool success, std::string error) {
    s_common_exchange_operation_pending.store(false, std::memory_order_release);
    s_exchange_remove_undo.operation_pending = false;
    s_exchange_remove_undo.restored = success;
    s_exchange_remove_undo.error = success ? std::string() : error;
    if (success)
        publish_network_selection(exchange_artifact_identity(
            s_exchange_remove_undo.proxy_exchange,
            artifact_kind_t::request), true);
    if (s_exchange_remove_undo.open_requested) {
        const std::string label = s_exchange_remove_undo.source == exchange_remove_source_t::proxy
            ? "Proxy exchange #" + std::to_string(s_exchange_remove_undo.proxy_exchange.id)
            : std::string("Repeater tab");
        present_remove_receipt(label);
    }
    request_proxy_runtime_snapshot(true);
    toast_notification::push(success
        ? "Removed proxy exchange restored"
        : (error.empty() ? "Proxy exchange recovery failed" : error),
        success ? toast_notification::toast_type_t::success
                : toast_notification::toast_type_t::error);
}

static void publish_remove_undo_completion_fallback(
    bool success, const std::string& error) noexcept {
    std::snprintf(s_remove_undo_completion_error.data(),
        s_remove_undo_completion_error.size(), "%s", error.c_str());
    s_remove_undo_completion_status.store(success ? 1 : 2,
        std::memory_order_release);
}

static void drain_remove_undo_completion_fallback() {
    const int status = s_remove_undo_completion_status.exchange(
        0, std::memory_order_acq_rel);
    if (status == 0)
        return;
    apply_remove_undo_completion(status == 1,
        status == 1 ? std::string()
                    : std::string(s_remove_undo_completion_error.data()));
    s_remove_undo_completion_error.fill('\0');
}

static bool submit_remove_undo(std::string& reason) {
    if (s_exchange_remove_undo.restored) {
        reason = "The removed artifact has already been restored.";
        return false;
    }
    if (s_exchange_remove_undo.source == exchange_remove_source_t::repeater) {
        const auto entry = s_exchange_remove_undo.repeater_entry;
        if (!entry || g_state.repeater_entries.size() >= k_max_repeater_entries) {
            reason = "Repeater has no capacity to restore the removed tab.";
            return false;
        }
        const bool duplicate = std::any_of(g_state.repeater_entries.begin(),
            g_state.repeater_entries.end(), [&](const auto& current) {
                return current && current->id == entry->id;
            });
        if (duplicate) {
            reason = "A Repeater tab with the removed identity already exists.";
            return false;
        }
        const std::size_t index = (std::min)(
            s_exchange_remove_undo.original_index, g_state.repeater_entries.size());
        g_state.repeater_entries.insert(
            g_state.repeater_entries.begin() + static_cast<std::ptrdiff_t>(index), entry);
        publish_repeater_request_artifacts(g_state);
        g_state.repeater_selected = static_cast<int>(index);
        s_repeater_selected_artifact_kinds[entry->id] = artifact_kind_t::repeater_request;
        publish_network_selection(repeater_artifact_identity(
            *entry, artifact_kind_t::repeater_request), true);
        const std::string task_id = register_network_operation(
            "network.exchange.remove.undo", "Restore removed Repeater tab",
            "view.network.repeater", std::to_string(entry->id));
        finish_network_operation(task_id, true, "Completed", "Repeater tab restored");
        s_exchange_remove_undo.restored = true;
        s_exchange_remove_undo.error.clear();
        reason.clear();
        return true;
    }
    if (s_exchange_remove_undo.source != exchange_remove_source_t::proxy ||
        s_exchange_remove_undo.proxy_exchange.id == 0) {
        reason = "No removed proxy exchange is available for recovery.";
        return false;
    }
    if (s_proxy_operation_pending.load(std::memory_order_acquire)) {
        reason = "Wait for the active Proxy operation before restoring reviewed history.";
        return false;
    }
    bool expected = false;
    if (!s_common_exchange_operation_pending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        reason = "Another reviewed Network artifact operation is still active.";
        return false;
    }
    s_exchange_remove_undo.operation_pending = true;
    s_remove_undo_completion_error.fill('\0');
    s_remove_undo_completion_status.store(0, std::memory_order_release);
    const auto exchange = s_exchange_remove_undo.proxy_exchange;
    const std::size_t original_index = s_exchange_remove_undo.original_index;
    const std::string task_id = register_network_operation(
        "network.exchange.remove.undo", "Restore removed proxy exchange",
        "view.network.proxy", std::to_string(exchange.id));
    const bool posted = post_network_task(
        "network_exchange_restore", aida::infra::executor::domain_t::diagnostics,
        "bounded_task", [exchange, original_index, task_id] {
            bool success = false;
            std::string error;
            try {
                std::lock_guard<std::mutex> lock(mitm_proxy::g_state.history_mutex);
                const bool duplicate = std::any_of(mitm_proxy::g_state.history.begin(),
                    mitm_proxy::g_state.history.end(), [&](const auto& current) {
                        return current && current->id == exchange.id;
                    });
                if (duplicate) {
                    error = "A proxy exchange with the removed identity already exists";
                } else if (mitm_proxy::g_state.history.size() >=
                           mitm_proxy::g_state.config.max_history) {
                    error = "Proxy history reached its configured capacity before recovery";
                } else {
                    const std::size_t index = (std::min)(
                        original_index, mitm_proxy::g_state.history.size());
                    mitm_proxy::g_state.history.insert(
                        mitm_proxy::g_state.history.begin() +
                            static_cast<std::ptrdiff_t>(index),
                        std::make_shared<mitm_proxy::http_exchange>(exchange));
                    std::uint64_t next = mitm_proxy::g_state.next_id.load(
                        std::memory_order_acquire);
                    while (next <= exchange.id &&
                           !mitm_proxy::g_state.next_id.compare_exchange_weak(
                               next, exchange.id + 1U, std::memory_order_acq_rel)) {
                    }
                    success = true;
                }
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Proxy exchange recovery failed";
            }
            finish_network_operation(task_id, success,
                success ? "Completed" : "Failed",
                success ? "Proxy exchange restored at its reviewed position" : error);
            const bool completion_queued = post_network_ui_completion(
                [success, error] {
                    apply_remove_undo_completion(success, error);
            });
            if (!completion_queued)
                publish_remove_undo_completion_fallback(success, error);
        }, false);
    if (!posted) {
        s_common_exchange_operation_pending.store(false, std::memory_order_release);
        s_exchange_remove_undo.operation_pending = false;
        finish_network_operation(task_id, false, "Rejected",
            "Executor rejected proxy exchange recovery");
        reason = "The Network executor rejected proxy exchange recovery.";
        return false;
    }
    reason.clear();
    return true;
}

static bool blank_text(const char* value) {
    if (!value || value[0] == '\0')
        return true;
    return std::all_of(value, value + std::strlen(value), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
}

static void present_exchange_review() {
    if (!s_exchange_review_display)
        return;
    exchange_review_presented_t presented;
    presented.kind = s_exchange_review.kind;
    presented.primary = s_exchange_review.primary;
    presented.related = s_exchange_review.related;
    presented.issue_name = s_exchange_review.issue_name;
    presented.issue_description = s_exchange_review.issue_description;
    presented.issue_remediation = s_exchange_review.issue_remediation;
    presented.issue_severity = s_exchange_review.issue_severity;
    presented.issue_confidence = s_exchange_review.issue_confidence;
    s_exchange_review_display(presented);
}

static void present_remove_receipt(const std::string& label) {
    if (!s_exchange_remove_receipt_display)
        return;
    exchange_remove_receipt_t receipt;
    receipt.source = s_exchange_remove_undo.source;
    receipt.label = label;
    receipt.operation_pending = s_exchange_remove_undo.operation_pending;
    receipt.restored = s_exchange_remove_undo.restored;
    receipt.error = s_exchange_remove_undo.error;
    s_exchange_remove_receipt_display(receipt);
}

}

bool submit_exchange_review_issue(const exchange_review_presented_t& values,
                                  std::string& reason) {
    std::snprintf(s_exchange_review.issue_name,
        sizeof(s_exchange_review.issue_name), "%s", values.issue_name.c_str());
    std::snprintf(s_exchange_review.issue_description,
        sizeof(s_exchange_review.issue_description), "%s", values.issue_description.c_str());
    std::snprintf(s_exchange_review.issue_remediation,
        sizeof(s_exchange_review.issue_remediation), "%s", values.issue_remediation.c_str());
    s_exchange_review.issue_severity = values.issue_severity;
    s_exchange_review.issue_confidence = values.issue_confidence;
    if (!submit_reviewed_issue(reason))
        return false;
    s_exchange_review = {};
    return true;
}

bool submit_exchange_review_replay(std::string& reason) {
    if (!submit_reviewed_replay(reason))
        return false;
    s_exchange_review = {};
    return true;
}

bool submit_exchange_review_removal(std::string& reason) {
    if (!submit_reviewed_removal(reason))
        return false;
    s_exchange_review = {};
    return true;
}

void cancel_exchange_review() noexcept {
    s_exchange_review = {};
}

bool submit_exchange_remove_undo(std::string& reason) {
    return submit_remove_undo(reason);
}

void dismiss_exchange_remove_receipt() noexcept {
    s_exchange_remove_undo = {};
}

void drain_exchange_remove_undo_fallback() {
    drain_remove_undo_completion_fallback();
}

namespace {

std::string capability_reason(network_exchange_action_t action,
                              const exchange_context_runtime_t& context) {
    if (!context.primary_current) return context.unavailable_reason.empty()
        ? "The selected artifact is stale; reopen the menu on a current row."
        : context.unavailable_reason;
    const auto* request = request_identity(context);
    const auto* response = response_identity(context);
    switch (action) {
    case network_exchange_action_t::repeater:
    case network_exchange_action_t::fuzzer:
    case network_exchange_action_t::intruder:
    case network_exchange_action_t::scanner:
    case network_exchange_action_t::sequencer: {
        if (!request) return "This action requires a retained HTTP request artifact.";
        if (request->kind == artifact_kind_t::http2_request)
            return "HTTP/2 requests must be reviewed and replayed in the HTTP/2 editor; HTTP/1 tools cannot preserve frame semantics.";
        if (request->target_host.empty() || request->target_port == 0)
            return "The request has no verified target host and port.";
        if ((action == network_exchange_action_t::intruder ||
             action == network_exchange_action_t::sequencer) &&
            (request->target_host.size() >= 256U ||
             aida::qt::net::http_text::contains_binary_bytes(request->target_host)))
            return "The retained request host cannot be represented by the destination's bounded text field.";
        if (action == network_exchange_action_t::repeater)
            return g_state.repeater_entries.size() < k_max_repeater_entries
                ? std::string()
                : "Repeater retains at most 128 reviewed tabs; close a tab before opening another.";
        const std::size_t limit = action == network_exchange_action_t::sequencer
            ? 8191U : 65535U;
        if (request->content_size > limit)
            return "The retained request exceeds the destination editor's bounded capacity.";
        if (request->target_host.size() >= 256U)
            return "The retained request host exceeds the destination's bounded host field.";
        artifact_snapshot_t snapshot;
        std::string reason;
        if (!snapshot_for(*request, snapshot, reason))
            return reason.empty() ? "The retained request is no longer available." : reason;
        if (std::find(snapshot.bytes.begin(), snapshot.bytes.end(), 0) != snapshot.bytes.end())
            return "This text-based tool cannot accept a request containing embedded NUL bytes.";
        if (action == network_exchange_action_t::sequencer ||
            action == network_exchange_action_t::scanner) {
            const std::string raw = artifact_text(snapshot);
            const std::string url = artifact_url(*request, raw);
            if (url.size() >= 1024U || aida::qt::net::http_text::contains_binary_bytes(url))
                return "The retained request URL cannot be represented by the destination's bounded text field.";
        }
        if (!intercept_editor_compatible(snapshot.bytes, reason))
            return reason;
        return {};
    }
    case network_exchange_action_t::session_handling: {
        if (!request) return "Session Handling requires a retained HTTP request artifact.";
        if (request->kind == artifact_kind_t::http2_request || request->raw_protocol)
            return "Session Handling accepts retained HTTP/1 requests only.";
        if (request->target_host.empty() || request->target_port == 0)
            return "The retained request has no verified target host and port.";
        if (request->target_host.size() >= 256U || request->content_size == 0 ||
            request->content_size >= 8192U)
            return "Session Handling requires a bounded target and request of at most 8191 bytes.";
        artifact_snapshot_t snapshot;
        std::string reason;
        if (!snapshot_for(*request, snapshot, reason))
            return reason.empty() ? "The retained request is no longer available." : reason;
        if (std::find(snapshot.bytes.begin(), snapshot.bytes.end(), 0) != snapshot.bytes.end())
            return "Session Handling cannot stage a request containing embedded NUL bytes.";
        return {};
    }
    case network_exchange_action_t::cookies: {
        if (!request) return "Cookie Jar context requires a retained HTTP request artifact.";
        if (request->kind == artifact_kind_t::http2_request || request->raw_protocol)
            return "Cookie Jar context accepts retained HTTP/1 requests only.";
        if (request->target_host.empty() || request->target_port == 0 ||
            request->target_host.size() >= 256U)
            return "The retained request has no bounded verified target.";
        artifact_snapshot_t snapshot;
        std::string reason;
        if (!snapshot_for(*request, snapshot, reason))
            return reason.empty() ? "The retained request is no longer available." : reason;
        const std::string path = cookie_request_path(artifact_text(snapshot));
        return path.size() <= 2048U ? std::string()
            : "The retained request path exceeds Cookie Jar's reviewed context limit.";
    }
    case network_exchange_action_t::match_replace: {
        const artifact_identity_t& source = context.primary;
        if ((!request_kind(source.kind) && !response_kind(source.kind)) || source.raw_protocol ||
            source.kind == artifact_kind_t::http2_request ||
            source.kind == artifact_kind_t::http2_response)
            return "Match and Replace context requires a retained HTTP/1 request or response.";
        if (source.target_host.empty() || source.target_port == 0 ||
            source.target_host.size() >= 256U)
            return "The retained artifact has no bounded verified target.";
        return {};
    }
    case network_exchange_action_t::compare_request_response:
        if (!request || !response)
            return "Request vs Response comparison requires both retained artifacts.";
        if (!matching_http1_pair(*request, *response))
            return "Request vs Response comparison requires a matching retained HTTP/1 exchange pair.";
        if (request->content_size > 16U * 1024U * 1024U ||
            response->content_size > 16U * 1024U * 1024U)
            return "Comparer pair handoff is bounded to 16 MiB per retained artifact.";
        if (aida::burp::comparer::list_slots().size() > 254U)
            return "Comparer retains at most 256 slots; remove a slot before adding this pair.";
        return {};
    case network_exchange_action_t::decoder: {
        if (context.primary.content_size >= sizeof(g_state.decoder_input))
            return "Decoder's reviewed input accepts at most 16383 bytes.";
        artifact_snapshot_t snapshot;
        std::string reason;
        if (!snapshot_for(context.primary, snapshot, reason))
            return reason.empty() ? "The retained artifact is no longer available." : reason;
        if (std::find(snapshot.bytes.begin(), snapshot.bytes.end(), 0) != snapshot.bytes.end())
            return "Decoder's text input cannot accept embedded NUL bytes.";
        const std::string_view decoder_text(
            snapshot.bytes.empty() ? "" :
                reinterpret_cast<const char*>(snapshot.bytes.data()),
            snapshot.bytes.size());
        if (aida::qt::net::http_text::contains_binary_bytes(decoder_text))
            return "Decoder's text input requires valid UTF-8 without binary control bytes.";
        return {};
    }
    case network_exchange_action_t::camoufox:
    case network_exchange_action_t::copy_url:
    case network_exchange_action_t::copy_method:
    case network_exchange_action_t::scope_include:
    case network_exchange_action_t::scope_exclude:
        if (!request) return "The selected context has no retained HTTP request.";
        if (request->raw_protocol)
            return "Raw protocol frames have no verified HTTP URL or method; inspect or decode the artifact first.";
        return {};
    case network_exchange_action_t::copy_status:
    case network_exchange_action_t::copy_response:
        return response ? std::string() : "The selected context has no retained HTTP response.";
    case network_exchange_action_t::copy_request:
        return request ? std::string() : "The selected context has no retained HTTP request.";
    case network_exchange_action_t::copy_headers:
    case network_exchange_action_t::copy_body:
        if (context.primary.kind == artifact_kind_t::websocket_frame ||
            context.primary.kind == artifact_kind_t::websocket_editor_frame ||
            context.primary.kind == artifact_kind_t::packet || context.primary.raw_protocol)
            return "This artifact is a raw payload and has no HTTP header/body boundary.";
        return {};
    case network_exchange_action_t::save_export:
        return context.primary.content_size <= k_network_export_limit
            ? std::string()
            : "The selected artifact exceeds the 256 MiB export safety limit.";
    case network_exchange_action_t::create_issue: {
        if (context.primary.target_host.empty())
            return "Manual Scanner issues require a retained target host.";
        if (context.primary.content_size > k_issue_evidence_limit ||
            (context.related.valid() && context.related.content_size > k_issue_evidence_limit))
            return "Manual issue evidence is bounded to 1 MiB per retained artifact.";
        return {};
    }
    case network_exchange_action_t::replay_live: {
        if (!request)
            return "Live replay requires a retained HTTP request artifact.";
        if (request->kind == artifact_kind_t::http2_request)
            return "HTTP/2 replay requires the protocol editor to preserve stream semantics.";
        if (request->raw_protocol)
            return "Raw protocol payloads require their protocol-specific sender.";
        if (request->target_host.empty() || request->target_port == 0)
            return "Live replay requires a verified target host and port.";
        if (request->content_size == 0 || request->content_size > 65535U)
            return "Live replay accepts reviewed HTTP/1 requests from 1 to 65535 bytes.";
        if (s_common_exchange_operation_pending.load(std::memory_order_acquire))
            return "Another reviewed Network artifact operation is still active.";
        artifact_snapshot_t snapshot;
        std::string reason;
        if (!snapshot_for(*request, snapshot, reason))
            return reason.empty() ? "The retained request is no longer available." : reason;
        if (!intercept_editor_compatible(snapshot.bytes, reason))
            return reason;
        return {};
    }
    case network_exchange_action_t::remove:
        if (s_common_exchange_operation_pending.load(std::memory_order_acquire))
            return "Another reviewed Network artifact operation is still active.";
        if (proxy_artifact_kind(context.primary.kind) &&
            context.primary.source_view_id == "view.network.proxy")
            return s_proxy_operation_pending.load(std::memory_order_acquire)
                ? "Wait for the active Proxy operation before removing reviewed history."
                : std::string();
        if (repeater_artifact_kind(context.primary.kind)) {
            const auto found = std::find_if(g_state.repeater_entries.begin(),
                g_state.repeater_entries.end(), [&](const auto& entry) {
                    return entry && entry->id == context.primary.source_id;
                });
            if (found == g_state.repeater_entries.end())
                return "The reviewed Repeater tab is no longer retained.";
            return (*found)->in_progress.load(std::memory_order_acquire)
                ? "Wait for the active Repeater send to finish before removing its tab."
                : std::string();
        }
        return "This artifact source does not expose a reversible remove operation.";
    default:
        return {};
    }
}

bool execute_exchange_action(network_exchange_action_t action,
                             const exchange_context_runtime_t& context,
                             std::string& reason) {
    const auto* request = request_identity(context);
    const auto* response = response_identity(context);
    artifact_snapshot_t primary_snapshot;
    if (!snapshot_for(context.primary, primary_snapshot, reason)) return false;
    artifact_snapshot_t request_snapshot;
    artifact_snapshot_t response_snapshot;
    const bool needs_request = action == network_exchange_action_t::repeater ||
        action == network_exchange_action_t::fuzzer ||
        action == network_exchange_action_t::intruder ||
        action == network_exchange_action_t::scanner ||
        action == network_exchange_action_t::compare_request_response ||
        action == network_exchange_action_t::session_handling ||
        action == network_exchange_action_t::cookies ||
        action == network_exchange_action_t::sequencer ||
        action == network_exchange_action_t::camoufox ||
        action == network_exchange_action_t::copy_url ||
        action == network_exchange_action_t::copy_method ||
        action == network_exchange_action_t::copy_request ||
        action == network_exchange_action_t::scope_include ||
        action == network_exchange_action_t::scope_exclude ||
        action == network_exchange_action_t::replay_live;
    const bool needs_response = action == network_exchange_action_t::copy_status ||
        action == network_exchange_action_t::copy_response ||
        action == network_exchange_action_t::compare_request_response;
    if (needs_request && request && !snapshot_for(*request, request_snapshot, reason)) return false;
    if (needs_response && response && !snapshot_for(*response, response_snapshot, reason)) return false;
    const std::string request_raw = request ? artifact_text(request_snapshot) : std::string();
    const std::string response_raw = response ? artifact_text(response_snapshot) : std::string();
    const std::string url = request ? artifact_url(*request, request_raw) : std::string();
    const auto copy = [](const std::string& value) {
        const std::string safe = clipboard_text(value);
        network_clipboard_set(safe);
        return true;
    };
    switch (action) {
    case network_exchange_action_t::repeater:
        return request && send_artifact_to_repeater(*request, reason);
    case network_exchange_action_t::fuzzer: {
        if (!request) {
            reason = "Fuzzer requires an exact retained HTTP/1 request artifact.";
            return false;
        }
        if (!intercept_editor_compatible(request_snapshot.bytes, reason))
            return false;
        const std::string open_error = open_view("view.network.fuzzer");
        if (!open_error.empty()) {
            reason = open_error;
            return false;
        }
        g_state.fuzz_config.host = request->target_host;
        g_state.fuzz_config.port = request->target_port;
        g_state.fuzz_config.use_tls = request->use_tls;
        g_state.fuzz_config.base_request.assign(
            request_snapshot.bytes.begin(), request_snapshot.bytes.end());
        if (++g_state.fuzz_request_revision == 0)
            ++g_state.fuzz_request_revision;
        reason.clear();
        return true;
    }
    case network_exchange_action_t::comparer:
        return send_artifact_to_comparer(context.primary, reason);
    case network_exchange_action_t::compare_request_response: {
        if (!request || !response || !matching_http1_pair(*request, *response)) {
            reason = "Request vs Response comparison requires a matching retained HTTP/1 exchange pair.";
            return false;
        }
        if (aida::burp::comparer::list_slots().size() > 254U) {
            reason = "Comparer retains at most 256 slots; remove a slot before adding this pair.";
            return false;
        }
        const std::string request_label = (request->label.empty() ? request->id : request->label) +
            " [Request]";
        const std::string response_label = (response->label.empty() ? response->id : response->label) +
            " [Response]";
        const std::uint64_t request_slot = aida::burp::comparer::add_slot_from_bytes(
            request_label, request_snapshot.bytes, request->id);
        if (request_slot == 0) {
            reason = "Comparer rejected the retained request: " + aida::burp::comparer::last_error();
            return false;
        }
        const std::uint64_t response_slot = aida::burp::comparer::add_slot_from_bytes(
            response_label, response_snapshot.bytes, response->id);
        if (response_slot == 0) {
            const std::string add_error = aida::burp::comparer::last_error();
            const bool request_removed = aida::burp::comparer::remove_slot(request_slot);
            reason = "Comparer rejected the retained response: " + add_error;
            if (!request_removed)
                reason += " The staged request slot remains in Comparer because rollback failed.";
            return false;
        }
        const std::string open_error = open_view("view.network.comparer");
        if (!open_error.empty()) {
            const bool response_removed = aida::burp::comparer::remove_slot(response_slot);
            const bool request_removed = aida::burp::comparer::remove_slot(request_slot);
            reason = open_error;
            if (!request_removed || !response_removed) {
                reason += " Comparer rollback was incomplete; ";
                if (!request_removed && !response_removed)
                    reason += "both staged slots remain.";
                else if (!request_removed)
                    reason += "the staged request slot remains.";
                else
                    reason += "the staged response slot remains.";
            }
            return false;
        }
        reason.clear();
        return true;
    }
    case network_exchange_action_t::session_handling: {
        if (!request) {
            reason = "Session Handling requires a retained HTTP/1 request.";
            return false;
        }
        const std::string open_error = open_view("view.network.session");
        if (!open_error.empty()) {
            reason = open_error;
            return false;
        }
        return aida::burp::session_handler_view::stage_reviewed_context(*request, reason);
    }
    case network_exchange_action_t::cookies: {
        if (!request) {
            reason = "Cookie Jar context requires a retained HTTP/1 request.";
            return false;
        }
        const std::string open_error = open_view("view.network.cookies");
        if (!open_error.empty()) {
            reason = open_error;
            return false;
        }
        return aida::burp::cookie_jar::stage_reviewed_context(
            *request, cookie_request_path(request_raw), reason);
    }
    case network_exchange_action_t::match_replace: {
        const std::string open_error = open_view("view.network.match_replace");
        if (!open_error.empty()) {
            reason = open_error;
            return false;
        }
        return aida::burp::match_replace_view::stage_reviewed_context(
            context.primary, response_kind(context.primary.kind), reason);
    }
    case network_exchange_action_t::intruder:
        if (!request || request_raw.size() >= 65536U ||
            request_raw.find('\0') != std::string::npos) {
            reason = "Intruder requires a NUL-free reviewed request of at most 65535 bytes.";
            return false;
        }
        if (!intercept_editor_compatible(request_snapshot.bytes, reason))
            return false;
        if (!aida::burp::intruder_view::open_new_attack_with(request->target_host,
                request->target_port, request->use_tls, request_raw, reason)) return false;
        return open_view("view.network.intruder").empty();
    case network_exchange_action_t::scanner:
        if (!request || request_raw.size() >= 65536U ||
            request_raw.find('\0') != std::string::npos) {
            reason = "Scanner requires a NUL-free reviewed request of at most 65535 bytes.";
            return false;
        }
        if (!intercept_editor_compatible(request_snapshot.bytes, reason))
            return false;
        if (!aida::qt::net::QtScannerView::openNewAuditWith(url, request_raw)) {
            reason = "Scanner rejected the reviewed audit draft.";
            return false;
        }
        return open_view("view.network.scanner").empty();
    case network_exchange_action_t::decoder:
        if (primary_snapshot.bytes.size() >= sizeof(g_state.decoder_input) ||
            std::find(primary_snapshot.bytes.begin(), primary_snapshot.bytes.end(), 0) !=
                primary_snapshot.bytes.end()) {
            reason = "Decoder requires NUL-free reviewed input of at most 16383 bytes.";
            return false;
        }
        {
            const std::string_view decoder_text(
                primary_snapshot.bytes.empty() ? "" :
                    reinterpret_cast<const char*>(primary_snapshot.bytes.data()),
                primary_snapshot.bytes.size());
            if (aida::qt::net::http_text::contains_binary_bytes(decoder_text)) {
                reason = "Decoder's text input requires valid UTF-8 without binary control bytes.";
                return false;
            }
        }
        aida::qt::net::decoder_stage_input(std::string(
            reinterpret_cast<const char*>(primary_snapshot.bytes.data()),
            primary_snapshot.bytes.size()));
        return open_view("view.network.decoder").empty();
    case network_exchange_action_t::sequencer:
        if (!request || request_raw.size() >= 8192U ||
            request_raw.find('\0') != std::string::npos || url.size() >= 1024U) {
            reason = "Sequencer requires a bounded NUL-free URL and request of at most 8191 bytes.";
            return false;
        }
        if (!intercept_editor_compatible(request_snapshot.bytes, reason))
            return false;
        if (!aida::burp::sequencer_view::open_new_collection_with(url, request->target_host,
                request->target_port, request->use_tls, request_raw, reason)) return false;
        return open_view("view.network.sequencer").empty();
    case network_exchange_action_t::camoufox:
        if (!aida::qt::net::QtBrowserLauncherController::stageCamoufoxUrl(url, reason)) return false;
        return open_view("view.network.browser").empty();
    case network_exchange_action_t::scope_include:
    case network_exchange_action_t::scope_exclude:
        if (!aida::burp::scope::stage_rule(url,
                action == network_exchange_action_t::scope_exclude
                    ? aida::burp::scope::rule_kind_t::exclude
                    : aida::burp::scope::rule_kind_t::include, reason)) return false;
        return open_view("view.network.scope").empty();
    case network_exchange_action_t::copy_url: return copy(url);
    case network_exchange_action_t::copy_method: return copy(request_method(request_raw));
    case network_exchange_action_t::copy_status: return copy(response_status(response_raw));
    case network_exchange_action_t::copy_request: return copy(request_raw);
    case network_exchange_action_t::copy_response: return copy(response_raw);
    case network_exchange_action_t::copy_headers:
        return copy(http_headers_body(artifact_text(primary_snapshot)).first);
    case network_exchange_action_t::copy_body:
        return copy(http_headers_body(artifact_text(primary_snapshot)).second);
    case network_exchange_action_t::copy_artifact:
        return copy(artifact_text(primary_snapshot));
    case network_exchange_action_t::save_export:
        return export_reviewed_artifact(context.primary, reason);
    case network_exchange_action_t::create_issue:
        stage_exchange_review(exchange_review_kind_t::create_issue, context);
        reason.clear();
        return true;
    case network_exchange_action_t::replay_live:
        stage_exchange_review(exchange_review_kind_t::replay, context);
        reason.clear();
        return true;
    case network_exchange_action_t::remove:
        stage_exchange_review(exchange_review_kind_t::remove, context);
        reason.clear();
        return true;
    case network_exchange_action_t::chat:
        return add_artifact_to_chat(context.primary, reason);
    case network_exchange_action_t::agent:
        return assign_artifact_to_agent(context.primary, reason);
    default:
        reason = capability_reason(action, context);
        return false;
    }
}

}

static void reset_common_exchange_actions() {
    s_exchange_review = {};
    s_exchange_remove_undo = {};
    s_common_exchange_operation_pending.store(false, std::memory_order_release);
    s_remove_undo_completion_status.store(0, std::memory_order_release);
}

bool execute_retained_exchange_toolbar_action(
    const char* requested_action_id, artifact_identity_t primary,
    artifact_identity_t related, std::string& unavailable_reason) {
    if (!requested_action_id || requested_action_id[0] == '\0') {
        unavailable_reason = "The retained Network action identity is missing.";
        return false;
    }
    const auto descriptor = std::find_if(std::begin(k_exchange_actions),
        std::end(k_exchange_actions), [&](const auto& candidate) {
            return std::strcmp(candidate.id, requested_action_id) == 0;
        });
    if (descriptor == std::end(k_exchange_actions)) {
        unavailable_reason = "The retained Network action is not registered by the exchange provider.";
        return false;
    }
    exchange_context_runtime_t retained;
    retained.primary = std::move(primary);
    retained.related = std::move(related);
    artifact_snapshot_t snapshot;
    retained.primary_current = resolve_artifact(
        retained.primary, snapshot, retained.unavailable_reason);
    if (!retained.primary_current) {
        unavailable_reason = retained.unavailable_reason;
        return false;
    }
    const std::string capability = capability_reason(descriptor->action, retained);
    aida::ui::application_ui::retained_entity_context_t context;
    context.owner_id = "network.exchange.artifact";
    context.entity_id = retained.primary.id;
    context.entity_generation = retained.primary.timestamp ^ retained.primary.revision ^
        retained.primary.content_hash;
    context.active_view = aida::ui::stable_view_id_t(
        retained.primary.source_view_id.empty()
            ? "view.network" : retained.primary.source_view_id);
    const auto retained_identity = retained.primary;
    const auto retained_related_identity = retained.related;
    context.validate_identity = [retained_identity, retained_related_identity] {
        artifact_snapshot_t live;
        std::string reason;
        if (!resolve_artifact(retained_identity, live, reason))
            return aida::ui::capability_state_t::unavailable(reason.empty()
                ? "The network artifact was replaced; select it again" : reason);
        if (retained_related_identity.valid() &&
            !resolve_artifact(retained_related_identity, live, reason))
            return aida::ui::capability_state_t::unavailable(reason.empty()
                ? "The related network artifact was replaced; select it again" : reason);
        return aida::ui::capability_state_t::available();
    };
    aida::ui::application_ui::retained_entity_action_t action;
    action.action_id = descriptor->id;
    action.capability = capability.empty()
        ? aida::ui::capability_state_t::available()
        : aida::ui::capability_state_t::unavailable(capability);
    const auto operation = descriptor->action;
    action.invoke = [retained, operation] {
        std::string reason;
        return execute_exchange_action(operation, retained, reason)
            ? aida::ui::action_handler_result_t::completed()
            : aida::ui::action_handler_result_t::failed(reason.empty()
                ? "The retained Network operation was rejected" : reason);
    };
    context.actions.push_back(std::move(action));
    const auto result = aida::ui::application_ui::execute_retained_entity_action(
        requested_action_id, aida::ui::action_invocation_source_t::toolbar, context);
    if (!result.executed()) {
        unavailable_reason = result.message.empty()
            ? "The retained Network operation was rejected" : result.message;
        return false;
    }
    unavailable_reason.clear();
    return true;
}

void open_exchange_context(artifact_identity_t primary, artifact_identity_t related,
                           exchange_context_origin_t origin,
                           bool include_intercept_actions) {
    exchange_context_runtime_t retained;
    retained.primary = std::move(primary);
    retained.related = std::move(related);
    artifact_snapshot_t snapshot;
    retained.primary_current = resolve_artifact(
        retained.primary, snapshot, retained.unavailable_reason);
    if (!retained.primary_current) return;

    aida::ui::application_ui::retained_entity_context_t context;
    context.owner_id = "network.exchange.artifact";
    context.entity_id = retained.primary.id;
    context.entity_generation = retained.primary.timestamp ^ retained.primary.revision ^
        retained.primary.content_hash;
    context.active_view = aida::ui::stable_view_id_t(
        retained.primary.source_view_id.empty()
            ? "view.network" : retained.primary.source_view_id);
    const auto retained_identity = retained.primary;
    const auto retained_related_identity = retained.related;
    context.validate_identity = [retained_identity, retained_related_identity] {
        artifact_snapshot_t live;
        std::string reason;
        if (!resolve_artifact(retained_identity, live, reason))
            return aida::ui::capability_state_t::unavailable(reason.empty()
                ? "The network artifact was replaced; select it again" : reason);
        if (retained_related_identity.valid() &&
            !resolve_artifact(retained_related_identity, live, reason))
            return aida::ui::capability_state_t::unavailable(reason.empty()
                ? "The related network artifact was replaced; select it again" : reason);
        return aida::ui::capability_state_t::available();
    };
    const auto append = [&context](const char* id, bool enabled, const char* reason,
            std::function<aida::ui::action_handler_result_t()> invoke) {
        aida::ui::application_ui::retained_entity_action_t action;
        action.action_id = id;
        action.capability = enabled
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(reason);
        action.invoke = std::move(invoke);
        context.actions.push_back(std::move(action));
    };
    for (const auto& descriptor : k_exchange_actions) {
        aida::ui::application_ui::retained_entity_action_t action;
        action.action_id = descriptor.id;
        const std::string unavailable = capability_reason(descriptor.action, retained);
        action.capability = unavailable.empty()
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(unavailable);
        const auto operation = descriptor.action;
        action.invoke = [retained, operation] {
            std::string reason;
            return execute_exchange_action(operation, retained, reason)
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(reason.empty()
                    ? "The network operation was rejected" : reason);
        };
        context.actions.push_back(std::move(action));
    }
    if (include_intercept_actions &&
        retained.primary.kind == artifact_kind_t::intercept_request) {
        const auto publication = std::atomic_load_explicit(
            &s_intercept_runtime_snapshot, std::memory_order_acquire);
        intercept_target_identity_t target;
        if (publication) {
            const auto found = std::find_if(publication->held.begin(),
                publication->held.end(), [&](const auto& exchange) {
                    return exchange.id == retained.primary.source_id &&
                        exchange.timestamp == retained.primary.timestamp &&
                        exchange.raw_request.size() == retained.primary.content_size &&
                        artifact_hash(exchange.raw_request) == retained.primary.content_hash;
                });
            if (found != publication->held.end())
                target = intercept_target_identity(*publication, *found);
        }
        const auto append_intercept = [&](const char* id,
                                          intercept_command_t command) {
            const auto capability = intercept_command_capability_for(
                command, publication, target);
            append(id, capability.enabled,
                capability.disabled_reason.empty()
                    ? "The retained Intercept action is unavailable"
                    : capability.disabled_reason.c_str(),
                [command, publication, target] {
                    std::string reason;
                    return execute_reviewed_intercept_command(
                            command, publication, target, &reason)
                        ? aida::ui::action_handler_result_t::completed()
                        : aida::ui::action_handler_result_t::failed(reason.empty()
                            ? "The retained Intercept operation was rejected" : reason);
                });
        };
        append_intercept("network.intercept.forward_selected",
            intercept_command_t::forward_selected);
        append_intercept("network.intercept.drop_selected",
            intercept_command_t::drop_selected);
        append_intercept("network.intercept.forward_modified",
            intercept_command_t::forward_modified);
    }
    const auto* request = request_identity(retained);
    artifact_snapshot_t request_snapshot;
    std::string request_reason;
    const bool request_current = request &&
        snapshot_for(*request, request_snapshot, request_reason);
    const std::string request_raw = request_current
        ? artifact_text(request_snapshot) : std::string();
    const std::string curl = request_current
        ? curl_command(*request, request_raw) : std::string();
    append("network.exchange.copy_curl", !curl.empty(),
        request_reason.empty() ? "The retained context has no complete HTTP request"
                               : request_reason.c_str(), [curl] {
            network_clipboard_set(curl);
            return aida::ui::action_handler_result_t::completed();
        });

    if (retained.related.valid()) {
        const auto related = retained.related;
        append("network.exchange.related_comparer", true, "", [related] {
            std::string reason;
            return send_artifact_to_comparer(related, reason)
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(reason);
        });
        append("network.exchange.related_chat", true, "", [related] {
            std::string reason;
            return add_artifact_to_chat(related, reason)
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(reason);
        });
        append("network.exchange.related_agent", true, "", [related] {
            std::string reason;
            return assign_artifact_to_agent(related, reason)
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(reason);
        });
    }

    if (request && request->source_view_id == "view.network.proxy") {
        const std::string host = request->target_host;
        const std::string method = request_method(request_raw);
        append("network.proxy.filter_host", !host.empty(),
            "The retained request has no target host", [host] {
                std::snprintf(g_state.proxy_filter_text,
                    sizeof(g_state.proxy_filter_text), "%s", host.c_str());
                return aida::ui::action_handler_result_t::completed();
            });
        append("network.proxy.filter_method", !method.empty(),
            "The retained request has no HTTP method", [method] {
                std::snprintf(g_state.proxy_filter_text,
                    sizeof(g_state.proxy_filter_text), "%s", method.c_str());
                return aida::ui::action_handler_result_t::completed();
            });
        append("network.proxy.clear_filter", g_state.proxy_filter_text[0] != '\0',
            "The Proxy history filter is already clear", [] {
                g_state.proxy_filter_text[0] = '\0';
                return aida::ui::action_handler_result_t::completed();
            });
    }

    if (request && request->kind == artifact_kind_t::repeater_request) {
        const auto retained_request = *request;
        const bool can_duplicate = g_state.repeater_entries.size() < k_max_repeater_entries;
        append("network.repeater.duplicate", can_duplicate,
            "Repeater retains at most 128 reviewed tabs; close a tab before duplicating.",
            [retained_request] {
            const auto found = std::find_if(g_state.repeater_entries.begin(),
                g_state.repeater_entries.end(), [&](const auto& item) {
                    return item && item->id == retained_request.source_id;
                });
            if (found == g_state.repeater_entries.end())
                return aida::ui::action_handler_result_t::failed(
                    "The Repeater request was removed; select it again");
            const auto& source = **found;
            if (source.request_revision != retained_request.revision ||
                source.request_hash != retained_request.content_hash)
                return aida::ui::action_handler_result_t::failed(
                    "The Repeater request changed; review it again before duplicating");
            if (g_state.repeater_entries.size() >= k_max_repeater_entries)
                return aida::ui::action_handler_result_t::failed(
                    "Repeater capacity changed; close a tab before duplicating");
            auto duplicate = std::make_shared<repeater_entry_t>();
            duplicate->id = s_repeater_artifact_sequence.fetch_add(
                1, std::memory_order_relaxed);
            duplicate->source_artifact_id = retained_request.id;
            duplicate->source_session_id = retained_request.session_id;
            duplicate->host = source.host;
            duplicate->port = source.port;
            duplicate->use_tls = source.use_tls;
            duplicate->raw_request = source.raw_request;
            duplicate->request_hash = source.request_hash;
            duplicate->reviewed_source_hash = source.reviewed_source_hash;
            duplicate->review_provenance = source.review_provenance;
            duplicate->reviewed_draft = source.reviewed_draft;
            g_state.repeater_entries.push_back(std::move(duplicate));
            publish_repeater_request_artifacts(g_state);
            g_state.repeater_selected = static_cast<int>(
                g_state.repeater_entries.size()) - 1;
            return aida::ui::action_handler_result_t::completed();
        });
    }

    if (retained.primary.kind == artifact_kind_t::repeater_response) {
        const auto response = retained.primary;
        append("network.repeater.clear_response", response.content_size != 0,
            "Send the request and receive a response first", [response] {
                const auto found = std::find_if(g_state.repeater_entries.begin(),
                    g_state.repeater_entries.end(), [&](const auto& item) {
                        return item && item->id == response.source_id;
                    });
                if (found == g_state.repeater_entries.end())
                    return aida::ui::action_handler_result_t::failed(
                        "The Repeater response was removed; select it again");
                auto& entry = **found;
                if (entry.response_timestamp != response.timestamp ||
                    entry.response_hash != response.content_hash)
                    return aida::ui::action_handler_result_t::failed(
                        "The Repeater response changed; select it again");
                entry.raw_response.clear();
                entry.status_code = 0;
                entry.latency_ms = 0;
                entry.response_hash = 0;
                entry.response_timestamp = 0;
                return aida::ui::action_handler_result_t::completed();
            });
    }

    if (retained.primary.kind == artifact_kind_t::websocket_frame) {
        const std::string host = retained.primary.target_host;
        append("network.websocket.copy_host", !host.empty(),
            "The retained frame has no host", [host] {
                network_clipboard_set(host);
                return aida::ui::action_handler_result_t::completed();
            });
        append("network.websocket.filter_host", !host.empty(),
            "The retained frame has no host", [host] {
                aida::qt::net::request_ws_filter_host(host);
                return aida::ui::action_handler_result_t::completed();
            });
        append("network.websocket.toggle_follow", true, "", [] {
            aida::qt::net::request_ws_toggle_follow();
            return aida::ui::action_handler_result_t::completed();
        });
        append("network.websocket.open_editor", false,
            "The WebSocket Editor backend does not expose a capability-backed import operation for captured frames", {});
    }
    const auto retained_origin = origin == exchange_context_origin_t::pointer
        ? aida::ui::context_menu_open_origin_t::pointer
        : origin == exchange_context_origin_t::shift_f10
        ? aida::ui::context_menu_open_origin_t::shift_f10
        : aida::ui::context_menu_open_origin_t::menu_key;
    if (s_exchange_context_display) {
        s_exchange_context_display(std::move(context), retained_origin);
        return;
    }
    aida::ui::application_ui::open_retained_entity_context_menu(
        std::move(context), retained_origin);
}

}
