#include <QPoint>

#include <cstdio>
#include <memory>

#include "helpers/diag_log.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/shell_host_contract.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_binary_map_dialogs.hpp"
#include "qt/analysis/qt_binary_map_list_model.hpp"
#include "qt/analysis/qt_binary_map_shared.hpp"
#include "qt/analysis/qt_binary_map_view.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/bridge/clipboard.hpp"

namespace aida::qt::analysis {

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

std::string hex_va(std::uint64_t value) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(value));
    return buf;
}

}

// view-row -1 == heatmap cell menu (analysis.binary_map.heat_function).
void show_binary_map_menu(QtBinaryMapView* view, QWidget* parent,
                          const QPoint& global_pos, int view_row) {
    auto* bridge = &QtAnalysisBridge::instance();
    auto* context = bridge->activeContext();
    if (!context) return;
    auto state = context->binaryMapState;
    if (!state) return;
    const auto map_snapshot = std::atomic_load_explicit(&state->map,
        std::memory_order_acquire);
    const auto live_snapshot = std::atomic_load_explicit(&state->live,
        std::memory_order_acquire);
    const QtBinaryMapListModel::list_row_t* row = nullptr;
    if (view_row >= 0)
        row = view->listModel() ? view->listModel()->rowAt(view_row) : nullptr;
    if (!row) return;

    const auto present = [&](retained_menu_t& retained) {
        bridge->showRetainedMenu(retained,
            aida::ui::context_menu_open_origin_t::pointer, global_pos, parent);
    };

    switch (row->kind) {
    case QtBinaryMapListModel::row_kind_t::region: {
        const auto region = row->region;
        state->live_selected_base.store(region.base);
        retained_menu_t retained;
        retained.owner_id = "analysis.binary_map.region";
        retained.entity_id = std::to_string(region.base) + ":" +
            std::to_string(region.size);
        retained.entity_generation =
            state->workspace_generation.load(std::memory_order_acquire);
        retained.active_view = aida::ui::stable_view_id_t("view.analysis.binary_map");
        const auto retained_live = live_snapshot;
        auto* s = state.get();
        retained.validate_identity = [s, retained_live, region] {
            const auto current_live = std::atomic_load_explicit(&s->live,
                std::memory_order_acquire);
            return current_live == retained_live &&
                s->live_selected_base.load() == region.base &&
                bm_binding_matches_workspace(retained_live->target_binding,
                    s->workspace.lock()) &&
                s->workspace_generation.load(std::memory_order_acquire) ==
                    retained_live->target_binding.workspace_generation
                ? aida::ui::capability_state_t::available()
                : aida::ui::capability_state_t::unavailable(
                    "The live memory-map publication or selected region changed");
        };
        menu_add(retained, "analysis.binary_map.region.follow_disassembly", true, "",
            [s, region] {
                bm_jump_to_address(*s, region.base);
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.region.open_hex", true, "",
            [s, retained_live, region] {
                bm_jump_to_hex(*s, region.base, bm_hex_request_size(region.size),
                    retained_live->target_binding);
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.region.dump", region.size != 0,
            "The retained region is empty", [s, retained_live, region, parent] {
                bm_dump_region_to_disk(*s, region.base, region.size,
                    bm_region_kind_label(region), parent,
                    retained_live->target_binding);
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.region.change_protection",
            region.size != 0 &&
                !s->change_protect_pending.load(std::memory_order_acquire),
            s->change_protect_pending.load(std::memory_order_acquire)
                ? "Another reviewed protection change is running"
                : "The retained region is empty",
            [state, retained_live, region, parent] {
                state->change_protect_addr = region.base;
                state->change_protect_size = region.size;
                state->change_protect_old = region.protect;
                state->change_protect_binding = retained_live->target_binding;
                state->change_protect_choice = 0;
                auto* dialog = new QtChangeProtectionDialog(state,
                    retained_live->target_binding, region.base, region.size,
                    region.protect, parent);
                dialog->open();
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.region.copy_va", true, "", [region] {
            clipboard::set_text(QString::fromStdString(hex_va(region.base)));
            return aida::ui::action_handler_result_t::completed();
        });
        menu_add(retained, "analysis.binary_map.region.copy_json", true, "", [region] {
            const std::string json = bm_region_to_json(region);
            clipboard::set_text(QString::fromStdString(json));
            diag::log_tagged_fmt("binary_map",
                "region_ctx copy_json base=0x%llX bytes=%zu",
                static_cast<unsigned long long>(region.base), json.size());
            return aida::ui::action_handler_result_t::completed();
        });
        menu_add(retained, "analysis.binary_map.region.send_chat", true, "", [region] {
            const std::string payload = bm_make_region_chat_payload(region);
            QtAnalysisBridge::instance().injectToChatText(
                QString::fromStdString(payload));
            return aida::ui::action_handler_result_t::completed();
        });
        present(retained);
        return;
    }
    case QtBinaryMapListModel::row_kind_t::module: {
        const auto module = row->module;
        state->live_selected_base.store(module.base);
        retained_menu_t retained;
        retained.owner_id = "analysis.binary_map.module";
        retained.entity_id = std::to_string(module.base) + ":" + module.name;
        retained.entity_generation =
            state->workspace_generation.load(std::memory_order_acquire);
        retained.active_view = aida::ui::stable_view_id_t("view.analysis.binary_map");
        const auto retained_live = live_snapshot;
        auto* s = state.get();
        retained.validate_identity = [s, module, retained_live] {
            return std::atomic_load_explicit(&s->live, std::memory_order_acquire) ==
                    retained_live &&
                s->live_selected_base.load() == module.base &&
                bm_binding_matches_workspace(retained_live->target_binding,
                    s->workspace.lock()) &&
                s->workspace_generation.load(std::memory_order_acquire) ==
                    retained_live->target_binding.workspace_generation
                ? aida::ui::capability_state_t::available()
                : aida::ui::capability_state_t::unavailable(
                    "The live module publication or selection changed");
        };
        menu_add(retained, "analysis.binary_map.module.follow_disassembly", true, "",
            [s, module] {
                bm_jump_to_address(*s, module.base);
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.module.open_hex", true, "",
            [s, retained_live, module] {
                bm_jump_to_hex(*s, module.base, 0x400, retained_live->target_binding);
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.module.copy_name",
            !module.name.empty(), "The retained module has no name", [module] {
                clipboard::set_text(QString::fromStdString(module.name));
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.module.copy_path",
            !module.path.empty(), "The retained module has no path", [module] {
                clipboard::set_text(QString::fromStdString(module.path));
                return aida::ui::action_handler_result_t::completed();
            });
        present(retained);
        return;
    }
    case QtBinaryMapListModel::row_kind_t::section: {
        const auto section = row->section;
        state->selected_va.store(section.va);
        retained_menu_t retained;
        retained.owner_id = "analysis.binary_map.section";
        retained.entity_id = std::to_string(section.va) + ":" + section.name;
        retained.entity_generation =
            state->workspace_generation.load(std::memory_order_acquire);
        retained.active_view = aida::ui::stable_view_id_t("view.analysis.binary_map");
        auto* s = state.get();
        retained.validate_identity = [s, section, map_snapshot] {
            return std::atomic_load_explicit(&s->map, std::memory_order_acquire) ==
                    map_snapshot &&
                s->selected_va.load() == section.va
                ? aida::ui::capability_state_t::available()
                : aida::ui::capability_state_t::unavailable(
                    "The static Binary Map publication or selected section changed");
        };
        menu_add(retained, "analysis.binary_map.section.follow_disassembly", true, "",
            [s, section] {
                bm_jump_to_address(*s, section.va);
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.section.open_hex", true, "",
            [s, section] {
                bm_jump_to_hex(*s, section.va, bm_hex_request_size(section.size));
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.section.dump", section.size != 0,
            "The retained section is empty", [s, section, parent] {
                bm_dump_region_to_disk(*s, section.va, section.size,
                    section.name.empty() ? std::string("section") : section.name,
                    parent);
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.section.copy_name",
            !section.name.empty(), "The retained section has no name", [section] {
                clipboard::set_text(QString::fromStdString(section.name));
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.section.copy_va", true, "", [section] {
            clipboard::set_text(QString::fromStdString(hex_va(section.va)));
            return aida::ui::action_handler_result_t::completed();
        });
        present(retained);
        return;
    }
    case QtBinaryMapListModel::row_kind_t::function: {
        const auto function = row->function;
        state->selected_va.store(function.va);
        retained_menu_t retained;
        retained.owner_id = "analysis.binary_map.function";
        retained.entity_id = std::to_string(function.va) + ":" + function.name;
        retained.entity_generation =
            state->workspace_generation.load(std::memory_order_acquire);
        retained.active_view = aida::ui::stable_view_id_t("view.analysis.binary_map");
        auto* s = state.get();
        retained.validate_identity = [s, function, map_snapshot] {
            return std::atomic_load_explicit(&s->map, std::memory_order_acquire) ==
                    map_snapshot &&
                s->selected_va.load() == function.va
                ? aida::ui::capability_state_t::available()
                : aida::ui::capability_state_t::unavailable(
                    "The Binary Map function publication or selection changed");
        };
        menu_add(retained, "analysis.binary_map.function.send_chat", true, "",
            [function] {
                QtAnalysisBridge::instance().injectToChatText(QString::fromStdString(
                    bm_make_function_chat_payload(function)));
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, function.pinned
                ? "analysis.binary_map.function.unpin"
                : "analysis.binary_map.function.pin", true, "",
            [s, function] {
                bm_set_function_pinned(*s, function.va, !function.pinned);
                s->refresh_after_pin_requested = true;
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.function.follow_disassembly", true, "",
            [s, function] {
                bm_jump_to_address(*s, function.va);
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.function.open_hex", true, "",
            [s, function] {
                bm_jump_to_hex(*s, function.va, 0x400);
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.function.copy_va", true, "",
            [function] {
                clipboard::set_text(QString::fromStdString(hex_va(function.va)));
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.function.copy_name",
            !function.name.empty(), "The retained function has no name", [function] {
                clipboard::set_text(QString::fromStdString(function.name));
                return aida::ui::action_handler_result_t::completed();
            });
        present(retained);
        return;
    }
    case QtBinaryMapListModel::row_kind_t::global: {
        const auto global = row->global;
        const std::string global_id = "global:" + std::to_string(global.va);
        state->selected_entity_id = global_id;
        retained_menu_t retained;
        retained.owner_id = "analysis.binary_map.global";
        retained.entity_id = global_id + ":" + global.name;
        retained.entity_generation =
            state->workspace_generation.load(std::memory_order_acquire);
        retained.active_view = aida::ui::stable_view_id_t("view.analysis.binary_map");
        auto* s = state.get();
        retained.validate_identity = [s, global_id, map_snapshot] {
            return std::atomic_load_explicit(&s->map, std::memory_order_acquire) ==
                    map_snapshot &&
                s->selected_entity_id == global_id
                ? aida::ui::capability_state_t::available()
                : aida::ui::capability_state_t::unavailable(
                    "The Binary Map global publication or selection changed");
        };
        menu_add(retained, "analysis.binary_map.global.send_chat", true, "", [global] {
            QtAnalysisBridge::instance().injectToChatText(QString::fromStdString(
                bm_make_global_chat_payload(global)));
            return aida::ui::action_handler_result_t::completed();
        });
        menu_add(retained, "analysis.binary_map.global.open_hex", true, "",
            [s, global] {
                bm_jump_to_hex(*s, global.va, 0x200);
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.global.follow_disassembly", true, "",
            [s, global] {
                bm_jump_to_address(*s, global.va);
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.global.copy_va", true, "", [global] {
            clipboard::set_text(QString::fromStdString(hex_va(global.va)));
            return aida::ui::action_handler_result_t::completed();
        });
        menu_add(retained, "analysis.binary_map.global.copy_name",
            !global.name.empty(), "The retained global has no name", [global] {
                clipboard::set_text(QString::fromStdString(global.name));
                return aida::ui::action_handler_result_t::completed();
            });
        present(retained);
        return;
    }
    case QtBinaryMapListModel::row_kind_t::import_dll: {
        const std::string dll = row->dll;
        state->selected_entity_id = "import-dll:" + dll;
        std::vector<std::string> funcs;
        if (map_snapshot) {
            for (const auto& imp : map_snapshot->imports) {
                if (imp.compare(0, dll.size(), dll) == 0 && imp.size() > dll.size() &&
                    imp[dll.size()] == ':') {
                    std::string rest = imp.substr(dll.size() + 1);
                    std::size_t pos = 0;
                    while (pos < rest.size()) {
                        std::size_t next = rest.find(',', pos);
                        if (next == std::string::npos) next = rest.size();
                        std::string token = rest.substr(pos, next - pos);
                        while (!token.empty() && token.front() == ' ')
                            token.erase(token.begin());
                        while (!token.empty() && token.back() == ' ') token.pop_back();
                        if (!token.empty()) funcs.push_back(std::move(token));
                        pos = next + 1u;
                    }
                }
            }
        }
        retained_menu_t retained;
        retained.owner_id = "analysis.binary_map.import_dll";
        retained.entity_id = "import-dll:" + dll;
        retained.entity_generation =
            state->workspace_generation.load(std::memory_order_acquire);
        retained.active_view = aida::ui::stable_view_id_t("view.analysis.binary_map");
        auto* s = state.get();
        const std::string retained_id = retained.entity_id;
        retained.validate_identity = [s, retained_id, map_snapshot] {
            return std::atomic_load_explicit(&s->map, std::memory_order_acquire) ==
                    map_snapshot &&
                s->selected_entity_id == retained_id
                ? aida::ui::capability_state_t::available()
                : aida::ui::capability_state_t::unavailable(
                    "The import publication or selected DLL changed");
        };
        menu_add(retained, "analysis.binary_map.import.copy_dll_name", !dll.empty(),
            "The retained import has no DLL name", [dll] {
                clipboard::set_text(QString::fromStdString(dll));
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.import.copy_function_list",
            !funcs.empty(), "The retained DLL has no imported functions", [funcs] {
                std::string joined;
                for (std::size_t fi = 0; fi < funcs.size(); ++fi) {
                    if (fi) joined += "\n";
                    joined += funcs[fi];
                }
                clipboard::set_text(QString::fromStdString(joined));
                return aida::ui::action_handler_result_t::completed();
            });
        present(retained);
        return;
    }
    case QtBinaryMapListModel::row_kind_t::import_function: {
        const std::string id = "import-function:" + row->dll + "!" + row->function_name;
        state->selected_entity_id = id;
        retained_menu_t retained;
        retained.owner_id = "analysis.binary_map.import_function";
        retained.entity_id = id;
        retained.entity_generation =
            state->workspace_generation.load(std::memory_order_acquire);
        retained.active_view = aida::ui::stable_view_id_t("view.analysis.binary_map");
        auto* s = state.get();
        retained.validate_identity = [s, id, map_snapshot] {
            return std::atomic_load_explicit(&s->map, std::memory_order_acquire) ==
                    map_snapshot &&
                s->selected_entity_id == id
                ? aida::ui::capability_state_t::available()
                : aida::ui::capability_state_t::unavailable(
                    "The import publication or selected function changed");
        };
        const std::string dll = row->dll;
        const std::string fn = row->function_name;
        menu_add(retained, "analysis.binary_map.import.copy_qualified_name", true, "",
            [dll, fn] {
                clipboard::set_text(QString::fromStdString(dll + "!" + fn));
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.import.copy_function_name",
            !fn.empty(), "The retained import has no function name", [fn] {
                clipboard::set_text(QString::fromStdString(fn));
                return aida::ui::action_handler_result_t::completed();
            });
        present(retained);
        return;
    }
    case QtBinaryMapListModel::row_kind_t::export_entry: {
        const std::string id = "export:" + row->export_name;
        state->selected_entity_id = id;
        retained_menu_t retained;
        retained.owner_id = "analysis.binary_map.export";
        retained.entity_id = id;
        retained.entity_generation =
            state->workspace_generation.load(std::memory_order_acquire);
        retained.active_view = aida::ui::stable_view_id_t("view.analysis.binary_map");
        auto* s = state.get();
        retained.validate_identity = [s, id, map_snapshot] {
            return std::atomic_load_explicit(&s->map, std::memory_order_acquire) ==
                    map_snapshot &&
                s->selected_entity_id == id
                ? aida::ui::capability_state_t::available()
                : aida::ui::capability_state_t::unavailable(
                    "The export publication or selected symbol changed");
        };
        const std::uint64_t resolved_va = row->export_va;
        const std::string name = row->export_name;
        menu_add(retained, "analysis.binary_map.export.follow_disassembly",
            resolved_va != 0, "No function address was resolved for this export",
            [s, resolved_va] {
                bm_jump_to_address(*s, resolved_va);
                return aida::ui::action_handler_result_t::completed();
            });
        menu_add(retained, "analysis.binary_map.export.copy_name", !name.empty(),
            "The retained export has no name", [name] {
                clipboard::set_text(QString::fromStdString(name));
                return aida::ui::action_handler_result_t::completed();
            });
        present(retained);
        return;
    }
    default:
        return;
    }
}

}
