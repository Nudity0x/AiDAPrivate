#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "helpers/diag_log.hpp"
#include "core/diagnostics/crash_snapshot.hpp"
#include "core/infra/executor.hpp"
#include "core/infra/taskflow_runtime.hpp"
#include "core/mcp/mcp_standalone.hpp"
#include "core/runtime/diagnostic_exception_scope.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/testlab/test_all_features.hpp"
#include "core/ui/ui_thread_dispatcher.hpp"
#include "qt/qt_early_startup.hpp"

namespace aida_tracer {
    const char* message_name(UINT msg);
    void format_tracer_crash_snapshot(char* out, std::size_t cap);
    const char* crash_render_phase_name();
    std::uint64_t crash_render_last_tick_ms();
    const char* crash_wndproc_stage();
    const char* crash_dispatch_stage();
    UINT crash_dispatch_msg();
    UINT crash_wndproc_msg();
}

namespace aida_shutdown_diag {
    const char* phase_name();
    std::uint64_t phase_age_ms();
}

std::uint64_t g_last_input_event_tick_ms = 0;

namespace aida::qt::crash_integration {

using snapshot_provider_t = void (*)(char* out, std::size_t cap);

bool register_snapshot_provider(const char* name, snapshot_provider_t provider);
PVOID install_diagnostic_veh();
void install_unhandled_exception_filter();
bool safe_read_qword(const void* p, std::uintptr_t& out);
void format_taskflow_runtime_crash_snapshot(char* out, std::size_t cap);
void format_taskflow_runtime_hung_snapshot(char* out, std::size_t cap);

namespace {

struct snapshot_provider_slot_t {
    char name[64];
    snapshot_provider_t provider;
};

static snapshot_provider_slot_t g_provider_slots[16] = {};
static std::atomic<std::size_t> g_provider_count{0};

}

bool register_snapshot_provider(const char* name, snapshot_provider_t provider)
{
    if (!provider) {
        diag::log_tagged_critical("diag", "crash_snapshot_provider_rejected reason=null_provider");
        return false;
    }
    const std::size_t idx = g_provider_count.fetch_add(1, std::memory_order_acq_rel);
    if (idx >= 16) {
        g_provider_count.store(16, std::memory_order_release);
        diag::log_tagged_critical_fmt("diag",
            "crash_snapshot_provider_rejected name=%s reason=registry_full",
            name ? name : "<unnamed>");
        return false;
    }
    _snprintf_s(g_provider_slots[idx].name, sizeof(g_provider_slots[idx].name), _TRUNCATE, "%s", name ? name : "<unnamed>");
    g_provider_slots[idx].provider = provider;
    diag::log_tagged_critical_fmt("diag",
        "crash_snapshot_provider_registered name=%s slot=%llu tid=%lu",
        g_provider_slots[idx].name,
        static_cast<unsigned long long>(idx),
        static_cast<unsigned long>(::GetCurrentThreadId()));
    return true;
}

bool safe_read_qword(const void* p, std::uintptr_t& out)
{
    __try {
        out = *reinterpret_cast<const std::uintptr_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        aida::diagnostics::crash::emit_crash_breadcrumb(GetExceptionCode(), nullptr, "safe_read_qword");
        out = 0;
        return false;
    }
}

static bool aida_is_fatal_exception_code(DWORD code)
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
        return code == 0xC0000409u;
    }
}

static bool aida_build_local_log_path(const char* file_name, char* out, size_t cap)
{
    if (!file_name || !out || cap == 0)
        return false;
    out[0] = '\0';
    char module[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, module, static_cast<DWORD>(sizeof(module)));
    if (n == 0 || n >= sizeof(module))
        return false;
    char* slash = std::strrchr(module, '\\');
    if (!slash)
        return false;
    *(slash + 1) = '\0';
    _snprintf_s(out, cap, _TRUNCATE, "%s%s", module, file_name);
    return out[0] != '\0';
}

static void aida_append_direct_log_line(const char* file_name, const char* msg)
{
    if (!file_name || !msg || msg[0] == '\0')
        return;
    char path[MAX_PATH] = {};
    if (!aida_build_local_log_path(file_name, path, sizeof(path)))
        return;
    HANDLE hf = CreateFileA(path,
        FILE_APPEND_DATA | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (hf == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    const DWORD len = static_cast<DWORD>(strnlen_s(msg, 4095));
    if (len != 0)
        WriteFile(hf, msg, len, &written, nullptr);
    DWORD newline_written = 0;
    WriteFile(hf, "\r\n", 2, &newline_written, nullptr);
    FlushFileBuffers(hf);
    CloseHandle(hf);
}

static void aida_append_direct_fatal_line(const char* msg)
{
    aida_append_direct_log_line("aida_crash.log", msg);
    aida_append_direct_log_line("aida_debug.log", msg);
}

static void aida_write_minimal_fatal_exception_line(EXCEPTION_POINTERS* ep, const char* phase)
{
    if (!ep || !ep->ExceptionRecord)
        return;
    CONTEXT* ctx = ep->ContextRecord;
    HMODULE exe_base = GetModuleHandleA(nullptr);
    uintptr_t exe_addr = reinterpret_cast<uintptr_t>(exe_base);
    uintptr_t rip = ctx ? static_cast<uintptr_t>(ctx->Rip) : 0;
    uintptr_t rsp = ctx ? static_cast<uintptr_t>(ctx->Rsp) : 0;
    uintptr_t rbp = ctx ? static_cast<uintptr_t>(ctx->Rbp) : 0;
    uintptr_t crash_addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    uintptr_t rip_offset = exe_addr && rip >= exe_addr ? rip - exe_addr : 0;
    uintptr_t addr_offset = exe_addr && crash_addr >= exe_addr ? crash_addr - exe_addr : 0;
    unsigned long long p0 = ep->ExceptionRecord->NumberParameters > 0
        ? static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[0])
        : 0ULL;
    unsigned long long p1 = ep->ExceptionRecord->NumberParameters > 1
        ? static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1])
        : 0ULL;
    char line[1536] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
        "FIRST_CHANCE_FATAL_EXCEPTION phase=%s code=0x%08X addr=0x%016llX addr_off_exe=0x%llX rip=0x%016llX rip_off_exe=0x%llX rsp=0x%016llX rbp=0x%016llX tid=%lu flags=0x%08X params=%lu p0=0x%016llX p1=0x%016llX last_error=%lu tick=%llu",
        phase ? phase : "minimal",
        ep->ExceptionRecord->ExceptionCode,
        static_cast<unsigned long long>(crash_addr),
        static_cast<unsigned long long>(addr_offset),
        static_cast<unsigned long long>(rip),
        static_cast<unsigned long long>(rip_offset),
        static_cast<unsigned long long>(rsp),
        static_cast<unsigned long long>(rbp),
        GetCurrentThreadId(),
        ep->ExceptionRecord->ExceptionFlags,
        static_cast<unsigned long>(ep->ExceptionRecord->NumberParameters),
        p0,
        p1,
        GetLastError(),
        static_cast<unsigned long long>(GetTickCount64()));
    aida_append_direct_fatal_line(line);
}

static const char* crash_basename_ptr(const char* path)
{
    if (!path)
        return "<none>";
    const char* slash = std::strrchr(path, '\\');
    const char* fwd = std::strrchr(path, '/');
    const char* base = slash && fwd ? (slash > fwd ? slash : fwd) : (slash ? slash : fwd);
    return base ? base + 1 : path;
}

static void format_current_thread_description(char* out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    using get_thread_description_t = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto fn = kernel ? reinterpret_cast<get_thread_description_t>(GetProcAddress(kernel, "GetThreadDescription")) : nullptr;
    if (!fn) {
        HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
        fn = kernelbase ? reinterpret_cast<get_thread_description_t>(GetProcAddress(kernelbase, "GetThreadDescription")) : nullptr;
    }
    if (!fn) {
        _snprintf_s(out, cap, _TRUNCATE, "<unavailable>");
        return;
    }
    PWSTR desc = nullptr;
    HRESULT hr = fn(GetCurrentThread(), &desc);
    if (SUCCEEDED(hr) && desc) {
        int wrote = WideCharToMultiByte(CP_UTF8, 0, desc, -1, out, static_cast<int>(cap), nullptr, nullptr);
        if (wrote <= 0)
            _snprintf_s(out, cap, _TRUNCATE, "<convert_failed gle=%lu>", GetLastError());
        LocalFree(desc);
        if (out[0] == 0)
            _snprintf_s(out, cap, _TRUNCATE, "<empty>");
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "<hr=0x%08lX>", static_cast<unsigned long>(hr));
}

static void append_stack_module_token(char* out, size_t cap, int idx, uintptr_t value)
{
    if (!out || cap == 0)
        return;
    size_t len = 0;
    while (len < cap && out[len] != 0)
        ++len;
    if (len >= cap - 1)
        return;
    HMODULE mod = nullptr;
    char path[MAX_PATH] = {};
    const bool have_mod = value != 0 &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(value), &mod) &&
        mod;
    if (have_mod)
        GetModuleFileNameA(mod, path, static_cast<DWORD>(sizeof(path)));
    const uintptr_t base = reinterpret_cast<uintptr_t>(mod);
    const uintptr_t off = have_mod && value >= base ? value - base : 0;
    _snprintf_s(out + len, cap - len, _TRUNCATE,
        "%s[%02d]=0x%016llX:%s+0x%llX",
        len == 0 ? "" : " ",
        idx,
        static_cast<unsigned long long>(value),
        have_mod ? crash_basename_ptr(path) : "no_module",
        static_cast<unsigned long long>(off));
}

static void format_context_stack_modules(CONTEXT* ctx, char* out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if (!ctx) {
        _snprintf_s(out, cap, _TRUNCATE, "<no_context>");
        return;
    }
#if defined(_M_X64)
    const uintptr_t* rsp_ptr = reinterpret_cast<const uintptr_t*>(ctx->Rsp);
#else
    const uintptr_t* rsp_ptr = reinterpret_cast<const uintptr_t*>(ctx->Esp);
#endif
    for (int i = 0; i < 32; ++i) {
        uintptr_t value = 0;
        if (!safe_read_qword(rsp_ptr + i, value))
            break;
        if (value == 0)
            continue;
        HMODULE mod = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(value), &mod) || !mod)
            continue;
        append_stack_module_token(out, cap, i, value);
    }
    if (out[0] == 0)
        _snprintf_s(out, cap, _TRUNCATE, "<no_module_stack_values>");
}

enum class taskflow_family_diag_kind_t {
    work,
    service,
    critical
};

struct taskflow_family_diag_t {
    uint32_t active = 0;
    uint64_t pending = 0;
    uint64_t oldest_active_ms = 0;
    uint32_t active_label_count = 0;
    std::string active_labels;
};

static taskflow_family_diag_kind_t taskflow_family_for_domain(aida::infra::taskflow_runtime::executor_domain_t domain)
{
    using domain_t = aida::infra::taskflow_runtime::executor_domain_t;
    switch (domain) {
    case domain_t::service:
    case domain_t::long_running:
        return taskflow_family_diag_kind_t::service;
    case domain_t::critical:
    case domain_t::security_liveness:
        return taskflow_family_diag_kind_t::critical;
    case domain_t::general:
    case domain_t::ui_dispatch:
    case domain_t::external_tool:
    case domain_t::feature_worker:
    case domain_t::diagnostics:
    default:
        return taskflow_family_diag_kind_t::work;
    }
}

static taskflow_family_diag_t make_taskflow_family_diag(
    const aida::infra::taskflow_runtime::runtime_snapshot_t& snapshot,
    taskflow_family_diag_kind_t family)
{
    taskflow_family_diag_t out;
    if (family == taskflow_family_diag_kind_t::service) {
        out.active = snapshot.service_queue_active;
        out.pending = snapshot.service_queue_pending;
    } else if (family == taskflow_family_diag_kind_t::critical) {
        out.active = snapshot.critical_queue_active;
        out.pending = snapshot.critical_queue_pending;
    } else {
        out.active = snapshot.work_queue_active;
        out.pending = snapshot.work_queue_pending;
    }
    for (const auto& job : snapshot.active_jobs) {
        if (taskflow_family_for_domain(job.domain) != family)
            continue;
        ++out.active_label_count;
        if (job.active_ms > out.oldest_active_ms)
            out.oldest_active_ms = job.active_ms;
        if (out.active_labels.size() < 900) {
            if (!out.active_labels.empty())
                out.active_labels += ";";
            out.active_labels += "#";
            out.active_labels += std::to_string(job.job_id);
            out.active_labels += ":";
            out.active_labels += aida::infra::taskflow_runtime::domain_name(job.domain);
            out.active_labels += ":";
            out.active_labels += aida::infra::taskflow_runtime::job_state_name(job.state);
            out.active_labels += ":";
            out.active_labels += job.label.empty() ? "<unnamed>" : job.label;
        }
    }
    return out;
}

void format_taskflow_runtime_crash_snapshot(char* out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    const auto runtime_snapshot = aida::infra::taskflow_runtime::active_snapshot(128);
    const auto work = make_taskflow_family_diag(runtime_snapshot, taskflow_family_diag_kind_t::work);
    const auto service = make_taskflow_family_diag(runtime_snapshot, taskflow_family_diag_kind_t::service);
    const auto critical = make_taskflow_family_diag(runtime_snapshot, taskflow_family_diag_kind_t::critical);
    _snprintf_s(out, cap, _TRUNCATE,
        "taskflow{accepting=%d shutdown=%d total_active=%u oldest_active_ms=%llu submitted=%llu rejected=%llu cancelled=%llu failed=%llu timed_out=%llu labels=%.700s} work{pending=%llu active=%u active_labels=%u oldest_active_ms=%llu labels=%.700s} service{pending=%llu active=%u active_labels=%u oldest_active_ms=%llu labels=%.700s} critical{pending=%llu active=%u active_labels=%u oldest_active_ms=%llu labels=%.700s}",
        runtime_snapshot.accepting ? 1 : 0,
        runtime_snapshot.shutting_down ? 1 : 0,
        static_cast<unsigned>(runtime_snapshot.total_active),
        static_cast<unsigned long long>(runtime_snapshot.oldest_active_ms),
        static_cast<unsigned long long>(runtime_snapshot.total_submitted),
        static_cast<unsigned long long>(runtime_snapshot.total_rejected),
        static_cast<unsigned long long>(runtime_snapshot.total_cancelled),
        static_cast<unsigned long long>(runtime_snapshot.total_failed),
        static_cast<unsigned long long>(runtime_snapshot.total_timed_out),
        runtime_snapshot.labels_under_pressure.empty() ? "<none>" : runtime_snapshot.labels_under_pressure.c_str(),
        static_cast<unsigned long long>(work.pending),
        static_cast<unsigned>(work.active),
        static_cast<unsigned>(work.active_label_count),
        static_cast<unsigned long long>(work.oldest_active_ms),
        work.active_labels.empty() ? "<none>" : work.active_labels.c_str(),
        static_cast<unsigned long long>(service.pending),
        static_cast<unsigned>(service.active),
        static_cast<unsigned>(service.active_label_count),
        static_cast<unsigned long long>(service.oldest_active_ms),
        service.active_labels.empty() ? "<none>" : service.active_labels.c_str(),
        static_cast<unsigned long long>(critical.pending),
        static_cast<unsigned>(critical.active),
        static_cast<unsigned>(critical.active_label_count),
        static_cast<unsigned long long>(critical.oldest_active_ms),
        critical.active_labels.empty() ? "<none>" : critical.active_labels.c_str());
}

void format_taskflow_runtime_hung_snapshot(char* out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    constexpr uint64_t kStuckAgeMs = 5000ULL;
    const auto runtime_snapshot = aida::infra::taskflow_runtime::active_snapshot(128);
    const auto work = make_taskflow_family_diag(runtime_snapshot, taskflow_family_diag_kind_t::work);
    const auto service = make_taskflow_family_diag(runtime_snapshot, taskflow_family_diag_kind_t::service);
    const auto critical = make_taskflow_family_diag(runtime_snapshot, taskflow_family_diag_kind_t::critical);
    _snprintf_s(out, cap, _TRUNCATE,
        "taskflow{accepting=%d shutdown=%d total_active=%u stuck=%d oldest_active_ms=%llu submitted=%llu rejected=%llu failed=%llu timed_out=%llu labels=%.520s} work{pending=%llu active=%u stuck=%d oldest_active_ms=%llu active_labels=%u labels=%.520s} service{pending=%llu active=%u stuck=%d oldest_active_ms=%llu active_labels=%u labels=%.520s} critical{pending=%llu active=%u stuck=%d oldest_active_ms=%llu active_labels=%u labels=%.520s}",
        runtime_snapshot.accepting ? 1 : 0,
        runtime_snapshot.shutting_down ? 1 : 0,
        static_cast<unsigned>(runtime_snapshot.total_active),
        runtime_snapshot.oldest_active_ms >= kStuckAgeMs ? 1 : 0,
        static_cast<unsigned long long>(runtime_snapshot.oldest_active_ms),
        static_cast<unsigned long long>(runtime_snapshot.total_submitted),
        static_cast<unsigned long long>(runtime_snapshot.total_rejected),
        static_cast<unsigned long long>(runtime_snapshot.total_failed),
        static_cast<unsigned long long>(runtime_snapshot.total_timed_out),
        runtime_snapshot.labels_under_pressure.empty() ? "<none>" : runtime_snapshot.labels_under_pressure.c_str(),
        static_cast<unsigned long long>(work.pending),
        static_cast<unsigned>(work.active),
        work.oldest_active_ms >= kStuckAgeMs ? 1 : 0,
        static_cast<unsigned long long>(work.oldest_active_ms),
        static_cast<unsigned>(work.active_label_count),
        work.active_labels.empty() ? "<none>" : work.active_labels.c_str(),
        static_cast<unsigned long long>(service.pending),
        static_cast<unsigned>(service.active),
        service.oldest_active_ms >= kStuckAgeMs ? 1 : 0,
        static_cast<unsigned long long>(service.oldest_active_ms),
        static_cast<unsigned>(service.active_label_count),
        service.active_labels.empty() ? "<none>" : service.active_labels.c_str(),
        static_cast<unsigned long long>(critical.pending),
        static_cast<unsigned>(critical.active),
        critical.oldest_active_ms >= kStuckAgeMs ? 1 : 0,
        static_cast<unsigned long long>(critical.oldest_active_ms),
        static_cast<unsigned>(critical.active_label_count),
        critical.active_labels.empty() ? "<none>" : critical.active_labels.c_str());
}

static void format_shutdown_crash_snapshot(char* out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    char thread_desc[512] = {};
    char queue_snapshot[2400] = {};
    format_current_thread_description(thread_desc, sizeof(thread_desc));
    format_taskflow_runtime_crash_snapshot(queue_snapshot, sizeof(queue_snapshot));
    const char* shutdown_phase = aida_shutdown_diag::phase_name();
    const char* render_phase = aida_tracer::crash_render_phase_name();
    const char* dispatch_stage = aida_tracer::crash_dispatch_stage();
    const char* wndproc_stage = aida_tracer::crash_wndproc_stage();
    _snprintf_s(out, cap, _TRUNCATE,
        "shutdown_phase=%s shutdown_phase_age_ms=%llu tid=%lu thread_desc=%s render_phase=%s dispatch_stage=%s dispatch_msg=%s(0x%04X) wndproc_stage=%s wndproc_msg=%s(0x%04X) queues={%s}",
        shutdown_phase ? shutdown_phase : "<null>",
        static_cast<unsigned long long>(aida_shutdown_diag::phase_age_ms()),
        GetCurrentThreadId(),
        thread_desc[0] ? thread_desc : "<none>",
        render_phase ? render_phase : "<null>",
        dispatch_stage ? dispatch_stage : "<null>",
        aida_tracer::message_name(aida_tracer::crash_dispatch_msg()),
        aida_tracer::crash_dispatch_msg(),
        wndproc_stage ? wndproc_stage : "<null>",
        aida_tracer::message_name(aida_tracer::crash_wndproc_msg()),
        aida_tracer::crash_wndproc_msg(),
        queue_snapshot);
}

static void aida_write_first_chance_crash_log(EXCEPTION_POINTERS* ep)
{
    static std::atomic<bool> written{false};
    if (!ep || !ep->ExceptionRecord || !aida_is_fatal_exception_code(ep->ExceptionRecord->ExceptionCode))
        return;
    bool expected = false;
    if (!written.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    __try {
        aida_write_minimal_fatal_exception_line(ep, "minimal");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    CONTEXT* ctx = ep->ContextRecord;
    HMODULE exe_base = GetModuleHandleA(nullptr);
    uintptr_t exe_addr = reinterpret_cast<uintptr_t>(exe_base);
    uintptr_t rip = ctx ? static_cast<uintptr_t>(ctx->Rip) : 0;
    uintptr_t rsp = ctx ? static_cast<uintptr_t>(ctx->Rsp) : 0;
    uintptr_t rbp = ctx ? static_cast<uintptr_t>(ctx->Rbp) : 0;
    uintptr_t crash_addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    uintptr_t rip_offset = exe_addr && rip >= exe_addr ? rip - exe_addr : 0;
    uintptr_t addr_offset = exe_addr && crash_addr >= exe_addr ? crash_addr - exe_addr : 0;
    unsigned long param_count = static_cast<unsigned long>(ep->ExceptionRecord->NumberParameters);
    unsigned long long p0 = ep->ExceptionRecord->NumberParameters > 0
        ? static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[0])
        : 0ULL;
    unsigned long long p1 = ep->ExceptionRecord->NumberParameters > 1
        ? static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1])
        : 0ULL;

    static char tracer_snapshot[2600] = {};
    static char shutdown_snapshot[4200] = {};
    static char stack_modules[2200] = {};
    __try {
        aida_tracer::format_tracer_crash_snapshot(tracer_snapshot, sizeof(tracer_snapshot));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(tracer_snapshot, sizeof(tracer_snapshot), _TRUNCATE, "<tracer_snapshot_exception=0x%08X>", GetExceptionCode());
    }
    __try {
        format_shutdown_crash_snapshot(shutdown_snapshot, sizeof(shutdown_snapshot));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(shutdown_snapshot, sizeof(shutdown_snapshot), _TRUNCATE, "<shutdown_snapshot_exception=0x%08X>", GetExceptionCode());
    }
    __try {
        format_context_stack_modules(ctx, stack_modules, sizeof(stack_modules));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(stack_modules, sizeof(stack_modules), _TRUNCATE, "<stack_modules_exception=0x%08X>", GetExceptionCode());
    }
    static char buf[8192] = {};
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "FIRST_CHANCE_FATAL_CONTEXT code=0x%08X addr=0x%016llX addr_off_exe=0x%llX rip=0x%016llX rip_off_exe=0x%llX rsp=0x%016llX rbp=0x%016llX tid=%lu flags=0x%08X params=%lu p0=0x%016llX p1=0x%016llX last_error=%lu stack_modules={%s} shutdown={%s} tracer={%s}",
        ep->ExceptionRecord->ExceptionCode,
        static_cast<unsigned long long>(crash_addr),
        static_cast<unsigned long long>(addr_offset),
        static_cast<unsigned long long>(rip),
        static_cast<unsigned long long>(rip_offset),
        static_cast<unsigned long long>(rsp),
        static_cast<unsigned long long>(rbp),
        GetCurrentThreadId(),
        ep->ExceptionRecord->ExceptionFlags,
        param_count,
        p0,
        p1,
        GetLastError(),
        stack_modules,
        shutdown_snapshot,
        tracer_snapshot);
    aida_append_direct_fatal_line(buf);
}

static thread_local bool g_veh_in_handler = false;

static LONG CALLBACK aida_diagnostic_veh(EXCEPTION_POINTERS* ep)
{
    if (g_veh_in_handler)
        return EXCEPTION_CONTINUE_SEARCH;
    g_veh_in_handler = true;
    struct veh_reentry_reset_t { ~veh_reentry_reset_t() { g_veh_in_handler = false; } } veh_reentry_reset;
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x40010006u || code == 0x4001000Au || code == DBG_PRINTEXCEPTION_C ||
        code == DBG_PRINTEXCEPTION_WIDE_C ||
        code == 0x406D1388u ||
        code == 0xE06D7363u ||
        code == 0x06D007E0u ||
        code == STATUS_GUARD_PAGE_VIOLATION ||
        code == STATUS_SINGLE_STEP ||
        code == EXCEPTION_BREAKPOINT)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (aida::diagnostic_exception_scope::active())
    {
        unsigned long long p0 = ep->ExceptionRecord->NumberParameters > 0
            ? static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[0])
            : 0ULL;
        unsigned long long p1 = ep->ExceptionRecord->NumberParameters > 1
            ? static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1])
            : 0ULL;
        diag::log_tagged_critical_fmt("veh",
            "scoped_first_chance scope=%s code=0x%08X addr=0x%016llX tid=%lu flags=0x%08X params=%lu p0=0x%016llX p1=0x%016llX",
            aida::diagnostic_exception_scope::label(),
            code,
            (unsigned long long)reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress),
            GetCurrentThreadId(),
            ep->ExceptionRecord->ExceptionFlags,
            (unsigned long)ep->ExceptionRecord->NumberParameters,
            p0,
            p1);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (aida_is_fatal_exception_code(code))
    {
        aida_write_first_chance_crash_log(ep);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    aida::diagnostics::crash::emit_crash_breadcrumb(code, ep->ExceptionRecord->ExceptionAddress, "aida_diagnostic_veh");
    if (!ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE crash_mod = nullptr;
    char crash_mod_name[MAX_PATH] = "<unknown>";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(ep->ExceptionRecord->ExceptionAddress), &crash_mod);
    if (crash_mod) GetModuleFileNameA(crash_mod, crash_mod_name, MAX_PATH);
    HMODULE exe_base = GetModuleHandleA(nullptr);
    uintptr_t rip_off_exe = ep->ContextRecord->Rip - reinterpret_cast<uintptr_t>(exe_base);
    uintptr_t addr_off_mod = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress)
        - reinterpret_cast<uintptr_t>(crash_mod);
    char test_all_snapshot[1200] = {};
    test_all_features::format_debug_snapshot(test_all_snapshot, sizeof(test_all_snapshot));
    diag::log_tagged_critical_fmt("veh",
        "code=0x%08X addr=0x%016llX rip=0x%016llX rip_off_exe=0x%llX "
        "mod=%s mod_off=0x%llX tid=%lu params=%lu p0=0x%016llX p1=0x%016llX test_all={%s}",
        code,
        (unsigned long long)reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress),
        (unsigned long long)ep->ContextRecord->Rip,
        (unsigned long long)rip_off_exe,
        crash_mod_name, (unsigned long long)addr_off_mod,
        GetCurrentThreadId(),
        (unsigned long)ep->ExceptionRecord->NumberParameters,
        (unsigned long long)(ep->ExceptionRecord->NumberParameters > 0
            ? ep->ExceptionRecord->ExceptionInformation[0] : 0ULL),
        (unsigned long long)(ep->ExceptionRecord->NumberParameters > 1
            ? ep->ExceptionRecord->ExceptionInformation[1] : 0ULL),
        test_all_snapshot);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void aida_run_crash_snapshot_providers()
{
    const std::size_t provider_count = g_provider_count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < provider_count; ++i) {
        const snapshot_provider_slot_t& slot = g_provider_slots[i];
        if (!slot.provider)
            continue;
        char provider_buf[4096] = {};
        __try {
            slot.provider(provider_buf, sizeof(provider_buf));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            _snprintf_s(provider_buf, sizeof(provider_buf), _TRUNCATE, "<provider_exception=0x%08X>", GetExceptionCode());
        }
        diag::log_tagged_critical_fmt("diag",
            "CRASH-SNAPSHOT-PROVIDER name=%s snapshot={%.3800s}",
            slot.name[0] ? slot.name : "<unnamed>",
            provider_buf[0] ? provider_buf : "<empty>");
    }
}

static LONG WINAPI aida_unhandled_exception_filter(EXCEPTION_POINTERS* ep)
{
    if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == STATUS_SINGLE_STEP)
    {
        HMODULE single_step_mod = nullptr;
        char single_step_module[MAX_PATH] = "<unknown>";
        uintptr_t single_step_addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        if (ep->ExceptionRecord->ExceptionAddress &&
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(ep->ExceptionRecord->ExceptionAddress), &single_step_mod) &&
            single_step_mod)
        {
            GetModuleFileNameA(single_step_mod, single_step_module, MAX_PATH);
        }
        const uintptr_t single_step_module_base = reinterpret_cast<uintptr_t>(single_step_mod);
        const uintptr_t single_step_module_offset = single_step_module_base && single_step_addr >= single_step_module_base
            ? single_step_addr - single_step_module_base
            : 0;
        const char* early_phase = aida_early_startup::g_phase.load(std::memory_order_acquire);
        const char* render_phase = aida_tracer::crash_render_phase_name();
        char single_step_buf[1024] = {};
        _snprintf_s(single_step_buf, sizeof(single_step_buf), _TRUNCATE,
            "single_step_unconsumed code=0x%08X pid=%lu tid=%lu addr=0x%016llX module=%s module_offset=0x%llX phase=%s render_phase=%s note=not_consumed_by_earlier_handlers",
            ep->ExceptionRecord->ExceptionCode,
            GetCurrentProcessId(),
            GetCurrentThreadId(),
            static_cast<unsigned long long>(single_step_addr),
            single_step_module,
            static_cast<unsigned long long>(single_step_module_offset),
            early_phase ? early_phase : "<unknown>",
            render_phase ? render_phase : "<unknown>");
        diag::log_tagged("main", single_step_buf);
        diag::write_crash_log(single_step_buf, false);
        diag::log_tagged_critical("exception", single_step_buf);
    }

    char buf[16384];
    HMODULE crash_mod = nullptr;
    char crash_mod_name[MAX_PATH] = "<unknown>";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(ep->ExceptionRecord->ExceptionAddress), &crash_mod);
    if (crash_mod)
        GetModuleFileNameA(crash_mod, crash_mod_name, MAX_PATH);

    HMODULE exe_base = GetModuleHandleA(nullptr);
    uintptr_t rip_offset = ep->ContextRecord->Rip - reinterpret_cast<uintptr_t>(exe_base);
    uintptr_t addr_offset = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress) - reinterpret_cast<uintptr_t>(crash_mod);
    char test_all_snapshot[1200] = {};
    test_all_features::format_debug_snapshot(test_all_snapshot, sizeof(test_all_snapshot));
    char tracer_snapshot[2600] = {};
    aida_tracer::format_tracer_crash_snapshot(tracer_snapshot, sizeof(tracer_snapshot));
    char shutdown_snapshot[4200] = {};
    char stack_module_buf[2200] = {};
    format_shutdown_crash_snapshot(shutdown_snapshot, sizeof(shutdown_snapshot));
    format_context_stack_modules(ep->ContextRecord, stack_module_buf, sizeof(stack_module_buf));

    char stack_buf[512] = {};
    {
        const uintptr_t* rsp_ptr = reinterpret_cast<const uintptr_t*>(ep->ContextRecord->Rsp);
        int off = 0;
        for (int i = 0; i < 12 && off < static_cast<int>(sizeof(stack_buf) - 32); ++i) {
            uintptr_t v = 0;
            if (!safe_read_qword(rsp_ptr + i, v)) break;
            off += _snprintf_s(stack_buf + off, sizeof(stack_buf) - off, _TRUNCATE,
                "%s[%02d]=%016llX", (i == 0 ? "" : " "), i * 8,
                static_cast<unsigned long long>(v));
        }
    }

    snprintf(buf, sizeof(buf),
        "EXCEPTION: code=0x%08X addr=0x%016llX tid=%lu\n"
        "CrashModule=%s ModuleOffset=0x%llX\n"
        "ExeBase=0x%p RipOffsetFromExe=0x%llX\n"
        "Flags=0x%08X NumParams=%lu\n"
        "Info[0]=0x%016llX Info[1]=0x%016llX\n"
        "Rax=%016llX Rcx=%016llX Rdx=%016llX Rbx=%016llX\n"
        "Rsp=%016llX Rbp=%016llX Rsi=%016llX Rdi=%016llX\n"
        "R8=%016llX R9=%016llX R10=%016llX R11=%016llX\n"
        "R12=%016llX R13=%016llX R14=%016llX R15=%016llX\n"
        "Rip=%016llX\n"
        "EFlags=%08lX Dr6=%016llX Dr7=%016llX\n"
        "Stack: %s\n"
        "StackModules=%s\n"
        "ShutdownSnapshot=%s\n"
        "TestAllSnapshot=%s\n"
        "TracerSnapshot=%s\n"
        "LastError=%lu\n",
        ep->ExceptionRecord->ExceptionCode,
        reinterpret_cast<unsigned long long>(ep->ExceptionRecord->ExceptionAddress),
        GetCurrentThreadId(),
        crash_mod_name,
        static_cast<unsigned long long>(addr_offset),
        exe_base,
        static_cast<unsigned long long>(rip_offset),
        ep->ExceptionRecord->ExceptionFlags,
        ep->ExceptionRecord->NumberParameters,
        ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 0ULL,
        ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0ULL,
        ep->ContextRecord->Rax, ep->ContextRecord->Rcx,
        ep->ContextRecord->Rdx, ep->ContextRecord->Rbx,
        ep->ContextRecord->Rsp, ep->ContextRecord->Rbp,
        ep->ContextRecord->Rsi, ep->ContextRecord->Rdi,
        ep->ContextRecord->R8,  ep->ContextRecord->R9,
        ep->ContextRecord->R10, ep->ContextRecord->R11,
        ep->ContextRecord->R12, ep->ContextRecord->R13,
        ep->ContextRecord->R14, ep->ContextRecord->R15,
        ep->ContextRecord->Rip,
        static_cast<unsigned long>(ep->ContextRecord->EFlags),
        static_cast<unsigned long long>(ep->ContextRecord->Dr6),
        static_cast<unsigned long long>(ep->ContextRecord->Dr7),
        stack_buf,
        stack_module_buf,
        shutdown_snapshot,
        test_all_snapshot,
        tracer_snapshot,
        GetLastError());

    diag::log_tagged("main", buf);
    diag::write_crash_log(buf, false);

    {
        aida::diagnostics::crash::crash_context_t ctx;
        ctx.exception_code = ep->ExceptionRecord->ExceptionCode;
        ctx.exception_flags = ep->ExceptionRecord->ExceptionFlags;
        ctx.exception_address = ep->ExceptionRecord->ExceptionAddress;
        ctx.exception_record_count = ep->ExceptionRecord->NumberParameters;
        ctx.crash_boundary_name = "unhandled_exception_filter";
        ctx.current_tid = GetCurrentThreadId();
        ctx.ui_owner_tid = aida::ui_thread::owner_tid();
        ctx.last_render_phase = aida_tracer::crash_render_phase_name();
        ctx.last_render_tick_ms = aida_tracer::crash_render_last_tick_ms();
        const uint64_t crash_now_ms = static_cast<uint64_t>(GetTickCount64());
        ctx.render_heartbeat_age_ms = (ctx.last_render_tick_ms > 0 && crash_now_ms >= ctx.last_render_tick_ms)
            ? (crash_now_ms - ctx.last_render_tick_ms) : 0;
        ctx.last_wndproc_stage = aida_tracer::crash_wndproc_stage();
        ctx.last_dispatch_stage = aida_tracer::crash_dispatch_stage();
        ctx.last_message_pump_phase = aida_tracer::crash_render_phase_name();
        ctx.last_input_event_ms = (g_last_input_event_tick_ms != 0 && crash_now_ms >= g_last_input_event_tick_ms)
            ? (crash_now_ms - g_last_input_event_tick_ms) : 0;
        char testlab_phase_buf[200] = {};
        char testlab_step_buf[260] = {};
        uint64_t testlab_step_start = 0;
        test_all_features::current_phase_and_step(testlab_phase_buf, sizeof(testlab_phase_buf),
            testlab_step_buf, sizeof(testlab_step_buf), &testlab_step_start);
        ctx.testlab_phase = testlab_phase_buf;
        ctx.testlab_step = testlab_step_buf;
        ctx.testlab_step_start_ms = testlab_step_start;
        ctx.driver_watchdog_ms = driver_bridge::driver_watchdog_age_ms();
        char thread_classes_buf[320] = {};
        {
            const auto exec_snap = aida::infra::executor::active_snapshot();
            _snprintf_s(thread_classes_buf, sizeof(thread_classes_buf), _TRUNCATE,
                "general=%u service=%u critical=%u ui_dispatch=%u external=%u long_running=%u security=%u feature=%u diagnostics=%u total=%u oldest_ms=%llu",
                static_cast<unsigned>(exec_snap.active_per_domain[0]),
                static_cast<unsigned>(exec_snap.active_per_domain[1]),
                static_cast<unsigned>(exec_snap.active_per_domain[2]),
                static_cast<unsigned>(exec_snap.active_per_domain[3]),
                static_cast<unsigned>(exec_snap.active_per_domain[4]),
                static_cast<unsigned>(exec_snap.active_per_domain[5]),
                static_cast<unsigned>(exec_snap.active_per_domain[6]),
                static_cast<unsigned>(exec_snap.active_per_domain[7]),
                static_cast<unsigned>(exec_snap.active_per_domain[8]),
                static_cast<unsigned>(exec_snap.total_active),
                static_cast<unsigned long long>(exec_snap.oldest_active_ms));
        }
        ctx.thread_runtime_active_classes = thread_classes_buf;
        char camoufox_longop_buf[64] = {};
        mcp_standalone::bounded_diag_snapshot_t bdiag = mcp_standalone::bounded_diagnostic_snapshot();
        _snprintf_s(camoufox_longop_buf, sizeof(camoufox_longop_buf), _TRUNCATE,
            "active=%zu", bdiag.camoufox_longop_active);
        ctx.camoufox_longop = camoufox_longop_buf;
        char mcp_snap[1400] = {};
        char queue_snap[3000] = {};
        char ui_dispatch_snap[1400] = {};
        mcp_standalone::format_runtime_diagnostic_snapshot(mcp_snap, sizeof(mcp_snap));
        format_taskflow_runtime_hung_snapshot(queue_snap, sizeof(queue_snap));
        aida::ui_thread::format_snapshot(ui_dispatch_snap, sizeof(ui_dispatch_snap));
        ctx.mcp_snapshot = mcp_snap;
        ctx.queue_snapshot = queue_snap;
        ctx.ui_dispatch_snapshot = ui_dispatch_snap;
        ctx.capacity_snapshot = bdiag.capacity_snapshot;
        ctx.lease_registry_snapshot = bdiag.lease_registry_snapshot;
        ctx.downstream_snapshot = bdiag.downstream_snapshot;
        aida::diagnostics::crash::log_crash_snapshot(ctx);
    }

    aida_run_crash_snapshot_providers();

    return EXCEPTION_CONTINUE_SEARCH;
}

PVOID install_diagnostic_veh()
{
    return AddVectoredExceptionHandler(1, aida_diagnostic_veh);
}

void install_unhandled_exception_filter()
{
    SetUnhandledExceptionFilter(aida_unhandled_exception_filter);
}

}
