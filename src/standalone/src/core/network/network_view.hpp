#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mitm_proxy.hpp"


namespace voyager { class device_t; }
namespace ssl_keylog { struct keylog_entry; }
namespace cert_intercept {
struct diagnostic_context_t;
struct process_diagnostics_t;
struct provider_status_t;
}
namespace aida::ui {
enum class context_menu_open_origin_t : std::uint8_t;
namespace application_ui { struct retained_entity_context_t; }
}

namespace network_view {


enum class sub_tab_t : int {
    connections = 0,
    capture,
    intercept,
    proxy,
    dns,
    filters,
    bandwidth,
    repeater,
    keylog,
    pcap_export,
    fuzzer,
    offensive,
    websocket,
    scripting,
    decoder,
    sitemap,
    scope,
    cookies,
    scanner,
    recon,
    intruder,
    collab,
    sequencer,
    comparer,
    jwt,
    mr,
    session,
    api,
    ws_edit,
    h2_edit,
    logger,
    csp,
    upstream,
    browser,
    reports,
    headless,
    COUNT
};

enum class intercept_command_t : std::uint8_t {
    forward_selected,
    drop_selected,
    forward_all,
    drop_all,
    forward_modified
};

struct intercept_command_capability_t {
    bool enabled = false;
    std::string disabled_reason;
};

intercept_command_capability_t intercept_command_capability(intercept_command_t command);
bool execute_intercept_command(intercept_command_t command, std::string* error = nullptr);

enum class operational_command_t : std::uint8_t {
    capture_start,
    capture_stop,
    proxy_start,
    proxy_stop,
    proxy_history_clear,
    proxy_ca_trust_repair,
    filter_add,
    filter_remove_selected,
    filter_clear,
    intercept_toggle,
    keylog_launch,
    keylog_watch,
    keylog_stop,
    keylog_clear
};

struct operational_command_capability_t {
    bool enabled = false;
    bool checked = false;
    std::string disabled_reason;
    std::string target_summary;
};

operational_command_capability_t operational_command_capability(
    operational_command_t command);
bool prepare_operational_command_confirmation(operational_command_t command,
                                              std::string* error = nullptr);
void cancel_operational_command_confirmation(operational_command_t command) noexcept;
bool execute_operational_command(operational_command_t command,
                                 std::string* error = nullptr);


struct connection_entry_t {
    uint32_t pid = 0;
    uint8_t  protocol = 0;
    uint8_t  state = 0;
    uint16_t local_port = 0;
    uint16_t remote_port = 0;
    uint8_t  address_family = 0;
    uint8_t  local_addr[16] = {};
    uint8_t  remote_addr[16] = {};
    std::string process_name;
};


struct packet_entry_t {
    uint64_t    timestamp = 0;
    uint32_t    pid = 0;
    uint8_t     protocol = 0;
    uint8_t     direction = 0;
    uint16_t    src_port = 0;
    uint16_t    dst_port = 0;
    uint8_t     src_addr[16] = {};
    uint8_t     dst_addr[16] = {};
    uint32_t    payload_size = 0;
    std::vector<uint8_t> payload;
    std::string protocol_label;
    std::string summary;
};


struct dns_entry_t {
    uint64_t    timestamp = 0;
    uint32_t    pid = 0;
    uint16_t    query_type = 0;
    std::string domain;
    std::string resolved_addr;
    uint32_t    response_code = 0;
    uint32_t    ttl = 0;
};


struct filter_entry_t {
    uint32_t rule_id = 0;
    uint8_t  action = 0;
    uint8_t  direction = 0;
    uint8_t  protocol = 0;
    uint32_t pid = 0;
    uint16_t port = 0;
    std::string ip_addr;
    bool     active = true;
};


struct bw_entry_t {
    uint32_t    pid = 0;
    std::string process_name;
    uint64_t    bytes_in = 0;
    uint64_t    bytes_out = 0;
    float       rate_in = 0.f;
    float       rate_out = 0.f;
    float       rate_history[64] = {};
    int         history_index = 0;
};


struct repeater_entry_t {
    std::uint64_t       id = 0;
    std::string         source_artifact_id;
    std::string         source_session_id;
    std::uint64_t       request_revision = 1;
    std::uint64_t       request_hash = 0;
    std::uint64_t       response_hash = 0;
    std::uint64_t       response_timestamp = 0;
    std::uint64_t       reviewed_source_hash = 0;
    std::string         review_provenance;
    std::string       host;
    uint16_t          port = 443;
    bool              use_tls = true;
    std::string       raw_request;
    std::string       raw_response;
    int               status_code = 0;
    uint64_t          latency_ms = 0;
    bool              reviewed_draft = false;
    std::atomic<bool> in_progress{false};
};

enum class fuzzer_attack_mode_t : int {
    sniper      = 0,
    pitchfork   = 1,
    clusterbomb = 2,
};

enum class artifact_kind_t : std::uint8_t {
    packet = 0,
    exchange,
    request,
    response,
    websocket_frame,
    repeater_request,
    repeater_response,
    sitemap_request,
    sitemap_response,
    api_request,
    api_response,
    websocket_editor_frame,
    http2_request,
    http2_response,
    intruder_response,
    scanner_request,
    scanner_response,
    intercept_request
};

enum class exchange_context_origin_t : std::uint8_t {
    pointer = 0,
    menu_key,
    shift_f10
};

struct artifact_identity_t {
    std::string id;
    std::string parent_id;
    std::string source_view_id;
    std::string session_id;
    artifact_kind_t kind = artifact_kind_t::packet;
    std::uint64_t source_id = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t revision = 0;
    std::uint64_t content_hash = 0;
    std::size_t content_size = 0;
    std::string label;
    std::string target_host;
    std::uint16_t target_port = 0;
    bool use_tls = false;
    bool raw_protocol = false;

    bool valid() const noexcept {
        return !id.empty() && !source_view_id.empty() && content_hash != 0;
    }
};

struct artifact_snapshot_t {
    artifact_identity_t identity;
    std::vector<std::uint8_t> bytes;
};

bool resolve_artifact(const artifact_identity_t& identity,
                      artifact_snapshot_t& snapshot,
                      std::string& unavailable_reason);
bool validate_reviewed_request(const artifact_identity_t& source,
                               const std::vector<std::uint8_t>& reviewed_request,
                               artifact_identity_t& canonical_source,
                               std::string& unavailable_reason);
bool stage_validated_reviewed_request(const artifact_identity_t& canonical_source,
                                      const std::vector<std::uint8_t>& reviewed_request,
                                      const std::string& provenance,
                                      artifact_identity_t& staged_identity,
                                      std::string& unavailable_reason);
struct payload_set_t {
    std::string              name;
    std::string              source;
    int                      type = 0;
    std::vector<std::string> entries;
};


struct state_t {
    bool active = false;

    sub_tab_t active_tab = sub_tab_t::connections;

    uint32_t                      conn_filter_pid = 0;
    uint8_t                       conn_filter_protocol = 0;
    std::atomic<bool>             conn_pane_visible{false};
    std::atomic<bool>             conn_auto_refresh_enabled{true};
    std::atomic<bool>             conn_thread_done{true};
    std::atomic<bool>             conn_polling{false};
    std::atomic<bool>             conn_refresh_pending{false};
    std::atomic<std::uint64_t>    conn_refresh_serial{0};
    std::mutex                    conn_cv_mutex;
    std::condition_variable       conn_cv;


    std::mutex                    cap_mutex;
    std::deque<packet_entry_t>    captured_packets;
    size_t                        cap_max_packets = 8192;
    std::atomic<bool>             cap_running{false};
    std::atomic<bool>             cap_start_pending{false};
    std::atomic<bool>             cap_stop_pending{false};
    uint32_t                      cap_filter_pid = 0;
    uint16_t                      cap_filter_port = 0;
    uint8_t                       cap_filter_protocol = 0;
    std::atomic<bool>             cap_thread_done{true};
    std::atomic<bool>             cap_polling{false};
    std::mutex                    cap_cv_mutex;
    std::condition_variable       cap_cv;
    std::atomic<bool>             cap_thread_alive{false};


    char                          proxy_bind_addr[64] = "127.0.0.1";
    int                           proxy_port = 8443;
    bool                          proxy_decode_tls = true;
    char                          proxy_filter_text[128] = {};


    std::mutex                    dns_mutex;
    std::deque<dns_entry_t>       dns_entries;
    size_t                        dns_max_entries = 8192;
    uint32_t                      dns_filter_pid = 0;
    std::atomic<bool>             dns_thread_done{true};
    std::atomic<bool>             dns_polling{false};
    std::atomic<bool>             dns_refresh_pending{false};
    std::atomic<std::uint64_t>    dns_refresh_serial{0};
    std::mutex                    dns_cv_mutex;
    std::condition_variable       dns_cv;
    std::atomic<bool>             dns_thread_alive{false};


    std::vector<filter_entry_t>   filters;
    int                           filter_selected = -1;
    std::atomic<bool>             filter_mutation_pending{false};
    std::atomic<std::uint64_t>    filter_mutation_serial{0};

    int   nf_action = 0;
    int   nf_direction = 2;
    int   nf_protocol = 0;
    char  nf_pid[16] = {};
    char  nf_port[16] = {};
    char  nf_ip[64] = {};


    std::mutex                    bw_mutex;
    std::vector<bw_entry_t>       bw_entries;
    bool                          bw_monitoring = false;
    std::atomic<bool>             bw_thread_done{true};
    std::atomic<bool>             bw_polling{false};
    std::atomic<bool>             bw_control_pending{false};
    std::atomic<std::uint64_t>    bw_control_serial{0};
    std::mutex                    bw_cv_mutex;
    std::condition_variable       bw_cv;
    std::atomic<bool>             bw_thread_alive{false};


    std::vector<std::shared_ptr<repeater_entry_t>> repeater_entries;
    int                                            repeater_selected = 0;
    char                          rep_host[256] = {};
    int                           rep_port = 443;
    bool                          rep_use_tls = true;


    char                          kl_exe_path[512] = {};
    char                          kl_args[512] = {};
    char                          kl_watch_path[512] = {};


    char                          pcap_path[512] = {};
    std::atomic<bool>             pcap_writing{false};
    std::atomic<uint32_t>         pcap_written_count{0};
    uint32_t                      pcap_filter_pid = 0;
    uint8_t                       pcap_filter_protocol = 0;
    std::string                   pcap_last_path;
    std::string                   pcap_last_error;
    std::mutex                    pcap_error_mutex;
    std::atomic<bool>             har_writing{false};
    std::atomic<std::uint32_t>    har_written_count{0};
    std::string                   har_last_path;
    std::string                   har_last_error;
    std::mutex                    har_status_mutex;


    struct fuzzer_entry_t {
        std::string host;
        uint16_t    port = 443;
        bool        use_tls = true;
        std::string base_request;
        std::string payload_source;
        int         payload_type = 0;
        int         thread_count = 4;
        int         delay_ms = 0;
        int         match_status = 0;
        std::string match_body;
        int         match_size_op = 0;
        int         match_size = 0;
        bool        stop_on_match = false;
        std::uint64_t maximum_requests = 10000;
        bool        maximum_requests_reviewed = false;
        char        extract_literal[256] = {};
        fuzzer_attack_mode_t    attack_mode = fuzzer_attack_mode_t::sniper;
        std::vector<payload_set_t> payload_sets;
    };

    struct fuzzer_result_t {
        std::uint64_t index = 0;
        int         status_code = 0;
        size_t      response_len = 0;
        uint64_t    latency_ms = 0;
        bool        match = false;
        std::string response_preview;
        std::string error;
        std::vector<std::uint32_t> payload_indices;
        std::uint32_t active_position = 0;
        std::string extracted_value;
    };

    struct fuzzer_result_page_t {
        std::vector<fuzzer_result_t> rows;
        std::uint64_t retained_bytes = 0;
    };

    struct fuzzer_results_snapshot_t {
        std::vector<std::shared_ptr<const fuzzer_result_page_t>> pages;
        std::shared_ptr<const std::vector<std::vector<std::string>>> payload_catalog;
        std::uint64_t retained_count = 0;
        std::uint64_t dropped_count = 0;
        std::uint64_t retained_bytes = 0;
        std::uint64_t generation = 0;
        std::size_t maximum_payload_columns = 1;
        bool has_extracted_values = false;
        bool has_failures = false;
    };

    fuzzer_entry_t                fuzz_config;
    fuzzer_entry_t                fuzz_active_config;
    std::uint64_t                 fuzz_request_revision = 1;
    std::mutex                    fuzz_mutex;
    std::deque<std::shared_ptr<const fuzzer_result_page_t>> fuzz_result_pages;
    std::vector<fuzzer_result_t>  fuzz_result_pending;
    std::shared_ptr<const std::vector<std::vector<std::string>>> fuzz_payload_catalog;
    std::shared_ptr<const fuzzer_results_snapshot_t> fuzz_results_snapshot =
        std::make_shared<const fuzzer_results_snapshot_t>();
    std::uint64_t                 fuzz_retained_count = 0;
    std::uint64_t                 fuzz_dropped_count = 0;
    std::uint64_t                 fuzz_retained_bytes = 0;
    std::uint64_t                 fuzz_pending_bytes = 0;
    std::uint64_t                 fuzz_results_generation = 0;
    std::size_t                   fuzz_maximum_payload_columns = 1;
    bool                          fuzz_has_extracted_values = false;
    bool                          fuzz_has_failures = false;
    std::atomic<bool>             fuzz_running{false};
    std::atomic<bool>             fuzz_cancel_requested{false};
    std::atomic<std::uint64_t>    fuzz_progress{0};
    std::atomic<std::uint64_t>    fuzz_total{0};
    std::atomic<std::uint64_t>    fuzz_run_generation{0};
    std::atomic<bool>             fuzz_thread_done{true};
    std::mutex                    fuzz_cv_mutex;
    std::condition_variable       fuzz_cv;
    std::atomic<bool>             fuzz_thread_alive{false};
    std::uint64_t                 fuzz_selected = 0;
    bool                          fuzz_has_selection = false;
    std::string                   fuzz_task_id;
    std::string                   fuzz_last_stage;
    std::string                   fuzz_last_error;

    char                          off_target_url[1024] = {};
    char                          off_target_param[128] = {};
    char                          off_payload_json[8192] = "{}";
    char                          off_raw_request[32768] = {};
    int                           off_workflow = 0;
    int                           off_timeout_ms = 15000;
    int                           off_max_payloads = 16;
    int                           off_max_requests = 32;
    bool                          off_scope_only = true;
    std::atomic<bool>             off_running{false};
    std::atomic<uint64_t>         off_run_id{0};
    std::atomic<uint64_t>         off_active_fuzz_job_id{0};
    std::mutex                    off_mutex;
    std::string                   off_status = "Idle";
    std::string                   off_result;


    struct ws_frame_entry_t {
        uint64_t    timestamp = 0;
        uint64_t    exchange_id = 0;
        std::string host;
        uint16_t    port = 0;
        bool        is_outbound = false;
        bool        is_text = false;
        uint8_t     opcode = 0;
        std::vector<uint8_t> payload;
        std::string preview;
    };
    std::mutex                    ws_mutex;
    std::deque<ws_frame_entry_t> ws_frames;
    size_t                        ws_max_frames = 4096;
    int                           ws_selected = -1;
    bool                          ws_auto_scroll = true;
    char                          ws_filter_text[128] = {};


    struct script_entry_t {
        std::string name;
        std::string path;
        bool        enabled = true;
        bool        loaded = false;
    };
    std::vector<script_entry_t>   scripts;
    int                           script_selected = -1;
    std::atomic<bool>             script_operation_pending{false};
    std::atomic<bool>             script_open_pending{false};
    std::atomic<std::uint64_t>    script_operation_serial{0};
    char                          script_editor_buf[32768] = {};
    char                          script_console_buf[512] = {};
    std::mutex                    script_log_mutex;
    std::deque<std::string>       script_log;
    size_t                        script_log_max = 2048;
    bool                          script_log_auto_scroll = true;


    struct decoder_step_t {
        std::string transform_name;
        std::vector<std::pair<std::string, std::string>> params;
    };
    std::vector<decoder_step_t>   decoder_pipeline;
    char                          decoder_input[16384] = {};
    std::size_t                   decoder_input_size = 0;
    std::string                   decoder_output;
    int                           decoder_selected_step = -1;
    int                           decoder_add_transform = 0;
};

inline state_t g_state;


void initialize();
void shutdown();


struct proxy_runtime_snapshot_t {
    mitm_proxy::proxy_stats stats;
    std::vector<mitm_proxy::http_exchange> history;
    bool ca_ready = false;
    bool ca_installed = false;
    std::string spki_prefix;
    bool controlled_browser_running = false;
    bool bypass_active = false;
    std::size_t bypass_count = 0;
};

struct intercept_runtime_snapshot_t {
    std::uint64_t generation = 0;
    bool running = false;
    bool enabled = false;
    std::vector<mitm_proxy::http_exchange> held;
};

struct intercept_target_identity_t {
    std::uint64_t publication_generation = 0;
    std::uint64_t exchange_id = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t content_hash = 0;
    std::size_t content_size = 0;

    bool valid() const noexcept {
        return publication_generation != 0 && exchange_id != 0 &&
            content_hash != 0;
    }
};

inline constexpr std::size_t k_intercept_editor_capacity = 65536U;

struct intercept_modified_draft_t {
    intercept_target_identity_t source;
    std::string raw_request;
    std::uint64_t content_hash = 0;
    bool loaded = false;
    bool editable = false;
    std::string unavailable_reason;
};

enum class intercept_operation_t {
    set_enabled,
    forward_all,
    drop_all,
    forward_one,
    drop_one,
    forward_modified
};

struct intercept_drop_review_t {
    bool open = false;
    bool all = false;
    intercept_target_identity_t target;
    std::size_t reviewed_count = 0;
    std::shared_ptr<const intercept_runtime_snapshot_t> reviewed_publication;
};

struct keylog_runtime_snapshot_t {
    bool watching = false;
    std::string path;
    std::size_t entry_count = 0;
    std::vector<ssl_keylog::keylog_entry> entries;
};


using open_view_handler_t = std::function<std::string(const char* view_id)>;
void set_open_view_handler(open_view_handler_t handler);
std::string open_view(const char* view_id);

using save_file_dialog_fn = std::function<std::string(
    const char* title, const char* filter_pairs, const char* default_extension,
    const std::string& initial_name)>;
void set_save_file_dialog_handler(save_file_dialog_fn fn);

void set_clipboard_text_handler(std::function<void(const std::string&)> fn);

void request_driver_available_snapshot(bool force = false);
bool driver_available_snapshot();

using exchange_context_display_fn = std::function<void(
    aida::ui::application_ui::retained_entity_context_t context,
    aida::ui::context_menu_open_origin_t origin)>;
void set_exchange_context_display(exchange_context_display_fn fn);


enum class exchange_review_kind_t : std::uint8_t {
    none,
    create_issue,
    replay,
    remove
};

enum class exchange_remove_source_t : std::uint8_t {
    none,
    proxy,
    repeater
};

struct exchange_review_presented_t {
    exchange_review_kind_t kind = exchange_review_kind_t::none;
    artifact_identity_t primary;
    artifact_identity_t related;
    std::string issue_name;
    std::string issue_description;
    std::string issue_remediation;
    int issue_severity = 0;
    int issue_confidence = 0;
};

using exchange_review_display_fn = std::function<void(const exchange_review_presented_t& presented)>;
void set_exchange_review_display(exchange_review_display_fn fn);
bool submit_exchange_review_issue(const exchange_review_presented_t& values, std::string& reason);
bool submit_exchange_review_replay(std::string& reason);
bool submit_exchange_review_removal(std::string& reason);
void cancel_exchange_review() noexcept;

struct exchange_remove_receipt_t {
    exchange_remove_source_t source = exchange_remove_source_t::none;
    std::string label;
    bool operation_pending = false;
    bool restored = false;
    std::string error;
};

using exchange_remove_receipt_fn = std::function<void(const exchange_remove_receipt_t& receipt)>;
void set_exchange_remove_receipt_display(exchange_remove_receipt_fn fn);
bool submit_exchange_remove_undo(std::string& reason);
void dismiss_exchange_remove_receipt() noexcept;
void drain_exchange_remove_undo_fallback();


using connection_snapshot_sink_t = std::function<void(
    std::shared_ptr<const std::vector<connection_entry_t>> snapshot)>;
using capture_batch_sink_t = std::function<void(
    std::shared_ptr<const std::vector<packet_entry_t>> batch,
    std::size_t trimmed_from_front)>;
using dns_batch_sink_t = std::function<void(
    std::shared_ptr<const std::vector<dns_entry_t>> batch,
    std::size_t trimmed_from_front)>;
using bandwidth_snapshot_sink_t = std::function<void(
    std::shared_ptr<const std::vector<bw_entry_t>> snapshot)>;

void set_connection_snapshot_sink(connection_snapshot_sink_t sink);
void set_capture_batch_sink(capture_batch_sink_t sink);
void set_dns_batch_sink(dns_batch_sink_t sink);
void set_bandwidth_snapshot_sink(bandwidth_snapshot_sink_t sink);
std::shared_ptr<const std::vector<packet_entry_t>> capture_packets_snapshot();
std::shared_ptr<const std::vector<dns_entry_t>> dns_entries_snapshot();
std::string capture_control_status_text();
std::size_t capture_buffered_count();

void request_connection_refresh();
void request_dns_refresh();
void request_bandwidth_control(bool start);
bool start_pcap_export();
bool start_har_export(const std::string& har_path);
std::string filter_draft_error();
bool filter_mutation_pending();
using filters_changed_sink_t = std::function<void()>;
void set_filters_changed_sink(filters_changed_sink_t sink);


void request_proxy_runtime_snapshot(bool force = false);
std::shared_ptr<const proxy_runtime_snapshot_t> proxy_runtime_snapshot();
using proxy_snapshot_sink_t = std::function<void(
    std::shared_ptr<const proxy_runtime_snapshot_t> snapshot)>;
void set_proxy_snapshot_sink(proxy_snapshot_sink_t sink);
bool proxy_operation_pending();
void request_legacy_bypass_revert(std::size_t reviewed_count);
void request_certificate_diagnostics(std::uint32_t target_pid,
                                     cert_intercept::diagnostic_context_t context);
void request_certificate_handoff(cert_intercept::process_diagnostics_t report,
                                 std::vector<cert_intercept::provider_status_t> providers,
                                 std::string proxy_endpoint);
bool cert_diagnostics_pending();
bool cert_handoff_pending();
using cert_diagnostics_sink_t = std::function<void(
    bool success, cert_intercept::process_diagnostics_t report,
    std::vector<cert_intercept::provider_status_t> providers, std::string status)>;
using cert_handoff_sink_t = std::function<void(bool success, std::string status)>;
void set_cert_diagnostics_sink(cert_diagnostics_sink_t sink);
void set_cert_handoff_sink(cert_handoff_sink_t sink);


void request_intercept_runtime_snapshot(bool force = false);
std::shared_ptr<const intercept_runtime_snapshot_t> intercept_runtime_snapshot();
using intercept_snapshot_sink_t = std::function<void(
    std::shared_ptr<const intercept_runtime_snapshot_t> snapshot)>;
void set_intercept_snapshot_sink(intercept_snapshot_sink_t sink);
bool intercept_operation_pending();
intercept_target_identity_t intercept_target_identity(
    const intercept_runtime_snapshot_t& publication,
    const mitm_proxy::http_exchange& exchange);
bool intercept_editor_compatible(const std::vector<std::uint8_t>& bytes,
                                 std::string& unavailable_reason);
void retain_intercept_modified_draft(const intercept_runtime_snapshot_t& publication,
                                     const mitm_proxy::http_exchange& exchange);
bool refresh_intercept_modified_draft(std::string& unavailable_reason);
intercept_modified_draft_t intercept_modified_draft();
void set_intercept_modified_draft_text(std::string raw_request);
void set_intercept_editor_state(bool pretty_dirty, bool oversized, bool binary,
                                std::string error);
void set_intercept_selected_exchange(std::uint64_t exchange_id);
using intercept_drop_review_display_fn = std::function<void(intercept_drop_review_t review)>;
void set_intercept_drop_review_display(intercept_drop_review_display_fn fn);
bool confirm_intercept_drop_review();
void cancel_intercept_drop_review() noexcept;


void request_keylog_runtime_snapshot(bool force = false);
std::shared_ptr<const keylog_runtime_snapshot_t> keylog_runtime_snapshot();
using keylog_snapshot_sink_t = std::function<void(
    std::shared_ptr<const keylog_runtime_snapshot_t> snapshot)>;
void set_keylog_snapshot_sink(keylog_snapshot_sink_t sink);
bool keylog_operation_pending();


artifact_identity_t exchange_artifact_identity(const mitm_proxy::http_exchange& exchange,
                                               artifact_kind_t kind);
bool execute_retained_exchange_toolbar_action(const char* action_id,
                                              artifact_identity_t primary,
                                              artifact_identity_t related,
                                              std::string& unavailable_reason);
void publish_network_selection(const artifact_identity_t& identity, bool force = false);
void clear_stale_network_selection(std::string_view source_view_id);

bool resolve_artifact(const artifact_identity_t& identity, artifact_snapshot_t& snapshot,
                      std::string& unavailable_reason);
std::uint64_t artifact_content_hash(const std::vector<std::uint8_t>& bytes);
bool send_artifact_to_repeater(const artifact_identity_t& identity, std::string& unavailable_reason);
bool send_artifact_to_comparer(const artifact_identity_t& identity, std::string& unavailable_reason);
bool add_artifact_to_chat(const artifact_identity_t& identity, std::string& unavailable_reason);
bool assign_artifact_to_agent(const artifact_identity_t& identity, std::string& unavailable_reason);
bool make_sitemap_artifact(std::uint64_t exchange_id, artifact_kind_t kind,
                           artifact_identity_t& identity, std::string& unavailable_reason);
void open_exchange_context(artifact_identity_t primary, artifact_identity_t related,
                           exchange_context_origin_t origin,
                           bool include_intercept_actions = false);

}
