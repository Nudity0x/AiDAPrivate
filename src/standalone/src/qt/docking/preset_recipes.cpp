#include "qt/docking/preset_recipes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace aida::qt::docking {

namespace {

constexpr std::array<workspace_preset_descriptor_t, 8> kPresetDescriptors{{
    {workspace_preset_t::analysis, "analysis", "Analysis", "Disassembly, pseudocode, graph, symbols, references and inspection", 3},
    {workspace_preset_t::debugging, "debugging", "Debugging", "Execution controls, CPU, registers, breakpoints, threads, stack and trace", 5},
    {workspace_preset_t::memory, "memory", "Memory", "Process sessions, scans, results, memory map, hex, pointers and patches", 5},
    {workspace_preset_t::types_structures, "types-structures", "Types and Structures", "Type catalogs, structure layouts, live values and propagation", 3},
    {workspace_preset_t::network, "network", "Network", "Proxy history, repeater, browser, protocol streams and evidence", 3},
    {workspace_preset_t::automation_ai, "automation-ai", "Automation and AI", "Chat, agents, skills, MCP activity, evidence review and tasks", 4},
    {workspace_preset_t::programming, "programming", "Programming", "Project explorer, source editing, search, terminal, problems and debugging", 4},
    {workspace_preset_t::safe, "safe", "Safe Layout", "Recovery workspace with Start Center, diagnostics and essential navigation", 3}
}};

const preset_recipe_t kAnalysisRecipe = {
    {"view.project_explorer", "view.sessions", "view.navigator", "view.analysis.functions",
        "view.analysis.imports", "view.analysis.exports", "view.analysis.names",
        "view.analysis.strings", "view.analysis.segments", "view.analysis.local_types",
        "view.analysis.segment_registers", "view.analysis.proximity"},
    {"view.start_center", "document.disassembly", "document.pseudocode", "document.graph",
        "document.hex", "document.code", "view.analysis.binary_map"},
    {"view.inspector", "view.analysis.references", "view.ai_chat"},
    {"view.output", "view.background_tasks", "view.diagnostics"},
    "view.analysis.functions", "document.disassembly", "view.inspector", "view.output"
};

const preset_recipe_t kDebuggingRecipe = {
    {"view.sessions", "view.debug.threads", "view.debug.modules", "view.debug.call_stack"},
    {"view.start_center", "view.debug.cpu", "view.debug.source", "document.code",
        "document.hex", "view.debug.cfg"},
    {"view.debug.registers", "view.debug.breakpoints", "view.debug.watches",
        "view.debug.strings", "view.debug.bookmarks"},
    {"view.debug.stack", "view.debug.memory_map", "view.debug.trace", "view.debug.patches",
        "view.debug.seh", "view.debug.handles", "view.terminal", "view.background_tasks",
        "view.diagnostics"},
    "view.debug.threads", "view.debug.cpu", "view.debug.registers", "view.debug.stack"
};

const preset_recipe_t kMemoryRecipe = {
    {"view.sessions", "view.memory.value_scan", "view.memory.crypto", "view.memory.aob",
        "view.memory.decrypt", "view.memory.integrity"},
    {"view.start_center", "document.hex", "view.memory.value_scan_results",
        "view.memory.pointers", "view.memory.snapshots"},
    {"view.debug.memory_map", "view.types.dissector"},
    {"view.memory.address_list", "view.debug.patches", "view.debug.watches",
        "view.background_tasks", "view.diagnostics"},
    "view.memory.value_scan", "document.hex", "view.debug.memory_map", "view.memory.address_list"
};

const preset_recipe_t kTypesRecipe = {
    {"view.sessions", "view.types.structures", "view.types.unions", "view.types.enums",
        "view.types.typedefs", "view.types.functions", "view.types.inferred"},
    {"view.start_center", "view.types.struct_recon", "document.code", "document.hex"},
    {"view.types.dissector"},
    {"view.analysis.references", "view.background_tasks", "view.diagnostics"},
    "view.types.structures", "view.types.struct_recon", "view.types.dissector",
    "view.analysis.references"
};

const preset_recipe_t kNetworkRecipe = {
    {"view.sessions", "view.network.site_map", "view.network.scope", "view.network.cookies",
        "view.network.session"},
    {"view.start_center", "view.network.proxy", "view.network.intercept", "view.network.repeater",
        "view.network.browser", "view.network.api"},
    {"view.network.decoder", "view.network.comparer", "view.network.scanner",
        "view.network.reports"},
    {"view.network.capture", "view.network.logger", "view.network.websocket",
        "view.network.h2_editor", "view.background_tasks", "view.diagnostics"},
    "view.network.site_map", "view.network.proxy", "view.network.scanner", "view.network.capture"
};

const preset_recipe_t kAutomationRecipe = {
    {"view.sessions", "view.ai.agents", "view.ai.skills", "view.ai.scripts",
        "view.project_explorer"},
    {"view.start_center", "view.ai_chat", "document.code"},
    {"view.ai.evidence", "view.ai.providers", "view.ai.mcp_marketplace"},
    {"view.background_tasks", "view.mcp_log", "view.output", "view.terminal",
        "view.diagnostics"},
    "view.ai.agents", "view.ai_chat", "view.ai.evidence", "view.background_tasks"
};

const preset_recipe_t kProgrammingRecipe = {
    {"view.project_explorer", "view.programming.outline", "view.sessions",
        "view.workspace_search"},
    {"view.start_center", "document.code", "document.disassembly", "document.pseudocode"},
    {"view.inspector", "view.analysis.references", "view.ai_chat"},
    {"view.programming.source_debug_console", "view.output", "view.terminal",
        "view.programming.references", "view.background_tasks", "view.diagnostics"},
    "view.project_explorer", "document.code", "view.inspector", "view.diagnostics"
};

const preset_recipe_t kSafeRecipe = {
    {"view.project_explorer", "view.sessions", "view.recent"},
    {"view.start_center", "document.code"},
    {"view.ai_chat"},
    {"view.diagnostics", "view.output"},
    "view.project_explorer", "document.code", "view.ai_chat", "view.diagnostics"
};

}

const workspace_preset_descriptor_t* presets(std::size_t& count) noexcept {
    count = kPresetDescriptors.size();
    return kPresetDescriptors.data();
}

const workspace_preset_descriptor_t& preset_descriptor(workspace_preset_t preset) noexcept {
    for (const auto& descriptor : kPresetDescriptors) {
        if (descriptor.id == preset)
            return descriptor;
    }
    return kPresetDescriptors.front();
}

std::uint32_t preset_revision(workspace_preset_t preset) noexcept {
    return preset_descriptor(preset).revision;
}

std::string_view preset_stable_id(workspace_preset_t preset) noexcept {
    return preset_descriptor(preset).stable_id;
}

bool preset_for_stable_id(std::string_view stable_id, workspace_preset_t& preset) noexcept {
    for (const auto& descriptor : kPresetDescriptors) {
        if (stable_id == descriptor.stable_id) {
            preset = descriptor.id;
            return true;
        }
    }
    return false;
}

std::string workspace_preset_key(workspace_preset_t preset) {
    return std::string(preset_descriptor(preset).stable_id);
}

bool valid_user_layout_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ')
        return false;
    bool previous_space = false;
    for (const char character : name) {
        const auto byte = static_cast<unsigned char>(character);
        if (!(byte >= 'a' && byte <= 'z') &&
            !(byte >= 'A' && byte <= 'Z') &&
            !(byte >= '0' && byte <= '9') &&
            byte != '-' && byte != '_' && byte != ' ')
            return false;
        if (byte == ' ' && previous_space)
            return false;
        previous_space = byte == ' ';
    }
    return true;
}

std::string identity_key(workspace_preset_t preset, std::string_view user_name) {
    std::string key = user_name.empty() ? "builtin:" : "user:";
    key.append(preset_descriptor(preset).stable_id);
    if (!user_name.empty()) {
        key.push_back(':');
        constexpr char digits[] = "0123456789abcdef";
        for (const char character : user_name) {
            const auto byte = static_cast<unsigned char>(character);
            key.push_back(digits[byte >> 4U]);
            key.push_back(digits[byte & 0x0FU]);
        }
    }
    return key;
}

bool preset_default_opens_view(workspace_preset_t preset,
    std::string_view stable_view_id) noexcept {
    const auto matches = [stable_view_id](std::initializer_list<std::string_view> ids) noexcept {
        return std::find(ids.begin(), ids.end(), stable_view_id) != ids.end();
    };
    switch (preset) {
    case workspace_preset_t::analysis:
        return matches({"view.analysis.functions", "document.disassembly", "document.pseudocode",
            "document.graph", "document.hex", "view.inspector", "view.analysis.references",
            "view.ai_chat", "view.output", "view.background_tasks", "view.diagnostics"});
    case workspace_preset_t::debugging:
        return matches({"view.sessions", "view.debug.threads", "view.debug.modules",
            "view.debug.call_stack", "view.debug.cpu", "view.debug.registers", "view.debug.stack",
            "view.debug.source", "document.hex",
            "view.debug.breakpoints", "view.debug.watches", "view.debug.strings",
            "view.debug.bookmarks", "view.debug.memory_map",
            "view.debug.trace", "view.terminal", "view.background_tasks", "view.diagnostics"});
    case workspace_preset_t::memory:
        return matches({"view.sessions", "view.memory.value_scan", "view.memory.value_scan_results",
            "view.memory.address_list", "view.memory.aob",
            "view.memory.decrypt", "view.memory.integrity", "document.hex",
            "view.memory.pointers", "view.memory.snapshots",
            "view.debug.memory_map", "view.types.dissector", "view.debug.patches",
            "view.debug.watches", "view.background_tasks"});
    case workspace_preset_t::types_structures:
        return matches({"view.sessions", "view.types.structures", "view.types.unions",
            "view.types.enums", "view.types.struct_recon", "document.hex",
            "view.types.dissector", "view.analysis.references", "view.background_tasks"});
    case workspace_preset_t::network:
        return matches({"view.sessions", "view.network.site_map", "view.network.scope",
            "view.network.proxy", "view.network.repeater", "view.network.browser",
            "view.network.decoder", "view.network.comparer", "view.network.scanner",
            "view.network.capture", "view.network.logger", "view.background_tasks"});
    case workspace_preset_t::automation_ai:
        return matches({"view.sessions", "view.ai.agents", "view.ai.skills",
            "view.ai.scripts", "view.project_explorer", "view.ai_chat", "document.code",
            "view.ai.evidence", "view.ai.providers", "view.ai.mcp_marketplace",
            "view.background_tasks", "view.mcp_log", "view.output", "view.terminal",
            "view.diagnostics"});
    case workspace_preset_t::programming:
        return matches({"view.project_explorer", "view.programming.outline", "view.sessions",
            "view.workspace_search", "document.code", "document.disassembly",
            "view.inspector", "view.analysis.references", "view.ai_chat", "view.output",
            "view.terminal", "view.programming.references",
            "view.programming.source_debug_console", "view.background_tasks",
            "view.diagnostics"});
    case workspace_preset_t::safe:
        return matches({"view.project_explorer", "view.sessions", "view.recent",
            "document.code", "view.ai_chat", "view.diagnostics", "view.output"});
    }
    return false;
}

layout_ratios_t calculate_layout_ratios(
    workspace_preset_t preset, float logical_width, float logical_height,
    const std::function<float(std::initializer_list<const char*>)>& declared_minimum_width) {
    const float desired_left_ratio = preset == workspace_preset_t::memory ? 0.23f :
        preset == workspace_preset_t::automation_ai ? 0.21f :
        preset == workspace_preset_t::debugging || preset == workspace_preset_t::network ? 0.20f : 0.18f;
    const float desired_right_ratio = preset == workspace_preset_t::types_structures ? 0.26f :
        preset == workspace_preset_t::automation_ai ? 0.24f : 0.22f;
    const float desired_bottom_ratio = preset == workspace_preset_t::debugging ? 0.30f :
        preset == workspace_preset_t::network ? 0.28f :
        preset == workspace_preset_t::programming ? 0.25f :
        preset == workspace_preset_t::safe ? 0.18f : 0.24f;
    const float usable_width = (std::max)(logical_width, 1.0f);
    const float usable_height = (std::max)(logical_height, 1.0f);
    const bool compact_analysis = preset == workspace_preset_t::analysis &&
        usable_width < 1500.0f;
    const float analysis_left_minimum = declared_minimum_width(
        {"view.project_explorer", "view.navigator", "view.analysis.functions"});
    const float analysis_right_minimum = declared_minimum_width(
        {"view.inspector", "view.analysis.references", "view.ai_chat"});
    const float analysis_center_minimum = declared_minimum_width(
        {"document.disassembly", "document.pseudocode", "document.graph",
         "document.hex", "document.code", "view.analysis.binary_map"});
    const float center_target = preset == workspace_preset_t::analysis
        ? (compact_analysis ? analysis_center_minimum
            : (std::max)(640.0f, analysis_center_minimum)) : 480.0f;
    const float center_minimum = (std::min)(center_target, usable_width * 0.60f);
    const float generic_side_floor = (std::min)(180.0f, usable_width * 0.20f);
    const float left_floor = preset == workspace_preset_t::analysis
        ? (compact_analysis ? (std::min)(260.0f, analysis_left_minimum)
            : analysis_left_minimum) : generic_side_floor;
    const float right_floor = preset == workspace_preset_t::analysis
        ? (compact_analysis ? (std::min)(300.0f, analysis_right_minimum)
            : analysis_right_minimum) : generic_side_floor;
    float left_width = (std::max)(usable_width * desired_left_ratio,
        (std::min)(left_floor, usable_width * 0.28f));
    float right_width = (std::max)(usable_width * desired_right_ratio,
        (std::min)(right_floor, usable_width * 0.34f));
    const float side_budget = (std::max)(0.0f, usable_width - center_minimum);
    const float requested_sides = left_width + right_width;
    if (requested_sides > side_budget && requested_sides > 0.0f) {
        const float contraction = side_budget / requested_sides;
        left_width *= contraction;
        right_width *= contraction;
    }
    layout_ratios_t ratios;
    ratios.left = (std::clamp)(left_width / usable_width, 0.05f, 0.45f);
    ratios.right = (std::clamp)(right_width / usable_width, 0.05f, 0.55f);
    const float document_height_minimum = (std::min)(300.0f, usable_height * 0.72f);
    const float bottom_floor = (std::min)(140.0f, usable_height * 0.22f);
    const float bottom_height = (std::clamp)(usable_height * desired_bottom_ratio,
        bottom_floor, (std::max)(bottom_floor, usable_height - document_height_minimum));
    ratios.bottom = (std::clamp)(bottom_height / usable_height, 0.08f, 0.45f);
    return ratios;
}

bool compact_single_node_recipe(float logical_width, float logical_height) noexcept {
    return logical_width < 900.0f || logical_height < 520.0f;
}

const char* compact_primary_view(workspace_preset_t preset) noexcept {
    switch (preset) {
    case workspace_preset_t::analysis: return "document.disassembly";
    case workspace_preset_t::debugging: return "view.debug.cpu";
    case workspace_preset_t::memory: return "document.hex";
    case workspace_preset_t::types_structures: return "view.types.struct_recon";
    case workspace_preset_t::network: return "view.network.proxy";
    case workspace_preset_t::automation_ai: return "view.ai_chat";
    case workspace_preset_t::programming: return "document.code";
    case workspace_preset_t::safe: return "view.start_center";
    }
    return "view.start_center";
}

const preset_recipe_t& preset_recipe(workspace_preset_t preset) noexcept {
    switch (preset) {
    case workspace_preset_t::analysis: return kAnalysisRecipe;
    case workspace_preset_t::debugging: return kDebuggingRecipe;
    case workspace_preset_t::memory: return kMemoryRecipe;
    case workspace_preset_t::types_structures: return kTypesRecipe;
    case workspace_preset_t::network: return kNetworkRecipe;
    case workspace_preset_t::automation_ai: return kAutomationRecipe;
    case workspace_preset_t::programming: return kProgrammingRecipe;
    case workspace_preset_t::safe: return kSafeRecipe;
    }
    return kAnalysisRecipe;
}

}
