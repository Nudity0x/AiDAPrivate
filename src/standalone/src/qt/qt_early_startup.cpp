#include "qt/qt_early_startup.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>

#include "core/diagnostics/crash_snapshot.hpp"

namespace aida_early_startup {

static constexpr DWORD kMaxLogBytes = 1024u * 1024u;
std::atomic<const char*> g_phase{ "image_static_init_pending" };
static std::atomic<bool> g_veh_installed{ false };
static std::atomic<bool> g_fatal_exception_written{ false };
static std::atomic<bool> g_status_exception_written{ false };
static std::atomic<bool> g_unhandled_exception_written{ false };
static std::atomic<bool> g_normal_diagnostics_reached{ false };
static std::atomic<long> g_write_active{ 0 };

static size_t bounded_strlen(const char* s, size_t cap)
{
    if (!s)
        return 0;
    size_t n = 0;
    while (n < cap && s[n] != '\0')
        ++n;
    return n;
}

static size_t bounded_wcslen(const wchar_t* s, size_t cap)
{
    if (!s)
        return 0;
    size_t n = 0;
    while (n < cap && s[n] != L'\0')
        ++n;
    return n;
}

static uint64_t fnv1a_wide(const wchar_t* s, size_t cap)
{
    uint64_t h = 14695981039346656037ULL;
    if (!s)
        return h;
    for (size_t i = 0; i < cap && s[i] != L'\0'; ++i) {
        wchar_t ch = s[i];
        h ^= static_cast<uint8_t>(ch & 0xFFu);
        h *= 1099511628211ULL;
        h ^= static_cast<uint8_t>((ch >> 8) & 0xFFu);
        h *= 1099511628211ULL;
    }
    return h;
}

void wide_to_utf8(const wchar_t* in, char* out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (!in)
        return;
    int wrote = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, in, -1, out, static_cast<int>(cap), nullptr, nullptr);
    if (wrote <= 0)
        wrote = WideCharToMultiByte(CP_ACP, 0, in, -1, out, static_cast<int>(cap), nullptr, nullptr);
    if (wrote <= 0)
        _snprintf_s(out, cap, _TRUNCATE, "<wide_conversion_failed_gle_%lu>", GetLastError());
}

static bool build_exe_log_path(wchar_t* out, size_t cap)
{
    if (!out || cap == 0)
        return false;
    out[0] = L'\0';
    wchar_t exe[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
    wchar_t* last = wcsrchr(exe, L'\\');
    if (!last)
        return false;
    *(last + 1) = L'\0';
    _snwprintf_s(out, cap, _TRUNCATE, L"%saida_early_startup.log", exe);
    return out[0] != L'\0';
}

static bool append_file(const wchar_t* path, const char* line)
{
    if (!path || !line)
        return false;
    size_t len = bounded_strlen(line, 8192);
    if (len == 0)
        return false;
    DWORD creation = OPEN_ALWAYS;
    WIN32_FILE_ATTRIBUTE_DATA existing{};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &existing)) {
        ULARGE_INTEGER size{};
        size.LowPart = existing.nFileSizeLow;
        size.HighPart = existing.nFileSizeHigh;
        if (size.QuadPart > kMaxLogBytes)
            creation = CREATE_ALWAYS;
    }
    HANDLE h = CreateFileW(path,
        FILE_APPEND_DATA | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        creation,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, line, static_cast<DWORD>(len), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok && written == static_cast<DWORD>(len);
}

static size_t image_size_from_headers(HMODULE image)
{
    if (!image)
        return 0;
    __try {
        auto* base = reinterpret_cast<const uint8_t*>(image);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
            return 0;
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return 0;
        return static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        aida::diagnostics::crash::emit_crash_breadcrumb(GetExceptionCode(), nullptr, "early_image_size_seh");
        return 0;
    }
}

static void token_elevation_and_session(int& elevated, DWORD& session, int& session_ok)
{
    elevated = -1;
    session = 0xFFFFFFFFu;
    session_ok = 0;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION te{};
        DWORD cb = 0;
        if (GetTokenInformation(token, TokenElevation, &te, sizeof(te), &cb))
            elevated = te.TokenIsElevated ? 1 : 0;
        CloseHandle(token);
    }
    DWORD sid = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sid)) {
        session = sid;
        session_ok = 1;
    }
}

static void write_line(const char* event_name, const char* detail)
{
    if (g_write_active.exchange(1, std::memory_order_acq_rel) != 0)
        return;

    wchar_t exe[MAX_PATH] = {};
    wchar_t cwd[MAX_PATH] = {};
    DWORD exe_len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    DWORD exe_gle = exe_len ? 0 : GetLastError();
    DWORD cwd_len = GetCurrentDirectoryW(MAX_PATH, cwd);
    DWORD cwd_gle = (cwd_len > 0 && cwd_len < MAX_PATH) ? 0 : GetLastError();
    const wchar_t* cmd = GetCommandLineW();
    const size_t cmd_len = bounded_wcslen(cmd, 32768);
    const uint64_t cmd_hash = fnv1a_wide(cmd, 32768);
    int elevated = -1;
    DWORD session = 0xFFFFFFFFu;
    int session_ok = 0;
    token_elevation_and_session(elevated, session, session_ok);

    HMODULE image = GetModuleHandleW(nullptr);
    const uintptr_t image_base = reinterpret_cast<uintptr_t>(image);
    const size_t image_size = image_size_from_headers(image);
    const uintptr_t image_end = image_base + image_size;
    MEMORY_BASIC_INFORMATION mbi{};
    if (image)
        VirtualQuery(image, &mbi, sizeof(mbi));

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    BOOL fad_ok = exe_len > 0 && exe_len < MAX_PATH
        ? GetFileAttributesExW(exe, GetFileExInfoStandard, &fad)
        : FALSE;

    char exe_u8[1024] = {};
    char cwd_u8[1024] = {};
    wide_to_utf8(exe, exe_u8, sizeof(exe_u8));
    wide_to_utf8(cwd, cwd_u8, sizeof(cwd_u8));

    SYSTEMTIME st{};
    GetLocalTime(&st);
    const char* phase = g_phase.load(std::memory_order_acquire);
    const bool normal_diag = g_normal_diagnostics_reached.load(std::memory_order_acquire);
    char line[8192] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [early_startup] event=%s phase=%s detail=%s normal_diag=%d pid=%lu tid=%lu tick=%llu exe_len=%lu exe_gle=%lu exe=%s module=%s cwd_len=%lu cwd_gle=%lu cwd=%s cmd_len=%llu cmd_hash=0x%016llX elevated=%d session=%lu session_ok=%d image_base=0x%016llX image_end=0x%016llX image_size=0x%llX mbi_base=0x%016llX mbi_alloc=0x%016llX mbi_size=0x%llX mbi_state=0x%08lX mbi_protect=0x%08lX exe_write_ok=%d exe_write_ft=0x%08lX%08lX build=%s_%s\r\n",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        event_name ? event_name : "<null>",
        phase ? phase : "<null>",
        detail ? detail : "<null>",
        normal_diag ? 1 : 0,
        GetCurrentProcessId(),
        GetCurrentThreadId(),
        static_cast<unsigned long long>(GetTickCount64()),
        static_cast<unsigned long>(exe_len),
        static_cast<unsigned long>(exe_gle),
        exe_u8,
        exe_u8,
        static_cast<unsigned long>(cwd_len),
        static_cast<unsigned long>(cwd_gle),
        cwd_u8,
        static_cast<unsigned long long>(cmd_len),
        static_cast<unsigned long long>(cmd_hash),
        elevated,
        static_cast<unsigned long>(session),
        session_ok,
        static_cast<unsigned long long>(image_base),
        static_cast<unsigned long long>(image_end),
        static_cast<unsigned long long>(image_size),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mbi.AllocationBase)),
        static_cast<unsigned long long>(mbi.RegionSize),
        static_cast<unsigned long>(mbi.State),
        static_cast<unsigned long>(mbi.Protect),
        fad_ok ? 1 : 0,
        fad_ok ? fad.ftLastWriteTime.dwHighDateTime : 0,
        fad_ok ? fad.ftLastWriteTime.dwLowDateTime : 0,
        __DATE__,
        __TIME__);

    wchar_t path[MAX_PATH] = {};
    if (build_exe_log_path(path, _countof(path)))
        append_file(path, line);

    g_write_active.store(0, std::memory_order_release);
}

static bool is_fatal_exception_code(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_INVALID_DISPOSITION:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
        return true;
    default:
        return code == 0xC0000409u || code == 0x40000015u;
    }
}

static bool is_status_exception_code(DWORD code)
{
    return code == STATUS_SINGLE_STEP || code == EXCEPTION_BREAKPOINT || code == STATUS_GUARD_PAGE_VIOLATION;
}

static void write_exception_line(const char* handler, EXCEPTION_POINTERS* ep, bool allow_all)
{
    if (!ep || !ep->ExceptionRecord)
        return;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool fatal = is_fatal_exception_code(code);
    const bool status = is_status_exception_code(code);
    if (!allow_all && !fatal && !status)
        return;
    std::atomic<bool>* gate = allow_all ? &g_unhandled_exception_written : (fatal ? &g_fatal_exception_written : &g_status_exception_written);
    bool expected = false;
    if (!gate->compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    CONTEXT* ctx = ep->ContextRecord;
    uintptr_t rip = 0;
    uintptr_t rsp = 0;
    uintptr_t rbp = 0;
#if defined(_M_X64)
    if (ctx) {
        rip = static_cast<uintptr_t>(ctx->Rip);
        rsp = static_cast<uintptr_t>(ctx->Rsp);
        rbp = static_cast<uintptr_t>(ctx->Rbp);
    }
#endif
    const uintptr_t addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    HMODULE crash_mod = nullptr;
    wchar_t crash_mod_w[MAX_PATH] = L"<unknown>";
    char crash_mod_u8[1024] = "<unknown>";
    if (ep->ExceptionRecord->ExceptionAddress &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(ep->ExceptionRecord->ExceptionAddress),
            &crash_mod) &&
        crash_mod) {
        GetModuleFileNameW(crash_mod, crash_mod_w, MAX_PATH);
        wide_to_utf8(crash_mod_w, crash_mod_u8, sizeof(crash_mod_u8));
    }
    const uintptr_t crash_mod_base = reinterpret_cast<uintptr_t>(crash_mod);
    const uintptr_t module_off = crash_mod_base && addr >= crash_mod_base ? addr - crash_mod_base : 0;
    const unsigned long params = static_cast<unsigned long>(ep->ExceptionRecord->NumberParameters);
    const unsigned long long p0 = ep->ExceptionRecord->NumberParameters > 0
        ? static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[0])
        : 0ULL;
    const unsigned long long p1 = ep->ExceptionRecord->NumberParameters > 1
        ? static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1])
        : 0ULL;
    char detail[1024] = {};
    _snprintf_s(detail, sizeof(detail), _TRUNCATE,
        "handler=%s exception code=0x%08lX fatal=%d status=%d flags=0x%08lX addr=0x%016llX rip=0x%016llX rsp=0x%016llX rbp=0x%016llX module=%s module_off=0x%llX params=%lu p0=0x%016llX p1=0x%016llX last_error=%lu",
        handler ? handler : "<null>",
        static_cast<unsigned long>(code),
        fatal ? 1 : 0,
        status ? 1 : 0,
        static_cast<unsigned long>(ep->ExceptionRecord->ExceptionFlags),
        static_cast<unsigned long long>(addr),
        static_cast<unsigned long long>(rip),
        static_cast<unsigned long long>(rsp),
        static_cast<unsigned long long>(rbp),
        crash_mod_u8,
        static_cast<unsigned long long>(module_off),
        params,
        p0,
        p1,
        GetLastError());
    write_line(allow_all ? "unhandled_exception" : (fatal ? "veh_first_chance_fatal_exception" : "veh_first_chance_status_exception"), detail);
}

static LONG CALLBACK early_veh(EXCEPTION_POINTERS* ep)
{
    write_exception_line("early_veh", ep, false);
    if (ep && ep->ExceptionRecord)
        aida::diagnostics::crash::emit_crash_breadcrumb(ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, "early_veh");
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI early_unhandled(EXCEPTION_POINTERS* ep)
{
    write_exception_line("early_unhandled", ep, true);
    if (ep && ep->ExceptionRecord)
        aida::diagnostics::crash::emit_crash_breadcrumb(ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, "early_unhandled");
    return EXCEPTION_CONTINUE_SEARCH;
}

void install()
{
    bool expected = false;
    PVOID veh = nullptr;
    bool added_veh = g_veh_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    if (added_veh)
        veh = AddVectoredExceptionHandler(1, early_veh);
    SetUnhandledExceptionFilter(early_unhandled);
    char detail[160] = {};
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, "early_veh=0x%016llX early_veh_added=%d unhandled_set=1 gle=%lu", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(veh)), added_veh ? 1 : 0, GetLastError());
    write_line("install", detail);
}

void mark(const char* phase)
{
    g_phase.store(phase ? phase : "<null>", std::memory_order_release);
    write_line("phase", phase ? phase : "<null>");
}

void mark_normal_diagnostics_reached()
{
    g_normal_diagnostics_reached.store(true, std::memory_order_release);
    mark("normal_diag_reached");
}

struct bootstrap_t {
    bootstrap_t()
    {
        g_phase.store("static_ctor_enter", std::memory_order_release);
        install();
        mark("static_ctor_exit");
    }
};

static bootstrap_t g_bootstrap;

}
