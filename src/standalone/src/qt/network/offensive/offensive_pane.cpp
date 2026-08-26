#include "qt/network/offensive/offensive_pane.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <nlohmann/json.hpp>

#include "core/infra/executor.hpp"
#include "core/network/network_view.hpp"
#include "core/network/burp/offensive/api_security_engine.hpp"
#include "core/network/burp/offensive/auth_attack_engine.hpp"
#include "core/network/burp/offensive/business_logic_engine.hpp"
#include "core/network/burp/offensive/client_attack_engine.hpp"
#include "core/network/burp/offensive/fuzzing_engine.hpp"
#include "core/network/burp/offensive/js_analysis_engine.hpp"
#include "core/network/burp/offensive/recon_engine.hpp"
#include "core/network/burp/offensive/server_attack_engine.hpp"
#include "core/network/burp/offensive/sqli_engine.hpp"
#include "core/network/burp/offensive/xss_engine.hpp"
#include "helpers/diag_log.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/bounded_plain_text_edit.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"

namespace aida::qt::net {

namespace {

using json = nlohmann::json;

enum class offensive_workflow_kind_t : int {
    sqli_detect,
    sqli_fingerprint,
    xss_detect,
    xss_dom,
    auth_bruteforce,
    auth_idor,
    server_ssrf,
    server_ssti,
    server_cmdi,
    server_traversal,
    server_xxe,
    server_smuggle,
    api_param_fuzz,
    api_authz_matrix,
    client_cors,
    client_csrf,
    client_postmessage,
    business_race,
    fuzz_start,
    fuzz_mutate,
    js_secrets,
    js_endpoints,
    recon_fingerprint,
    recon_waf
};

struct offensive_workflow_t {
    const char* label;
    offensive_workflow_kind_t kind;
};

const offensive_workflow_t k_offensive_workflows[] = {
    { "SQLi Detect", offensive_workflow_kind_t::sqli_detect },
    { "SQLi Fingerprint", offensive_workflow_kind_t::sqli_fingerprint },
    { "XSS Detect", offensive_workflow_kind_t::xss_detect },
    { "XSS DOM", offensive_workflow_kind_t::xss_dom },
    { "Auth Brute Force", offensive_workflow_kind_t::auth_bruteforce },
    { "Auth IDOR", offensive_workflow_kind_t::auth_idor },
    { "SSRF", offensive_workflow_kind_t::server_ssrf },
    { "SSTI", offensive_workflow_kind_t::server_ssti },
    { "CMDi", offensive_workflow_kind_t::server_cmdi },
    { "Path Traversal", offensive_workflow_kind_t::server_traversal },
    { "XXE", offensive_workflow_kind_t::server_xxe },
    { "Request Smuggling", offensive_workflow_kind_t::server_smuggle },
    { "API Param Fuzz", offensive_workflow_kind_t::api_param_fuzz },
    { "API Auth Matrix", offensive_workflow_kind_t::api_authz_matrix },
    { "Client CORS", offensive_workflow_kind_t::client_cors },
    { "Client CSRF", offensive_workflow_kind_t::client_csrf },
    { "Client PostMessage", offensive_workflow_kind_t::client_postmessage },
    { "Business Race", offensive_workflow_kind_t::business_race },
    { "Fuzz Start", offensive_workflow_kind_t::fuzz_start },
    { "Fuzz Mutate", offensive_workflow_kind_t::fuzz_mutate },
    { "JS Secrets", offensive_workflow_kind_t::js_secrets },
    { "JS Endpoints", offensive_workflow_kind_t::js_endpoints },
    { "Recon Fingerprint", offensive_workflow_kind_t::recon_fingerprint },
    { "Recon WAF", offensive_workflow_kind_t::recon_waf }
};

int offensive_workflow_count() {
    return static_cast<int>(sizeof(k_offensive_workflows) / sizeof(k_offensive_workflows[0]));
}

int clamp_offensive_workflow_index(int idx) {
    return std::max(0, std::min(idx, offensive_workflow_count() - 1));
}

const offensive_workflow_t& offensive_workflow_at(int idx) {
    return k_offensive_workflows[clamp_offensive_workflow_index(idx)];
}

std::string lower_ascii_copy(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool offensive_sensitive_key(const std::string& key) {
    const std::string k = lower_ascii_copy(key);
    return k.find("password") != std::string::npos ||
           k.find("passwd") != std::string::npos ||
           k.find("pwd") != std::string::npos ||
           k.find("token") != std::string::npos ||
           k.find("secret") != std::string::npos ||
           k.find("cookie") != std::string::npos ||
           k.find("authorization") != std::string::npos ||
           k.find("api_key") != std::string::npos ||
           k.find("apikey") != std::string::npos ||
           k.find("private_key") != std::string::npos ||
           k.find("session") != std::string::npos ||
           k.find("credential") != std::string::npos ||
           k.find("bearer") != std::string::npos ||
           k == "jwt";
}

json offensive_redact_json(const json& src) {
    if (src.is_object()) {
        json out = json::object();
        std::string named_value_key;
        if (src.contains("name") && src["name"].is_string())
            named_value_key = src["name"].get<std::string>();
        for (auto it = src.begin(); it != src.end(); ++it) {
            if (offensive_sensitive_key(it.key()) || (it.key() == "value" && offensive_sensitive_key(named_value_key)))
                out[it.key()] = "[redacted]";
            else
                out[it.key()] = offensive_redact_json(it.value());
        }
        return out;
    }
    if (src.is_array()) {
        json out = json::array();
        for (const auto& item : src)
            out.push_back(offensive_redact_json(item));
        return out;
    }
    return src;
}

bool offensive_parse_json_object(const char* text, json& out, std::string& err) {
    std::string raw = text ? std::string(text) : std::string();
    if (raw.empty()) {
        out = json::object();
        return true;
    }
    bool all_space = true;
    for (char c : raw) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            all_space = false;
            break;
        }
    }
    if (all_space) {
        out = json::object();
        return true;
    }
    json parsed = json::parse(raw, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        err = "Payload JSON must be an object";
        return false;
    }
    out = std::move(parsed);
    return true;
}

bool offensive_json_bool(const json& obj, const char* primary, const char* secondary, bool fallback) {
    if (obj.is_object() && obj.contains(primary) && obj[primary].is_boolean())
        return obj[primary].get<bool>();
    if (obj.is_object() && obj.contains(secondary) && obj[secondary].is_boolean())
        return obj[secondary].get<bool>();
    return fallback;
}

struct offensive_run_result_t {
    bool success = false;
    std::string message;
    std::string code;
    json data = json::object();
};

offensive_run_result_t offensive_from_tool_result(const mcp_standalone::tool_result_t& tr) {
    offensive_run_result_t out;
    out.success = tr.success;
    out.message = tr.text;
    out.code = tr.error_code;
    if (!tr.data.is_null() && !(tr.data.is_object() && tr.data.empty()))
        out.data = tr.data;
    else if (!tr.error_details.is_null() && !(tr.error_details.is_object() && tr.error_details.empty()))
        out.data = tr.error_details;
    else
        out.data = json{{"text", tr.text}};
    return out;
}

offensive_run_result_t offensive_from_json_result(const json& result, const std::string& fallback_message) {
    offensive_run_result_t out;
    out.data = result;
    out.success = offensive_json_bool(result, "ok", "success", true);
    out.message = result.is_object() && result.contains("message") && result["message"].is_string()
        ? result["message"].get<std::string>() : fallback_message;
    out.code = result.is_object() && result.contains("error_code") && result["error_code"].is_string()
        ? result["error_code"].get<std::string>() : std::string();
    if (out.code.empty() && result.is_object() && result.contains("code") && result["code"].is_string())
        out.code = result["code"].get<std::string>();
    return out;
}

offensive_run_result_t offensive_from_auth_result(const aida::burp::offensive::auth_attack::result_t& r) {
    return offensive_run_result_t{r.success, r.message, r.error_code, r.data};
}

offensive_run_result_t offensive_from_business_result(const aida::burp::offensive::business_logic::result_t& r) {
    return offensive_run_result_t{r.success, r.message, r.error_code, r.data};
}

offensive_run_result_t offensive_from_server_result(const aida::burp::offensive::server_attack::action_result_t& r) {
    return offensive_run_result_t{r.success, r.message, r.code, r.data};
}

offensive_run_result_t offensive_from_sqli_result(const aida::burp::offensive::sqli::engine_result_t& r) {
    return offensive_run_result_t{r.ok, r.message, r.code, r.data};
}

offensive_run_result_t offensive_from_xss_result(const aida::burp::offensive::xss::engine_result_t& r) {
    return offensive_run_result_t{r.ok, r.message, r.code, r.data};
}

offensive_run_result_t offensive_dispatch(const offensive_workflow_t& workflow, json payload) {
    using namespace aida::burp::offensive;
    switch (workflow.kind) {
        case offensive_workflow_kind_t::sqli_detect:
            return offensive_from_sqli_result(sqli::detect(payload));
        case offensive_workflow_kind_t::sqli_fingerprint:
            return offensive_from_sqli_result(sqli::fingerprint_db(payload));
        case offensive_workflow_kind_t::xss_detect:
            return offensive_from_xss_result(xss::detect(payload));
        case offensive_workflow_kind_t::xss_dom:
            return offensive_from_xss_result(xss::dom_analyze(payload));
        case offensive_workflow_kind_t::auth_bruteforce:
            return offensive_from_auth_result(auth_attack::handle_action("brute_force", payload));
        case offensive_workflow_kind_t::auth_idor:
            return offensive_from_auth_result(auth_attack::handle_action("idor_test", payload));
        case offensive_workflow_kind_t::server_ssrf:
            return offensive_from_server_result(server_attack::handle_action("ssrf_exploit", payload));
        case offensive_workflow_kind_t::server_ssti:
            return offensive_from_server_result(server_attack::handle_action("ssti_exploit", payload));
        case offensive_workflow_kind_t::server_cmdi:
            return offensive_from_server_result(server_attack::handle_action("cmdi_exploit", payload));
        case offensive_workflow_kind_t::server_traversal:
            return offensive_from_server_result(server_attack::handle_action("path_traversal_exploit", payload));
        case offensive_workflow_kind_t::server_xxe:
            return offensive_from_server_result(server_attack::handle_action("xxe_exploit", payload));
        case offensive_workflow_kind_t::server_smuggle:
            return offensive_from_server_result(server_attack::handle_action("smuggle_exploit", payload));
        case offensive_workflow_kind_t::api_param_fuzz:
            return offensive_from_tool_result(api_security::param_fuzz(payload));
        case offensive_workflow_kind_t::api_authz_matrix:
            return offensive_from_tool_result(api_security::authz_matrix(payload));
        case offensive_workflow_kind_t::client_cors:
            return offensive_from_tool_result(client_attack::cors_exploit(payload));
        case offensive_workflow_kind_t::client_csrf:
            return offensive_from_tool_result(client_attack::csrf_test(payload));
        case offensive_workflow_kind_t::client_postmessage:
            return offensive_from_tool_result(client_attack::postmessage_scan(payload));
        case offensive_workflow_kind_t::business_race:
            return offensive_from_business_result(business_logic::handle_action("race_test", payload));
        case offensive_workflow_kind_t::fuzz_start:
            return offensive_from_tool_result(fuzzing::start(payload));
        case offensive_workflow_kind_t::fuzz_mutate:
            return offensive_from_tool_result(fuzzing::mutate(payload));
        case offensive_workflow_kind_t::js_secrets:
            return offensive_from_json_result(js_analysis::extract_secrets(payload), "JavaScript secret extraction completed");
        case offensive_workflow_kind_t::js_endpoints:
            return offensive_from_json_result(js_analysis::extract_endpoints(payload), "JavaScript endpoint extraction completed");
        case offensive_workflow_kind_t::recon_fingerprint:
            return offensive_from_json_result(recon::fingerprint(payload), "Recon fingerprint completed");
        case offensive_workflow_kind_t::recon_waf:
            return offensive_from_json_result(recon::waf_detect(payload), "Recon WAF detection completed");
    }
    return offensive_run_result_t{false, "Unsupported offensive workflow", "unsupported_workflow", json::object()};
}

void collect_offensive_issue_ids(const json& node, std::vector<uint64_t>& ids) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string key = lower_ascii_copy(it.key());
            if ((key == "issue_id" || key == "issue" || key == "id") && it.value().is_number_unsigned()) {
                const uint64_t id = it.value().get<uint64_t>();
                if (id != 0 && std::find(ids.begin(), ids.end(), id) == ids.end())
                    ids.push_back(id);
            }
            collect_offensive_issue_ids(it.value(), ids);
        }
    } else if (node.is_array()) {
        for (const auto& item : node)
            collect_offensive_issue_ids(item, ids);
    }
}

}

OffensivePane::OffensivePane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.offensive"));
    const auto& t = theme::tokens();

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(t.spacing.sm);
    toolbar->addWidget(new QLabel(QStringLiteral("Workflow:"), content));
    workflow_combo_ = new QComboBox(content);
    for (int i = 0; i < offensive_workflow_count(); ++i)
        workflow_combo_->addItem(QString::fromLatin1(k_offensive_workflows[i].label));
    toolbar->addWidget(workflow_combo_);
    scope_check_ = new QCheckBox(QStringLiteral("Scope"), content);
    scope_check_->setChecked(true);
    toolbar->addWidget(scope_check_);
    toolbar->addWidget(new QLabel(QStringLiteral("Timeout:"), content));
    timeout_spin_ = new QSpinBox(content);
    timeout_spin_->setRange(1000, 120000);
    timeout_spin_->setValue(15000);
    toolbar->addWidget(timeout_spin_);
    toolbar->addWidget(new QLabel(QStringLiteral("Payloads:"), content));
    payloads_spin_ = new QSpinBox(content);
    payloads_spin_->setRange(1, 256);
    payloads_spin_->setValue(16);
    toolbar->addWidget(payloads_spin_);
    toolbar->addWidget(new QLabel(QStringLiteral("Requests:"), content));
    requests_spin_ = new QSpinBox(content);
    requests_spin_->setRange(1, 1000);
    requests_spin_->setValue(32);
    toolbar->addWidget(requests_spin_);
    toolbar->addStretch(1);
    layout->addLayout(toolbar);

    auto* targetRow = new QHBoxLayout();
    targetRow->setSpacing(t.spacing.sm);
    targetRow->addWidget(new QLabel(QStringLiteral("Target:"), content));
    url_edit_ = new QLineEdit(content);
    url_edit_->setMaxLength(1023);
    url_edit_->setPlaceholderText(QStringLiteral("https://target.example/path?name=value"));
    targetRow->addWidget(url_edit_, 1);
    targetRow->addWidget(new QLabel(QStringLiteral("Param:"), content));
    param_edit_ = new QLineEdit(content);
    param_edit_->setMaxLength(127);
    param_edit_->setPlaceholderText(QStringLiteral("name"));
    targetRow->addWidget(param_edit_);
    layout->addLayout(targetRow);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);
    auto* payloadPanel = new QWidget(splitter);
    auto* payloadLayout = new QVBoxLayout(payloadPanel);
    payloadLayout->setContentsMargins(0, 0, 0, 0);
    payloadLayout->setSpacing(t.spacing.xs);
    auto* payloadTitle = new QLabel(QStringLiteral("Payload JSON"), payloadPanel);
    payloadTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    payloadLayout->addWidget(payloadTitle);
    payload_edit_ = new BoundedPlainTextEdit(8191, payloadPanel);
    payload_edit_->setFont(theme::fonts::codeRegular());
    payload_edit_->setPlainText(QStringLiteral("{}"));
    payloadLayout->addWidget(payload_edit_, 1);
    splitter->addWidget(payloadPanel);
    auto* rawPanel = new QWidget(splitter);
    auto* rawLayout = new QVBoxLayout(rawPanel);
    rawLayout->setContentsMargins(0, 0, 0, 0);
    rawLayout->setSpacing(t.spacing.xs);
    auto* rawTitle = new QLabel(QStringLiteral("Raw Request"), rawPanel);
    rawTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    rawLayout->addWidget(rawTitle);
    raw_editor_ = new QtHumanRequestEditor(rawPanel);
    QtHumanRequestEditor::Config rawConfig;
    rawConfig.stableId = QStringLiteral("offensive-raw-request");
    rawConfig.maxBytes = 32767;
    rawConfig.editable = true;
    rawConfig.allowEmpty = true;
    raw_editor_->setConfig(rawConfig);
    rawLayout->addWidget(raw_editor_, 1);
    splitter->addWidget(rawPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter);

    auto* runRow = new QHBoxLayout();
    runRow->setSpacing(t.spacing.sm);
    run_button_ = new widgets::AidaButton(QStringLiteral("Run"), content);
    run_button_->setKind(widgets::AidaButton::Kind::Primary);
    run_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    runRow->addWidget(run_button_);
    clear_button_ = new widgets::AidaButton(QStringLiteral("Clear"), content);
    clear_button_->setKind(widgets::AidaButton::Kind::Ghost);
    clear_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    runRow->addWidget(clear_button_);
    stop_job_button_ = new widgets::AidaButton(QStringLiteral("Stop Job"), content);
    stop_job_button_->setKind(widgets::AidaButton::Kind::Destructive);
    stop_job_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    stop_job_button_->setVisible(false);
    runRow->addWidget(stop_job_button_);
    cancel_button_ = new widgets::AidaButton(QStringLiteral("Cancel"), content);
    cancel_button_->setKind(widgets::AidaButton::Kind::Ghost);
    cancel_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    cancel_button_->setEnabled(false);
    cancel_button_->setVisible(false);
    cancel_button_->setToolTip(QStringLiteral(
        "This backend operation has no cooperative cancellation contract. It must finish before another workflow can start."));
    runRow->addWidget(cancel_button_);
    run_state_label_ = new QLabel(content);
    run_state_label_->setProperty("aidaTone", QStringLiteral("titleAccent"));
    run_state_label_->setVisible(false);
    runRow->addWidget(run_state_label_);
    status_label_ = new QLabel(QStringLiteral("Idle"), content);
    status_label_->setProperty("aidaTone", QStringLiteral("dim"));
    runRow->addWidget(status_label_, 1);
    layout->addLayout(runRow);

    issue_row_host_ = new QWidget(content);
    issue_row_ = new QHBoxLayout(issue_row_host_);
    issue_row_->setContentsMargins(0, 0, 0, 0);
    issue_row_->setSpacing(t.spacing.sm);
    issue_row_host_->setVisible(false);
    layout->addWidget(issue_row_host_);

    result_view_ = new QPlainTextEdit(content);
    result_view_->setReadOnly(true);
    result_view_->setFont(theme::fonts::codeRegular());
    result_view_->setPlaceholderText(QStringLiteral(
        "Run a workflow to see its result JSON here"));
    layout->addWidget(result_view_, 1);

    connect(run_button_, &QAbstractButton::clicked, this, [this] { runClicked(); });
    connect(clear_button_, &QAbstractButton::clicked, this, [this] { clearClicked(); });
    connect(stop_job_button_, &QAbstractButton::clicked, this, [this] { stopJobClicked(); });
    connect(raw_editor_, &QtHumanRequestEditor::validityChanged, this,
        [this](bool, const QString&) { refreshRunControls(); });
    connect(raw_editor_, &QtHumanRequestEditor::hasUnappliedPrettyChanged, this,
        [this](bool) { refreshRunControls(); });

    refreshRunControls();
    setContent(content);
}

void OffensivePane::refreshRunControls() {
    const bool running = running_.load(std::memory_order_acquire);
    run_button_->setVisible(!running);
    clear_button_->setVisible(!running);
    run_state_label_->setVisible(running);
    cancel_button_->setVisible(running);
    if (running) {
        run_state_label_->setText(QStringLiteral("Running"));
        status_label_->setText(QString::fromStdString(status_));
        return;
    }
    const bool rawPresent = !raw_editor_->authority().isEmpty();
    const bool runDisabled = rawPresent &&
        (!raw_editor_->isValid() || raw_editor_->hasUnappliedPretty());
    run_button_->setEnabled(!runDisabled);
    stop_job_button_->setVisible(active_fuzz_job_id_.load(std::memory_order_acquire) != 0);
    status_label_->setText(QString::fromStdString(status_));
}

void OffensivePane::runClicked() {
    const auto& workflow = offensive_workflow_at(workflow_combo_->currentIndex());
    const int timeoutMs = std::max(1000, std::min(timeout_spin_->value(), 120000));
    const int maxPayloads = std::max(1, std::min(payloads_spin_->value(), 256));
    const int maxRequests = std::max(1, std::min(requests_spin_->value(), 1000));
    timeout_spin_->setValue(timeoutMs);
    payloads_spin_->setValue(maxPayloads);
    requests_spin_->setValue(maxRequests);
    diag::log_tagged_fmt("network", "offensive_run_clicked workflow=%s scope_only=%d timeout_ms=%d max_payloads=%d max_requests=%d url_len=%zu raw_len=%zu",
        workflow.label, scope_check_->isChecked() ? 1 : 0, timeoutMs, maxPayloads, maxRequests,
        url_edit_->text().toStdString().size(), raw_editor_->authority().toStdString().size());

    json payload;
    std::string err;
    const std::string payloadText = payload_edit_->toPlainText().toStdString();
    if (!offensive_parse_json_object(payloadText.c_str(), payload, err)) {
        status_ = err;
        result_ = json{{"success", false}, {"error", err}}.dump(2);
        result_view_->setPlainText(QString::fromStdString(result_));
        refreshRunControls();
        return;
    }
    const std::string url = url_edit_->text().toStdString();
    const std::string param = param_edit_->text().toStdString();
    const std::string raw = raw_editor_->authority().toStdString();
    const bool scopeOnly = scope_check_->isChecked();
    if (!url.empty()) {
        payload["url"] = url;
        if (!payload.contains("base_url"))
            payload["base_url"] = url;
    }
    if (!param.empty()) {
        payload["param"] = param;
        payload["param_target"] = param;
        payload["param_name"] = param;
    }
    if (!raw.empty() && !payload.contains("raw_request"))
        payload["raw_request"] = raw;
    payload["scope_only"] = scopeOnly;
    payload["enforce_scope"] = scopeOnly;
    payload["timeout_ms"] = timeoutMs;
    payload["max_payloads"] = maxPayloads;
    payload["max_requests"] = maxRequests;
    if (!payload.contains("max_attempts"))
        payload["max_attempts"] = maxRequests;
    if (!payload.contains("request_count"))
        payload["request_count"] = maxRequests;
    if (!payload.contains("max_params"))
        payload["max_params"] = maxPayloads;
    if (!payload.contains("max_variants"))
        payload["max_variants"] = maxPayloads;
    if (!payload.contains("max_payloads_per_set"))
        payload["max_payloads_per_set"] = maxPayloads;

    const std::uint64_t runId = run_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
    running_.store(true, std::memory_order_release);
    status_ = std::string("Running ") + workflow.label;
    result_.clear();
    result_view_->clear();
    issue_row_host_->setVisible(false);
    refreshRunControls();

    const int workflowIndex = clamp_offensive_workflow_index(workflow_combo_->currentIndex());
    QPointer<OffensivePane> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = "offensive_workflow";
    submission.thread_class = "long_running";
    submission.domain = aida::infra::executor::domain_t::long_running;
    submission.priority = 3;
    submission.body = [pane, payload = std::move(payload), workflowIndex, runId]() mutable {
        const offensive_workflow_t workflowCopy = offensive_workflow_at(workflowIndex);
        offensive_run_result_t result;
        const std::uint64_t begin = static_cast<std::uint64_t>(GetTickCount64());
        try {
            result = offensive_dispatch(workflowCopy, std::move(payload));
        } catch (const std::exception& e) {
            result.success = false;
            result.message = "Offensive workflow failed";
            result.code = "exception";
            result.data = json{{"exception_len", std::string(e.what()).size()}};
        } catch (...) {
            result.success = false;
            result.message = "Offensive workflow failed";
            result.code = "unknown_exception";
            result.data = json::object();
        }
        const std::uint64_t elapsedMs = static_cast<std::uint64_t>(GetTickCount64()) - begin;
        json out;
        out["run_id"] = runId;
        out["workflow"] = workflowCopy.label;
        out["success"] = result.success;
        out["message"] = result.message;
        out["code"] = result.code;
        out["elapsed_ms"] = elapsedMs;
        out["data"] = offensive_redact_json(result.data);
        std::uint64_t fuzzJobId = 0;
        if (workflowCopy.kind == offensive_workflow_kind_t::fuzz_start && result.success &&
            result.data.is_object()) {
            if (result.data.contains("job_id") && result.data["job_id"].is_number_unsigned())
                fuzzJobId = result.data["job_id"].get<std::uint64_t>();
        }
        const std::string statusText = result.message.empty()
            ? (result.success ? "Completed" : "Failed") : result.message;
        const std::string resultText = out.dump(2);
        if (pane) {
            if (fuzzJobId != 0)
                pane->active_fuzz_job_id_.store(fuzzJobId, std::memory_order_release);
            QMetaObject::invokeMethod(pane.data(),
                [pane, runId, statusText, resultText = std::move(resultText)]() {
                    if (pane->run_id_.load(std::memory_order_acquire) != runId)
                        return;
                    pane->status_ = statusText;
                    pane->result_ = resultText;
                    pane->running_.store(false, std::memory_order_release);
                    pane->result_view_->setPlainText(QString::fromStdString(pane->result_));
                    pane->rebuildIssueLinks(pane->result_);
                    pane->refreshRunControls();
                }, Qt::QueuedConnection);
        }
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        running_.store(false, std::memory_order_release);
        status_ = "Network work queue unavailable";
        result_ = json{{"success", false}, {"error", "network_executor_unavailable"}}.dump(2);
        result_view_->setPlainText(QString::fromStdString(result_));
        refreshRunControls();
    }
}

void OffensivePane::clearClicked() {
    status_ = "Idle";
    result_.clear();
    result_view_->clear();
    issue_row_host_->setVisible(false);
    refreshRunControls();
}

void OffensivePane::stopJobClicked() {
    const std::uint64_t jobId = active_fuzz_job_id_.exchange(0, std::memory_order_acq_rel);
    if (jobId == 0)
        return;
    status_ = "Stopping fuzz job";
    refreshRunControls();
    QPointer<OffensivePane> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = "offensive_fuzz_stop";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 3;
    submission.body = [pane, jobId]() {
        auto result = aida::burp::offensive::fuzzing::stop(json{{"job_id", jobId}});
        json out;
        out["workflow"] = "Fuzz Stop";
        out["success"] = result.success;
        out["message"] = result.text;
        out["data"] = offensive_redact_json(result.data);
        const std::string statusText = result.success ? "Fuzz job stopped" : result.text;
        const std::string resultText = out.dump(2);
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(),
            [pane, statusText, resultText = std::move(resultText)]() {
                pane->status_ = statusText;
                pane->result_ = resultText;
                pane->result_view_->setPlainText(QString::fromStdString(pane->result_));
                pane->refreshRunControls();
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        status_ = "Network work queue unavailable";
        refreshRunControls();
    }
}

void OffensivePane::rebuildIssueLinks(const std::string& resultJson) {
    while (auto* item = issue_row_->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    json parsed = json::parse(resultJson, nullptr, false);
    if (parsed.is_discarded()) {
        issue_row_host_->setVisible(false);
        return;
    }
    std::vector<uint64_t> ids;
    collect_offensive_issue_ids(parsed, ids);
    if (ids.empty()) {
        issue_row_host_->setVisible(false);
        return;
    }
    issue_row_->addWidget(new QLabel(QStringLiteral("Issues:"), issue_row_host_));
    for (std::size_t i = 0; i < ids.size() && i < 8; ++i) {
        auto* button = new widgets::AidaButton(
            QStringLiteral("#%1").arg(static_cast<unsigned long long>(ids[i])), issue_row_host_);
        button->setKind(widgets::AidaButton::Kind::Ghost);
        button->setControlSize(widgets::AidaButton::ControlSize::Small);
        button->setToolTip(QStringLiteral("Open the Scanner view for this issue"));
        issue_row_->addWidget(button);
        connect(button, &QAbstractButton::clicked, this, [] {
            (void)network_view::open_view("view.network.scanner");
        });
    }
    issue_row_->addStretch(1);
    issue_row_host_->setVisible(true);
}

}
