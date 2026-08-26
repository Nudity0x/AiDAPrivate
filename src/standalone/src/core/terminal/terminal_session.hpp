#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../infra/win_thread.hpp"
#include "../../helpers/diag_log.hpp"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aida::terminal
{

// Cell colors are packed 0xAABBGGRR (R in bits 0..7, G in 8..15, B in 16..23,
// A in 24..31) — bit-identical to the retired ImU32 IM_COL32 layout, so the Qt
// presenter reconstructs the exact QColor(r, g, b, a) triplets with shifts.
inline constexpr std::uint32_t cell_color(int r, int g, int b, int a) noexcept
{
    return (static_cast<std::uint32_t>(a) << 24) |
           (static_cast<std::uint32_t>(b) << 16) |
           (static_cast<std::uint32_t>(g) << 8) |
           static_cast<std::uint32_t>(r);
}

inline constexpr std::uint32_t cell_color_alpha_mask = 0xFF000000u;

struct Cell {
    char           ch    = ' ';
    std::uint32_t  fg    = cell_color(204, 204, 204, 255);
    std::uint32_t  bg    = cell_color(0, 0, 0, 0);
    bool           bold  = false;
};

enum class ansi_state_t : std::uint8_t {
    normal,
    escape,
    csi,
    osc,
    osc_escape
};

struct TerminalSession
{
    HPCON                hPC          = INVALID_HANDLE_VALUE;
    HANDLE               hPipeIn      = INVALID_HANDLE_VALUE;
    HANDLE               hPipeOut     = INVALID_HANDLE_VALUE;
    HANDLE               hProcess     = INVALID_HANDLE_VALUE;
    HANDLE               hThread      = INVALID_HANDLE_VALUE;
    HANDLE               hJob         = INVALID_HANDLE_VALUE;


    aida::infra::win_thread::joinable_thread_t reader_thread;
    aida::infra::win_thread::joinable_thread_t writer_thread;
    std::atomic<bool>    stop_reader{false};
    std::atomic<bool>    reader_done{true};
    std::atomic<bool>    writer_done{true};


    std::mutex           input_mtx;
    std::condition_variable input_cv;
    std::deque<std::string> input_queue;
    std::size_t          input_queue_bytes = 0;
    std::string          input_error;
    static constexpr std::size_t MAX_INPUT_QUEUE_BYTES = 1ULL << 20;


    std::mutex           buffer_mtx;
    std::deque<std::vector<Cell>> lines;
    static constexpr int DEFAULT_MAX_LINES = 10000;
    int                  max_lines = DEFAULT_MAX_LINES;
    int                  cols     = 120;
    int                  rows_vis = 24;


    std::uint32_t        cur_fg   = cell_color(204, 204, 204, 255);
    std::uint32_t        cur_bg   = cell_color(0, 0, 0, 0);
    bool                 cur_bold = false;


    int                  cursor_row = 0;
    int                  cursor_col = 0;


    float                scroll_y = 0.f;
    bool                 scroll_to_bottom = true;
    bool                 auto_follow = true;


    std::string          title = "Terminal";
    std::atomic<bool>    alive{false};
    std::uint64_t        id = 0;
    std::string          profile_id;
    std::string          profile_label;
    std::wstring         command;
    std::wstring         cwd;
    std::string          start_error;
    std::atomic<std::uint32_t> exit_code{std::numeric_limits<std::uint32_t>::max()};
    std::atomic<std::uint64_t> buffer_generation{1};

    struct search_match_t {
        int line = 0;
        int column = 0;
        int length = 0;
    };
    std::string          search_query;
    std::vector<search_match_t> search_matches;
    int                  active_search_match = -1;
    std::uint64_t        searched_generation = 0;


    int                  prev_line_count = 0;
    std::deque<float>    line_entrance_time;
    std::atomic<bool>    bell_pending{false};
    ansi_state_t         ansi_state = ansi_state_t::normal;
    std::string          ansi_params;

    // Reader->GUI notification seam (the only transport change versus the
    // retired ImGui view): the reader sets output_notify_pending and, when it
    // was clear, invokes output_notify; the GUI side clears the flag when it
    // consumes the notification. Both are assigned before the reader starts
    // and never mutated afterwards, so the reader reads them lock-free.
    std::atomic<bool>    output_notify_pending{false};
    std::function<void(TerminalSession&)> output_notify;
};

std::uint32_t ansi_color(int idx, bool bright);
void parse_ansi_sgr(TerminalSession& s, const std::string& params);
void ensure_line(TerminalSession& s, int row);
void push_char(TerminalSession& s, char ch);
void process_output(TerminalSession& s, const char* data, size_t len);
void reader_thread_func(TerminalSession* s);
void writer_thread_func(TerminalSession* s);
void close_native_handle(HANDLE& handle) noexcept;
void close_native_pseudoconsole(HPCON& pseudoconsole) noexcept;
bool create_session(TerminalSession& s, const wchar_t* shell = nullptr,
                    const wchar_t* cwd = nullptr);
void send_input(TerminalSession& s, const char* data, size_t len);
void clear_session(TerminalSession& s);
bool try_clear_session(TerminalSession& s);
bool refresh_search(TerminalSession& s, const std::string& query);
bool move_search_match(TerminalSession& s, int delta);
bool try_copy_all_text(TerminalSession& s, std::string& all_text);
void resize_pty(TerminalSession& s, int cols, int rows);
void release_session_process_handles(TerminalSession& s);
bool finalize_destroyed_session(TerminalSession& s, std::uint32_t timeout_ms);
bool destroy_session(TerminalSession& s);

struct profile_t {
    std::string id;
    std::string label;
    std::wstring command;
};

std::vector<profile_t> available_profiles(const std::string& configured_shell);

enum class split_mode_t : std::uint8_t {
    none,
    vertical,
    horizontal
};

struct TerminalManager
{
    std::vector<TerminalSession*> sessions;
    std::vector<TerminalSession*> retired_sessions;
    int active_tab = -1;
    int secondary_tab = -1;
    split_mode_t split_mode = split_mode_t::none;
    std::uint64_t next_id = 1;
    std::string last_error;

    // Assigned to every created/restarted session; fires after each parsed
    // output burst so the GUI can coalesce a queued snapshot refresh.
    std::function<void(TerminalSession&)> output_notify;

    void reap_retired_sessions();
    void retire_or_delete(TerminalSession* session);
    TerminalSession* create_terminal(const wchar_t* shell = nullptr,
                                     const wchar_t* cwd = nullptr,
                                     const char* profile_id = nullptr,
                                     const char* profile_label = nullptr);
    void close_terminal(int idx);
    bool restart_terminal(int idx);
    bool cycle(int delta);
    bool set_split(split_mode_t mode);
    void shutdown();

    bool has_active() const { return active_tab >= 0 && active_tab < static_cast<int>(sessions.size()); }
    TerminalSession* current() { return has_active() ? sessions[static_cast<size_t>(active_tab)] : nullptr; }
    TerminalSession* secondary() {
        return split_mode != split_mode_t::none && secondary_tab >= 0 &&
            secondary_tab < static_cast<int>(sessions.size())
            ? sessions[static_cast<std::size_t>(secondary_tab)] : nullptr;
    }
};

}
