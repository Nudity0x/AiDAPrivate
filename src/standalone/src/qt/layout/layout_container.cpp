#include "qt/layout/layout_container.hpp"

#include "helpers/diag_log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace aida::qt::layout {

namespace {

constexpr std::wstring_view k_legacy_primary_name = L"standalone-layout-v1.aida-layout";
constexpr std::wstring_view k_active_record_name = L"active-workspace-v2.txt";

std::recursive_mutex& write_mutex() noexcept {
    static std::recursive_mutex value;
    return value;
}

std::atomic<std::uint64_t>& committed_generation_storage() noexcept {
    static std::atomic<std::uint64_t> value{0};
    return value;
}

std::atomic<std::uint64_t>& failed_generation_storage() noexcept {
    static std::atomic<std::uint64_t> value{0};
    return value;
}

}

std::uint64_t fnv1a64(std::string_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char raw_byte : value) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool parse_decimal(std::string_view value, std::uint64_t& output) noexcept {
    if (value.empty())
        return false;
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
        return false;
    output = parsed;
    return true;
}

bool parse_hex(std::string_view value, std::uint64_t& output) noexcept {
    if (value.empty())
        return false;
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 16);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
        return false;
    output = parsed;
    return true;
}

bool parse_signed_decimal(std::string_view value, std::int64_t& output) noexcept {
    if (value.empty())
        return false;
    std::int64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
        return false;
    output = parsed;
    return true;
}

bool valid_registry_fingerprint(std::string_view value) noexcept {
    if (value.size() != 16)
        return false;
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f');
    });
}

bool workspace_directory(std::filesystem::path& directory) noexcept {
    PWSTR roaming = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &roaming);
    if (FAILED(result) || !roaming)
        return false;
    try {
        directory = std::filesystem::path(roaming) / L"AiDA" / L"Standalone" / L"workspaces";
    } catch (...) {
        CoTaskMemFree(roaming);
        return false;
    }
    CoTaskMemFree(roaming);
    return true;
}

std::filesystem::path active_record_path(const std::filesystem::path& directory) {
    return directory / std::wstring(k_active_record_name);
}

std::filesystem::path legacy_primary_path(const std::filesystem::path& directory) {
    return directory / std::wstring(k_legacy_primary_name);
}

layout_paths_t preset_paths(const std::filesystem::path& directory,
                            docking::workspace_preset_t preset) {
    const std::string_view id = docking::preset_stable_id(preset);
    const std::wstring wide_id(id.begin(), id.end());
    layout_paths_t paths;
    paths.directory = directory;
    paths.primary = directory / (wide_id + L".aida-layout");
    paths.backup = directory / (wide_id + L".aida-layout.bak");
    paths.invalid = directory / (wide_id + L".aida-layout.invalid");
    return paths;
}

std::filesystem::path user_layout_path(const std::filesystem::path& directory,
                                       std::string_view name) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring encoded = L"u-";
    encoded.reserve(2U + name.size() * 2U + 12U);
    for (const char raw_byte : name) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0FU]);
    }
    return directory / L"user" / (encoded + L".aida-layout");
}

layout_paths_t named_user_paths(const std::filesystem::path& directory, std::string_view name) {
    layout_paths_t paths;
    paths.directory = directory / L"user";
    paths.primary = user_layout_path(directory, name);
    paths.backup = paths.primary;
    paths.backup += L".bak";
    paths.invalid = paths.primary;
    paths.invalid += L".invalid";
    return paths;
}

bool decode_user_layout_filename(const std::filesystem::path& path, std::string& name) noexcept {
    try {
        constexpr std::wstring_view suffix = L".aida-layout";
        const std::wstring filename = path.filename().wstring();
        if (filename.size() <= 2U + suffix.size() || filename.compare(
                filename.size() - suffix.size(), suffix.size(), suffix) != 0 ||
            filename[0] != L'u' || filename[1] != L'-')
            return false;
        const std::wstring_view encoded(filename.data() + 2U,
            filename.size() - 2U - suffix.size());
        if (encoded.empty() || encoded.size() % 2U != 0)
            return false;
        auto nibble = [](wchar_t value) noexcept -> int {
            if (value >= L'0' && value <= L'9') return value - L'0';
            if (value >= L'a' && value <= L'f') return value - L'a' + 10;
            return -1;
        };
        std::string decoded;
        decoded.reserve(encoded.size() / 2U);
        for (std::size_t index = 0; index < encoded.size(); index += 2U) {
            const int high = nibble(encoded[index]);
            const int low = nibble(encoded[index + 1U]);
            if (high < 0 || low < 0)
                return false;
            decoded.push_back(static_cast<char>((high << 4) | low));
        }
        if (!docking::valid_user_layout_name(decoded))
            return false;
        name = std::move(decoded);
        return true;
    } catch (...) {
        return false;
    }
}

bool managed_user_layout_artifact(const std::filesystem::path& path) noexcept {
    try {
        std::filesystem::path primary = path;
        const std::wstring filename = primary.filename().wstring();
        if (filename.size() > 4U && filename.compare(filename.size() - 4U, 4U, L".bak") == 0)
            primary.replace_filename(filename.substr(0, filename.size() - 4U));
        else if (filename.size() > 8U && filename.compare(filename.size() - 8U, 8U, L".invalid") == 0)
            primary.replace_filename(filename.substr(0, filename.size() - 8U));
        std::string name;
        return decode_user_layout_filename(primary, name);
    } catch (...) {
        return false;
    }
}

bool remove_file_exact(const std::filesystem::path& path) noexcept {
    if (DeleteFileW(path.c_str()))
        return true;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

active_record_t read_active_record(const std::filesystem::path& path) noexcept {
    active_record_t result;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return result;
    char value[160]{};
    DWORD read = 0;
    const bool valid = ReadFile(file, value, sizeof(value) - 1U, &read, nullptr) != FALSE;
    CloseHandle(file);
    if (!valid || read == 0 || read >= sizeof(value))
        return result;
    const std::string_view record(value, read);
    const std::size_t separator = record.find('\n');
    const std::string_view id = record.substr(0, separator);
    result.locked = separator != std::string_view::npos &&
        separator + 1U < record.size() && record[separator + 1U] == '1';
    const std::size_t user_separator = separator == std::string_view::npos
        ? std::string_view::npos : record.find('\n', separator + 1U);
    if (user_separator != std::string_view::npos && user_separator + 1U < record.size()) {
        const std::string_view user_name = record.substr(user_separator + 1U);
        if (docking::valid_user_layout_name(user_name))
            result.user_name.assign(user_name);
    }
    docking::workspace_preset_t preset = docking::workspace_preset_t::analysis;
    if (!docking::preset_for_stable_id(id, preset))
        return result;
    result.preset = preset;
    result.present = true;
    return result;
}

bool write_active_record(const std::filesystem::path& directory,
                         const std::filesystem::path& active_record,
                         docking::workspace_preset_t preset, bool locked,
                         std::string_view user_name) noexcept {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
    std::filesystem::path temporary = active_record;
    temporary += L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    std::string record;
    try {
        record = std::string(docking::preset_stable_id(preset)) +
            (locked ? "\n1\n" : "\n0\n");
        if (!user_name.empty()) {
            if (!docking::valid_user_layout_name(user_name))
                throw std::invalid_argument("invalid user workspace name");
            record.append(user_name);
        }
    } catch (...) {
        CloseHandle(file);
        DeleteFileW(temporary.c_str());
        return false;
    }
    DWORD written = 0;
    const bool saved = WriteFile(file, record.data(), static_cast<DWORD>(record.size()), &written, nullptr) != FALSE &&
        written == record.size() && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!saved || !MoveFileExW(temporary.c_str(), active_record.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

namespace {

struct header_values_t {
    std::uint64_t schema = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t checksum = 0;
    std::uint64_t surface_bytes = 0;
    std::uint64_t surface_checksum = 0;
    std::uint64_t generation = 0;
    std::uint64_t clean_shutdown = 0;
    std::uint64_t preset_revision_value = 0;
    std::uint64_t saved_unix_ms = 0;
    std::int64_t monitor_work_x = 0;
    std::int64_t monitor_work_y = 0;
    std::uint64_t monitor_work_width = 0;
    std::uint64_t monitor_work_height = 0;
    std::uint64_t monitor_dpi_milli = 0;
    std::uint64_t layout_locked = 0;
    std::uint64_t ads_version = 0;
    std::string_view registry_fingerprint;
    docking::workspace_preset_t preset = docking::workspace_preset_t::analysis;
    bool schema_seen = false;
    bool bytes_seen = false;
    bool checksum_seen = false;
    bool surface_bytes_seen = false;
    bool surface_checksum_seen = false;
    bool imgui_seen = false;
    bool imgui_source_seen = false;
    bool qt_version_seen = false;
    bool ads_version_seen = false;
    bool preset_seen = false;
    bool preset_revision_seen = false;
    bool registry_seen = false;
    bool generation_seen = false;
    bool clean_seen = false;
    bool lock_seen = false;
    bool timestamp_seen = false;
    bool monitor_x_seen = false;
    bool monitor_y_seen = false;
    bool monitor_width_seen = false;
    bool monitor_height_seen = false;
    bool monitor_dpi_seen = false;
    bool node_root_seen = false;
    bool node_navigator_seen = false;
    bool node_documents_seen = false;
    bool node_inspector_seen = false;
    bool node_bottom_seen = false;
};

bool parse_header_fields(std::string_view input, std::size_t terminator,
                         header_values_t& values) noexcept {
    std::size_t cursor = k_magic.size();
    while (cursor < terminator) {
        const std::size_t line_end = input.find("\r\n", cursor);
        if (line_end == std::string_view::npos || line_end > terminator)
            return false;
        const std::string_view line = input.substr(cursor, line_end - cursor);
        const std::size_t separator = line.find('=');
        if (separator == std::string_view::npos)
            return false;
        const std::string_view key = line.substr(0, separator);
        const std::string_view value = line.substr(separator + 1U);
        if (key == "schema") {
            if (values.schema_seen || !parse_decimal(value, values.schema))
                return false;
            values.schema_seen = true;
        } else if (key == "payload_bytes") {
            if (values.bytes_seen || !parse_decimal(value, values.payload_bytes))
                return false;
            values.bytes_seen = true;
        } else if (key == "payload_fnv1a64") {
            if (values.checksum_seen || !parse_hex(value, values.checksum))
                return false;
            values.checksum_seen = true;
        } else if (key == "surface_bytes") {
            if (values.surface_bytes_seen || !parse_decimal(value, values.surface_bytes))
                return false;
            values.surface_bytes_seen = true;
        } else if (key == "surface_fnv1a64") {
            if (values.surface_checksum_seen || !parse_hex(value, values.surface_checksum))
                return false;
            values.surface_checksum_seen = true;
        } else if (key == "imgui_version") {
            if (values.imgui_seen || value.empty())
                return false;
            values.imgui_seen = true;
        } else if (key == "imgui_source_sha256") {
            if (values.imgui_source_seen || value.empty())
                return false;
            values.imgui_source_seen = true;
        } else if (key == "qt_version") {
            if (values.qt_version_seen || value != k_expected_qt_version)
                return false;
            values.qt_version_seen = true;
        } else if (key == "ads_version") {
            if (values.ads_version_seen || !parse_decimal(value, values.ads_version) ||
                values.ads_version == 0 || values.ads_version > k_expected_ads_state_version)
                return false;
            values.ads_version_seen = true;
        } else if (key == "preset_id") {
            if (values.preset_seen)
                return false;
            docking::workspace_preset_t parsed_preset = docking::workspace_preset_t::analysis;
            if (!docking::preset_for_stable_id(value, parsed_preset))
                return false;
            values.preset = parsed_preset;
            values.preset_seen = true;
        } else if (key == "preset_revision") {
            if (values.preset_revision_seen ||
                !parse_decimal(value, values.preset_revision_value) ||
                values.preset_revision_value == 0)
                return false;
            values.preset_revision_seen = true;
        } else if (key == "view_registry") {
            if (values.registry_seen)
                return false;
            values.registry_fingerprint = value;
            values.registry_seen = true;
        } else if (key == "generation") {
            if (values.generation_seen || !parse_decimal(value, values.generation) ||
                values.generation == 0)
                return false;
            values.generation_seen = true;
        } else if (key == "clean_shutdown") {
            if (values.clean_seen || !parse_decimal(value, values.clean_shutdown) ||
                values.clean_shutdown > 1)
                return false;
            values.clean_seen = true;
        } else if (key == "layout_locked") {
            if (values.lock_seen || !parse_decimal(value, values.layout_locked) ||
                values.layout_locked > 1)
                return false;
            values.lock_seen = true;
        } else if (key == "saved_unix_ms") {
            if (values.timestamp_seen || !parse_decimal(value, values.saved_unix_ms) ||
                values.saved_unix_ms == 0)
                return false;
            values.timestamp_seen = true;
        } else if (key == "monitor_work_x") {
            if (values.monitor_x_seen || !parse_signed_decimal(value, values.monitor_work_x))
                return false;
            values.monitor_x_seen = true;
        } else if (key == "monitor_work_y") {
            if (values.monitor_y_seen || !parse_signed_decimal(value, values.monitor_work_y))
                return false;
            values.monitor_y_seen = true;
        } else if (key == "monitor_work_width") {
            if (values.monitor_width_seen || !parse_decimal(value, values.monitor_work_width))
                return false;
            values.monitor_width_seen = true;
        } else if (key == "monitor_work_height") {
            if (values.monitor_height_seen || !parse_decimal(value, values.monitor_work_height))
                return false;
            values.monitor_height_seen = true;
        } else if (key == "monitor_dpi_milli") {
            if (values.monitor_dpi_seen || !parse_decimal(value, values.monitor_dpi_milli))
                return false;
            values.monitor_dpi_seen = true;
        } else if (key == "node_root" || key == "node_navigator" || key == "node_documents" ||
                   key == "node_inspector" || key == "node_bottom") {
            std::uint64_t ignored = 0;
            if (!parse_hex(value, ignored))
                return false;
            if (key == "node_root") {
                if (values.node_root_seen)
                    return false;
                values.node_root_seen = true;
            } else if (key == "node_navigator") {
                if (values.node_navigator_seen)
                    return false;
                values.node_navigator_seen = true;
            } else if (key == "node_documents") {
                if (values.node_documents_seen)
                    return false;
                values.node_documents_seen = true;
            } else if (key == "node_inspector") {
                if (values.node_inspector_seen)
                    return false;
                values.node_inspector_seen = true;
            } else {
                if (values.node_bottom_seen)
                    return false;
                values.node_bottom_seen = true;
            }
        } else {
            return false;
        }
        cursor = line_end + 2U;
    }
    return true;
}

bool validate_schema4(const header_values_t& values, std::size_t payload_offset,
                      std::size_t input_size) noexcept {
    if (!values.schema_seen || !values.bytes_seen || !values.checksum_seen ||
        !values.surface_bytes_seen || !values.surface_checksum_seen ||
        !values.qt_version_seen || !values.ads_version_seen || !values.preset_seen ||
        !values.preset_revision_seen || !values.registry_seen || !values.generation_seen ||
        !values.clean_seen || !values.lock_seen || !values.timestamp_seen ||
        !values.monitor_x_seen || !values.monitor_y_seen || !values.monitor_width_seen ||
        !values.monitor_height_seen || !values.monitor_dpi_seen)
        return false;
    if (values.payload_bytes == 0 || values.payload_bytes > k_maximum_payload_bytes ||
        values.surface_bytes == 0 || values.surface_bytes > k_maximum_surface_bytes)
        return false;
    if (payload_offset > input_size)
        return false;
    const std::uint64_t total_payload = values.payload_bytes + values.surface_bytes;
    if (total_payload != input_size - payload_offset)
        return false;
    if (values.preset_revision_value > docking::preset_revision(values.preset))
        return false;
    if (!valid_registry_fingerprint(values.registry_fingerprint))
        return false;
    if (values.monitor_work_x < -10000000 || values.monitor_work_x > 10000000 ||
        values.monitor_work_y < -10000000 || values.monitor_work_y > 10000000 ||
        values.monitor_work_width == 0 || values.monitor_work_width > 10000000 ||
        values.monitor_work_height == 0 || values.monitor_work_height > 10000000 ||
        values.monitor_dpi_milli < 250 || values.monitor_dpi_milli > 8000)
        return false;
    return true;
}

bool validate_legacy(const header_values_t& values, std::size_t payload_offset,
                     std::size_t input_size) noexcept {
    if (!values.schema_seen || !values.bytes_seen || !values.checksum_seen ||
        !values.imgui_seen || !values.imgui_source_seen || !values.preset_seen ||
        !values.preset_revision_seen || !values.registry_seen || !values.generation_seen ||
        !values.clean_seen || !values.node_root_seen || !values.node_navigator_seen ||
        !values.node_documents_seen || !values.node_inspector_seen || !values.node_bottom_seen)
        return false;
    if (values.schema < 1 || values.schema > 3)
        return false;
    if (values.payload_bytes == 0 || values.payload_bytes > k_maximum_payload_bytes)
        return false;
    if (payload_offset > input_size || values.payload_bytes != input_size - payload_offset)
        return false;
    if (values.preset_revision_value > docking::preset_revision(values.preset))
        return false;
    if (values.schema >= 2 && !values.lock_seen)
        return false;
    if (values.schema >= 3) {
        if (!values.timestamp_seen || !values.monitor_x_seen || !values.monitor_y_seen ||
            !values.monitor_width_seen || !values.monitor_height_seen || !values.monitor_dpi_seen ||
            !valid_registry_fingerprint(values.registry_fingerprint) ||
            values.monitor_work_x < -10000000 || values.monitor_work_x > 10000000 ||
            values.monitor_work_y < -10000000 || values.monitor_work_y > 10000000 ||
            values.monitor_work_width == 0 || values.monitor_work_width > 10000000 ||
            values.monitor_work_height == 0 || values.monitor_work_height > 10000000 ||
            values.monitor_dpi_milli < 250 || values.monitor_dpi_milli > 8000)
            return false;
    } else if (values.registry_fingerprint != "compatibility-v1" &&
               values.registry_fingerprint != "stable-v2") {
        return false;
    }
    return true;
}

void fill_metadata(const header_values_t& values, record_metadata_t& metadata,
                   payload_kind_t kind) noexcept {
    metadata.generation = values.generation;
    metadata.clean_shutdown = values.clean_shutdown != 0;
    metadata.preset = values.preset;
    metadata.locked = values.layout_locked != 0;
    metadata.preset_revision = static_cast<std::uint32_t>(values.preset_revision_value);
    metadata.saved_unix_ms = values.saved_unix_ms;
    metadata.environment = {values.monitor_work_x, values.monitor_work_y,
        values.monitor_work_width, values.monitor_work_height,
        static_cast<std::uint32_t>(values.monitor_dpi_milli == 0
            ? 1000 : values.monitor_dpi_milli)};
    metadata.registry_fingerprint.assign(values.registry_fingerprint);
    metadata.schema = static_cast<std::uint32_t>(values.schema);
    metadata.payload_kind = kind;
}

}

bool extract_container(const std::vector<char>& container, record_metadata_t& metadata,
                       std::string& payload, std::string& surface) noexcept {
    const std::string_view input(container.data(), container.size());
    if (input.size() < k_magic.size() || input.substr(0, k_magic.size()) != k_magic)
        return false;
    const std::size_t terminator = input.find(k_header_terminator, k_magic.size());
    if (terminator == std::string_view::npos || terminator > 1536U)
        return false;

    header_values_t values;
    if (!parse_header_fields(input, terminator, values))
        return false;
    if (!values.schema_seen || values.schema < 1 || values.schema > k_schema_version)
        return false;

    const std::size_t payload_offset = terminator + k_header_terminator.size();
    if (values.schema == k_schema_version) {
        if (!validate_schema4(values, payload_offset, input.size()))
            return false;
        const std::string_view dock_payload = input.substr(payload_offset,
            static_cast<std::size_t>(values.payload_bytes));
        const std::string_view surface_payload = input.substr(
            payload_offset + values.payload_bytes,
            static_cast<std::size_t>(values.surface_bytes));
        if (dock_payload.find('\0') != std::string_view::npos ||
            fnv1a64(dock_payload) != values.checksum ||
            dock_payload.find("<QtAdvancedDockingSystem") == std::string_view::npos)
            return false;
        if (surface_payload.find('\0') != std::string_view::npos ||
            fnv1a64(surface_payload) != values.surface_checksum)
            return false;
        try {
            payload.assign(dock_payload);
            surface.assign(surface_payload);
        } catch (...) {
            return false;
        }
        fill_metadata(values, metadata, payload_kind_t::qads_xml);
        return true;
    }

    if (!validate_legacy(values, payload_offset, input.size()))
        return false;
    const std::string_view legacy_payload = input.substr(payload_offset,
        static_cast<std::size_t>(values.payload_bytes));
    if (legacy_payload.find('\0') != std::string_view::npos ||
        fnv1a64(legacy_payload) != values.checksum)
        return false;
    payload.clear();
    surface.clear();
    fill_metadata(values, metadata, payload_kind_t::imgui_ini);
    return true;
}

std::string serialize_container(docking::workspace_preset_t preset, bool locked,
    std::uint64_t generation, bool clean_shutdown,
    const layout_environment_t& environment, std::string_view registry_fingerprint,
    std::string_view payload, std::string_view surface) {
    if (payload.empty() || payload.size() > k_maximum_payload_bytes || generation == 0 ||
        surface.empty() || surface.size() > k_maximum_surface_bytes ||
        !valid_registry_fingerprint(registry_fingerprint) ||
        payload.find("<QtAdvancedDockingSystem") == std::string_view::npos)
        return {};
    FILETIME file_time{};
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER timestamp{};
    timestamp.LowPart = file_time.dwLowDateTime;
    timestamp.HighPart = file_time.dwHighDateTime;
    constexpr std::uint64_t windows_to_unix_epoch = 116444736000000000ULL;
    if (timestamp.QuadPart <= windows_to_unix_epoch)
        return {};
    const std::uint64_t saved_unix_ms =
        (timestamp.QuadPart - windows_to_unix_epoch) / 10000ULL;

    char header[1536]{};
    const int header_length = std::snprintf(header, sizeof(header),
        "AIDA_WORKSPACE_LAYOUT\r\nschema=%u\r\nqt_version=%.*s\r\nads_version=%llu\r\npreset_id=%.*s\r\npreset_revision=%u\r\nview_registry=%.*s\r\ngeneration=%llu\r\nclean_shutdown=%u\r\nlayout_locked=%u\r\nsaved_unix_ms=%llu\r\nmonitor_work_x=%lld\r\nmonitor_work_y=%lld\r\nmonitor_work_width=%llu\r\nmonitor_work_height=%llu\r\nmonitor_dpi_milli=%u\r\npayload_bytes=%llu\r\npayload_fnv1a64=%016llx\r\nsurface_bytes=%llu\r\nsurface_fnv1a64=%016llx\r\n\r\n",
        k_schema_version,
        static_cast<int>(k_expected_qt_version.size()), k_expected_qt_version.data(),
        static_cast<unsigned long long>(k_expected_ads_state_version),
        static_cast<int>(docking::preset_stable_id(preset).size()),
        docking::preset_stable_id(preset).data(),
        docking::preset_revision(preset),
        static_cast<int>(registry_fingerprint.size()), registry_fingerprint.data(),
        static_cast<unsigned long long>(generation),
        clean_shutdown ? 1U : 0U,
        locked ? 1U : 0U,
        static_cast<unsigned long long>(saved_unix_ms),
        static_cast<long long>(environment.work_x),
        static_cast<long long>(environment.work_y),
        static_cast<unsigned long long>(environment.work_width),
        static_cast<unsigned long long>(environment.work_height),
        environment.dpi_milli,
        static_cast<unsigned long long>(payload.size()),
        static_cast<unsigned long long>(fnv1a64(payload)),
        static_cast<unsigned long long>(surface.size()),
        static_cast<unsigned long long>(fnv1a64(surface)));
    if (header_length <= 0 || static_cast<std::size_t>(header_length) >= sizeof(header))
        return {};
    try {
        std::string container;
        container.reserve(static_cast<std::size_t>(header_length) +
            payload.size() + surface.size());
        container.assign(header, static_cast<std::size_t>(header_length));
        container.append(payload);
        container.append(surface);
        return container;
    } catch (...) {
        return {};
    }
}

bool read_file_bounded(const std::filesystem::path& path, std::vector<char>& output,
                       read_result_t& result) noexcept {
    result = read_result_t::io_failure;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result = GetLastError() == ERROR_FILE_NOT_FOUND ? read_result_t::absent : read_result_t::io_failure;
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > k_maximum_container_bytes) {
        CloseHandle(file);
        result = read_result_t::invalid;
        return false;
    }

    try {
        output.resize(static_cast<std::size_t>(size.QuadPart));
    } catch (...) {
        CloseHandle(file);
        result = read_result_t::io_failure;
        return false;
    }

    std::size_t offset = 0;
    while (offset < output.size()) {
        const DWORD requested = static_cast<DWORD>((std::min)(output.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD completed = 0;
        if (!ReadFile(file, output.data() + offset, requested, &completed, nullptr) || completed == 0) {
            CloseHandle(file);
            output.clear();
            result = read_result_t::io_failure;
            return false;
        }
        offset += completed;
    }
    CloseHandle(file);
    result = read_result_t::valid;
    return true;
}

read_result_t read_layout_payload(const std::filesystem::path& path,
                                  record_metadata_t& metadata,
                                  container_payloads_t& payloads) noexcept {
    std::vector<char> container;
    read_result_t result = read_result_t::io_failure;
    if (!read_file_bounded(path, container, result))
        return result;
    if (!extract_container(container, metadata, payloads.dock_xml, payloads.surface_json))
        return read_result_t::invalid;
    return read_result_t::valid;
}

bool inspect_layout_file(const std::filesystem::path& path,
                         record_metadata_t& metadata) noexcept {
    std::vector<char> container;
    read_result_t result = read_result_t::io_failure;
    if (!read_file_bounded(path, container, result))
        return false;
    std::string payload;
    std::string surface;
    return extract_container(container, metadata, payload, surface);
}

bool validate_layout_file(const std::filesystem::path& path) noexcept {
    record_metadata_t metadata;
    return inspect_layout_file(path, metadata);
}

read_result_t read_layout_with_backup(const layout_paths_t& paths,
                                      record_metadata_t& metadata,
                                      container_payloads_t& payloads) noexcept {
    read_result_t result = read_layout_payload(paths.primary, metadata, payloads);
    if (result == read_result_t::valid)
        return result;
    if (result == read_result_t::invalid &&
        !MoveFileExW(paths.primary.c_str(), paths.invalid.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return read_result_t::io_failure;
    record_metadata_t backup_metadata;
    container_payloads_t backup_payloads;
    const read_result_t backup = read_layout_payload(paths.backup,
        backup_metadata, backup_payloads);
    if (backup == read_result_t::valid) {
        metadata = backup_metadata;
        payloads = std::move(backup_payloads);
        return read_result_t::valid;
    }
    return result == read_result_t::io_failure || backup == read_result_t::io_failure
        ? read_result_t::io_failure
        : result == read_result_t::invalid || backup == read_result_t::invalid
            ? read_result_t::invalid : read_result_t::absent;
}

namespace {

bool write_all(HANDLE file, std::string_view data) noexcept {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const DWORD requested = static_cast<DWORD>((std::min)(data.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD completed = 0;
        if (!WriteFile(file, data.data() + offset, requested, &completed, nullptr) || completed == 0)
            return false;
        offset += completed;
    }
    return true;
}

bool refresh_backup_atomic(const layout_paths_t& paths) noexcept {
    std::filesystem::path temporary;
    try {
        temporary = paths.backup;
        temporary += L".tmp";
    } catch (...) {
        return false;
    }
    DeleteFileW(temporary.c_str());
    if (!CopyFileW(paths.primary.c_str(), temporary.c_str(), TRUE))
        return false;
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    const bool flushed = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!flushed || !MoveFileExW(temporary.c_str(), paths.backup.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

}

bool save_payload(const layout_paths_t& paths, docking::workspace_preset_t preset, bool locked,
                  std::uint64_t generation, bool clean_shutdown, bool skip_backup,
                  const layout_environment_t& environment, std::string_view registry_fingerprint,
                  std::string_view payload, std::string_view surface) {
    if (environment.work_width == 0 || environment.work_height == 0 ||
        environment.dpi_milli < 250 || environment.dpi_milli > 8000)
        return false;
    const std::string container = serialize_container(preset, locked, generation,
        clean_shutdown, environment, registry_fingerprint, payload, surface);
    if (container.empty())
        return false;

    std::error_code directory_error;
    std::filesystem::create_directories(paths.directory, directory_error);
    if (directory_error)
        return false;

    std::filesystem::path temporary = paths.primary;
    temporary += L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const bool wrote = write_all(file, container) && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!wrote) {
        DeleteFileW(temporary.c_str());
        return false;
    }

    if (!skip_backup && GetFileAttributesW(paths.primary.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (validate_layout_file(paths.primary)) {
            if (!refresh_backup_atomic(paths)) {
                DeleteFileW(temporary.c_str());
                return false;
            }
        } else if (!MoveFileExW(paths.primary.c_str(), paths.invalid.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), paths.primary.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool write_generation(const layout_paths_t& paths, docking::workspace_preset_t preset,
                      bool locked, std::uint64_t generation, bool clean_shutdown,
                      bool skip_backup, const layout_environment_t& environment,
                      std::string_view registry_fingerprint,
                      std::string_view payload, std::string_view surface) noexcept {
    std::lock_guard<std::recursive_mutex> lock(write_mutex());
    if (generation <= committed_generation_storage().load(std::memory_order_acquire))
        return true;
    const std::uint64_t started_ms = static_cast<std::uint64_t>(GetTickCount64());
    bool saved = false;
    try {
        saved = save_payload(paths, preset, locked, generation, clean_shutdown,
            skip_backup, environment, registry_fingerprint, payload, surface);
    } catch (...) {
        saved = false;
    }
    if (saved)
        committed_generation_storage().store(generation, std::memory_order_release);
    else
        failed_generation_storage().store(generation, std::memory_order_release);
    diag::log_tagged_fmt("workspace_layout",
        "layout_write_complete generation=%llu clean_shutdown=%d payload_bytes=%llu payload_fnv1a64=%016llx surface_bytes=%llu elapsed_ms=%llu result=%s",
        static_cast<unsigned long long>(generation), clean_shutdown ? 1 : 0,
        static_cast<unsigned long long>(payload.size()),
        static_cast<unsigned long long>(fnv1a64(payload)),
        static_cast<unsigned long long>(surface.size()),
        static_cast<unsigned long long>(static_cast<std::uint64_t>(GetTickCount64()) - started_ms),
        saved ? "saved" : "failed");
    return saved;
}

std::uint64_t committed_generation() noexcept {
    return committed_generation_storage().load(std::memory_order_acquire);
}

std::uint64_t failed_generation() noexcept {
    return failed_generation_storage().exchange(0, std::memory_order_acq_rel);
}

void note_committed_generation(std::uint64_t generation) noexcept {
    committed_generation_storage().store(generation, std::memory_order_release);
}

void reset_generation_state() noexcept {
    committed_generation_storage().store(0, std::memory_order_release);
    failed_generation_storage().store(0, std::memory_order_release);
}

std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>> scan_user_catalog(
    const std::filesystem::path& user_directory, std::string_view active_user) noexcept {
    try {
        auto catalog = std::make_shared<std::vector<docking::user_workspace_descriptor_t>>();
        std::error_code error;
        if (!std::filesystem::exists(user_directory, error))
            return error ? nullptr : catalog;
        std::filesystem::directory_iterator cursor(user_directory, error);
        const std::filesystem::directory_iterator end;
        while (!error && cursor != end) {
            if (catalog->size() >= k_maximum_named_user_layouts)
                return nullptr;
            if (cursor->is_regular_file(error)) {
                std::string name;
                if (decode_user_layout_filename(cursor->path(), name)) {
                    record_metadata_t metadata;
                    if (inspect_layout_file(cursor->path(), metadata))
                        catalog->push_back({name, metadata.preset, metadata.generation,
                            name == active_user});
                }
            }
            if (error)
                return nullptr;
            cursor.increment(error);
        }
        if (error)
            return nullptr;
        std::sort(catalog->begin(), catalog->end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        return catalog;
    } catch (...) {
        return nullptr;
    }
}

}
