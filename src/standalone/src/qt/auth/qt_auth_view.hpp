#pragma once

#include <QObject>
#include <QString>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/auth/auth_claude_code.hpp"
#include "core/auth/auth_codex.hpp"
#include "core/auth/auth_copilot.hpp"
#include "core/auth/auth_store.hpp"
#include "core/infra/event_bus.hpp"

class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedLayout;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::auth {

struct oauth_provider_descriptor_t {
    std::string provider_id;
    std::string display_name;
    std::string description;
    int glyph_hue = 200;
};

struct provider_status_snapshot_t {
    QString label;
    int severity = 0;
    bool authenticated = false;
    bool expired = false;
    QString detail;
};

struct validation_snapshot_t {
    bool busy = false;
    bool completed = false;
    bool success = false;
    int latency_ms = 0;
    QString message;
};

class AidaAuthViewModel : public QObject {
    Q_OBJECT
public:
    static AidaAuthViewModel& instance();

    void initialize();
    void shutdown();

    bool anyLoginInProgress() const;
    bool isProviderAuthenticated(const std::string& provider_id);
    QString lastError();

    struct chatbox_provider_entry_t {
        std::string id;
        std::string display_name;
        std::string console_url;
        std::string fallback_base;
        std::string models_path;
        std::string key_header_name;
        std::string key_header_prefix;
        std::string key_query_param;
    };
    static const std::vector<chatbox_provider_entry_t>& chatboxProviderCatalog();
    static const std::vector<oauth_provider_descriptor_t>& oauthCatalog();
    static const chatbox_provider_entry_t* chatboxEntryFor(const std::string& id);

    provider_status_snapshot_t providerStatus(const std::string& provider_id);
    validation_snapshot_t validationState(const std::string& provider_id);
    std::string persistedKeyFor(const std::string& provider_id);
    std::string preferredModelId(const std::string& provider_id);
    void scheduleAuthSnapshotRefresh(bool force = false);

    void runValidation(const std::string& provider_id, const std::string& key);
    void clearValidationResult(const std::string& provider_id);
    void clearCredentialsFor(const std::string& provider_id);
    void startCatalogRefresh();
    void setDefaultModel(const std::string& provider_id, const std::string& model_id);

    void startCodexLogin();
    void startClaudeCodeLogin();
    void openCopilotFlow();
    void startCopilotFlow(std::optional<std::string> enterprise_url);
    void closeCodexFlow();
    void closeClaudeCodeFlow();
    void closeCopilotFlow();

    std::shared_ptr<aida::auth::codex::codex_login_state_t> codexState();
    std::shared_ptr<aida::auth::claude_code::claude_code_login_state_t> claudeCodeState();
    std::shared_ptr<aida::auth::copilot::copilot_login_state_t> copilotState();
    bool codexFlowOpen() const;
    bool claudeFlowOpen() const;
    bool copilotFlowOpen() const;
    bool copilotFlowStarted() const;
    std::string copilotEnterpriseUrl();
    void setCopilotEnterpriseUrl(const std::string& url);
    bool codexExchangeInFlight() const;
    bool claudeExchangeInFlight() const;
    bool copilotPollInFlight() const;
    bool codexStartActive() const;
    bool claudeStartActive() const;
    bool copilotStartActive() const;
    bool codexSuccessPlayed() const;
    bool claudeSuccessPlayed() const;
    bool copilotSuccessPlayed() const;

    bool openUrlInBrowser(const std::string& url);

Q_SIGNALS:
    void flowProgressed();
    void validationChanged(const QString& provider_id);
    void authStateChanged();
    void catalogRefreshFinished(bool success, const QString& message);

private:
    explicit AidaAuthViewModel(QObject* parent = nullptr);
    ~AidaAuthViewModel() override = default;

    enum class exchange_result_t : int {
        pending = 0,
        success = 1,
        failure = 2,
        consumed = 3,
    };

    enum class login_start_result_t : int {
        idle = 0,
        pending,
        succeeded,
        failed,
        cancelled,
        timed_out,
        rejected
    };

    struct login_start_ticket_t {
        std::atomic<bool> worker_started{false};
        std::atomic<bool> cancellation_requested{false};
        std::atomic<bool> provider_cancel_invoked{false};
        std::atomic<bool> terminal_published{false};
        std::uint64_t generation = 0;
        std::uint64_t deadline_ms = 0;
        std::atomic<std::uint64_t> task_id{0};
    };

    struct login_start_control_t {
        std::mutex mutex;
        std::shared_ptr<login_start_ticket_t> current;
        std::atomic<bool> active{false};
        std::atomic<bool> completion_pending{false};
        std::atomic<int> result{static_cast<int>(login_start_result_t::idle)};
        std::atomic<std::uint64_t> deadline_ms{0};
        std::atomic<std::uint64_t> generation{0};
    };

    struct browser_open_control_t {
        std::atomic<bool> in_flight{false};
        std::atomic<bool> completion_pending{false};
        std::atomic<int> result{0};
        std::atomic<std::uint64_t> generation{0};
        std::atomic<std::uint64_t> task_id{0};
    };

    struct provider_cancel_ticket_t {
        std::atomic<bool> execution_claimed{false};
        std::atomic<bool> terminal_published{false};
    };

    struct test_result_t {
        bool completed = false;
        bool success = false;
        int latency_ms = 0;
        int http_status = 0;
        std::string message;
    };

    struct provider_validation_ticket_t {
        std::atomic<bool> cancellation_requested{false};
        std::atomic<bool> terminal_claimed{false};
        std::atomic<bool> completed{false};
        std::atomic<std::uint64_t> task_id{0};
        std::uint64_t generation = 0;
        std::uint64_t deadline_ms = 0;
    };

    struct refresh_state_t {
        std::atomic<bool> in_flight{false};
        std::atomic<bool> completed{false};
        std::atomic<bool> success{false};
        std::string message;
    };

    struct sensitive_validation_key_t {
        std::string value;
        ~sensitive_validation_key_t() noexcept;
    };

    void setErrLocked(const std::string& msg);
    void publishAuthSnapshot(
        const std::vector<std::pair<std::string, aida::auth::auth_info_t>>& entries);

    template <typename State>
    void publishLoginStartFailure(const std::shared_ptr<State>& state,
                                  const char* error) noexcept;
    template <typename State>
    std::shared_ptr<State> allocateLoginState(const char* error) noexcept;
    bool codexFlowCurrentLocked(
        const std::shared_ptr<aida::auth::codex::codex_login_state_t>& state,
        std::uint64_t generation) noexcept;
    bool copilotFlowCurrentLocked(
        const std::shared_ptr<aida::auth::copilot::copilot_login_state_t>& state,
        std::uint64_t generation) noexcept;
    bool claudeFlowCurrentLocked(
        const std::shared_ptr<aida::auth::claude_code::claude_code_login_state_t>& state,
        std::uint64_t generation) noexcept;

    template <typename State, typename StartFn, typename CancelFn, typename ErrorFn>
    bool submitLoginStartTask(const char* label, const char* provider,
        login_start_control_t& control, const std::shared_ptr<State>& state,
        const std::shared_ptr<State>& previous, StartFn start_fn, CancelFn cancel_fn,
        ErrorFn error_fn);
    template <typename State, typename CancelFn>
    void submitProviderCancel(const char* label, const char* provider,
        const std::shared_ptr<State>& state, CancelFn cancel_fn);

    void pollBrowserOpenCompletion();
    template <typename State>
    void pollLoginStartControl(const char* provider, login_start_control_t& control,
        const std::shared_ptr<State>& state);
    void pollLoginStartups();
    void requestLoginStartCancel(login_start_control_t& control) noexcept;
    void pollActiveLogins();
    void onPollTick();
    void armPollTimer();
    bool validationActiveLocked() const;

    bool publishProviderValidation(const std::string& provider_id,
        const std::shared_ptr<provider_validation_ticket_t>& ticket,
        const test_result_t& result, bool terminal_reserved = false) noexcept;

    std::mutex mtx_;
    std::shared_ptr<aida::auth::codex::codex_login_state_t> codex_state_;
    std::shared_ptr<aida::auth::copilot::copilot_login_state_t> copilot_state_;
    std::shared_ptr<aida::auth::claude_code::claude_code_login_state_t> claude_code_state_;

    std::atomic<bool> codex_flow_open_{false};
    std::atomic<bool> copilot_flow_open_{false};
    std::atomic<bool> claude_code_flow_open_{false};

    login_start_control_t codex_start_;
    login_start_control_t copilot_start_;
    login_start_control_t claude_code_start_;

    std::atomic<bool> codex_exchange_in_flight_{false};
    std::atomic<bool> copilot_poll_in_flight_{false};
    std::atomic<bool> claude_code_exchange_in_flight_{false};
    std::atomic<std::uint64_t> codex_flow_generation_{0};
    std::atomic<std::uint64_t> copilot_flow_generation_{0};
    std::atomic<std::uint64_t> claude_code_flow_generation_{0};
    std::atomic<std::uint64_t> codex_exchange_task_id_{0};
    std::atomic<std::uint64_t> copilot_poll_task_id_{0};
    std::atomic<std::uint64_t> claude_code_exchange_task_id_{0};

    std::atomic<int> codex_exchange_result_{0};
    std::atomic<int> copilot_poll_result_{0};
    std::atomic<int> claude_code_exchange_result_{0};

    std::string err_;
    browser_open_control_t browser_open_;

    aida::events::subscription_handle_t sub_completed_;
    aida::events::subscription_handle_t sub_failed_;

    std::string last_completed_provider_;
    std::string last_completed_email_;
    std::atomic<bool> have_completed_event_{false};

    std::string last_failed_provider_;
    std::string last_failed_error_;
    std::atomic<bool> have_failed_event_{false};

    std::string copilot_ghe_buf_;
    std::atomic<bool> copilot_flow_started_{false};

    bool codex_success_played_ = false;
    bool copilot_success_played_ = false;
    bool claude_code_success_played_ = false;

    refresh_state_t refresh_;
    std::atomic<bool> shutdown_flag_{false};

    std::map<std::string, test_result_t> validate_results_;
    std::map<std::string, std::shared_ptr<provider_validation_ticket_t>> validate_in_flight_;
    std::atomic<std::uint64_t> validate_generation_{0};

    QTimer* poll_timer_ = nullptr;
};

class AidaOAuthProviderRow : public QWidget {
    Q_OBJECT
public:
    explicit AidaOAuthProviderRow(const oauth_provider_descriptor_t& descriptor,
                                  QWidget* parent = nullptr);

    void refreshStatus();

Q_SIGNALS:
    void signInRequested(const QString& provider_id);
    void signOutRequested(const QString& provider_id);

private:
    oauth_provider_descriptor_t descriptor_;
    QLabel* glyph_ = nullptr;
    QLabel* name_label_ = nullptr;
    QLabel* description_label_ = nullptr;
    QLabel* status_pill_ = nullptr;
    QPushButton* action_button_ = nullptr;
};

class AidaAuthView : public QWidget {
    Q_OBJECT
public:
    explicit AidaAuthView(QWidget* parent = nullptr);

    void focusProvider(const QString& provider_id);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void refreshValidationPresentation();
    void refreshProviderStatusPill();
    void refreshDefaultModelCombo();
    void syncKeyFieldForProvider(const QString& provider_id);
    void openOAuthDialog(const QString& provider_id);

    AidaAuthViewModel* model_ = nullptr;
    QToolButton* api_section_button_ = nullptr;
    QToolButton* oauth_section_button_ = nullptr;
    QStackedLayout* section_stack_ = nullptr;
    QComboBox* provider_combo_ = nullptr;
    QLabel* status_pill_ = nullptr;
    QLabel* status_detail_ = nullptr;
    QLineEdit* key_edit_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QPushButton* clear_button_ = nullptr;
    QPushButton* get_key_button_ = nullptr;
    QLabel* busy_label_ = nullptr;
    QLabel* result_label_ = nullptr;
    QLabel* default_model_label_ = nullptr;
    QComboBox* default_model_combo_ = nullptr;
    QPushButton* refresh_catalog_button_ = nullptr;
    QVBoxLayout* oauth_rows_layout_ = nullptr;
    bool loading_ = false;
};

void install_auth_domain(docking::AidaDockHost* host);

}
