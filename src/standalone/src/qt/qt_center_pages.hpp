#pragma once

#include <cstddef>

namespace aida::qt {

inline constexpr const char* k_center_pages[] = {
    "aob_generator",
    "analysis_hub",
    "binary_map",
    "code_editor",
    "crypto_scanner",
    "debugger_view",
    "decrypt_oracle",
    "deobfuscation_view",
    "disassembly",
    "document.code",
    "document.diff",
    "document.disassembly",
    "document.graph",
    "document.hex",
    "document.image",
    "document.pseudocode",
    "functions_panel",
    "fuzzer_view",
    "graph_view",
    "hex_view",
    "image_view",
    "integrity_hunter",
    "memory_scanner",
    "network_view",
    "pointer_scanner",
    "pseudocode",
    "scan_hub",
    "settings_view",
    "snapshot_diff",
    "stealth_view",
    "struct_recon",
    "symbolic_view",
    "taint_view",
    "test_lab",
    "types_hub",
    "view.analysis.binary_map",
    "view.start_center",
    "view.types.struct_recon",
    "welcome",
    "workbench",
    "xref_browser",
    "xref_database",
};

inline constexpr std::size_t k_center_page_count =
    sizeof(k_center_pages) / sizeof(k_center_pages[0]);

}
