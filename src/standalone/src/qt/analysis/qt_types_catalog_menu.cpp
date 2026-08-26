#include "qt/analysis/qt_types_catalog_menu.hpp"

#include <QDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <set>

#include "helpers/diag_log.hpp"

#include "core/analysis/struct_dissector.hpp"
#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/disasm/pseudocode_view.hpp"
#include "core/ai/entity_evidence_handoff.hpp"
#include "core/workbench/workbench_shell_integration.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_analysis_host_hooks.hpp"
#include "qt/analysis/qt_declaration_review_dialog.hpp"
#include "qt/analysis/qt_types_catalog_view.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/widgets/aida_notice.hpp"

namespace aida::qt::analysis {

namespace {

template <typename Invoke>
void add_menu_action(aida::ui::application_ui::retained_entity_context_t& menu,
                     const char* id, bool enabled, const char* reason, Invoke invoke) {
    aida::ui::application_ui::retained_entity_action_t action;
    action.action_id = id;
    action.capability = enabled ? aida::ui::capability_state_t::available()
        : aida::ui::capability_state_t::unavailable(reason);
    action.invoke = std::move(invoke);
    menu.actions.push_back(std::move(action));
}

int send_to_dissector_recursive(const pdb_parser::struct_def_t& definition,
                                const qt_type_catalog_t* catalog,
                                std::set<std::string>& importing,
                                bool strict_validation = false,
                                const std::string& root_source_name = {}) {
    int index = struct_dissector::structure_index_by_name(definition.name);
    if (index >= 0)
        return index;
    if (!importing.insert(definition.name).second || importing.size() > 256)
        return -1;
    const auto fail = [&] {
        importing.erase(definition.name);
        return -1;
    };
    if (catalog) {
        for (const auto& member : definition.members) {
            if (member.is_pointer)
                continue;
            if (!root_source_name.empty() &&
                qt_canonical_record_name(member.type_name) ==
                    qt_canonical_record_name(root_source_name))
                continue;
            if (const auto* target = qt_catalog_record(*catalog, member.type_name)) {
                if (send_to_dissector_recursive(*target, catalog, importing,
                        strict_validation) < 0 && strict_validation)
                    return fail();
            }
        }
    }
    index = struct_dissector::create_struct(definition.name);
    if (index < 0)
        return fail();
    if (!struct_dissector::set_structure_kind(index, definition.is_union
        ? struct_dissector::structure_kind_t::union_type
        : struct_dissector::structure_kind_t::structure) && strict_validation)
        return fail();
    for (const auto& member : definition.members) {
        if (member.offset > (std::numeric_limits<std::uint32_t>::max)() ||
            member.size > (std::numeric_limits<std::uint32_t>::max)()) {
            if (strict_validation)
                return fail();
            continue;
        }
        struct_dissector::field_def_t field;
        field.name = member.name.empty() ? "field_" + std::to_string(member.offset)
            : member.name;
        field.offset = static_cast<std::uint32_t>(member.offset);
        const std::uint32_t array_count = member.is_array && member.array_count > 0
            ? static_cast<std::uint32_t>(member.array_count) : 1u;
        field.array_count = array_count;
        field.size = static_cast<std::uint32_t>(member.size == 0 ? 1 :
            (member.size % array_count == 0 ? member.size / array_count : member.size));
        field.referenced_type_name = qt_canonical_record_name(member.type_name);
        if (member.is_pointer)
            field.type = struct_dissector::field_type_t::pointer;
        else if (catalog && qt_catalog_record(*catalog, member.type_name))
            field.type = struct_dissector::field_type_t::byte_array;
        else if (member.size == 1)
            field.type = struct_dissector::field_type_t::uint8;
        else if (member.size == 2)
            field.type = struct_dissector::field_type_t::uint16;
        else if (member.size == 4)
            field.type = struct_dissector::field_type_t::uint32;
        else if (member.size == 8)
            field.type = struct_dissector::field_type_t::uint64;
        else
            field.type = struct_dissector::field_type_t::byte_array;
        field.description = member.type_name;
        if (member.bit_offset >= 0 && member.bit_size > 0) {
            field.bit_offset = static_cast<std::uint16_t>((std::min)(member.bit_offset, 65535));
            field.bit_width = static_cast<std::uint16_t>((std::min)(member.bit_size, 65535));
        }
        const int field_index = struct_dissector::add_field(index, field);
        if (field_index < 0) {
            if (strict_validation)
                return fail();
            continue;
        }
        if (catalog) {
            if (const auto* target = qt_catalog_record(*catalog, member.type_name)) {
                const int target_index = !root_source_name.empty() &&
                    qt_canonical_record_name(member.type_name) ==
                        qt_canonical_record_name(root_source_name)
                    ? index : struct_dissector::structure_index_by_name(target->name);
                if (target_index >= 0) {
                    if (!struct_dissector::set_field_nested_target(index, field_index,
                            target_index, member.is_pointer) && strict_validation)
                        return fail();
                } else if (strict_validation && !member.is_pointer) {
                    return fail();
                }
            } else if (const auto* source_enum = qt_catalog_enum(*catalog, member.type_name)) {
                struct_dissector::enum_def_t enumeration;
                enumeration.name = source_enum->name;
                enumeration.values.reserve(source_enum->members.size());
                for (const auto& value : source_enum->members)
                    enumeration.values.push_back({value.name, value.value});
                if (!struct_dissector::upsert_enum(enumeration) && strict_validation)
                    return fail();
                if (!struct_dissector::set_field_enum_reference(index, field_index,
                        qt_canonical_record_name(member.type_name)) && strict_validation)
                    return fail();
            }
        }
    }
    importing.erase(definition.name);
    return index;
}

bool send_to_dissector(const pdb_parser::struct_def_t& definition,
                       const qt_type_catalog_t* catalog = nullptr,
                       bool* rollback_complete = nullptr) {
    if (rollback_complete)
        *rollback_complete = true;
    if (!struct_dissector::catalog_mutation_available())
        return false;
    auto rollback_state = struct_dissector::capture_catalog_transaction();
    std::set<std::string> importing;
    const int imported = send_to_dissector_recursive(definition, catalog, importing, true);
    if (imported >= 0) {
        const auto imported_revision = struct_dissector::catalog_schema_revision();
        return struct_dissector::request_save_schema_transactional(
            std::move(rollback_state), imported_revision);
    }
    const bool rolled_back = struct_dissector::rollback_catalog_transaction(
        std::move(rollback_state), struct_dissector::catalog_schema_revision());
    if (rollback_complete)
        *rollback_complete = rolled_back;
    return false;
}

}

void open_type_declaration_review(QtWorkspaceContext* context, std::string name,
                                  std::string declaration, QWidget* parent) {
    if (!context) return;
    const auto workspace = context->workspace().lock();
    const auto capture = disasm_view::capture_workspace(workspace);
    if (!capture.workspace || !capture.publication) return;
    const auto generation = workspace->generation();
    const auto analysis_revision = capture.publication->analysis_revision;
    const auto overlay_revision = workspace->overlay_revision();
    auto* dialog = new QtDeclarationReviewDialog(workspace, capture.publication,
        generation, analysis_revision, overlay_revision,
        QStringLiteral("Review Global Type Declaration"),
        QStringLiteral("Global declaration: %1").arg(QString::fromStdString(name)),
        std::move(declaration), QStringLiteral("Commit to Overlay"),
        [workspace](const std::string& committed) {
            const auto context = disasm_view::capture_workspace(workspace);
            return context && disasm_view::queue_type_declaration(context, committed);
        }, parent);
    dialog->open();
}

void show_types_catalog_menu(QtTypesCatalogView* view, QWidget* parent,
                             const QPoint& global_pos, int view_row) {
    if (!view) return;
    auto* context = QtAnalysisBridge::instance().activeContext();
    if (!context) return;
    const auto workspace = context->workspace().lock();
    const auto capture = disasm_view::capture_workspace(workspace);
    if (!capture.workspace || !capture.publication) return;
    auto state = context->typesHubState;
    std::shared_ptr<const qt_type_catalog_t> catalog_handle;
    std::shared_ptr<const std::vector<std::size_t>> visible_handle;
    int row = view_row;
    qt_types_tab_t tab = qt_types_tab_t::structures;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        catalog_handle = state->catalog;
        visible_handle = state->visible_indices;
        tab = state->active;
        state->context_row = row;
        state->context_tab = tab;
        state->context_generation = capture.publication->generation;
        state->context_analysis_revision = capture.publication->analysis_revision;
    }
    if (!catalog_handle || !visible_handle || row < 0 ||
        static_cast<std::size_t>(row) >= visible_handle->size())
        return;
    const auto& catalog = *catalog_handle;
    const std::size_t index = (*visible_handle)[static_cast<std::size_t>(row)];

    aida::ui::application_ui::retained_entity_context_t retained;
    retained.owner_id = "types.catalog.entity";
    retained.entity_generation = capture.publication->generation;
    const auto publication = capture.publication;
    const auto workspace_id = workspace->identity().binary_id().to_hex();
    const auto workspace_generation = workspace->generation();
    const auto analysis_revision = publication->analysis_revision;
    retained.validate_identity = [publication, workspace, catalog_handle,
        visible_handle, index, row, tab, analysis_revision, workspace_generation,
        workspace_id, state] {
        const auto selected = disasm_view::capture_selected_workspace();
        if (!publication || !workspace || workspace->closing() || workspace->closed() ||
            workspace->generation() != workspace_generation ||
            publication->generation != workspace_generation ||
            publication->analysis_revision != analysis_revision ||
            !selected.workspace || !selected.publication ||
            selected.workspace != workspace || selected.publication != publication ||
            selected.workspace->identity().binary_id().to_hex() != workspace_id ||
            selected.workspace->generation() != workspace_generation ||
            selected.publication->analysis_revision != analysis_revision)
            return aida::ui::capability_state_t::unavailable(
                "The analysis workspace changed; select the type again");
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->context_generation != publication->generation ||
            state->context_analysis_revision != publication->analysis_revision ||
            state->catalog_generation != state->context_generation ||
            state->catalog_analysis_revision != state->context_analysis_revision ||
            state->context_row != row || state->context_tab != tab ||
            state->catalog != catalog_handle ||
            state->visible_indices != visible_handle ||
            !visible_handle || static_cast<std::size_t>(row) >= visible_handle->size() ||
            (*visible_handle)[static_cast<std::size_t>(row)] != index)
            return aida::ui::capability_state_t::unavailable(
                "The type catalog publication or filtered row changed; select the type again");
        return aida::ui::capability_state_t::available();
    };
    const auto add = [&retained](const char* id, bool enabled, const char* reason,
                                 auto invoke) {
        add_menu_action(retained, id, enabled, reason, std::move(invoke));
    };
    auto& bridge = QtAnalysisBridge::instance();

    if (tab == qt_types_tab_t::structures || tab == qt_types_tab_t::unions_) {
        const auto& entry = tab == qt_types_tab_t::structures
            ? catalog.structs[index] : catalog.unions[index];
        retained.entity_id = (tab == qt_types_tab_t::structures ? "struct:" : "union:") +
            entry.module + ":" + entry.definition.name;
        const auto definition = entry.definition;
        const std::string name = definition.name;
        add("types.catalog.copy_name", true, "", [name] {
            clipboard::set_text(QString::fromStdString(name));
            return aida::ui::action_handler_result_t::completed();
        });
        const std::string ida_declaration = qt_struct_to_ida_syntax(definition);
        add("types.catalog.copy_ida_declaration",
            !ida_declaration.empty() && ida_declaration.size() <= 64U * 1024U,
            "The IDA declaration exceeds the bounded 64 KiB export limit",
            [ida_declaration] {
                clipboard::set_text(QString::fromStdString(ida_declaration));
                return aida::ui::action_handler_result_t::completed();
            });
        const std::string c_declaration = qt_struct_to_c_bounded(definition);
        const bool mutable_persistence_available =
            !struct_dissector::g_state.persistence_in_flight.load(
                std::memory_order_acquire);
        add("types.catalog.export_declaration",
            !c_declaration.empty() && c_declaration.size() <= 64U * 1024U,
            "The declaration is empty or exceeds the bounded 64 KiB export limit",
            [c_declaration] {
                clipboard::set_text(QString::fromStdString(c_declaration));
                return aida::ui::action_handler_result_t::completed();
            });
        add("types.catalog.duplicate_to_editor", mutable_persistence_available &&
            definition.members.size() <= 65536,
            !mutable_persistence_available ? "Another mutable catalog operation is running"
                : "The type exceeds the mutable editor's bounded member limit",
            [definition, catalog_handle, state] {
                pdb_parser::struct_def_t duplicate = definition;
                auto rollback_state = struct_dissector::capture_catalog_transaction();
                std::string base = definition.name.empty()
                    ? "anonymous_copy" : definition.name + "_copy";
                if (base.size() > 240) base.resize(240);
                {
                    auto& editor = struct_dissector::g_state;
                    std::lock_guard<std::mutex> lock(editor.mtx);
                    if (editor.persistence_in_flight.load(std::memory_order_acquire))
                        return aida::ui::action_handler_result_t::failed(
                            "Another mutable catalog operation started before duplication");
                    std::string candidate = base;
                    for (std::size_t suffix = 2; suffix <= 1024 &&
                        std::any_of(editor.structs.begin(), editor.structs.end(),
                            [&](const auto& item) { return item.name == candidate; });
                        ++suffix)
                        candidate = base + "_" + std::to_string(suffix);
                    if (std::any_of(editor.structs.begin(), editor.structs.end(),
                        [&](const auto& item) { return item.name == candidate; }))
                        return aida::ui::action_handler_result_t::failed(
                            "A bounded unique duplicate name could not be allocated");
                    duplicate.name = std::move(candidate);
                }
                auto rollback = [&] {
                    return struct_dissector::rollback_catalog_transaction(
                        std::move(rollback_state),
                        struct_dissector::catalog_schema_revision());
                };
                std::set<std::string> importing;
                const int created = send_to_dissector_recursive(duplicate,
                    catalog_handle.get(), importing, true, definition.name);
                if (created < 0) {
                    if (!rollback())
                        return aida::ui::action_handler_result_t::failed(
                            "Duplicate validation failed and exact catalog rollback was blocked");
                    return aida::ui::action_handler_result_t::failed(
                        "The duplicate failed mutable-catalog validation");
                }
                {
                    auto& editor = struct_dissector::g_state;
                    std::lock_guard<std::mutex> lock(editor.mtx);
                    editor.active_struct = created;
                }
                if (!struct_dissector::request_save_schema()) {
                    if (!rollback())
                        return aida::ui::action_handler_result_t::failed(
                            "Durable save was rejected and exact catalog rollback was blocked");
                    return aida::ui::action_handler_result_t::failed(
                        "The durable catalog save was not queued; all imported dependencies were rolled back");
                }
                (void)state;
                auto& hooks = analysis_host_hooks();
                if (hooks.activate_types_subview)
                    hooks.activate_types_subview(
                        static_cast<int>(qt_types_tab_t::dissector));
                return aida::ui::action_handler_result_t::completed(
                    "Editable duplicate created; durable catalog save queued");
            });
        add("types.catalog.open_structure_editor", true, "",
            [definition, catalog_handle] {
                bool rollback_complete = true;
                if (!send_to_dissector(definition, catalog_handle.get(),
                        &rollback_complete))
                    return aida::ui::action_handler_result_t::failed(
                        rollback_complete
                            ? "The editable import failed or durable persistence was rejected; no mutation was retained"
                            : "The editable import failed and exact catalog rollback was blocked");
                auto& hooks = analysis_host_hooks();
                if (hooks.activate_types_subview)
                    hooks.activate_types_subview(
                        static_cast<int>(qt_types_tab_t::dissector));
                return aida::ui::action_handler_result_t::completed(
                    "Editable type imported; durable catalog save queued");
            });
        const bool global_promotion_available = !name.empty() && name.size() <= 256 &&
            !c_declaration.empty();
        add("types.catalog.promote_global", global_promotion_available,
            name.empty() || name.size() > 256
                ? "The type name is empty or exceeds the bounded 256-byte review identity"
                : "The declaration is empty or exceeds the bounded 64 KiB review limit",
            [name, c_declaration, context] {
                open_type_declaration_review(context, name, c_declaration, nullptr);
                return aida::ui::action_handler_result_t::completed();
            });
        add("types.catalog.rename", false,
            "PDB catalog definitions are immutable; declare an edited overlay type instead",
            [] { return aida::ui::action_handler_result_t::completed(); });
        add("types.catalog.delete", false,
            "PDB catalog definitions cannot be deleted from the analysis workspace",
            [] { return aida::ui::action_handler_result_t::completed(); });
        aida::automation_ui::entity_evidence::snapshot_t evidence;
        evidence.workspace_id = workspace_id;
        evidence.source_view_id = definition.is_union
            ? "view.types.unions" : "view.types.structures";
        evidence.source_kind = definition.is_union ? "type_union" : "type_structure";
        evidence.entity_id = retained.entity_id;
        evidence.display_label =
            (definition.is_union ? "union " : "struct ") + definition.name;
        evidence.excerpt = c_declaration;
        evidence.revision = analysis_revision;
        evidence.generation = retained.entity_generation;
        aida::automation_ui::entity_evidence::append_actions(retained,
            std::move(evidence), c_declaration.empty()
                ? aida::ui::capability_state_t::unavailable(
                    "The retained type has no bounded declaration evidence")
                : aida::ui::capability_state_t::available());
    } else if (tab == qt_types_tab_t::enums) {
        const auto& entry = catalog.enums[index];
        retained.entity_id = "enum:" + entry.module + ":" + entry.definition.name;
        const auto definition = entry.definition;
        const std::string name = definition.name;
        add("types.catalog.copy_name", true, "", [name] {
            clipboard::set_text(QString::fromStdString(name));
            return aida::ui::action_handler_result_t::completed();
        });
        const std::string enum_declaration = qt_enum_to_c(definition);
        add("types.catalog.edit_enum", true, "", [definition] {
            struct_dissector::enum_def_t editable;
            bool existing = false;
            {
                auto& editor = struct_dissector::g_state;
                std::lock_guard<std::mutex> lock(editor.mtx);
                const auto found = std::find_if(editor.enums.begin(), editor.enums.end(),
                    [&](const auto& item) { return item.name == definition.name; });
                if (found != editor.enums.end()) {
                    editable = *found;
                    existing = true;
                }
            }
            if (!existing) {
                if (!struct_dissector::catalog_mutation_available())
                    return aida::ui::action_handler_result_t::failed(
                        "Another mutable catalog operation is running");
                auto rollback_state = struct_dissector::capture_catalog_transaction();
                const auto import_base_revision = rollback_state.schema_revision;
                editable.name = definition.name;
                editable.underlying_type = struct_dissector::field_type_t::int64;
                editable.values.reserve(definition.members.size());
                for (const auto& member : definition.members)
                    editable.values.push_back({member.name, member.value});
                if (!struct_dissector::upsert_enum(editable)) {
                    if (!struct_dissector::rollback_catalog_transaction(
                            std::move(rollback_state), import_base_revision))
                        return aida::ui::action_handler_result_t::failed(
                            "The enum import failed and exact catalog rollback was blocked");
                    return aida::ui::action_handler_result_t::failed(
                        "The enum could not be imported; no catalog mutation was retained");
                }
                const auto imported_revision = struct_dissector::catalog_schema_revision();
                if (!struct_dissector::request_save_schema_transactional(
                        std::move(rollback_state), imported_revision)) {
                    return aida::ui::action_handler_result_t::failed(
                        "Durable enum import was rejected; the mutable catalog was restored");
                }
                auto& editor = struct_dissector::g_state;
                std::lock_guard<std::mutex> lock(editor.mtx);
                const auto found = std::find_if(editor.enums.begin(), editor.enums.end(),
                    [&](const auto& item) { return item.name == definition.name; });
                if (found == editor.enums.end())
                    return aida::ui::action_handler_result_t::failed(
                        "The imported enum was not published to the mutable catalog");
                editable = *found;
            }
            auto& hooks = analysis_host_hooks();
            if (hooks.activate_types_subview)
                hooks.activate_types_subview(static_cast<int>(qt_types_tab_t::dissector));
            return aida::ui::action_handler_result_t::completed(
                existing
                    ? "Opened existing mutable enum unchanged; review replacements in Enum Manager"
                    : "Imported enum into the mutable catalog");
        });
        const bool mutable_persistence_available =
            !struct_dissector::g_state.persistence_in_flight.load(
                std::memory_order_acquire);
        add("types.catalog.export_declaration", !enum_declaration.empty(),
            "The enum declaration exceeds the bounded 64 KiB export limit",
            [enum_declaration] {
                clipboard::set_text(QString::fromStdString(enum_declaration));
                return aida::ui::action_handler_result_t::completed();
            });
        add("types.catalog.duplicate_to_editor", mutable_persistence_available &&
            definition.members.size() <= 65536,
            !mutable_persistence_available ? "Another mutable catalog operation is running"
                : "The enum exceeds the mutable editor's bounded value limit",
            [definition] {
                struct_dissector::enum_def_t duplicate;
                duplicate.underlying_type = struct_dissector::field_type_t::int64;
                duplicate.values.reserve(definition.members.size());
                for (const auto& member : definition.members)
                    duplicate.values.push_back({member.name, member.value});
                auto rollback_state = struct_dissector::capture_catalog_transaction();
                std::uint64_t before_revision = 0;
                {
                    auto& editor = struct_dissector::g_state;
                    std::lock_guard<std::mutex> lock(editor.mtx);
                    if (editor.persistence_in_flight.load(std::memory_order_acquire))
                        return aida::ui::action_handler_result_t::failed(
                            "Another mutable catalog operation started before duplication");
                    before_revision = editor.schema_revision;
                    std::string base = definition.name.empty() ? "anonymous_enum_copy"
                        : definition.name + "_copy";
                    if (base.size() > 240) base.resize(240);
                    duplicate.name = base;
                    for (std::size_t suffix = 2; suffix <= 4096 &&
                        std::any_of(editor.enums.begin(), editor.enums.end(),
                            [&](const auto& item) { return item.name == duplicate.name; });
                        ++suffix)
                        duplicate.name = base + "_" + std::to_string(suffix);
                    if (std::any_of(editor.enums.begin(), editor.enums.end(),
                        [&](const auto& item) { return item.name == duplicate.name; }))
                        return aida::ui::action_handler_result_t::failed(
                            "A bounded unique enum name could not be allocated");
                }
                if (!struct_dissector::upsert_enum_exact(duplicate, before_revision)) {
                    if (!struct_dissector::rollback_catalog_transaction(
                            std::move(rollback_state), before_revision))
                        return aida::ui::action_handler_result_t::failed(
                            "Enum duplication failed and exact catalog rollback was blocked");
                    return aida::ui::action_handler_result_t::failed(
                        "The enum catalog changed or duplication failed; no mutation was retained");
                }
                if (!struct_dissector::request_save_schema()) {
                    std::uint64_t created_revision = 0;
                    {
                        auto& editor = struct_dissector::g_state;
                        std::lock_guard<std::mutex> lock(editor.mtx);
                        created_revision = editor.schema_revision;
                    }
                    if (!struct_dissector::rollback_catalog_transaction(
                            std::move(rollback_state), created_revision))
                        return aida::ui::action_handler_result_t::failed(
                            "The durable save was rejected and exact enum rollback was blocked");
                    return aida::ui::action_handler_result_t::failed(
                        "The durable catalog save was not queued; the duplicate was rolled back");
                }
                auto& hooks = analysis_host_hooks();
                if (hooks.activate_types_subview)
                    hooks.activate_types_subview(
                        static_cast<int>(qt_types_tab_t::dissector));
                return aida::ui::action_handler_result_t::completed(
                    "Editable enum duplicate created; durable catalog save queued");
            });
        const bool global_enum_promotion_available = !definition.name.empty() &&
            definition.name.size() <= 256 && !enum_declaration.empty();
        add("types.catalog.promote_global", global_enum_promotion_available,
            definition.name.empty() || definition.name.size() > 256
                ? "The enum name is empty or exceeds the bounded 256-byte review identity"
                : "The enum declaration exceeds the bounded review limit",
            [definition, enum_declaration, context] {
                open_type_declaration_review(context, definition.name,
                    enum_declaration, nullptr);
                return aida::ui::action_handler_result_t::completed();
            });
        aida::automation_ui::entity_evidence::snapshot_t evidence;
        evidence.workspace_id = workspace_id;
        evidence.source_view_id = "view.types.enums";
        evidence.source_kind = "type_enum";
        evidence.entity_id = retained.entity_id;
        evidence.display_label = "enum " + definition.name;
        evidence.excerpt = enum_declaration;
        evidence.revision = analysis_revision;
        evidence.generation = retained.entity_generation;
        aida::automation_ui::entity_evidence::append_actions(retained,
            std::move(evidence), enum_declaration.empty()
                ? aida::ui::capability_state_t::unavailable(
                    "The enum declaration exceeds the bounded evidence contract")
                : aida::ui::capability_state_t::available());
    } else if (tab == qt_types_tab_t::functions) {
        const auto& entry = catalog.functions[index];
        const auto address = disasm_view::runtime_address(capture, entry.address)
            .value_or(entry.address.value);
        retained.entity_id = "function:" + std::to_string(entry.address.value) + ":" +
            entry.name;
        add("types.catalog.function.follow_disassembly", address != 0,
            "The function has no concrete address", [address, capture] {
                disasm_view::goto_address(address, capture);
                QtAnalysisBridge::instance().openView("document.disassembly");
                return aida::ui::action_handler_result_t::completed();
            });
        add("types.catalog.function.open_pseudocode", address != 0,
            "The function has no concrete address", [address, capture] {
                pseudocode_view::request_decompile(capture, address, false);
                QtAnalysisBridge::instance().openView("document.pseudocode");
                return aida::ui::action_handler_result_t::completed();
            });
        const std::string name = entry.name;
        const std::string signature = entry.signature;
        add("types.catalog.copy_name", true, "", [name] {
            clipboard::set_text(QString::fromStdString(name));
            return aida::ui::action_handler_result_t::completed();
        });
        add("types.catalog.function.copy_signature", !signature.empty(),
            "The retained function has no signature", [signature] {
                clipboard::set_text(QString::fromStdString(signature));
                return aida::ui::action_handler_result_t::completed();
            });
        add("types.catalog.function.copy_address", address != 0,
            "The function has no concrete address", [address] {
                char text[32]{};
                std::snprintf(text, sizeof(text), "0x%llX",
                    static_cast<unsigned long long>(address));
                clipboard::set_text(QString::fromLatin1(text));
                return aida::ui::action_handler_result_t::completed();
            });
        add("types.catalog.function.retype", false,
            "Use the reversible type overlay at a concrete function address",
            [] { return aida::ui::action_handler_result_t::completed(); });
        aida::automation_ui::entity_evidence::snapshot_t evidence;
        evidence.workspace_id = workspace_id;
        evidence.source_view_id = "view.types.functions";
        evidence.source_kind = "typed_function";
        evidence.entity_id = retained.entity_id;
        evidence.display_label = name;
        evidence.excerpt = signature.empty() ? name : signature;
        evidence.address = address;
        evidence.revision = analysis_revision;
        evidence.generation = retained.entity_generation;
        aida::automation_ui::entity_evidence::append_actions(retained,
            std::move(evidence));
    } else {
        const auto& entry = catalog.typedefs[index];
        retained.entity_id = (tab == qt_types_tab_t::typedefs ? "typedef:" : "inferred:") +
            entry.name + ":" + std::to_string(entry.address.value);
        const std::string name = entry.name;
        const std::string canonical = entry.canonical_type;
        add("types.catalog.copy_name", true, "", [name] {
            clipboard::set_text(QString::fromStdString(name));
            return aida::ui::action_handler_result_t::completed();
        });
        add("types.catalog.copy_canonical_type",
            !entry.explicitly_unknown && !canonical.empty(),
            "The retained type is explicitly unknown or has no canonical spelling",
            [canonical] {
                clipboard::set_text(QString::fromStdString(canonical));
                return aida::ui::action_handler_result_t::completed();
            });
        const auto address = entry.address.value != 0
            ? disasm_view::runtime_address(capture, entry.address)
                .value_or(entry.address.value) : 0;
        add("types.catalog.evidence.follow_disassembly", address != 0,
            "The retained type has no evidence address", [address, capture] {
                disasm_view::goto_address(address, capture);
                QtAnalysisBridge::instance().openView("document.disassembly");
                return aida::ui::action_handler_result_t::completed();
            });
        const std::string alias_declaration = !entry.explicitly_unknown &&
            !name.empty() && !canonical.empty()
            ? "using " + name + " = " + canonical + ";\n" : std::string{};
        add("types.catalog.export_declaration", !alias_declaration.empty() &&
            alias_declaration.size() <= 64U * 1024U,
            "The retained type cannot produce a bounded declaration", [alias_declaration] {
                clipboard::set_text(QString::fromStdString(alias_declaration));
                return aida::ui::action_handler_result_t::completed();
            });
        add("types.catalog.promote_global", !alias_declaration.empty() &&
            alias_declaration.size() <= 64U * 1024U,
            "The retained type is unknown or cannot produce a bounded declaration",
            [context, name, alias_declaration] {
                open_type_declaration_review(context, name, alias_declaration, nullptr);
                return aida::ui::action_handler_result_t::completed();
            });
        aida::automation_ui::entity_evidence::snapshot_t evidence;
        evidence.workspace_id = workspace_id;
        evidence.source_view_id = tab == qt_types_tab_t::typedefs
            ? "view.types.typedefs" : "view.types.inferred";
        evidence.source_kind = tab == qt_types_tab_t::typedefs ? "type_alias"
            : "inferred_type";
        evidence.entity_id = retained.entity_id;
        evidence.display_label = name;
        evidence.excerpt = alias_declaration.empty()
            ? name + ": unknown type evidence" : alias_declaration;
        evidence.address = address;
        evidence.revision = analysis_revision;
        evidence.generation = retained.entity_generation;
        aida::automation_ui::entity_evidence::append_actions(retained,
            std::move(evidence));
    }
    bridge.showRetainedMenu(retained, aida::ui::context_menu_open_origin_t::pointer,
        global_pos, parent);
}

}
