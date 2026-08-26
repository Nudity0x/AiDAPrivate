#include "qt/qt_session_bridge.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QSessionManager>

#include <utility>

#include "helpers/diag_log.hpp"
#include "core/diagnostics/crash_snapshot.hpp"

namespace aida::qt {

AidaSessionBridge::AidaSessionBridge(QObject* parent)
    : QObject(parent)
{
}

void AidaSessionBridge::install()
{
    if (installed_) {
        diag::log_tagged_critical("qt_session", "session_bridge_install_skipped already=1");
        return;
    }
    QGuiApplication* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (!app) {
        diag::log_tagged_critical("qt_session", "session_bridge_install_failed reason=no_qguiapplication");
        return;
    }
    connect(app, &QGuiApplication::commitDataRequest, this, [this](QSessionManager& session) {
        onCommitDataRequest(session);
    });
    QCoreApplication::instance()->installNativeEventFilter(this);
    installed_ = true;
    diag::log_tagged_critical_fmt("qt_session",
        "session_bridge_installed commit_data_hook=1 native_filter=1 tid=%lu",
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

void AidaSessionBridge::setExitReviewGateHook(std::function<bool()> hook)
{
    exit_review_gate_hook_ = std::move(hook);
    diag::log_tagged_critical_fmt("qt_session",
        "session_exit_review_hook_set present=%d tid=%lu",
        exit_review_gate_hook_ ? 1 : 0,
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

void AidaSessionBridge::setSessionAbortHook(std::function<void()> hook)
{
    session_abort_hook_ = std::move(hook);
    diag::log_tagged_critical_fmt("qt_session",
        "session_abort_hook_set present=%d tid=%lu",
        session_abort_hook_ ? 1 : 0,
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

void AidaSessionBridge::onCommitDataRequest(QSessionManager& session)
{
    abort_signaled_this_cycle_ = false;
    if (!exit_review_gate_hook_) {
        diag::log_tagged_critical("qt_session", "queryendsession_no_hook allowed=1");
        return;
    }
    bool committed = false;
    try {
        committed = exit_review_gate_hook_();
    } catch (...) {
        aida::diagnostics::crash::emit_crash_breadcrumb(0xE06D7363u, nullptr, "qt_session_bridge_commit_data");
        diag::log_tagged_critical("qt_session", "queryendsession_hook_exception vetoed=1");
        session.cancel();
        return;
    }
    if (committed) {
        diag::log_tagged_critical("qt_session", "queryendsession_committed allowed=1");
        return;
    }
    session.cancel();
    diag::log_tagged_critical("qt_session", "queryendsession_review_pending vetoed=1");
}

bool AidaSessionBridge::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(result);
    if (eventType != "windows_generic_MSG")
        return false;
    const MSG* msg = static_cast<const MSG*>(message);
    if (!msg || msg->message != WM_ENDSESSION)
        return false;
    if (msg->wParam == FALSE) {
        if (!abort_signaled_this_cycle_) {
            abort_signaled_this_cycle_ = true;
            diag::log_tagged_critical_fmt("qt_session",
                "session_end_abort_observed hwnd=0x%llX hook=%d tid=%lu",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(msg->hwnd)),
                session_abort_hook_ ? 1 : 0,
                static_cast<unsigned long>(::GetCurrentThreadId()));
            if (session_abort_hook_) {
                try {
                    session_abort_hook_();
                } catch (...) {
                    aida::diagnostics::crash::emit_crash_breadcrumb(0xE06D7363u, nullptr, "qt_session_bridge_abort");
                    diag::log_tagged_critical("qt_session", "session_abort_hook_exception");
                }
            }
        }
        return false;
    }
    abort_signaled_this_cycle_ = false;
    diag::log_tagged_critical_fmt("qt_session",
        "session_end_commit_observed hwnd=0x%llX tid=%lu",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(msg->hwnd)),
        static_cast<unsigned long>(::GetCurrentThreadId()));
    return false;
}

}
