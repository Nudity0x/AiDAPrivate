#include "qt/programming/programming_host_hooks.hpp"

#include <QApplication>
#include <QPointer>
#include <QThread>

#include <mutex>

#include "qt/docking/dock_host.hpp"
#include "qt/programming/aida_language_views.hpp"
#include "qt/programming/aida_output_pane.hpp"
#include "qt/programming/aida_terminal_view.hpp"

namespace aida::qt::programming::host {
namespace {

QPointer<docking::AidaDockHost> g_host;
std::mutex g_scope_mutex;
std::vector<std::string> g_scope;

operation_result_t not_ready() {
    return {false, "The programming workspace is not initialized"};
}

bool terminal_tab(bottom_tab_t tab) noexcept {
    return tab == bottom_tab_t::terminal;
}

}

void install(docking::AidaDockHost* host) {
    g_host = host;
}

operation_result_t copy_all(bottom_tab_t tab) {
    if (terminal_tab(tab)) {
        if (!AidaTerminalController::exists()) return not_ready();
        return AidaTerminalController::instance().terminalCopyAll();
    }
    if (!AidaOutputController::exists()) return not_ready();
    return AidaOutputController::instance().copy_all(tab);
}

operation_result_t clear(bottom_tab_t tab) {
    if (terminal_tab(tab)) {
        if (!AidaTerminalController::exists()) return not_ready();
        return AidaTerminalController::instance().terminalClear();
    }
    if (!AidaOutputController::exists()) return not_ready();
    return AidaOutputController::instance().clear(tab);
}

operation_result_t select_all(bottom_tab_t tab) {
    if (terminal_tab(tab)) {
        if (!AidaTerminalController::exists()) return not_ready();
        return AidaTerminalController::instance().terminalSelectAll();
    }
    if (!AidaOutputController::exists()) return not_ready();
    return AidaOutputController::instance().select_all(tab);
}

operation_result_t toggle_follow(bottom_tab_t tab) {
    if (terminal_tab(tab)) {
        if (!AidaTerminalController::exists()) return not_ready();
        return AidaTerminalController::instance().terminalToggleFollow();
    }
    if (!AidaOutputController::exists()) return not_ready();
    return AidaOutputController::instance().toggle_follow(tab);
}

operation_result_t focus_filter(bottom_tab_t tab) {
    if (terminal_tab(tab))
        return {false, "Terminal output cannot be filtered without changing interactive terminal semantics"};
    if (!AidaOutputController::exists()) return not_ready();
    return AidaOutputController::instance().focus_filter(tab);
}

operation_result_t export_all(bottom_tab_t tab) {
    if (terminal_tab(tab)) {
        if (!AidaTerminalController::exists()) return not_ready();
        return AidaTerminalController::instance().terminalExport();
    }
    if (!AidaOutputController::exists()) return not_ready();
    return AidaOutputController::instance().export_all(tab);
}

operation_result_t terminal_new() {
    return AidaTerminalController::instance().terminalNew();
}

operation_result_t terminal_new_at(const std::string& working_directory) {
    return AidaTerminalController::instance().terminalNewAt(working_directory);
}

operation_result_t terminal_close() {
    if (!AidaTerminalController::exists()) return not_ready();
    return AidaTerminalController::instance().terminalClose();
}

operation_result_t terminal_restart() {
    if (!AidaTerminalController::exists()) return not_ready();
    return AidaTerminalController::instance().terminalRestart();
}

operation_result_t terminal_next() {
    if (!AidaTerminalController::exists()) return not_ready();
    return AidaTerminalController::instance().terminalNext();
}

operation_result_t terminal_previous() {
    if (!AidaTerminalController::exists()) return not_ready();
    return AidaTerminalController::instance().terminalPrevious();
}

operation_result_t terminal_split_vertical() {
    if (!AidaTerminalController::exists()) return not_ready();
    return AidaTerminalController::instance().terminalSplit(
        aida::terminal::split_mode_t::vertical);
}

operation_result_t terminal_split_horizontal() {
    if (!AidaTerminalController::exists()) return not_ready();
    return AidaTerminalController::instance().terminalSplit(
        aida::terminal::split_mode_t::horizontal);
}

operation_result_t terminal_unsplit() {
    if (!AidaTerminalController::exists()) return not_ready();
    return AidaTerminalController::instance().terminalUnsplit();
}

operation_result_t terminal_focus_search() {
    if (!AidaTerminalController::exists()) return not_ready();
    return AidaTerminalController::instance().terminalFocusSearch();
}

operation_result_t terminal_paste() {
    if (!AidaTerminalController::exists()) return not_ready();
    return AidaTerminalController::instance().terminalPaste();
}

bool has_content(bottom_tab_t tab) {
    if (terminal_tab(tab))
        return AidaTerminalController::exists() &&
            AidaTerminalController::instance().hasActiveContent();
    return AidaOutputController::exists() &&
        AidaOutputController::instance().has_content(tab);
}

bool supports_filter(bottom_tab_t tab) noexcept {
    return !terminal_tab(tab);
}

bool follows_tail(bottom_tab_t tab) {
    if (terminal_tab(tab))
        return AidaTerminalController::exists() &&
            AidaTerminalController::instance().followsTail();
    return AidaOutputController::exists()
        ? AidaOutputController::instance().follows_tail(tab) : true;
}

bool source_available(bottom_tab_t tab) noexcept {
    if (terminal_tab(tab))
        return AidaTerminalController::exists() &&
            AidaTerminalController::instance().sourceAvailable();
    return true;
}

std::size_t terminal_session_count() noexcept {
    return AidaTerminalController::exists()
        ? AidaTerminalController::instance().sessionCount() : 0;
}

bool terminal_is_split() noexcept {
    return AidaTerminalController::exists() &&
        AidaTerminalController::instance().isSplit();
}

void open_rename_dialog() {
    programming::open_rename_dialog(QApplication::activeWindow());
}

void shutdown_terminal() {
    if (AidaTerminalController::exists())
        AidaTerminalController::instance().shutdown();
}

bool open_or_focus_view(const char* view_id) {
    if (!g_host || !view_id)
        return false;
    const bool gui_thread = QThread::currentThread() == qApp->thread();
    if (gui_thread)
        return g_host->open_or_focus(registry::stable_view_id_t(view_id)).ok();
    auto* context = &AidaOutputController::instance();
    QMetaObject::invokeMethod(context, [host = g_host, id = std::string(view_id)] {
        if (host)
            static_cast<void>(host->open_or_focus(registry::stable_view_id_t(id)));
    }, Qt::QueuedConnection);
    return true;
}

void set_workspace_search_scope(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_scope_mutex);
    g_scope.assign(1, path);
}

std::vector<std::string> workspace_search_scope() {
    std::lock_guard<std::mutex> lock(g_scope_mutex);
    return g_scope;
}

void clear_workspace_search_scope() {
    std::lock_guard<std::mutex> lock(g_scope_mutex);
    g_scope.clear();
}

}
