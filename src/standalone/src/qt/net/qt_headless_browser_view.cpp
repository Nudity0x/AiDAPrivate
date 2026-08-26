#include "qt/net/qt_headless_browser_view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStandardPaths>
#include <QDir>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <cstring>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "core/infra/executor.hpp"
#include "core/infra/event_bus.hpp"
#include "core/network/burp/headless_view.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_pill.hpp"
#include "qt/widgets/aida_state_view.hpp"
#include "qt/widgets/aida_status.hpp"

namespace aida::qt::net {

namespace {

constexpr std::size_t kConsoleCacheCapacity = 200;
constexpr std::size_t kNetworkCacheCapacity = 50;
constexpr std::size_t kEvalOutputRenderCap = 8 * 1024;
constexpr std::uint64_t kPollIntervalMs = 750;

const char* kHookPresets[] = {
    "xss_sentinel",
    "alert_capture",
    "eval_capture",
    "function_capture",
    "setTimeout_capture",
    "location_capture",
};
constexpr int kHookPresetCount = static_cast<int>(sizeof(kHookPresets) / sizeof(kHookPresets[0]));

const char* kOsPresets[] = { "windows" };

const char* kLocalePresets[] = { "auto", "en-US", "en-GB", "de-DE", "fr-FR", "es-ES",
    "ja-JP", "zh-CN" };

const char* stateLabel(aida::burp::camoufox::bridge_state_t state)
{
    switch (state) {
    case aida::burp::camoufox::bridge_state_t::stopped:  return "Stopped";
    case aida::burp::camoufox::bridge_state_t::starting: return "Starting";
    case aida::burp::camoufox::bridge_state_t::ready:    return "Ready";
    case aida::burp::camoufox::bridge_state_t::error:    return "Error";
    }
    return "Unknown";
}

widgets::AidaSemantic stateSemantic(aida::burp::camoufox::bridge_state_t state)
{
    switch (state) {
    case aida::burp::camoufox::bridge_state_t::stopped:  return widgets::AidaSemantic::Neutral;
    case aida::burp::camoufox::bridge_state_t::starting: return widgets::AidaSemantic::Warning;
    case aida::burp::camoufox::bridge_state_t::ready:    return widgets::AidaSemantic::Success;
    case aida::burp::camoufox::bridge_state_t::error:    return widgets::AidaSemantic::Error;
    }
    return widgets::AidaSemantic::Neutral;
}

std::uint64_t nowMs()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

QString formatElapsed(std::uint64_t launchedMs)
{
    if (launchedMs == 0)
        return QStringLiteral("-");
    const std::uint64_t now = nowMs();
    if (now < launchedMs)
        return QStringLiteral("0s");
    const std::uint64_t diff = (now - launchedMs) / 1000ULL;
    if (diff < 60)
        return QStringLiteral("%1s").arg(static_cast<quint64>(diff));
    if (diff < 3600)
        return QStringLiteral("%1m %2s").arg(diff / 60).arg(diff % 60);
    return QStringLiteral("%1h %2m").arg(diff / 3600).arg((diff % 3600) / 60);
}

const char* installStateLabel(const aida::burp::camoufox::install::status_t& status)
{
    using aida::burp::camoufox::install::install_state_t;
    if (status.state == install_state_t::ok) return "Ready";
    if (status.python_path.empty())          return "Python missing";
    if (status.module_version.empty())       return "Module missing";
    if (status.browser_path.empty())         return "Browser missing";
    return "Not ready";
}

const char* installStateVariant(const aida::burp::camoufox::install::status_t& status)
{
    using aida::burp::camoufox::install::install_state_t;
    if (status.state == install_state_t::ok)
        return "success";
    if (status.python_path.empty() || status.module_version.empty() ||
        status.browser_path.empty())
        return "error";
    return "warning";
}

bool needInstallPanel(const aida::burp::camoufox::install::status_t& status)
{
    using aida::burp::camoufox::install::install_state_t;
    return status.state != install_state_t::ok;
}

constexpr qsizetype kLabelElideChars = 96;
constexpr qsizetype kCellDisplayChars = 2048;
constexpr qsizetype kCellTooltipChars = 4096;

QString elideMiddle(const QString& text, qsizetype cap)
{
    if (text.size() <= cap)
        return text;
    const qsizetype keep = cap - 1;
    const qsizetype head = keep / 2;
    return text.left(head) + QChar(0x2026) + text.right(keep - head);
}

QString capTail(const QString& text, qsizetype cap)
{
    if (text.size() <= cap)
        return text;
    return text.left(cap) + QChar(0x2026);
}

void setElidedLabelText(QLabel* label, const QString& full)
{
    const QString shown = elideMiddle(full, kLabelElideChars);
    label->setText(shown);
    label->setToolTip(shown.size() < full.size() ? full : QString());
}

bool stringEqLower(const std::string& a, const char* b)
{
    if (!b)
        return false;
    const std::size_t blen = std::strlen(b);
    if (a.size() != blen)
        return false;
    for (std::size_t i = 0; i < blen; ++i) {
        char ca = a[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
        char cb = b[i];
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
        if (ca != cb)
            return false;
    }
    return true;
}

QString consoleRowSummary(const nlohmann::json& j, QString& levelOut)
{
    std::string level;
    std::string text;
    std::string ts;
    try {
        if (j.is_object()) {
            if (j.contains("level") && j["level"].is_string())
                level = j["level"].get<std::string>();
            else if (j.contains("type") && j["type"].is_string())
                level = j["type"].get<std::string>();
            if (j.contains("text") && j["text"].is_string())
                text = j["text"].get<std::string>();
            else if (j.contains("message") && j["message"].is_string())
                text = j["message"].get<std::string>();
            else if (j.contains("args"))
                text = j["args"].dump();
            if (j.contains("timestamp")) {
                if (j["timestamp"].is_number_unsigned()) {
                    ts = std::to_string(j["timestamp"].get<std::uint64_t>());
                } else if (j["timestamp"].is_string()) {
                    ts = j["timestamp"].get<std::string>();
                }
            }
        } else if (j.is_string()) {
            text = j.get<std::string>();
        } else {
            text = j.dump();
        }
    } catch (...) {}
    if (level.empty())
        level = "log";
    levelOut = QString::fromStdString(level);
    QString out;
    if (!ts.empty())
        out += QStringLiteral("[%1] ").arg(QString::fromStdString(ts));
    out += QStringLiteral("[%1] %2")
        .arg(QString::fromStdString(level))
        .arg(QString::fromStdString(text));
    return out;
}

QColor consoleLevelColor(const QString& level)
{
    const auto& t = theme::tokens();
    const std::string levelStd = level.toStdString();
    if (stringEqLower(levelStd, "error") || stringEqLower(levelStd, "exception"))
        return t.error;
    if (stringEqLower(levelStd, "warn") || stringEqLower(levelStd, "warning"))
        return t.warning;
    if (stringEqLower(levelStd, "info"))
        return t.info;
    if (stringEqLower(levelStd, "debug") || stringEqLower(levelStd, "trace"))
        return t.text_dim;
    return t.text_primary;
}

QString networkRowSummary(const nlohmann::json& j, QString& statusOut, QString& methodOut,
                          std::uint64_t& lengthOut, std::uint64_t& timeOut)
{
    statusOut.clear();
    methodOut.clear();
    lengthOut = 0;
    timeOut = 0;
    std::string url;
    try {
        if (j.is_object()) {
            if (j.contains("url") && j["url"].is_string())
                url = j["url"].get<std::string>();
            if (j.contains("method") && j["method"].is_string())
                methodOut = QString::fromStdString(j["method"].get<std::string>());
            if (j.contains("status")) {
                if (j["status"].is_number_unsigned())
                    statusOut = QString::number(j["status"].get<unsigned>());
                else if (j["status"].is_string())
                    statusOut = QString::fromStdString(j["status"].get<std::string>());
            }
            if (j.contains("response_size") && j["response_size"].is_number_unsigned())
                lengthOut = j["response_size"].get<std::uint64_t>();
            else if (j.contains("length") && j["length"].is_number_unsigned())
                lengthOut = j["length"].get<std::uint64_t>();
            else if (j.contains("size") && j["size"].is_number_unsigned())
                lengthOut = j["size"].get<std::uint64_t>();
            if (j.contains("duration_ms") && j["duration_ms"].is_number_unsigned())
                timeOut = j["duration_ms"].get<std::uint64_t>();
            else if (j.contains("time_ms") && j["time_ms"].is_number_unsigned())
                timeOut = j["time_ms"].get<std::uint64_t>();
        }
    } catch (...) {}
    return QString::fromStdString(url);
}

// The lifecycle core is TU-static and thread-agnostic exactly like the ImGui
// g_state: burp_module.cpp:177/213 and the Test Lab call headless_view::
// initialize/shutdown/last_error directly, potentially before the Qt
// controller exists and from non-GUI threads. Only cold-path state lives here
// (the event-bus publisher-thread callback updates latest_event under its
// mutex); all hot view-facing state is GUI-owned by the controller with no
// locks (worker results arrive through queued invokeMethod).
struct headless_core_t {
    std::atomic<bool> initialized{false};
    std::mutex subscription_mutex;
    aida::events::subscription_handle_t subscription;
    std::mutex error_mutex;
    std::string last_error;
    std::mutex event_mutex;
    aida::burp::camoufox::bridge_state_t latest_state =
        aida::burp::camoufox::bridge_state_t::stopped;
    std::string latest_error;
    std::uint32_t latest_child_pid = 0;
};

headless_core_t& headlessCore()
{
    static headless_core_t core;
    return core;
}

void setCoreError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(headlessCore().error_mutex);
    headlessCore().last_error = message;
}

void onBridgeStateChanged(const aida::burp::camoufox::bridge_state_changed_t& ev)
{
    {
        std::lock_guard<std::mutex> lock(headlessCore().event_mutex);
        headlessCore().latest_state = ev.state;
        headlessCore().latest_error = ev.last_error;
        headlessCore().latest_child_pid = ev.child_pid;
    }
    ::diag::log_tagged_fmt("headless_v", "bridge_state_changed state=%s pid=%u",
        stateLabel(ev.state), static_cast<unsigned>(ev.child_pid));
    if (auto* controller = QtHeadlessBrowserController::instance()) {
        const int state = static_cast<int>(ev.state);
        const QString error = QString::fromStdString(ev.last_error);
        const std::uint32_t pid = ev.child_pid;
        QMetaObject::invokeMethod(controller,
            [state, error, pid] {
                if (auto* self = QtHeadlessBrowserController::instance())
                    self->applyBridgeState(state, error, pid);
            }, Qt::QueuedConnection);
    }
}

QString computeScreenshotPath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty())
        base = QStringLiteral("C:/Users/Public/Downloads");
    QDir().mkpath(base);
    const std::uint64_t ts = nowMs() / 1000ULL;
    return QStringLiteral("%1/camoufox_%2.png")
        .arg(base)
        .arg(static_cast<quint64>(ts));
}

void openInExplorer(const QString& path)
{
    if (path.isEmpty())
        return;
    const QString nativePath = QDir::toNativeSeparators(path);
    std::wstring wpath = nativePath.toStdWString();
    std::wstring args = L"/select,\"" + wpath + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

}

QtHeadlessBrowserController* QtHeadlessBrowserController::instance_ = nullptr;

void QtHeadlessBrowserController::registerInstance(QtHeadlessBrowserController* controller) noexcept
{
    instance_ = controller;
}

void QtHeadlessBrowserController::clearInstance(QtHeadlessBrowserController* controller) noexcept
{
    if (instance_ == controller)
        instance_ = nullptr;
}

QtHeadlessBrowserController::QtHeadlessBrowserController(QObject* parent)
    : QObject(parent)
{
    {
        std::lock_guard<std::mutex> lock(headlessCore().event_mutex);
        bridge_status_.state = headlessCore().latest_state;
        bridge_status_.last_error = headlessCore().latest_error;
        bridge_status_.child_pid = headlessCore().latest_child_pid;
    }
}

QtHeadlessBrowserController::~QtHeadlessBrowserController()
{
    clearInstance(this);
}

bool QtHeadlessBrowserController::initialize()
{
    return aida::burp::headless_view::initialize();
}

void QtHeadlessBrowserController::shutdown()
{
    aida::burp::headless_view::shutdown();
}

std::string QtHeadlessBrowserController::lastError()
{
    return aida::burp::headless_view::last_error();
}

void QtHeadlessBrowserController::appendInstallLog(const QString& line)
{
    Q_EMIT installLogAppended(line);
}

void QtHeadlessBrowserController::applyBridgeState(int state, const QString& lastError,
                                                   std::uint32_t childPid)
{
    bridge_status_.state = static_cast<aida::burp::camoufox::bridge_state_t>(state);
    bridge_status_.last_error = lastError.toStdString();
    bridge_status_.child_pid = childPid;
    Q_EMIT bridgeStateChanged();
}

std::shared_ptr<const std::vector<nlohmann::json>> QtHeadlessBrowserController::consoleCache() const
{
    return std::atomic_load_explicit(&console_cache_, std::memory_order_acquire);
}

std::shared_ptr<const std::vector<nlohmann::json>> QtHeadlessBrowserController::networkCache() const
{
    return std::atomic_load_explicit(&network_cache_, std::memory_order_acquire);
}

void QtHeadlessBrowserController::scheduleStatusPoll()
{
    bool expected = false;
    if (!poll_in_flight_.compare_exchange_strong(expected, true))
        return;
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.status_poll";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard]() {
        auto* controller = guard.data();
        aida::burp::camoufox::bridge_status_t status =
            aida::burp::camoufox::get_status();
        std::shared_ptr<const std::vector<nlohmann::json>> consoleRows;
        std::shared_ptr<const std::vector<nlohmann::json>> networkRows;
        bool haveConsole = false;
        bool haveNetwork = false;
        QString pageUrl;
        if (status.state == aida::burp::camoufox::bridge_state_t::ready) {
            auto console = aida::burp::camoufox::get_console_logs(kConsoleCacheCapacity);
            if (console.ok) {
                std::vector<nlohmann::json> rows;
                try {
                    if (console.data.is_array()) {
                        for (const auto& it : console.data)
                            rows.push_back(it);
                    } else if (console.data.is_object() && console.data.contains("logs") &&
                               console.data["logs"].is_array()) {
                        for (const auto& it : console.data["logs"])
                            rows.push_back(it);
                    } else if (console.data.is_object() && console.data.contains("entries") &&
                               console.data["entries"].is_array()) {
                        for (const auto& it : console.data["entries"])
                            rows.push_back(it);
                    }
                } catch (...) {}
                consoleRows = std::make_shared<const std::vector<nlohmann::json>>(
                    std::move(rows));
                haveConsole = true;
            }
            auto network = aida::burp::camoufox::list_network_requests(kNetworkCacheCapacity);
            if (network.ok) {
                std::vector<nlohmann::json> rows;
                try {
                    if (network.data.is_array()) {
                        for (const auto& it : network.data)
                            rows.push_back(it);
                    } else if (network.data.is_object() && network.data.contains("requests") &&
                               network.data["requests"].is_array()) {
                        for (const auto& it : network.data["requests"])
                            rows.push_back(it);
                    } else if (network.data.is_object() && network.data.contains("entries") &&
                               network.data["entries"].is_array()) {
                        for (const auto& it : network.data["entries"])
                            rows.push_back(it);
                    }
                } catch (...) {}
                networkRows = std::make_shared<const std::vector<nlohmann::json>>(
                    std::move(rows));
                haveNetwork = true;
            }
            auto info = aida::burp::camoufox::get_page_info();
            if (info.ok) {
                try {
                    if (info.data.is_object() && info.data.contains("url") &&
                        info.data["url"].is_string())
                        pageUrl = QString::fromStdString(info.data["url"].get<std::string>());
                } catch (...) {}
            }
        }
        if (!controller)
            return;
        QMetaObject::invokeMethod(controller,
            [guard, status = std::move(status), consoleRows = std::move(consoleRows),
             networkRows = std::move(networkRows), haveConsole, haveNetwork,
             pageUrl = std::move(pageUrl)]() mutable {
                auto* self = guard.data();
                if (!self)
                    return;
                self->bridge_status_ = std::move(status);
                if (!pageUrl.isEmpty())
                    self->bridge_status_.active_page_url = pageUrl.toStdString();
                if (haveConsole) {
                    std::atomic_store_explicit(&self->console_cache_,
                        std::move(consoleRows), std::memory_order_release);
                    ++self->console_signature_;
                }
                if (haveNetwork) {
                    std::atomic_store_explicit(&self->network_cache_,
                        std::move(networkRows), std::memory_order_release);
                    ++self->network_signature_;
                }
                self->poll_in_flight_.store(false, std::memory_order_release);
                Q_EMIT self->bridgeStateChanged();
                Q_EMIT self->logsChanged();
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted)
        poll_in_flight_.store(false, std::memory_order_release);
}

void QtHeadlessBrowserController::scheduleInstallProbe(bool alsoLog)
{
    bool expected = false;
    if (!install_poll_in_flight_.compare_exchange_strong(expected, true))
        return;
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.install_probe";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, alsoLog]() {
        auto* controller = guard.data();
        aida::burp::camoufox::install::status_t status =
            aida::burp::camoufox::install::probe();
        if (!controller)
            return;
        QMetaObject::invokeMethod(controller,
            [guard, status = std::move(status), alsoLog]() mutable {
                auto* self = guard.data();
                if (!self)
                    return;
                self->install_status_ = std::move(status);
                if (alsoLog && !self->install_status_.last_message.empty())
                    self->appendInstallLog(QString::fromStdString(
                        self->install_status_.last_message));
                if (!self->install_panel_user_toggled_) {
                    const bool show = needInstallPanel(self->install_status_);
                    if (show != self->show_install_panel_) {
                        self->show_install_panel_ = show;
                        Q_EMIT self->installPanelAutoChanged(show);
                    }
                }
                self->install_poll_in_flight_.store(false, std::memory_order_release);
                Q_EMIT self->installStatusChanged();
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted)
        install_poll_in_flight_.store(false, std::memory_order_release);
}

void QtHeadlessBrowserController::installModule()
{
    ::diag::log_tagged("headless_v", "install_module_start");
    appendInstallLog(QStringLiteral("[install_module] starting pip install"));
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.install_module";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard]() {
        std::string log;
        bool ok = false;
        try { ok = aida::burp::camoufox::install::pip_install_module(log); }
        catch (...) { ok = false; }
        ::diag::log_tagged_fmt("headless_v", "install_module_result ok=%d", ok ? 1 : 0);
        std::string detail = aida::burp::camoufox::install::last_error();
        if (!ok && detail.empty())
            detail = "pip install returned false";
        const QString line = ok ? QStringLiteral("[install_module] completed")
            : QStringLiteral("[install_module] %1").arg(QString::fromStdString(detail));
        if (auto* self = guard.data()) {
            QMetaObject::invokeMethod(self, [self, line]() {
                self->appendInstallLog(line);
            }, Qt::QueuedConnection);
            self->scheduleInstallProbe(true);
        }
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtHeadlessBrowserController::fetchBrowser()
{
    ::diag::log_tagged("headless_v", "fetch_browser_start");
    appendInstallLog(QStringLiteral("[fetch_browser] starting download"));
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.fetch_browser";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard]() {
        std::string log;
        bool ok = false;
        try { ok = aida::burp::camoufox::install::fetch_browser(log); }
        catch (...) { ok = false; }
        ::diag::log_tagged_fmt("headless_v", "fetch_browser_result ok=%d", ok ? 1 : 0);
        std::string detail = aida::burp::camoufox::install::last_error();
        if (!ok && detail.empty())
            detail = "browser fetch returned false";
        const QString line = ok ? QStringLiteral("[fetch_browser] completed")
            : QStringLiteral("[fetch_browser] %1").arg(QString::fromStdString(detail));
        if (auto* self = guard.data()) {
            QMetaObject::invokeMethod(self, [self, line]() {
                self->appendInstallLog(line);
            }, Qt::QueuedConnection);
            self->scheduleInstallProbe(true);
        }
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtHeadlessBrowserController::startBridge(bool headless, bool humanize, bool blockImages,
                                              const QString& osPreset,
                                              const QString& localePreset)
{
    aida::burp::camoufox::launch_config_t config;
    config.headless = headless;
    config.humanize = humanize;
    config.block_images = blockImages;
    config.block_webrtc = true;
    if (!osPreset.isEmpty())
        config.os = osPreset.toStdString();
    if (!localePreset.isEmpty())
        config.locale = localePreset.toStdString();
    if (!install_status_.python_path.empty())
        config.python_executable = install_status_.python_path;
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.start_bridge";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, config]() {
        bool ok = false;
        try { ok = aida::burp::camoufox::start_bridge(config); } catch (...) { ok = false; }
        if (!ok) {
            std::string message = aida::burp::camoufox::last_error();
            if (message.empty())
                message = "start_bridge returned false";
            setCoreError(message);
            ::diag::log_tagged_fmt("headless_v", "start_bridge_failed msg='%s'",
                message.c_str());
        } else {
            ::diag::log_tagged("headless_v", "start_bridge_completed");
        }
        if (auto* self = guard.data())
            self->scheduleStatusPoll();
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtHeadlessBrowserController::stopBridge()
{
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.stop_bridge";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard]() {
        bool ok = false;
        try { ok = aida::burp::camoufox::stop_bridge("headless_view.stop"); }
        catch (...) { ok = false; }
        if (!ok) {
            std::string message = aida::burp::camoufox::last_error();
            if (message.empty())
                message = "stop_bridge returned false";
            setCoreError(message);
            ::diag::log_tagged_fmt("headless_v", "stop_bridge_failed msg='%s'",
                message.c_str());
        } else {
            ::diag::log_tagged("headless_v", "stop_bridge_completed");
        }
        if (auto* self = guard.data())
            self->scheduleStatusPoll();
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtHeadlessBrowserController::resetState()
{
    ::diag::log_tagged("headless_v", "reset_state");
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.reset_state";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard]() {
        bool ok = false;
        try { ok = aida::burp::camoufox::reset_browser_state(); } catch (...) { ok = false; }
        ::diag::log_tagged_fmt("headless_v", "reset_state_result ok=%d", ok ? 1 : 0);
        if (!ok) {
            std::string message = aida::burp::camoufox::last_error();
            if (message.empty())
                message = "reset_browser_state returned false";
            setCoreError(message);
        }
        if (auto* self = guard.data())
            self->scheduleStatusPoll();
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtHeadlessBrowserController::navigate(const QString& url)
{
    if (url.isEmpty()) {
        setCoreError("navigate: empty url");
        return;
    }
    const std::string target = url.toStdString();
    ::diag::log_tagged_fmt("headless_v", "navigate url='%s'", target.c_str());
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.navigate";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, target]() {
        bool ok = false;
        try {
            ok = aida::burp::camoufox::navigate(target, std::string("load"), 30000);
        } catch (...) { ok = false; }
        ::diag::log_tagged_fmt("headless_v", "navigate_result ok=%d url='%s'", ok ? 1 : 0,
            target.c_str());
        if (!ok) {
            std::string message = aida::burp::camoufox::last_error();
            if (message.empty())
                message = "navigate returned false";
            setCoreError(message);
        }
        if (auto* self = guard.data())
            self->scheduleStatusPoll();
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtHeadlessBrowserController::reload()
{
    ::diag::log_tagged("headless_v", "reload");
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.reload";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard]() {
        bool ok = false;
        try { ok = aida::burp::camoufox::reload(std::string("load")); } catch (...) { ok = false; }
        ::diag::log_tagged_fmt("headless_v", "reload_result ok=%d", ok ? 1 : 0);
        if (!ok) {
            std::string message = aida::burp::camoufox::last_error();
            if (message.empty())
                message = "reload returned false";
            setCoreError(message);
        }
        if (auto* self = guard.data())
            self->scheduleStatusPoll();
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtHeadlessBrowserController::takeScreenshot()
{
    const QString out = computeScreenshotPath();
    ::diag::log_tagged_fmt("headless_v", "screenshot path='%s'", out.toUtf8().constData());
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.screenshot";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, out]() {
        bool ok = false;
        try { ok = aida::burp::camoufox::take_screenshot(out.toStdString(), true); }
        catch (...) { ok = false; }
        ::diag::log_tagged_fmt("headless_v", "screenshot_result ok=%d path='%s'", ok ? 1 : 0,
            out.toUtf8().constData());
        if (!ok) {
            std::string message = aida::burp::camoufox::last_error();
            if (message.empty())
                message = "take_screenshot returned false";
            setCoreError(message);
        }
        if (auto* self = guard.data()) {
            QMetaObject::invokeMethod(self, [self, out, ok]() {
                if (ok) {
                    self->last_screenshot_path_ = out;
                    Q_EMIT self->screenshotChanged();
                }
            }, Qt::QueuedConnection);
        }
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtHeadlessBrowserController::injectHookPreset(const QString& preset)
{
    const std::string name = preset.toStdString();
    ::diag::log_tagged_fmt("headless_v", "inject_hook preset='%s'", name.c_str());
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.inject_preset";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, name]() {
        bool ok = false;
        try { ok = aida::burp::camoufox::inject_hook_preset(name); } catch (...) { ok = false; }
        ::diag::log_tagged_fmt("headless_v", "inject_hook_result ok=%d preset='%s'",
            ok ? 1 : 0, name.c_str());
        if (!ok) {
            std::string message = aida::burp::camoufox::last_error();
            if (message.empty())
                message = "inject_hook_preset returned false";
            setCoreError(message);
        }
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtHeadlessBrowserController::removeAllHooks()
{
    ::diag::log_tagged("headless_v", "remove_hooks");
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.remove_hooks";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard]() {
        bool ok = false;
        try { ok = aida::burp::camoufox::remove_hooks(); } catch (...) { ok = false; }
        ::diag::log_tagged_fmt("headless_v", "remove_hooks_result ok=%d", ok ? 1 : 0);
        if (!ok) {
            std::string message = aida::burp::camoufox::last_error();
            if (message.empty())
                message = "remove_hooks returned false";
            setCoreError(message);
        }
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtHeadlessBrowserController::evaluateJs(const QString& expression)
{
    if (expression.isEmpty()) {
        eval_output_ = QStringLiteral("[error] expression is empty");
        Q_EMIT evalOutputChanged();
        return;
    }
    ::diag::log_tagged_fmt("headless_v", "evaluate_js expr_len=%zu",
        static_cast<std::size_t>(expression.toStdString().size()));
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.evaluate_js";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, expression]() {
        aida::burp::camoufox::call_result_t result;
        try {
            result = aida::burp::camoufox::evaluate_js(expression.toStdString(), true);
        } catch (...) { result.ok = false; result.error = "evaluate_js threw"; }
        ::diag::log_tagged_fmt("headless_v", "evaluate_js_result ok=%d error='%s'",
            result.ok ? 1 : 0, result.ok ? "" : result.error.c_str());
        QString text;
        if (!result.ok) {
            text = QStringLiteral("[error] %1").arg(result.error.empty()
                ? QStringLiteral("evaluate_js failed")
                : QString::fromStdString(result.error));
        } else {
            try {
                if (!result.text.empty())
                    text = QString::fromStdString(result.text);
                else if (!result.data.is_null())
                    text = QString::fromStdString(result.data.dump(2));
                else
                    text = QStringLiteral("(no result)");
            } catch (...) {
                text = QStringLiteral("[error] response not serialisable");
            }
        }
        if (text.size() > static_cast<qsizetype>(kEvalOutputRenderCap)) {
            text.resize(static_cast<qsizetype>(kEvalOutputRenderCap));
            text.append(QStringLiteral("\n[truncated]"));
        }
        if (auto* self = guard.data()) {
            QMetaObject::invokeMethod(self, [self, text]() {
                self->eval_output_ = text;
                Q_EMIT self->evalOutputChanged();
            }, Qt::QueuedConnection);
        }
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtHeadlessBrowserController::addInitScript(const QString& script)
{
    if (script.isEmpty()) {
        setCoreError("add_init_script: empty");
        return;
    }
    ::diag::log_tagged_fmt("headless_v", "add_init_script js_len=%zu",
        static_cast<std::size_t>(script.toStdString().size()));
    QPointer<QtHeadlessBrowserController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.headless_view";
    submission.label = "headless.add_init_script";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, script]() {
        bool ok = false;
        try { ok = aida::burp::camoufox::add_init_script(script.toStdString()); }
        catch (...) { ok = false; }
        ::diag::log_tagged_fmt("headless_v", "add_init_script_result ok=%d", ok ? 1 : 0);
        if (!ok) {
            std::string message = aida::burp::camoufox::last_error();
            if (message.empty())
                message = "add_init_script returned false";
            setCoreError(message);
        }
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtHeadlessBrowserController::clearConsoleCache()
{
    ::diag::log_tagged("headless_v", "clear_console_cache");
    std::atomic_store_explicit(&console_cache_,
        std::make_shared<const std::vector<nlohmann::json>>(), std::memory_order_release);
    ++console_signature_;
    Q_EMIT logsChanged();
}

void QtHeadlessBrowserController::clearNetworkCache()
{
    ::diag::log_tagged("headless_v", "clear_network_cache");
    std::atomic_store_explicit(&network_cache_,
        std::make_shared<const std::vector<nlohmann::json>>(), std::memory_order_release);
    ++network_signature_;
    Q_EMIT logsChanged();
}

void QtHeadlessBrowserController::clearEvalOutput()
{
    ::diag::log_tagged("headless_v", "eval_clear");
    eval_output_.clear();
    Q_EMIT evalOutputChanged();
}

void QtHeadlessBrowserController::setInstallPanelUserToggled(bool visible)
{
    install_panel_user_toggled_ = true;
    show_install_panel_ = visible;
}

QtJsonLogModel::QtJsonLogModel(Kind kind, QObject* parent)
    : QAbstractTableModel(parent), kind_(kind) {}

void QtJsonLogModel::adopt(std::shared_ptr<const std::vector<nlohmann::json>> rows,
                           std::uint64_t signature)
{
    if (rows_ && rows && *rows_ == *rows)
        return;
    beginResetModel();
    rows_ = std::move(rows);
    signature_ = signature;
    endResetModel();
}

int QtJsonLogModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() || !rows_ ? 0 : static_cast<int>(rows_->size());
}

int QtJsonLogModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : (kind_ == Kind::Console ? 1 : 5);
}

QVariant QtJsonLogModel::data(const QModelIndex& index, int role) const
{
    if (!rows_ || !index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(rows_->size()))
        return {};
    const auto& row = rows_->at(static_cast<std::size_t>(index.row()));
    const auto& t = theme::tokens();
    if (kind_ == Kind::Console) {
        QString level;
        const QString line = consoleRowSummary(row, level);
        if (role == Qt::DisplayRole)
            return capTail(line, kCellDisplayChars);
        if (role == Qt::ForegroundRole)
            return consoleLevelColor(level);
        if (role == Qt::ToolTipRole)
            return capTail(line, kCellTooltipChars);
        return {};
    }
    QString status, method;
    std::uint64_t length = 0, timeMs = 0;
    const QString url = networkRowSummary(row, status, method, length, timeMs);
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return method.isEmpty() ? QStringLiteral("-") : method;
        case 1: return status.isEmpty() ? QStringLiteral("-") : status;
        case 2: return QString::number(static_cast<quint64>(length));
        case 3: return QStringLiteral("%1ms").arg(static_cast<quint64>(timeMs));
        case 4: return capTail(url, kCellDisplayChars);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        if (index.column() == 1 && !status.isEmpty()) {
            const int code = status.toInt();
            if (code >= 500) return t.error;
            if (code >= 400) return t.warning;
            if (code >= 300) return t.info;
            if (code >= 200) return t.success;
        }
        return t.text_primary;
    }
    if (role == Qt::ToolTipRole && index.column() == 4)
        return capTail(url, kCellTooltipChars);
    return {};
}

QVariant QtJsonLogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    if (kind_ == Kind::Console)
        return section == 0 ? QStringLiteral("Console") : QVariant{};
    switch (section) {
    case 0: return QStringLiteral("Method");
    case 1: return QStringLiteral("Status");
    case 2: return QStringLiteral("Bytes");
    case 3: return QStringLiteral("Time");
    case 4: return QStringLiteral("URL");
    default: return {};
    }
}

QtHeadlessBrowserView::QtHeadlessBrowserView(QWidget* parent)
    : NetworkPaneBase(parent)
{
    setObjectName(QStringLiteral("view.network.headless"));
    setRequiresTarget(false);

    controller_ = QtHeadlessBrowserController::instance();
    if (!controller_)
        controller_ = new QtHeadlessBrowserController(this);
    QtHeadlessBrowserController::registerInstance(controller_);

    const auto& t = theme::tokens();
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.xs);

    auto* header = new QFrame(content);
    header->setObjectName(QStringLiteral("view.network.headless.header"));
    header->setProperty("aidaRole", QStringLiteral("header"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(t.spacing.sm, t.spacing.xs, t.spacing.sm, t.spacing.xs);
    headerLayout->setSpacing(t.spacing.sm);
    statusDot_ = new widgets::AidaStatusDot(header);
    statusDot_->setObjectName(QStringLiteral("view.network.headless.status_dot"));
    statusDot_->setDotRadius(t.status_bar.dot);
    statusDot_->setPulsing(false);
    headerLayout->addWidget(statusDot_);
    auto* titleStack = new QVBoxLayout();
    titleStack->setSpacing(0);
    statusTitle_ = new QLabel(QStringLiteral("Camoufox Headless Browser"), header);
    statusTitle_->setFont(theme::fonts::strong());
    titleStack->addWidget(statusTitle_);
    statusSub_ = new QLabel(header);
    statusSub_->setProperty("aidaVariant", QStringLiteral("secondary"));
    titleStack->addWidget(statusSub_);
    headerLayout->addLayout(titleStack);
    headerLayout->addStretch(1);
    auto* rightStack = new QVBoxLayout();
    rightStack->setSpacing(0);
    statusRight_ = new QLabel(header);
    statusRight_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    rightStack->addWidget(statusRight_);
    statusRightSub_ = new QLabel(header);
    statusRightSub_->setProperty("aidaVariant", QStringLiteral("secondary"));
    statusRightSub_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    rightStack->addWidget(statusRightSub_);
    headerLayout->addLayout(rightStack);
    layout->addWidget(header);

    auto* pillRow = new QHBoxLayout();
    pillRow->setSpacing(t.spacing.xs);
    camoufoxPill_ = new widgets::AidaPill(QStringLiteral("Camoufox only"),
        widgets::AidaSemantic::Success, content);
    pillRow->addWidget(camoufoxPill_);
    webrtcPill_ = new widgets::AidaPill(QStringLiteral("WebRTC blocked"),
        widgets::AidaSemantic::Neutral, content);
    pillRow->addWidget(webrtcPill_);
    nativeUaPill_ = new widgets::AidaPill(QStringLiteral("Native UA"),
        widgets::AidaSemantic::Neutral, content);
    pillRow->addWidget(nativeUaPill_);
    pageVerifiedPill_ = new widgets::AidaPill(QStringLiteral("Page verified"),
        widgets::AidaSemantic::Neutral, content);
    pillRow->addWidget(pageVerifiedPill_);
    privacyPill_ = new widgets::AidaPill(QStringLiteral("Privacy verified"),
        widgets::AidaSemantic::Neutral, content);
    pillRow->addWidget(privacyPill_);
    pillRow->addStretch(1);
    layout->addLayout(pillRow);

    auto* installToggleRow = new QHBoxLayout();
    installToggleRow->setSpacing(t.spacing.xs);
    installToggle_ = new QPushButton(QStringLiteral("Show install panel"), content);
    installToggle_->setObjectName(QStringLiteral("view.network.headless.install_toggle"));
    installToggle_->setToolTip(QStringLiteral(
        "Show or hide the Camoufox install diagnostics panel"));
    installToggleRow->addWidget(installToggle_);
    installStateLabel_ = new QLabel(content);
    installToggleRow->addWidget(installStateLabel_);
    installToggleRow->addStretch(1);
    layout->addLayout(installToggleRow);

    installPanel_ = buildInstallPanel(content);
    layout->addWidget(installPanel_);

    auto* controlsRow = new QHBoxLayout();
    controlsRow->setSpacing(t.spacing.xs);
    startButton_ = new widgets::AidaButton(QStringLiteral("Start"), content);
    startButton_->setObjectName(QStringLiteral("view.network.headless.start"));
    startButton_->setKind(widgets::AidaButton::Kind::Primary);
    startButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    controlsRow->addWidget(startButton_);
    stopButton_ = new widgets::AidaButton(QStringLiteral("Stop"), content);
    stopButton_->setObjectName(QStringLiteral("view.network.headless.stop"));
    stopButton_->setKind(widgets::AidaButton::Kind::Destructive);
    stopButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    controlsRow->addWidget(stopButton_);
    resetButton_ = new widgets::AidaButton(QStringLiteral("Reset State"), content);
    resetButton_->setObjectName(QStringLiteral("view.network.headless.reset"));
    resetButton_->setKind(widgets::AidaButton::Kind::Secondary);
    resetButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    controlsRow->addWidget(resetButton_);
    headlessCheck_ = new QCheckBox(QStringLiteral("Headless"), content);
    headlessCheck_->setObjectName(QStringLiteral("view.network.headless.opt_headless"));
    headlessCheck_->setToolTip(QStringLiteral("Run Camoufox without a visible browser window"));
    controlsRow->addWidget(headlessCheck_);
    humanizeCheck_ = new QCheckBox(QStringLiteral("Humanize"), content);
    humanizeCheck_->setObjectName(QStringLiteral("view.network.headless.opt_humanize"));
    humanizeCheck_->setToolTip(QStringLiteral(
        "Add human-like timing and cursor behavior to page interactions"));
    controlsRow->addWidget(humanizeCheck_);
    blockImagesCheck_ = new QCheckBox(QStringLiteral("Block Images"), content);
    blockImagesCheck_->setObjectName(QStringLiteral("view.network.headless.opt_block_images"));
    blockImagesCheck_->setToolTip(QStringLiteral(
        "Skip loading images (faster navigation, less bandwidth)"));
    controlsRow->addWidget(blockImagesCheck_);
    controlsRow->addStretch(1);
    layout->addLayout(controlsRow);

    auto* urlRow = new QHBoxLayout();
    urlRow->setSpacing(t.spacing.xs);
    urlEdit_ = new QLineEdit(QStringLiteral("https://example.com"), content);
    urlEdit_->setObjectName(QStringLiteral("view.network.headless.url"));
    urlEdit_->setMaxLength(2047);
    urlEdit_->setPlaceholderText(QStringLiteral("https://example.com"));
    urlEdit_->setClearButtonEnabled(true);
    urlRow->addWidget(urlEdit_, 1);
    navigateButton_ = new widgets::AidaButton(QStringLiteral("Navigate"), content);
    navigateButton_->setObjectName(QStringLiteral("view.network.headless.navigate"));
    navigateButton_->setKind(widgets::AidaButton::Kind::Primary);
    navigateButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    urlRow->addWidget(navigateButton_);
    reloadButton_ = new widgets::AidaButton(QStringLiteral("Reload"), content);
    reloadButton_->setObjectName(QStringLiteral("view.network.headless.reload"));
    reloadButton_->setKind(widgets::AidaButton::Kind::Secondary);
    reloadButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    urlRow->addWidget(reloadButton_);
    screenshotButton_ = new widgets::AidaButton(QStringLiteral("Screenshot"), content);
    screenshotButton_->setObjectName(QStringLiteral("view.network.headless.screenshot"));
    screenshotButton_->setKind(widgets::AidaButton::Kind::Secondary);
    screenshotButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    urlRow->addWidget(screenshotButton_);
    layout->addLayout(urlRow);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(buildLeftPane(splitter));
    splitter->addWidget(buildRightPane(splitter));
    splitter->setStretchFactor(0, 60);
    splitter->setStretchFactor(1, 40);
    layout->addWidget(splitter, 1);

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(static_cast<int>(kPollIntervalMs));
    connect(pollTimer_, &QTimer::timeout, this, [this] {
        controller_->scheduleStatusPoll();
        if (installPanel_->isVisible() || !controller_->installPanelUserToggled())
            controller_->scheduleInstallProbe(false);
    });

    connect(installToggle_, &QPushButton::clicked, this, [this] {
        const bool next = !installPanel_->isVisible();
        installPanel_->setVisible(next);
        installToggle_->setText(next ? QStringLiteral("Hide install panel")
                                     : QStringLiteral("Show install panel"));
        controller_->setInstallPanelUserToggled(next);
    });
    connect(startButton_, &QAbstractButton::clicked, this, [this] {
        controller_->startBridge(headlessCheck_->isChecked(), humanizeCheck_->isChecked(),
            blockImagesCheck_->isChecked(),
            osCombo_->currentText(), localeCombo_->currentText());
    });
    connect(stopButton_, &QAbstractButton::clicked, this, [this] {
        controller_->stopBridge();
    });
    connect(resetButton_, &QAbstractButton::clicked, this, [this] {
        controller_->resetState();
    });
    const auto submitNavigate = [this] { controller_->navigate(urlEdit_->text()); };
    connect(navigateButton_, &QAbstractButton::clicked, this, submitNavigate);
    connect(urlEdit_, &QLineEdit::returnPressed, this, submitNavigate);
    connect(reloadButton_, &QAbstractButton::clicked, this, [this] {
        controller_->reload();
    });
    connect(screenshotButton_, &QAbstractButton::clicked, this, [this] {
        controller_->takeScreenshot();
    });
    connect(controller_, &QtHeadlessBrowserController::bridgeStateChanged, this, [this] {
        refreshStatusHeader();
        refreshBridgeControls();
        refreshPageInfo();
    });
    connect(controller_, &QtHeadlessBrowserController::installStatusChanged, this, [this] {
        refreshInstallPanel();
        refreshBridgeControls();
    });
    connect(controller_, &QtHeadlessBrowserController::logsChanged, this, [this] {
        applyConsoleSnapshot();
        applyNetworkSnapshot();
    });
    connect(controller_, &QtHeadlessBrowserController::evalOutputChanged, this,
        &QtHeadlessBrowserView::refreshEvalOutput);
    connect(controller_, &QtHeadlessBrowserController::screenshotChanged, this,
        &QtHeadlessBrowserView::refreshScreenshot);
    connect(controller_, &QtHeadlessBrowserController::installPanelAutoChanged, this,
        [this](bool visible) {
            installPanel_->setVisible(visible);
            installToggle_->setText(visible ? QStringLiteral("Hide install panel")
                                            : QStringLiteral("Show install panel"));
        });
    connect(controller_, &QtHeadlessBrowserController::installLogAppended, this,
        [this](const QString& line) {
            if (!line.isEmpty())
                installLog_->appendPlainText(line);
        });

    setContent(content);
    installPanel_->setVisible(controller_->installPanelAutoShow());
    installToggle_->setText(installPanel_->isVisible()
        ? QStringLiteral("Hide install panel") : QStringLiteral("Show install panel"));
    refreshStatusHeader();
    refreshInstallPanel();
    refreshBridgeControls();
    refreshPageInfo();
    applyConsoleSnapshot();
    applyNetworkSnapshot();
    refreshEvalOutput();
    refreshScreenshot();
}

QWidget* QtHeadlessBrowserView::buildInstallPanel(QWidget* parent)
{
    const auto& t = theme::tokens();
    auto* panel = new QFrame(parent);
    panel->setObjectName(QStringLiteral("view.network.headless.install_panel"));
    panel->setProperty("aidaRole", QStringLiteral("card"));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(t.spacing.sm, t.spacing.sm, t.spacing.sm, t.spacing.sm);
    layout->setSpacing(t.spacing.xs);

    const auto installRow = [&t](QWidget* rowParent, widgets::AidaStatusDot*& dot,
                                 QLabel*& label, QLabel*& detail, const QString& labelText) {
        auto* row = new QWidget(rowParent);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(t.spacing.sm);
        dot = new widgets::AidaStatusDot(row);
        dot->setPulsing(false);
        rowLayout->addWidget(dot);
        label = new QLabel(labelText, row);
        label->setMinimumWidth(t.row.property_label_w);
        rowLayout->addWidget(label);
        detail = new QLabel(row);
        detail->setProperty("aidaVariant", QStringLiteral("secondary"));
        rowLayout->addWidget(detail, 1);
        return row;
    };
    layout->addWidget(installRow(panel, pythonDot_, pythonLabel_, pythonDetail_,
        QStringLiteral("Python")));
    layout->addWidget(installRow(panel, moduleDot_, moduleLabel_, moduleDetail_,
        QStringLiteral("Camoufox module")));
    layout->addWidget(installRow(panel, browserDot_, browserLabel_, browserDetail_,
        QStringLiteral("Browser binary")));

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(t.spacing.xs);
    auto* installButton = new widgets::AidaButton(QStringLiteral("Install Module"), panel);
    installButton->setObjectName(QStringLiteral("view.network.headless.install_module"));
    installButton->setKind(widgets::AidaButton::Kind::Secondary);
    installButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    connect(installButton, &QAbstractButton::clicked, this,
        [this] { controller_->installModule(); });
    buttonRow->addWidget(installButton);
    auto* fetchButton = new widgets::AidaButton(QStringLiteral("Fetch Browser"), panel);
    fetchButton->setObjectName(QStringLiteral("view.network.headless.install_fetch"));
    fetchButton->setKind(widgets::AidaButton::Kind::Secondary);
    fetchButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    connect(fetchButton, &QAbstractButton::clicked, this,
        [this] { controller_->fetchBrowser(); });
    buttonRow->addWidget(fetchButton);
    auto* reprobeButton = new widgets::AidaButton(QStringLiteral("Re-probe"), panel);
    reprobeButton->setObjectName(QStringLiteral("view.network.headless.install_reprobe"));
    reprobeButton->setKind(widgets::AidaButton::Kind::Secondary);
    reprobeButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    connect(reprobeButton, &QAbstractButton::clicked, this, [this] {
        installLog_->appendPlainText(QStringLiteral("[re-probe] requested"));
        controller_->scheduleInstallProbe(true);
    });
    buttonRow->addWidget(reprobeButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    installLog_ = new QPlainTextEdit(panel);
    installLog_->setObjectName(QStringLiteral("view.network.headless.install_log"));
    installLog_->setReadOnly(true);
    installLog_->setFont(theme::fonts::codeRegular());
    installLog_->setMaximumBlockCount(256);
    installLog_->setPlaceholderText(QStringLiteral("No install output yet."));
    installLog_->setMaximumHeight(t.table.row_h * 4 + t.spacing.sm);
    layout->addWidget(installLog_);
    return panel;
}

QWidget* QtHeadlessBrowserView::buildLeftPane(QWidget* parent)
{
    const auto& t = theme::tokens();
    auto* pane = new QWidget(parent);
    auto* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(t.spacing.xs, t.spacing.xs, t.spacing.xs, t.spacing.xs);
    layout->setSpacing(t.spacing.xs);

    auto* infoTitle = new QLabel(QStringLiteral("Current page info"), pane);
    infoTitle->setFont(theme::fonts::strong());
    layout->addWidget(infoTitle);
    pageUrlLabel_ = new QLabel(pane);
    pageUrlLabel_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(pageUrlLabel_);
    pageBrowserLabel_ = new QLabel(pane);
    pageBrowserLabel_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(pageBrowserLabel_);
    pageCallsLabel_ = new QLabel(pane);
    pageCallsLabel_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(pageCallsLabel_);
    pageErrorLabel_ = new QLabel(pane);
    pageErrorLabel_->setProperty("aidaVariant", QStringLiteral("error"));
    pageErrorLabel_->setWordWrap(true);
    layout->addWidget(pageErrorLabel_);

    auto* consoleHeader = new QHBoxLayout();
    consoleHeader->setSpacing(t.spacing.xs);
    auto* consoleTitle = new QLabel(QStringLiteral("Console logs"), pane);
    consoleTitle->setFont(theme::fonts::strong());
    consoleHeader->addWidget(consoleTitle);
    consoleAutoscroll_ = new QCheckBox(QStringLiteral("auto-scroll"), pane);
    consoleAutoscroll_->setObjectName(QStringLiteral("view.network.headless.console_autoscroll"));
    consoleAutoscroll_->setChecked(true);
    consoleAutoscroll_->setToolTip(QStringLiteral(
        "Keep the newest console entry visible when new entries arrive"));
    consoleHeader->addWidget(consoleAutoscroll_);
    auto* clearConsole = new QPushButton(QStringLiteral("Clear"), pane);
    clearConsole->setObjectName(QStringLiteral("view.network.headless.console_clear"));
    clearConsole->setToolTip(QStringLiteral("Clear the cached console entries"));
    connect(clearConsole, &QPushButton::clicked, this,
        [this] { controller_->clearConsoleCache(); });
    consoleHeader->addWidget(clearConsole);
    consoleHeader->addStretch(1);
    layout->addLayout(consoleHeader);
    consoleModel_ = new QtJsonLogModel(QtJsonLogModel::Kind::Console, pane);
    consoleView_ = new QTableView(pane);
    consoleView_->setObjectName(QStringLiteral("view.network.headless.console"));
    consoleView_->verticalHeader()->hide();
    consoleView_->verticalHeader()->setDefaultSectionSize(t.table.compact_row_h);
    consoleView_->horizontalHeader()->hide();
    consoleView_->horizontalHeader()->setStretchLastSection(true);
    consoleView_->setAlternatingRowColors(true);
    consoleView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    consoleView_->setSelectionMode(QAbstractItemView::NoSelection);
    consoleView_->setFont(theme::fonts::codeRegular());
    consoleView_->setModel(consoleModel_);
    layout->addWidget(consoleView_, 55);
    consoleEmpty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No console entries"),
        QStringLiteral("Start the bridge and navigate to capture page console output."),
        pane);
    layout->addWidget(consoleEmpty_);

    auto* networkHeader = new QHBoxLayout();
    networkHeader->setSpacing(t.spacing.xs);
    auto* networkTitle = new QLabel(QStringLiteral("Network requests"), pane);
    networkTitle->setFont(theme::fonts::strong());
    networkHeader->addWidget(networkTitle);
    networkAutoscroll_ = new QCheckBox(QStringLiteral("auto-scroll"), pane);
    networkAutoscroll_->setObjectName(QStringLiteral("view.network.headless.network_autoscroll"));
    networkAutoscroll_->setChecked(true);
    networkAutoscroll_->setToolTip(QStringLiteral(
        "Keep the newest network request visible when new requests arrive"));
    networkHeader->addWidget(networkAutoscroll_);
    auto* clearNetwork = new QPushButton(QStringLiteral("Clear"), pane);
    clearNetwork->setObjectName(QStringLiteral("view.network.headless.network_clear"));
    clearNetwork->setToolTip(QStringLiteral("Clear the cached network requests"));
    connect(clearNetwork, &QPushButton::clicked, this,
        [this] { controller_->clearNetworkCache(); });
    networkHeader->addWidget(clearNetwork);
    networkHeader->addStretch(1);
    layout->addLayout(networkHeader);
    networkModel_ = new QtJsonLogModel(QtJsonLogModel::Kind::Network, pane);
    networkView_ = new QTableView(pane);
    networkView_->setModel(networkModel_);
    networkView_->setObjectName(QStringLiteral("view.network.headless.network"));
    networkView_->verticalHeader()->hide();
    networkView_->verticalHeader()->setDefaultSectionSize(t.table.compact_row_h);
    networkView_->horizontalHeader()->setStretchLastSection(true);
    networkView_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    networkView_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    networkView_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    networkView_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    networkView_->setAlternatingRowColors(true);
    networkView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    networkView_->setSelectionMode(QAbstractItemView::NoSelection);
    networkView_->setFont(theme::fonts::codeRegular());
    layout->addWidget(networkView_, 45);
    networkEmpty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No network requests"),
        QStringLiteral("Start the bridge and navigate to capture page traffic."),
        pane);
    layout->addWidget(networkEmpty_);
    return pane;
}

QWidget* QtHeadlessBrowserView::buildRightPane(QWidget* parent)
{
    const auto& t = theme::tokens();
    auto* pane = new QWidget(parent);
    auto* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(t.spacing.xs, t.spacing.xs, t.spacing.xs, t.spacing.xs);
    layout->setSpacing(t.spacing.xs);

    auto* evalTitle = new QLabel(QStringLiteral("Evaluate JS"), pane);
    evalTitle->setFont(theme::fonts::strong());
    layout->addWidget(evalTitle);
    evalInput_ = new QtByteCappedPlainTextEdit(pane);
    evalInput_->setObjectName(QStringLiteral("view.network.headless.eval_input"));
    evalInput_->setMaxBytes(8191);
    evalInput_->setTabChangesFocus(false);
    evalInput_->setFont(theme::fonts::codeRegular());
    evalInput_->setMinimumHeight(t.table.row_h * 4 - t.spacing.xxs);
    evalInput_->setMaximumHeight(t.table.row_h * 6 - t.spacing.sm);
    layout->addWidget(evalInput_);
    auto* evalRow = new QHBoxLayout();
    evalRow->setSpacing(t.spacing.xs);
    auto* runButton = new widgets::AidaButton(QStringLiteral("Run"), pane);
    runButton->setObjectName(QStringLiteral("view.network.headless.eval_run"));
    runButton->setKind(widgets::AidaButton::Kind::Primary);
    runButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    connect(runButton, &QAbstractButton::clicked, this,
        [this] { controller_->evaluateJs(evalInput_->toPlainText()); });
    evalRow->addWidget(runButton);
    auto* initButton = new widgets::AidaButton(QStringLiteral("Add as init script"), pane);
    initButton->setObjectName(QStringLiteral("view.network.headless.eval_init"));
    initButton->setKind(widgets::AidaButton::Kind::Secondary);
    initButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    connect(initButton, &QAbstractButton::clicked, this,
        [this] { controller_->addInitScript(evalInput_->toPlainText()); });
    evalRow->addWidget(initButton);
    auto* clearButton = new widgets::AidaButton(QStringLiteral("Clear"), pane);
    clearButton->setObjectName(QStringLiteral("view.network.headless.eval_clear"));
    clearButton->setKind(widgets::AidaButton::Kind::Ghost);
    clearButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    connect(clearButton, &QAbstractButton::clicked, this, [this] {
        evalInput_->clear();
        controller_->clearEvalOutput();
    });
    evalRow->addWidget(clearButton);
    auto* copyButton = new widgets::AidaButton(QStringLiteral("Copy"), pane);
    copyButton->setObjectName(QStringLiteral("view.network.headless.eval_copy"));
    copyButton->setKind(widgets::AidaButton::Kind::Ghost);
    copyButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    connect(copyButton, &QAbstractButton::clicked, this, [this] {
        const QString out = controller_->evalOutput();
        if (!out.isEmpty()) {
            ::diag::log_tagged_fmt("headless_v", "eval_copy_output len=%zu",
                static_cast<std::size_t>(out.size()));
            aida::qt::clipboard::set_text(out);
        }
    });
    evalRow->addWidget(copyButton);
    evalRow->addStretch(1);
    layout->addLayout(evalRow);
    auto* outputTitle = new QLabel(QStringLiteral("Output:"), pane);
    outputTitle->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(outputTitle);
    evalOutput_ = new QPlainTextEdit(pane);
    evalOutput_->setObjectName(QStringLiteral("view.network.headless.eval_output"));
    evalOutput_->setReadOnly(true);
    evalOutput_->setFont(theme::fonts::codeRegular());
    evalOutput_->setMaximumHeight(t.table.row_h * 5);
    layout->addWidget(evalOutput_);

    auto* hooksTitle = new QLabel(QStringLiteral("Hooks"), pane);
    hooksTitle->setFont(theme::fonts::strong());
    layout->addWidget(hooksTitle);
    auto* hooksRow = new QHBoxLayout();
    hooksRow->setSpacing(t.spacing.xs);
    hookPresetCombo_ = new QComboBox(pane);
    hookPresetCombo_->setObjectName(QStringLiteral("view.network.headless.hook_preset"));
    for (int i = 0; i < kHookPresetCount; ++i)
        hookPresetCombo_->addItem(QString::fromLatin1(kHookPresets[i]));
    hookPresetCombo_->setToolTip(QStringLiteral(
        "Predefined JS hook injected into the page (XSS sentinel, alert/eval capture, ...)"));
    hooksRow->addWidget(hookPresetCombo_, 1);
    injectHookButton_ = new widgets::AidaButton(QStringLiteral("Inject Hook"), pane);
    injectHookButton_->setObjectName(QStringLiteral("view.network.headless.hook_inject"));
    injectHookButton_->setKind(widgets::AidaButton::Kind::Secondary);
    injectHookButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    connect(injectHookButton_, &QAbstractButton::clicked, this, [this] {
        controller_->injectHookPreset(hookPresetCombo_->currentText());
    });
    hooksRow->addWidget(injectHookButton_);
    removeHooksButton_ = new widgets::AidaButton(QStringLiteral("Remove All Hooks"), pane);
    removeHooksButton_->setObjectName(QStringLiteral("view.network.headless.hook_remove_all"));
    removeHooksButton_->setKind(widgets::AidaButton::Kind::Secondary);
    removeHooksButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    connect(removeHooksButton_, &QAbstractButton::clicked, this,
        [this] { controller_->removeAllHooks(); });
    hooksRow->addWidget(removeHooksButton_);
    layout->addLayout(hooksRow);

    auto* screenshotTitle = new QLabel(QStringLiteral("Latest screenshot"), pane);
    screenshotTitle->setFont(theme::fonts::strong());
    layout->addWidget(screenshotTitle);
    screenshotPath_ = new QLabel(QStringLiteral("(none yet)"), pane);
    screenshotPath_->setProperty("aidaVariant", QStringLiteral("secondary"));
    screenshotPath_->setWordWrap(true);
    layout->addWidget(screenshotPath_);
    auto* screenshotRow = new QHBoxLayout();
    screenshotRow->setSpacing(t.spacing.xs);
    openExplorerButton_ = new widgets::AidaButton(QStringLiteral("Open in Explorer"), pane);
    openExplorerButton_->setObjectName(QStringLiteral("view.network.headless.open_explorer"));
    openExplorerButton_->setKind(widgets::AidaButton::Kind::Secondary);
    openExplorerButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    connect(openExplorerButton_, &QAbstractButton::clicked, this, [this] {
        const QString path = controller_->lastScreenshotPath();
        if (!path.isEmpty()) {
            ::diag::log_tagged_fmt("headless_v", "screenshot_open_explorer path='%s'",
                path.toUtf8().constData());
            openInExplorer(path);
        }
    });
    screenshotRow->addWidget(openExplorerButton_);
    copyPathButton_ = new widgets::AidaButton(QStringLiteral("Copy Path"), pane);
    copyPathButton_->setObjectName(QStringLiteral("view.network.headless.copy_path"));
    copyPathButton_->setKind(widgets::AidaButton::Kind::Ghost);
    copyPathButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    connect(copyPathButton_, &QAbstractButton::clicked, this, [this] {
        const QString path = controller_->lastScreenshotPath();
        if (!path.isEmpty()) {
            ::diag::log_tagged_fmt("headless_v", "screenshot_copy_path path='%s'",
                path.toUtf8().constData());
            aida::qt::clipboard::set_text(path);
        }
    });
    screenshotRow->addWidget(copyPathButton_);
    screenshotRow->addStretch(1);
    layout->addLayout(screenshotRow);

    auto* profileTitle = new QLabel(QStringLiteral("Launch profile"), pane);
    profileTitle->setFont(theme::fonts::strong());
    layout->addWidget(profileTitle);
    auto* profileRow = new QHBoxLayout();
    profileRow->setSpacing(t.spacing.xs);
    osCombo_ = new QComboBox(pane);
    osCombo_->setObjectName(QStringLiteral("view.network.headless.os"));
    for (const char* os : kOsPresets)
        osCombo_->addItem(QString::fromLatin1(os));
    osCombo_->setToolTip(QStringLiteral("OS fingerprint preset presented by the browser"));
    profileRow->addWidget(osCombo_);
    localeCombo_ = new QComboBox(pane);
    localeCombo_->setObjectName(QStringLiteral("view.network.headless.locale"));
    for (const char* locale : kLocalePresets)
        localeCombo_->addItem(QString::fromLatin1(locale));
    localeCombo_->setToolTip(QStringLiteral(
        "Locale preset (Accept-Language and navigator.language)"));
    profileRow->addWidget(localeCombo_);
    profileRow->addStretch(1);
    layout->addLayout(profileRow);
    layout->addStretch(1);
    return pane;
}

void QtHeadlessBrowserView::onPaneShown()
{
    controller_->scheduleStatusPoll();
    controller_->scheduleInstallProbe(false);
    pollTimer_->start();
}

void QtHeadlessBrowserView::onPaneHidden()
{
    pollTimer_->stop();
}

void QtHeadlessBrowserView::refreshStatusHeader()
{
    const auto& status = controller_->bridgeStatus();
    statusDot_->setKind(stateSemantic(status.state));
    statusDot_->setPulsing(status.state == aida::burp::camoufox::bridge_state_t::starting);
    statusSub_->setText(QStringLiteral("state=%1    elapsed=%2")
        .arg(QString::fromLatin1(stateLabel(status.state)))
        .arg(formatElapsed(status.launched_ms)));
    const bool active =
        status.state == aida::burp::camoufox::bridge_state_t::ready ||
        status.state == aida::burp::camoufox::bridge_state_t::starting;
    statusRight_->setText(active
        ? QStringLiteral("PID: %1").arg(status.child_pid)
        : QStringLiteral("Not running"));
    const char* variant = "secondary";
    if (status.state == aida::burp::camoufox::bridge_state_t::ready &&
        !status.active_page_url.empty()) {
        setElidedLabelText(statusRightSub_, QString::fromStdString(
            "Open URL: " + status.active_page_url));
    } else if (!status.last_error.empty()) {
        setElidedLabelText(statusRightSub_, QString::fromStdString(
            "err: " + status.last_error));
        variant = "error";
    } else {
        statusRightSub_->clear();
        statusRightSub_->setToolTip(QString());
    }
    if (statusRightSub_->property("aidaVariant").toString() != QLatin1String(variant)) {
        statusRightSub_->setProperty("aidaVariant", QString::fromLatin1(variant));
        theme::stylesheet::repolish(statusRightSub_);
    }

    const bool nativeUa = !status.ua_override &&
        (status.effective_ua_policy.empty() || status.effective_ua_policy == "camoufox_native");
    const auto pillKind = [](bool ok) {
        return ok ? widgets::AidaSemantic::Success : widgets::AidaSemantic::Neutral;
    };
    webrtcPill_->setKind(pillKind(
        status.webrtc_blocked ||
            status.state == aida::burp::camoufox::bridge_state_t::stopped));
    nativeUaPill_->setKind(pillKind(nativeUa));
    pageVerifiedPill_->setKind(pillKind(status.page_verified));
    privacyPill_->setKind(pillKind(status.privacy_verified));
}

void QtHeadlessBrowserView::refreshInstallPanel()
{
    const auto& status = controller_->installStatus();
    installStateLabel_->setText(QStringLiteral("install=%1")
        .arg(QString::fromLatin1(installStateLabel(status))));
    const char* variant = installStateVariant(status);
    if (installStateLabel_->property("aidaVariant").toString() != QLatin1String(variant)) {
        installStateLabel_->setProperty("aidaVariant", QString::fromLatin1(variant));
        theme::stylesheet::repolish(installStateLabel_);
    }
    const bool pythonOk = !status.python_path.empty();
    const bool moduleOk = !status.module_version.empty();
    const bool browserOk = !status.browser_path.empty();
    pythonDot_->setKind(pythonOk ? widgets::AidaSemantic::Success
                                 : widgets::AidaSemantic::Error);
    setElidedLabelText(pythonDetail_, QString::fromStdString(
        pythonOk ? status.python_path : "not found"));
    moduleDot_->setKind(moduleOk ? widgets::AidaSemantic::Success
                                 : widgets::AidaSemantic::Error);
    setElidedLabelText(moduleDetail_, QString::fromStdString(
        moduleOk ? std::string("v") + status.module_version : "not installed"));
    browserDot_->setKind(browserOk ? widgets::AidaSemantic::Success
                                   : widgets::AidaSemantic::Error);
    setElidedLabelText(browserDetail_, QString::fromStdString(
        browserOk ? status.browser_path : "not fetched"));
}

void QtHeadlessBrowserView::refreshBridgeControls()
{
    const auto state = controller_->bridgeStatus().state;
    const bool starting = state == aida::burp::camoufox::bridge_state_t::starting;
    const bool ready = state == aida::burp::camoufox::bridge_state_t::ready;
    const bool stopped = state == aida::burp::camoufox::bridge_state_t::stopped ||
        state == aida::burp::camoufox::bridge_state_t::error;
    startButton_->setEnabled(!(ready || starting));
    stopButton_->setEnabled(!(stopped || starting));
    resetButton_->setEnabled(ready);
    navigateButton_->setEnabled(ready);
    screenshotButton_->setEnabled(ready);
    injectHookButton_->setEnabled(ready);
    removeHooksButton_->setEnabled(ready);
}

void QtHeadlessBrowserView::refreshPageInfo()
{
    const auto& status = controller_->bridgeStatus();
    setElidedLabelText(pageUrlLabel_, QStringLiteral("URL:       %1")
        .arg(status.active_page_url.empty() ? QStringLiteral("-")
            : QString::fromStdString(status.active_page_url)));
    pageBrowserLabel_->setText(QStringLiteral("Browser:   %1")
        .arg(status.browser_open ? QStringLiteral("open") : QStringLiteral("closed")));
    pageCallsLabel_->setText(QStringLiteral("Calls:     %1  errors=%2")
        .arg(static_cast<quint64>(status.total_calls))
        .arg(static_cast<quint64>(status.total_errors)));
    pageErrorLabel_->setText(status.last_error.empty()
        ? QString() : QStringLiteral("Err: %1").arg(QString::fromStdString(status.last_error)));
}

void QtHeadlessBrowserView::autoScrollIfPinned(QTableView* view,
    QAbstractTableModel* model, bool enabled, std::uint64_t signature, std::uint64_t& lastSeen)
{
    if (!enabled || signature == lastSeen)
        return;
    lastSeen = signature;
    auto* scrollBar = view->verticalScrollBar();
    const bool atBottom = scrollBar->maximum() <= 0 ||
        scrollBar->value() >= scrollBar->maximum() - 8;
    if (atBottom)
        view->scrollToBottom();
}

void QtHeadlessBrowserView::applyConsoleSnapshot()
{
    const auto rows = controller_->consoleCache();
    consoleModel_->adopt(rows, controller_->consoleSignature());
    autoScrollIfPinned(consoleView_, consoleModel_, consoleAutoscroll_->isChecked(),
        controller_->consoleSignature(), last_console_signature_);
    const bool empty = consoleModel_->rowCount() == 0;
    consoleEmpty_->setVisible(empty);
    consoleView_->setVisible(!empty);
}

void QtHeadlessBrowserView::applyNetworkSnapshot()
{
    const auto rows = controller_->networkCache();
    networkModel_->adopt(rows, controller_->networkSignature());
    autoScrollIfPinned(networkView_, networkModel_, networkAutoscroll_->isChecked(),
        controller_->networkSignature(), last_network_signature_);
    const bool empty = networkModel_->rowCount() == 0;
    networkEmpty_->setVisible(empty);
    networkView_->setVisible(!empty);
}

void QtHeadlessBrowserView::refreshEvalOutput()
{
    const QString out = controller_->evalOutput();
    evalOutput_->setPlainText(out.isEmpty() ? QStringLiteral("(no output yet)") : out);
    const bool isError = out.size() >= 7 && out.left(7) == QStringLiteral("[error]");
    const bool hasError = evalOutput_->property("aidaVariant").isValid();
    if (isError != hasError) {
        evalOutput_->setProperty("aidaVariant",
            isError ? QVariant(QStringLiteral("error")) : QVariant());
        theme::stylesheet::repolish(evalOutput_);
    }
}

void QtHeadlessBrowserView::refreshScreenshot()
{
    const QString path = controller_->lastScreenshotPath();
    screenshotPath_->setText(path.isEmpty() ? QStringLiteral("(none yet)") : path);
    screenshotPath_->setToolTip(path);
    openExplorerButton_->setEnabled(!path.isEmpty());
    copyPathButton_->setEnabled(!path.isEmpty());
}

}

namespace aida::burp::headless_view {

bool initialize()
{
    auto& core = aida::qt::net::headlessCore();
    bool expected = false;
    if (!core.initialized.compare_exchange_strong(expected, true))
        return true;
    {
        std::lock_guard<std::mutex> lock(core.subscription_mutex);
        core.subscription = aida::events::subscribe(
            aida::burp::camoufox::kBridgeStateChanged,
            [](const aida::burp::camoufox::bridge_state_changed_t& ev) {
                aida::qt::net::onBridgeStateChanged(ev);
            });
    }
    if (auto* controller = aida::qt::net::QtHeadlessBrowserController::instance())
        controller->scheduleStatusPoll();
    ::diag::log_tagged("headless_v", "headless_view_initialized");
    return true;
}

void shutdown()
{
    auto& core = aida::qt::net::headlessCore();
    if (!core.initialized.exchange(false))
        return;
    aida::events::subscription_handle_t handle;
    {
        std::lock_guard<std::mutex> lock(core.subscription_mutex);
        handle = core.subscription;
        core.subscription = {};
    }
    if (handle.valid())
        aida::events::unsubscribe(handle);
    ::diag::log_tagged("headless_v", "headless_view_shutdown");
}

std::string last_error()
{
    std::lock_guard<std::mutex> lock(aida::qt::net::headlessCore().error_mutex);
    return aida::qt::net::headlessCore().last_error;
}

}
