#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aida::qt::docking {

enum class workspace_preset_t {
    analysis,
    debugging,
    memory,
    types_structures,
    network,
    automation_ai,
    programming,
    safe
};

struct workspace_preset_descriptor_t {
    workspace_preset_t id;
    std::string_view stable_id;
    std::string_view display_name;
    std::string_view description;
    std::uint32_t revision = 1;
};

enum class workspace_identity_kind_t {
    built_in,
    user
};

struct workspace_identity_t {
    workspace_identity_kind_t kind = workspace_identity_kind_t::built_in;
    workspace_preset_t preset = workspace_preset_t::analysis;
    std::string user_name;
};

struct user_workspace_descriptor_t {
    std::string name;
    workspace_preset_t base_preset = workspace_preset_t::analysis;
    std::uint64_t generation = 0;
    bool active = false;
};

enum class workspace_request_result_t {
    completed,
    queued,
    unchanged,
    busy,
    invalid_name,
    already_exists,
    not_found,
    unavailable,
    failed
};

const workspace_preset_descriptor_t* presets(std::size_t& count) noexcept;
const workspace_preset_descriptor_t& preset_descriptor(workspace_preset_t preset) noexcept;
std::uint32_t preset_revision(workspace_preset_t preset) noexcept;
std::string_view preset_stable_id(workspace_preset_t preset) noexcept;
bool preset_for_stable_id(std::string_view stable_id, workspace_preset_t& preset) noexcept;
std::string workspace_preset_key(workspace_preset_t preset);

bool valid_user_layout_name(std::string_view name) noexcept;
std::string identity_key(workspace_preset_t preset, std::string_view user_name);

bool preset_default_opens_view(workspace_preset_t preset,
                               std::string_view stable_view_id) noexcept;

struct layout_ratios_t {
    float left = 0.18f;
    float right = 0.22f;
    float bottom = 0.24f;
};

layout_ratios_t calculate_layout_ratios(
    workspace_preset_t preset, float logical_width, float logical_height,
    const std::function<float(std::initializer_list<const char*>)>& declared_minimum_width);

bool compact_single_node_recipe(float logical_width, float logical_height) noexcept;
const char* compact_primary_view(workspace_preset_t preset) noexcept;

struct preset_recipe_t {
    std::vector<const char*> left;
    std::vector<const char*> center;
    std::vector<const char*> right;
    std::vector<const char*> bottom;
    const char* select_left = nullptr;
    const char* select_center = nullptr;
    const char* select_right = nullptr;
    const char* select_bottom = nullptr;
};

const preset_recipe_t& preset_recipe(workspace_preset_t preset) noexcept;

}
