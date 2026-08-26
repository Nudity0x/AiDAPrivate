#pragma once

#include "qt/registry/qt_view_descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aida::qt::registry {

enum class catalog_owner_t : std::uint8_t {
    registry,
    task_center
};

struct catalog_entry_t {
    const char* id;
    const char* label;
    view_category_t category;
    view_presentation_role_t role;
    catalog_owner_t owner;
    hub_kind_t hub;
    int hub_subview;
    float minimum_width;
    float minimum_height;
    bool default_open;
    bool closeable;
    bool requires_workspace;
    std::uint32_t persistence_version;
    const char* persistence_alias;
    content_policy_t content_policy;
};

#define AIDA_QT_VIEW(ID, LABEL, CATEGORY, ROLE, OWNER, HUB, SUBVALUE, WIDTH, HEIGHT, OPEN, CLOSE, WORKSPACE) \
    {ID, LABEL, view_category_t::CATEGORY, view_presentation_role_t::ROLE, catalog_owner_t::OWNER, hub_kind_t::HUB, SUBVALUE, WIDTH, HEIGHT, OPEN, CLOSE, WORKSPACE, 1, nullptr, content_policy_t::lazy_dispose}
#define AIDA_QT_DEBUG_VIEW(ID, LABEL, INDEX) \
    {ID, LABEL, view_category_t::debugger, view_presentation_role_t::tool_window, catalog_owner_t::registry, hub_kind_t::debugger, INDEX, 420, 240, false, true, false, 1, nullptr, content_policy_t::lazy_dispose}
#define AIDA_QT_NETWORK_VIEW(ID, LABEL, INDEX) \
    {ID, LABEL, view_category_t::network, view_presentation_role_t::tool_window, catalog_owner_t::registry, hub_kind_t::network, INDEX, 460, 260, false, true, false, 1, nullptr, content_policy_t::lazy_dispose}

inline constexpr catalog_entry_t k_catalog[] = {
    AIDA_QT_VIEW("view.start_center", "Start Center", shell, document, registry, none, 0, 560, 360, true, true, false),
    AIDA_QT_VIEW("view.project_explorer", "Project Explorer", explorer, tool_window, registry, none, 0, 240, 220, true, true, false),
    AIDA_QT_VIEW("view.workspace_search", "Workspace Search", explorer, tool_window, registry, none, 0, 280, 220, false, true, false),
    AIDA_QT_VIEW("view.recent", "Recent", explorer, tool_window, registry, none, 0, 240, 180, false, true, false),
    AIDA_QT_VIEW("view.sessions", "Sessions", shell, shell_surface, registry, none, 0, 320, 160, true, true, false),
    AIDA_QT_VIEW("view.navigator", "Navigator", analysis, tool_window, registry, none, 0, 260, 220, false, true, true),
    AIDA_QT_VIEW("view.inspector", "Inspector", analysis, inspector, registry, none, 0, 300, 220, true, true, false),
    AIDA_QT_VIEW("view.ai_chat", "AI Chat", automation, tool_window, registry, none, 0, 420, 300, true, true, false),
    AIDA_QT_VIEW("view.output", "Output", output, bottom_panel, registry, none, 0, 360, 160, false, true, false),
    {"view.mcp_log", "MCP Activity", view_category_t::output, view_presentation_role_t::bottom_panel, catalog_owner_t::registry, hub_kind_t::none, 0, 360, 160, false, true, false, 2, "view.ai.mcp_activity", content_policy_t::lazy_dispose},
    AIDA_QT_VIEW("view.driver_log", "Driver Log", output, bottom_panel, registry, none, 0, 360, 160, false, true, false),
    AIDA_QT_VIEW("view.sandbox_log", "Sandbox Log", output, bottom_panel, registry, none, 0, 360, 160, false, true, false),
    {"view.terminal", "Terminal", view_category_t::programming, view_presentation_role_t::bottom_panel, catalog_owner_t::registry, hub_kind_t::none, 0, 420, 180, false, true, false, 1, nullptr, content_policy_t::lazy_keep},
    AIDA_QT_VIEW("view.programming.outline", "Programming Outline", programming, tool_window, registry, none, 0, 280, 220, false, true, false),
    AIDA_QT_VIEW("view.programming.references", "Programming References", programming, bottom_panel, registry, none, 0, 420, 180, false, true, false),
    AIDA_QT_VIEW("view.programming.source_debug_console", "Source Debug Console", programming, bottom_panel, registry, none, 0, 440, 200, false, true, false),
    {"document.code", "Code Editor", view_category_t::programming, view_presentation_role_t::document, catalog_owner_t::registry, hub_kind_t::none, 0, 480, 300, false, true, false, 1, nullptr, content_policy_t::lazy_keep},
    AIDA_QT_VIEW("document.disassembly", "Disassembly", document, document, registry, none, 0, 480, 300, false, true, true),
    AIDA_QT_VIEW("document.hex", "Hex", document, document, registry, none, 0, 480, 300, false, true, true),
    AIDA_QT_VIEW("document.pseudocode", "Pseudocode", document, document, registry, none, 0, 480, 300, false, true, true),
    AIDA_QT_VIEW("document.graph", "Graph", document, document, registry, none, 0, 520, 340, false, true, true),
    AIDA_QT_VIEW("document.image", "Image", document, document, registry, none, 0, 480, 300, false, true, false),
    AIDA_QT_VIEW("document.diff", "Diff", document, document, registry, none, 0, 520, 300, false, true, true),
    AIDA_QT_VIEW("view.analysis.binary_map", "Binary Map", analysis, document, registry, none, 0, 520, 340, false, true, true),
    AIDA_QT_VIEW("view.analysis.functions", "Functions", analysis, tool_window, registry, none, 0, 300, 260, false, true, true),
    AIDA_QT_VIEW("view.analysis.imports", "Imports", analysis, tool_window, registry, none, 0, 360, 240, false, true, true),
    AIDA_QT_VIEW("view.analysis.exports", "Exports", analysis, tool_window, registry, none, 0, 360, 240, false, true, true),
    AIDA_QT_VIEW("view.analysis.names", "Names", analysis, tool_window, registry, none, 0, 360, 240, false, true, true),
    AIDA_QT_VIEW("view.analysis.strings", "Strings", analysis, tool_window, registry, none, 0, 400, 240, false, true, true),
    AIDA_QT_VIEW("view.analysis.segments", "Segments", analysis, tool_window, registry, none, 0, 400, 220, false, true, true),
    AIDA_QT_VIEW("view.analysis.local_types", "Local Types", analysis, tool_window, registry, none, 0, 420, 240, false, true, true),
    AIDA_QT_VIEW("view.analysis.segment_registers", "Segment Registers", analysis, tool_window, registry, none, 0, 360, 220, false, true, true),
    AIDA_QT_VIEW("view.analysis.proximity", "Proximity Browser", analysis, tool_window, registry, none, 0, 420, 260, false, true, true),
    AIDA_QT_VIEW("view.analysis.references", "Cross References", analysis, tool_window, registry, none, 0, 360, 240, false, true, true),
    AIDA_QT_VIEW("view.analysis.symbolic", "Symbolic Execution", analysis, tool_window, registry, analysis, 0, 440, 280, false, true, true),
    AIDA_QT_VIEW("view.analysis.taint", "Taint Analysis", analysis, tool_window, registry, analysis, 1, 440, 280, false, true, true),
    AIDA_QT_VIEW("view.analysis.deobfuscation", "Deobfuscation", analysis, tool_window, registry, analysis, 2, 440, 280, false, true, true),
    AIDA_QT_VIEW("view.analysis.fuzzer", "Analysis Fuzzer", analysis, tool_window, registry, analysis, 3, 440, 280, false, true, true),
    AIDA_QT_VIEW("view.analysis.protection", "Protection Analysis", analysis, tool_window, registry, analysis, 4, 440, 280, false, true, true),
    AIDA_QT_VIEW("view.memory.value_scan", "Value Scan", memory, tool_window, registry, scan, 0, 480, 280, false, true, false),
    AIDA_QT_VIEW("view.memory.value_scan_results", "Value Scan Results", memory, tool_window, registry, none, 0, 480, 240, false, true, false),
    AIDA_QT_VIEW("view.memory.address_list", "Address List", memory, tool_window, registry, none, 0, 480, 220, false, true, false),
    AIDA_QT_VIEW("view.memory.crypto", "Crypto Scanner", memory, tool_window, registry, scan, 1, 440, 260, false, true, false),
    AIDA_QT_VIEW("view.memory.aob", "AOB Generator", memory, tool_window, registry, scan, 2, 440, 260, false, true, true),
    AIDA_QT_VIEW("view.memory.decrypt", "Decrypt Oracle", memory, tool_window, registry, scan, 3, 440, 260, false, true, false),
    AIDA_QT_VIEW("view.memory.pointers", "Pointer Scanner", memory, tool_window, registry, scan, 4, 480, 280, false, true, false),
    AIDA_QT_VIEW("view.memory.snapshots", "Snapshot Diff", memory, tool_window, registry, scan, 5, 480, 280, false, true, false),
    AIDA_QT_VIEW("view.memory.integrity", "Integrity Hunter", memory, tool_window, registry, scan, 6, 480, 280, false, true, false),
    AIDA_QT_VIEW("view.types.structures", "Structures", types, tool_window, registry, types, 0, 460, 280, false, true, true),
    AIDA_QT_VIEW("view.types.unions", "Unions", types, tool_window, registry, types, 1, 420, 260, false, true, true),
    AIDA_QT_VIEW("view.types.enums", "Enums", types, tool_window, registry, types, 2, 420, 260, false, true, true),
    AIDA_QT_VIEW("view.types.typedefs", "Typedefs", types, tool_window, registry, types, 3, 420, 260, false, true, true),
    AIDA_QT_VIEW("view.types.functions", "Function Types", types, tool_window, registry, types, 4, 440, 260, false, true, true),
    AIDA_QT_VIEW("view.types.inferred", "Inferred Types", types, tool_window, registry, types, 5, 440, 260, false, true, true),
    {"view.types.dissector", "Structure Dissector", view_category_t::types, view_presentation_role_t::tool_window, catalog_owner_t::registry, hub_kind_t::types, 6, 560, 360, false, true, false, 2, "view.types.live_inspector", content_policy_t::lazy_dispose},
    AIDA_QT_VIEW("view.types.struct_recon", "Structure Reconstruction", types, document, registry, none, 0, 560, 360, false, true, false),
    AIDA_QT_VIEW("view.debug.cpu", "CPU", debugger, tool_window, registry, debugger, 0, 620, 380, false, true, false),
    AIDA_QT_VIEW("view.debug.registers", "Registers", debugger, tool_window, registry, none, 0, 380, 300, false, true, false),
    AIDA_QT_VIEW("view.debug.stack", "Stack", debugger, tool_window, registry, none, 0, 420, 240, false, true, false),
    AIDA_QT_DEBUG_VIEW("view.debug.breakpoints", "Breakpoints", 1),
    AIDA_QT_DEBUG_VIEW("view.debug.memory_map", "Memory Map", 2),
    AIDA_QT_DEBUG_VIEW("view.debug.call_stack", "Call Stack", 3),
    AIDA_QT_DEBUG_VIEW("view.debug.threads", "Threads", 4),
    AIDA_QT_DEBUG_VIEW("view.debug.watches", "Watches", 5),
    AIDA_QT_DEBUG_VIEW("view.debug.handles", "Handles", 6),
    AIDA_QT_DEBUG_VIEW("view.debug.trace", "Trace", 7),
    AIDA_QT_DEBUG_VIEW("view.debug.strings", "Debugger Strings", 8),
    AIDA_QT_DEBUG_VIEW("view.debug.bookmarks", "Bookmarks", 9),
    AIDA_QT_DEBUG_VIEW("view.debug.modules", "Modules", 10),
    AIDA_QT_DEBUG_VIEW("view.debug.patches", "Debugger Patches", 11),
    AIDA_QT_DEBUG_VIEW("view.debug.seh", "SEH Chain", 12),
    AIDA_QT_DEBUG_VIEW("view.debug.cfg", "Debugger CFG", 13),
    AIDA_QT_DEBUG_VIEW("view.debug.source", "Source / Assembly", 14),
    AIDA_QT_NETWORK_VIEW("view.network.connections", "Connections", 0),
    AIDA_QT_NETWORK_VIEW("view.network.capture", "Capture", 1),
    AIDA_QT_NETWORK_VIEW("view.network.intercept", "Intercept", 2),
    AIDA_QT_NETWORK_VIEW("view.network.proxy", "Proxy", 3),
    AIDA_QT_NETWORK_VIEW("view.network.dns", "DNS", 4),
    AIDA_QT_NETWORK_VIEW("view.network.filters", "Filters", 5),
    AIDA_QT_NETWORK_VIEW("view.network.bandwidth", "Bandwidth", 6),
    {"view.network.repeater", "Repeater", view_category_t::network, view_presentation_role_t::tool_window, catalog_owner_t::registry, hub_kind_t::network, 7, 520, 320, false, true, false, 1, nullptr, content_policy_t::lazy_dispose},
    AIDA_QT_NETWORK_VIEW("view.network.keylog", "KeyLog", 8),
    AIDA_QT_NETWORK_VIEW("view.network.pcap", "PCAP", 9),
    AIDA_QT_NETWORK_VIEW("view.network.fuzzer", "Network Fuzzer", 10),
    AIDA_QT_NETWORK_VIEW("view.network.offensive", "Offensive", 11),
    AIDA_QT_NETWORK_VIEW("view.network.websocket", "WebSocket", 12),
    AIDA_QT_NETWORK_VIEW("view.network.scripting", "Scripting", 13),
    AIDA_QT_NETWORK_VIEW("view.network.decoder", "Decoder", 14),
    AIDA_QT_NETWORK_VIEW("view.network.site_map", "Site Map", 15),
    AIDA_QT_NETWORK_VIEW("view.network.scope", "Scope", 16),
    AIDA_QT_NETWORK_VIEW("view.network.cookies", "Cookies", 17),
    AIDA_QT_NETWORK_VIEW("view.network.scanner", "Scanner", 18),
    AIDA_QT_NETWORK_VIEW("view.network.recon", "Recon", 19),
    AIDA_QT_NETWORK_VIEW("view.network.intruder", "Intruder", 20),
    AIDA_QT_NETWORK_VIEW("view.network.collaborator", "Collaborator", 21),
    AIDA_QT_NETWORK_VIEW("view.network.sequencer", "Sequencer", 22),
    AIDA_QT_NETWORK_VIEW("view.network.comparer", "Comparer", 23),
    AIDA_QT_NETWORK_VIEW("view.network.jwt_lab", "JWT Lab", 24),
    AIDA_QT_NETWORK_VIEW("view.network.match_replace", "Match and Replace", 25),
    AIDA_QT_NETWORK_VIEW("view.network.session", "Session", 26),
    {"view.network.api", "API", view_category_t::network, view_presentation_role_t::tool_window, catalog_owner_t::registry, hub_kind_t::network, 27, 520, 320, false, true, false, 1, nullptr, content_policy_t::lazy_dispose},
    AIDA_QT_VIEW("view.network.project", "Burp Project", network, tool_window, registry, none, 0, 480, 260, false, true, false),
    {"view.network.ws_editor", "WebSocket Editor", view_category_t::network, view_presentation_role_t::tool_window, catalog_owner_t::registry, hub_kind_t::network, 28, 520, 320, false, true, false, 1, nullptr, content_policy_t::lazy_dispose},
    {"view.network.h2_editor", "HTTP/2 Editor", view_category_t::network, view_presentation_role_t::tool_window, catalog_owner_t::registry, hub_kind_t::network, 29, 520, 320, false, true, false, 1, nullptr, content_policy_t::lazy_dispose},
    AIDA_QT_NETWORK_VIEW("view.network.logger", "Logger", 30),
    AIDA_QT_NETWORK_VIEW("view.network.csp", "CSP", 31),
    AIDA_QT_NETWORK_VIEW("view.network.upstream", "Upstream Proxy", 32),
    AIDA_QT_NETWORK_VIEW("view.network.browser", "Browser", 33),
    AIDA_QT_NETWORK_VIEW("view.network.reports", "Reports", 34),
    AIDA_QT_NETWORK_VIEW("view.network.headless", "Headless Browser", 35),
    AIDA_QT_VIEW("view.ai.agents", "Agents", automation, tool_window, registry, none, 0, 420, 280, false, true, false),
    AIDA_QT_VIEW("view.ai.skills", "Skills", automation, tool_window, registry, none, 0, 480, 300, false, true, false),
    AIDA_QT_VIEW("view.ai.providers", "AI Providers", automation, tool_window, registry, none, 0, 460, 300, false, true, false),
    AIDA_QT_VIEW("view.ai.mcp_marketplace", "MCP Marketplace", automation, tool_window, registry, none, 0, 600, 420, false, true, false),
    AIDA_QT_VIEW("view.ai.evidence", "Evidence Review", automation, tool_window, registry, none, 0, 420, 280, false, true, false),
    AIDA_QT_VIEW("view.ai.scripts", "Automation Scripts", automation, tool_window, registry, none, 0, 520, 300, false, true, false),
    AIDA_QT_VIEW("view.test_lab", "Test Lab", automation, tool_window, registry, none, 0, 620, 420, false, true, false),
    AIDA_QT_VIEW("view.settings", "Settings", settings, tool_window, registry, none, 0, 520, 360, false, true, false),
    AIDA_QT_VIEW("view.background_tasks", "Tasks", output, bottom_panel, task_center, none, 0, 420, 180, false, true, false),
    {"view.diagnostics", "Diagnostics", view_category_t::output, view_presentation_role_t::bottom_panel, catalog_owner_t::task_center, hub_kind_t::none, 0, 420, 180, false, true, false, 2, "view.problems", content_policy_t::lazy_dispose}
};

#undef AIDA_QT_NETWORK_VIEW
#undef AIDA_QT_DEBUG_VIEW
#undef AIDA_QT_VIEW

inline constexpr std::size_t k_catalog_size = sizeof(k_catalog) / sizeof(k_catalog[0]);
static_assert(k_catalog_size == 120, "catalog must carry 118 registry views plus 2 task_center views");

constexpr const catalog_entry_t* find_catalog_entry(std::string_view id) noexcept {
    for (const auto& entry : k_catalog)
        if (id == entry.id)
            return &entry;
    return nullptr;
}

constexpr std::size_t hub_member_count(hub_kind_t hub) noexcept {
    std::size_t count = 0;
    for (const auto& entry : k_catalog)
        if (entry.hub == hub)
            ++count;
    return count;
}

constexpr bool hub_membership_valid() noexcept {
    for (const auto& entry : k_catalog) {
        if (entry.hub == hub_kind_t::none) {
            if (entry.hub_subview != 0)
                return false;
            continue;
        }
        if (entry.hub_subview < 0)
            return false;
        if (entry.role != view_presentation_role_t::tool_window)
            return false;
        if (entry.default_open)
            return false;
        if (std::string_view(entry.id).compare(0, 9, "document.") == 0)
            return false;
        for (const auto& other : k_catalog) {
            if (&other == &entry)
                continue;
            if (other.hub == entry.hub && other.hub_subview == entry.hub_subview)
                return false;
        }
    }
    return true;
}
static_assert(hub_membership_valid(), "hub subview membership must be unique and well-formed");

constexpr bool hub_subviews_dense(hub_kind_t hub) noexcept {
    const std::size_t count = hub_member_count(hub);
    for (std::size_t index = 0; index < count; ++index) {
        bool found = false;
        for (const auto& entry : k_catalog)
            if (entry.hub == hub && entry.hub_subview == static_cast<int>(index))
                found = true;
        if (!found)
            return false;
    }
    return true;
}
static_assert(hub_subviews_dense(hub_kind_t::analysis), "analysis hub subviews must be dense");
static_assert(hub_subviews_dense(hub_kind_t::scan), "scan hub subviews must be dense");
static_assert(hub_subviews_dense(hub_kind_t::types), "types hub subviews must be dense");
static_assert(hub_subviews_dense(hub_kind_t::debugger), "debugger hub subviews must be dense");
static_assert(hub_subviews_dense(hub_kind_t::network), "network hub subviews must be dense");

}
