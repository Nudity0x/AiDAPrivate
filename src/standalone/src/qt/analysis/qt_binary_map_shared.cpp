#include "qt/analysis/qt_binary_map_shared.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>

#include "helpers/diag_log.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/editor/hex_view.hpp"
#include "core/infra/event_bus.hpp"
#include "core/infra/executor.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/runtime/standalone_driver_identity.hpp"
#include "core/ui/task_center.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/dialogs.hpp"

namespace aida::qt::analysis {

std::string bm_to_lower_copy(const std::string& s) {
    std::string out;
    out.resize(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        out[i] = static_cast<char>(std::tolower(c));
    }
    return out;
}

bool bm_filter_matches(const std::string& filter_lower, const std::string& text) {
    if (filter_lower.empty()) return true;
    std::string lower = bm_to_lower_copy(text);
    return lower.find(filter_lower) != std::string::npos;
}

std::string bm_format_size_human(std::uint64_t bytes) {
    char buf[48];
    if (bytes >= (1ull << 30)) {
        const double v = static_cast<double>(bytes) / static_cast<double>(1ull << 30);
        std::snprintf(buf, sizeof(buf), "%.2f GiB", v);
    } else if (bytes >= (1ull << 20)) {
        const double v = static_cast<double>(bytes) / static_cast<double>(1ull << 20);
        std::snprintf(buf, sizeof(buf), "%.2f MiB", v);
    } else if (bytes >= (1ull << 10)) {
        const double v = static_cast<double>(bytes) / static_cast<double>(1ull << 10);
        std::snprintf(buf, sizeof(buf), "%.2f KiB", v);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return std::string(buf);
}

std::string bm_format_protect_word(std::uint32_t protect) {
    if (protect == 0) return "---";
    std::string out;
    const std::uint32_t base = protect & 0xFF;
    switch (base) {
        case 0x01: out = "----"; break;
        case 0x02: out = "R---"; break;
        case 0x04: out = "RW--"; break;
        case 0x08: out = "RWC-"; break;
        case 0x10: out = "--X-"; break;
        case 0x20: out = "R-X-"; break;
        case 0x40: out = "RWX-"; break;
        case 0x80: out = "RWXC"; break;
        default: {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%02X", base);
            out = buf;
            break;
        }
    }
    if (protect & 0x100) out += " G";
    if (protect & 0x200) out += " NC";
    if (protect & 0x400) out += " WC";
    return out;
}

std::string bm_format_state_word(std::uint32_t state) {
    if (state == 0x1000)  return "COMMIT";
    if (state == 0x2000)  return "RESERVE";
    if (state == 0x10000) return "FREE";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", state);
    return std::string(buf);
}

std::string bm_format_type_word(std::uint32_t type) {
    if (type == 0x1000000) return "IMAGE";
    if (type == 0x20000)   return "PRIVATE";
    if (type == 0x40000)   return "MAPPED";
    if (type == 0) return "";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", type);
    return std::string(buf);
}

std::string bm_region_kind_label(const qt_binary_map_live_region_t& r) {
    if (r.is_guard) return "GUARD";
    if (r.is_noaccess) return "NOACCESS";
    if (r.is_stack) return "STACK";
    if (r.is_heap) return "HEAP";
    if (r.is_image) return "IMAGE";
    if (r.is_mapped) return "MAPPED";
    if (r.is_private && r.is_committed) return "PRIVATE";
    if (r.is_reserved) return "RESERVED";
    return "FREE";
}

std::string bm_section_perm_string(const aida::binary_map::map_section_t& s) {
    std::string out;
    out += s.readable ? 'R' : '-';
    out += s.writable ? 'W' : '-';
    out += s.executable ? 'X' : '-';
    return out;
}

float bm_section_entropy_normalized(const aida::binary_map::map_section_t& s) {
    if (s.sampled_bytes == 0) return 0.f;
    float e = s.entropy;
    if (e < 0.f) e = 0.f;
    if (e > 1.f) e = 1.f;
    return e;
}

std::size_t bm_hex_request_size(std::uint64_t size) {
    constexpr std::uint64_t maximum = 1ULL * 1024ULL * 1024ULL;
    return static_cast<std::size_t>((std::min)(size, maximum));
}

std::string bm_region_to_json(const qt_binary_map_live_region_t& r) {
    std::string out = "{";
    char buf[256];
    std::snprintf(buf, sizeof(buf), "\"base\":\"0x%016llX\",",
        static_cast<unsigned long long>(r.base)); out += buf;
    std::snprintf(buf, sizeof(buf), "\"size\":%llu,",
        static_cast<unsigned long long>(r.size)); out += buf;
    std::snprintf(buf, sizeof(buf), "\"protect\":\"0x%X\",", r.protect); out += buf;
    std::snprintf(buf, sizeof(buf), "\"state\":\"0x%X\",", r.state); out += buf;
    std::snprintf(buf, sizeof(buf), "\"type\":\"0x%X\",", r.type); out += buf;
    out += "\"kind\":\"" + bm_region_kind_label(r) + "\",";
    out += "\"module\":\"" + r.module_name + "\",";
    out += "\"info\":\"" + r.info + "\"";
    out += "}";
    return out;
}

std::string bm_export_live_snapshot_json(const qt_binary_map_live_snapshot_t& snap) {
    std::string out;
    char hdr[512];
    std::snprintf(hdr, sizeof(hdr),
        "{\"pid\":%u,\"process\":\"%s\",\"committed\":%llu,\"reserved\":%llu,"
        "\"rwx_count\":%u,\"region_count\":%zu,\"process_creation_time_100ns\":%llu,"
        "\"module_base\":%llu,\"module_size\":%llu,\"workspace_generation\":%llu,"
        "\"refresh_serial\":%llu,\"regions\":[",
        static_cast<unsigned>(snap.pid),
        snap.process_name.c_str(),
        static_cast<unsigned long long>(snap.total_committed),
        static_cast<unsigned long long>(snap.total_reserved),
        static_cast<unsigned>(snap.rwx_count),
        snap.regions.size(),
        static_cast<unsigned long long>(snap.target_binding.process.creation_time_100ns),
        static_cast<unsigned long long>(snap.target_binding.module
            ? snap.target_binding.module->base : 0),
        static_cast<unsigned long long>(snap.target_binding.module
            ? snap.target_binding.module->size : 0),
        static_cast<unsigned long long>(snap.target_binding.workspace_generation),
        static_cast<unsigned long long>(snap.target_binding.refresh_serial));
    out = hdr;
    for (std::size_t i = 0; i < snap.regions.size(); ++i) {
        if (i) out += ",";
        out += bm_region_to_json(snap.regions[i]);
    }
    out += "]}";
    return out;
}

std::string bm_format_function_summary(const aida::binary_map::map_function_t& f) {
    std::string callees;
    for (std::size_t i = 0; i < f.top_callees.size() && i < 5; ++i) {
        if (i > 0) callees += ", ";
        callees += f.top_callees[i];
    }
    if (callees.empty()) callees = "(none)";
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "%s @ 0x%llX (xrefs=%d, callees: %s)",
        f.name.c_str(), static_cast<unsigned long long>(f.va), f.xref_count,
        callees.c_str());
    return std::string(buf);
}

std::string bm_make_function_chat_payload(const aida::binary_map::map_function_t& f) {
    std::string out = "Binary map function summary:\n";
    out += bm_format_function_summary(f);
    if (!f.section_name.empty()) {
        out += "\nSection: ";
        out += f.section_name;
    }
    if (f.pinned) out += "\n(pinned)";
    out += "\n";
    return out;
}

std::string bm_make_global_chat_payload(const aida::binary_map::map_global_t& g) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "Binary map global: %s @ 0x%llX (xrefs=%d, %s%s)\n",
        g.name.c_str(), static_cast<unsigned long long>(g.va), g.xref_count,
        g.writable ? "rw" : "ro",
        g.section_name.empty() ? "" : (std::string(", ") + g.section_name).c_str());
    return std::string(buf);
}

std::string bm_make_region_chat_payload(const qt_binary_map_live_region_t& r) {
    std::string out = "Live memory region:\n";
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "  Base 0x%016llX  Size %s\n"
        "  Kind %s  State %s  Type %s  Protect %s\n",
        static_cast<unsigned long long>(r.base),
        bm_format_size_human(r.size).c_str(),
        bm_region_kind_label(r).c_str(),
        bm_format_state_word(r.state).c_str(),
        bm_format_type_word(r.type).c_str(),
        bm_format_protect_word(r.protect).c_str());
    out += buf;
    if (!r.module_name.empty()) {
        std::snprintf(buf, sizeof(buf), "  Module %s\n", r.module_name.c_str());
        out += buf;
    }
    if (!r.info.empty()) {
        std::snprintf(buf, sizeof(buf), "  Info %s\n", r.info.c_str());
        out += buf;
    }
    return out;
}

qt_binary_map_live_target_binding_t bm_capture_workspace_binding(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    std::uint64_t generation, std::uint64_t refresh_serial) {
    qt_binary_map_live_target_binding_t binding;
    if (!workspace) return binding;
    if (const auto process = workspace->identity().process()) binding.process = *process;
    binding.module = workspace->identity().module();
    binding.workspace_generation = generation;
    binding.refresh_serial = refresh_serial;
    return binding;
}

bool bm_binding_matches_workspace(
    const qt_binary_map_live_target_binding_t& binding,
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace) {
    return binding.valid() && workspace && !workspace->closing() && !workspace->closed() &&
        workspace->generation() == binding.workspace_generation &&
        workspace->identity().process() &&
        *workspace->identity().process() == binding.process &&
        workspace->identity().module() == binding.module;
}

namespace {

driver_bridge::identity::live_target_identity_t bm_driver_identity(
    const qt_binary_map_live_target_binding_t& binding) {
    driver_bridge::identity::live_target_identity_t identity;
    identity.process.pid = binding.process.pid;
    identity.process.creation_time_100ns = binding.process.creation_time_100ns;
    identity.process.normalized_process_path = binding.process.normalized_process_path;
    if (binding.module) {
        identity.module.base = binding.module->base;
        identity.module.size = binding.module->size;
        identity.module.normalized_name = binding.module->normalized_name;
        identity.module.normalized_path = binding.module->normalized_path;
    }
    return identity;
}

void bm_raise_uncertain_mutation_diagnostic(const std::string& task_id,
    const std::string& target, const std::string& details) {
    aida::ui::task_center::diagnostic_registration_t diagnostic;
    diagnostic.id = "diagnostic." + task_id + ".uncertain";
    diagnostic.task_id = task_id;
    diagnostic.owner = "Binary Map";
    diagnostic.target = target;
    diagnostic.summary = "Live protection state is uncertain";
    diagnostic.details = details;
    diagnostic.severity = aida::ui::task_center::diagnostic_severity_t::security;
    diagnostic.callbacks.focus = [] {
        QtAnalysisBridge::instance().openView("view.analysis.binary_map");
    };
    static_cast<void>(aida::ui::task_center::raise_diagnostic(std::move(diagnostic)));
}

bool bm_atomic_write_exact(const std::string& destination,
    const void* data, std::size_t size, std::string& error) {
    if (destination.empty() || (size != 0 && data == nullptr)) {
        error = "The export destination or payload is invalid";
        return false;
    }
    const std::filesystem::path final_path = std::filesystem::u8path(destination);
    static std::atomic<std::uint64_t> sequence{1};
    const std::filesystem::path temporary(final_path.wstring() + L".tmp." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetCurrentThreadId()) + L"." +
        std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)));
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Creating the export temporary file failed with Win32 error " +
            std::to_string(GetLastError());
        return false;
    }
    bool succeeded = true;
    std::size_t offset = 0;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>((std::min)(size - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, chunk, &written, nullptr)) {
            error = "Writing the export temporary file failed with Win32 error " +
                std::to_string(GetLastError());
            succeeded = false;
            break;
        }
        if (written != chunk) {
            error = "Writing the export temporary file completed with a short write";
            succeeded = false;
            break;
        }
        offset += written;
    }
    if (succeeded && !FlushFileBuffers(file)) {
        error = "Flushing the export temporary file failed with Win32 error " +
            std::to_string(GetLastError());
        succeeded = false;
    }
    LARGE_INTEGER observed_size{};
    if (succeeded && (!GetFileSizeEx(file, &observed_size) ||
        observed_size.QuadPart < 0 ||
        static_cast<std::uint64_t>(observed_size.QuadPart) != size)) {
        error = "The export temporary file size did not match the requested payload";
        succeeded = false;
    }
    if (!CloseHandle(file) && succeeded) {
        error = "Closing the export temporary file failed with Win32 error " +
            std::to_string(GetLastError());
        succeeded = false;
    }
    if (succeeded && !MoveFileExW(temporary.c_str(), final_path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Replacing the export destination failed with Win32 error " +
            std::to_string(GetLastError());
        succeeded = false;
    }
    if (!succeeded)
        DeleteFileW(temporary.c_str());
    return succeeded;
}

}

bool bm_validate_live_binding(
    const qt_binary_map_live_target_binding_t& binding,
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    std::string& error) {
    if (!bm_binding_matches_workspace(binding, workspace)) {
        error = "The Binary Map workspace generation or immutable target identity changed";
        return false;
    }
    const auto validation = driver_bridge::identity::validate_live_target_identity(
        bm_driver_identity(binding));
    if (validation.matches) return true;
    error = std::string(driver_bridge::identity::staleness_code(validation.staleness)) +
        ": " + validation.detail;
    return false;
}

bool bm_live_available(const QtBinaryMapViewState& state) {
    const auto workspace = state.workspace.lock();
    const auto process = workspace ? workspace->identity().process()
        : std::optional<aida::analysis::process_identity_t>{};
    return driver_bridge::is_loaded()
        && workspace
        && workspace->target_kind() == aida::analysis::target_kind_t::live_snapshot
        && process && process->pid != 0 && process->creation_time_100ns != 0
        && workspace->identity().module() &&
            workspace->identity().module()->base != 0 &&
            workspace->identity().module()->size != 0
        && driver_bridge::can_read_memory();
}

bool bm_static_available(const QtBinaryMapViewState& state) {
    const auto workspace = state.workspace.lock();
    return workspace &&
        workspace->target_kind() == aida::analysis::target_kind_t::static_file &&
        workspace->image() && !workspace->closing() && !workspace->closed();
}

qt_binary_map_active_mode_t bm_resolve_active_mode(
    const QtBinaryMapViewState& state, qt_binary_map_display_mode_t pref) {
    const bool live = bm_live_available(state);
    const bool stat = bm_static_available(state);
    if (pref == qt_binary_map_display_mode_t::live_only)
        return live ? qt_binary_map_active_mode_t::live_process
            : qt_binary_map_active_mode_t::none;
    if (pref == qt_binary_map_display_mode_t::static_only)
        return stat ? qt_binary_map_active_mode_t::pe_static
            : qt_binary_map_active_mode_t::none;
    if (live && stat) return qt_binary_map_active_mode_t::merged;
    if (live)         return qt_binary_map_active_mode_t::live_process;
    if (stat)         return qt_binary_map_active_mode_t::pe_static;
    return qt_binary_map_active_mode_t::none;
}

namespace {

void bm_classify_region(qt_binary_map_live_region_t& r,
    const std::vector<driver_bridge::module_info_t>& modules,
    const std::vector<driver_bridge::thread_info_t>& threads,
    std::uint64_t process_heap) {
    r.is_committed = (r.state == 0x1000);
    r.is_reserved  = (r.state == 0x2000);
    r.is_guard     = (r.protect & 0x100) != 0;
    r.is_noaccess  = ((r.protect & 0xFF) == 0x01);
    r.is_image     = (r.type == 0x1000000);
    r.is_mapped    = (r.type == 0x40000);
    r.is_private   = (r.type == 0x20000);
    for (const auto& m : modules) {
        if (r.base >= m.base && r.base < m.base + static_cast<std::uint64_t>(m.size)) {
            r.module_name = m.name;
            r.module_path = m.path;
            break;
        }
    }
    for (const auto& th : threads) {
        if (th.rip == 0) continue;
        if (r.base <= th.rip && th.rip < r.base + r.size && r.is_private &&
            r.is_committed) {
            r.is_stack = true;
            r.owner_tid = th.tid;
            break;
        }
    }
    if (!r.is_stack && r.is_private && r.is_committed && process_heap != 0) {
        if (r.base == process_heap) r.is_heap = true;
    }
}

}

void bm_perform_refresh(const std::shared_ptr<QtBinaryMapViewState>& state) {
    if (!state) return;
    auto& s = *state;
    const auto workspace = s.workspace.lock();
    if (!workspace ||
        workspace->target_kind() != aida::analysis::target_kind_t::static_file) {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.last_error = "Static binary map requires an explicit static workspace.";
        return;
    }
    if (s.refreshing.exchange(true)) {
        diag::log_tagged_fmt("binary_map", "refresh SKIPPED already_in_flight");
        return;
    }
    aida::binary_map::map_options_t opts_copy;
    {
        std::lock_guard<std::mutex> g(s.mutex);
        opts_copy = s.opts;
    }
    diag::log_tagged_fmt("binary_map",
        "refresh START max_functions=%d max_globals=%d max_callees=%d imp=%d exp=%d",
        opts_copy.max_functions, opts_copy.max_globals,
        opts_copy.max_callees_per_function,
        opts_copy.include_imports ? 1 : 0, opts_copy.include_exports ? 1 : 0);
    const std::uint64_t request_generation = workspace->generation();
    const std::uint64_t refresh_serial = s.refresh_serial.fetch_add(1,
        std::memory_order_acq_rel) + 1;
    const std::string task_id = "binary_map.static_refresh." + s.binary_id + "." +
        std::to_string(refresh_serial);
    aida::ui::task_center::task_registration_t registration;
    registration.id = task_id;
    registration.source = "binary_map";
    registration.owner = "Binary Map";
    registration.owner_view = "view.analysis.binary_map";
    registration.owner_action = "Refresh static map";
    registration.target = workspace->identity().bin_name();
    registration.label = "Generate static Binary Map";
    registration.stage = "Queued workspace analysis";
    registration.affected_entity = workspace->identity().normalized_source_path();
    if (!aida::ui::task_center::register_task(std::move(registration))) {
        s.refreshing.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(s.mutex);
        s.last_error = "Task Center rejected ownership of the static Binary Map refresh.";
        return;
    }
    aida::infra::executor::submission_t sub;
    sub.owner_subsystem = "analysis";
    sub.label = "analysis.binary_map.refresh";
    sub.thread_class = "bounded_task";
    sub.domain = aida::infra::executor::domain_t::diagnostics;
    sub.priority = 3;
    sub.generation = request_generation;
    sub.body = [state, workspace, opts_copy, request_generation, task_id]() {
        auto& s = *state;
        struct refresh_guard_t {
            std::shared_ptr<QtBinaryMapViewState> state;
            ~refresh_guard_t() {
                state->refreshing.store(false, std::memory_order_release);
            }
        } refresh_guard{state};
        static_cast<void>(refresh_guard);
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::running, 0.05f,
            "Generating static workspace map"));
        try {
            const auto start_clock = std::chrono::steady_clock::now();
            aida::binary_map::clear_cache(workspace);
            aida::binary_map::map_t fresh;
            bool ok = aida::binary_map::generate(workspace, opts_copy, fresh);
            std::string err_copy;
            if (!ok) err_copy = "Workspace binary-map generation failed.";
            if (ok && workspace->generation() != request_generation) {
                ok = false;
                err_copy = "The workspace changed while the Binary Map was being generated.";
                s.refresh_requested.store(true, std::memory_order_release);
            }
            std::size_t f = 0, g_count = 0, i = 0, e = 0, sec = 0;
            std::string mod;
            if (ok) {
                f = fresh.functions.size();
                g_count = fresh.globals.size();
                i = fresh.imports.size();
                e = fresh.exports.size();
                sec = fresh.sections.size();
                mod = fresh.module_name;
            }
            if (ok) {
                auto published_map = std::make_shared<const aida::binary_map::map_t>(
                    std::move(fresh));
                auto rendered = std::make_shared<const std::string>(
                    aida::binary_map::render_text(*published_map, opts_copy));
                std::atomic_store_explicit(&s.map, std::move(published_map),
                    std::memory_order_release);
                std::atomic_store_explicit(&s.rendered_text, std::move(rendered),
                    std::memory_order_release);
            }
            {
                std::lock_guard<std::mutex> g(s.mutex);
                if (ok) {
                    s.has_map.store(true);
                    s.last_error.clear();
                } else {
                    s.last_error = err_copy;
                }
            }
            const auto end_clock = std::chrono::steady_clock::now();
            const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                end_clock - start_clock).count();
            if (ok) {
                diag::log_tagged_fmt("binary_map",
                    "refresh DONE module='%s' sections=%zu funcs=%zu globals=%zu imports=%zu exports=%zu duration_ms=%lld",
                    mod.c_str(), sec, f, g_count, i, e, static_cast<long long>(dur_ms));
            } else {
                diag::log_tagged_fmt("binary_map",
                    "refresh FAILED err='%s' duration_ms=%lld",
                    err_copy.c_str(), static_cast<long long>(dur_ms));
            }
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                ok ? aida::ui::task_center::task_state_t::completed
                   : workspace->generation() != request_generation
                    ? aida::ui::task_center::task_state_t::cancelled
                    : aida::ui::task_center::task_state_t::failed,
                1.0f, ok ? "Static map published" : "Static map not published",
                ok ? std::to_string(sec) + " sections, " + std::to_string(f) +
                    " functions" : err_copy));
        } catch (const std::exception& exception) {
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.last_error = "Static Binary Map refresh failed: " +
                    std::string(exception.what());
            }
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "Static refresh failed", exception.what()));
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.last_error =
                    "Static Binary Map refresh failed with an unknown worker error.";
            }
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "Static refresh failed", "Unknown worker error"));
        }
    };
    const auto submitted = aida::infra::executor::submit(std::move(sub));
    if (!submitted.submitted) {
        s.refreshing.store(false);
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.last_error = "Worker queue rejected the static Binary Map refresh: " +
                submitted.reject_reason;
        }
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Worker queue rejected", submitted.reject_reason));
        diag::log_tagged_fmt("binary_map", "refresh FAILED post rejected by executor");
    }
}

void bm_perform_live_refresh(const std::shared_ptr<QtBinaryMapViewState>& state) {
    if (!state) return;
    auto& s = *state;
    const auto workspace = s.workspace.lock();
    const auto process = workspace ? workspace->identity().process()
        : std::optional<aida::analysis::process_identity_t>{};
    if (!bm_live_available(s) || !process) {
        diag::log_tagged_fmt("binary_map",
            "live_refresh SKIPPED driver_loaded=%d attached_pid=%u",
            driver_bridge::is_loaded() ? 1 : 0,
            process ? static_cast<unsigned>(process->pid) : 0u);
        return;
    }
    if (s.live_refreshing.exchange(true)) {
        diag::log_tagged_fmt("binary_map", "live_refresh SKIPPED already_in_flight");
        return;
    }
    diag::log_tagged_fmt("binary_map", "live_refresh START pid=%u",
        static_cast<unsigned>(process->pid));
    const std::uint64_t request_generation = workspace->generation();
    const std::uint64_t refresh_serial = s.live_refresh_serial.fetch_add(1,
        std::memory_order_acq_rel) + 1;
    const qt_binary_map_live_target_binding_t target_binding =
        bm_capture_workspace_binding(workspace, request_generation, refresh_serial);
    if (!target_binding.valid()) {
        s.live_refreshing.store(false, std::memory_order_release);
        std::atomic_store_explicit(&s.live,
            std::shared_ptr<const qt_binary_map_live_snapshot_t>(
                std::make_shared<qt_binary_map_live_snapshot_t>()),
            std::memory_order_release);
        s.live_selected_base.store(0, std::memory_order_release);
        std::lock_guard<std::mutex> lock(s.mutex);
        s.live_last_error =
            "The live workspace has no complete process-creation and module identity.";
        return;
    }
    const std::string task_id = "binary_map.live_refresh." + s.binary_id + "." +
        std::to_string(refresh_serial);
    const auto cancellation = std::make_shared<std::atomic<bool>>(false);
    aida::ui::task_center::task_registration_t registration;
    registration.id = task_id;
    registration.source = "binary_map";
    registration.owner = "Binary Map";
    registration.owner_view = "view.analysis.binary_map";
    registration.owner_action = "Refresh live map";
    registration.target = "PID " + std::to_string(process->pid);
    registration.label = "Enumerate live Binary Map";
    registration.stage = "Queued target enumeration";
    registration.affected_entity = workspace->identity().bin_name();
    registration.cancellation_is_safe = true;
    registration.callbacks.cancel = [cancellation] {
        cancellation->store(true, std::memory_order_release);
        return true;
    };
    if (!aida::ui::task_center::register_task(std::move(registration))) {
        s.live_refreshing.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(s.mutex);
        s.live_last_error =
            "Task Center rejected ownership of the live Binary Map refresh.";
        return;
    }
    aida::infra::executor::submission_t sub;
    sub.owner_subsystem = "analysis";
    sub.label = "analysis.binary_map.live_refresh";
    sub.thread_class = "bounded_task";
    sub.domain = aida::infra::executor::domain_t::diagnostics;
    sub.priority = 3;
    sub.target_pid = process->pid;
    sub.generation = request_generation;
    sub.cancel_hook = [cancellation] {
        cancellation->store(true, std::memory_order_release);
    };
    sub.body = [state, workspace, pid = process->pid, request_generation,
        target_binding, cancellation, task_id]() {
        auto& s = *state;
        struct refresh_guard_t {
            std::shared_ptr<QtBinaryMapViewState> state;
            ~refresh_guard_t() {
                state->live_refreshing.store(false, std::memory_order_release);
            }
        } refresh_guard{state};
        static_cast<void>(refresh_guard);
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::running, 0.05f,
            "Enumerating target regions, modules, and threads"));
        try {
            const auto start_clock = std::chrono::steady_clock::now();
            auto require_identity = [&](const char* boundary) {
                if (cancellation->load(std::memory_order_acquire)) {
                    static_cast<void>(aida::ui::task_center::update_task(task_id,
                        aida::ui::task_center::task_state_t::cancelled, 1.0f,
                        "Live refresh cancelled", boundary));
                    return false;
                }
                std::string identity_error;
                if (bm_validate_live_binding(target_binding, workspace, identity_error))
                    return true;
                const std::string detail =
                    std::string("The live target changed ") + boundary + ": " +
                    identity_error;
                {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    s.live_last_error = detail;
                }
                std::atomic_store_explicit(&s.live,
                    std::shared_ptr<const qt_binary_map_live_snapshot_t>(
                        std::make_shared<qt_binary_map_live_snapshot_t>()),
                    std::memory_order_release);
                s.live_selected_base.store(0, std::memory_order_release);
                s.live_hover_index.store(-1, std::memory_order_release);
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::cancelled, 1.0f,
                    "Discarded stale live map", detail));
                return false;
            };
            if (!require_identity("before region enumeration")) return;
            auto regions_raw = driver_bridge::enumerate_memory_regions_for(pid, 8192);
            if (!require_identity("after region enumeration")) return;
            auto modules = driver_bridge::enumerate_modules_for(pid);
            if (!require_identity("after module enumeration")) return;
            auto threads = driver_bridge::enumerate_threads_for(pid);
            if (!require_identity("after thread enumeration")) return;
            if (regions_raw.empty()) {
                const std::string error =
                    "The target returned no readable memory-region enumeration.";
                {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    s.live_last_error = error;
                }
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "Live enumeration failed", error));
                return;
            }
            const std::string proc_name = workspace->identity().bin_name();
            driver_bridge::peb_info_t peb{};
            std::uint64_t process_heap = 0;
            if (driver_bridge::read_peb_for(pid, peb))
                process_heap = peb.process_heap;
            if (!require_identity("after PEB read")) return;
            qt_binary_map_live_snapshot_t snap;
            snap.modules = std::move(modules);
            snap.threads = std::move(threads);
            snap.process_heap = process_heap;
            snap.pid = pid;
            snap.process_name = proc_name;
            snap.target_binding = target_binding;
            snap.regions.reserve(regions_raw.size());
            std::uint64_t total_committed = 0;
            std::uint64_t total_reserved = 0;
            std::uint32_t rwx = 0;
            for (const auto& src : regions_raw) {
                if (cancellation->load(std::memory_order_acquire)) {
                    static_cast<void>(aida::ui::task_center::update_task(task_id,
                        aida::ui::task_center::task_state_t::cancelled, 1.0f,
                        "Live refresh cancelled", "Cancelled while classifying regions"));
                    return;
                }
                qt_binary_map_live_region_t r;
                r.base = src.base;
                r.size = src.size;
                r.state = src.state;
                r.protect = src.protect;
                r.type = src.type;
                bm_classify_region(r, snap.modules, snap.threads, snap.process_heap);
                if (r.is_committed) total_committed += r.size;
                if (r.is_reserved)  total_reserved  += r.size;
                const bool exec  = (r.protect & 0xF0) != 0;
                const std::uint32_t low = r.protect & 0xFF;
                const bool write = (low == 0x04) || (low == 0x08) || (low == 0x40) ||
                    (low == 0x80);
                if (exec && write) ++rwx;
                if (r.is_image && !r.module_name.empty()) {
                    r.section_name.clear();
                    if (const auto image = workspace->image()) {
                        for (const auto& section : image->sections()) {
                            const std::uint64_t section_start = image->image_base() +
                                section.virtual_address;
                            const std::uint64_t section_size = (std::max)(
                                static_cast<std::uint64_t>(section.virtual_size),
                                static_cast<std::uint64_t>(section.raw_size));
                            if (r.base >= section_start &&
                                r.base - section_start < section_size) {
                                r.section_name = section.name;
                                break;
                            }
                        }
                    }
                }
                if (r.is_stack) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "Thread %u stack",
                        static_cast<unsigned>(r.owner_tid));
                    r.info = buf;
                } else if (r.is_heap) {
                    r.info = "Process heap";
                } else if (r.is_image && !r.module_name.empty()) {
                    r.info = r.module_name;
                }
                snap.regions.push_back(std::move(r));
            }
            snap.total_committed = total_committed;
            snap.total_reserved  = total_reserved;
            snap.rwx_count = rwx;
            snap.generated_unix = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            const auto end_clock = std::chrono::steady_clock::now();
            snap.enum_elapsed_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    end_clock - start_clock).count());
            const bool superseded = s.live_refresh_serial.load(
                std::memory_order_acquire) != target_binding.refresh_serial;
            if (superseded || !require_identity("before publication")) {
                s.live_refresh_requested.store(true, std::memory_order_release);
                if (superseded) {
                    static_cast<void>(aida::ui::task_center::update_task(task_id,
                        aida::ui::task_center::task_state_t::cancelled, 1.0f,
                        "Discarded superseded live map",
                        "A newer live refresh owns publication"));
                }
                diag::log_tagged_fmt("binary_map",
                    "live_refresh STALE pid=%u request_generation=%llu current_generation=%llu serial=%llu",
                    static_cast<unsigned>(pid),
                    static_cast<unsigned long long>(request_generation),
                    static_cast<unsigned long long>(workspace->generation()),
                    static_cast<unsigned long long>(target_binding.refresh_serial));
                return;
            }
            auto published =
                std::make_shared<const qt_binary_map_live_snapshot_t>(std::move(snap));
            std::atomic_store_explicit(&s.live, published, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.live_last_error.clear();
            }
            s.live_last_refresh_unix.store(static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
            diag::log_tagged_fmt("binary_map",
                "live_refresh DONE pid=%u proc='%s' regions=%zu modules=%zu threads=%zu committed=%llu reserved=%llu rwx=%u elapsed_ms=%llu",
                static_cast<unsigned>(published->pid),
                published->process_name.c_str(),
                published->regions.size(), published->modules.size(),
                published->threads.size(),
                static_cast<unsigned long long>(published->total_committed),
                static_cast<unsigned long long>(published->total_reserved),
                static_cast<unsigned>(published->rwx_count),
                static_cast<unsigned long long>(published->enum_elapsed_ms));
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::completed, 1.0f,
                "Live map published", std::to_string(published->regions.size()) +
                    " regions, " + std::to_string(published->modules.size()) +
                    " modules"));
        } catch (const std::exception& exception) {
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.live_last_error = "Live Binary Map refresh failed: " +
                    std::string(exception.what());
            }
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "Live refresh failed", exception.what()));
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.live_last_error =
                    "Live Binary Map refresh failed with an unknown worker error.";
            }
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "Live refresh failed", "Unknown worker error"));
        }
    };
    const auto submitted = aida::infra::executor::submit(std::move(sub));
    if (!submitted.submitted) {
        s.live_refreshing.store(false);
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.live_last_error = "Worker queue rejected the live Binary Map refresh: " +
                submitted.reject_reason;
        }
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Worker queue rejected", submitted.reject_reason));
        diag::log_tagged_fmt("binary_map", "live_refresh FAILED post rejected by executor");
    }
}

bool bm_queue_snapshot_export(const std::shared_ptr<QtBinaryMapViewState>& state,
    const std::string& destination, std::string label,
    std::shared_ptr<const qt_binary_map_live_snapshot_t> live,
    std::shared_ptr<const std::string> text) {
    if (!state || (!live && (!text || text->empty())) || destination.empty())
        return false;
    bool expected = false;
    if (!state->export_pending.compare_exchange_strong(expected, true,
        std::memory_order_acq_rel))
        return false;
    static std::atomic<std::uint64_t> sequence{1};
    const std::string task_id = "analysis.binary_map.export." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    aida::ui::task_center::task_registration_t registration;
    registration.id = task_id;
    registration.source = "analysis.binary_map";
    registration.owner = "Binary Map";
    registration.owner_view = "view.analysis.binary_map";
    registration.owner_action = "analysis.binary_map.export";
    registration.target = destination;
    registration.label = std::move(label);
    registration.stage = "Queued for immutable serialization and atomic export";
    registration.affected_entity = destination;
    registration.callbacks.focus = [] {
        QtAnalysisBridge::instance().openView("view.analysis.binary_map");
    };
    registration.callbacks.open_log = registration.callbacks.focus;
    if (!aida::ui::task_center::register_task(std::move(registration))) {
        state->export_pending.store(false, std::memory_order_release);
        return false;
    }
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "analysis";
    submission.label = "analysis.binary_map.export";
    submission.thread_class = "bounded_file_io";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.session_id = task_id.c_str();
    submission.target_id = destination.c_str();
    submission.diagnostic_id = task_id.c_str();
    submission.ui_access_policy = "immutable_snapshots_only";
    submission.failure_policy = "typed_diagnostic";
    submission.shutdown_policy = "drain";
    submission.body = [state, task_id, destination,
        live = std::move(live), text = std::move(text)] {
        struct pending_guard_t {
            std::shared_ptr<QtBinaryMapViewState> state;
            ~pending_guard_t() {
                state->export_pending.store(false, std::memory_order_release);
            }
        } pending_guard{state};
        static_cast<void>(pending_guard);
        try {
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::running, -1.0f,
                "Serializing an immutable Binary Map snapshot"));
            std::string payload =
                live ? bm_export_live_snapshot_json(*live) : *text;
            constexpr std::size_t maximum_export_bytes = 64U * 1024U * 1024U;
            if (payload.size() > maximum_export_bytes) {
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "Binary Map export exceeded the size limit",
                    "The serialized export exceeded the 64 MiB bounded export limit",
                    "diagnostic." + task_id));
                return;
            }
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::running, -1.0f,
                "Writing a same-directory temporary file"));
            std::string error;
            if (!bm_atomic_write_exact(destination, payload.data(), payload.size(),
                    error)) {
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "Atomic Binary Map export failed", error, "diagnostic." + task_id));
                return;
            }
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::completed, 1.0f,
                "Finished", "Binary Map exported atomically to " + destination));
        } catch (const std::exception& exception) {
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "Binary Map export failed", exception.what(), "diagnostic." + task_id));
        } catch (...) {
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "Binary Map export failed", "Unknown export failure",
                "diagnostic." + task_id));
        }
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        state->export_pending.store(false, std::memory_order_release);
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Executor rejected Binary Map export", submitted.reject_reason,
            "diagnostic." + task_id));
        return false;
    }
    return true;
}

bool bm_queue_protection_change(
    const std::shared_ptr<QtBinaryMapViewState>& state,
    const disasm_view::workspace_context_t& context,
    const qt_binary_map_live_target_binding_t& target_binding, std::uint64_t address,
    std::uint64_t size, std::uint32_t expected_protect, std::uint32_t new_protect) {
    // DRIVER WRITE - verbatim reviewed-protection safety pipeline.
    if (!state || !context || !context.workspace->identity().process() ||
        !bm_binding_matches_workspace(target_binding, context.workspace) ||
        address == 0 || size == 0 ||
        address > (std::numeric_limits<std::uint64_t>::max)() - size)
        return false;
    bool expected = false;
    if (!state->change_protect_pending.compare_exchange_strong(expected, true,
        std::memory_order_acq_rel))
        return false;
    const std::uint32_t pid = context.workspace->identity().process()->pid;
    const std::uint64_t generation = context.workspace->generation();
    static std::atomic<std::uint64_t> sequence{1};
    const std::string task_id = "analysis.binary_map.protect." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    char target[160] = {};
    std::snprintf(target, sizeof(target), "PID %u 0x%llX-0x%llX",
        static_cast<unsigned>(pid), static_cast<unsigned long long>(address),
        static_cast<unsigned long long>(address + size));
    const std::string target_text(target);
    aida::ui::task_center::task_registration_t registration;
    registration.id = task_id;
    registration.source = "analysis.binary_map";
    registration.owner = "Binary Map";
    registration.owner_view = "view.analysis.binary_map";
    registration.owner_action = "analysis.binary_map.change_protection";
    registration.target = target;
    registration.label = "Change live memory protection";
    registration.stage = "Queued after reviewed confirmation";
    registration.affected_entity = target;
    registration.security_critical = true;
    registration.callbacks.focus = [] {
        QtAnalysisBridge::instance().openView("view.analysis.binary_map");
    };
    registration.callbacks.open_log = registration.callbacks.focus;
    if (!aida::ui::task_center::register_task(std::move(registration))) {
        state->change_protect_pending.store(false, std::memory_order_release);
        return false;
    }
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "analysis";
    submission.label = "analysis.binary_map.change_protection";
    submission.thread_class = "live_target_mutation";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.target_pid = pid;
    submission.generation = generation;
    submission.session_id = task_id.c_str();
    submission.target_id = target;
    submission.diagnostic_id = task_id.c_str();
    submission.ui_access_policy = "immutable_snapshots_only";
    submission.failure_policy = "typed_diagnostic";
    submission.shutdown_policy = "drain";
    submission.body = [state, context, target_binding, task_id, target_text, pid,
        address, size, expected_protect, new_protect] {
        struct pending_guard_t {
            std::shared_ptr<QtBinaryMapViewState> state;
            ~pending_guard_t() {
                state->change_protect_pending.store(false, std::memory_order_release);
            }
        } pending_guard{state};
        static_cast<void>(pending_guard);
        try {
            std::string identity_error;
            if (!bm_validate_live_binding(target_binding, context.workspace,
                    identity_error)) {
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "Target identity changed before mutation", identity_error,
                    "diagnostic." + task_id));
                return;
            }
            auto observed_protection = [&](std::uint32_t& value) {
                const auto regions =
                    driver_bridge::enumerate_memory_regions_for(pid, 8192);
                if (!bm_validate_live_binding(target_binding, context.workspace,
                        identity_error))
                    return false;
                for (const auto& region : regions) {
                    if (address >= region.base && address - region.base < region.size) {
                        value = region.protect;
                        return true;
                    }
                }
                return false;
            };
            std::uint32_t preflight_protect = 0;
            if (!observed_protection(preflight_protect) ||
                preflight_protect != expected_protect) {
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "Protection changed before mutation",
                    "The selected region no longer has the protection value reviewed in the dialog",
                    "diagnostic." + task_id));
                return;
            }
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::running, -1.0f,
                "Applying reviewed protection change"));
            std::uint32_t old_protect = 0;
            if (!driver_bridge::protect_memory_for(pid, address, size,
                new_protect, &old_protect)) {
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "Protection mutation failed",
                    "The driver rejected the reviewed protection change",
                    "diagnostic." + task_id));
                return;
            }
            if (!bm_validate_live_binding(target_binding, context.workspace,
                    identity_error)) {
                bm_raise_uncertain_mutation_diagnostic(task_id, target_text,
                    "The target identity changed after the driver accepted the mutation: " +
                    identity_error);
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "Protection state is uncertain", identity_error,
                    "diagnostic." + task_id + ".uncertain"));
                return;
            }
            std::uint32_t observed = 0;
            const bool verified = old_protect == expected_protect &&
                observed_protection(observed) && observed == new_protect;
            state->live_refresh_requested.store(true, std::memory_order_release);
            if (!verified) {
                bool restored = false;
                std::string rollback_error;
                if (old_protect != 0 &&
                    bm_validate_live_binding(target_binding, context.workspace,
                        rollback_error)) {
                    std::uint32_t ignored = 0;
                    if (driver_bridge::protect_memory_for(pid, address, size,
                        old_protect, &ignored) &&
                        bm_validate_live_binding(target_binding, context.workspace,
                            rollback_error)) {
                        std::uint32_t rolled_back = 0;
                        restored = observed_protection(rolled_back) &&
                            rolled_back == old_protect;
                    }
                }
                const std::string failure = restored
                    ? "The requested protection was not confirmed; the original protection was restored and verified"
                    : "The requested protection was not confirmed and restoration could not be verified";
                if (!restored)
                    bm_raise_uncertain_mutation_diagnostic(task_id, target_text,
                        failure + (rollback_error.empty() ? std::string()
                            : ": " + rollback_error));
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "Protection readback did not match", failure,
                    "diagnostic." + task_id));
                return;
            }
            char summary[160] = {};
            std::snprintf(summary, sizeof(summary),
                "Protection changed and verified: 0x%X -> 0x%X",
                static_cast<unsigned>(old_protect), static_cast<unsigned>(new_protect));
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::completed, 1.0f,
                "Finished", summary));
        } catch (const std::exception& exception) {
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "Protection mutation failed", exception.what(),
                "diagnostic." + task_id));
        } catch (...) {
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "Protection mutation failed", "Unknown mutation failure",
                "diagnostic." + task_id));
        }
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        state->change_protect_pending.store(false, std::memory_order_release);
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Executor rejected protection mutation", submitted.reject_reason,
            "diagnostic." + task_id));
        return false;
    }
    return true;
}

bool bm_dump_region_to_disk(QtBinaryMapViewState& state, std::uint64_t base,
    std::uint64_t size, const std::string& kind_label, QWidget* dialog_parent,
    std::optional<qt_binary_map_live_target_binding_t> live_binding) {
    auto& bridge = QtAnalysisBridge::instance();
    if (size == 0 || base > (std::numeric_limits<std::uint64_t>::max)() - size) {
        bridge.toastError(QStringLiteral("Region range is empty or invalid"), 2.5);
        return false;
    }
    const std::uint64_t kMaxDump = 256ULL * 1024ULL * 1024ULL;
    if (size > kMaxDump) {
        bridge.toastWarning(QStringLiteral("Region exceeds 256 MiB dump cap"), 3.0);
        return false;
    }
    char default_name[96] = {};
    std::snprintf(default_name, sizeof(default_name), "dump_%s_%016llX_%llu.bin",
        kind_label.empty() ? "region" : kind_label.c_str(),
        static_cast<unsigned long long>(base), static_cast<unsigned long long>(size));
    const auto path = dialogs::save_file(dialog_parent, QStringLiteral("Dump Region"),
        "Binary (*.bin)\0*.bin\0All files (*.*)\0*.*\0\0", QStringLiteral("bin"),
        QString::fromLatin1(default_name));
    if (!path || path->empty()) {
        diag::log_tagged_fmt("binary_map",
            "dump_region cancelled base=0x%llX size=%llu",
            static_cast<unsigned long long>(base), static_cast<unsigned long long>(size));
        return false;
    }
    const auto context = disasm_view::capture_workspace(state.workspace.lock());
    if (!context) return false;
    const bool live_dump = context.workspace->target_kind() ==
        aida::analysis::target_kind_t::live_snapshot;
    if (live_dump && (!live_binding ||
        !bm_binding_matches_workspace(*live_binding, context.workspace))) {
        bridge.toastError(
            QStringLiteral("The selected live region belongs to a stale target"), 4.0);
        return false;
    }
    const std::string output_path = *path;
    static std::atomic<std::uint64_t> dump_sequence{1};
    const std::string task_id = "analysis.binary_map.dump." +
        std::to_string(dump_sequence.fetch_add(1, std::memory_order_relaxed));
    const auto cancellation = std::make_shared<std::atomic<bool>>(false);
    diag::log_tagged_critical_fmt("binary_map",
        "dump_region START base=0x%llX size=%llu path='%s'",
        static_cast<unsigned long long>(base), static_cast<unsigned long long>(size),
        output_path.c_str());
    aida::ui::task_center::task_registration_t registration;
    registration.id = task_id;
    registration.source = "analysis.binary_map";
    registration.owner = "Binary Map";
    registration.owner_view = "view.analysis.binary_map";
    registration.owner_action = "analysis.binary_map.dump_region";
    registration.target = output_path;
    registration.label = "Dump Binary Map region";
    registration.stage = "Queued for exact target read and atomic dump";
    registration.affected_entity = output_path;
    registration.callbacks.focus = [] {
        QtAnalysisBridge::instance().openView("view.analysis.binary_map");
    };
    registration.callbacks.open_log = registration.callbacks.focus;
    registration.cancellation_is_safe = live_dump;
    if (live_dump) {
        registration.callbacks.cancel = [cancellation] {
            cancellation->store(true, std::memory_order_release);
            return true;
        };
    }
    if (!aida::ui::task_center::register_task(std::move(registration))) {
        bridge.toastError(QStringLiteral("Task Center rejected the region dump"), 3.0);
        return false;
    }
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "analysis";
    submission.label = "analysis.binary_map.dump_region";
    submission.thread_class = "bounded_file_io";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 2;
    submission.target_pid = context.workspace->identity().process()
        ? context.workspace->identity().process()->pid : 0;
    submission.session_id = task_id.c_str();
    submission.target_id = output_path.c_str();
    submission.generation = context.workspace->generation();
    submission.diagnostic_id = task_id.c_str();
    submission.ui_access_policy = "immutable_snapshots_only";
    submission.failure_policy = "typed_diagnostic";
    submission.shutdown_policy = "drain";
    submission.cancel_hook = [cancellation] {
        cancellation->store(true, std::memory_order_release);
    };
    submission.body = [context, live_binding, cancellation, base, size,
        output_path, task_id]() {
        try {
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::running, -1.0f,
                "Reading exact bytes from the captured target identity"));
            if (context.workspace->target_kind() ==
                    aida::analysis::target_kind_t::live_snapshot && live_binding) {
                std::string identity_error;
                if (!bm_validate_live_binding(*live_binding, context.workspace,
                        identity_error)) {
                    static_cast<void>(aida::ui::task_center::update_task(task_id,
                        aida::ui::task_center::task_state_t::failed, 1.0f,
                        "Stale live target refused", identity_error,
                        "diagnostic." + task_id));
                    return;
                }
                bool cancelled = false;
                std::string error;
                const std::filesystem::path final_path =
                    std::filesystem::u8path(output_path);
                static std::atomic<std::uint64_t> sequence{1};
                const std::filesystem::path temporary(final_path.wstring() + L".tmp." +
                    std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(GetCurrentThreadId()) + L"." +
                    std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)));
                HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH,
                    nullptr);
                if (file == INVALID_HANDLE_VALUE) {
                    static_cast<void>(aida::ui::task_center::update_task(task_id,
                        aida::ui::task_center::task_state_t::failed, 1.0f,
                        "Live region dump failed",
                        "Creating the dump temporary file failed", "diagnostic." + task_id));
                    return;
                }
                bool succeeded = true;
                std::uint64_t offset = 0;
                constexpr std::size_t chunk_capacity = 1U * 1024U * 1024U;
                std::vector<std::uint8_t> bytes;
                bytes.reserve(chunk_capacity);
                while (offset < size) {
                    if (cancellation->load(std::memory_order_acquire)) {
                        cancelled = true;
                        error = "Region dump cancelled";
                        succeeded = false;
                        break;
                    }
                    std::string chunk_identity_error;
                    if (!bm_validate_live_binding(*live_binding, context.workspace,
                            chunk_identity_error)) {
                        error = "The live target changed before a dump read: " +
                            chunk_identity_error;
                        succeeded = false;
                        break;
                    }
                    const std::size_t chunk = static_cast<std::size_t>(
                        (std::min<std::uint64_t>)(size - offset, chunk_capacity));
                    bytes.clear();
                    if (!driver_bridge::read_memory_for(live_binding->process.pid,
                        base + offset, chunk, bytes) || bytes.size() != chunk) {
                        error = "The driver returned a short or failed live-region read at offset " +
                            std::to_string(offset);
                        succeeded = false;
                        break;
                    }
                    if (!bm_validate_live_binding(*live_binding, context.workspace,
                            chunk_identity_error)) {
                        error = "The live target changed after a dump read: " +
                            chunk_identity_error;
                        succeeded = false;
                        break;
                    }
                    DWORD written = 0;
                    if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                        &written, nullptr) ||
                        static_cast<std::size_t>(written) != bytes.size()) {
                        error = "Writing the dump temporary file failed with Win32 error " +
                            std::to_string(GetLastError());
                        succeeded = false;
                        break;
                    }
                    offset += written;
                    static_cast<void>(aida::ui::task_center::update_task(task_id,
                        aida::ui::task_center::task_state_t::running,
                        static_cast<float>(static_cast<double>(offset) /
                            static_cast<double>(size)),
                        "Reading and writing bounded live-memory chunks"));
                }
                if (succeeded && cancellation->load(std::memory_order_acquire)) {
                    cancelled = true;
                    error = "Region dump cancelled before commit";
                    succeeded = false;
                }
                std::string final_identity_error;
                if (succeeded && !bm_validate_live_binding(*live_binding,
                        context.workspace, final_identity_error)) {
                    error = "The live target changed before dump commit: " +
                        final_identity_error;
                    succeeded = false;
                }
                if (succeeded && !FlushFileBuffers(file)) {
                    error = "Flushing the dump temporary file failed with Win32 error " +
                        std::to_string(GetLastError());
                    succeeded = false;
                }
                if (succeeded && cancellation->load(std::memory_order_acquire)) {
                    cancelled = true;
                    error = "Region dump cancelled before atomic replacement";
                    succeeded = false;
                }
                LARGE_INTEGER observed_size{};
                if (succeeded && (!GetFileSizeEx(file, &observed_size) ||
                    observed_size.QuadPart < 0 ||
                    static_cast<std::uint64_t>(observed_size.QuadPart) != size)) {
                    error = "The dump temporary file size did not match the selected region";
                    succeeded = false;
                }
                if (!CloseHandle(file) && succeeded) {
                    error = "Closing the dump temporary file failed with Win32 error " +
                        std::to_string(GetLastError());
                    succeeded = false;
                }
                if (succeeded && !MoveFileExW(temporary.c_str(), final_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    error = "Replacing the dump destination failed with Win32 error " +
                        std::to_string(GetLastError());
                    succeeded = false;
                }
                if (!succeeded) DeleteFileW(temporary.c_str());
                if (!succeeded) {
                    static_cast<void>(aida::ui::task_center::update_task(task_id,
                        cancelled ? aida::ui::task_center::task_state_t::cancelled
                            : aida::ui::task_center::task_state_t::failed,
                        1.0f, cancelled ? "Region dump cancelled"
                            : "Live region dump failed", error,
                        cancelled ? std::string() : "diagnostic." + task_id));
                    return;
                }
                diag::log_tagged_critical_fmt("binary_map",
                    "dump_region DONE bytes=%llu path='%s'",
                    static_cast<unsigned long long>(size), output_path.c_str());
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::completed, 1.0f,
                    "Finished", "Region dumped atomically to " + output_path));
                return;
            } else if (const auto address = disasm_view::typed_address(context, base)) {
                std::vector<std::uint8_t> buffer;
                auto bytes = disasm_view::read_bytes(context, *address,
                    static_cast<std::size_t>(size));
                if (bytes) buffer = std::move(bytes.value());
                if (buffer.size() != static_cast<std::size_t>(size)) {
                    static_cast<void>(aida::ui::task_center::update_task(task_id,
                        aida::ui::task_center::task_state_t::failed, 1.0f,
                        "Exact target read failed",
                        "Requested " + std::to_string(size) + " bytes but received " +
                            std::to_string(buffer.size()), "diagnostic." + task_id));
                    return;
                }
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::running, -1.0f,
                    "Writing a same-directory temporary file"));
                std::string error;
                if (!bm_atomic_write_exact(output_path, buffer.data(), buffer.size(),
                        error)) {
                    static_cast<void>(aida::ui::task_center::update_task(task_id,
                        aida::ui::task_center::task_state_t::failed, 1.0f,
                        "Atomic region dump failed", error, "diagnostic." + task_id));
                    return;
                }
                diag::log_tagged_critical_fmt("binary_map",
                    "dump_region DONE bytes=%zu path='%s'", buffer.size(),
                    output_path.c_str());
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::completed, 1.0f,
                    "Finished", "Region dumped atomically to " + output_path));
                return;
            }
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "Exact target read failed",
                "The selected address is not readable in this workspace",
                "diagnostic." + task_id));
        } catch (const std::exception& exception) {
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "Region dump failed", exception.what(), "diagnostic." + task_id));
        } catch (...) {
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "Region dump failed", "Unknown dump failure", "diagnostic." + task_id));
        }
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Executor rejected region dump", submitted.reject_reason,
            "diagnostic." + task_id));
        bridge.toastError(QStringLiteral(
            "The region dump could not be queued; see Task Center"), 3.0);
        return false;
    }
    return true;
}

void bm_jump_to_address(QtBinaryMapViewState& state, std::uint64_t va) {
    if (va == 0) {
        diag::log_tagged_fmt("binary_map", "jump_to_address SKIPPED va=0x0");
        return;
    }
    QtAnalysisBridge::instance().navigateTo(state.workspace.lock(), va,
        "document.disassembly");
    diag::log_tagged_fmt("binary_map", "jump_to_disasm va=0x%llX",
        static_cast<unsigned long long>(va));
}

void bm_jump_to_hex(QtBinaryMapViewState& state, std::uint64_t va, std::size_t size,
    std::optional<qt_binary_map_live_target_binding_t> live_binding) {
    auto& bridge = QtAnalysisBridge::instance();
    if (va == 0) {
        diag::log_tagged_fmt("binary_map", "[binmap_audit] jump_to_hex SKIPPED va=0x0");
        return;
    }
    if (size == 0) size = 0x200;
    const std::size_t kMaxHex = 1u * 1024u * 1024u;
    if (size > kMaxHex) size = kMaxHex;
    const auto context = disasm_view::capture_workspace(state.workspace.lock());
    if (!context) return;
    const bool live_ok = context.workspace->target_kind() ==
        aida::analysis::target_kind_t::live_snapshot;
    if (live_ok) {
        std::string identity_error;
        if (!live_binding ||
            !bm_validate_live_binding(*live_binding, context.workspace, identity_error)) {
            bridge.toastError(QStringLiteral(
                "The selected live address belongs to a stale target"), 4.0);
            return;
        }
    }
    bool used_static = false;
    bool ok = false;
    if (live_ok)
        ok = hex_view::request_live_memory(context, va, size);
    if (!ok) {
        const auto address = disasm_view::typed_address(context, va);
        auto blob = address ? disasm_view::read_bytes(context, *address, size)
            : aida::analysis::workspace_result_t<std::vector<std::uint8_t>>::failure(
                aida::analysis::make_workspace_error(
                    aida::analysis::workspace_error_code_t::out_of_range,
                    "Address is outside the selected workspace", "binary_map.hex"));
        if (blob && !blob.value().empty()) {
            hex_view::activate(context);
            ok = true;
            used_static = true;
        }
    }
    if (ok) {
        bridge.openView("document.hex");
        diag::log_tagged_fmt("binary_map",
            "jump_to_hex va=0x%llX size=%zu path=%s",
            static_cast<unsigned long long>(va), size,
            used_static ? "static_pe" : "live");
    } else {
        bridge.toastWarning(QStringLiteral("Hex view: failed to read bytes"), 2.5);
        diag::log_tagged_fmt("binary_map",
            "[binmap_audit] jump_to_hex FAILED va=0x%llX size=%zu live_ok=%d",
            static_cast<unsigned long long>(va), size, live_ok ? 1 : 0);
    }
}

void bm_set_function_pinned(QtBinaryMapViewState& state, std::uint64_t va, bool pinned) {
    const auto workspace = state.workspace.lock();
    if (!workspace) return;
    if (pinned) aida::binary_map::pin_function(workspace, va);
    else aida::binary_map::unpin_function(workspace, va);
}

QtBinaryMapViewState::~QtBinaryMapViewState() {
    if (sub_binary_loaded_.valid()) aida::events::unsubscribe(sub_binary_loaded_);
    if (sub_process_created_.valid()) aida::events::unsubscribe(sub_process_created_);
    if (sub_process_exited_.valid()) aida::events::unsubscribe(sub_process_exited_);
}

void bm_install_event_subscriptions(
    const std::shared_ptr<QtBinaryMapViewState>& state_handle) {
    if (!state_handle) return;
    auto& s = *state_handle;
    const std::weak_ptr<QtBinaryMapViewState> weak_state = state_handle;
    const std::weak_ptr<aida::analysis::analysis_workspace_t> weak_workspace =
        s.workspace;
    if (!s.sub_binary_loaded_.valid()) {
        s.sub_binary_loaded_ = aida::events::subscribe(
            aida::events::event_binary_loaded,
            [weak_state, weak_workspace](const aida::events::binary_loaded_t& payload) {
                auto state = weak_state.lock();
                auto workspace = weak_workspace.lock();
                if (!state || !workspace ||
                    workspace->identity().normalized_source_path() !=
                        payload.binary_path) return;
                auto& vs = *state;
                vs.refresh_requested.store(true, std::memory_order_release);
                vs.live_refresh_requested.store(true, std::memory_order_release);
                bool identity_changed = false;
                {
                    std::lock_guard<std::mutex> g(vs.mutex);
                    identity_changed =
                        vs.last_binary_identity_path != payload.binary_path ||
                        vs.last_binary_identity_base != payload.image_base ||
                        vs.last_binary_identity_size != payload.image_size;
                    vs.last_binary_identity_path = payload.binary_path;
                    vs.last_binary_identity_base = payload.image_base;
                    vs.last_binary_identity_size = payload.image_size;
                    if (identity_changed) {
                        std::atomic_store_explicit(&vs.map,
                            std::shared_ptr<const aida::binary_map::map_t>(
                                std::make_shared<aida::binary_map::map_t>()),
                            std::memory_order_release);
                        vs.has_map.store(false, std::memory_order_release);
                        vs.collapsed_groups.clear();
                        std::atomic_store_explicit(&vs.rendered_text,
                            std::shared_ptr<const std::string>(
                                std::make_shared<std::string>()),
                            std::memory_order_release);
                    }
                }
                if (identity_changed) {
                    vs.auto_refreshed_once = false;
                    vs.selected_va.store(0, std::memory_order_release);
                    vs.hover_function_va = 0;
                }
                diag::log_tagged_fmt("binary_map",
                    "event_binary_loaded path='%s' image_base=0x%llX image_size=%u identity_changed=%d -> refresh_requested + live_refresh_requested",
                    payload.binary_path.c_str(),
                    static_cast<unsigned long long>(payload.image_base),
                    static_cast<unsigned>(payload.image_size),
                    identity_changed ? 1 : 0);
            });
    }
    if (!s.sub_process_created_.valid()) {
        s.sub_process_created_ = aida::events::subscribe(
            aida::events::event_process_created,
            [weak_state, weak_workspace](const aida::events::process_created_t& payload) {
                auto state = weak_state.lock();
                auto workspace = weak_workspace.lock();
                if (!state || !workspace || !workspace->identity().process() ||
                    workspace->identity().process()->pid != payload.process_id) return;
                auto& vs = *state;
                vs.live_refresh_requested.store(true, std::memory_order_release);
                diag::log_tagged_fmt("binary_map",
                    "event_process_created pid=%u image='%s' -> live_refresh_requested",
                    static_cast<unsigned>(payload.process_id),
                    payload.image_name.c_str());
            });
    }
    if (!s.sub_process_exited_.valid()) {
        s.sub_process_exited_ = aida::events::subscribe(
            aida::events::event_process_exited,
            [weak_state, weak_workspace](const aida::events::process_exited_t& payload) {
                auto state = weak_state.lock();
                auto workspace = weak_workspace.lock();
                if (!state || !workspace || !workspace->identity().process() ||
                    workspace->identity().process()->pid != payload.process_id) return;
                auto& vs = *state;
                std::atomic_store_explicit(&vs.live,
                    std::shared_ptr<const qt_binary_map_live_snapshot_t>(
                        std::make_shared<qt_binary_map_live_snapshot_t>()),
                    std::memory_order_release);
                vs.live_selected_base.store(0, std::memory_order_release);
                vs.live_hover_index.store(-1, std::memory_order_release);
                diag::log_tagged_fmt("binary_map",
                    "event_process_exited pid=%u -> live snapshot cleared",
                    static_cast<unsigned>(payload.process_id));
            });
    }
}

}
