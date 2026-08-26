#pragma once

#include "qt/docking/preset_recipes.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aida::qt::layout {

inline constexpr std::uint32_t k_schema_version = 4;
inline constexpr std::string_view k_expected_qt_version = "6.8.3";
inline constexpr std::uint64_t k_expected_ads_state_version = 1;
inline constexpr std::size_t k_maximum_payload_bytes = 4U * 1024U * 1024U;
inline constexpr std::size_t k_maximum_surface_bytes = 1024U * 1024U;
inline constexpr std::size_t k_maximum_container_bytes =
    k_maximum_payload_bytes + k_maximum_surface_bytes + 2048U;
inline constexpr std::size_t k_maximum_named_user_layouts = 4096U;
inline constexpr std::string_view k_magic = "AIDA_WORKSPACE_LAYOUT\r\n";
inline constexpr std::string_view k_header_terminator = "\r\n\r\n";

enum class read_result_t {
    absent,
    io_failure,
    invalid,
    valid
};

enum class payload_kind_t : std::uint8_t {
    qads_xml,
    imgui_ini
};

struct layout_environment_t {
    std::int64_t work_x = 0;
    std::int64_t work_y = 0;
    std::uint64_t work_width = 0;
    std::uint64_t work_height = 0;
    std::uint32_t dpi_milli = 1000;
};

struct record_metadata_t {
    std::uint64_t generation = 0;
    bool clean_shutdown = false;
    docking::workspace_preset_t preset = docking::workspace_preset_t::analysis;
    bool locked = false;
    std::uint32_t preset_revision = 1;
    std::uint64_t saved_unix_ms = 0;
    layout_environment_t environment;
    std::string registry_fingerprint;
    std::uint32_t schema = 0;
    payload_kind_t payload_kind = payload_kind_t::qads_xml;
};

struct layout_paths_t {
    std::filesystem::path directory;
    std::filesystem::path primary;
    std::filesystem::path backup;
    std::filesystem::path invalid;
};

std::uint64_t fnv1a64(std::string_view value) noexcept;
bool parse_decimal(std::string_view value, std::uint64_t& output) noexcept;
bool parse_hex(std::string_view value, std::uint64_t& output) noexcept;
bool parse_signed_decimal(std::string_view value, std::int64_t& output) noexcept;
bool valid_registry_fingerprint(std::string_view value) noexcept;

bool workspace_directory(std::filesystem::path& directory) noexcept;
std::filesystem::path active_record_path(const std::filesystem::path& directory);
std::filesystem::path legacy_primary_path(const std::filesystem::path& directory);
layout_paths_t preset_paths(const std::filesystem::path& directory,
                            docking::workspace_preset_t preset);
layout_paths_t named_user_paths(const std::filesystem::path& directory, std::string_view name);
std::filesystem::path user_layout_path(const std::filesystem::path& directory,
                                       std::string_view name);
bool decode_user_layout_filename(const std::filesystem::path& path, std::string& name) noexcept;
bool managed_user_layout_artifact(const std::filesystem::path& path) noexcept;
bool remove_file_exact(const std::filesystem::path& path) noexcept;

struct active_record_t {
    bool present = false;
    docking::workspace_preset_t preset = docking::workspace_preset_t::analysis;
    bool locked = false;
    std::string user_name;
};

active_record_t read_active_record(const std::filesystem::path& path) noexcept;
bool write_active_record(const std::filesystem::path& directory,
                         const std::filesystem::path& active_record,
                         docking::workspace_preset_t preset, bool locked,
                         std::string_view user_name = {}) noexcept;

struct container_payloads_t {
    std::string dock_xml;
    std::string surface_json;
};

bool extract_container(const std::vector<char>& container, record_metadata_t& metadata,
                       std::string& payload, std::string& surface) noexcept;
std::string serialize_container(docking::workspace_preset_t preset, bool locked,
    std::uint64_t generation, bool clean_shutdown,
    const layout_environment_t& environment, std::string_view registry_fingerprint,
    std::string_view payload, std::string_view surface);

bool read_file_bounded(const std::filesystem::path& path, std::vector<char>& output,
                       read_result_t& result) noexcept;
read_result_t read_layout_payload(const std::filesystem::path& path,
                                  record_metadata_t& metadata,
                                  container_payloads_t& payloads) noexcept;
bool inspect_layout_file(const std::filesystem::path& path,
                         record_metadata_t& metadata) noexcept;
bool validate_layout_file(const std::filesystem::path& path) noexcept;
read_result_t read_layout_with_backup(const layout_paths_t& paths,
                                      record_metadata_t& metadata,
                                      container_payloads_t& payloads) noexcept;

bool save_payload(const layout_paths_t& paths, docking::workspace_preset_t preset, bool locked,
                  std::uint64_t generation, bool clean_shutdown, bool skip_backup,
                  const layout_environment_t& environment, std::string_view registry_fingerprint,
                  std::string_view payload, std::string_view surface);
bool write_generation(const layout_paths_t& paths, docking::workspace_preset_t preset,
                      bool locked, std::uint64_t generation, bool clean_shutdown,
                      bool skip_backup, const layout_environment_t& environment,
                      std::string_view registry_fingerprint,
                      std::string_view payload, std::string_view surface) noexcept;
std::uint64_t committed_generation() noexcept;
std::uint64_t failed_generation() noexcept;
void note_committed_generation(std::uint64_t generation) noexcept;
void reset_generation_state() noexcept;

std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>> scan_user_catalog(
    const std::filesystem::path& user_directory, std::string_view active_user) noexcept;

}
