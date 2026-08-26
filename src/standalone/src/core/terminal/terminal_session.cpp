#include "terminal_session.hpp"

namespace aida::terminal
{

std::uint32_t ansi_color(int idx, bool bright)
{

    static const std::uint32_t normal[8] = {
        cell_color(  0,   0,   0, 255), cell_color(170,   0,   0, 255),
        cell_color(  0, 170,   0, 255), cell_color(170, 170,   0, 255),
        cell_color(  0,   0, 170, 255), cell_color(170,   0, 170, 255),
        cell_color(  0, 170, 170, 255), cell_color(170, 170, 170, 255),
    };
    static const std::uint32_t bold[8] = {
        cell_color( 85,  85,  85, 255), cell_color(255,  85,  85, 255),
        cell_color( 85, 255,  85, 255), cell_color(255, 255,  85, 255),
        cell_color( 85,  85, 255, 255), cell_color(255,  85, 255, 255),
        cell_color( 85, 255, 255, 255), cell_color(255, 255, 255, 255),
    };
    if (idx < 0 || idx > 7) idx = 7;
    return bright ? bold[idx] : normal[idx];
}


void parse_ansi_sgr(TerminalSession& s, const std::string& params)
{

    std::vector<int> codes;
    {
        int val = 0;
        bool has = false;
        for (char c : params) {
            if (c >= '0' && c <= '9') {
                val = val * 10 + (c - '0');
                has = true;
            } else if (c == ';') {
                codes.push_back(has ? val : 0);
                val = 0;
                has = false;
            }
        }
        codes.push_back(has ? val : 0);
    }

    for (size_t i = 0; i < codes.size(); ++i) {
        int c = codes[i];
        if (c == 0) {
            s.cur_fg = cell_color(204, 204, 204, 255);
            s.cur_bg = cell_color(0, 0, 0, 0);
            s.cur_bold = false;
        } else if (c == 1) {
            s.cur_bold = true;
        } else if (c == 22) {
            s.cur_bold = false;
        } else if (c >= 30 && c <= 37) {
            s.cur_fg = ansi_color(c - 30, s.cur_bold);
        } else if (c == 39) {
            s.cur_fg = cell_color(204, 204, 204, 255);
        } else if (c >= 40 && c <= 47) {
            s.cur_bg = ansi_color(c - 40, false);
        } else if (c == 49) {
            s.cur_bg = cell_color(0, 0, 0, 0);
        } else if (c >= 90 && c <= 97) {
            s.cur_fg = ansi_color(c - 90, true);
        } else if (c >= 100 && c <= 107) {
            s.cur_bg = ansi_color(c - 100, true);
        } else if (c == 38 && i + 2 < codes.size() && codes[i + 1] == 5) {

            int n = codes[i + 2];
            i += 2;
            if (n < 8)        s.cur_fg = ansi_color(n, false);
            else if (n < 16)  s.cur_fg = ansi_color(n - 8, true);
            else if (n < 232) {
                n -= 16;
                int r = (n / 36) * 51, g = ((n % 36) / 6) * 51, b = (n % 6) * 51;
                s.cur_fg = cell_color(r, g, b, 255);
            } else {
                int v = 8 + (n - 232) * 10;
                s.cur_fg = cell_color(v, v, v, 255);
            }
        } else if (c == 48 && i + 2 < codes.size() && codes[i + 1] == 5) {
            int n = codes[i + 2];
            i += 2;
            if (n < 8)        s.cur_bg = ansi_color(n, false);
            else if (n < 16)  s.cur_bg = ansi_color(n - 8, true);
            else if (n < 232) {
                n -= 16;
                int r = (n / 36) * 51, g = ((n % 36) / 6) * 51, b = (n % 6) * 51;
                s.cur_bg = cell_color(r, g, b, 255);
            } else {
                int v = 8 + (n - 232) * 10;
                s.cur_bg = cell_color(v, v, v, 255);
            }
        } else if (c == 38 && i + 4 < codes.size() && codes[i + 1] == 2) {

            s.cur_fg = cell_color(codes[i + 2], codes[i + 3], codes[i + 4], 255);
            i += 4;
        } else if (c == 48 && i + 4 < codes.size() && codes[i + 1] == 2) {
            s.cur_bg = cell_color(codes[i + 2], codes[i + 3], codes[i + 4], 255);
            i += 4;
        }
    }
}

void ensure_line(TerminalSession& s, int row)
{
    if (row < 0)
        return;
    const size_t row_index = static_cast<size_t>(row);
    const size_t column_count = static_cast<size_t>(std::max(0, s.cols));
    while (s.lines.size() <= row_index)
        s.lines.push_back(std::vector<Cell>(column_count));
}

void push_char(TerminalSession& s, char ch)
{
    if (ch == '\n') {
        s.cursor_row++;
        s.cursor_col = 0;
        s.scroll_to_bottom = true;
        return;
    }
    if (ch == '\r') {
        s.cursor_col = 0;
        return;
    }
    if (ch == '\t') {
        int next = (s.cursor_col + 8) & ~7;
        while (s.cursor_col < next && s.cursor_col < s.cols) {
            ensure_line(s, s.cursor_row);
            auto& row = s.lines[static_cast<size_t>(s.cursor_row)];
            if (s.cursor_col < static_cast<int>(row.size())) {
                row[static_cast<size_t>(s.cursor_col)] = Cell{' ', s.cur_fg, s.cur_bg, s.cur_bold};
            }
            s.cursor_col++;
        }
        return;
    }
    if (ch == '\b') {
        if (s.cursor_col > 0) s.cursor_col--;
        return;
    }
    if (ch == '\x07') {
        s.bell_pending.store(true, std::memory_order_release);
        return;
    }

    ensure_line(s, s.cursor_row);
    auto& row = s.lines[static_cast<size_t>(s.cursor_row)];
    if (s.cursor_col >= static_cast<int>(row.size()))
        row.resize(static_cast<size_t>(s.cursor_col) + 1U);
    row[static_cast<size_t>(s.cursor_col)] = Cell{ch, s.cur_fg, s.cur_bg, s.cur_bold};
    s.cursor_col++;
    if (s.cursor_col >= s.cols) {
        s.cursor_col = 0;
        s.cursor_row++;
    }
}


void process_output(TerminalSession& s, const char* data, size_t len)
{
    std::lock_guard<std::mutex> lk(s.buffer_mtx);

    for (size_t i = 0; i < len; ++i) {
        char ch = data[i];
        switch (s.ansi_state) {
        case ansi_state_t::normal:
            if (ch == '\x1b') {
                s.ansi_state = ansi_state_t::escape;
            } else {
                push_char(s, ch);
            }
            break;
        case ansi_state_t::escape:
            if (ch == '[') {
                s.ansi_state = ansi_state_t::csi;
                s.ansi_params.clear();
            } else if (ch == ']') {
                s.ansi_state = ansi_state_t::osc;
            } else {
                s.ansi_state = ansi_state_t::normal;
            }
            break;
        case ansi_state_t::csi:
            if ((ch >= '0' && ch <= '9') || ch == ';' || ch == '?') {
                if (s.ansi_params.size() < 128)
                    s.ansi_params += ch;
                else {
                    s.ansi_params.clear();
                    s.ansi_state = ansi_state_t::normal;
                }
            } else {

                if (ch == 'm') {
                    parse_ansi_sgr(s, s.ansi_params);
                } else if (ch == 'H' || ch == 'f') {

                    int r = 1, c2 = 1;
                    if (!s.ansi_params.empty()) {
                        auto semi = s.ansi_params.find(';');
                        if (semi != std::string::npos) {
                            r  = std::max(1, atoi(s.ansi_params.substr(0, semi).c_str()));
                            c2 = std::max(1, atoi(s.ansi_params.substr(semi + 1).c_str()));
                        } else {
                            r = std::max(1, atoi(s.ansi_params.c_str()));
                        }
                    }
                    s.cursor_row = r - 1;
                    s.cursor_col = c2 - 1;
                } else if (ch == 'J') {

                    int mode = s.ansi_params.empty() ? 0 : atoi(s.ansi_params.c_str());
                    if (mode == 2 || mode == 3) {
                        s.lines.clear();
                        s.cursor_row = 0;
                        s.cursor_col = 0;
                    }
                } else if (ch == 'K') {

                    ensure_line(s, s.cursor_row);
                    auto& row = s.lines[static_cast<size_t>(s.cursor_row)];
                    int mode = s.ansi_params.empty() ? 0 : atoi(s.ansi_params.c_str());
                    if (mode == 0) {
                        for (int j = s.cursor_col; j < static_cast<int>(row.size()); ++j)
                            row[static_cast<size_t>(j)] = Cell{};
                    } else if (mode == 1) {
                        for (int j = 0; j <= s.cursor_col && j < static_cast<int>(row.size()); ++j)
                            row[static_cast<size_t>(j)] = Cell{};
                    } else if (mode == 2) {
                        for (auto& cell : row) cell = Cell{};
                    }
                } else if (ch == 'A') {
                    int n = s.ansi_params.empty() ? 1 : std::max(1, atoi(s.ansi_params.c_str()));
                    s.cursor_row = std::max(0, s.cursor_row - n);
                } else if (ch == 'B') {
                    int n = s.ansi_params.empty() ? 1 : std::max(1, atoi(s.ansi_params.c_str()));
                    s.cursor_row += n;
                } else if (ch == 'C') {
                    int n = s.ansi_params.empty() ? 1 : std::max(1, atoi(s.ansi_params.c_str()));
                    s.cursor_col = std::min(s.cols - 1, s.cursor_col + n);
                } else if (ch == 'D') {
                    int n = s.ansi_params.empty() ? 1 : std::max(1, atoi(s.ansi_params.c_str()));
                    s.cursor_col = std::max(0, s.cursor_col - n);
                }

                s.ansi_params.clear();
                s.ansi_state = ansi_state_t::normal;
            }
            break;
        case ansi_state_t::osc:
            if (ch == '\x07') {
                s.bell_pending.store(true, std::memory_order_release);
                s.ansi_state = ansi_state_t::normal;
            } else if (ch == '\x1b') {
                s.ansi_state = ansi_state_t::osc_escape;
            }
            break;
        case ansi_state_t::osc_escape:
            s.ansi_state = ch == '\\' ? ansi_state_t::normal : ansi_state_t::osc;
            break;
        }
    }


    while (static_cast<int>(s.lines.size()) > s.max_lines) {
        s.lines.pop_front();
        if (s.cursor_row > 0) s.cursor_row--;
    }
    if (s.cursor_row < 0) s.cursor_row = 0;
    s.buffer_generation.fetch_add(1, std::memory_order_acq_rel);
}


void reader_thread_func(TerminalSession* s)
{
    if (!s)
        return;
    char buf[4096];
    try {
        while (!s->stop_reader.load(std::memory_order_acquire)) {
            DWORD bytes_read = 0;
            BOOL ok = ReadFile(s->hPipeIn, buf, sizeof(buf), &bytes_read, nullptr);
            if (!ok || bytes_read == 0) {
                s->alive.store(false, std::memory_order_release);
                if (s->hProcess != INVALID_HANDLE_VALUE) {
                    DWORD code = STILL_ACTIVE;
                    if (GetExitCodeProcess(s->hProcess, &code) && code != STILL_ACTIVE)
                        s->exit_code.store(static_cast<std::uint32_t>(code), std::memory_order_release);
                }
                break;
            }
            process_output(*s, buf, bytes_read);
            if (s->output_notify &&
                !s->output_notify_pending.exchange(true, std::memory_order_acq_rel))
                s->output_notify(*s);
        }
    } catch (...) {
        s->alive.store(false, std::memory_order_release);
    }
    s->reader_done.store(true, std::memory_order_release);
    if (s->output_notify &&
        !s->output_notify_pending.exchange(true, std::memory_order_acq_rel))
        s->output_notify(*s);
}


void writer_thread_func(TerminalSession* s)
{
    if (!s)
        return;
    for (;;) {
        std::string input;
        {
            std::unique_lock<std::mutex> lock(s->input_mtx);
            s->input_cv.wait(lock, [s]() {
                return s->stop_reader.load(std::memory_order_acquire) || !s->input_queue.empty();
            });
            if (s->stop_reader.load(std::memory_order_acquire)) {
                s->input_queue.clear();
                s->input_queue_bytes = 0;
                break;
            }
            input = std::move(s->input_queue.front());
            s->input_queue.pop_front();
            s->input_queue_bytes -= input.size();
        }
        std::size_t offset = 0;
        bool failed = false;
        while (offset < input.size() && !s->stop_reader.load(std::memory_order_acquire)) {
            const DWORD requested = static_cast<DWORD>((std::min)(
                input.size() - offset, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (s->hPipeOut == INVALID_HANDLE_VALUE ||
                !WriteFile(s->hPipeOut, input.data() + offset, requested, &written, nullptr) || written == 0) {
                const DWORD error = GetLastError();
                std::lock_guard<std::mutex> lock(s->input_mtx);
                s->input_error = "Terminal input failed (Win32 " + std::to_string(error) + ")";
                s->input_queue.clear();
                s->input_queue_bytes = 0;
                failed = true;
                break;
            }
            offset += written;
        }
        if (failed)
            break;
    }
    s->writer_done.store(true, std::memory_order_release);
}

void close_native_handle(HANDLE& handle) noexcept
{
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
        return;
    const HANDLE owned = handle;
    handle = INVALID_HANDLE_VALUE;
    CloseHandle(owned);
}

void close_native_pseudoconsole(HPCON& pseudoconsole) noexcept
{
    if (pseudoconsole == INVALID_HANDLE_VALUE || pseudoconsole == nullptr)
        return;
    const HPCON owned = pseudoconsole;
    pseudoconsole = INVALID_HANDLE_VALUE;
    ClosePseudoConsole(owned);
}


bool create_session(TerminalSession& s, const wchar_t* shell,
                    const wchar_t* cwd)
{
    if (!shell)
        shell = L"powershell.exe";
    if (*shell == L'\0') {
        s.start_error = "The terminal profile has no command";
        return false;
    }
    s.command = shell;
    s.cwd = cwd ? cwd : L"";
    s.start_error.clear();


    HANDLE hPipeInRead = INVALID_HANDLE_VALUE, hPipeInWrite = INVALID_HANDLE_VALUE;
    HANDLE hPipeOutRead = INVALID_HANDLE_VALUE, hPipeOutWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&hPipeInRead, &hPipeInWrite, nullptr, 0))
    {
        s.start_error = "CreatePipe for terminal input failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!CreatePipe(&hPipeOutRead, &hPipeOutWrite, nullptr, 0)) {
        const DWORD error = GetLastError();
        close_native_handle(hPipeInRead);
        close_native_handle(hPipeInWrite);
        s.start_error = "CreatePipe for terminal output failed (" + std::to_string(error) + ")";
        return false;
    }


    COORD size{};
    size.X = static_cast<SHORT>(s.cols);
    size.Y = static_cast<SHORT>(s.rows_vis);

    HRESULT hr = CreatePseudoConsole(size, hPipeInRead, hPipeOutWrite, 0, &s.hPC);
    if (FAILED(hr)) {
        close_native_handle(hPipeInRead);
        close_native_handle(hPipeInWrite);
        close_native_handle(hPipeOutRead);
        close_native_handle(hPipeOutWrite);
        s.start_error = "CreatePseudoConsole failed (" + std::to_string(static_cast<unsigned long>(hr)) + ")";
        return false;
    }


    close_native_handle(hPipeInRead);
    close_native_handle(hPipeOutWrite);

    s.hPipeIn  = hPipeOutRead;
    s.hPipeOut = hPipeInWrite;


    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<BYTE> attr_buf(attr_size);
    auto attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
        const DWORD error = GetLastError();
        close_native_pseudoconsole(s.hPC);
        close_native_handle(s.hPipeIn);
        close_native_handle(s.hPipeOut);
        s.start_error = "InitializeProcThreadAttributeList failed (" + std::to_string(error) + ")";
        return false;
    }
    if (!UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   s.hPC, sizeof(HPCON), nullptr, nullptr)) {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(attr_list);
        close_native_pseudoconsole(s.hPC);
        close_native_handle(s.hPipeIn);
        close_native_handle(s.hPipeOut);
        s.start_error = "UpdateProcThreadAttribute failed (" + std::to_string(error) + ")";
        return false;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attr_list;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(shell, shell + std::wcslen(shell) + 1);

    s.hJob = CreateJobObjectW(nullptr, nullptr);
    if (s.hJob == INVALID_HANDLE_VALUE || s.hJob == nullptr) {
        const DWORD error = GetLastError();
        s.hJob = INVALID_HANDLE_VALUE;
        DeleteProcThreadAttributeList(attr_list);
        close_native_pseudoconsole(s.hPC);
        close_native_handle(s.hPipeIn);
        close_native_handle(s.hPipeOut);
        s.start_error = "CreateJobObject failed (" + std::to_string(error) + ")";
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(s.hJob, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) {
        const DWORD error = GetLastError();
        close_native_handle(s.hJob);
        DeleteProcThreadAttributeList(attr_list);
        close_native_pseudoconsole(s.hPC);
        close_native_handle(s.hPipeIn);
        close_native_handle(s.hPipeOut);
        s.start_error = "SetInformationJobObject failed (" + std::to_string(error) + ")";
        return false;
    }

    BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                                      CREATE_SUSPENDED,
                                  nullptr, s.cwd.empty() ? nullptr : s.cwd.c_str(),
                                  &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attr_list);

    if (!created) {
        const DWORD error = GetLastError();
        close_native_pseudoconsole(s.hPC);
        close_native_handle(s.hPipeIn);
        close_native_handle(s.hPipeOut);
        close_native_handle(s.hJob);
        s.start_error = "CreateProcessW failed (" + std::to_string(error) + ")";
        return false;
    }

    if (!AssignProcessToJobObject(s.hJob, pi.hProcess)) {
        const DWORD error = GetLastError();
        TerminateProcess(pi.hProcess, error);
        close_native_handle(pi.hThread);
        close_native_handle(pi.hProcess);
        close_native_pseudoconsole(s.hPC);
        close_native_handle(s.hPipeIn);
        close_native_handle(s.hPipeOut);
        close_native_handle(s.hJob);
        s.start_error = "AssignProcessToJobObject failed (" + std::to_string(error) + ")";
        return false;
    }
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        TerminateJobObject(s.hJob, error);
        close_native_handle(pi.hThread);
        close_native_handle(pi.hProcess);
        close_native_pseudoconsole(s.hPC);
        close_native_handle(s.hPipeIn);
        close_native_handle(s.hPipeOut);
        close_native_handle(s.hJob);
        s.start_error = "ResumeThread failed (" + std::to_string(error) + ")";
        return false;
    }

    s.hProcess = pi.hProcess;
    s.hThread  = pi.hThread;
    s.alive.store(true, std::memory_order_release);
    s.exit_code.store(std::numeric_limits<std::uint32_t>::max(), std::memory_order_release);


    s.stop_reader.store(false, std::memory_order_release);
    s.reader_done.store(false, std::memory_order_release);
    std::string reader_err;
    if (!s.reader_thread.start([&s]() { reader_thread_func(&s); },
            &reader_err,
            aida::infra::win_thread::default_stack_reserve,
            "terminal_reader")) {
        s.start_error = reader_err.empty() ? "The terminal reader thread could not start" : reader_err;
        s.stop_reader.store(true, std::memory_order_release);
        s.reader_done.store(true, std::memory_order_release);
        s.alive.store(false, std::memory_order_release);
        close_native_pseudoconsole(s.hPC);
        close_native_handle(s.hPipeIn);
        close_native_handle(s.hPipeOut);
        if (s.hProcess != INVALID_HANDLE_VALUE) {
            TerminateProcess(s.hProcess, 1);
        }
        close_native_handle(s.hProcess);
        close_native_handle(s.hThread);
        close_native_handle(s.hJob);
        return false;
    }

    return true;
}


void send_input(TerminalSession& s, const char* data, size_t len)
{
    if (s.hPipeOut == INVALID_HANDLE_VALUE || !s.alive.load(std::memory_order_acquire) ||
        data == nullptr || len == 0)
        return;
    std::unique_lock<std::mutex> lock(s.input_mtx);
    if (!s.input_error.empty())
        return;
    if (len > TerminalSession::MAX_INPUT_QUEUE_BYTES ||
        s.input_queue_bytes > TerminalSession::MAX_INPUT_QUEUE_BYTES - len) {
        s.input_error = "Terminal input queue is full; the child process is not accepting input";
        return;
    }
    if (!s.writer_thread.joinable()) {
        s.writer_done.store(false, std::memory_order_release);
        std::string error;
        if (!s.writer_thread.start([&s]() { writer_thread_func(&s); }, &error,
                aida::infra::win_thread::default_stack_reserve, "terminal_writer")) {
            s.writer_done.store(true, std::memory_order_release);
            s.input_error = error.empty() ? "The terminal input worker could not start" : error;
            return;
        }
    }
    if (!s.input_queue.empty() && len <= (64ULL << 10) &&
        s.input_queue.back().size() <= (64ULL << 10) - len)
        s.input_queue.back().append(data, len);
    else
        s.input_queue.emplace_back(data, len);
    s.input_queue_bytes += len;
    lock.unlock();
    s.input_cv.notify_one();
}

void clear_session(TerminalSession& s)
{
    std::lock_guard<std::mutex> lk(s.buffer_mtx);
    s.lines.clear();
    s.line_entrance_time.clear();
    s.cursor_row = 0;
    s.cursor_col = 0;
    s.scroll_y = 0.f;
    s.scroll_to_bottom = true;
    s.auto_follow = true;
    s.prev_line_count = 0;
    s.search_matches.clear();
    s.active_search_match = -1;
    s.ansi_state = ansi_state_t::normal;
    s.ansi_params.clear();
    s.buffer_generation.fetch_add(1, std::memory_order_acq_rel);
}

bool try_clear_session(TerminalSession& s)
{
    std::unique_lock<std::mutex> lk(s.buffer_mtx, std::try_to_lock);
    if (!lk.owns_lock())
        return false;
    s.lines.clear();
    s.line_entrance_time.clear();
    s.cursor_row = 0;
    s.cursor_col = 0;
    s.scroll_y = 0.f;
    s.scroll_to_bottom = true;
    s.auto_follow = true;
    s.prev_line_count = 0;
    s.search_matches.clear();
    s.active_search_match = -1;
    s.buffer_generation.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool refresh_search(TerminalSession& s, const std::string& query)
{
    const std::uint64_t generation = s.buffer_generation.load(std::memory_order_acquire);
    if (query == s.search_query && generation == s.searched_generation)
        return true;
    std::unique_lock<std::mutex> lock(s.buffer_mtx, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    std::vector<TerminalSession::search_match_t> matches;
    if (!query.empty()) {
        for (std::size_t line_index = 0; line_index < s.lines.size(); ++line_index) {
            const auto& line = s.lines[line_index];
            std::string text;
            text.reserve(line.size());
            for (const auto& cell : line)
                text.push_back(cell.ch);
            auto begin = text.begin();
            while (begin != text.end()) {
                const auto found = std::search(begin, text.end(), query.begin(), query.end(),
                    [](unsigned char left, unsigned char right) {
                        return std::tolower(left) == std::tolower(right);
                    });
                if (found == text.end())
                    break;
                const auto column = static_cast<int>(std::distance(text.begin(), found));
                matches.push_back({static_cast<int>(line_index), column,
                    static_cast<int>(query.size())});
                begin = found + 1;
            }
        }
    }
    s.search_query = query;
    s.search_matches = std::move(matches);
    s.searched_generation = generation;
    if (s.search_matches.empty())
        s.active_search_match = -1;
    else if (s.active_search_match < 0 ||
             s.active_search_match >= static_cast<int>(s.search_matches.size()))
        s.active_search_match = 0;
    return true;
}

bool move_search_match(TerminalSession& s, int delta)
{
    if (s.search_matches.empty())
        return false;
    const int count = static_cast<int>(s.search_matches.size());
    int next = s.active_search_match < 0 ? 0 : s.active_search_match + delta;
    next %= count;
    if (next < 0) next += count;
    s.active_search_match = next;
    s.auto_follow = false;
    s.scroll_y = static_cast<float>(s.search_matches[static_cast<std::size_t>(next)].line);
    return true;
}

bool try_copy_all_text(TerminalSession& s, std::string& all_text)
{
    std::unique_lock<std::mutex> lk(s.buffer_mtx, std::try_to_lock);
    if (!lk.owns_lock())
        return false;
    all_text.clear();
    all_text.reserve(s.lines.size() * static_cast<size_t>(std::max(1, s.cols + 1)));
    for (auto& row : s.lines) {
        for (auto& cell : row)
            all_text += cell.ch;
        while (!all_text.empty() && all_text.back() == ' ')
            all_text.pop_back();
        all_text += '\n';
    }
    return true;
}


void resize_pty(TerminalSession& s, int cols, int rows)
{
    s.cols = cols;
    s.rows_vis = rows;
    if (s.hPC != INVALID_HANDLE_VALUE) {
        COORD size;
        size.X = static_cast<SHORT>(cols);
        size.Y = static_cast<SHORT>(rows);
        ResizePseudoConsole(s.hPC, size);
    }
}


void release_session_process_handles(TerminalSession& s)
{
    close_native_handle(s.hProcess);
    close_native_handle(s.hThread);
}

bool finalize_destroyed_session(TerminalSession& s, std::uint32_t timeout_ms)
{
    if (s.reader_thread.joinable() && !s.reader_thread.join_for(timeout_ms))
        return false;
    if (s.writer_thread.joinable() && !s.writer_thread.join_for(timeout_ms))
        return false;
    if (!s.reader_done.load(std::memory_order_acquire))
        return false;
    if (!s.writer_done.load(std::memory_order_acquire))
        return false;
    release_session_process_handles(s);
    return true;
}

bool destroy_session(TerminalSession& s)
{
    s.stop_reader.store(true, std::memory_order_release);
    s.alive.store(false, std::memory_order_release);

    unsigned reader_tid = 0;
    unsigned writer_tid = 0;
    const HANDLE log_pipe_in = s.hPipeIn;
    const HANDLE log_pipe_out = s.hPipeOut;
    const HPCON log_hpc = s.hPC;
    const HANDLE log_process = s.hProcess;
    if (s.reader_thread.joinable()) {
        reader_tid = s.reader_thread.id();
        if (reader_tid != 0) {
            HANDLE hThread = OpenThread(THREAD_TERMINATE, FALSE, static_cast<DWORD>(reader_tid));
            if (hThread) {
                CancelSynchronousIo(hThread);
                close_native_handle(hThread);
            }
        }
    }
    if (s.writer_thread.joinable()) {
        writer_tid = s.writer_thread.id();
        if (writer_tid != 0) {
            HANDLE hThread = OpenThread(THREAD_TERMINATE, FALSE, static_cast<DWORD>(writer_tid));
            if (hThread) {
                CancelSynchronousIo(hThread);
                close_native_handle(hThread);
            }
        }
    }
    s.input_cv.notify_all();

    close_native_handle(s.hPipeOut);
    close_native_handle(s.hPipeIn);
    close_native_pseudoconsole(s.hPC);
    if (s.hJob != INVALID_HANDLE_VALUE) {
        TerminateJobObject(s.hJob, 0);
        close_native_handle(s.hJob);
    } else if (s.hProcess != INVALID_HANDLE_VALUE) {
        TerminateProcess(s.hProcess, 0);
    }
    if (finalize_destroyed_session(s, 0))
        return true;
    diag::log_tagged_fmt("terminal",
        "terminal_workers_join_deferred reader_tid=%u writer_tid=%u pipe_in=%p pipe_out=%p hpc=%p process=%p",
        reader_tid,
        writer_tid,
        log_pipe_in,
        log_pipe_out,
        log_hpc,
        log_process);
    return false;
}


std::vector<profile_t> available_profiles(const std::string& configured_shell)
{
    std::vector<profile_t> profiles;
    std::unordered_set<std::wstring> commands;
    const auto add = [&](std::string id, std::string label, std::wstring command) {
        if (command.empty()) return;
        std::wstring key = command;
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        if (commands.insert(key).second)
            profiles.push_back({std::move(id), std::move(label), std::move(command)});
    };
    add("configured", "Configured Shell",
        std::wstring(configured_shell.begin(), configured_shell.end()));
    const auto resolved = [](const wchar_t* executable) {
        std::vector<wchar_t> buffer(32768);
        const DWORD count = SearchPathW(nullptr, executable, nullptr,
            static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (count == 0 || count >= buffer.size()) return std::wstring{};
        const std::wstring path(buffer.data(), count);
        return path.find(L' ') == std::wstring::npos ? path : L"\"" + path + L"\"";
    };
    add("powershell", "Windows PowerShell", resolved(L"powershell.exe"));
    add("pwsh", "PowerShell 7", resolved(L"pwsh.exe"));
    add("cmd", "Command Prompt", resolved(L"cmd.exe"));
    return profiles;
}

void TerminalManager::reap_retired_sessions()
{
    auto it = retired_sessions.begin();
    while (it != retired_sessions.end()) {
        auto* session = *it;
        if (session && finalize_destroyed_session(*session, 0)) {
            delete session;
            it = retired_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

void TerminalManager::retire_or_delete(TerminalSession* session)
{
    if (!session)
        return;
    if (destroy_session(*session))
        delete session;
    else
        retired_sessions.push_back(session);
}

TerminalSession* TerminalManager::create_terminal(const wchar_t* shell,
                                                  const wchar_t* cwd,
                                                  const char* profile_id,
                                                  const char* profile_label)
{
    auto* session = new TerminalSession();
    session->id = next_id++;
    session->profile_id = profile_id ? profile_id : "configured";
    session->profile_label = profile_label ? profile_label : "Configured Shell";
    session->title = session->profile_label + " " + std::to_string(session->id);
    session->output_notify = output_notify;
    if (create_session(*session, shell, cwd)) {
        sessions.push_back(session);
        active_tab = static_cast<int>(sessions.size()) - 1;
        last_error.clear();
        return session;
    }
    last_error = session->start_error.empty()
        ? "The configured terminal process could not be started" : session->start_error;
    delete session;
    return nullptr;
}

void TerminalManager::close_terminal(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(sessions.size()))
        return;
    const size_t session_index = static_cast<size_t>(idx);
    retire_or_delete(sessions[session_index]);
    sessions.erase(sessions.begin() + static_cast<std::ptrdiff_t>(session_index));
    if (secondary_tab == idx) {
        secondary_tab = -1;
        split_mode = split_mode_t::none;
    } else if (secondary_tab > idx) {
        --secondary_tab;
    }
    if (active_tab > idx) --active_tab;
    if (active_tab >= static_cast<int>(sessions.size()))
        active_tab = static_cast<int>(sessions.size()) - 1;
    if (sessions.empty()) {
        active_tab = -1;
        secondary_tab = -1;
        split_mode = split_mode_t::none;
    }
}

bool TerminalManager::restart_terminal(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(sessions.size())) {
        last_error = "The terminal session selected for restart no longer exists";
        return false;
    }
    auto* prior = sessions[static_cast<std::size_t>(idx)];
    if (!prior) {
        last_error = "The terminal session selected for restart is unavailable";
        return false;
    }
    auto* replacement = new TerminalSession();
    replacement->id = prior->id;
    replacement->title = prior->title;
    replacement->profile_id = prior->profile_id;
    replacement->profile_label = prior->profile_label;
    replacement->output_notify = output_notify;
    const std::wstring command = prior->command;
    const std::wstring cwd = prior->cwd;
    if (!create_session(*replacement, command.c_str(), cwd.empty() ? nullptr : cwd.c_str())) {
        last_error = replacement->start_error.empty()
            ? "The terminal session could not be restarted" : replacement->start_error;
        delete replacement;
        return false;
    }
    sessions[static_cast<std::size_t>(idx)] = replacement;
    retire_or_delete(prior);
    last_error.clear();
    return true;
}

bool TerminalManager::cycle(int delta)
{
    if (sessions.empty()) return false;
    const int count = static_cast<int>(sessions.size());
    active_tab = (active_tab + delta) % count;
    if (active_tab < 0) active_tab += count;
    return true;
}

bool TerminalManager::set_split(split_mode_t mode)
{
    if (mode == split_mode_t::none) {
        secondary_tab = -1;
        split_mode = mode;
        return true;
    }
    if (sessions.size() < 2)
        return false;
    if (secondary_tab < 0 || secondary_tab >= static_cast<int>(sessions.size()) ||
        secondary_tab == active_tab) {
        secondary_tab = active_tab == 0 ? 1 : 0;
    }
    split_mode = mode;
    return true;
}

void TerminalManager::shutdown()
{
    for (auto* session : sessions)
        retire_or_delete(session);
    sessions.clear();
    reap_retired_sessions();
    if (!retired_sessions.empty())
        diag::log_tagged_fmt("terminal", "shutdown_readers_deferred count=%zu",
            retired_sessions.size());
    active_tab = -1;
    secondary_tab = -1;
    split_mode = split_mode_t::none;
}

}
