#include "qt/auth/qt_auth_view.hpp"

#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPainter>
#include <QPushButton>
#include <QStackedLayout>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "core/ai/provider_catalog.hpp"
#include "core/ai/provider_transforms.hpp"
#include "core/ai/standalone_chat.hpp"
#include "core/auth/auth_browser_launch.hpp"
#include "core/auth/auth_http.hpp"
#include "core/infra/executor.hpp"
#include "core/infra/win_thread.hpp"
#include "core/settings/settings_persistence_service.hpp"
#include "core/settings/standalone_settings.hpp"
#include "helpers/diag_log.hpp"
#include "qt/ai/qt_ai_chat_dialogs.hpp"
#include "qt/auth/qt_auth_oauth_dialog.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_icons.hpp"
#include "qt/theme/aida_tokens.hpp"

extern settings_sa_t g_sa_settings;

namespace aida::qt::auth {

namespace {

constexpr std::uint64_t kLoginStartDeadlineMs =
    aida::auth::kBrowserExternalOperationDeadlineMs;
constexpr std::uint64_t kLoginCancelDeadlineMs = 35000;
constexpr std::uint64_t kProviderValidationDeadlineMs = 16000;

int64_t now_unix() {
    return static_cast<int64_t>(std::time(nullptr));
}

std::string canonical_provider_key(const std::string& provider_id) {
    std::string out;
    out.reserve(provider_id.size());
    for (const char raw_ch : provider_id) {
        const auto ch = static_cast<unsigned char>(raw_ch);
        if (ch == '\\')
            out.push_back('/');
        else
            out.push_back(ch >= 'A' && ch <= 'Z'
                ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch));
    }
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    return out;
}

bool auth_info_authenticated(const aida::auth::auth_info_t& info, int64_t now) {
    if (info.kind == aida::auth::auth_kind_t::none)
        return false;
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

std::shared_ptr<const std::unordered_map<std::string, bool>>& auth_snapshot_ref() {
    static std::shared_ptr<const std::unordered_map<std::string, bool>> s;
    return s;
}

std::atomic<bool>& auth_snapshot_refreshing() {
    static std::atomic<bool> b{false};
    return b;
}

std::atomic<int64_t>& auth_snapshot_last_unix() {
    static std::atomic<int64_t> v{0};
    return v;
}

std::string format_relative_time(int64_t expires_unix) {
    int64_t now = now_unix();
    int64_t diff = expires_unix - now;
    if (diff <= 0)
        return "expired";
    if (diff < 60)
        return "expires in " + std::to_string(diff) + "s";
    if (diff < 3600)
        return "expires in " + std::to_string(diff / 60) + " min";
    if (diff < 86400) {
        int64_t hours = diff / 3600;
        int64_t mins = (diff % 3600) / 60;
        return "expires in " + std::to_string(hours) + "h " + std::to_string(mins) + "m";
    }
    int64_t days = diff / 86400;
    return "expires in " + std::to_string(days) + " day" + (days == 1 ? "" : "s");
}

QString mask_key(const std::string& k) {
    if (k.size() <= 8)
        return QString(static_cast<int>(k.size()), QLatin1Char('*'));
    return QString::fromStdString(k.substr(0, 4) +
        std::string(k.size() - 8, '*') + k.substr(k.size() - 4));
}

const char* login_start_result_name(int result) {
    switch (result) {
    case 0: return "idle";
    case 1: return "pending";
    case 2: return "succeeded";
    case 3: return "failed";
    case 4: return "cancelled";
    case 5: return "timed_out";
    case 6: return "rejected";
    default: return "unknown";
    }
}

std::uint64_t bounded_deadline(std::uint64_t budget_ms) {
    const std::uint64_t now = aida::infra::executor::now_ms();
    return now > (std::numeric_limits<std::uint64_t>::max)() - budget_ms
        ? (std::numeric_limits<std::uint64_t>::max)()
        : now + budget_ms;
}

std::string browser_open_failure_message(int result) {
    using aida::auth::browser_open_result_t;
    switch (static_cast<browser_open_result_t>(result)) {
    case browser_open_result_t::queued:
        return {};
    case browser_open_result_t::invalid_url:
        return "The browser URL was rejected";
    case browser_open_result_t::queue_rejected:
        return "The Camoufox request queue is unavailable";
    case browser_open_result_t::cancelled:
        return "The Camoufox request was cancelled";
    case browser_open_result_t::deadline_expired:
        return "The Camoufox request timed out";
    case browser_open_result_t::ensure_ready_failed:
        return "Camoufox could not become ready";
    case browser_open_result_t::navigate_failed:
        return "Camoufox could not open the requested page";
    case browser_open_result_t::opened:
    default:
        return {};
    }
}

std::string extract_error_from_body(const std::string& body) {
    if (body.empty())
        return std::string();
    try {
        auto json = nlohmann::json::parse(body);
        if (json.is_object()) {
            if (json.contains("error")) {
                const auto& e = json["error"];
                if (e.is_string())
                    return e.get<std::string>();
                if (e.is_object()) {
                    if (e.contains("message") && e["message"].is_string())
                        return e["message"].get<std::string>();
                    if (e.contains("code") && e["code"].is_string())
                        return e["code"].get<std::string>();
                }
            }
            if (json.contains("message") && json["message"].is_string())
                return json["message"].get<std::string>();
        }
    } catch (...) {
    }
    std::string snippet = body.substr(0, 200);
    for (char& c : snippet)
        if (c == '\n' || c == '\r')
            c = ' ';
    return snippet;
}

std::string encode_query_value(const std::string& input) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(input.size() * 3);
    for (const char raw_value : input) {
        const auto value = static_cast<unsigned char>(raw_value);
        if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9') || value == '-' || value == '.'
            || value == '_' || value == '~') {
            output.push_back(static_cast<char>(value));
        } else {
            output.push_back('%');
            output.push_back(hex[value >> 4]);
            output.push_back(hex[value & 0x0F]);
        }
    }
    return output;
}

std::vector<const aida::provider::model_info_t*> sorted_models_for(
    const std::string& provider_id) {
    std::vector<const aida::provider::model_info_t*> out;
    const auto* prov = aida::provider::catalog::get_provider(provider_id);
    if (!prov)
        return out;
    out.reserve(prov->model_ids.size());
    for (const auto& mid : prov->model_ids) {
        const auto* m = aida::provider::catalog::get_model(provider_id, mid);
        if (m && m->status != aida::provider::model_info_t::status_t::deprecated)
            out.push_back(m);
    }
    std::sort(out.begin(), out.end(),
        [](const aida::provider::model_info_t* a, const aida::provider::model_info_t* b) {
            const double ca = a->cost.input_per_million + a->cost.output_per_million;
            const double cb = b->cost.input_per_million + b->cost.output_per_million;
            if (ca != cb)
                return ca < cb;
            return a->id < b->id;
        });
    return out;
}

QString format_cost_pair(double in_per_m, double out_per_m) {
    char buf[96];
    if (in_per_m <= 0.0 && out_per_m <= 0.0)
        std::snprintf(buf, sizeof(buf), "free");
    else
        std::snprintf(buf, sizeof(buf), "$%.2f / $%.2f per M", in_per_m, out_per_m);
    return QString::fromLatin1(buf);
}

QString format_context_pretty(int64_t context) {
    if (context <= 0)
        return QStringLiteral("ctx ?");
    char buf[32];
    if (context >= 1000)
        std::snprintf(buf, sizeof(buf), "ctx %lldK", static_cast<long long>(context / 1000));
    else
        std::snprintf(buf, sizeof(buf), "ctx %lld", static_cast<long long>(context));
    return QString::fromLatin1(buf);
}

QColor provider_glyph_color(const QString& provider_id) {
    const auto& t = theme::tokens();
    if (provider_id == QLatin1String("anthropic"))
        return t.warning;
    if (provider_id == QLatin1String("openai"))
        return t.success;
    if (provider_id == QLatin1String("github-copilot"))
        return t.info;
    return t.accent;
}

void repolish_variant(QWidget* widget) {
    if (!widget)
        return;
    if (QStyle* style = widget->style()) {
        style->unpolish(widget);
        style->polish(widget);
    }
}

}

AidaAuthViewModel::sensitive_validation_key_t::~sensitive_validation_key_t() noexcept {
    if (!value.empty())
        SecureZeroMemory(value.data(), value.size());
}

AidaAuthViewModel::AidaAuthViewModel(QObject* parent) : QObject(parent) {
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(100);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, &AidaAuthViewModel::onPollTick);
}

AidaAuthViewModel& AidaAuthViewModel::instance() {
    static AidaAuthViewModel* model = [] {
        auto* created = new AidaAuthViewModel();
        return created;
    }();
    return *model;
}

void AidaAuthViewModel::setErrLocked(const std::string& msg) {
    err_ = msg;
}

bool AidaAuthViewModel::anyLoginInProgress() const {
    return codex_flow_open_.load() || copilot_flow_open_.load() ||
        claude_code_flow_open_.load();
}

QString AidaAuthViewModel::lastError() {
    std::lock_guard<std::mutex> lk(mtx_);
    return QString::fromStdString(err_);
}

bool AidaAuthViewModel::isProviderAuthenticated(const std::string& provider_id) {
    if (provider_id.empty())
        return false;
    const int64_t last = auth_snapshot_last_unix().load(std::memory_order_acquire);
    if (last == 0 || now_unix() - last >= 5)
        scheduleAuthSnapshotRefresh(false);
    auto snapshot = std::atomic_load_explicit(&auth_snapshot_ref(), std::memory_order_acquire);
    if (!snapshot)
        return false;
    auto it = snapshot->find(canonical_provider_key(provider_id));
    if (it != snapshot->end())
        return it->second;
    it = snapshot->find(provider_id);
    if (it != snapshot->end())
        return it->second;
    return false;
}

void AidaAuthViewModel::publishAuthSnapshot(
    const std::vector<std::pair<std::string, aida::auth::auth_info_t>>& entries) {
    auto snapshot = std::make_shared<std::unordered_map<std::string, bool>>();
    snapshot->reserve(entries.size() * 2 + 8);
    const int64_t now = now_unix();
    for (const auto& kv : entries) {
        const bool authed = auth_info_authenticated(kv.second, now);
        (*snapshot)[kv.first] = authed;
        (*snapshot)[canonical_provider_key(kv.first)] = authed;
    }
    std::atomic_store_explicit(&auth_snapshot_ref(),
        std::static_pointer_cast<const std::unordered_map<std::string, bool>>(snapshot),
        std::memory_order_release);
    auth_snapshot_last_unix().store(now, std::memory_order_release);
}

void AidaAuthViewModel::scheduleAuthSnapshotRefresh(bool force) {
    if (shutdown_flag_.load(std::memory_order_acquire))
        return;
    const int64_t now = now_unix();
    const int64_t last = auth_snapshot_last_unix().load(std::memory_order_acquire);
    if (!force && last > 0 && now >= last && now - last < 5)
        return;
    bool expected = false;
    if (!auth_snapshot_refreshing().compare_exchange_strong(expected, true,
            std::memory_order_acq_rel))
        return;
    const uint64_t start_ms = GetTickCount64();
    aida::infra::executor::submission_t sub;
    sub.owner_subsystem = "auth_view";
    sub.label = "auth.snapshot_refresh";
    sub.thread_class = "service_task";
    sub.domain = aida::infra::executor::domain_t::security_liveness;
    sub.priority = 1;
    sub.body = [this, start_ms]() {
        bool success = false;
        size_t provider_count = 0;
        try {
            std::vector<std::pair<std::string, aida::auth::auth_info_t>> entries;
            if (aida::auth::store::all(entries)) {
                provider_count = entries.size();
                publishAuthSnapshot(entries);
                success = true;
            }
        } catch (...) {
        }
        auth_snapshot_refreshing().store(false, std::memory_order_release);
        const uint64_t elapsed_ms = GetTickCount64() - start_ms;
        if (!success || elapsed_ms >= 250) {
            diag::log_tagged_fmt("auth",
                "auth_snapshot_refresh_done ok=%d providers=%zu elapsed_ms=%llu",
                success ? 1 : 0,
                provider_count,
                static_cast<unsigned long long>(elapsed_ms));
        }
        QMetaObject::invokeMethod(this, [this] {
            Q_EMIT authStateChanged();
        }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(sub)).submitted) {
        auth_snapshot_refreshing().store(false, std::memory_order_release);
    }
}

provider_status_snapshot_t AidaAuthViewModel::providerStatus(const std::string& provider_id) {
    provider_status_snapshot_t s;
    aida::auth::auth_info_t info;
    const bool present = aida::auth::store::get(provider_id, info);
    if (!present || info.kind == aida::auth::auth_kind_t::none) {
        s.label = QStringLiteral("Not connected");
        s.severity = 0;
        return s;
    }
    const int64_t now = now_unix();
    if (info.kind == aida::auth::auth_kind_t::oauth) {
        if (info.expires_unix > 0 && info.expires_unix <= now) {
            s.label = QStringLiteral("Token expired");
            s.severity = 2;
            s.expired = true;
            s.detail = QString::fromStdString(
                info.email.empty() ? info.account_id : info.email);
            return s;
        }
        s.label = info.expires_unix > 0
            ? QString::fromStdString(format_relative_time(info.expires_unix))
            : QStringLiteral("OAuth");
        s.severity = 1;
        s.authenticated = true;
        s.detail = QString::fromStdString(info.email.empty() ? info.account_id : info.email);
        if (s.detail.isEmpty())
            s.detail = QStringLiteral("Signed in");
        return s;
    }
    if (info.kind == aida::auth::auth_kind_t::api && !info.api_key.empty()) {
        s.label = QStringLiteral("API key set");
        s.severity = 1;
        s.authenticated = true;
        s.detail = mask_key(info.api_key);
        return s;
    }
    if (info.kind == aida::auth::auth_kind_t::wellknown) {
        s.label = QStringLiteral("Well-known");
        s.severity = 3;
        s.authenticated = true;
        s.detail = QStringLiteral("Configured");
        return s;
    }
    s.label = QStringLiteral("Connected");
    s.severity = 1;
    s.authenticated = true;
    return s;
}

template <typename State>
void AidaAuthViewModel::publishLoginStartFailure(const std::shared_ptr<State>& state,
                                                 const char* error) noexcept {
    if (!state || !error || !*error)
        return;
    bool phase_claimed = false;
    try {
        std::lock_guard<std::mutex> state_lock(state->mutex);
        std::uint8_t expected_phase = 0;
        phase_claimed = state->terminal_phase.compare_exchange_strong(
            expected_phase, 2, std::memory_order_acq_rel, std::memory_order_acquire);
        if (!phase_claimed)
            return;
        try { state->error = error; } catch (...) {}
        state->done.store(true, std::memory_order_release);
    } catch (...) {
        if (phase_claimed)
            state->done.store(true, std::memory_order_release);
    }
    if (!phase_claimed)
        return;
    try {
        std::lock_guard<std::mutex> view_lock(mtx_);
        setErrLocked(error);
    } catch (...) {
    }
}

template <typename State>
std::shared_ptr<State> AidaAuthViewModel::allocateLoginState(const char* error) noexcept {
    try {
        return std::make_shared<State>();
    } catch (...) {
        try {
            std::lock_guard<std::mutex> view_lock(mtx_);
            setErrLocked(error ? error : "Login state allocation failed");
        } catch (...) {
        }
        try {
            chrome::toast_error(QString::fromLatin1(
                error ? error : "Login state allocation failed"), 6.0);
        } catch (...) {
        }
        return {};
    }
}

bool AidaAuthViewModel::codexFlowCurrentLocked(
    const std::shared_ptr<aida::auth::codex::codex_login_state_t>& state,
    std::uint64_t generation) noexcept {
    return !shutdown_flag_.load(std::memory_order_acquire)
        && codex_flow_open_.load(std::memory_order_acquire)
        && codex_flow_generation_.load(std::memory_order_acquire) == generation
        && codex_state_ == state;
}

bool AidaAuthViewModel::copilotFlowCurrentLocked(
    const std::shared_ptr<aida::auth::copilot::copilot_login_state_t>& state,
    std::uint64_t generation) noexcept {
    return !shutdown_flag_.load(std::memory_order_acquire)
        && copilot_flow_open_.load(std::memory_order_acquire)
        && copilot_flow_generation_.load(std::memory_order_acquire) == generation
        && copilot_state_ == state;
}

bool AidaAuthViewModel::claudeFlowCurrentLocked(
    const std::shared_ptr<aida::auth::claude_code::claude_code_login_state_t>& state,
    std::uint64_t generation) noexcept {
    return !shutdown_flag_.load(std::memory_order_acquire)
        && claude_code_flow_open_.load(std::memory_order_acquire)
        && claude_code_flow_generation_.load(std::memory_order_acquire) == generation
        && claude_code_state_ == state;
}

template <typename State, typename StartFn, typename CancelFn, typename ErrorFn>
bool AidaAuthViewModel::submitLoginStartTask(const char* label, const char* provider,
    login_start_control_t& control, const std::shared_ptr<State>& state,
    const std::shared_ptr<State>& previous, StartFn start_fn, CancelFn cancel_fn,
    ErrorFn error_fn) {
    std::shared_ptr<login_start_ticket_t> ticket;
    try {
        ticket = std::make_shared<login_start_ticket_t>();
    } catch (...) {
        publishLoginStartFailure(state, "Login startup state allocation failed");
        control.result.store(static_cast<int>(login_start_result_t::rejected),
            std::memory_order_release);
        control.completion_pending.store(true, std::memory_order_release);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(control.mutex);
        if (control.current || control.active.load(std::memory_order_acquire))
            return false;
        ticket->generation = control.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        control.current = ticket;
        control.active.store(true, std::memory_order_release);
    }

    const std::uint64_t generation = ticket->generation;
    const std::uint64_t deadline = bounded_deadline(kLoginStartDeadlineMs);
    ticket->deadline_ms = deadline;
    control.completion_pending.store(false, std::memory_order_release);
    control.result.store(static_cast<int>(login_start_result_t::pending),
        std::memory_order_release);
    control.deadline_ms.store(deadline, std::memory_order_release);

    auto finish = [this, state, ticket, &control, provider](
        login_start_result_t result, const char* error, bool worker_finished) noexcept {
        try {
            bool expected_terminal = false;
            if (ticket->terminal_published.compare_exchange_strong(expected_terminal, true,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
                bool current = false;
                {
                    std::lock_guard<std::mutex> lock(control.mutex);
                    current = control.current == ticket
                        && control.generation.load(std::memory_order_acquire)
                            == ticket->generation;
                    if (current) {
                        control.result.store(static_cast<int>(result),
                            std::memory_order_release);
                        control.deadline_ms.store(0, std::memory_order_release);
                        control.completion_pending.store(true, std::memory_order_release);
                    }
                }
                if (current && (result == login_start_result_t::failed
                    || result == login_start_result_t::timed_out
                    || result == login_start_result_t::rejected))
                    publishLoginStartFailure(state, error);
            }
            if (worker_finished || !ticket->worker_started.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(control.mutex);
                if (control.current == ticket) {
                    control.current.reset();
                    control.active.store(false, std::memory_order_release);
                }
            }
            diag::log_tagged_fmt("auth",
                "AUTH-LOGIN-START-PUBLISH provider=%s generation=%llu result=%s worker_finished=%d error_len=%zu",
                provider, static_cast<unsigned long long>(ticket->generation),
                login_start_result_name(static_cast<int>(result)),
                worker_finished ? 1 : 0,
                error ? std::strlen(error) : 0);
        } catch (...) {
            if (worker_finished) {
                try {
                    std::lock_guard<std::mutex> lock(control.mutex);
                    if (control.current == ticket) {
                        control.current.reset();
                        control.active.store(false, std::memory_order_release);
                    }
                } catch (...) {
                }
            }
        }
        QMetaObject::invokeMethod(this, [this] { armPollTimer(); }, Qt::QueuedConnection);
    };
    std::shared_ptr<CancelFn> cancel_operation;
    try {
        cancel_operation = std::make_shared<CancelFn>(std::move(cancel_fn));
    } catch (...) {
        finish(login_start_result_t::rejected, "Login cancellation state allocation failed",
            false);
        return false;
    }
    auto invoke_cancel = [state, ticket, cancel_operation]() noexcept {
        bool expected = false;
        if (!state || !cancel_operation
            || !ticket->provider_cancel_invoked.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return;
        try {
            const std::function<void()> guarded_cancel = [&]() {
                (*cancel_operation)(*state);
            };
            aida::infra::win_thread::run_function_seh_guarded(guarded_cancel);
        } catch (...) {
        }
    };

    aida::infra::executor::submit_result_t submitted;
    try {
        aida::infra::executor::submission_t sub;
        sub.owner_subsystem = "auth_provider";
        sub.label = label;
        sub.thread_class = "security_liveness";
        sub.domain = aida::infra::executor::domain_t::security_liveness;
        sub.priority = 1;
        sub.deadline_ms = deadline;
        sub.generation = generation;
        sub.ui_access_policy = "none";
        sub.failure_policy = "publish_typed_failure";
        sub.shutdown_policy = "cancel_pending";
        sub.cancel_hook = [state, ticket, deadline, finish, invoke_cancel]() mutable noexcept {
            ticket->cancellation_requested.store(true, std::memory_order_release);
            if (state)
                state->cancelled.store(true, std::memory_order_release);
            const login_start_result_t result = aida::infra::executor::now_ms() >= deadline
                ? login_start_result_t::timed_out
                : login_start_result_t::cancelled;
            finish(result,
                result == login_start_result_t::timed_out
                    ? "Login startup timed out"
                    : "Login startup cancelled",
                false);
            invoke_cancel();
        };
        sub.body = [this, state, previous, start_fn = std::move(start_fn),
            cancel_operation, error_fn = std::move(error_fn),
            ticket, finish, invoke_cancel]() mutable noexcept {
            ticket->worker_started.store(true, std::memory_order_release);
            bool ok = false;
            std::string error;
            const char* fallback_error = nullptr;
            login_start_result_t result = login_start_result_t::failed;
            struct terminal_guard_t {
                decltype(finish)* callback;
                login_start_result_t* result;
                std::string* error;
                const char** fallback_error;
                ~terminal_guard_t() noexcept {
                    (*callback)(*result, *fallback_error ? *fallback_error
                        : (error->empty() ? nullptr : error->c_str()), true);
                }
            } terminal{&finish, &result, &error, &fallback_error};
            try {
                const std::function<void()> guarded = [&]() {
                    if (previous)
                        (*cancel_operation)(*previous);
                    if (!state) {
                        error = "Login startup state is unavailable";
                        return;
                    }
                    if (shutdown_flag_.load(std::memory_order_acquire)
                        || ticket->cancellation_requested.load(std::memory_order_acquire)
                        || state->cancelled.load(std::memory_order_acquire)) {
                        result = login_start_result_t::cancelled;
                        error = "Login startup cancelled";
                        return;
                    }
                    ok = start_fn(*state, ticket->deadline_ms);
                    if (ticket->cancellation_requested.load(std::memory_order_acquire)) {
                        state->cancelled.store(true, std::memory_order_release);
                        invoke_cancel();
                        result = login_start_result_t::cancelled;
                        return;
                    }
                    if (!ok)
                        error = error_fn();
                };
                const DWORD seh = aida::infra::win_thread::run_function_seh_guarded(guarded);
                if (seh != 0) {
                    ok = false;
                    char buffer[96];
                    std::snprintf(buffer, sizeof(buffer),
                        "Login startup raised SEH 0x%08lX",
                        static_cast<unsigned long>(seh));
                    error = buffer;
                    result = login_start_result_t::failed;
                }
            } catch (...) {
                ok = false;
                fallback_error = "Login startup exception";
                result = login_start_result_t::failed;
            }
            if (ticket->cancellation_requested.load(std::memory_order_acquire)
                || (state && state->cancelled.load(std::memory_order_acquire))) {
                result = login_start_result_t::cancelled;
            } else if (ok) {
                result = login_start_result_t::succeeded;
            }
            if (!ok && error.empty() && !fallback_error
                && result != login_start_result_t::cancelled)
                fallback_error = "Login startup failed";
            if (!ok && state) {
                if (result == login_start_result_t::failed
                    || result == login_start_result_t::timed_out
                    || result == login_start_result_t::rejected) {
                    publishLoginStartFailure(state, fallback_error
                        ? fallback_error
                        : (error.empty() ? "Login startup failed" : error.c_str()));
                } else {
                    state->cancelled.store(true, std::memory_order_release);
                }
                invoke_cancel();
            }
        };
        submitted = aida::infra::executor::submit(std::move(sub));
    } catch (...) {
        finish(login_start_result_t::rejected, "Login startup submission exception", false);
        return false;
    }
    if (!submitted.submitted) {
        finish(login_start_result_t::rejected,
            submitted.reject_reason.empty()
                ? "Login startup queue rejected"
                : submitted.reject_reason.c_str(),
            false);
        return false;
    }
    ticket->task_id.store(submitted.task_id, std::memory_order_release);
    if (ticket->cancellation_requested.load(std::memory_order_acquire)) {
        try { aida::infra::executor::cancel(submitted.task_id); } catch (...) {}
    }
    try {
        diag::log_tagged_fmt("auth",
            "AUTH-LOGIN-START-QUEUED provider=%s generation=%llu task_id=%llu deadline_ms=%llu",
            provider,
            static_cast<unsigned long long>(generation),
            static_cast<unsigned long long>(submitted.task_id),
            static_cast<unsigned long long>(deadline));
    } catch (...) {
    }
    return true;
}

template <typename State, typename CancelFn>
void AidaAuthViewModel::submitProviderCancel(const char* label, const char* provider,
    const std::shared_ptr<State>& state, CancelFn cancel_fn) {
    if (!state)
        return;
    std::shared_ptr<provider_cancel_ticket_t> ticket;
    try { ticket = std::make_shared<provider_cancel_ticket_t>(); } catch (...) {}
    auto publish_terminal = [ticket, provider](const char* result) noexcept {
        if (!ticket)
            return;
        bool expected = false;
        if (!ticket->terminal_published.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
            return;
        try {
            diag::log_tagged_fmt("auth", "AUTH-LOGIN-CANCEL-TERMINAL provider=%s result=%s",
                provider, result ? result : "unknown");
        } catch (...) {
        }
    };
    auto execute_cancel = [state, cancel_fn, publish_terminal, ticket]() mutable noexcept {
        if (ticket) {
            bool expected = false;
            if (!ticket->execution_claimed.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_acquire))
                return;
        }
        const char* result = "completed";
        struct terminal_guard_t {
            decltype(publish_terminal)* publish;
            const char** result;
            ~terminal_guard_t() noexcept { (*publish)(*result); }
        } guard{&publish_terminal, &result};
        try {
            const std::function<void()> guarded = [&]() { cancel_fn(*state); };
            if (aida::infra::win_thread::run_function_seh_guarded(guarded) != 0)
                result = "seh_failure";
        } catch (...) {
            result = "exception";
        }
    };
    if (!ticket) {
        execute_cancel();
        return;
    }
    aida::infra::executor::submit_result_t submitted;
    try {
        aida::infra::executor::submission_t sub;
        sub.owner_subsystem = "auth_provider";
        sub.label = label;
        sub.thread_class = "security_liveness";
        sub.domain = aida::infra::executor::domain_t::security_liveness;
        sub.priority = 0;
        sub.deadline_ms = bounded_deadline(kLoginCancelDeadlineMs);
        sub.ui_access_policy = "none";
        sub.failure_policy = "publish_typed_failure";
        sub.shutdown_policy = "cancel_pending";
        sub.cancel_hook = execute_cancel;
        sub.body = execute_cancel;
        submitted = aida::infra::executor::submit(std::move(sub));
    } catch (...) {
        execute_cancel();
        return;
    }
    if (!submitted.submitted)
        execute_cancel();
    try {
        diag::log_tagged_fmt("auth",
            "AUTH-LOGIN-CANCEL-QUEUE provider=%s submitted=%d task_id=%llu reason=%s",
            provider,
            submitted.submitted ? 1 : 0,
            static_cast<unsigned long long>(submitted.task_id),
            submitted.reject_reason.empty() ? "none" : submitted.reject_reason.c_str());
    } catch (...) {
    }
}

bool AidaAuthViewModel::openUrlInBrowser(const std::string& url) {
    if (shutdown_flag_.load(std::memory_order_acquire))
        return false;
    bool expected = false;
    if (!browser_open_.in_flight.compare_exchange_strong(expected, true,
        std::memory_order_acq_rel, std::memory_order_acquire)) {
        diag::log_tagged_fmt("auth", "AUTH-BROWSER-UI-QUEUE-REJECT reason=already_in_flight");
        chrome::toast_warning(QStringLiteral("A Camoufox request is already in progress"),
            4.0);
        return false;
    }
    const std::uint64_t generation =
        browser_open_.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    browser_open_.completion_pending.store(false, std::memory_order_release);
    browser_open_.result.store(
        static_cast<int>(aida::auth::browser_open_result_t::queued),
        std::memory_order_release);
    aida::auth::browser_open_submission_t submitted;
    try {
        submitted = aida::auth::submit_open_url_external(url,
            [this, generation](const aida::auth::browser_open_completion_t& result) noexcept {
                if (browser_open_.generation.load(std::memory_order_acquire) != generation)
                    return;
                browser_open_.result.store(static_cast<int>(result.result),
                    std::memory_order_release);
                browser_open_.task_id.store(0, std::memory_order_release);
                browser_open_.in_flight.store(false, std::memory_order_release);
                browser_open_.completion_pending.store(true, std::memory_order_release);
                QMetaObject::invokeMethod(this, [this] { armPollTimer(); },
                    Qt::QueuedConnection);
            });
    } catch (...) {
        browser_open_.result.store(
            static_cast<int>(aida::auth::browser_open_result_t::exception),
            std::memory_order_release);
        browser_open_.task_id.store(0, std::memory_order_release);
        browser_open_.in_flight.store(false, std::memory_order_release);
        browser_open_.completion_pending.store(true, std::memory_order_release);
        return false;
    }
    if (submitted.submitted
        && browser_open_.generation.load(std::memory_order_acquire) == generation
        && browser_open_.in_flight.load(std::memory_order_acquire))
        browser_open_.task_id.store(submitted.task_id, std::memory_order_release);
    armPollTimer();
    return submitted.submitted;
}

void AidaAuthViewModel::pollBrowserOpenCompletion() {
    if (!browser_open_.completion_pending.exchange(false, std::memory_order_acq_rel))
        return;
    const auto result = static_cast<aida::auth::browser_open_result_t>(
        browser_open_.result.load(std::memory_order_acquire));
    if (result == aida::auth::browser_open_result_t::opened)
        return;
    const std::string message = browser_open_failure_message(static_cast<int>(result));
    if (!message.empty() && !shutdown_flag_.load(std::memory_order_acquire)) {
        chrome::toast_error(QString::fromStdString(message), 6.0);
    }
}

template <typename State>
void AidaAuthViewModel::pollLoginStartControl(const char* provider,
    login_start_control_t& control, const std::shared_ptr<State>& state) {
    std::shared_ptr<login_start_ticket_t> ticket;
    {
        std::lock_guard<std::mutex> lock(control.mutex);
        ticket = control.current;
    }
    const std::uint64_t deadline = control.deadline_ms.load(std::memory_order_acquire);
    const std::uint64_t now = aida::infra::executor::now_ms();
    if (ticket && control.active.load(std::memory_order_acquire)
        && deadline != 0 && now >= deadline) {
        bool expected_terminal = false;
        if (ticket->terminal_published.compare_exchange_strong(expected_terminal, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
            ticket->cancellation_requested.store(true, std::memory_order_release);
            if (state)
                state->cancelled.store(true, std::memory_order_release);
            bool current = false;
            {
                std::lock_guard<std::mutex> lock(control.mutex);
                current = control.current == ticket
                    && control.generation.load(std::memory_order_acquire)
                        == ticket->generation;
                if (current) {
                    control.result.store(
                        static_cast<int>(login_start_result_t::timed_out),
                        std::memory_order_release);
                    control.deadline_ms.store(0, std::memory_order_release);
                    control.completion_pending.store(true, std::memory_order_release);
                }
            }
            if (current)
                publishLoginStartFailure(state, "Login startup timed out");
            const std::uint64_t task_id = ticket->task_id.load(std::memory_order_acquire);
            if (task_id != 0) {
                try { aida::infra::executor::cancel(task_id); } catch (...) {}
            }
        }
    }
    if (control.completion_pending.exchange(false, std::memory_order_acq_rel)) {
        const auto result = control.result.load(std::memory_order_acquire);
        diag::log_tagged_fmt("auth",
            "AUTH-LOGIN-START-POLL provider=%s result=%s active=%d worker_started=%d",
            provider,
            login_start_result_name(result),
            control.active.load(std::memory_order_acquire) ? 1 : 0,
            ticket && ticket->worker_started.load(std::memory_order_acquire) ? 1 : 0);
        Q_EMIT flowProgressed();
    }
}

void AidaAuthViewModel::pollLoginStartups() {
    std::shared_ptr<aida::auth::codex::codex_login_state_t> codex;
    std::shared_ptr<aida::auth::copilot::copilot_login_state_t> copilot;
    std::shared_ptr<aida::auth::claude_code::claude_code_login_state_t> claude;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        codex = codex_state_;
        copilot = copilot_state_;
        claude = claude_code_state_;
    }
    pollLoginStartControl("openai", codex_start_, codex);
    pollLoginStartControl("github-copilot", copilot_start_, copilot);
    pollLoginStartControl("anthropic", claude_code_start_, claude);
}

void AidaAuthViewModel::requestLoginStartCancel(login_start_control_t& control) noexcept {
    std::shared_ptr<login_start_ticket_t> ticket;
    try {
        std::lock_guard<std::mutex> lock(control.mutex);
        ticket = control.current;
    } catch (...) {
        return;
    }
    if (!ticket)
        return;
    ticket->cancellation_requested.store(true, std::memory_order_release);
    const std::uint64_t task_id = ticket->task_id.load(std::memory_order_acquire);
    if (task_id != 0) {
        try { aida::infra::executor::cancel(task_id); } catch (...) {}
    }
}

void AidaAuthViewModel::startCodexLogin() {
    if (codex_start_.active.load(std::memory_order_acquire)) {
        chrome::toast_warning(QStringLiteral("The previous OpenAI login is still stopping"),
            4.0);
        return;
    }
    std::shared_ptr<aida::auth::codex::codex_login_state_t> previous;
    auto current = allocateLoginState<aida::auth::codex::codex_login_state_t>(
        "OpenAI login state allocation failed");
    if (!current)
        return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        previous = codex_state_;
        codex_flow_generation_.fetch_add(1, std::memory_order_acq_rel);
        codex_state_ = current;
        codex_flow_open_.store(true);
        codex_success_played_ = false;
        codex_exchange_result_.store(static_cast<int>(exchange_result_t::pending));
    }
    if (previous)
        aida::auth::codex::request_cancel(*previous);
    submitLoginStartTask(
        "auth.codex.start_login", "openai", codex_start_, current, previous,
        [](aida::auth::codex::codex_login_state_t& state, std::uint64_t deadline_ms) {
            return aida::auth::codex::start_login(state, deadline_ms);
        },
        [](aida::auth::codex::codex_login_state_t& state) {
            return aida::auth::codex::cancel_login(state);
        },
        []() { return aida::auth::codex::last_error(); });
    armPollTimer();
}

void AidaAuthViewModel::openCopilotFlow() {
    if (copilot_start_.active.load(std::memory_order_acquire)) {
        chrome::toast_warning(QStringLiteral("The previous GitHub login is still stopping"),
            4.0);
        return;
    }
    auto current = allocateLoginState<aida::auth::copilot::copilot_login_state_t>(
        "GitHub login state allocation failed");
    if (!current)
        return;
    std::shared_ptr<aida::auth::copilot::copilot_login_state_t> previous;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        previous = copilot_state_;
        copilot_flow_generation_.fetch_add(1, std::memory_order_acq_rel);
        copilot_state_ = current;
        copilot_flow_open_.store(true);
        copilot_success_played_ = false;
        copilot_poll_result_.store(static_cast<int>(exchange_result_t::pending));
        copilot_flow_started_.store(false);
        if (!copilot_ghe_buf_.empty())
            SecureZeroMemory(copilot_ghe_buf_.data(), copilot_ghe_buf_.size());
        copilot_ghe_buf_.clear();

        aida::auth::auth_info_t prev_info;
        if (aida::auth::store::get("github-copilot", prev_info)
            && !prev_info.enterprise_url.empty()) {
            copilot_ghe_buf_ = prev_info.enterprise_url;
        }
    }
    if (previous) {
        aida::auth::copilot::request_cancel(*previous);
        submitProviderCancel("auth.copilot.cancel_previous", "github-copilot", previous,
            [](aida::auth::copilot::copilot_login_state_t& state) {
                return aida::auth::copilot::cancel_login(state);
            });
    }
}

void AidaAuthViewModel::startCopilotFlow(std::optional<std::string> enterprise_url) {
    if (copilot_start_.active.load(std::memory_order_acquire)) {
        chrome::toast_warning(QStringLiteral("The previous GitHub login is still stopping"),
            4.0);
        return;
    }
    copilot_flow_started_.store(true);
    std::shared_ptr<aida::auth::copilot::copilot_login_state_t> current;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        current = copilot_state_;
    }
    submitLoginStartTask(
        "auth.copilot.start_login", "github-copilot", copilot_start_, current,
        std::shared_ptr<aida::auth::copilot::copilot_login_state_t>{},
        [enterprise_url](aida::auth::copilot::copilot_login_state_t& state,
            std::uint64_t deadline_ms) {
            return aida::auth::copilot::start_login(state, enterprise_url, deadline_ms);
        },
        [](aida::auth::copilot::copilot_login_state_t& state) {
            return aida::auth::copilot::cancel_login(state);
        },
        []() { return aida::auth::copilot::last_error(); });
    armPollTimer();
}

void AidaAuthViewModel::startClaudeCodeLogin() {
    if (claude_code_start_.active.load(std::memory_order_acquire)) {
        chrome::toast_warning(QStringLiteral(
            "The previous Claude Code login is still stopping"), 4.0);
        return;
    }
    std::shared_ptr<aida::auth::claude_code::claude_code_login_state_t> previous;
    auto current = allocateLoginState<aida::auth::claude_code::claude_code_login_state_t>(
        "Claude login state allocation failed");
    if (!current)
        return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        previous = claude_code_state_;
        claude_code_flow_generation_.fetch_add(1, std::memory_order_acq_rel);
        claude_code_state_ = current;
        claude_code_flow_open_.store(true);
        claude_code_success_played_ = false;
        claude_code_exchange_result_.store(static_cast<int>(exchange_result_t::pending));
    }
    if (previous)
        aida::auth::claude_code::request_cancel(*previous);
    submitLoginStartTask(
        "auth.claude_code.start_login", "anthropic", claude_code_start_, current, previous,
        [](aida::auth::claude_code::claude_code_login_state_t& state,
            std::uint64_t deadline_ms) {
            return aida::auth::claude_code::start_login(state, deadline_ms);
        },
        [](aida::auth::claude_code::claude_code_login_state_t& state) {
            return aida::auth::claude_code::cancel_login(state);
        },
        []() { return aida::auth::claude_code::last_error(); });
    armPollTimer();
}

void AidaAuthViewModel::closeCodexFlow() {
    requestLoginStartCancel(codex_start_);
    std::shared_ptr<aida::auth::codex::codex_login_state_t> state_ref;
    std::uint64_t exchange_task_id = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ref = codex_state_;
        codex_flow_generation_.fetch_add(1, std::memory_order_acq_rel);
        exchange_task_id = codex_exchange_task_id_.exchange(0, std::memory_order_acq_rel);
        codex_flow_open_.store(false);
        codex_exchange_in_flight_.store(false, std::memory_order_release);
    }
    if (state_ref)
        aida::auth::codex::request_cancel(*state_ref);
    if (exchange_task_id != 0) {
        try { aida::infra::executor::cancel(exchange_task_id); } catch (...) {}
    }
    if (state_ref) {
        submitProviderCancel("auth.codex.cancel_login", "openai", state_ref,
            [](aida::auth::codex::codex_login_state_t& state) {
                return aida::auth::codex::cancel_login(state);
            });
    }
}

void AidaAuthViewModel::closeCopilotFlow() {
    requestLoginStartCancel(copilot_start_);
    std::shared_ptr<aida::auth::copilot::copilot_login_state_t> state_ref;
    std::uint64_t poll_task_id = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ref = copilot_state_;
        copilot_flow_generation_.fetch_add(1, std::memory_order_acq_rel);
        poll_task_id = copilot_poll_task_id_.exchange(0, std::memory_order_acq_rel);
        copilot_flow_open_.store(false);
        copilot_poll_in_flight_.store(false, std::memory_order_release);
        copilot_flow_started_.store(false);
        if (!copilot_ghe_buf_.empty())
            SecureZeroMemory(copilot_ghe_buf_.data(), copilot_ghe_buf_.size());
        copilot_ghe_buf_.clear();
    }
    if (state_ref)
        aida::auth::copilot::request_cancel(*state_ref);
    if (poll_task_id != 0) {
        try { aida::infra::executor::cancel(poll_task_id); } catch (...) {}
    }
    if (state_ref) {
        submitProviderCancel("auth.copilot.cancel_login", "github-copilot", state_ref,
            [](aida::auth::copilot::copilot_login_state_t& state) {
                return aida::auth::copilot::cancel_login(state);
            });
    }
}

void AidaAuthViewModel::closeClaudeCodeFlow() {
    requestLoginStartCancel(claude_code_start_);
    std::shared_ptr<aida::auth::claude_code::claude_code_login_state_t> state_ref;
    std::uint64_t exchange_task_id = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ref = claude_code_state_;
        claude_code_flow_generation_.fetch_add(1, std::memory_order_acq_rel);
        exchange_task_id = claude_code_exchange_task_id_.exchange(0,
            std::memory_order_acq_rel);
        claude_code_flow_open_.store(false);
        claude_code_exchange_in_flight_.store(false, std::memory_order_release);
    }
    if (state_ref)
        aida::auth::claude_code::request_cancel(*state_ref);
    if (exchange_task_id != 0) {
        try { aida::infra::executor::cancel(exchange_task_id); } catch (...) {}
    }
    if (state_ref) {
        submitProviderCancel("auth.claude_code.cancel_login", "anthropic", state_ref,
            [](aida::auth::claude_code::claude_code_login_state_t& state) {
                return aida::auth::claude_code::cancel_login(state);
            });
    }
}

void AidaAuthViewModel::pollActiveLogins() {
    std::unique_lock<std::mutex> lk(mtx_);
    bool progressed = false;

    if (codex_flow_open_.load() && codex_state_) {
        auto sp = codex_state_;
        bool finished = sp->done.load();
        const int prior_result = codex_exchange_result_.load();
        const auto start_result = static_cast<login_start_result_t>(
            codex_start_.result.load(std::memory_order_acquire));
        const bool start_failed = start_result == login_start_result_t::failed
            || start_result == login_start_result_t::timed_out
            || start_result == login_start_result_t::rejected;

        if (finished && start_failed
            && prior_result == static_cast<int>(exchange_result_t::pending)) {
            codex_exchange_result_.store(static_cast<int>(exchange_result_t::failure));
        } else if (finished
            && !codex_exchange_in_flight_.load()
            && prior_result == static_cast<int>(exchange_result_t::pending)) {
            codex_exchange_in_flight_.store(true);
            auto state_ref = sp;
            const std::uint64_t generation =
                codex_flow_generation_.load(std::memory_order_acquire);
            aida::infra::executor::submission_t sub;
            sub.owner_subsystem = "auth_provider";
            sub.label = "auth.codex.poll_login";
            sub.thread_class = "service_task";
            sub.domain = aida::infra::executor::domain_t::security_liveness;
            sub.priority = 1;
            sub.generation = generation;
            sub.cancel_hook = [this, state_ref, generation]() noexcept {
                try {
                    std::lock_guard<std::mutex> lk2(mtx_);
                    if (!codexFlowCurrentLocked(state_ref, generation))
                        return;
                    codex_exchange_task_id_.store(0, std::memory_order_release);
                    codex_exchange_in_flight_.store(false, std::memory_order_release);
                } catch (...) {
                }
            };
            sub.body = [this, state_ref, generation]() noexcept {
                bool ok = false;
                try {
                    if (state_ref) {
                        const std::function<void()> guarded = [&]() {
                            ok = aida::auth::codex::poll_login(*state_ref);
                        };
                        if (aida::infra::win_thread::run_function_seh_guarded(guarded) != 0)
                            ok = false;
                    }
                } catch (...) {
                    ok = false;
                }
                try {
                    std::lock_guard<std::mutex> lk2(mtx_);
                    if (!codexFlowCurrentLocked(state_ref, generation))
                        return;
                    codex_exchange_result_.store(static_cast<int>(
                        ok ? exchange_result_t::success : exchange_result_t::failure),
                        std::memory_order_release);
                    codex_exchange_task_id_.store(0, std::memory_order_release);
                    codex_exchange_in_flight_.store(false, std::memory_order_release);
                } catch (...) {
                }
                QMetaObject::invokeMethod(this, [this] { armPollTimer(); },
                    Qt::QueuedConnection);
            };
            const auto submitted = aida::infra::executor::submit(std::move(sub));
            if (!submitted.submitted) {
                codex_exchange_in_flight_.store(false);
                codex_exchange_result_.store(static_cast<int>(exchange_result_t::failure));
            } else if (codexFlowCurrentLocked(state_ref, generation)
                && codex_exchange_in_flight_.load(std::memory_order_acquire)) {
                codex_exchange_task_id_.store(submitted.task_id, std::memory_order_release);
            }
        }

        if (prior_result == static_cast<int>(exchange_result_t::success)) {
            codex_exchange_result_.store(static_cast<int>(exchange_result_t::consumed));
            lk.unlock();
            aida::auth::auth_info_t info;
            aida::auth::store::get("openai", info);
            std::string disp = info.email.empty() ? info.account_id : info.email;
            if (disp.empty())
                disp = "OpenAI account";
            chrome::toast_info(QStringLiteral("Signed in: %1")
                .arg(QString::fromStdString(disp)), 5.0);
            aida::events::publish(aida::events::event_oauth_completed,
                aida::events::oauth_completed_t{ "openai", disp });
            codex_success_played_ = true;
            progressed = true;
            lk.lock();
        } else if (prior_result == static_cast<int>(exchange_result_t::failure)) {
            codex_exchange_result_.store(static_cast<int>(exchange_result_t::consumed));
            std::string err = aida::auth::codex::last_error();
            if (err.empty() && sp)
                err = aida::auth::codex::snapshot(*sp).error;
            if (err.empty())
                err = "OpenAI login failed";
            lk.unlock();
            chrome::toast_error(QStringLiteral("OpenAI login failed: %1")
                .arg(QString::fromStdString(err)), 6.0);
            aida::events::publish(aida::events::event_oauth_failed,
                aida::events::oauth_failed_t{ "openai", err });
            progressed = true;
            lk.lock();
        }
    }

    if (copilot_flow_open_.load() && copilot_state_) {
        auto sp = copilot_state_;
        int64_t now = now_unix();
        const auto copilot_snapshot = aida::auth::copilot::snapshot(*sp);
        bool ready_to_poll = copilot_snapshot.next_poll_unix == 0 ||
            now >= copilot_snapshot.next_poll_unix;
        bool finished = sp->done.load();
        bool can_poll = !finished && ready_to_poll &&
            !copilot_snapshot.device_code.empty();
        bool start_failed = finished && copilot_snapshot.device_code.empty();
        const int prior_result = copilot_poll_result_.load();

        if (start_failed && prior_result == static_cast<int>(exchange_result_t::pending)) {
            copilot_poll_result_.store(static_cast<int>(exchange_result_t::failure));
        } else if (can_poll && !copilot_poll_in_flight_.load()
            && prior_result == static_cast<int>(exchange_result_t::pending)) {
            copilot_poll_in_flight_.store(true);
            auto state_ref = sp;
            const std::uint64_t generation =
                copilot_flow_generation_.load(std::memory_order_acquire);
            aida::infra::executor::submission_t sub;
            sub.owner_subsystem = "auth_provider";
            sub.label = "auth.copilot.poll_login";
            sub.thread_class = "service_task";
            sub.domain = aida::infra::executor::domain_t::security_liveness;
            sub.priority = 1;
            sub.generation = generation;
            sub.cancel_hook = [this, state_ref, generation]() noexcept {
                try {
                    std::lock_guard<std::mutex> lk2(mtx_);
                    if (!copilotFlowCurrentLocked(state_ref, generation))
                        return;
                    copilot_poll_task_id_.store(0, std::memory_order_release);
                    copilot_poll_in_flight_.store(false, std::memory_order_release);
                } catch (...) {
                }
            };
            sub.body = [this, state_ref, generation]() noexcept {
                bool ok = false;
                try {
                    if (state_ref) {
                        const std::function<void()> guarded = [&]() {
                            ok = aida::auth::copilot::poll_login(*state_ref);
                        };
                        if (aida::infra::win_thread::run_function_seh_guarded(guarded) != 0)
                            ok = false;
                    }
                } catch (...) {
                    ok = false;
                }
                try {
                    std::lock_guard<std::mutex> lk2(mtx_);
                    if (!copilotFlowCurrentLocked(state_ref, generation))
                        return;
                    if (ok) {
                        copilot_poll_result_.store(
                            static_cast<int>(exchange_result_t::success),
                            std::memory_order_release);
                    } else if (!state_ref ||
                        state_ref->done.load(std::memory_order_acquire)) {
                        copilot_poll_result_.store(
                            static_cast<int>(exchange_result_t::failure),
                            std::memory_order_release);
                    }
                    copilot_poll_task_id_.store(0, std::memory_order_release);
                    copilot_poll_in_flight_.store(false, std::memory_order_release);
                } catch (...) {
                }
                QMetaObject::invokeMethod(this, [this] { armPollTimer(); },
                    Qt::QueuedConnection);
            };
            const auto submitted = aida::infra::executor::submit(std::move(sub));
            if (!submitted.submitted) {
                copilot_poll_in_flight_.store(false);
            } else if (copilotFlowCurrentLocked(state_ref, generation)
                && copilot_poll_in_flight_.load(std::memory_order_acquire)) {
                copilot_poll_task_id_.store(submitted.task_id, std::memory_order_release);
            }
        }

        if (prior_result == static_cast<int>(exchange_result_t::success)) {
            copilot_poll_result_.store(static_cast<int>(exchange_result_t::consumed));
            lk.unlock();
            aida::auth::auth_info_t info;
            aida::auth::store::get("github-copilot", info);
            std::string disp = info.email.empty() ? info.account_id : info.email;
            if (disp.empty())
                disp = "GitHub account";
            chrome::toast_info(QStringLiteral("Signed in: %1")
                .arg(QString::fromStdString(disp)), 5.0);
            aida::events::publish(aida::events::event_oauth_completed,
                aida::events::oauth_completed_t{ "github-copilot", disp });
            copilot_success_played_ = true;
            progressed = true;
            lk.lock();
        } else if (prior_result == static_cast<int>(exchange_result_t::failure)) {
            copilot_poll_result_.store(static_cast<int>(exchange_result_t::consumed));
            std::string err = aida::auth::copilot::last_error();
            if (err.empty() && sp)
                err = aida::auth::copilot::snapshot(*sp).error;
            if (err.empty())
                err = "Copilot login failed";
            lk.unlock();
            chrome::toast_error(QStringLiteral("Copilot login failed: %1")
                .arg(QString::fromStdString(err)), 6.0);
            aida::events::publish(aida::events::event_oauth_failed,
                aida::events::oauth_failed_t{ "github-copilot", err });
            progressed = true;
            lk.lock();
        }
    }

    if (claude_code_flow_open_.load() && claude_code_state_) {
        auto sp = claude_code_state_;
        bool finished = sp->done.load();
        const int prior_result = claude_code_exchange_result_.load();
        const auto start_result = static_cast<login_start_result_t>(
            claude_code_start_.result.load(std::memory_order_acquire));
        const bool start_failed = start_result == login_start_result_t::failed
            || start_result == login_start_result_t::timed_out
            || start_result == login_start_result_t::rejected;

        if (finished && start_failed
            && prior_result == static_cast<int>(exchange_result_t::pending)) {
            claude_code_exchange_result_.store(static_cast<int>(exchange_result_t::failure));
        } else if (finished
            && !claude_code_exchange_in_flight_.load()
            && prior_result == static_cast<int>(exchange_result_t::pending)) {
            claude_code_exchange_in_flight_.store(true);
            auto state_ref = sp;
            const std::uint64_t generation =
                claude_code_flow_generation_.load(std::memory_order_acquire);
            aida::infra::executor::submission_t sub;
            sub.owner_subsystem = "auth_provider";
            sub.label = "auth.claude_code.poll_login";
            sub.thread_class = "service_task";
            sub.domain = aida::infra::executor::domain_t::security_liveness;
            sub.priority = 1;
            sub.generation = generation;
            sub.cancel_hook = [this, state_ref, generation]() noexcept {
                try {
                    std::lock_guard<std::mutex> lk2(mtx_);
                    if (!claudeFlowCurrentLocked(state_ref, generation))
                        return;
                    claude_code_exchange_task_id_.store(0, std::memory_order_release);
                    claude_code_exchange_in_flight_.store(false, std::memory_order_release);
                } catch (...) {
                }
            };
            sub.body = [this, state_ref, generation]() noexcept {
                bool ok = false;
                try {
                    if (state_ref) {
                        const std::function<void()> guarded = [&]() {
                            ok = aida::auth::claude_code::poll_login(*state_ref);
                        };
                        if (aida::infra::win_thread::run_function_seh_guarded(guarded) != 0)
                            ok = false;
                    }
                } catch (...) {
                    ok = false;
                }
                try {
                    std::lock_guard<std::mutex> lk2(mtx_);
                    if (!claudeFlowCurrentLocked(state_ref, generation))
                        return;
                    claude_code_exchange_result_.store(static_cast<int>(
                        ok ? exchange_result_t::success : exchange_result_t::failure),
                        std::memory_order_release);
                    claude_code_exchange_task_id_.store(0, std::memory_order_release);
                    claude_code_exchange_in_flight_.store(false, std::memory_order_release);
                } catch (...) {
                }
                QMetaObject::invokeMethod(this, [this] { armPollTimer(); },
                    Qt::QueuedConnection);
            };
            const auto submitted = aida::infra::executor::submit(std::move(sub));
            if (!submitted.submitted) {
                claude_code_exchange_in_flight_.store(false);
                claude_code_exchange_result_.store(
                    static_cast<int>(exchange_result_t::failure));
            } else if (claudeFlowCurrentLocked(state_ref, generation)
                && claude_code_exchange_in_flight_.load(std::memory_order_acquire)) {
                claude_code_exchange_task_id_.store(submitted.task_id,
                    std::memory_order_release);
            }
        }

        if (prior_result == static_cast<int>(exchange_result_t::success)) {
            claude_code_exchange_result_.store(
                static_cast<int>(exchange_result_t::consumed));
            lk.unlock();
            aida::auth::auth_info_t info;
            aida::auth::store::get("anthropic", info);
            std::string disp = info.email.empty() ? info.account_id : info.email;
            if (disp.empty())
                disp = "Anthropic account";
            chrome::toast_info(QStringLiteral("Signed in: %1")
                .arg(QString::fromStdString(disp)), 5.0);
            aida::events::publish(aida::events::event_oauth_completed,
                aida::events::oauth_completed_t{ "anthropic", disp });
            claude_code_success_played_ = true;
            progressed = true;
            lk.lock();
        } else if (prior_result == static_cast<int>(exchange_result_t::failure)) {
            claude_code_exchange_result_.store(
                static_cast<int>(exchange_result_t::consumed));
            std::string err = aida::auth::claude_code::last_error();
            if (err.empty() && sp)
                err = aida::auth::claude_code::snapshot(*sp).error;
            if (err.empty())
                err = "Claude Code login failed";
            lk.unlock();
            chrome::toast_error(QStringLiteral("Claude Code login failed: %1")
                .arg(QString::fromStdString(err)), 6.0);
            aida::events::publish(aida::events::event_oauth_failed,
                aida::events::oauth_failed_t{ "anthropic", err });
            progressed = true;
            lk.lock();
        }
    }

    const bool any_active = codex_flow_open_.load() || copilot_flow_open_.load() ||
        claude_code_flow_open_.load() ||
        codex_start_.active.load(std::memory_order_acquire) ||
        copilot_start_.active.load(std::memory_order_acquire) ||
        claude_code_start_.active.load(std::memory_order_acquire) ||
        browser_open_.in_flight.load(std::memory_order_acquire) ||
        refresh_.in_flight.load() || validationActiveLocked();
    lk.unlock();
    if (progressed)
        Q_EMIT flowProgressed();
    if (!any_active && poll_timer_->isActive())
        poll_timer_->stop();
}

void AidaAuthViewModel::onPollTick() {
    pollLoginStartups();
    pollBrowserOpenCompletion();
    pollActiveLogins();
}

bool AidaAuthViewModel::validationActiveLocked() const {
    return !validate_in_flight_.empty();
}

void AidaAuthViewModel::armPollTimer() {
    if (!poll_timer_->isActive())
        poll_timer_->start();
}

bool AidaAuthViewModel::publishProviderValidation(const std::string& provider_id,
    const std::shared_ptr<provider_validation_ticket_t>& ticket,
    const test_result_t& result, bool terminal_reserved) noexcept {
    if (!ticket)
        return false;
    if (terminal_reserved) {
        if (!ticket->terminal_claimed.load(std::memory_order_acquire))
            return false;
    } else {
        bool expected = false;
        if (!ticket->terminal_claimed.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
            return false;
    }
    bool current = false;
    try {
        std::lock_guard<std::mutex> lock(mtx_);
        auto found = validate_in_flight_.find(provider_id);
        current = found != validate_in_flight_.end() && found->second == ticket;
        if (current) {
            validate_results_[provider_id] = result;
            validate_in_flight_.erase(found);
        }
    } catch (...) {
    }
    ticket->completed.store(true, std::memory_order_release);
    if (current && result.success && !shutdown_flag_.load(std::memory_order_acquire)) {
        try {
            chrome::toast_info(QStringLiteral("%1 connected")
                .arg(QString::fromStdString(provider_id)), 3.5);
        } catch (...) {
        }
    }
    if (current) {
        QMetaObject::invokeMethod(this, [this, provider_id] {
            Q_EMIT validationChanged(QString::fromStdString(provider_id));
            Q_EMIT authStateChanged();
        }, Qt::QueuedConnection);
    }
    return current;
}

void AidaAuthViewModel::runValidation(const std::string& provider_id,
                                      const std::string& key) {
    if (shutdown_flag_.load(std::memory_order_acquire))
        return;
    const chatbox_provider_entry_t* entry = chatboxEntryFor(provider_id);
    if (entry == nullptr || key.empty() || key.size() > 4096)
        return;
    std::shared_ptr<provider_validation_ticket_t> ticket;
    std::shared_ptr<sensitive_validation_key_t> captured_key;
    chatbox_provider_entry_t entry_copy;
    std::string captured_id;
    try {
        ticket = std::make_shared<provider_validation_ticket_t>();
        captured_key = std::make_shared<sensitive_validation_key_t>();
        captured_key->value = key;
        entry_copy = *entry;
        captured_id = provider_id;
        ticket->generation = validate_generation_.fetch_add(1,
            std::memory_order_acq_rel) + 1;
        ticket->deadline_ms = bounded_deadline(kProviderValidationDeadlineMs);
        std::lock_guard<std::mutex> lock(mtx_);
        auto found = validate_in_flight_.find(provider_id);
        if (found != validate_in_flight_.end() && found->second
            && !found->second->completed.load(std::memory_order_acquire))
            return;
        validate_in_flight_[provider_id] = ticket;
        validate_results_[provider_id] = test_result_t{};
    } catch (...) {
        test_result_t failed;
        failed.completed = true;
        try { failed.message = "Validation setup failed"; } catch (...) {}
        try {
            std::lock_guard<std::mutex> lock(mtx_);
            validate_results_[provider_id] = std::move(failed);
            validate_in_flight_.erase(provider_id);
        } catch (...) {
        }
        return;
    }

    try {
        auto publish_failure = [this, captured_id, ticket](const std::string& message,
            int status = 0) noexcept {
            test_result_t result;
            result.completed = true;
            result.success = false;
            result.http_status = status;
            try { result.message = message; } catch (...) {}
            publishProviderValidation(captured_id, ticket, result);
        };

        aida::infra::executor::submission_t sub;
        sub.owner_subsystem = "auth_provider";
        sub.label = "auth.provider_key.validate";
        sub.thread_class = "service_task";
        sub.domain = aida::infra::executor::domain_t::security_liveness;
        sub.priority = 1;
        sub.deadline_ms = ticket->deadline_ms;
        sub.generation = ticket->generation;
        sub.ui_access_policy = "none";
        sub.failure_policy = "publish_typed_failure";
        sub.shutdown_policy = "cancel_pending";
        sub.cancel_hook = [this, ticket, publish_failure]() mutable noexcept {
            ticket->cancellation_requested.store(true, std::memory_order_release);
            publish_failure(aida::infra::executor::now_ms() >= ticket->deadline_ms
                ? "Validation timed out" : "Validation cancelled");
        };
        sub.body = [this, captured_id, captured_key, entry_copy, ticket,
            publish_failure]() mutable noexcept {
            bool credential_committed = false;
            if (shutdown_flag_.load(std::memory_order_acquire)
                || ticket->cancellation_requested.load(std::memory_order_acquire)) {
                publish_failure("Validation cancelled");
                return;
            }
            if (aida::infra::executor::now_ms() >= ticket->deadline_ms) {
                publish_failure("Validation timed out");
                return;
            }
            try {
                std::string base_url;
                aida::auth::auth_info_t tmp_info;
                tmp_info.api_key = captured_key->value;
                std::string resolved = aida::provider::transforms::resolve_endpoint(
                    captured_id, preferredModelId(captured_id), tmp_info);
                if (!tmp_info.api_key.empty())
                    SecureZeroMemory(tmp_info.api_key.data(), tmp_info.api_key.size());
                tmp_info.api_key.clear();
                if (!resolved.empty() && resolved.rfind("http", 0) == 0)
                    base_url = resolved;
                if (base_url.empty()) {
                    const auto* provider =
                        aida::provider::catalog::get_provider(captured_id);
                    if (provider && !provider->base_url.empty())
                        base_url = provider->base_url;
                }
                if (base_url.empty())
                    base_url = entry_copy.fallback_base;
                while (!base_url.empty() && base_url.back() == '/')
                    base_url.pop_back();
                std::string url = base_url + entry_copy.models_path;
                if (!entry_copy.key_query_param.empty()) {
                    url += (url.find('?') == std::string::npos ? '?' : '&');
                    url += entry_copy.key_query_param;
                    url += '=';
                    url += encode_query_value(captured_key->value);
                }
                aida::auth::http::header_list_t headers;
                headers.emplace_back("User-Agent", "AiDAStandalone/1.0");
                headers.emplace_back("Accept", "application/json");
                if (captured_id == "anthropic")
                    headers.emplace_back("anthropic-version", "2023-06-01");
                if (captured_id == "openrouter") {
                    headers.emplace_back("HTTP-Referer", "https://aida.dev/");
                    headers.emplace_back("X-Title", "AiDA");
                }
                if (!entry_copy.key_header_name.empty()) {
                    headers.emplace_back(entry_copy.key_header_name,
                        entry_copy.key_header_prefix + captured_key->value);
                }
                const auto started = std::chrono::steady_clock::now();
                const aida::auth::http::response_t response =
                    aida::auth::http::get(url, headers, 14, [this, ticket]() noexcept {
                        return shutdown_flag_.load(std::memory_order_acquire)
                            || ticket->cancellation_requested.load(std::memory_order_acquire)
                            || aida::infra::executor::now_ms() >= ticket->deadline_ms;
                    });
                if (!entry_copy.key_query_param.empty() && !url.empty())
                    SecureZeroMemory(url.data(), url.size());
                if (!entry_copy.key_header_name.empty() && !headers.empty()) {
                    auto& value = headers.back().second;
                    if (!value.empty())
                        SecureZeroMemory(value.data(), value.size());
                }
                const auto completed = std::chrono::steady_clock::now();
                test_result_t result;
                result.completed = true;
                result.latency_ms = static_cast<int>(std::chrono::duration_cast<
                    std::chrono::milliseconds>(completed - started).count());
                result.http_status = response.status;
                if (shutdown_flag_.load(std::memory_order_acquire)
                    || ticket->cancellation_requested.load(std::memory_order_acquire)) {
                    publish_failure("Validation cancelled");
                    return;
                }
                if (aida::infra::executor::now_ms() >= ticket->deadline_ms) {
                    publish_failure("Validation timed out");
                    return;
                }
                if (response.cancelled) {
                    publish_failure("Validation cancelled", response.status);
                    return;
                }
                if (!response.ok || !response.complete || response.truncated) {
                    publish_failure(
                        response.error.empty() ? "Transport error" : response.error,
                        response.status);
                    return;
                }
                if (response.status < 200 || response.status >= 300) {
                    const std::string detail = extract_error_from_body(response.body);
                    publish_failure("HTTP " + std::to_string(response.status) + " - "
                        + (detail.empty() ? "request failed" : detail), response.status);
                    return;
                }
                const aida::provider::model_list_validation_t semantics =
                    aida::provider::catalog::validate_provider_model_list_response(
                        captured_id, response.body);
                if (!semantics.valid) {
                    publish_failure(semantics.error.empty()
                        ? "Provider response failed semantic validation" : semantics.error,
                        response.status);
                    return;
                }
                aida::auth::auth_info_t previous;
                const bool had_previous = aida::auth::store::get(captured_id, previous);
                aida::auth::auth_info_t info;
                info.kind = aida::auth::auth_kind_t::api;
                info.api_key = captured_key->value;
                info.metadata = previous.metadata;
                info.custom_client_id = previous.custom_client_id;
                info.custom_redirect_uri = previous.custom_redirect_uri;
                info.custom_scopes = previous.custom_scopes;
                bool stored = false;
                bool settings_saved = true;
                bool rollback_succeeded = true;
                std::string model_id;
                std::string persistence_error;
                {
                    std::lock_guard<std::recursive_mutex> settings_lock(
                        sa_settings_detail::io_mutex());
                    settings_sa_t settings_before = g_sa_settings;
                    stored = aida::auth::store::set_if(captured_id, info,
                        [this, ticket]() noexcept {
                            if (shutdown_flag_.load(std::memory_order_acquire)
                                || ticket->cancellation_requested.load(
                                    std::memory_order_acquire)
                                || aida::infra::executor::now_ms() >= ticket->deadline_ms)
                                return false;
                            bool expected = false;
                            return ticket->terminal_claimed.compare_exchange_strong(
                                expected, true, std::memory_order_acq_rel,
                                std::memory_order_acquire);
                        });
                    if (stored) {
                        model_id = preferredModelId(captured_id);
                        if (!model_id.empty()) {
                            g_sa_settings.set_selection(captured_id, model_id);
                            auto* profile = g_sa_settings.get_active_profile();
                            if (profile != nullptr) {
                                profile->model = model_id;
                                g_sa_settings.sync_legacy_fields_from_active_profile();
                            }
                            settings_saved = aida::settings_persistence::commit_lifecycle(
                                g_sa_settings, persistence_error);
                            if (!settings_saved) {
                                g_sa_settings = std::move(settings_before);
                                rollback_succeeded = had_previous
                                    ? aida::auth::store::set(captured_id, previous)
                                    : aida::auth::store::remove(captured_id);
                            }
                        }
                    }
                }
                if (!stored) {
                    result.success = false;
                    result.message = "Credential persistence failed: "
                        + aida::auth::store::last_error();
                    publishProviderValidation(captured_id, ticket, result,
                        ticket->terminal_claimed.load(std::memory_order_acquire));
                    return;
                }
                if (!settings_saved) {
                    result.success = false;
                    result.message = "Settings persistence failed: "
                        + (persistence_error.empty() ? std::string("unknown error")
                            : persistence_error);
                    if (!rollback_succeeded)
                        result.message += "; credential rollback failed: "
                            + aida::auth::store::last_error();
                    publishProviderValidation(captured_id, ticket, result, true);
                    return;
                }
                credential_committed = true;
                result.success = true;
                result.message = "Connected - " + std::to_string(semantics.model_count)
                    + (semantics.model_count == 1 ? " model available"
                        : " models available");
                scheduleAuthSnapshotRefresh(true);
                if (!model_id.empty()) {
                    aida::events::model_changed_t event;
                    event.session_id.clear();
                    event.provider_id = captured_id;
                    event.model_id = model_id;
                    aida::events::publish(aida::events::event_model_changed, event);
                }
                publishProviderValidation(captured_id, ticket, result, true);
            } catch (...) {
                test_result_t result;
                result.completed = true;
                result.success = credential_committed;
                try {
                    result.message = credential_committed
                        ? "Connected; post-connect update failed"
                        : "Validation failed with an internal exception";
                } catch (...) {
                }
                publishProviderValidation(captured_id, ticket, result,
                    ticket->terminal_claimed.load(std::memory_order_acquire));
            }
        };
        aida::infra::executor::submit_result_t posted;
        try { posted = aida::infra::executor::submit(std::move(sub)); } catch (...) {
            publish_failure("Validation submission failed");
            return;
        }
        if (!posted.submitted) {
            publish_failure(posted.reject_reason.empty()
                ? "Validation queue rejected" : posted.reject_reason);
            return;
        }
        ticket->task_id.store(posted.task_id, std::memory_order_release);
        if (ticket->cancellation_requested.load(std::memory_order_acquire)) {
            try { aida::infra::executor::cancel(posted.task_id); } catch (...) {}
        }
    } catch (...) {
        test_result_t failed;
        failed.completed = true;
        try { failed.message = "Validation scheduling failed"; } catch (...) {}
        publishProviderValidation(captured_id, ticket, failed);
    }
    armPollTimer();
}

validation_snapshot_t AidaAuthViewModel::validationState(const std::string& provider_id) {
    validation_snapshot_t out;
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = validate_in_flight_.find(provider_id);
    if (it != validate_in_flight_.end() && it->second)
        out.busy = !it->second->completed.load(std::memory_order_acquire);
    auto rit = validate_results_.find(provider_id);
    if (rit != validate_results_.end()) {
        out.completed = rit->second.completed;
        out.success = rit->second.success;
        out.latency_ms = rit->second.latency_ms;
        out.message = QString::fromStdString(rit->second.message);
    }
    return out;
}

void AidaAuthViewModel::clearValidationResult(const std::string& provider_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    validate_results_.erase(provider_id);
}

std::string AidaAuthViewModel::persistedKeyFor(const std::string& provider_id) {
    std::string out;
    aida::auth::auth_info_t info;
    if (aida::auth::store::get(provider_id, info)
        && info.kind == aida::auth::auth_kind_t::api
        && !info.api_key.empty())
        out = info.api_key;
    return out;
}

std::string AidaAuthViewModel::preferredModelId(const std::string& provider_id) {
    std::string preferred;
    {
        std::lock_guard<std::recursive_mutex> lock(sa_settings_detail::io_mutex());
        const auto& prefs = g_sa_settings.preferred_model_per_provider;
        const auto it = prefs.find(provider_id);
        if (it != prefs.end())
            preferred = it->second;
    }
    if (!preferred.empty() &&
        aida::provider::catalog::get_model(provider_id, preferred) != nullptr)
        return preferred;
    const auto* def = aida::provider::catalog::default_model(provider_id);
    if (def)
        return def->id;
    const auto* p = aida::provider::catalog::get_provider(provider_id);
    if (p && !p->model_ids.empty())
        return p->model_ids.front();
    return std::string();
}

void AidaAuthViewModel::setDefaultModel(const std::string& provider_id,
                                        const std::string& model_id) {
    bool saved = false;
    std::string persistence_error;
    {
        std::lock_guard<std::recursive_mutex> settings_lock(
            sa_settings_detail::io_mutex());
        settings_sa_t settings_before = g_sa_settings;
        g_sa_settings.preferred_model_per_provider[provider_id] = model_id;
        g_sa_settings.set_selection(provider_id, model_id);
        auto* prof = g_sa_settings.get_active_profile();
        if (prof != nullptr) {
            prof->model = model_id;
            g_sa_settings.sync_legacy_fields_from_active_profile();
        }
        saved = aida::settings_persistence::commit_lifecycle(g_sa_settings,
            persistence_error);
        if (!saved)
            g_sa_settings = std::move(settings_before);
    }
    if (saved) {
        aida::events::model_changed_t evt;
        evt.session_id.clear();
        evt.provider_id = provider_id;
        evt.model_id = model_id;
        aida::events::publish(aida::events::event_model_changed, evt);
    } else {
        chrome::toast_error(QString::fromStdString(persistence_error.empty()
            ? "Unable to save default model" : persistence_error), 6.0);
    }
}

void AidaAuthViewModel::clearCredentialsFor(const std::string& provider_id) {
    aida::auth::auth_info_t info;
    if (!aida::auth::store::get(provider_id, info))
        return;
    const bool revoke_oauth = (info.kind == aida::auth::auth_kind_t::oauth)
        && (!info.access.empty() || !info.refresh.empty());
    std::string access = info.access;
    std::string refresh = info.refresh;
    if (!aida::auth::store::remove(provider_id)) {
        chrome::toast_error(QStringLiteral("Failed to sign out"), 5.0);
        if (!access.empty())
            SecureZeroMemory(access.data(), access.size());
        if (!refresh.empty())
            SecureZeroMemory(refresh.data(), refresh.size());
        return;
    }
    scheduleAuthSnapshotRefresh(true);
    if (revoke_oauth) {
        aida::infra::executor::submission_t sub;
        sub.owner_subsystem = "auth_provider";
        sub.label = "auth.credentials.revoke";
        sub.thread_class = "service_task";
        sub.domain = aida::infra::executor::domain_t::security_liveness;
        sub.priority = 1;
        sub.body = [provider_id, access, refresh]() mutable {
            if (provider_id == "anthropic")
                aida::auth::claude_code::revoke_tokens(access, refresh, std::string());
            else if (provider_id == "openai")
                aida::auth::codex::revoke_tokens(access, refresh, std::string());
            else if (provider_id == "github-copilot")
                aida::auth::copilot::revoke_tokens(access, refresh, std::string());
            if (!access.empty())
                SecureZeroMemory(access.data(), access.size());
            if (!refresh.empty())
                SecureZeroMemory(refresh.data(), refresh.size());
        };
        aida::infra::executor::submit(std::move(sub));
    }
    if (!access.empty())
        SecureZeroMemory(access.data(), access.size());
    if (!refresh.empty())
        SecureZeroMemory(refresh.data(), refresh.size());
    chrome::toast_info(QStringLiteral("%1 signed out")
        .arg(QString::fromStdString(provider_id)), 3.5);
}

void AidaAuthViewModel::startCatalogRefresh() {
    if (shutdown_flag_.load())
        return;
    bool expected = false;
    if (!refresh_.in_flight.compare_exchange_strong(expected, true))
        return;
    refresh_.completed.store(false);
    refresh_.success.store(false);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        refresh_.message.clear();
    }
    aida::infra::executor::submission_t sub;
    sub.owner_subsystem = "auth_provider";
    sub.label = "auth.catalog.refresh";
    sub.thread_class = "service_task";
    sub.domain = aida::infra::executor::domain_t::service;
    sub.priority = 2;
    sub.body = [this]() {
        if (shutdown_flag_.load()) {
            refresh_.in_flight.store(false);
            refresh_.completed.store(false);
            return;
        }
        const bool ok = aida::provider::catalog::fetch_and_cache(10000);
        if (shutdown_flag_.load()) {
            refresh_.in_flight.store(false);
            refresh_.completed.store(false);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            refresh_.success.store(ok);
            refresh_.message = ok ? std::string("Catalog updated")
                : aida::provider::catalog::last_error();
        }
        refresh_.completed.store(true);
        refresh_.in_flight.store(false);
        QMetaObject::invokeMethod(this, [this] {
            bool success = false;
            QString message;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                success = refresh_.success.load();
                message = QString::fromStdString(refresh_.message);
            }
            Q_EMIT catalogRefreshFinished(success, message);
        }, Qt::QueuedConnection);
    };
    const bool posted = aida::infra::executor::submit(std::move(sub)).submitted;
    if (!posted) {
        refresh_.in_flight.store(false);
        refresh_.completed.store(false);
    } else {
        armPollTimer();
    }
}

const std::vector<AidaAuthViewModel::chatbox_provider_entry_t>&
AidaAuthViewModel::chatboxProviderCatalog() {
    static const std::vector<chatbox_provider_entry_t> entries = {
        { "anthropic",  "Anthropic",       "https://console.anthropic.com/settings/keys",
          "https://api.anthropic.com",          "/v1/models",
          "x-api-key",        "",            ""    },
        { "openai",     "OpenAI",          "https://platform.openai.com/api-keys",
          "https://api.openai.com",             "/v1/models",
          "Authorization",    "Bearer ",     ""    },
        { "openrouter", "OpenRouter",      "https://openrouter.ai/settings/keys",
          "https://openrouter.ai/api",          "/v1/models",
          "Authorization",    "Bearer ",     ""    },
        { "google",     "Google Gemini",   "https://aistudio.google.com/app/apikey",
          "https://generativelanguage.googleapis.com", "/v1beta/models",
          "",                 "",            "key" },
        { "mistral",    "Mistral",         "https://console.mistral.ai/api-keys/",
          "https://api.mistral.ai",             "/v1/models",
          "Authorization",    "Bearer ",     ""    },
        { "groq",       "Groq",            "https://console.groq.com/keys",
          "https://api.groq.com",               "/openai/v1/models",
          "Authorization",    "Bearer ",     ""    },
        { "deepseek",   "DeepSeek",        "https://platform.deepseek.com/api_keys",
          "https://api.deepseek.com",           "/v1/models",
          "Authorization",    "Bearer ",     ""    },
        { "xai",        "xAI Grok",        "https://console.x.ai/",
          "https://api.x.ai",                   "/v1/models",
          "Authorization",    "Bearer ",     ""    },
        { "cerebras",   "Cerebras",        "https://cloud.cerebras.ai/?tab=api-keys",
          "https://api.cerebras.ai",            "/v1/models",
          "Authorization",    "Bearer ",     ""    },
    };
    return entries;
}

const std::vector<oauth_provider_descriptor_t>& AidaAuthViewModel::oauthCatalog() {
    static const std::vector<oauth_provider_descriptor_t> entries = {
        { "anthropic",      "Claude Code",
          "Anthropic OAuth (PKCE) - claude.com",                 24 },
        { "openai",         "OpenAI Codex",
          "ChatGPT OAuth (PKCE) - chatgpt.com",                  160 },
        { "github-copilot", "GitHub Copilot",
          "GitHub Device Code - github.com/login/device",        232 },
    };
    return entries;
}

const AidaAuthViewModel::chatbox_provider_entry_t* AidaAuthViewModel::chatboxEntryFor(
    const std::string& id) {
    const auto& cat = chatboxProviderCatalog();
    for (const auto& e : cat)
        if (e.id == id)
            return &e;
    return nullptr;
}

std::shared_ptr<aida::auth::codex::codex_login_state_t> AidaAuthViewModel::codexState() {
    std::lock_guard<std::mutex> lk(mtx_);
    return codex_state_;
}

std::shared_ptr<aida::auth::claude_code::claude_code_login_state_t>
AidaAuthViewModel::claudeCodeState() {
    std::lock_guard<std::mutex> lk(mtx_);
    return claude_code_state_;
}

std::shared_ptr<aida::auth::copilot::copilot_login_state_t>
AidaAuthViewModel::copilotState() {
    std::lock_guard<std::mutex> lk(mtx_);
    return copilot_state_;
}

bool AidaAuthViewModel::codexFlowOpen() const { return codex_flow_open_.load(); }
bool AidaAuthViewModel::claudeFlowOpen() const { return claude_code_flow_open_.load(); }
bool AidaAuthViewModel::copilotFlowOpen() const { return copilot_flow_open_.load(); }
bool AidaAuthViewModel::copilotFlowStarted() const { return copilot_flow_started_.load(); }

std::string AidaAuthViewModel::copilotEnterpriseUrl() {
    std::lock_guard<std::mutex> lk(mtx_);
    return copilot_ghe_buf_;
}

void AidaAuthViewModel::setCopilotEnterpriseUrl(const std::string& url) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!copilot_ghe_buf_.empty())
        SecureZeroMemory(copilot_ghe_buf_.data(), copilot_ghe_buf_.size());
    copilot_ghe_buf_ = url;
}

bool AidaAuthViewModel::codexExchangeInFlight() const {
    return codex_exchange_in_flight_.load();
}
bool AidaAuthViewModel::claudeExchangeInFlight() const {
    return claude_code_exchange_in_flight_.load();
}
bool AidaAuthViewModel::copilotPollInFlight() const {
    return copilot_poll_in_flight_.load();
}
bool AidaAuthViewModel::codexStartActive() const {
    return codex_start_.active.load(std::memory_order_acquire);
}
bool AidaAuthViewModel::claudeStartActive() const {
    return claude_code_start_.active.load(std::memory_order_acquire);
}
bool AidaAuthViewModel::copilotStartActive() const {
    return copilot_start_.active.load(std::memory_order_acquire);
}
bool AidaAuthViewModel::codexSuccessPlayed() const { return codex_success_played_; }
bool AidaAuthViewModel::claudeSuccessPlayed() const { return claude_code_success_played_; }
bool AidaAuthViewModel::copilotSuccessPlayed() const { return copilot_success_played_; }

void AidaAuthViewModel::initialize() {
    shutdown_flag_.store(false);
    aida::provider::catalog::initialize_async(86400);
    {
        std::lock_guard<std::mutex> lk(mtx_);

        if (sub_completed_.valid())
            aida::events::unsubscribe(sub_completed_);
        if (sub_failed_.valid())
            aida::events::unsubscribe(sub_failed_);

        sub_completed_ = aida::events::subscribe(
            aida::events::event_oauth_completed,
            std::function<void(const aida::events::oauth_completed_t&)>(
                [this](const aida::events::oauth_completed_t& ev) {
                    {
                        std::lock_guard<std::mutex> lk2(mtx_);
                        last_completed_provider_ = ev.provider_id;
                        last_completed_email_ = ev.email;
                        have_completed_event_.store(true);
                    }
                    scheduleAuthSnapshotRefresh(true);
                }));

        sub_failed_ = aida::events::subscribe(
            aida::events::event_oauth_failed,
            std::function<void(const aida::events::oauth_failed_t&)>(
                [this](const aida::events::oauth_failed_t& ev) {
                    std::lock_guard<std::mutex> lk2(mtx_);
                    last_failed_provider_ = ev.provider_id;
                    last_failed_error_ = ev.error;
                    have_failed_event_.store(true);
                    setErrLocked(ev.error);
                }));
    }
    scheduleAuthSnapshotRefresh(true);
}

void AidaAuthViewModel::shutdown() {
    shutdown_flag_.store(true);
    aida::provider::catalog::cancel_initialize();
    requestLoginStartCancel(codex_start_);
    requestLoginStartCancel(copilot_start_);
    requestLoginStartCancel(claude_code_start_);
    std::shared_ptr<aida::auth::codex::codex_login_state_t> codex_local;
    std::shared_ptr<aida::auth::copilot::copilot_login_state_t> copilot_local;
    std::shared_ptr<aida::auth::claude_code::claude_code_login_state_t> claude_local;
    std::uint64_t codex_exchange_task_id = 0;
    std::uint64_t copilot_poll_task_id = 0;
    std::uint64_t claude_exchange_task_id = 0;
    std::vector<std::shared_ptr<provider_validation_ticket_t>> validation_tickets;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        codex_local = codex_state_;
        copilot_local = copilot_state_;
        claude_local = claude_code_state_;
        codex_flow_generation_.fetch_add(1, std::memory_order_acq_rel);
        copilot_flow_generation_.fetch_add(1, std::memory_order_acq_rel);
        claude_code_flow_generation_.fetch_add(1, std::memory_order_acq_rel);
        codex_exchange_task_id = codex_exchange_task_id_.exchange(0,
            std::memory_order_acq_rel);
        copilot_poll_task_id = copilot_poll_task_id_.exchange(0, std::memory_order_acq_rel);
        claude_exchange_task_id = claude_code_exchange_task_id_.exchange(0,
            std::memory_order_acq_rel);
        codex_exchange_in_flight_.store(false, std::memory_order_release);
        copilot_poll_in_flight_.store(false, std::memory_order_release);
        claude_code_exchange_in_flight_.store(false, std::memory_order_release);
        for (auto& kv : validate_in_flight_) {
            if (kv.second) {
                kv.second->cancellation_requested.store(true, std::memory_order_release);
                validation_tickets.push_back(kv.second);
            }
        }
        validate_in_flight_.clear();
        validate_results_.clear();

        codex_flow_open_.store(false);
        copilot_flow_open_.store(false);
        claude_code_flow_open_.store(false);
        copilot_flow_started_.store(false);
        if (!copilot_ghe_buf_.empty())
            SecureZeroMemory(copilot_ghe_buf_.data(), copilot_ghe_buf_.size());
        copilot_ghe_buf_.clear();
    }
    if (codex_local)
        aida::auth::codex::request_cancel(*codex_local);
    if (copilot_local)
        aida::auth::copilot::request_cancel(*copilot_local);
    if (claude_local)
        aida::auth::claude_code::request_cancel(*claude_local);
    for (const auto& ticket : validation_tickets) {
        const std::uint64_t task_id = ticket->task_id.exchange(0, std::memory_order_acq_rel);
        if (task_id != 0) {
            try { aida::infra::executor::cancel(task_id); } catch (...) {}
        }
    }
    if (codex_exchange_task_id != 0) {
        try { aida::infra::executor::cancel(codex_exchange_task_id); } catch (...) {}
    }
    if (copilot_poll_task_id != 0) {
        try { aida::infra::executor::cancel(copilot_poll_task_id); } catch (...) {}
    }
    if (claude_exchange_task_id != 0) {
        try { aida::infra::executor::cancel(claude_exchange_task_id); } catch (...) {}
    }

    if (codex_local) {
        submitProviderCancel("auth.codex.shutdown_cancel", "openai", codex_local,
            [](aida::auth::codex::codex_login_state_t& state) {
                return aida::auth::codex::cancel_login(state);
            });
    }
    if (copilot_local) {
        submitProviderCancel("auth.copilot.shutdown_cancel", "github-copilot", copilot_local,
            [](aida::auth::copilot::copilot_login_state_t& state) {
                return aida::auth::copilot::cancel_login(state);
            });
    }
    if (claude_local) {
        submitProviderCancel("auth.claude_code.shutdown_cancel", "anthropic", claude_local,
            [](aida::auth::claude_code::claude_code_login_state_t& state) {
                return aida::auth::claude_code::cancel_login(state);
            });
    }
    const std::uint64_t browser_task_id = browser_open_.task_id.exchange(0,
        std::memory_order_acq_rel);
    if (browser_task_id != 0)
        aida::auth::cancel_open_url_external(browser_task_id);
    browser_open_.generation.fetch_add(1, std::memory_order_acq_rel);
    browser_open_.in_flight.store(false, std::memory_order_release);
    browser_open_.completion_pending.store(false, std::memory_order_release);

    std::lock_guard<std::mutex> lk(mtx_);

    if (sub_completed_.valid()) {
        aida::events::unsubscribe(sub_completed_);
        sub_completed_ = aida::events::subscription_handle_t{};
    }
    if (sub_failed_.valid()) {
        aida::events::unsubscribe(sub_failed_);
        sub_failed_ = aida::events::subscription_handle_t{};
    }

    codex_state_.reset();
    copilot_state_.reset();
    claude_code_state_.reset();
}

AidaOAuthProviderRow::AidaOAuthProviderRow(const oauth_provider_descriptor_t& descriptor,
                                           QWidget* parent)
    : QWidget(parent), descriptor_(descriptor) {
    const auto& t = theme::tokens();
    setObjectName(QStringLiteral("aida.view.auth.providerRow.%1")
        .arg(QString::fromStdString(descriptor_.provider_id)));
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(t.spacing.md, t.spacing.sm, t.spacing.md, t.spacing.sm);
    root->setSpacing(t.spacing.md);
    glyph_ = new QLabel(this);
    glyph_->setObjectName(QStringLiteral("aida.view.auth.providerRow.glyph"));
    const int glyph_logical = t.control.height_lg;
    glyph_->setFixedSize(glyph_logical, glyph_logical);
    const QColor color = provider_glyph_color(
        QString::fromStdString(descriptor_.provider_id));
    const qreal dpr = glyph_->devicePixelRatioF();
    QPixmap pixmap(QSize(glyph_logical, glyph_logical) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QRectF(0, 0, glyph_logical, glyph_logical));
        painter.setPen(t.text_primary);
        painter.setFont(theme::fonts::strong());
        const QString name = QString::fromStdString(descriptor_.display_name);
        painter.drawText(QRectF(0, 0, glyph_logical, glyph_logical), Qt::AlignCenter,
            name.isEmpty() ? QStringLiteral("?") : name.left(1));
    }
    glyph_->setPixmap(pixmap);
    auto* text_col = new QVBoxLayout();
    text_col->setSpacing(t.spacing.xxs);
    name_label_ = new QLabel(QString::fromStdString(descriptor_.display_name), this);
    name_label_->setObjectName(QStringLiteral("aida.view.auth.providerRow.name"));
    name_label_->setFont(theme::fonts::strong());
    description_label_ = new QLabel(QString::fromStdString(descriptor_.description), this);
    description_label_->setObjectName(QStringLiteral("aida.view.auth.providerRow.description"));
    description_label_->setFont(theme::fonts::caption());
    description_label_->setProperty("aidaVariant", "secondary");
    description_label_->setWordWrap(true);
    status_pill_ = new QLabel(this);
    status_pill_->setObjectName(QStringLiteral("aida.view.auth.providerRow.status"));
    status_pill_->setFont(theme::fonts::caption());
    status_pill_->setProperty("aidaVariant", "neutral");
    text_col->addWidget(name_label_);
    text_col->addWidget(description_label_);
    text_col->addWidget(status_pill_);
    root->addWidget(glyph_);
    root->addLayout(text_col, 1);
    action_button_ = new QPushButton(QStringLiteral("Sign in with browser"), this);
    action_button_->setObjectName(QStringLiteral("aida.view.auth.providerRow.action"));
    root->addWidget(action_button_, 0, Qt::AlignVCenter);
    connect(action_button_, &QPushButton::clicked, this, [this] {
        const auto status = AidaAuthViewModel::instance().providerStatus(
            descriptor_.provider_id);
        const bool authed = status.authenticated && !status.expired;
        if (authed)
            Q_EMIT signOutRequested(QString::fromStdString(descriptor_.provider_id));
        else
            Q_EMIT signInRequested(QString::fromStdString(descriptor_.provider_id));
    });
    refreshStatus();
}

void AidaOAuthProviderRow::refreshStatus() {
    auto& model = AidaAuthViewModel::instance();
    const auto status = model.providerStatus(descriptor_.provider_id);
    status_pill_->setText(status.detail.isEmpty()
        ? status.label
        : QStringLiteral("%1 - %2").arg(status.label, status.detail));
    const char* variant = status.severity == 1 ? "success"
        : status.severity == 2 ? "warning"
        : status.severity == 3 ? "info" : "neutral";
    if (status_pill_->property("aidaVariant").toString() != QLatin1String(variant)) {
        status_pill_->setProperty("aidaVariant", variant);
        repolish_variant(status_pill_);
    }
    const bool authed = status.authenticated && !status.expired;
    const std::string& id = descriptor_.provider_id;
    bool busy = false;
    if (id == "anthropic")
        busy = model.claudeFlowOpen() || model.claudeStartActive();
    else if (id == "openai")
        busy = model.codexFlowOpen() || model.codexStartActive();
    else if (id == "github-copilot")
        busy = model.copilotFlowOpen() || model.copilotStartActive();
    action_button_->setText(authed ? QStringLiteral("Sign out")
        : busy ? QStringLiteral("Signing in...")
        : QStringLiteral("Sign in with browser"));
    action_button_->setEnabled(!busy);
}

AidaAuthView::AidaAuthView(QWidget* parent) : QWidget(parent), model_(&AidaAuthViewModel::instance()) {
    buildUi();
}

void AidaAuthView::buildUi() {
    const auto& t = theme::tokens();
    setObjectName(QStringLiteral("aida.view.auth"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.spacing.md, t.spacing.sm, t.spacing.md, t.spacing.sm);
    root->setSpacing(t.spacing.sm);

    auto* header = new QHBoxLayout();
    header->setSpacing(t.spacing.sm);
    auto* title_block = new QVBoxLayout();
    title_block->setSpacing(t.spacing.xxs);
    auto* title = new QLabel(QStringLiteral("Accounts"), this);
    title->setObjectName(QStringLiteral("aida.view.auth.title"));
    title->setFont(theme::fonts::h1());
    auto* subtitle = new QLabel(QStringLiteral(
        "Connect model providers for chat, agents, and analysis workflows."), this);
    subtitle->setObjectName(QStringLiteral("aida.view.auth.subtitle"));
    subtitle->setFont(theme::fonts::caption());
    subtitle->setProperty("aidaVariant", "secondary");
    subtitle->setWordWrap(true);
    title_block->addWidget(title);
    title_block->addWidget(subtitle);
    header->addLayout(title_block, 1);
    refresh_catalog_button_ = new QPushButton(QStringLiteral("Refresh model catalog"), this);
    refresh_catalog_button_->setObjectName(QStringLiteral("aida.view.auth.refreshCatalog"));
    refresh_catalog_button_->setToolTip(QStringLiteral(
        "Fetch the latest provider and model catalog from models.dev"));
    header->addWidget(refresh_catalog_button_, 0, Qt::AlignTop);
    root->addLayout(header);

    auto* section_row = new QHBoxLayout();
    section_row->setSpacing(t.spacing.xs);
    api_section_button_ = new QToolButton(this);
    api_section_button_->setObjectName(QStringLiteral("aida.view.auth.section.api"));
    api_section_button_->setText(QStringLiteral("API key (chatbox)"));
    api_section_button_->setCheckable(true);
    api_section_button_->setChecked(true);
    api_section_button_->setAutoExclusive(true);
    api_section_button_->setToolTip(QStringLiteral("Authenticate with a provider API key"));
    oauth_section_button_ = new QToolButton(this);
    oauth_section_button_->setObjectName(QStringLiteral("aida.view.auth.section.oauth"));
    oauth_section_button_->setText(QStringLiteral("Browser OAuth"));
    oauth_section_button_->setCheckable(true);
    oauth_section_button_->setAutoExclusive(true);
    oauth_section_button_->setToolTip(QStringLiteral("Authenticate via browser OAuth flow"));
    section_row->addWidget(api_section_button_);
    section_row->addWidget(oauth_section_button_);
    section_row->addStretch(1);
    root->addLayout(section_row);

    auto* section_host = new QWidget(this);
    section_stack_ = new QStackedLayout(section_host);
    root->addWidget(section_host, 1);

    auto* api_page = new QWidget(section_host);
    auto* api = new QVBoxLayout(api_page);
    api->setContentsMargins(t.spacing.xs, t.spacing.sm, t.spacing.xs, t.spacing.xs);
    api->setSpacing(t.spacing.sm);
    auto* provider_row = new QHBoxLayout();
    provider_row->setSpacing(t.spacing.sm);
    auto* provider_label = new QLabel(QStringLiteral("Provider"), api_page);
    provider_label->setObjectName(QStringLiteral("aida.view.auth.providerLabel"));
    provider_label->setFont(theme::fonts::bodyEm());
    provider_combo_ = new QComboBox(api_page);
    provider_combo_->setObjectName(QStringLiteral("aida.view.auth.provider"));
    provider_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    provider_combo_->setMinimumContentsLength(14);
    for (const auto& entry : AidaAuthViewModel::chatboxProviderCatalog()) {
        provider_combo_->addItem(QString::fromStdString(entry.display_name),
            QString::fromStdString(entry.id));
    }
    provider_row->addWidget(provider_label);
    provider_row->addWidget(provider_combo_, 1);
    api->addLayout(provider_row);

    auto* status_row = new QHBoxLayout();
    status_row->setSpacing(t.spacing.sm);
    status_pill_ = new QLabel(api_page);
    status_pill_->setObjectName(QStringLiteral("aida.view.auth.statusPill"));
    status_pill_->setFont(theme::fonts::caption());
    status_pill_->setProperty("aidaVariant", "neutral");
    status_detail_ = new QLabel(api_page);
    status_detail_->setObjectName(QStringLiteral("aida.view.auth.statusDetail"));
    status_detail_->setFont(theme::fonts::caption());
    status_detail_->setProperty("aidaVariant", "secondary");
    status_detail_->setWordWrap(true);
    status_row->addWidget(status_pill_);
    status_row->addWidget(status_detail_, 1);
    api->addLayout(status_row);

    auto* key_label = new QLabel(QStringLiteral("API key *"), api_page);
    key_label->setObjectName(QStringLiteral("aida.view.auth.keyLabel"));
    key_label->setFont(theme::fonts::bodyEm());
    api->addWidget(key_label);
    key_edit_ = new QLineEdit(api_page);
    key_edit_->setObjectName(QStringLiteral("aida.view.auth.key"));
    key_edit_->setEchoMode(QLineEdit::Password);
    key_edit_->setMaxLength(1024);
    key_edit_->setPlaceholderText(QStringLiteral("Paste API key"));
    key_edit_->setToolTip(QStringLiteral(
        "Required. Stored encrypted in the DPAPI-protected auth store."));
    auto* eye = key_edit_->addAction(theme::icons::icon(QStringLiteral("padlock")),
        QLineEdit::TrailingPosition);
    eye->setToolTip(QStringLiteral("Reveal key"));
    api->addWidget(key_edit_);

    auto* actions_row = new QHBoxLayout();
    actions_row->setSpacing(t.spacing.sm);
    save_button_ = new QPushButton(QStringLiteral("Save & verify"), api_page);
    save_button_->setObjectName(QStringLiteral("aida.view.auth.save"));
    save_button_->setProperty("aidaVariant", "primary");
    clear_button_ = new QPushButton(QStringLiteral("Clear"), api_page);
    clear_button_->setObjectName(QStringLiteral("aida.view.auth.clear"));
    get_key_button_ = new QPushButton(QStringLiteral("Get key"), api_page);
    get_key_button_->setObjectName(QStringLiteral("aida.view.auth.getKey"));
    get_key_button_->setToolTip(QStringLiteral("Open the provider console to create a key"));
    actions_row->addWidget(save_button_);
    actions_row->addWidget(clear_button_);
    actions_row->addWidget(get_key_button_);
    actions_row->addStretch(1);
    api->addLayout(actions_row);

    busy_label_ = new QLabel(api_page);
    busy_label_->setObjectName(QStringLiteral("aida.view.auth.busy"));
    busy_label_->setFont(theme::fonts::caption());
    busy_label_->setProperty("aidaVariant", "info");
    busy_label_->setText(QStringLiteral("Calling /models endpoint to verify the key..."));
    busy_label_->setVisible(false);
    api->addWidget(busy_label_);
    result_label_ = new QLabel(api_page);
    result_label_->setObjectName(QStringLiteral("aida.view.auth.result"));
    result_label_->setFont(theme::fonts::caption());
    result_label_->setWordWrap(true);
    api->addWidget(result_label_);

    default_model_label_ = new QLabel(QStringLiteral("Default model for chat"), api_page);
    default_model_label_->setObjectName(QStringLiteral("aida.view.auth.defaultModelLabel"));
    default_model_label_->setFont(theme::fonts::bodyEm());
    default_model_combo_ = new QComboBox(api_page);
    default_model_combo_->setObjectName(QStringLiteral("aida.view.auth.defaultModel"));
    default_model_combo_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    default_model_combo_->setMinimumContentsLength(20);
    api->addWidget(default_model_label_);
    api->addWidget(default_model_combo_);
    api->addStretch(1);
    section_stack_->addWidget(api_page);

    auto* oauth_page = new QWidget(section_host);
    auto* oauth = new QVBoxLayout(oauth_page);
    oauth->setContentsMargins(t.spacing.xs, t.spacing.sm, t.spacing.xs, t.spacing.xs);
    oauth->setSpacing(t.spacing.sm);
    auto* oauth_hint = new QLabel(QStringLiteral(
        "Sign in with your browser - PKCE tokens stored in DPAPI-protected auth.json."),
        oauth_page);
    oauth_hint->setObjectName(QStringLiteral("aida.view.auth.oauthHint"));
    oauth_hint->setFont(theme::fonts::caption());
    oauth_hint->setProperty("aidaVariant", "secondary");
    oauth_hint->setWordWrap(true);
    oauth->addWidget(oauth_hint);
    oauth_rows_layout_ = new QVBoxLayout();
    oauth_rows_layout_->setSpacing(t.spacing.sm);
    oauth->addLayout(oauth_rows_layout_);
    oauth->addStretch(1);
    section_stack_->addWidget(oauth_page);

    for (const auto& descriptor : AidaAuthViewModel::oauthCatalog()) {
        auto* row = new AidaOAuthProviderRow(descriptor, oauth_page);
        oauth_rows_layout_->addWidget(row);
        connect(row, &AidaOAuthProviderRow::signInRequested, this,
                [this](const QString& provider_id) {
            openOAuthDialog(provider_id);
        });
        connect(row, &AidaOAuthProviderRow::signOutRequested, this,
                [this](const QString& provider_id) {
            const auto id = provider_id.toStdString();
            aida::qt::ai::aida_confirm_request_t request;
            request.verb = QStringLiteral("Sign out of");
            request.target = provider_id;
            request.scope = QStringLiteral(
                "The stored OAuth credential for this provider");
            request.effect = QStringLiteral(
                "Removes the DPAPI-protected tokens and revokes them with the provider.");
            request.reversibility = QStringLiteral(
                "Sign in again with the browser flow to reconnect.");
            request.prerequisite = QString();
            request.confirm_label = QStringLiteral("Sign out");
            request.destructive = true;
            request.confirm_enabled = true;
            aida::qt::ai::AidaConfirmDialog::request(request, this, [this, id] {
                model_->clearCredentialsFor(id);
                refreshProviderStatusPill();
            });
        });
    }

    connect(api_section_button_, &QToolButton::toggled, this, [this](bool checked) {
        if (checked)
            section_stack_->setCurrentIndex(0);
    });
    connect(oauth_section_button_, &QToolButton::toggled, this, [this](bool checked) {
        if (checked)
            section_stack_->setCurrentIndex(1);
    });
    connect(refresh_catalog_button_, &QPushButton::clicked, this, [this] {
        model_->startCatalogRefresh();
    });
    connect(model_, &AidaAuthViewModel::catalogRefreshFinished, this,
            [this](bool success, const QString& message) {
        if (success)
            chrome::toast_info(QStringLiteral("Provider catalog refreshed"), 3.0);
        else
            chrome::toast_error(QStringLiteral("Refresh failed: %1").arg(message), 5.0);
        refreshDefaultModelCombo();
    });
    connect(eye, &QAction::triggered, this, [this] {
        const bool password = key_edit_->echoMode() == QLineEdit::Password;
        key_edit_->setEchoMode(password ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(provider_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (loading_)
            return;
        syncKeyFieldForProvider(provider_combo_->currentData().toString());
    });
    connect(save_button_, &QPushButton::clicked, this, [this] {
        const auto id = provider_combo_->currentData().toString().toStdString();
        std::string key = key_edit_->text().toStdString();
        if (key.empty()) {
            chrome::toast_warning(QStringLiteral("Enter an API key first"), 3.0);
            return;
        }
        model_->runValidation(id, key);
        SecureZeroMemory(key.data(), key.size());
        refreshValidationPresentation();
    });
    connect(clear_button_, &QPushButton::clicked, this, [this] {
        const auto id = provider_combo_->currentData().toString().toStdString();
        model_->clearCredentialsFor(id);
        key_edit_->clear();
        key_edit_->setEchoMode(QLineEdit::Password);
        model_->clearValidationResult(id);
        refreshProviderStatusPill();
        refreshValidationPresentation();
    });
    connect(get_key_button_, &QPushButton::clicked, this, [this] {
        const auto* entry = AidaAuthViewModel::chatboxEntryFor(
            provider_combo_->currentData().toString().toStdString());
        if (entry != nullptr && !entry->console_url.empty())
            model_->openUrlInBrowser(entry->console_url);
    });
    connect(default_model_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (loading_)
            return;
        const auto pid = provider_combo_->currentData().toString().toStdString();
        const auto mid = default_model_combo_->currentData().toString().toStdString();
        if (!mid.empty())
            model_->setDefaultModel(pid, mid);
    });
    connect(model_, &AidaAuthViewModel::validationChanged, this,
            [this](const QString&) {
        refreshValidationPresentation();
        refreshProviderStatusPill();
        refreshDefaultModelCombo();
    });
    connect(model_, &AidaAuthViewModel::authStateChanged, this, [this] {
        refreshProviderStatusPill();
        for (int i = 0; i < oauth_rows_layout_->count(); ++i) {
            if (auto* row = qobject_cast<AidaOAuthProviderRow*>(
                    oauth_rows_layout_->itemAt(i)->widget()))
                row->refreshStatus();
        }
    });
    connect(model_, &AidaAuthViewModel::flowProgressed, this, [this] {
        for (int i = 0; i < oauth_rows_layout_->count(); ++i) {
            if (auto* row = qobject_cast<AidaOAuthProviderRow*>(
                    oauth_rows_layout_->itemAt(i)->widget()))
                row->refreshStatus();
        }
    });

    syncKeyFieldForProvider(provider_combo_->currentData().toString());
}

void AidaAuthView::openOAuthDialog(const QString& provider_id) {
    const auto id = provider_id.toStdString();
    if (id == "openai") {
        model_->startCodexLogin();
        AidaOAuthLoginDialog::showFor(AidaOAuthLoginDialog::Provider::Codex, this);
    } else if (id == "anthropic") {
        model_->startClaudeCodeLogin();
        AidaOAuthLoginDialog::showFor(AidaOAuthLoginDialog::Provider::ClaudeCode, this);
    } else if (id == "github-copilot") {
        model_->openCopilotFlow();
        AidaOAuthLoginDialog::showFor(AidaOAuthLoginDialog::Provider::Copilot, this);
    }
}

void AidaAuthView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshProviderStatusPill();
    refreshValidationPresentation();
    refreshDefaultModelCombo();
    model_->scheduleAuthSnapshotRefresh(false);
}

void AidaAuthView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
}

void AidaAuthView::focusProvider(const QString& provider_id) {
    const auto id = provider_id.toStdString();
    if (AidaAuthViewModel::chatboxEntryFor(id) != nullptr) {
        section_stack_->setCurrentIndex(0);
        api_section_button_->setChecked(true);
        const int index = provider_combo_->findData(provider_id);
        if (index >= 0)
            provider_combo_->setCurrentIndex(index);
        syncKeyFieldForProvider(provider_id);
    } else {
        section_stack_->setCurrentIndex(1);
        oauth_section_button_->setChecked(true);
    }
    key_edit_->setFocus();
}

void AidaAuthView::syncKeyFieldForProvider(const QString& provider_id) {
    key_edit_->clear();
    std::string persisted = model_->persistedKeyFor(provider_id.toStdString());
    if (!persisted.empty()) {
        key_edit_->setText(QString::fromStdString(persisted));
        SecureZeroMemory(persisted.data(), persisted.size());
        persisted.clear();
    }
    key_edit_->setEchoMode(QLineEdit::Password);
    refreshProviderStatusPill();
    refreshValidationPresentation();
    refreshDefaultModelCombo();
}

void AidaAuthView::refreshProviderStatusPill() {
    const auto id = provider_combo_->currentData().toString().toStdString();
    const auto status = model_->providerStatus(id);
    status_pill_->setText(status.label);
    const char* variant = status.severity == 1 ? "success"
        : status.severity == 2 ? "warning"
        : status.severity == 3 ? "info" : "neutral";
    if (status_pill_->property("aidaVariant").toString() != QLatin1String(variant)) {
        status_pill_->setProperty("aidaVariant", variant);
        repolish_variant(status_pill_);
    }
    status_detail_->setText(status.detail);
    status_detail_->setVisible(!status.detail.isEmpty());
    clear_button_->setVisible(status.authenticated);
    default_model_label_->setVisible(status.authenticated);
    default_model_combo_->setVisible(status.authenticated);
}

void AidaAuthView::refreshValidationPresentation() {
    const auto id = provider_combo_->currentData().toString().toStdString();
    const auto state = model_->validationState(id);
    busy_label_->setVisible(state.busy);
    save_button_->setEnabled(!state.busy);
    save_button_->setText(state.busy ? QStringLiteral("Verifying...")
                                     : QStringLiteral("Save & verify"));
    if (!state.busy && state.completed) {
        result_label_->setText(state.success
            ? QStringLiteral("%1  (%2ms)").arg(state.message).arg(state.latency_ms)
            : QStringLiteral("Failed: %1").arg(state.message));
        const char* variant = state.success ? "success" : "error";
        if (result_label_->property("aidaVariant").toString() != QLatin1String(variant)) {
            result_label_->setProperty("aidaVariant", variant);
            repolish_variant(result_label_);
        }
        result_label_->setVisible(true);
    } else {
        result_label_->setVisible(false);
    }
}

void AidaAuthView::refreshDefaultModelCombo() {
    const auto pid = provider_combo_->currentData().toString().toStdString();
    loading_ = true;
    default_model_combo_->clear();
    const auto models = sorted_models_for(pid);
    const std::string current = model_->preferredModelId(pid);
    int current_index = -1;
    for (const auto* m : models) {
        const QString label = QStringLiteral("%1   %2   %3")
            .arg(QString::fromStdString(m->name),
                 format_cost_pair(m->cost.input_per_million, m->cost.output_per_million),
                 format_context_pretty(m->limit.context));
        default_model_combo_->addItem(label, QString::fromStdString(m->id));
        if (m->id == current)
            current_index = default_model_combo_->count() - 1;
    }
    if (current_index >= 0)
        default_model_combo_->setCurrentIndex(current_index);
    loading_ = false;
}

void install_auth_domain(docking::AidaDockHost* host) {
    static_cast<void>(host);
    AidaAuthViewModel::instance().initialize();
    aida::automation_ui::add_ui_shutdown_hook([] {
        AidaAuthViewModel::instance().shutdown();
    });
    diag::log_tagged("qt_auth", "auth_domain_installed");
}

}
