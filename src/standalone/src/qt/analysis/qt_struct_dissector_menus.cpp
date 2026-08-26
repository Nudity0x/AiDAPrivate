#include <QPoint>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <utility>

#include "core/ai/entity_evidence_handoff.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/disasm/disasm_view.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_dissector_models.hpp"
#include "qt/analysis/qt_struct_dissector_view.hpp"
#include "qt/bridge/clipboard.hpp"

namespace aida::qt::analysis {

void open_type_declaration_review(QtWorkspaceContext* context, std::string name, std::string declaration, QWidget* parent);

namespace {

using retained_menu_t = aida::ui::application_ui::retained_entity_context_t;

void menu_add(retained_menu_t& menu, std::string id, bool enabled,
              const char* reason,
              std::function<aida::ui::action_handler_result_t()> invoke) {
    aida::ui::application_ui::retained_entity_action_t action;
    action.action_id = std::move(id);
    action.capability = enabled ? aida::ui::capability_state_t::available()
        : aida::ui::capability_state_t::unavailable(reason);
    action.invoke = std::move(invoke);
    menu.actions.push_back(std::move(action));
}

// Verbatim workspace-current revalidation for dissector retained entities.
std::function<aida::ui::capability_state_t()> make_workspace_validator(
    const disasm_view::workspace_context_t& retained_workspace,
    const std::string& workspace_id, std::uint64_t generation,
    std::uint64_t analysis_revision, std::function<bool()> entity_current,
    const char* message) {
    return [retained_workspace, workspace_id, generation, analysis_revision,
        entity_current, message] {
        const auto selected = disasm_view::capture_selected_workspace();
        const bool workspace_current = workspace_id.empty()
            ? !selected.workspace
            : retained_workspace.workspace && retained_workspace.publication &&
              selected.workspace && selected.publication &&
              selected.workspace == retained_workspace.workspace &&
              selected.publication == retained_workspace.publication &&
              selected.workspace->identity().binary_id().to_hex() == workspace_id &&
              selected.workspace->generation() == generation &&
              selected.publication->analysis_revision == analysis_revision;
        return entity_current() && workspace_current
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(message);
    };
}

}

void QtStructDissectorView::showStructureMenu(const QPoint& global_pos, int view_row) {
    auto* model = struct_model_;
    const int engine_index = model ? model->engineIndexFor(view_row) : -1;
    if (engine_index < 0) {
        operation_error_ = true;
        operation_status_ = "The requested structure no longer exists";
        return;
    }
    struct_dissector::struct_def_t snapshot;
    bool valid_structure = false;
    std::uint64_t retained_schema_revision = 0;
    {
        auto& state = struct_dissector::g_state;
        std::lock_guard<std::mutex> lock(state.mtx);
        if (struct_dissector::valid_index(engine_index, state.structs.size())) {
            snapshot = state.structs[static_cast<std::size_t>(engine_index)];
            retained_schema_revision = state.schema_revision;
            valid_structure = snapshot.stable_id != 0;
        }
    }
    if (!valid_structure) {
        operation_error_ = true;
        operation_status_ = "The requested structure no longer exists";
        return;
    }
    const std::uint64_t retained_id = snapshot.stable_id;
    const std::uint64_t retained_revision = snapshot.layout_revision;
    const auto resolve_structure = [retained_id, retained_revision,
        retained_schema_revision]() -> std::optional<int> {
        auto& state = struct_dissector::g_state;
        std::lock_guard<std::mutex> lock(state.mtx);
        const int index = struct_dissector::structure_index_by_id_locked(retained_id);
        if (state.schema_revision != retained_schema_revision ||
            !struct_dissector::valid_index(index, state.structs.size()) ||
            state.structs[static_cast<std::size_t>(index)].stable_id != retained_id ||
            state.structs[static_cast<std::size_t>(index)].layout_revision !=
                retained_revision)
            return std::nullopt;
        return index;
    };
    retained_menu_t retained;
    retained.owner_id = "types.dissector.structure";
    retained.entity_id = std::to_string(retained_id);
    retained.entity_generation = retained_revision;
    retained.active_view = aida::ui::stable_view_id_t("view.types.dissector");
    const auto structure_workspace = disasm_view::capture_selected_workspace();
    const std::string workspace_id = structure_workspace.workspace
        ? structure_workspace.workspace->identity().binary_id().to_hex() : std::string{};
    const std::uint64_t workspace_generation = structure_workspace.workspace
        ? structure_workspace.workspace->generation() : 0;
    const std::uint64_t analysis_revision = structure_workspace.publication
        ? structure_workspace.publication->analysis_revision : 0;
    retained.validate_identity = make_workspace_validator(structure_workspace,
        workspace_id, workspace_generation, analysis_revision,
        [resolve_structure] { return resolve_structure().has_value(); },
        "The structure layout or selected workspace changed; select it again");
    auto* view = this;
    menu_add(retained, "types.dissector.catalog.undo",
        struct_dissector::user_catalog_can_undo(),
        "No current catalog edit is available to undo", [view] {
            return view->applyCatalogUndo()
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(view->operationStatus());
        });
    menu_add(retained, "types.dissector.catalog.redo",
        struct_dissector::user_catalog_can_redo(),
        "No current catalog edit is available to redo", [view] {
            return view->applyCatalogRedo()
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(view->operationStatus());
        });
    menu_add(retained, "types.dissector.structure.copy_name", true,
        "The retained structure is stale", [resolve_structure] {
            const auto index = resolve_structure();
            if (!index)
                return aida::ui::action_handler_result_t::failed(
                    "The retained structure is stale");
            std::string name;
            {
                auto& state = struct_dissector::g_state;
                std::lock_guard<std::mutex> lock(state.mtx);
                if (!struct_dissector::valid_index(*index, state.structs.size()))
                    return aida::ui::action_handler_result_t::failed(
                        "The retained structure is stale");
                name = state.structs[static_cast<std::size_t>(*index)].name;
            }
            clipboard::set_text(QString::fromStdString(name));
            return aida::ui::action_handler_result_t::completed();
        });
    menu_add(retained, "types.dissector.structure.copy_declaration", true,
        "The retained structure is stale", [resolve_structure] {
            const auto index = resolve_structure();
            if (!index)
                return aida::ui::action_handler_result_t::failed(
                    "The retained structure is stale");
            const std::string declaration = struct_dissector::export_to_c(*index);
            if (declaration.empty())
                return aida::ui::action_handler_result_t::failed(
                    "The structure could not be exported");
            clipboard::set_text(QString::fromStdString(declaration));
            return aida::ui::action_handler_result_t::completed();
        });
    const std::string retained_declaration =
        struct_dissector::export_to_c(engine_index);
    const bool persistence_available =
        !struct_dissector::g_state.persistence_in_flight.load(std::memory_order_acquire);
    menu_add(retained, "types.dissector.structure.export_declaration",
        !retained_declaration.empty() && retained_declaration.size() <= 64U * 1024U,
        "The declaration is empty or exceeds the bounded 64 KiB export limit",
        [resolve_structure, retained_declaration] {
            if (!resolve_structure())
                return aida::ui::action_handler_result_t::failed(
                    "The retained structure is stale");
            clipboard::set_text(QString::fromStdString(retained_declaration));
            return aida::ui::action_handler_result_t::completed();
        });
    menu_add(retained, "types.dissector.structure.review_global_overlay",
        !workspace_id.empty() && !retained_declaration.empty() &&
            retained_declaration.size() <= 64U * 1024U,
        workspace_id.empty()
            ? "Select an analysis workspace before reviewing global propagation"
            : "The declaration is empty or exceeds the bounded review contract",
        [resolve_structure, name = snapshot.name, retained_declaration, this] {
            if (!resolve_structure())
                return aida::ui::action_handler_result_t::failed(
                    "The retained structure is stale");
            const auto context = disasm_view::capture_selected_workspace();
            if (!context.workspace || !context.publication)
                return aida::ui::action_handler_result_t::failed(
                    "The type workspace is unavailable");
            if (name.empty() || name.size() > 256 || retained_declaration.empty() ||
                retained_declaration.size() > 64U * 1024U)
                return aida::ui::action_handler_result_t::failed(
                    "The declaration is empty or exceeds the bounded 64 KiB review contract");
            open_type_declaration_review(
                QtAnalysisBridge::instance().activeContext(), name,
                retained_declaration, this);
            return aida::ui::action_handler_result_t::completed();
        });
    menu_add(retained, "types.dissector.structure.duplicate",
        persistence_available && snapshot.fields.size() <= 65536,
        !persistence_available ? "Another structure catalog operation is running"
            : "The structure exceeds the bounded mutable field limit",
        [resolve_structure, snapshot, view] {
            if (!resolve_structure())
                return aida::ui::action_handler_result_t::failed(
                    "The retained structure is stale");
            std::string base = snapshot.name + "_copy";
            if (base.size() > 240) base.resize(240);
            std::string name = base;
            {
                auto& state = struct_dissector::g_state;
                std::lock_guard<std::mutex> lock(state.mtx);
                if (state.persistence_in_flight.load(std::memory_order_acquire))
                    return aida::ui::action_handler_result_t::failed(
                        "Another structure catalog operation started before duplication");
                for (std::size_t suffix = 2; suffix <= 1024 &&
                    std::any_of(state.structs.begin(), state.structs.end(),
                        [&](const auto& item) { return item.name == name; }); ++suffix)
                    name = base + "_" + std::to_string(suffix);
                if (std::any_of(state.structs.begin(), state.structs.end(),
                    [&](const auto& item) { return item.name == name; }))
                    return aida::ui::action_handler_result_t::failed(
                        "A bounded unique structure name could not be allocated");
            }
            int created = -1;
            std::uint64_t created_id = 0;
            const bool duplicated = view->catalogEdit("Duplicate structure", [&] {
                created = struct_dissector::create_struct(name);
                if (created < 0) return false;
                {
                    auto& state = struct_dissector::g_state;
                    std::lock_guard<std::mutex> lock(state.mtx);
                    if (struct_dissector::valid_index(created, state.structs.size()))
                        created_id =
                            state.structs[static_cast<std::size_t>(created)].stable_id;
                }
                bool complete = created_id != 0 &&
                    struct_dissector::set_structure_kind(created, snapshot.kind) &&
                    struct_dissector::set_structure_packing(created, snapshot.packing) &&
                    struct_dissector::set_structure_alignment(created,
                        snapshot.explicit_alignment);
                for (const auto& source : snapshot.fields) {
                    if (!complete) break;
                    auto field = source;
                    field.stable_id = 0;
                    field.children.clear();
                    if (field.target_structure_id == snapshot.stable_id) {
                        field.target_structure_id = created_id;
                        field.pointer_target_struct = created;
                    }
                    complete = struct_dissector::add_field(created, field) >= 0;
                }
                return complete;
            });
            if (!duplicated)
                return aida::ui::action_handler_result_t::failed(
                    "The mutable catalog rejected the duplicate structure");
            {
                auto& state = struct_dissector::g_state;
                std::lock_guard<std::mutex> lock(state.mtx);
                state.active_struct = created;
            }
            view->resetFieldSelection();
            return aida::ui::action_handler_result_t::completed(
                "Structure duplicate created; durable catalog save queued");
        });
    menu_add(retained, "types.dissector.structure.configure_layout", true,
        "The retained structure is stale", [resolve_structure, view] {
            const auto index = resolve_structure();
            if (!index)
                return aida::ui::action_handler_result_t::failed(
                    "The retained structure is stale");
            {
                auto& state = struct_dissector::g_state;
                std::lock_guard<std::mutex> lock(state.mtx);
                if (!struct_dissector::valid_index(*index, state.structs.size()))
                    return aida::ui::action_handler_result_t::failed(
                        "The retained structure is stale");
                state.active_struct = *index;
            }
            view->openLayoutConfig();
            return aida::ui::action_handler_result_t::completed();
        });
    menu_add(retained, "types.dissector.structure.toggle_union", true,
        "The retained structure is stale",
        [resolve_structure, kind = snapshot.kind, view] {
            const auto index = resolve_structure();
            if (!index)
                return aida::ui::action_handler_result_t::failed(
                    "The retained structure is stale");
            const bool applied = view->catalogEdit("Convert structure kind", [&] {
                return struct_dissector::set_structure_kind(*index,
                    kind == struct_dissector::structure_kind_t::union_type
                        ? struct_dissector::structure_kind_t::structure
                        : struct_dissector::structure_kind_t::union_type);
            });
            return applied
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(
                    "The structure kind change failed validation");
        });
    menu_add(retained, "types.dissector.structure.save_catalog", persistence_available,
        "Another structure catalog operation is running", [resolve_structure] {
            if (!resolve_structure())
                return aida::ui::action_handler_result_t::failed(
                    "The retained structure is stale");
            return struct_dissector::request_save_schema()
                ? aida::ui::action_handler_result_t::completed(
                    "Structure schema save queued")
                : aida::ui::action_handler_result_t::failed(
                    "Structure schema save was not queued");
        });
    menu_add(retained, "types.dissector.structure.load_catalog", persistence_available,
        "Another structure catalog operation is running", [resolve_structure] {
            if (!resolve_structure())
                return aida::ui::action_handler_result_t::failed(
                    "The retained structure is stale");
            return struct_dissector::request_load_schema()
                ? aida::ui::action_handler_result_t::completed(
                    "Structure schema load queued")
                : aida::ui::action_handler_result_t::failed(
                    "Structure schema load was not queued");
        });
    aida::automation_ui::entity_evidence::snapshot_t evidence;
    evidence.workspace_id = !workspace_id.empty() ? workspace_id : "types.catalog";
    evidence.source_view_id = "view.types.dissector";
    evidence.source_kind = snapshot.kind == struct_dissector::structure_kind_t::union_type
        ? "editable_union" : "editable_structure";
    evidence.entity_id = retained.entity_id;
    evidence.display_label = snapshot.name;
    evidence.excerpt = retained_declaration;
    evidence.revision = snapshot.layout_revision;
    evidence.generation = workspace_generation;
    aida::automation_ui::entity_evidence::append_actions(retained,
        std::move(evidence), retained_declaration.empty()
            ? aida::ui::capability_state_t::unavailable(
                "The retained structure has no bounded declaration")
            : aida::ui::capability_state_t::available());
    QtAnalysisBridge::instance().showRetainedMenu(retained,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

void QtStructDissectorView::showFieldMenu(const QPoint& global_pos, int view_row) {
    const int target_field = view_row;
    struct_dissector::field_def_t field_snapshot;
    struct_dissector::live_value_t value_snapshot;
    bool valid_field = false;
    std::uint64_t retained_structure_id = 0;
    std::uint64_t retained_structure_revision = 0;
    std::uint64_t retained_schema_revision = 0;
    std::string retained_structure_name;
    std::size_t field_count = 0;
    std::uint64_t base_address = 0;
    {
        auto& state = struct_dissector::g_state;
        std::lock_guard<std::mutex> lock(state.mtx);
        const int active = state.active_struct;
        if (struct_dissector::valid_index(active, state.structs.size())) {
            const auto& selected = state.structs[static_cast<std::size_t>(active)];
            const auto& fields = selected.fields;
            if (struct_dissector::valid_index(target_field, fields.size())) {
                field_snapshot = fields[static_cast<std::size_t>(target_field)];
                retained_structure_id = selected.stable_id;
                retained_structure_revision = selected.layout_revision;
                retained_schema_revision = state.schema_revision;
                retained_structure_name = selected.name;
                if (static_cast<std::size_t>(target_field) < state.cached_values.size())
                    value_snapshot = state.cached_values[
                        static_cast<std::size_t>(target_field)];
                valid_field = true;
                field_count = fields.size();
                base_address = state.base_address;
            }
        }
    }
    selected_field_ = target_field;
    context_refresh_seq_ = struct_dissector::g_state.last_completed_seq.load(
        std::memory_order_acquire);
    context_base_address_ = base_address;
    context_target_pid_ = driver_bridge::attached_pid();
    diag::log_tagged_fmt("dissector", "field_ctx_open field_idx=%d", target_field);
    if (!valid_field || retained_structure_id == 0 ||
        retained_structure_revision == 0 || field_snapshot.stable_id == 0) {
        operation_error_ = true;
        operation_status_ = "The field context is stale; select it again";
        return;
    }
    const bool live_current = valid_field && driver_bridge::is_loaded() &&
        driver_bridge::attached_pid() != 0 &&
        driver_bridge::attached_pid() == context_target_pid_ &&
        base_address != 0 && base_address == context_base_address_ &&
        struct_dissector::g_state.last_completed_seq.load(std::memory_order_acquire) ==
            context_refresh_seq_;
    retained_menu_t retained;
    retained.owner_id = "types.dissector.field";
    retained.entity_id = std::to_string(retained_structure_id) + ":" +
        std::to_string(field_snapshot.stable_id);
    retained.entity_generation = retained_structure_revision;
    retained.active_view = aida::ui::stable_view_id_t("view.types.dissector");
    const auto retained_workspace = disasm_view::capture_selected_workspace();
    const std::uint64_t workspace_generation = retained_workspace.workspace
        ? retained_workspace.workspace->generation() : 0;
    const std::uint64_t analysis_revision = retained_workspace.publication
        ? retained_workspace.publication->analysis_revision : 0;
    const std::string workspace_id = retained_workspace.workspace
        ? retained_workspace.workspace->identity().binary_id().to_hex() : std::string{};
    const std::uint64_t retained_field_id = field_snapshot.stable_id;
    const auto resolve_field = [retained_structure_id, retained_structure_revision,
        retained_field_id, retained_schema_revision]()
        -> std::optional<std::pair<int, int>> {
        auto& state = struct_dissector::g_state;
        std::lock_guard<std::mutex> lock(state.mtx);
        const int structure_index =
            struct_dissector::structure_index_by_id_locked(retained_structure_id);
        if (state.schema_revision != retained_schema_revision ||
            !struct_dissector::valid_index(structure_index, state.structs.size()))
            return std::nullopt;
        const auto& structure = state.structs[static_cast<std::size_t>(structure_index)];
        if (structure.stable_id != retained_structure_id ||
            structure.layout_revision != retained_structure_revision)
            return std::nullopt;
        const auto found = std::find_if(structure.fields.begin(), structure.fields.end(),
            [retained_field_id](const auto& field) {
                return field.stable_id == retained_field_id;
            });
        if (found == structure.fields.end())
            return std::nullopt;
        const auto distance = static_cast<std::size_t>(
            std::distance(structure.fields.begin(), found));
        if (!struct_dissector::index_fits_int(distance))
            return std::nullopt;
        return std::pair<int, int>{structure_index, static_cast<int>(distance)};
    };
    retained.validate_identity = make_workspace_validator(retained_workspace,
        workspace_id, workspace_generation, analysis_revision,
        [resolve_field] { return resolve_field().has_value(); },
        "The structure, field, or selected workspace changed; select it again");
    auto* view = this;
    menu_add(retained, "types.dissector.catalog.undo",
        struct_dissector::user_catalog_can_undo(),
        "No current catalog edit is available to undo", [view] {
            return view->applyCatalogUndo()
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(view->operationStatus());
        });
    menu_add(retained, "types.dissector.catalog.redo",
        struct_dissector::user_catalog_can_redo(),
        "No current catalog edit is available to redo", [view] {
            return view->applyCatalogRedo()
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(view->operationStatus());
        });
    const auto activate_retained_field = [resolve_field, retained_structure_id,
        retained_structure_revision, retained_field_id]()
        -> std::optional<std::pair<int, int>> {
        const auto resolved = resolve_field();
        if (!resolved)
            return std::nullopt;
        auto& state = struct_dissector::g_state;
        std::lock_guard<std::mutex> lock(state.mtx);
        if (!struct_dissector::valid_index(resolved->first, state.structs.size()))
            return std::nullopt;
        const auto& structure = state.structs[static_cast<std::size_t>(resolved->first)];
        if (structure.stable_id != retained_structure_id ||
            structure.layout_revision != retained_structure_revision ||
            !struct_dissector::valid_index(resolved->second, structure.fields.size()) ||
            structure.fields[static_cast<std::size_t>(resolved->second)].stable_id !=
                retained_field_id)
            return std::nullopt;
        state.active_struct = resolved->first;
        return resolved;
    };
    const auto open_field_edit = [activate_retained_field, view,
        retained_structure_id, retained_structure_revision, retained_field_id](
        dissector_edit_target_t target, std::string seed) {
        const auto resolved = activate_retained_field();
        if (!resolved)
            return aida::ui::action_handler_result_t::failed(
                "The retained field is stale");
        view->openFieldEdit(static_cast<int>(target), resolved->second,
            std::move(seed), retained_structure_id, retained_structure_revision,
            retained_field_id);
        return aida::ui::action_handler_result_t::completed();
    };
    const std::string field_name = field_snapshot.name;
    menu_add(retained, "types.dissector.field.copy_name", valid_field,
        "The retained field is stale", [resolve_field, field_name] {
            if (!resolve_field())
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            clipboard::set_text(QString::fromStdString(field_name));
            return aida::ui::action_handler_result_t::completed();
        });
    menu_add(retained, "types.dissector.field.copy_offset", valid_field,
        "The retained field is stale", [resolve_field, field_snapshot] {
            if (!resolve_field())
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            char text[24]{};
            std::snprintf(text, sizeof(text), "0x%X", field_snapshot.offset);
            clipboard::set_text(QString::fromLatin1(text));
            return aida::ui::action_handler_result_t::completed();
        });
    const std::uint64_t absolute_address = context_base_address_ + field_snapshot.offset;
    menu_add(retained, "types.dissector.field.copy_absolute_address",
        valid_field && context_base_address_ != 0,
        "No live base address is selected", [resolve_field, absolute_address] {
            if (!resolve_field())
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            char text[32]{};
            std::snprintf(text, sizeof(text), "0x%016llX",
                static_cast<unsigned long long>(absolute_address));
            clipboard::set_text(QString::fromLatin1(text));
            return aida::ui::action_handler_result_t::completed();
        });
    const std::string display_value = value_snapshot.display_text;
    menu_add(retained, "types.dissector.field.copy_current_value",
        live_current && !display_value.empty(),
        "A current live value is unavailable for this field",
        [resolve_field, display_value] {
            if (!resolve_field())
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            clipboard::set_text(QString::fromStdString(display_value));
            return aida::ui::action_handler_result_t::completed();
        });
    const std::string field_declaration = std::string(
        struct_dissector::field_type_name(field_snapshot.type)) + " " +
        field_snapshot.name + (field_snapshot.array_count > 1
            ? "[" + std::to_string(field_snapshot.array_count) + "]" : "") +
        "; /* +0x" + [&] {
            char value[16]{};
            std::snprintf(value, sizeof(value), "%X", field_snapshot.offset);
            return std::string(value);
        }() + " */";
    const bool field_persistence_available =
        !struct_dissector::g_state.persistence_in_flight.load(std::memory_order_acquire);
    std::string containing_declaration;
    {
        const auto resolved = resolve_field();
        if (resolved)
            containing_declaration = struct_dissector::export_to_c(resolved->first);
    }
    menu_add(retained, "types.dissector.field.export_declaration",
        field_declaration.size() <= 4096,
        "The field declaration exceeds the bounded export limit",
        [resolve_field, field_declaration] {
            if (!resolve_field())
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            clipboard::set_text(QString::fromStdString(field_declaration));
            return aida::ui::action_handler_result_t::completed();
        });
    menu_add(retained, "types.dissector.field.duplicate", valid_field &&
        field_persistence_available && field_snapshot.parent_idx < 0 &&
        field_snapshot.children.empty(),
        !field_persistence_available ? "Another structure catalog operation is running"
            : "Only a current leaf top-level field can be duplicated without losing hierarchy",
        [activate_retained_field, field_snapshot, view] {
            const auto resolved = activate_retained_field();
            if (!resolved)
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            struct_dissector::field_def_t duplicate = field_snapshot;
            duplicate.stable_id = 0;
            duplicate.parent_idx = -1;
            duplicate.children.clear();
            {
                auto& state = struct_dissector::g_state;
                std::lock_guard<std::mutex> lock(state.mtx);
                if (state.persistence_in_flight.load(std::memory_order_acquire))
                    return aida::ui::action_handler_result_t::failed(
                        "Another structure catalog operation started before duplication");
                if (!struct_dissector::valid_index(resolved->first,
                        state.structs.size()))
                    return aida::ui::action_handler_result_t::failed(
                        "The retained structure is stale");
                const auto& structure =
                    state.structs[static_cast<std::size_t>(resolved->first)];
                std::string base = duplicate.name + "_copy";
                if (base.size() > 240) base.resize(240);
                duplicate.name = base;
                for (std::size_t suffix = 2; suffix <= 65536 &&
                    std::any_of(structure.fields.begin(), structure.fields.end(),
                        [&](const auto& item) { return item.name == duplicate.name; });
                    ++suffix)
                    duplicate.name = base + "_" + std::to_string(suffix);
                if (std::any_of(structure.fields.begin(), structure.fields.end(),
                    [&](const auto& item) { return item.name == duplicate.name; }))
                    return aida::ui::action_handler_result_t::failed(
                        "A bounded unique field name could not be allocated");
                if (structure.kind == struct_dissector::structure_kind_t::union_type)
                    duplicate.offset = 0;
                else {
                    const std::uint32_t alignment = duplicate.explicit_alignment != 0
                        ? duplicate.explicit_alignment
                        : static_cast<std::uint32_t>((std::min)(
                            struct_dissector::field_type_size(duplicate.type),
                            static_cast<std::size_t>(8)));
                    duplicate.offset = struct_dissector::align_up_u32(
                        structure.total_size, alignment == 0 ? 1 : alignment);
                }
            }
            const int created = view->catalogIndexEdit("Duplicate field", [&] {
                return struct_dissector::add_field(resolved->first, duplicate);
            });
            if (created < 0)
                return aida::ui::action_handler_result_t::failed(
                    "The duplicated field failed layout validation; no mutation was retained");
            return aida::ui::action_handler_result_t::completed(
                "Field duplicate created; durable catalog save queued");
        });
    menu_add(retained, "types.dissector.field.review_containing_type_global",
        static_cast<bool>(retained_workspace) && !containing_declaration.empty() &&
            containing_declaration.size() <= 64U * 1024U,
        !retained_workspace
            ? "Select an analysis workspace before reviewing global propagation"
            : "The containing type cannot produce a bounded global declaration",
        [resolve_field, name = retained_structure_name, containing_declaration, this] {
            if (!resolve_field())
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            extern void open_type_declaration_review(
                QtWorkspaceContext*, std::string, std::string, QWidget*);
            open_type_declaration_review(
                QtAnalysisBridge::instance().activeContext(), name,
                containing_declaration, this);
            return aida::ui::action_handler_result_t::completed();
        });
    menu_add(retained, "types.dissector.field.edit_live_value",
        live_current && !value_snapshot.raw_bytes.empty(),
        "Attach the original target and reselect the field before editing live memory",
        [activate_retained_field, value_snapshot, view, retained_structure_id,
            retained_structure_revision, retained_field_id, base_address] {
            const auto resolved = activate_retained_field();
            if (!resolved)
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            struct_dissector::field_def_t field;
            struct_dissector::live_value_t value;
            {
                auto& state = struct_dissector::g_state;
                std::lock_guard<std::mutex> lock(state.mtx);
                if (!struct_dissector::valid_index(resolved->first,
                        state.structs.size()) ||
                    !struct_dissector::valid_index(resolved->second,
                        state.structs[static_cast<std::size_t>(resolved->first)]
                            .fields.size()))
                    return aida::ui::action_handler_result_t::failed(
                        "The retained field is stale");
                field = state.structs[static_cast<std::size_t>(resolved->first)]
                    .fields[static_cast<std::size_t>(resolved->second)];
                if (static_cast<std::size_t>(resolved->second) <
                    state.cached_values.size())
                    value = state.cached_values[
                        static_cast<std::size_t>(resolved->second)];
            }
            std::string error;
            if (!view->stageWriteReview(resolved->first, resolved->second, field,
                    value, base_address, value_snapshot.display_text.c_str(), error))
                return aida::ui::action_handler_result_t::failed(error);
            return aida::ui::action_handler_result_t::completed();
        });
    const bool refresh_available = live_current &&
        !struct_dissector::g_state.refresh_in_flight.load(std::memory_order_acquire);
    menu_add(retained, "types.dissector.field.refresh_live_value", refresh_available,
        live_current ? "A live-value refresh is already running"
            : "Attach the original target and reselect the field before reading live memory",
        [activate_retained_field] {
            if (!activate_retained_field())
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            struct_dissector::refresh_values();
            return aida::ui::action_handler_result_t::completed(
                "Live structure values refresh requested");
        });
    menu_add(retained, "types.dissector.field.rename", valid_field,
        "The retained field is stale", [open_field_edit, field_name] {
            return open_field_edit(dissector_edit_target_t::field_name, field_name);
        });
    menu_add(retained, "types.dissector.field.set_size", valid_field,
        "The retained field is stale", [open_field_edit] {
            return open_field_edit(dissector_edit_target_t::field_size, {});
        });
    const std::string field_comment = field_snapshot.description;
    menu_add(retained, "types.dissector.field.set_comment", valid_field,
        "The retained field is stale", [open_field_edit, field_comment] {
            return open_field_edit(dissector_edit_target_t::field_comment,
                field_comment);
        });
    bool nested_available = false;
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        nested_available = struct_dissector::g_state.structs.size() > 1;
    }
    const auto type_count =
        static_cast<std::size_t>(struct_dissector::field_type_t::COUNT);
    for (std::size_t type_index = 0; type_index < type_count; ++type_index) {
        menu_add(retained,
            "types.dissector.field.change_type." + std::to_string(type_index),
            valid_field && (type_index != static_cast<std::size_t>(
                    struct_dissector::field_type_t::nested_struct) || nested_available),
            nested_available ? "The retained field is stale"
                : "Create another structure before selecting a nested type",
            [activate_retained_field, open_field_edit, type_index] {
                if (type_index == static_cast<std::size_t>(
                        struct_dissector::field_type_t::nested_struct)) {
                    return open_field_edit(dissector_edit_target_t::nested_target, {});
                }
                const auto resolved = activate_retained_field();
                if (!resolved)
                    return aida::ui::action_handler_result_t::failed(
                        "The retained field is stale");
                return struct_dissector::retype_field(resolved->first, resolved->second,
                        static_cast<struct_dissector::field_type_t>(type_index))
                    ? aida::ui::action_handler_result_t::completed()
                    : aida::ui::action_handler_result_t::failed(
                        "The field type change failed layout validation");
            });
    }
    menu_add(retained, "types.dissector.field.set_array_count", valid_field,
        "The retained field is stale", [open_field_edit, field_snapshot] {
            return open_field_edit(dissector_edit_target_t::array_count,
                std::to_string(field_snapshot.array_count));
        });
    menu_add(retained, "types.dissector.field.choose_nested",
        valid_field && nested_available,
        nested_available ? "The retained field is stale"
            : "Create another structure before linking a nested value",
        [open_field_edit, field_snapshot] {
            return open_field_edit(dissector_edit_target_t::nested_target,
                field_snapshot.referenced_type_name);
        });
    menu_add(retained, "types.dissector.field.choose_pointer_target",
        valid_field && nested_available,
        nested_available ? "The retained field is stale"
            : "Create another structure before selecting a pointee type",
        [open_field_edit, field_snapshot] {
            return open_field_edit(dissector_edit_target_t::pointer_target,
                field_snapshot.referenced_type_name);
        });
    bool enum_available = false;
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        enum_available = !struct_dissector::g_state.enums.empty();
    }
    menu_add(retained, "types.dissector.field.choose_enum",
        valid_field && enum_available,
        enum_available ? "The retained field is stale"
            : "Import an enum definition before applying an enum type",
        [open_field_edit, field_snapshot] {
            return open_field_edit(dissector_edit_target_t::enum_reference,
                field_snapshot.referenced_type_name);
        });
    const std::size_t bitfield_size = field_snapshot.size == 0
        ? struct_dissector::field_type_size(field_snapshot.type) : field_snapshot.size;
    const bool bitfield_capable = valid_field && field_snapshot.array_count == 1 &&
        bitfield_size >= 1 && bitfield_size <= 8 &&
        field_snapshot.type != struct_dissector::field_type_t::pointer &&
        field_snapshot.type != struct_dissector::field_type_t::nested_struct;
    menu_add(retained, "types.dissector.field.configure_bitfield", bitfield_capable,
        "Bitfields require one 1-8 byte non-pointer scalar",
        [open_field_edit, field_snapshot] {
            const std::string seed = field_snapshot.bit_width == 0
                ? "0:1" : std::to_string(field_snapshot.bit_offset) + ":" +
                    std::to_string(field_snapshot.bit_width);
            return open_field_edit(dissector_edit_target_t::bitfield, seed);
        });
    menu_add(retained, "types.dissector.field.set_alignment", valid_field,
        "The retained field is stale", [open_field_edit, field_snapshot] {
            return open_field_edit(dissector_edit_target_t::field_alignment,
                std::to_string(field_snapshot.explicit_alignment));
        });
    menu_add(retained, "types.dissector.field.insert_before", valid_field,
        "The retained field is stale", [activate_retained_field, view] {
            const auto resolved = activate_retained_field();
            if (!resolved)
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            view->setPendingInsertIndex(resolved->second);
            return aida::ui::action_handler_result_t::completed();
        });
    menu_add(retained, "types.dissector.field.insert_after", valid_field,
        "The retained field is stale", [activate_retained_field, view] {
            const auto resolved = activate_retained_field();
            if (!resolved)
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            view->setPendingInsertIndex(resolved->second + 1);
            return aida::ui::action_handler_result_t::completed();
        });
    menu_add(retained, "types.dissector.field.move_up", valid_field && target_field > 0,
        "The field is already first", [activate_retained_field, view] {
            const auto resolved = activate_retained_field();
            if (!resolved || resolved->second <= 0)
                return aida::ui::action_handler_result_t::failed(
                    "The retained field cannot move up");
            const bool applied = view->catalogEdit("Move field up", [&] {
                return struct_dissector::move_field(resolved->first, resolved->second,
                    resolved->second - 1);
            });
            return applied
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(
                    "The reordered layout failed validation");
        });
    menu_add(retained, "types.dissector.field.move_down", valid_field &&
        static_cast<std::size_t>(target_field + 1) < field_count,
        "The field is already last", [activate_retained_field, view] {
            const auto resolved = activate_retained_field();
            if (!resolved)
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            const bool applied = view->catalogEdit("Move field down", [&] {
                return struct_dissector::move_field(resolved->first, resolved->second,
                    resolved->second + 1);
            });
            return applied
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(
                    "The reordered layout failed validation");
        });
    menu_add(retained, "types.dissector.field.remove", valid_field,
        "The retained field is stale", [activate_retained_field, view,
            retained_structure_id, retained_structure_revision, retained_field_id] {
            const auto resolved = activate_retained_field();
            if (!resolved)
                return aida::ui::action_handler_result_t::failed(
                    "The retained field is stale");
            view->confirmRemoveField(retained_structure_id, retained_structure_revision,
                retained_field_id);
            return aida::ui::action_handler_result_t::completed();
        });
    aida::automation_ui::entity_evidence::snapshot_t evidence;
    evidence.workspace_id = !workspace_id.empty() ? workspace_id : "types.catalog";
    evidence.source_view_id = "view.types.dissector";
    evidence.source_kind = "editable_structure_field";
    evidence.entity_id = retained.entity_id;
    evidence.display_label = retained_structure_name + "." + field_snapshot.name;
    constexpr std::size_t maximum_evidence_bytes = 64U * 1024U;
    const auto append_evidence = [&](const std::string& value) {
        if (value.size() > maximum_evidence_bytes - evidence.excerpt.size())
            return false;
        evidence.excerpt.append(value);
        return true;
    };
    const bool evidence_bounded = append_evidence(field_declaration) &&
        append_evidence("\nSize: ") &&
        append_evidence(std::to_string(field_snapshot.size)) &&
        append_evidence("\nArray count: ") &&
        append_evidence(std::to_string(field_snapshot.array_count)) &&
        append_evidence("\nComment: ") &&
        append_evidence(field_snapshot.description) &&
        (display_value.empty() || (append_evidence("\nCurrent value: ") &&
            append_evidence(display_value)));
    if (!evidence_bounded)
        evidence.excerpt.clear();
    evidence.address = context_base_address_ != 0 ? absolute_address : 0;
    evidence.revision = retained_structure_revision;
    evidence.generation = workspace_generation;
    evidence.sensitive = live_current;
    aida::automation_ui::entity_evidence::append_actions(retained,
        std::move(evidence), evidence_bounded
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The field evidence exceeds the bounded 64 KiB contract"));
    QtAnalysisBridge::instance().showRetainedMenu(retained,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}
