#include "qt/workbench/qt_workbench_inspector_view.hpp"

#include <QFormLayout>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/analysis/workspace/paged_snapshot_view.hpp"
#include "core/analysis/workspace/workspace_registry.hpp"
#include "core/ai/entity_evidence_handoff.hpp"
#include "core/disasm/disasm_view.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_revision_poller.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::workbench {

namespace {

std::string hexadecimal(std::uint64_t value, unsigned minimum_digits = 0) {
    char buffer[32];
    const unsigned digits = (std::min)(minimum_digits, 16U);
    std::snprintf(buffer, sizeof(buffer), "0x%0*llX", static_cast<int>(digits),
        static_cast<unsigned long long>(value));
    return buffer;
}

const char* document_kind_label(aida::workbench::document_kind_t kind) noexcept {
    using kind_t = aida::workbench::document_kind_t;
    switch (kind) {
    case kind_t::binary: return "Binary";
    case kind_t::disassembly: return "Disassembly";
    case kind_t::hex: return "Hex";
    case kind_t::pseudocode: return "Pseudocode";
    case kind_t::graph: return "Graph";
    case kind_t::strings: return "Strings";
    case kind_t::imports: return "Imports";
    case kind_t::exports: return "Exports";
    case kind_t::functions: return "Functions";
    case kind_t::types: return "Types";
    case kind_t::diagnostics: return "Diagnostics";
    case kind_t::bookmarks: return "Bookmarks";
    case kind_t::memory: return "Memory";
    case kind_t::debugger: return "Debugger";
    case kind_t::custom: return "Custom";
    case kind_t::diff: return "Diff";
    case kind_t::unknown: return "Unknown";
    }
    return "Unknown";
}

const char* provenance_label(aida::analysis::fact_provenance_t provenance) noexcept {
    using provenance_t = aida::analysis::fact_provenance_t;
    switch (provenance) {
    case provenance_t::unknown: return "Unknown";
    case provenance_t::gap_recovery: return "Gap recovery";
    case provenance_t::linear_validation: return "Linear validation";
    case provenance_t::recursive_decode: return "Recursive decode";
    case provenance_t::relocation: return "Relocation";
    case provenance_t::call_target: return "Call target";
    case provenance_t::export_entry: return "Export metadata";
    case provenance_t::tls_entry: return "TLS metadata";
    case provenance_t::image_entry: return "Image entry";
    case provenance_t::unwind_metadata: return "Unwind metadata";
    case provenance_t::debug_symbol: return "Debug symbol";
    case provenance_t::user_definition: return "User definition";
    case provenance_t::decompiler_feedback: return "Decompiler feedback";
    }
    return "Unknown";
}

const char* operand_kind_label(aida::analysis::operand_kind_t kind) noexcept {
    using kind_t = aida::analysis::operand_kind_t;
    switch (kind) {
    case kind_t::none: return "None";
    case kind_t::reg: return "Register";
    case kind_t::memory: return "Memory";
    case kind_t::immediate: return "Immediate";
    case kind_t::pointer: return "Pointer";
    }
    return "Unknown";
}

const char* target_kind_label(aida::analysis::target_kind_record_t kind) noexcept {
    using kind_t = aida::analysis::target_kind_record_t;
    switch (kind) {
    case kind_t::branch: return "Branch";
    case kind_t::call: return "Call";
    case kind_t::data: return "Data";
    case kind_t::fallthrough: return "Fallthrough";
    }
    return "Unknown";
}

const char* coverage_label(aida::analysis::coverage_reason_t reason) noexcept {
    using reason_t = aida::analysis::coverage_reason_t;
    switch (reason) {
    case reason_t::decoded: return "Decoded";
    case reason_t::proven_data: return "Proven data";
    case reason_t::padding: return "Padding";
    case reason_t::conflict: return "Conflict";
    case reason_t::undecodable: return "Undecodable";
    case reason_t::non_executable: return "Non-executable";
    case reason_t::excluded_by_metadata: return "Excluded by metadata";
    case reason_t::pending: return "Pending";
    }
    return "Unknown";
}

std::string confidence_text(std::uint8_t confidence) {
    return std::to_string(static_cast<unsigned>(confidence)) + "%";
}

std::uint64_t presentation_address(
    const aida::analysis::workspace_identity_t& identity, std::uint64_t rva) noexcept {
    if (identity.target_kind() == aida::analysis::target_kind_t::live_snapshot &&
        identity.module() && rva <= (std::numeric_limits<std::uint64_t>::max)() -
            identity.module()->base)
        return identity.module()->base + rva;
    if (rva <= (std::numeric_limits<std::uint64_t>::max)() - identity.image_base())
        return identity.image_base() + rva;
    return rva;
}

// The inspector snapshot port (verbatim from workbench_registry_views.hpp).
struct inspector_row_t {
    std::string label;
    std::string value;
    std::string provenance;
};

struct inspector_snapshot_t {
    aida::workbench::inspector::inspector_context_t context;
    std::uint64_t analysis_generation = 0;
    std::uint64_t analysis_revision = 0;
    std::uint64_t overlay_revision = 0;
    std::string display_name;
    std::string qualified_name;
    std::string entity_kind;
    std::string document_kind;
    std::string module_name;
    std::string source_path;
    bool has_va = false;
    std::uint64_t va = 0;
    bool has_rva = false;
    std::uint64_t rva = 0;
    bool has_file_offset = false;
    std::uint64_t file_offset = 0;
    std::vector<inspector_row_t> identity;
    std::vector<inspector_row_t> location;
    std::vector<inspector_row_t> bytes;
    std::vector<inspector_row_t> operands;
    std::vector<inspector_row_t> xrefs;
    std::vector<inspector_row_t> calls;
    std::vector<inspector_row_t> stack_locals;
    std::vector<inspector_row_t> types;
    std::vector<inspector_row_t> overlays;
    std::vector<inspector_row_t> diagnostics;
    std::vector<inspector_row_t> provenance;
};

void unavailable(std::vector<inspector_row_t>& rows, const char* reason) {
    rows.push_back({"Status", "Unavailable", reason ? reason : "No provider is available."});
}

inspector_snapshot_t capture_inspector_snapshot(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    const aida::workbench::inspector::inspector_context_t& active,
    const aida::workbench::workbench_shell_workspace_context_t& shell) {
    inspector_snapshot_t output;
    output.context = active;
    output.analysis_generation = shell.analysis_generation;
    output.analysis_revision = shell.analysis_revision;
    output.overlay_revision = shell.overlay_revision;
    output.document_kind = document_kind_label(active.document.kind);
    const auto& identity = workspace->identity();
    output.display_name = identity.bin_name();
    output.qualified_name = active.selection.entity_key.empty()
        ? identity.normalized_source_path() : active.selection.entity_key;
    output.source_path = identity.normalized_source_path();
    output.module_name = identity.module() && !identity.module()->normalized_name.empty()
        ? identity.module()->normalized_name
        : (identity.normalized_member_path() ? *identity.normalized_member_path()
                                             : identity.bin_name());
    switch (active.selection.kind) {
    case aida::workbench::selection_kind_t::address: output.entity_kind = "Address"; break;
    case aida::workbench::selection_kind_t::entity: output.entity_kind = "Entity"; break;
    case aida::workbench::selection_kind_t::range: output.entity_kind = "Address range"; break;
    case aida::workbench::selection_kind_t::source: output.entity_kind = "Source location"; break;
    case aida::workbench::selection_kind_t::none: output.entity_kind = "Document"; break;
    }

    const auto image = workspace->image();
    if (active.selection.has_address) {
        const std::uint64_t address = active.selection.address;
        if (identity.target_kind() == aida::analysis::target_kind_t::live_snapshot) {
            output.has_va = true;
            output.va = address;
            if (identity.module() && address >= identity.module()->base &&
                address - identity.module()->base < identity.module()->size) {
                output.has_rva = true;
                output.rva = address - identity.module()->base;
            }
        } else if (image) {
            auto converted = image->va_to_rva(address);
            if (converted) {
                output.has_va = true;
                output.va = address;
                output.has_rva = true;
                output.rva = converted.take_value();
            } else if (address < image->image_size()) {
                output.has_rva = true;
                output.rva = address;
                auto va = image->rva_to_va(address);
                if (va) {
                    output.has_va = true;
                    output.va = va.take_value();
                }
            }
        } else {
            output.has_va = true;
            output.va = address;
        }
        if (image && output.has_rva) {
            auto offset = image->rva_to_file_offset(output.rva);
            if (offset) {
                output.has_file_offset = true;
                output.file_offset = offset.take_value();
            }
        }
    }

    const auto candidate_snapshot = workspace->snapshot();
    const bool publication_coherent = candidate_snapshot &&
        candidate_snapshot->generation == shell.analysis_generation &&
        candidate_snapshot->analysis_revision == shell.analysis_revision &&
        candidate_snapshot->overlay_revision == shell.overlay_revision;
    const auto snapshot = publication_coherent
        ? candidate_snapshot : std::shared_ptr<const aida::analysis::analysis_snapshot_t>{};
    const aida::analysis::instruction_record_t* instruction = nullptr;
    const aida::analysis::function_record_t* function = nullptr;
    const aida::analysis::symbol_record_t* symbol = nullptr;
    aida::analysis::instruction_record_t paged_instruction{};
    if (snapshot && output.has_rva) {
        const auto instruction_rows = aida::analysis::instructions_view(*snapshot);
        if (instruction_rows.resident()) {
            const auto resident = instruction_rows.resident_span();
            const auto instruction_it = std::lower_bound(resident.begin(),
                resident.end(), output.rva,
                [](const auto& candidate, std::uint64_t address) {
                    return candidate.address.value < address;
                });
            if (instruction_it != resident.end() &&
                instruction_it->address.value == output.rva)
                instruction = &*instruction_it;
        } else {
            aida::analysis::fact_page_pin_t lookup_pin;
            std::uint64_t low = 0;
            std::uint64_t high = instruction_rows.size();
            bool lookup_failed = false;
            while (low < high) {
                const std::uint64_t middle = low + (high - low) / 2ULL;
                auto row = instruction_rows.at(middle, lookup_pin);
                if (!row) {
                    lookup_failed = true;
                    break;
                }
                if (row.value()->address.value < output.rva)
                    low = middle + 1ULL;
                else
                    high = middle;
            }
            if (!lookup_failed && low < instruction_rows.size()) {
                auto row = instruction_rows.at(low, lookup_pin);
                if (row && row.value()->address.value == output.rva) {
                    paged_instruction = *row.value();
                    instruction = &paged_instruction;
                }
            }
        }
        const auto symbol_it = std::lower_bound(snapshot->symbols.begin(),
            snapshot->symbols.end(), output.rva,
            [](const auto& candidate, std::uint64_t address) {
                return candidate.address.value < address;
            });
        if (symbol_it != snapshot->symbols.end() && symbol_it->address.value == output.rva)
            symbol = &*symbol_it;
        auto function_it = std::upper_bound(snapshot->functions.begin(),
            snapshot->functions.end(), output.rva,
            [](std::uint64_t address, const auto& candidate) {
                return address < candidate.start.value;
            });
        if (function_it != snapshot->functions.begin()) {
            --function_it;
            if (function_it->start.value <= output.rva && output.rva < function_it->end.value)
                function = &*function_it;
        }
    }
    if (symbol && !symbol->name.empty()) {
        output.display_name = symbol->name;
        output.qualified_name = output.module_name + "!" + symbol->name;
        output.entity_kind = "Symbol";
    } else if (function) {
        output.display_name = "sub_" + hexadecimal(function->start.value, 8).substr(2);
        output.qualified_name = output.module_name + "!" + output.display_name;
        output.entity_kind = instruction ? "Instruction" : "Function";
    } else if (instruction) {
        output.display_name = "loc_" + hexadecimal(instruction->address.value, 8).substr(2);
        output.qualified_name = output.module_name + "!" + output.display_name;
        output.entity_kind = "Instruction";
    }

    if (instruction) {
        std::uint64_t provider_offset = 0;
        bool can_read = false;
        if (output.has_file_offset) {
            provider_offset = output.file_offset;
            can_read = true;
        } else if (identity.target_kind() == aida::analysis::target_kind_t::live_snapshot &&
                   output.has_rva) {
            provider_offset = output.rva;
            can_read = true;
        }
        if (can_read && provider_offset < workspace->provider().size()) {
            const std::uint64_t available = workspace->provider().size() - provider_offset;
            const std::uint64_t count = (std::min<std::uint64_t>)({
                instruction->length, available, 16ULL});
            auto bytes = workspace->provider().read_vector(provider_offset, count, 16,
                workspace->cancellation_token());
            if (bytes) {
                std::string text;
                char byte[4];
                for (const auto value : bytes.value()) {
                    std::snprintf(byte, sizeof(byte), "%02X", static_cast<unsigned>(value));
                    if (!text.empty()) text.push_back(' ');
                    text.append(byte);
                }
                output.bytes.push_back({"Instruction bytes", std::move(text),
                    "Read from the selected workspace byte provider."});
            }
        }
        if (output.bytes.empty())
            unavailable(output.bytes,
                "The selected instruction cannot be mapped to a readable provider offset.");

        if (snapshot) {
            const auto operand_rows = aida::analysis::operand_facts_view(*snapshot);
            aida::analysis::fact_page_pin_t operand_pin;
            if (instruction->operand_fact_begin <= operand_rows.size() &&
                instruction->operand_fact_count <= operand_rows.size() -
                    instruction->operand_fact_begin) {
                for (std::uint16_t index = 0; index < instruction->operand_fact_count;
                     ++index) {
                    auto operand_row = operand_rows.at(
                        instruction->operand_fact_begin + index, operand_pin);
                    if (!operand_row)
                        break;
                    const auto& operand = *operand_row.value();
                    std::string value = operand_kind_label(operand.kind);
                    if (operand.kind == aida::analysis::operand_kind_t::immediate ||
                        operand.kind == aida::analysis::operand_kind_t::pointer)
                        value += " " + hexadecimal(operand.immediate);
                    else if (operand.has_resolved_expression_value)
                        value += " -> " + hexadecimal(operand.resolved_expression_value);
                    if (operand.bit_width != 0)
                        value += " | " + std::to_string(operand.bit_width) + " bit";
                    output.operands.push_back({"Operand " + std::to_string(operand.operand_index),
                        std::move(value), "Canonical decoded operand fact."});
                }
            }
        }
    } else {
        unavailable(output.bytes, publication_coherent
            ? "No decoded instruction is selected."
            : "No analysis publication matches the active selection revision.");
    }
    if (output.operands.empty())
        unavailable(output.operands, publication_coherent
            ? "No decoded operand facts exist for this selection."
            : "No analysis publication matches the active selection revision.");

    if (snapshot && output.has_rva) {
        if (instruction) {
            const auto target_rows = aida::analysis::target_facts_view(*snapshot);
            aida::analysis::fact_page_pin_t target_pin;
            if (instruction->target_fact_begin <= target_rows.size() &&
                instruction->target_fact_count <= target_rows.size() -
                    instruction->target_fact_begin) {
                for (std::uint16_t index = 0; index < instruction->target_fact_count;
                     ++index) {
                    auto target_row = target_rows.at(
                        instruction->target_fact_begin + index, target_pin);
                    if (!target_row)
                        break;
                    const auto& target = *target_row.value();
                    const auto target_address = hexadecimal(
                        presentation_address(identity, target.target.value));
                    output.xrefs.push_back({"Outgoing",
                        std::string(target_kind_label(target.kind)) + " | " + target_address,
                        target.direct ? "Direct decoded target fact."
                                      : "Resolved indirect target fact."});
                    if (target.kind == aida::analysis::target_kind_record_t::call)
                        output.calls.push_back({"Calls", target_address,
                            target.direct ? "Direct decoded call target."
                                          : "Resolved indirect call target."});
                }
            }
        }
        if (!snapshot->xrefs.empty())
            output.xrefs.push_back({"Incoming", "Open with Show Xrefs (X)",
                "Incoming references are materialized by the existing cancellable Xrefs action."});
        const auto type_begin = std::lower_bound(snapshot->rich_facts.type_candidates.begin(),
            snapshot->rich_facts.type_candidates.end(), output.rva,
            [](const auto& candidate, std::uint64_t address) {
                return !candidate.address || candidate.address->value < address;
            });
        for (auto candidate = type_begin;
             candidate != snapshot->rich_facts.type_candidates.end() &&
                 candidate->address && candidate->address->value == output.rva;
             ++candidate) {
            std::string value = candidate->display_name;
            if (!candidate->canonical_type.empty())
                value += " | " + candidate->canonical_type;
            output.types.push_back({"Recovered type", std::move(value),
                "Confidence " + confidence_text(candidate->confidence)});
            if (output.types.size() >= 8)
                break;
        }
        if (instruction) {
            output.diagnostics.push_back({"Coverage", coverage_label(instruction->coverage),
                "Analysis coverage classification for the selected instruction."});
            output.provenance.push_back({"Instruction", provenance_label(instruction->provenance),
                "Confidence " + confidence_text(instruction->confidence)});
        }
        if (function)
            output.provenance.push_back({"Function", provenance_label(function->provenance),
                "Confidence " + confidence_text(function->confidence)});
        if (symbol)
            output.provenance.push_back({"Symbol", provenance_label(symbol->provenance),
                "Confidence " + confidence_text(symbol->confidence)});
    }
    if (output.xrefs.empty())
        unavailable(output.xrefs, publication_coherent
            ? "No cross-references are published for this selection."
            : "No analysis publication matches the active selection revision.");
    if (output.calls.empty())
        unavailable(output.calls, publication_coherent
            ? "No outgoing decoded call target is published inline; use Show Xrefs for incoming calls."
            : "No analysis publication matches the active selection revision.");
    unavailable(output.stack_locals,
        "No stack/local provider is registered for this selection context.");
    if (output.types.empty())
        unavailable(output.types, publication_coherent
            ? "No recovered type fact is published for this selection."
            : "No analysis publication matches the active selection revision.");
    output.overlays.push_back({"Workspace revision", std::to_string(shell.overlay_revision),
        "Selection-specific overlay enumeration is not exposed by the active Workbench adapter."});
    if (output.diagnostics.empty())
        unavailable(output.diagnostics, publication_coherent
            ? "No diagnostic fact is published for this selection."
            : "No analysis publication matches the active selection revision.");
    if (output.provenance.empty())
        unavailable(output.provenance, publication_coherent
            ? "No source-provenance fact is published for this selection."
            : "No analysis publication matches the active selection revision.");
    output.identity.push_back({"Kind", output.entity_kind,
        "Resolved from the active Workbench selection."});
    output.identity.push_back({"Document", output.document_kind,
        "Human-readable Workbench document kind."});
    output.identity.push_back({"Module", output.module_name,
        "Canonical workspace or live-module identity."});
    output.identity.push_back({"Source", output.source_path,
        "Canonical workspace source path."});
    output.location.push_back({"VA", output.has_va ? hexadecimal(output.va) : "Unavailable",
        output.has_va ? "Verified virtual address mapping."
                      : "No verified virtual address mapping exists for this selection."});
    output.location.push_back({"RVA", output.has_rva ? hexadecimal(output.rva) : "Unavailable",
        output.has_rva ? "Verified module-relative address mapping."
                       : "No verified module-relative mapping exists for this selection."});
    output.location.push_back({"File offset",
        output.has_file_offset ? hexadecimal(output.file_offset) : "Unavailable",
        output.has_file_offset ? "Verified image-to-file mapping."
                               : "The active image cannot map this selection to a file offset."});
    return output;
}

// The handoff capability fence (verbatim).
bool inspector_handoff_capability(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    const aida::workbench::workbench_shell_workspace_context_t& shell,
    const inspector_snapshot_t& snapshot, std::string& reason) {
    if (!workspace) {
        reason = "The analysis workspace is no longer available.";
        return false;
    }
    if (!shell.inspector_session) {
        reason = "The active workspace has no Inspector selection provider.";
        return false;
    }
    if (snapshot.context.workspace != shell.workspace ||
        snapshot.context.document.workspace != shell.workspace) {
        reason = "The Inspector snapshot belongs to a different workspace.";
        return false;
    }
    const auto typed_context =
        aida::workbench::inspector::validate_inspector_context(snapshot.context);
    if (!typed_context) {
        reason = "The Inspector selection failed typed context validation (code " +
            std::to_string(static_cast<unsigned>(typed_context.code)) +
            ", subject " + std::to_string(typed_context.subject) + ").";
        return false;
    }
    if (snapshot.analysis_generation != shell.analysis_generation) {
        reason = "The Inspector snapshot belongs to an older analysis generation.";
        return false;
    }
    if (snapshot.analysis_revision != shell.analysis_revision) {
        reason = "The Inspector snapshot belongs to an older analysis revision.";
        return false;
    }
    if (snapshot.overlay_revision != shell.overlay_revision) {
        reason = "The Inspector snapshot belongs to an older overlay revision.";
        return false;
    }
    const auto publication = workspace->snapshot();
    if (!publication) {
        reason = "The workspace has no published analysis snapshot.";
        return false;
    }
    if (publication->generation != snapshot.analysis_generation ||
        publication->analysis_revision != snapshot.analysis_revision) {
        reason = "Analysis changed after the Inspector snapshot was captured; select the item again.";
        return false;
    }
    if (publication->overlay_revision != snapshot.overlay_revision ||
        workspace->overlay_revision() != snapshot.overlay_revision) {
        reason = "Analysis overlays changed after the Inspector snapshot was captured; select the item again.";
        return false;
    }
    reason.clear();
    return true;
}

std::string bounded_evidence_text(const std::string& value,
    std::size_t maximum = 512U) {
    std::string output;
    output.reserve((std::min)(value.size(), maximum));
    bool previous_space = false;
    std::size_t offset = 0;
    while (offset < value.size() && output.size() < maximum) {
        const auto character = static_cast<unsigned char>(value[offset]);
        if (character < 0x80U) {
            const bool control = character < 0x20U || character == 0x7FU;
            const char emitted = control ? ' ' : static_cast<char>(character);
            ++offset;
            if (emitted != ' ') {
                output.push_back(emitted);
                previous_space = false;
                continue;
            }
            if (output.empty() || previous_space)
                continue;
            previous_space = true;
            output.push_back(' ');
            continue;
        }
        std::size_t width = 0;
        if (character >= 0xC2U && character <= 0xDFU)
            width = 2;
        else if (character >= 0xE0U && character <= 0xEFU)
            width = 3;
        else if (character >= 0xF0U && character <= 0xF4U)
            width = 4;
        bool valid = width != 0 && offset + width <= value.size();
        for (std::size_t index = 1; valid && index < width; ++index) {
            const auto continuation = static_cast<unsigned char>(value[offset + index]);
            valid = continuation >= 0x80U && continuation <= 0xBFU;
        }
        if (valid && width == 3) {
            const auto second = static_cast<unsigned char>(value[offset + 1]);
            valid = (character != 0xE0U || second >= 0xA0U) &&
                (character != 0xEDU || second <= 0x9FU);
        }
        if (valid && width == 4) {
            const auto second = static_cast<unsigned char>(value[offset + 1]);
            valid = (character != 0xF0U || second >= 0x90U) &&
                (character != 0xF4U || second <= 0x8FU);
        }
        if (valid) {
            if (output.size() + width > maximum)
                break;
            output.append(value, offset, width);
            offset += width;
        } else {
            constexpr char replacement[] = "\xEF\xBF\xBD";
            if (output.size() + 3U > maximum)
                break;
            output.append(replacement, 3U);
            ++offset;
        }
        previous_space = false;
    }
    while (!output.empty() && output.back() == ' ')
        output.pop_back();
    return output;
}

std::string evidence_json_string(const std::string& value,
    std::size_t maximum = 512U) {
    const auto bounded = bounded_evidence_text(value, maximum);
    std::string output;
    output.reserve(bounded.size() + 2U);
    output.push_back('"');
    for (const char character : bounded) {
        if (character == '"' || character == '\\')
            output.push_back('\\');
        output.push_back(character);
    }
    output.push_back('"');
    return output;
}

std::uint64_t inspector_evidence_hash(const std::string& value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1U : hash;
}

std::string inspector_evidence_excerpt(const inspector_snapshot_t& snapshot,
    bool& truncated) {
    constexpr std::size_t k_maximum_excerpt = 12U * 1024U;
    constexpr std::size_t k_maximum_rows = 48U;
    constexpr std::size_t k_maximum_scanned_rows = 96U;
    std::ostringstream stream;
    stream << "{\"schema\":\"aida.inspector.selection.v1\","
           << "\"trust\":\"untrusted_analysis_data\","
           << "\"raw_bytes_included\":false,\"source_paths_included\":false,"
           << "\"entity_kind\":" << evidence_json_string(snapshot.entity_kind, 128U)
           << ",\"document_kind\":" << evidence_json_string(snapshot.document_kind, 128U)
           << ",\"display_name\":" << evidence_json_string(snapshot.display_name)
           << ",\"module\":" << evidence_json_string(snapshot.module_name);
    if (snapshot.has_va)
        stream << ",\"va\":" << evidence_json_string(hexadecimal(snapshot.va));
    if (snapshot.has_rva)
        stream << ",\"rva\":" << evidence_json_string(hexadecimal(snapshot.rva));
    if (snapshot.has_file_offset)
        stream << ",\"file_offset\":" << evidence_json_string(hexadecimal(snapshot.file_offset));
    stream << ",\"facts\":[";
    std::size_t emitted = 0;
    std::size_t available = 0;
    std::size_t scanned = 0;
    bool row_limit_hit = false;
    const auto append_rows = [&](const char* category,
                                 const std::vector<inspector_row_t>& rows) {
        for (const auto& row : rows) {
            if (scanned++ >= k_maximum_scanned_rows) {
                row_limit_hit = true;
                break;
            }
            if (row.value.empty() || row.value == "Unavailable")
                continue;
            ++available;
            if (emitted >= k_maximum_rows || stream.tellp() >=
                static_cast<std::streampos>(k_maximum_excerpt - 1024U))
                continue;
            if (emitted++ != 0)
                stream << ',';
            stream << "{\"category\":" << evidence_json_string(category, 64U)
                   << ",\"label\":" << evidence_json_string(row.label, 128U)
                   << ",\"value\":" << evidence_json_string(row.value) << '}';
        }
    };
    append_rows("operand", snapshot.operands);
    append_rows("xref", snapshot.xrefs);
    append_rows("call", snapshot.calls);
    append_rows("type", snapshot.types);
    append_rows("provenance", snapshot.provenance);
    stream << "]}";
    auto result = stream.str();
    truncated = row_limit_hit || available > emitted;
    if (result.size() > k_maximum_excerpt) {
        truncated = true;
        result = "{\"schema\":\"aida.inspector.selection.v1\","
            "\"trust\":\"untrusted_analysis_data\",\"truncated\":true,"
            "\"raw_bytes_included\":false,\"source_paths_included\":false,"
            "\"entity_kind\":" + evidence_json_string(snapshot.entity_kind, 128U) +
            ",\"display_name\":" + evidence_json_string(snapshot.display_name) + "}";
        if (result.size() > k_maximum_excerpt)
            return {};
    }
    return result;
}

bool register_inspector_evidence(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    const aida::workbench::workbench_shell_workspace_context_t& shell,
    const inspector_snapshot_t& snapshot, std::string& evidence_id,
    std::string& reason) {
    if (!inspector_handoff_capability(workspace, shell, snapshot, reason))
        return false;
    bool truncated = false;
    const std::string excerpt = inspector_evidence_excerpt(snapshot, truncated);
    if (excerpt.empty()) {
        reason = "The Inspector selection could not be serialized as complete UTF-8 JSON within the 12 KiB evidence limit.";
        return false;
    }
    const std::uint64_t selection_hash = inspector_evidence_hash(
        std::to_string(snapshot.context.workspace.value) + ":" +
        std::to_string(static_cast<unsigned>(snapshot.context.document.kind)) + ":" +
        std::to_string(snapshot.context.document.object_id) + ":" +
        std::to_string(snapshot.context.document.variant_id) + ":" +
        snapshot.context.document.provider_key + ":" +
        std::to_string(snapshot.context.selection_generation) + ":" +
        std::to_string(static_cast<unsigned>(snapshot.context.selection.kind)) + ":" +
        std::to_string(snapshot.context.selection.address) + ":" +
        std::to_string(snapshot.context.selection.extent) + ":" +
        snapshot.context.selection.entity_key);
    const std::uint64_t provider_hash = inspector_evidence_hash(
        snapshot.context.document.provider_key);
    const std::uint64_t entity_hash = inspector_evidence_hash(
        snapshot.context.selection.entity_key);
    aida::automation_ui::evidence_envelope_t envelope;
    envelope.workspace_id = workspace->identity().binary_id().to_hex();
    envelope.source_view_id = "view.inspector";
    envelope.source_kind = "workbench_selection";
    envelope.entity_id = "selection:" + std::to_string(shell.workspace.value) + ":" +
        std::to_string(selection_hash);
    envelope.display_label = bounded_evidence_text(snapshot.display_name);
    envelope.return_target = "workbench:" + std::to_string(shell.workspace.value) +
        ":document:" + std::to_string(snapshot.context.document.object_id) +
        ":variant:" + std::to_string(snapshot.context.document.variant_id) +
        ":kind:" + std::to_string(static_cast<unsigned>(snapshot.context.document.kind)) +
        ":provider:" + std::to_string(provider_hash) +
        ":selection:" + std::to_string(snapshot.context.selection_generation) +
        ":selection-kind:" +
            std::to_string(static_cast<unsigned>(snapshot.context.selection.kind)) +
        ":entity:" + std::to_string(entity_hash) +
        (snapshot.context.selection.has_address
            ? ":address:" + hexadecimal(snapshot.context.selection.address)
            : std::string{}) +
        (snapshot.context.selection.extent != 0
            ? ":extent:" + std::to_string(snapshot.context.selection.extent)
            : std::string{});
    envelope.excerpt = excerpt;
    envelope.address = snapshot.context.selection.has_address
        ? snapshot.context.selection.address : 0;
    envelope.revision = snapshot.analysis_revision;
    envelope.generation = snapshot.analysis_generation;
    envelope.snapshot_hash = selection_hash ^ snapshot.overlay_revision;
    envelope.content_hash = inspector_evidence_hash(excerpt);
    envelope.truncated = truncated;
    envelope.sensitive = false;
    evidence_id = aida::automation_ui::register_evidence(std::move(envelope));
    if (evidence_id.empty()) {
        reason = "The bounded evidence registry rejected the Inspector selection identity.";
        return false;
    }
    reason.clear();
    return true;
}

}

QtWorkbenchInspectorView::QtWorkbenchInspectorView(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.inspector"));
    const auto& t = aida::qt::theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* controls = new QWidget(this);
    controls->setObjectName(QStringLiteral("aida.inspector.controls"));
    auto* controls_layout = new QHBoxLayout(controls);
    controls_layout->setContentsMargins(t.toolbar.padding_x, t.toolbar.padding_y,
        t.toolbar.padding_x, t.toolbar.padding_y);
    controls_layout->setSpacing(t.toolbar.group_gap);
    pin_button_ = new QToolButton(controls);
    pin_button_->setObjectName(QStringLiteral("aida.inspector.pin"));
    pin_button_->setText(QStringLiteral("Pin"));
    pin_button_->setCheckable(true);
    pin_button_->setToolTip(QStringLiteral("Pin the current Inspector snapshot"));
    follow_button_ = new QToolButton(controls);
    follow_button_->setObjectName(QStringLiteral("aida.inspector.follow"));
    follow_button_->setText(QStringLiteral("Follow"));
    follow_button_->setCheckable(true);
    follow_button_->setChecked(true);
    follow_button_->setToolTip(QStringLiteral("Follow the global selection"));
    controls_layout->addWidget(pin_button_);
    controls_layout->addWidget(follow_button_);
    mode_label_ = new QLabel(QStringLiteral("Global selection"), controls);
    mode_label_->setObjectName(QStringLiteral("aida.inspector.mode"));
    mode_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    controls_layout->addWidget(mode_label_);
    revision_label_ = new QLabel(controls);
    revision_label_->setObjectName(QStringLiteral("aida.inspector.revision"));
    revision_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    controls_layout->addWidget(revision_label_);
    controls_layout->addStretch(1);
    layout->addWidget(controls);
    stale_notice_ = new widgets::AidaNotice(QStringLiteral("Snapshot is stale"),
        QStringLiteral("The Inspector snapshot belongs to an older analysis revision."),
        widgets::AidaSemantic::Warning, this);
    stale_notice_->setVisible(false);
    layout->addWidget(stale_notice_);
    auto* identity_host = new QWidget(this);
    identity_host->setObjectName(QStringLiteral("aida.inspector.identity"));
    auto* identity_layout = new QVBoxLayout(identity_host);
    identity_layout->setContentsMargins(t.panel.padding, t.spacing.xs,
        t.panel.padding, t.spacing.xs);
    identity_layout->setSpacing(t.spacing.xxs);
    name_label_ = new QLabel(identity_host);
    name_label_->setObjectName(QStringLiteral("aida.inspector.name"));
    name_label_->setWordWrap(true);
    identity_layout->addWidget(name_label_);
    qualified_label_ = new QLabel(identity_host);
    qualified_label_->setObjectName(QStringLiteral("aida.inspector.qualified"));
    qualified_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    qualified_label_->setWordWrap(true);
    identity_layout->addWidget(qualified_label_);
    handoff_label_ = new QLabel(identity_host);
    handoff_label_->setObjectName(QStringLiteral("aida.inspector.handoff"));
    handoff_label_->setWordWrap(true);
    identity_layout->addWidget(handoff_label_);
    layout->addWidget(identity_host);
    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("aida.inspector.scroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(scroll_, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& pos) {
            showAddressMenu(scroll_->mapToGlobal(pos));
        });
    sections_host_ = new QWidget(scroll_);
    sections_host_->setObjectName(QStringLiteral("aida.inspector.sections"));
    sections_layout_ = new QVBoxLayout(sections_host_);
    sections_layout_->setContentsMargins(t.spacing.xs, t.spacing.xs,
        t.spacing.xs, t.spacing.xs);
    sections_layout_->setSpacing(t.spacing.xs);
    sections_layout_->addStretch(1);
    scroll_->setWidget(sections_host_);
    layout->addWidget(scroll_, 1);
    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.inspector.state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);

    connect(pin_button_, &QToolButton::toggled, this, [this](bool checked) {
        pinned_ = checked;
        handoff_status_.clear();
        rebuild();
    });
    connect(follow_button_, &QToolButton::toggled, this, [this](bool checked) {
        follow_selection_ = checked;
        rebuild();
    });

    auto* bridge = &analysis::QtAnalysisBridge::instance();
    connect(bridge, &analysis::QtAnalysisBridge::activeContextChanged, this,
            [this](analysis::QtWorkspaceContext* context) {
        if (context_connection_) disconnect(context_connection_);
        context_connection_ = QMetaObject::Connection();
        if (!context) {
            rebuild();
            return;
        }
        context_connection_ = connect(context->poller(),
            &analysis::QtRevisionPoller::revisionsChanged, this,
            [this](quint64, quint64, quint64, quint64) { pollSelection(); });
        pollSelection();
    });
    pollSelection();
}

void QtWorkbenchInspectorView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    pollSelection();
}

bool QtWorkbenchInspectorView::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Menu ||
            (key->key() == Qt::Key_F10 &&
                key->modifiers().testFlag(Qt::ShiftModifier))) {
            auto* widget = qobject_cast<QWidget*>(watched);
            if (widget && widget->objectName() ==
                    QLatin1String("aida.inspector.actions.button")) {
                showAddressMenu(widget->mapToGlobal(widget->rect().bottomLeft()));
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QtWorkbenchInspectorView::pollSelection() {
    rebuild();
}

void QtWorkbenchInspectorView::rebuild() {
    const auto workspace = aida::analysis::workspace_registry().selected_for_ui();
    if (!workspace) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No analysis workspace"));
        state_view_->setMessage(QStringLiteral(
            "Open and analyze a binary to use this Workbench surface."));
        state_view_->setVisible(true);
        scroll_->setVisible(false);
        name_label_->clear();
        qualified_label_->clear();
        return;
    }
    aida::workbench::workbench_shell_workspace_context_t shell;
    const auto loaded = aida::workbench::workbench_shell_runtime_t::instance()
        .workspace_context(workspace, shell);
    if (!loaded) {
        state_view_->setState(widgets::AidaStateView::State::Error);
        state_view_->setTitle(QStringLiteral("Inspector failed to load"));
        state_view_->setMessage(QStringLiteral(
            "The Workbench context for the active workspace could not be loaded (error %1).")
            .arg(static_cast<unsigned>(loaded.code)));
        state_view_->setVisible(true);
        scroll_->setVisible(false);
        name_label_->clear();
        qualified_label_->clear();
        return;
    }
    if (!shell.inspector_session) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("Nothing selected"));
        state_view_->setMessage(QStringLiteral(
            "Select an instruction, symbol, address, or document to inspect it."));
        state_view_->setVisible(true);
        scroll_->setVisible(false);
        return;
    }
    if (observed_generation_ != shell.analysis_generation) {
        observed_generation_ = shell.analysis_generation;
        pinned_ = false;
        pin_button_->setChecked(false);
    }
    const auto* active = shell.inspector_session->active_context();
    if (!active) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("Nothing selected"));
        state_view_->setMessage(QStringLiteral(
            "Select an instruction, symbol, address, or document to inspect it."));
        state_view_->setVisible(true);
        scroll_->setVisible(false);
        return;
    }
    const auto snapshot = capture_inspector_snapshot(workspace, *active, shell);
    captured_generation_ = snapshot.analysis_generation;
    captured_analysis_revision_ = snapshot.analysis_revision;
    captured_overlay_revision_ = snapshot.overlay_revision;
    const bool stale = snapshot.analysis_generation != shell.analysis_generation ||
        snapshot.analysis_revision != shell.analysis_revision ||
        snapshot.overlay_revision != shell.overlay_revision;
    stale_notice_->setVisible(stale && pinned_);
    state_view_->setVisible(false);
    scroll_->setVisible(true);
    mode_label_->setText(pinned_ ? QStringLiteral("Pinned snapshot")
        : follow_selection_ ? QStringLiteral("Global selection")
        : QStringLiteral("Selection paused"));
    revision_label_->setText(QStringLiteral("G%1 | A%2 | O%3")
        .arg(snapshot.analysis_generation).arg(snapshot.analysis_revision)
        .arg(snapshot.overlay_revision));
    name_label_->setText(QString::fromStdString(snapshot.display_name));
    qualified_label_->setVisible(
        !snapshot.qualified_name.empty() &&
        snapshot.qualified_name != snapshot.display_name);
    qualified_label_->setText(QString::fromStdString(snapshot.qualified_name));
    handoff_label_->setText(QString::fromStdString(handoff_status_));

    // Rebuild the sections (bounded small data; 07 sec. 8.2).
    QLayoutItem* item = nullptr;
    while ((item = sections_layout_->takeAt(0)) != nullptr) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    const auto add_section = [this](const char* title,
        const std::vector<inspector_row_t>& rows) {
        auto* group = new QGroupBox(QString::fromLatin1(title), sections_host_);
        group->setObjectName(QStringLiteral("aida.inspector.section"));
        group->setCheckable(false);
        auto* form = new QFormLayout(group);
        for (const auto& row : rows) {
            auto* label = new QLabel(QString::fromStdString(row.label), group);
            label->setProperty("aidaVariant", QStringLiteral("secondary"));
            auto* value = new QLabel(QString::fromStdString(row.value), group);
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            value->setWordWrap(true);
            if (row.value.rfind("0x", 0) == 0 || row.label == "Instruction bytes")
                value->setFont(aida::qt::theme::fonts::codeRegular());
            value->setToolTip(row.provenance.empty()
                ? QString::fromStdString(row.value)
                : QString::fromStdString(row.value) + QStringLiteral("\n") +
                    QString::fromStdString(row.provenance));
            if (row.value == "Unavailable")
                value->setEnabled(false);
            form->addRow(label, value);
        }
        sections_layout_->addWidget(group);
    };
    add_section("Identity", snapshot.identity);
    add_section("Location", snapshot.location);
    // The address context menu lives on the Location section (07 sec. 8.2).
    {
        auto* actions_row = new QGroupBox(QStringLiteral("Selection actions"),
            sections_host_);
        actions_row->setObjectName(QStringLiteral("aida.inspector.actions"));
        auto* form = new QFormLayout(actions_row);
        auto* actions_button = new QPushButton(QStringLiteral("Actions..."),
            actions_row);
        actions_button->setObjectName(QStringLiteral("aida.inspector.actions.button"));
        actions_button->setToolTip(QStringLiteral(
            "Open the typed selection and evidence actions (Menu or Shift+F10)"));
        connect(actions_button, &QPushButton::clicked, this,
            [this, actions_button] {
                showAddressMenu(actions_button->mapToGlobal(
                    actions_button->rect().bottomLeft()));
            });
        actions_button->installEventFilter(this);
        form->addRow(actions_button);
        auto* hint = new QLabel(QStringLiteral(
            "Right-click anywhere in the Inspector, use the Actions button, or press Menu / Shift+F10 for typed selection and evidence actions."),
            actions_row);
        hint->setWordWrap(true);
        hint->setProperty("aidaVariant", QStringLiteral("secondary"));
        form->addRow(hint);
        actions_row->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(actions_row, &QWidget::customContextMenuRequested, this,
            [this, actions_row](const QPoint& pos) {
                showAddressMenu(actions_row->mapToGlobal(pos));
            });
        sections_layout_->addWidget(actions_row);
    }
    add_section("Bytes", snapshot.bytes);
    add_section("Operands", snapshot.operands);
    add_section("Cross-references", snapshot.xrefs);
    add_section("Calls", snapshot.calls);
    add_section("Stack / Locals", snapshot.stack_locals);
    add_section("Types", snapshot.types);
    add_section("Overlays", snapshot.overlays);
    add_section("Diagnostics", snapshot.diagnostics);
    add_section("Source provenance", snapshot.provenance);
    sections_layout_->addStretch(1);
}

void QtWorkbenchInspectorView::showAddressMenu(const QPoint& global_pos) {
    const auto workspace = aida::analysis::workspace_registry().selected_for_ui();
    if (!workspace) return;
    aida::workbench::workbench_shell_workspace_context_t shell;
    if (!aida::workbench::workbench_shell_runtime_t::instance()
            .workspace_context(workspace, shell))
        return;
    const auto* active = shell.inspector_session
        ? shell.inspector_session->active_context() : nullptr;
    if (!active) return;
    const auto snapshot = capture_inspector_snapshot(workspace, *active, shell);
    const bool current_publication =
        snapshot.analysis_generation == shell.analysis_generation &&
        snapshot.analysis_revision == shell.analysis_revision &&
        snapshot.overlay_revision == shell.overlay_revision;
    const bool can_navigate = snapshot.context.selection.has_address &&
        current_publication;
    const auto disassembly_context = disasm_view::capture_selected_workspace();
    const bool can_show_xrefs = can_navigate &&
        disassembly_context.workspace == workspace &&
        disassembly_context.publication && disassembly_context.publication->snapshot;
    std::string handoff_reason;
    const bool can_handoff = inspector_handoff_capability(workspace, shell, snapshot,
        handoff_reason);
    aida::ui::application_ui::retained_entity_context_t context;
    context.owner_id = "workbench.inspector";
    context.entity_id = std::to_string(shell.workspace.value) + ":" +
        std::to_string(snapshot.context.selection_generation) + ":" +
        snapshot.context.selection.entity_key + ":" +
        std::to_string(snapshot.context.selection.address);
    context.entity_generation = snapshot.context.selection_generation;
    context.active_view = aida::ui::stable_view_id_t("view.inspector");
    const auto analysis_generation = snapshot.analysis_generation;
    const auto analysis_revision = snapshot.analysis_revision;
    const auto overlay_revision = snapshot.overlay_revision;
    context.validate_identity = [workspace, analysis_generation, analysis_revision,
        overlay_revision] {
        const auto current = aida::analysis::workspace_registry().selected_for_ui();
        if (!current || current != workspace)
            return aida::ui::capability_state_t::unavailable(
                "The retained Workbench entity belongs to an older analysis or overlay revision");
        aida::workbench::workbench_shell_workspace_context_t current_context;
        if (!aida::workbench::workbench_shell_runtime_t::instance()
                .workspace_context(current, current_context))
            return aida::ui::capability_state_t::unavailable(
                "The retained Workbench entity belongs to an older analysis or overlay revision");
        return current_context.analysis_generation == analysis_generation &&
            current_context.analysis_revision == analysis_revision &&
            current_context.overlay_revision == overlay_revision
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The retained Workbench entity belongs to an older analysis or overlay revision");
    };
    const auto add = [&context](const char* id, bool enabled, const char* reason,
                                auto invoke) {
        aida::ui::application_ui::retained_entity_action_t action;
        action.action_id = id;
        action.capability = enabled ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(reason);
        action.invoke = std::move(invoke);
        context.actions.push_back(std::move(action));
    };
    add("workbench.inspector.copy_va", snapshot.has_va,
        "The active selection has no verified virtual address mapping.",
        [va = snapshot.va] {
            clipboard::set_text(QString::fromStdString(hexadecimal(va)));
            return aida::ui::action_handler_result_t::completed();
        });
    add("workbench.inspector.copy_rva", snapshot.has_rva,
        "The active selection has no verified module-relative mapping.",
        [rva = snapshot.rva] {
            clipboard::set_text(QString::fromStdString(hexadecimal(rva)));
            return aida::ui::action_handler_result_t::completed();
        });
    add("workbench.inspector.copy_file_offset", snapshot.has_file_offset,
        "The active image cannot map this selection to a file offset.",
        [offset = snapshot.file_offset] {
            clipboard::set_text(QString::fromStdString(hexadecimal(offset)));
            return aida::ui::action_handler_result_t::completed();
        });
    const auto address = snapshot.context.selection.address;
    const auto entity_key = snapshot.context.selection.entity_key;
    add("workbench.inspector.follow_disassembly", can_navigate,
        "The pinned selection belongs to an older analysis or overlay revision.",
        [workspace, address, entity_key] {
            aida::workbench::workbench_shell_workspace_context_t context;
            if (!aida::workbench::workbench_shell_runtime_t::instance()
                    .workspace_context(workspace, context))
                return aida::ui::action_handler_result_t::failed(
                    "Workbench context is unavailable");
            aida::workbench::selection_context_t selection;
            selection.kind = aida::workbench::selection_kind_t::address;
            selection.has_address = true;
            selection.address = address;
            selection.entity_key = entity_key;
            aida::workbench::document_local_cursor_t cursor;
            cursor.has_position = true;
            cursor.position = address;
            static_cast<void>(aida::workbench::workbench_shell_runtime_t::instance()
                .navigate_document(workspace,
                    aida::workbench::document_kind_t::disassembly, std::nullopt,
                    selection, cursor, context));
            return aida::ui::action_handler_result_t::completed();
        });
    add("workbench.inspector.follow_hex", can_navigate,
        "The pinned selection belongs to an older analysis or overlay revision.",
        [workspace, address, entity_key] {
            aida::workbench::workbench_shell_workspace_context_t context;
            if (!aida::workbench::workbench_shell_runtime_t::instance()
                    .workspace_context(workspace, context))
                return aida::ui::action_handler_result_t::failed(
                    "Workbench context is unavailable");
            aida::workbench::selection_context_t selection;
            selection.kind = aida::workbench::selection_kind_t::address;
            selection.has_address = true;
            selection.address = address;
            selection.entity_key = entity_key;
            aida::workbench::document_local_cursor_t cursor;
            cursor.has_position = true;
            cursor.position = address;
            static_cast<void>(aida::workbench::workbench_shell_runtime_t::instance()
                .navigate_document(workspace, aida::workbench::document_kind_t::hex,
                    std::nullopt, selection, cursor, context));
            return aida::ui::action_handler_result_t::completed();
        });
    add("workbench.inspector.show_xrefs", can_show_xrefs, current_publication
        ? "The selected workspace has no published disassembly xref context."
        : "The pinned selection belongs to an older analysis or overlay revision.",
        [address, disassembly_context] {
            disasm_view::open_xrefs(address, disassembly_context);
            return aida::ui::action_handler_result_t::completed();
        });
    auto* view = this;
    add("workbench.inspector.send_chat", can_handoff, handoff_reason.c_str(),
        [workspace, shell, snapshot, view]() mutable {
            std::string evidence_id;
            std::string reason;
            if (register_inspector_evidence(workspace, shell, snapshot,
                    evidence_id, reason) &&
                aida::automation_ui::queue_evidence_for_chat(evidence_id, reason)) {
                view->handoff_status_ =
                    "The current Inspector selection was attached to AI Chat.";
                view->handoff_label_->setText(
                    QString::fromStdString(view->handoff_status_));
                return aida::ui::action_handler_result_t::completed();
            }
            view->handoff_status_ = reason.empty()
                ? "The Inspector selection could not be attached to AI Chat." : reason;
            view->handoff_label_->setText(
                QString::fromStdString(view->handoff_status_));
            return aida::ui::action_handler_result_t::failed(view->handoff_status_);
        });
    add("workbench.inspector.add_evidence", can_handoff, handoff_reason.c_str(),
        [workspace, shell, snapshot, view]() mutable {
            std::string evidence_id;
            std::string reason;
            if (!register_inspector_evidence(workspace, shell, snapshot,
                    evidence_id, reason)) {
                view->handoff_status_ = reason.empty()
                    ? "The Inspector selection could not be added to Evidence Review."
                    : reason;
                view->handoff_label_->setText(
                    QString::fromStdString(view->handoff_status_));
                return aida::ui::action_handler_result_t::failed(
                    view->handoff_status_);
            }
            aida::qt::analysis::QtAnalysisBridge::instance().openView(
                "view.ai.evidence");
            view->handoff_status_ =
                "The current Inspector selection was added to Evidence Review.";
            view->handoff_label_->setText(
                QString::fromStdString(view->handoff_status_));
            return aida::ui::action_handler_result_t::completed();
        });
    aida::qt::analysis::QtAnalysisBridge::instance().showRetainedMenu(context,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}
