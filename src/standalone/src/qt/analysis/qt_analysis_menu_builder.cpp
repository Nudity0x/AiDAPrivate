#include "qt/analysis/qt_analysis_menu_builder.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_analysis_host_hooks.hpp"
#include "qt/analysis/qt_xref_model.hpp"
#include "qt/analysis_bridge/revision_combine.hpp"
#include "qt/bridge/clipboard.hpp"

#include "core/disasm/cfg_view.hpp"
#include "core/disasm/pseudocode_view.hpp"
#include "core/disasm/disasm_view.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <utility>

namespace aida::qt::analysis::qt_analysis_menus {

namespace {

std::uint64_t workspace_generation_hash(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace) {
    if (!workspace) return 0;
    return aida::analysis_bridge::combine_generation_revision(
        workspace->generation(), workspace->analysis_revision());
}

std::string address_text(std::uint64_t address) {
    char value[32]{};
    std::snprintf(value, sizeof(value), "0x%llX",
        static_cast<unsigned long long>(address));
    return value;
}

void open_disassembly(const disasm_view::workspace_context_t& context,
                      std::uint64_t address) {
    disasm_view::goto_address(address, context);
    QtAnalysisBridge::instance().openView("document.disassembly");
}

void open_graph(const disasm_view::workspace_context_t& context,
                std::uint64_t address) {
    const auto function = disasm_view::enclosing_function_start(address, context);
    if (function == 0) return;
    cfg_view::build_cfg(context, function);
    QtAnalysisBridge::instance().openView("document.graph");
}

void open_pseudocode(const disasm_view::workspace_context_t& context,
                     std::uint64_t address) {
    const auto function = disasm_view::enclosing_function_start(address, context);
    if (function == 0) return;
    pseudocode_view::request_decompile(context, function, false);
    QtAnalysisBridge::instance().openView("document.pseudocode");
}

void open_related_view(const disasm_view::workspace_context_t& context,
                       std::uint64_t address, const char* stable_id) {
    disasm_view::select_address(address, context);
    QtAnalysisBridge::instance().openView(stable_id);
}

void open_xrefs_to(const disasm_view::workspace_context_t& context,
                   std::uint64_t address) {
    disasm_view::open_xrefs(address, context);
    QtAnalysisBridge::instance().openView("view.analysis.references");
}

void open_xrefs_direction(const disasm_view::workspace_context_t& context,
                          std::uint64_t address, bool query_to) {
    const auto typed = disasm_view::typed_address(context, address);
    if (!typed) return;
    std::string error;
    auto& hooks = analysis_host_hooks();
    if (hooks.submit_xref_query &&
        hooks.submit_xref_query(context.workspace, address, query_to, error))
        return;
}

void open_rename(const disasm_view::workspace_context_t& context,
                 std::uint64_t address) {
    // The Qt rename dialog is installed by the disasm domain via
    // disasm_view::set_rename_dialog_hook; request_* fires it.
    static_cast<void>(disasm_view::request_rename_dialog(context,
        aida::analysis::address_t{aida::analysis::address_space_id_t::virtual_address,
                                  address}));
}

void open_comment(const disasm_view::workspace_context_t& context,
                  std::uint64_t address) {
    static_cast<void>(disasm_view::request_comment_dialog(context,
        aida::analysis::address_t{aida::analysis::address_space_id_t::virtual_address,
                                  address}));
}

}

void add_action(retained_menu_t& menu, std::string id, bool enabled,
                const char* reason, invoke_fn_t invoke) {
    aida::ui::application_ui::retained_entity_action_t action;
    action.action_id = std::move(id);
    action.capability = enabled ? capability_t::available()
        : capability_t::unavailable(reason);
    action.invoke = std::move(invoke);
    menu.actions.push_back(std::move(action));
}

void add_separator_marker_copy_actions(retained_menu_t& menu,
    const std::string& address, const std::string& name,
    const std::string& line_text) {
    add_action(menu, "analysis.copy.address", true, "", [address] {
        clipboard::set_text(QString::fromStdString(address));
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.copy.name", true, "", [name] {
        clipboard::set_text(QString::fromStdString(name));
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.copy.line", true, "", [line_text] {
        clipboard::set_text(QString::fromStdString(line_text));
        return handler_result_t::completed();
    });
}

void fill_analysis_entity_actions(retained_menu_t& menu,
    const disasm_view::workspace_context_t& context, std::uint64_t address,
    bool has_address, const std::string& name, const std::string& context_text,
    const std::string& detail_text) {
    if (has_address) {
        add_action(menu, "analysis.navigate.disassembly", true, "",
            [context, address] {
                open_disassembly(context, address);
                return handler_result_t::completed();
            });
        if (disasm_view::enclosing_function_start(address, context) != 0) {
            add_action(menu, "analysis.navigate.graph", true, "",
                [context, address] {
                    open_graph(context, address);
                    return handler_result_t::completed();
                });
            add_action(menu, "analysis.navigate.pseudocode", true, "",
                [context, address] {
                    open_pseudocode(context, address);
                    return handler_result_t::completed();
                });
        }
        add_action(menu, "analysis.navigate.hex", true, "",
            [context, address] {
                open_related_view(context, address, "document.hex");
                return handler_result_t::completed();
            });
        add_action(menu, "analysis.navigate.types", true, "",
            [context, address] {
                open_related_view(context, address, "view.types.inferred");
                return handler_result_t::completed();
            });
        add_action(menu, "analysis.navigate.structures", true, "",
            [context, address] {
                open_related_view(context, address, "view.types.structures");
                return handler_result_t::completed();
            });
        add_action(menu, "analysis.navigate.xrefs", true, "",
            [context, address] {
                open_xrefs_to(context, address);
                return handler_result_t::completed();
            });
        add_action(menu, "analysis.navigate.xrefs_from", true, "",
            [context, address] {
                open_xrefs_direction(context, address, false);
                return handler_result_t::completed();
            });
        add_action(menu, "analysis.modify.rename", true, "",
            [context, address] {
                open_rename(context, address);
                return handler_result_t::completed();
            });
        add_action(menu, "analysis.modify.comment", true, "",
            [context, address] {
                open_comment(context, address);
                return handler_result_t::completed();
            });
    }
    add_separator_marker_copy_actions(menu, has_address ? address_text(address) : "-",
        name, (has_address ? address_text(address) : std::string("-")) + "\t" + name +
        "\t" + context_text + "\t" + detail_text);
}

retained_menu_t build_analysis_row_menu(
    const disasm_view::workspace_context_t& context, std::uint64_t address,
    bool has_address, const std::string& name, const std::string& context_text,
    const std::string& detail_text, validate_fn_t validate_identity) {
    retained_menu_t menu;
    menu.owner_id = "analysis.row";
    menu.menu = aida::ui::stable_menu_id_t("menu.analysis.function");
    menu.entity_id = std::string(has_address ? "address:" : "entity:") +
        std::to_string(address) + ":" + name + ":" + context_text + ":" + detail_text;
    // List rows use the short mix (analysis_list_views.hpp:543-549), not the
    // full boost combine used by the function/proximity/xref menus.
    menu.entity_generation = context.publication
        ? context.publication->generation ^
            (context.publication->analysis_revision +
             aida::analysis_bridge::k_golden_gamma)
        : 0;
    menu.validate_identity = [generation = menu.entity_generation,
                              workspace = context.workspace,
                              validate_identity = std::move(validate_identity)]() {
        const std::uint64_t live = workspace ? workspace->generation() ^
            (workspace->analysis_revision() +
             aida::analysis_bridge::k_golden_gamma) : 0;
        if (live != generation)
            return capability_t::unavailable(
                "The analysis selection is stale; select the item again");
        if (validate_identity) {
            const auto identity = validate_identity();
            if (!identity.enabled)
                return identity;
        }
        return capability_t::available();
    };
    fill_analysis_entity_actions(menu, context, address, has_address, name,
        context_text, detail_text);
    return menu;
}

retained_menu_t build_function_menu(
    const disasm_view::workspace_context_t& context, std::uint64_t address,
    const std::string& name, bool have_entry, validate_fn_t validate_identity) {
    retained_menu_t menu;
    menu.owner_id = "analysis.function";
    menu.menu = aida::ui::stable_menu_id_t("menu.analysis.function");
    menu.entity_id = "function:" + std::to_string(address);
    const auto workspace = context.workspace;
    menu.entity_generation = workspace_generation_hash(workspace);
    menu.validate_identity = [workspace, generation = menu.entity_generation,
                              validate_identity = std::move(validate_identity)]() {
        if (workspace_generation_hash(workspace) != generation)
            return capability_t::unavailable(
                "The analysis selection is stale; select the item again");
        if (validate_identity) {
            const auto identity = validate_identity();
            if (!identity.enabled)
                return identity;
        }
        return capability_t::available();
    };
    add_action(menu, "analysis.navigate.disassembly", true, "", [context, address] {
        open_disassembly(context, address);
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.navigate.graph", true, "", [context, address] {
        open_graph(context, address);
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.navigate.pseudocode", true, "", [context, address] {
        open_pseudocode(context, address);
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.navigate.hex", true, "", [context, address] {
        open_related_view(context, address, "document.hex");
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.navigate.types", true, "", [context, address] {
        open_related_view(context, address, "view.types.inferred");
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.navigate.structures", true, "", [context, address] {
        open_related_view(context, address, "view.types.structures");
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.navigate.xrefs", true, "", [context, address] {
        open_xrefs_to(context, address);
        return handler_result_t::completed();
    });
    // 07 sec. 10: xrefs_from/callees collapsed into ONE action (identical invoke).
    add_action(menu, "analysis.navigate.xrefs_from", true, "", [context, address] {
        open_xrefs_direction(context, address, false);
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.navigate.callers", true, "", [context, address] {
        open_xrefs_direction(context, address, true);
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.modify.rename", true, "", [context, address] {
        open_rename(context, address);
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.modify.comment", true, "", [context, address] {
        open_comment(context, address);
        return handler_result_t::completed();
    });
    const std::string address_value = address_text(address);
    add_action(menu, "analysis.copy.address", true, "", [address_value] {
        clipboard::set_text(QString::fromStdString(address_value));
        return handler_result_t::completed();
    });
    if (have_entry) {
        add_action(menu, "analysis.copy.name", true, "", [name] {
            clipboard::set_text(QString::fromStdString(name));
            return handler_result_t::completed();
        });
        add_action(menu, "analysis.copy.line", true, "", [address_value, name] {
            clipboard::set_text(QString::fromStdString(address_value + "\t" + name));
            return handler_result_t::completed();
        });
    }
    return menu;
}

retained_menu_t build_xref_menu(
    const disasm_view::workspace_context_t& context,
    const std::shared_ptr<QtXrefViewState>& state, std::uint64_t runtime,
    const std::string& name, const std::string& label) {
    retained_menu_t menu;
    menu.owner_id = "analysis.xref";
    menu.menu = aida::ui::stable_menu_id_t("menu.analysis.xref");
    menu.entity_id = "xref:" + std::to_string(runtime) + ":" + name + ":" + label;
    const auto generation = context.workspace->generation();
    const auto revision = context.workspace->analysis_revision();
    std::uint64_t visible_version = 0;
    if (state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        visible_version = state->visible_version;
    }
    menu.entity_generation =
        aida::analysis_bridge::combine_generation_revision(generation, revision) ^
        visible_version;
    menu.validate_identity = [state, runtime, label, workspace = context.workspace,
                              retained_generation = menu.entity_generation]() {
        std::uint64_t live_version = 0;
        if (state) {
            std::lock_guard<std::mutex> lock(state->mutex);
            live_version = state->visible_version;
        }
        const std::uint64_t live =
            aida::analysis_bridge::combine_generation_revision(
                workspace->generation(), workspace->analysis_revision()) ^
            live_version;
        if (live != retained_generation)
            return capability_t::unavailable(
                "The analysis selection is stale; select the item again");
        if (!state)
            return capability_t::unavailable("The selected cross-reference changed");
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto visible = state->visible_results;
        const bool retained = visible && std::any_of(
            visible->begin(), visible->end(), [&](const auto& item) {
                return item.runtime == runtime && item.label == label;
            });
        return state->selected_runtime == runtime && retained
            ? capability_t::available()
            : capability_t::unavailable("The selected cross-reference changed");
    };
    add_action(menu, "analysis.navigate.disassembly", true, "", [context, runtime] {
        disasm_view::goto_address(runtime, context);
        QtAnalysisBridge::instance().openView("document.disassembly");
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.navigate.xrefs", true, "", [context, runtime] {
        disasm_view::open_xrefs(runtime, context);
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.copy.line", true, "", [label] {
        clipboard::set_text(QString::fromStdString(label));
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.copy.name", true, "", [name] {
        clipboard::set_text(QString::fromStdString(name));
        return handler_result_t::completed();
    });
    add_action(menu, "analysis.copy.address", true, "", [runtime] {
        clipboard::set_text(QString::fromStdString(address_text(runtime)));
        return handler_result_t::completed();
    });
    return menu;
}

}
