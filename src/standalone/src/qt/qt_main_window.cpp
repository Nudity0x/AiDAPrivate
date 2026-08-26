#include "qt/qt_main_window.hpp"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QShowEvent>
#include <QUrl>

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "helpers/diag_log.hpp"
#include "core/diagnostics/crash_snapshot.hpp"
#include "core/ui/ui_thread_dispatcher.hpp"
#include "qt/docking/dock_host.hpp"

HWND g_hwnd = nullptr;

namespace aida::qt {

namespace {

static AidaMainWindow* g_main_window = nullptr;
static std::atomic<uint64_t> g_dragdrop_ui_generation{0};

constexpr DWORD kDwmwaUseImmersiveDarkModeBefore20h1 = 19;

void aida_dispatch_dropped_file_open(const std::function<void(const std::string&)>& open_handler,
                                     const std::string& path_for_ui,
                                     uint64_t generation,
                                     DWORD producer_tid,
                                     uint64_t capture_start_ms)
{
    const uint64_t current_generation = g_dragdrop_ui_generation.load(std::memory_order_acquire);
    const uint64_t dispatch_start_ms = static_cast<uint64_t>(::GetTickCount64());
    if (current_generation != generation) {
        diag::log_tagged_critical_fmt("DRAGDROP-UI-DISPATCH",
            "stale generation=%llu current_generation=%llu producer_tid=%lu ui_tid=%lu queued_age_ms=%llu path=%.260s",
            static_cast<unsigned long long>(generation),
            static_cast<unsigned long long>(current_generation),
            static_cast<unsigned long>(producer_tid),
            static_cast<unsigned long>(::GetCurrentThreadId()),
            static_cast<unsigned long long>(dispatch_start_ms - capture_start_ms),
            path_for_ui.c_str());
        return;
    }
    if (!aida::ui_thread::require_owner("dragdrop", "open_path", "dispatch")) {
        diag::log_tagged_critical_fmt("DRAGDROP-UI-DISPATCH",
            "owner_rejected generation=%llu producer_tid=%lu ui_tid=%lu path=%.260s",
            static_cast<unsigned long long>(generation),
            static_cast<unsigned long>(producer_tid),
            static_cast<unsigned long>(::GetCurrentThreadId()),
            path_for_ui.c_str());
        return;
    }
    diag::log_tagged_critical_fmt("DRAGDROP-UI-DISPATCH",
        "begin generation=%llu producer_tid=%lu ui_tid=%lu queued_age_ms=%llu path=%.260s",
        static_cast<unsigned long long>(generation),
        static_cast<unsigned long>(producer_tid),
        static_cast<unsigned long>(::GetCurrentThreadId()),
        static_cast<unsigned long long>(dispatch_start_ms - capture_start_ms),
        path_for_ui.c_str());
    if (open_handler) {
        open_handler(path_for_ui);
    } else {
        diag::log_tagged_critical_fmt("DRAGDROP-UI-DISPATCH",
            "open_handler_missing generation=%llu ui_tid=%lu path=%.260s",
            static_cast<unsigned long long>(generation),
            static_cast<unsigned long>(::GetCurrentThreadId()),
            path_for_ui.c_str());
    }
    diag::log_tagged_critical_fmt("DRAGDROP-UI-DISPATCH",
        "end generation=%llu ui_tid=%lu elapsed_ms=%llu path=%.260s",
        static_cast<unsigned long long>(generation),
        static_cast<unsigned long>(::GetCurrentThreadId()),
        static_cast<unsigned long long>(static_cast<uint64_t>(::GetTickCount64()) - dispatch_start_ms),
        path_for_ui.c_str());
}

}

AidaMainWindow::AidaMainWindow(QWidget* parent, Qt::WindowFlags flags)
    : QMainWindow(parent, flags)
{
    setAcceptDrops(true);
    dock_host_ = new docking::AidaDockHost(this, this);
    g_main_window = this;
    diag::log_tagged_critical_fmt("qt_shell",
        "aida_main_window_constructed accept_drops=1 dock_host=0x%llX dock_manager=0x%llX tid=%lu",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(dock_host_)),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(dockManager())),
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

AidaMainWindow::~AidaMainWindow()
{
    if (g_main_window == this)
        g_main_window = nullptr;
}

void AidaMainWindow::setExitReviewGateHook(std::function<bool()> hook)
{
    exit_review_gate_hook_ = std::move(hook);
    diag::log_tagged_critical_fmt("qt_shell",
        "exit_review_gate_hook_set present=%d tid=%lu",
        exit_review_gate_hook_ ? 1 : 0,
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

void AidaMainWindow::setFileOpenHandler(std::function<void(const std::string&)> handler)
{
    file_open_handler_ = std::move(handler);
    diag::log_tagged_critical_fmt("qt_shell",
        "file_open_handler_set present=%d tid=%lu",
        file_open_handler_ ? 1 : 0,
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

ads::CDockManager* AidaMainWindow::dockManager() const
{
    return dock_host_ ? dock_host_->manager() : nullptr;
}

docking::AidaDockHost* AidaMainWindow::dockHost() const
{
    return dock_host_;
}

void AidaMainWindow::applyDwmBackdrop()
{
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) {
        diag::log_tagged_critical_fmt("qt_shell",
            "dwm_backdrop_apply_failed reason=no_hwnd tid=%lu",
            static_cast<unsigned long>(::GetCurrentThreadId()));
        return;
    }
    INT backdrop = DWMSBT_MAINWINDOW;
    const HRESULT hr_backdrop = ::DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    const BOOL dark = TRUE;
    const HRESULT hr_dark_20 = ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const HRESULT hr_dark_19 = ::DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkModeBefore20h1, &dark, sizeof(dark));
    dwm_backdrop_applied_ = SUCCEEDED(hr_backdrop);
    diag::log_tagged_critical_fmt("qt_shell",
        "dwm_backdrop_applied hwnd=0x%llX backdrop_hr=0x%08lX dark20_hr=0x%08lX dark19_hr=0x%08lX tid=%lu",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(hwnd)),
        static_cast<unsigned long>(hr_backdrop),
        static_cast<unsigned long>(hr_dark_20),
        static_cast<unsigned long>(hr_dark_19),
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

void AidaMainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (!dwm_backdrop_applied_)
        applyDwmBackdrop();
    static bool shown_logged = false;
    if (!shown_logged) {
        shown_logged = true;
        diag::log_tagged_critical_fmt("qt_shell",
            "aida_window_shown hwnd=0x%llX w=%d h=%d tid=%lu",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(reinterpret_cast<HWND>(winId()))),
            width(),
            height(),
            static_cast<unsigned long>(::GetCurrentThreadId()));
    }
}

void AidaMainWindow::closeEvent(QCloseEvent* event)
{
    if (exit_review_gate_hook_) {
        bool committed = false;
        try {
            committed = exit_review_gate_hook_();
        } catch (...) {
            aida::diagnostics::crash::emit_crash_breadcrumb(0xE06D7363u, nullptr, "qt_main_window_close_event");
            diag::log_tagged_critical("qt_shell", "close_review_gate_hook_exception vetoed=1");
            event->ignore();
            return;
        }
        if (!committed) {
            diag::log_tagged_critical_fmt("qt_shell",
                "close_review_request source=qt_close_event accepted=1 vetoed=1 tid=%lu",
                static_cast<unsigned long>(::GetCurrentThreadId()));
            event->ignore();
            return;
        }
    }
    aida::ui_thread::mark_window_destroying("qt_main_window", "close_event", "commit_accept");
    diag::log_tagged_critical_fmt("qt_shell",
        "close_committed hwnd=0x%llX tid=%lu",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(reinterpret_cast<HWND>(winId()))),
        static_cast<unsigned long>(::GetCurrentThreadId()));
    event->accept();
    QCoreApplication::quit();
}

void AidaMainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    const QMimeData* mime = event->mimeData();
    bool has_local = false;
    if (mime && mime->hasUrls()) {
        const QList<QUrl> urls = mime->urls();
        for (const QUrl& url : urls) {
            if (url.isLocalFile()) {
                has_local = true;
                break;
            }
        }
    }
    if (has_local) {
        event->acceptProposedAction();
        diag::log_tagged_fmt("dragdrop",
            "qt_drag_enter accepted=1 urls=%d tid=%lu",
            mime ? static_cast<int>(mime->urls().size()) : 0,
            static_cast<unsigned long>(::GetCurrentThreadId()));
    } else {
        event->ignore();
        diag::log_tagged_fmt("dragdrop",
            "qt_drag_enter accepted=0 has_urls=%d tid=%lu",
            mime && mime->hasUrls() ? 1 : 0,
            static_cast<unsigned long>(::GetCurrentThreadId()));
    }
}

void AidaMainWindow::dropEvent(QDropEvent* event)
{
    const QMimeData* mime = event->mimeData();
    const uint64_t generation = g_dragdrop_ui_generation.fetch_add(1ULL, std::memory_order_acq_rel) + 1ULL;
    const uint64_t capture_start_ms = static_cast<uint64_t>(::GetTickCount64());
    const DWORD producer_tid = ::GetCurrentThreadId();
    const QPoint drop_point = event->position().toPoint();
    int url_count = 0;
    int local_count = 0;
    int queued_count = 0;
    if (mime && mime->hasUrls()) {
        const QList<QUrl> urls = mime->urls();
        url_count = static_cast<int>(urls.size());
        for (const QUrl& url : urls) {
            if (!url.isLocalFile())
                continue;
            ++local_count;
            const QByteArray path_utf8 = url.toLocalFile().toUtf8();
            if (path_utf8.isEmpty())
                continue;
            const std::string path_for_ui(path_utf8.constData(), static_cast<std::size_t>(path_utf8.size()));
            const uint64_t deadline_ms = static_cast<uint64_t>(::GetTickCount64()) + 5000ULL;
            aida::ui_thread::post_options_t options;
            options.subsystem = "dragdrop";
            options.label = "open_path";
            options.phase = "qt_drop_deferred";
            options.owner = "dragdrop";
            options.priority = aida::ui_thread::priority_t::high;
            options.deadline_ms = deadline_ms;
            options.cancelled = [generation]() {
                return g_dragdrop_ui_generation.load(std::memory_order_acquire) != generation;
            };
            const aida::ui_thread::enqueue_result_t dispatch_result = aida::ui_thread::post(
                [handler = file_open_handler_, path_for_ui, generation, producer_tid, capture_start_ms]() {
                    aida_dispatch_dropped_file_open(handler, path_for_ui, generation, producer_tid, capture_start_ms);
                },
                std::move(options));
            const bool queued = dispatch_result == aida::ui_thread::enqueue_result_t::accepted;
            if (queued)
                ++queued_count;
            diag::log_tagged_critical_fmt("DRAGDROP-UI-DISPATCH",
                "enqueue generation=%llu result=%s deadline_ms=%llu priority=high path_len=%zu",
                static_cast<unsigned long long>(generation),
                aida::ui_thread::result_name(dispatch_result),
                static_cast<unsigned long long>(deadline_ms),
                path_for_ui.size());
        }
    }
    diag::log_tagged_critical_fmt("WNDPROC-DEFERRED-WORK",
        "dropfiles generation=%llu hwnd=0x%llX urls=%d local=%d queued=%d elapsed_ms=%llu drop_x=%ld drop_y=%ld ui_pending=%zu",
        static_cast<unsigned long long>(generation),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(reinterpret_cast<HWND>(winId()))),
        url_count,
        local_count,
        queued_count,
        static_cast<unsigned long long>(static_cast<uint64_t>(::GetTickCount64()) - capture_start_ms),
        static_cast<long>(drop_point.x()),
        static_cast<long>(drop_point.y()),
        aida::ui_thread::pending_count());
    if (queued_count > 0)
        event->acceptProposedAction();
    else
        event->ignore();
}

bool AidaMainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg && msg->message == WM_DPICHANGED) {
        const DWORD dpi_x = LOWORD(msg->wParam);
        const DWORD dpi_y = HIWORD(msg->wParam);
        const RECT* suggested = reinterpret_cast<const RECT*>(msg->lParam);
        diag::log_tagged_fmt("qt_shell",
            "wm_dpichanged hwnd=0x%llX dpi_x=%lu dpi_y=%lu suggested_left=%ld suggested_top=%ld suggested_right=%ld suggested_bottom=%ld tid=%lu",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(msg->hwnd)),
            static_cast<unsigned long>(dpi_x),
            static_cast<unsigned long>(dpi_y),
            suggested ? static_cast<long>(suggested->left) : 0L,
            suggested ? static_cast<long>(suggested->top) : 0L,
            suggested ? static_cast<long>(suggested->right) : 0L,
            suggested ? static_cast<long>(suggested->bottom) : 0L,
            static_cast<unsigned long>(::GetCurrentThreadId()));
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

HWND main_window_handle()
{
    AidaMainWindow* window = g_main_window;
    if (!window)
        return nullptr;
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    ::g_hwnd = handle;
    return handle;
}

}
