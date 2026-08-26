#include "qt/analysis_bridge/pseudocode_session.hpp"

#include "qt/analysis_bridge/disasm_workspace_model.hpp"

#include "core/disasm/cfg_view.hpp"
#include "core/editor/hex_view.hpp"
#include "helpers/diag_log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <knownfolders.h>
#include <shlobj.h>

namespace pseudocode_view {

namespace {

using pseudocode_cache_state_t =
    aida::workbench::pseudocode_document::pseudocode_cache_state_t;
using pseudocode_request_t =
    aida::workbench::pseudocode_document::pseudocode_request_t;

struct tab_t {
    std::uint64_t address = 0;
    std::string entity_locator;
    std::string label;
    pseudocode_request_t request;
    bool has_request = false;
    std::uint64_t job_id = 0;
    pseudocode_cache_state_t state = pseudocode_cache_state_t::empty;
    std::string error;
    bool error_acknowledged = false;
    std::uint64_t renames_applied_job_id = 0;
    std::uint64_t resolve_ticket = 0;
    bool resolve_force_refresh = false;
};

using local_rename_map_t = std::map<std::string, std::string>;

struct ast_index_cache_t {
    const void* document = nullptr;
    std::unordered_map<std::uint64_t,
        const aida::analysis::typed_pseudocode_ast_node_t*> by_id;
    std::unordered_map<std::uint64_t,
        const aida::analysis::typed_pseudocode_ast_node_t*> parents;
};

struct state_t {
    std::mutex mutex;
    std::vector<tab_t> tabs;
    int active = -1;
    int selected_line = -1;
    std::uint32_t selected_token_begin = 0;
    std::uint32_t selected_token_end = 0;
    std::uint64_t generation = 0;
    bool renames_loaded = false;
    aida::analysis::binary_id_t binary_id;
    std::map<std::string, local_rename_map_t> local_renames;
    ast_index_cache_t ast_index;
};

std::filesystem::path local_rename_store_path(
    const aida::analysis::binary_id_t& binary_id)
{
    wchar_t* appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        auto dir = std::filesystem::path(appdata) / L"AiDA" / L"pseudocode_renames";
        CoTaskMemFree(appdata);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto name = binary_id.to_hex() + ".json";
        return dir / name;
    }
    return std::filesystem::current_path() / ("aida_pseudocode_renames_" + binary_id.to_hex() + ".json");
}

void load_local_renames(state_t& state, const aida::analysis::binary_id_t& binary_id)
{
    if (state.renames_loaded)
        return;
    state.renames_loaded = true;
    std::error_code ec;
    const auto path = local_rename_store_path(binary_id);
    if (!std::filesystem::exists(path, ec))
        return;
    try {
        nlohmann::json document = nlohmann::json::parse(std::ifstream(path));
        if (!document.is_object() || !document.contains("renames") ||
            !document["renames"].is_object())
            return;
        for (const auto& [identity, entries] : document["renames"].items()) {
            if (!entries.is_object() || identity.empty())
                continue;
            local_rename_map_t map;
            for (const auto& [old_name, new_name] : entries.items()) {
                if (!old_name.empty() && new_name.is_string() && !new_name.get<std::string>().empty())
                    map.emplace(old_name, new_name.get<std::string>());
            }
            if (!map.empty())
                state.local_renames.emplace(identity, std::move(map));
        }
    } catch (...) {
    }
}

void save_local_renames(const state_t& state, const aida::analysis::binary_id_t& binary_id)
{
    nlohmann::json document;
    document["version"] = 1;
    auto& renames = document["renames"];
    for (const auto& [identity, map] : state.local_renames) {
        if (map.empty())
            continue;
        auto& entries = renames[identity];
        for (const auto& [old_name, new_name] : map)
            entries[old_name] = new_name;
    }
    const auto path = local_rename_store_path(binary_id);
    const auto temporary = path.parent_path() / (path.filename().string() + ".tmp");
    try {
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << document.dump(2);
            output.flush();
            if (!output)
                return;
        }
        std::error_code ec;
        std::filesystem::rename(temporary, path, ec);
        if (ec)
            std::filesystem::remove(temporary, ec);
    } catch (...) {
    }
}

std::mutex& state_registry_mutex()
{
    static std::mutex value;
    return value;
}

std::unordered_map<aida::analysis::binary_id_t, std::shared_ptr<state_t>,
    aida::analysis::binary_id_hash_t>& state_registry()
{
    static std::unordered_map<aida::analysis::binary_id_t,
        std::shared_ptr<state_t>, aida::analysis::binary_id_hash_t> value;
    return value;
}

std::shared_ptr<state_t> state_for(
    const disasm_view::workspace_context_t& context)
{
    if (!context.workspace)
        return {};
    const auto binary_id = context.workspace->identity().binary_id();
    std::shared_ptr<state_t> created;
    {
        std::lock_guard<std::mutex> lock(state_registry_mutex());
        auto& registry = state_registry();
        const auto found = registry.find(binary_id);
        if (found != registry.end())
            return found->second;
        created = std::make_shared<state_t>();
        registry.emplace(binary_id, created);
    }
    {
        std::lock_guard<std::mutex> lock(created->mutex);
        created->binary_id = binary_id;
        load_local_renames(*created, binary_id);
    }
    return created;
}

std::optional<aida::analysis::address_t> function_address(
    const disasm_view::workspace_context_t& context,
    std::uint64_t address)
{
    const auto function = disasm_view::enclosing_function_start(address, context);
    if (function == 0)
        return std::nullopt;
    return disasm_view::typed_address(context, function);
}

std::uint64_t canonical_runtime_address(
    const disasm_view::workspace_context_t& context,
    const aida::analysis::address_t& address)
{
    return disasm_view::runtime_address(context, address).value_or(address.value);
}

std::string label_for(const disasm_view::workspace_context_t& context,
                      std::uint64_t address)
{
    const auto typed = function_address(context, address);
    if (typed) {
        auto label = disasm_view::resolve_name(context, *typed);
        if (!label.empty())
            return label;
    }
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "sub_%llX",
        static_cast<unsigned long long>(address));
    return buffer;
}

std::optional<std::size_t> find_tab(const state_t& state,
                                    std::uint64_t address)
{
    for (std::size_t index = 0; index < state.tabs.size(); ++index) {
        if (state.tabs[index].address == address)
            return index;
    }
    return std::nullopt;
}

std::optional<std::size_t> find_tab(const state_t& state,
                                    std::string_view entity_locator)
{
    if (entity_locator.empty())
        return std::nullopt;
    for (std::size_t index = 0; index < state.tabs.size(); ++index) {
        if (state.tabs[index].entity_locator == entity_locator)
            return index;
    }
    return std::nullopt;
}

std::string tab_identity(const tab_t& tab)
{
    return tab.entity_locator.empty()
        ? "native:" + std::to_string(tab.address)
        : tab.entity_locator;
}

void synchronize_tabs(
    const disasm_view::workspace_context_t& context,
    const aida::workbench::workbench_shell_workspace_context_t& workbench,
    const std::shared_ptr<state_t>& state)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto previous_active =
        state->active >= 0 &&
        static_cast<std::size_t>(state->active) < state->tabs.size()
            ? tab_identity(state->tabs[static_cast<std::size_t>(state->active)])
            : std::string();
    const bool generation_changed =
        state->generation != 0 && state->generation != workbench.analysis_generation;
    std::vector<tab_t> synchronized;
    synchronized.reserve(workbench.persistence.documents.size());
    int active = -1;
    for (const auto& document : workbench.persistence.documents) {
        if (document.identity.kind !=
                aida::workbench::document_kind_t::pseudocode)
            continue;
        const bool has_native_address =
            document.identity.has_address && document.identity.address != 0;
        std::string entity_locator;
        if (!has_native_address) {
            const auto& encoded = document.identity.provider_key != "analysis"
                ? document.identity.provider_key
                : document.local_state.selection.entity_key;
            const auto parsed =
                aida::workbench::pseudocode_document::
                    parse_pseudocode_entity_locator(
                        encoded);
            const auto canonical = parsed
                ? aida::workbench::pseudocode_document::
                    canonical_pseudocode_entity_locator(*parsed)
                : std::nullopt;
            if (canonical && *canonical == encoded)
                entity_locator = *canonical;
        }
        if (!has_native_address && entity_locator.empty())
            continue;
        tab_t tab;
        const auto existing = has_native_address
            ? find_tab(*state, document.identity.address)
            : find_tab(*state, entity_locator);
        if (existing)
            tab = std::move(state->tabs[*existing]);
        tab.address = has_native_address ? document.identity.address : 0;
        tab.entity_locator = std::move(entity_locator);
        tab.label = has_native_address
            ? label_for(context, tab.address) : tab.entity_locator;
        if (generation_changed) {
            if (tab.resolve_ticket != 0 && workbench.pseudocode_document)
                static_cast<void>(workbench.pseudocode_document->cancel_resolution(
                    tab.resolve_ticket));
            tab.request = {};
            tab.has_request = false;
            tab.job_id = 0;
            tab.state = pseudocode_cache_state_t::empty;
            tab.error.clear();
            tab.error_acknowledged = false;
            tab.resolve_ticket = 0;
            tab.resolve_force_refresh = false;
        }
        if (document.id == workbench.persistence.active_document)
            active = static_cast<int>(synchronized.size());
        synchronized.push_back(std::move(tab));
    }
    state->tabs = std::move(synchronized);
    state->active = active;
    state->generation = workbench.analysis_generation;
    const auto current_active =
        active >= 0 && static_cast<std::size_t>(active) < state->tabs.size()
            ? tab_identity(state->tabs[static_cast<std::size_t>(state->active)])
            : std::string();
    if (previous_active != current_active || generation_changed) {
        state->selected_line = -1;
        state->selected_token_begin = 0;
        state->selected_token_end = 0;
    }
}

std::string pseudocode_error_text(
    const aida::workbench::pseudocode_document::pseudocode_error_t& error)
{
    return "decompiler.document.error." +
        std::to_string(static_cast<unsigned>(error.code));
}

std::string first_diagnostic(
    aida::workbench::pseudocode_document::pseudocode_document_model_t& model)
{
    const auto diagnostics = model.diagnostics();
    if (diagnostics.empty())
        return {};
    if (!diagnostics.front().message.empty())
        return diagnostics.front().message;
    return diagnostics.front().localization_key;
}

void apply_resolution_outcomes(
    aida::workbench::pseudocode_document::pseudocode_document_model_t& model,
    const std::shared_ptr<state_t>& state,
    const std::vector<aida::workbench::pseudocode_document::
        pseudocode_document_model_t::resolution_outcome_t>& outcomes)
{
    if (outcomes.empty())
        return;
    std::lock_guard<std::mutex> lock(state->mutex);
    for (const auto& outcome : outcomes) {
        for (auto& tab : state->tabs) {
            if (tab.resolve_ticket == 0 || tab.resolve_ticket != outcome.ticket)
                continue;
            tab.resolve_ticket = 0;
            tab.resolve_force_refresh = false;
            if (outcome.submitted_request &&
                (outcome.error ||
                 outcome.error.code == aida::workbench::pseudocode_document::
                     pseudocode_error_code_t::request_in_progress)) {
                tab.request = outcome.request;
                tab.has_request = true;
                tab.error.clear();
                if (const auto* cached = model.cached_document(outcome.request)) {
                    tab.job_id = cached->job_id;
                    tab.state = cached->state;
                }
                diag::log_tagged_fmt("workbench",
                    "workbench.pseudocode.resolve completed ticket=%llu job_id=%llu",
                    static_cast<unsigned long long>(outcome.ticket),
                    static_cast<unsigned long long>(tab.job_id));
            } else {
                tab.has_request = false;
                tab.state = pseudocode_cache_state_t::failed;
                tab.error = pseudocode_error_text(outcome.error);
                const bool stale = outcome.error.code ==
                    aida::workbench::pseudocode_document::
                        pseudocode_error_code_t::stale_generation;
                if (stale) {
                    diag::log_tagged_fmt("workbench",
                        "workbench.pseudocode.resolve stale ticket=%llu generation=%llu",
                        static_cast<unsigned long long>(outcome.ticket),
                        static_cast<unsigned long long>(outcome.error.subject));
                } else {
                    diag::log_tagged_fmt("workbench",
                        "workbench.pseudocode.resolve failed ticket=%llu code=%u",
                        static_cast<unsigned long long>(outcome.ticket),
                        static_cast<unsigned>(outcome.error.code));
                }
            }
            break;
        }
    }
}

void reapply_tab_local_renames(
    aida::workbench::pseudocode_document::pseudocode_document_model_t& model,
    tab_t& tab,
    const std::shared_ptr<state_t>& state,
    const aida::analysis::binary_id_t& binary_id)
{
    const auto identity = tab_identity(tab);
    const auto found = state->local_renames.find(identity);
    if (found == state->local_renames.end() || found->second.empty()) {
        tab.renames_applied_job_id = tab.job_id;
        return;
    }
    bool map_changed = false;
    for (auto iterator = found->second.begin(); iterator != found->second.end();) {
        std::uint64_t renamed = 0;
        const auto applied = model.apply_local_rename(iterator->first, iterator->second, renamed);
        if (!applied || renamed == 0) {
            iterator = found->second.erase(iterator);
            map_changed = true;
        } else {
            ++iterator;
        }
    }
    if (found->second.empty()) {
        state->local_renames.erase(found);
        map_changed = true;
    }
    if (map_changed)
        save_local_renames(*state, binary_id);
    tab.renames_applied_job_id = tab.job_id;
}

void refresh_tab_states(
    aida::workbench::pseudocode_document::pseudocode_document_model_t& model,
    const std::shared_ptr<state_t>& state)
{
    apply_resolution_outcomes(model, state, model.drain_resolutions());
    const auto binary_id = state->binary_id;
    std::lock_guard<std::mutex> lock(state->mutex);
    for (auto& tab : state->tabs) {
        if (!tab.has_request)
            continue;
        auto activated = model.activate(tab.request);
        if (!activated) {
            tab.state = pseudocode_cache_state_t::failed;
            tab.error = pseudocode_error_text(activated);
            continue;
        }
        const auto* cached = model.cached_document(tab.request);
        if (cached && cached->state == pseudocode_cache_state_t::requesting) {
            static_cast<void>(model.poll(cached->job_id));
            static_cast<void>(model.activate(tab.request));
            cached = model.cached_document(tab.request);
        }
        if (!cached)
            continue;
        tab.job_id = cached->job_id;
        tab.state = cached->state;
        if (tab.state == pseudocode_cache_state_t::failed ||
            tab.state == pseudocode_cache_state_t::stale ||
            tab.state == pseudocode_cache_state_t::cancelled) {
            tab.error = first_diagnostic(model);
            if (tab.error.empty())
                tab.error = "decompiler.document.error." +
                    std::to_string(static_cast<unsigned>(tab.state));
        } else if (tab.state == pseudocode_cache_state_t::cached) {
            tab.error.clear();
            tab.error_acknowledged = false;
            if (tab.renames_applied_job_id != tab.job_id)
                reapply_tab_local_renames(model, tab, state, binary_id);
        }
    }
    if (state->active >= 0 &&
        static_cast<std::size_t>(state->active) < state->tabs.size()) {
        const auto& active = state->tabs[static_cast<std::size_t>(state->active)];
        if (active.has_request)
            static_cast<void>(model.activate(active.request));
    }
}

disasm_view::workspace_context_t selected_context()
{
    return disasm_view::capture_selected_workspace();
}

}

std::optional<aida::analysis::address_t> typed_source_address(
    const disasm_view::workspace_context_t& context,
    std::uint64_t address)
{
    if (context.workspace && context.image &&
        context.workspace->identity().target_kind() ==
            aida::analysis::target_kind_t::live_snapshot &&
        address < context.image->image_size() &&
        address <= (std::numeric_limits<std::uint64_t>::max)() -
            context.image->image_base())
        address += context.image->image_base();
    return disasm_view::typed_address(context, address);
}

std::optional<std::uint64_t> line_source_address(
    aida::workbench::pseudocode_document::pseudocode_document_model_t& model,
    const aida::workbench::pseudocode_document::pseudocode_line_view_t& line,
    const aida::workbench::pseudocode_document::pseudocode_page_t& page)
{
    aida::workbench::pseudocode_document::pseudocode_address_map_entry_t mapped;
    if (model.resolve_token(line.text_begin, mapped))
        return mapped.address;
    for (const auto& source_map : page.source_maps) {
        if (source_map.token_end <= line.text_begin ||
            source_map.token_begin >= line.text_end || !source_map.has_address)
            continue;
        return source_map.address;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> token_source_address(
    aida::workbench::pseudocode_document::pseudocode_document_model_t& model,
    const aida::workbench::pseudocode_document::pseudocode_token_view_t* token,
    const aida::workbench::pseudocode_document::pseudocode_line_view_t& line,
    const aida::workbench::pseudocode_document::pseudocode_page_t& page)
{
    if (token) {
        aida::workbench::pseudocode_document::pseudocode_address_map_entry_t mapped;
        if (model.resolve_token(token->range.begin, mapped))
            return mapped.address;
    }
    return line_source_address(model, line, page);
}

const aida::workbench::pseudocode_document::pseudocode_token_view_t*
token_for_position(
    const aida::workbench::pseudocode_document::pseudocode_page_t& page,
    std::uint32_t position)
{
    for (const auto& token : page.tokens) {
        if (token.range.begin <= position && position < token.range.end)
            return &token;
    }
    return nullptr;
}

std::string canonical_address_text(std::uint64_t address)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%016llX",
        static_cast<unsigned long long>(address));
    return buffer;
}

bool local_rename_identifier_text(const std::string& value)
{
    if (value.empty() || value.size() > 128)
        return false;
    const auto letter = [](const char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') || character == '_';
    };
    if (!letter(value.front()))
        return false;
    return std::all_of(value.begin() + 1, value.end(), [&](const char character) {
        return letter(character) || (character >= '0' && character <= '9');
    });
}

std::string local_rename_candidate(
    aida::workbench::pseudocode_document::pseudocode_document_model_t& model,
    const aida::workbench::pseudocode_document::pseudocode_page_t& page,
    std::uint32_t token_begin,
    const disasm_view::workspace_context_t& context)
{
    const auto state = state_for(context);
    if (!state)
        return {};
    const auto* cached = model.cached_document();
    if (!cached || !cached->document)
        return {};
    const aida::workbench::pseudocode_document::pseudocode_token_view_t* token = nullptr;
    for (const auto& candidate : page.tokens) {
        if (candidate.range.begin == token_begin) {
            token = &candidate;
            break;
        }
    }
    if (token == nullptr ||
        token->kind != aida::analysis::decompiler_document_token_kind_t::identifier ||
        !local_rename_identifier_text(token->text))
        return {};
    const auto& ast = cached->document->ast;
    auto& index = state->ast_index;
    if (index.document != static_cast<const void*>(cached->document.get())) {
        index.document = cached->document.get();
        index.by_id.clear();
        index.parents.clear();
        index.by_id.reserve(ast.nodes.size());
        for (const auto& node : ast.nodes)
            index.by_id.emplace(node.id, &node);
        index.parents.reserve(ast.nodes.size());
        for (const auto& node : ast.nodes) {
            for (const auto child_id : node.child_ids)
                index.parents.emplace(child_id, &node);
        }
    }
    const aida::analysis::typed_pseudocode_ast_node_t* ast_node = nullptr;
    const aida::analysis::typed_pseudocode_ast_node_t* root = nullptr;
    const auto node_it = index.by_id.find(token->ast_node_id);
    if (node_it != index.by_id.end())
        ast_node = node_it->second;
    const auto root_it = index.by_id.find(ast.root_node_id);
    if (root_it != index.by_id.end())
        root = root_it->second;
    if (ast_node == nullptr ||
        (ast_node->kind != aida::analysis::typed_pseudocode_ast_node_kind_t::declaration &&
         ast_node->kind != aida::analysis::typed_pseudocode_ast_node_kind_t::identifier) ||
        ast_node->stable_text != token->text)
        return {};
    if (root != nullptr && root->stable_text == token->text)
        return {};
    const auto parent = index.parents.find(ast_node->id);
    if (parent != index.parents.end() &&
        parent->second->kind == aida::analysis::typed_pseudocode_ast_node_kind_t::call_expression &&
        !parent->second->child_ids.empty() &&
        parent->second->child_ids.front() == ast_node->id)
        return {};
    return token->text;
}

bool apply_active_local_rename(const disasm_view::workspace_context_t& context,
                               const std::string& old_name,
                               const std::string& new_name,
                               std::string& error)
{
    if (new_name.empty()) {
        error = "The new name cannot be empty";
        return false;
    }
    auto* model = document_model(context);
    auto state = state_for(context);
    if (!model || !state) {
        error = "The workspace decompiler model is unavailable.";
        return false;
    }
    std::uint64_t renamed = 0;
    const auto applied = model->apply_local_rename(old_name, new_name, renamed);
    if (!applied || renamed == 0) {
        error = "The name is not a renameable function-local identifier " +
            std::to_string(static_cast<unsigned>(applied.code));
        return false;
    }
    const auto binary_id = context.workspace->identity().binary_id();
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->active >= 0 &&
        static_cast<std::size_t>(state->active) < state->tabs.size()) {
        auto& tab = state->tabs[static_cast<std::size_t>(state->active)];
        auto& map = state->local_renames[tab_identity(tab)];
        map[old_name] = new_name;
        tab.renames_applied_job_id = tab.job_id;
    }
    save_local_renames(*state, binary_id);
    error.clear();
    return true;
}

void acknowledge_active_error(const disasm_view::workspace_context_t& context)
{
    auto state = state_for(context);
    if (!state)
        return;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->active >= 0 &&
        static_cast<std::size_t>(state->active) < state->tabs.size())
        state->tabs[static_cast<std::size_t>(state->active)].error_acknowledged = true;
}

void navigate_to_disassembly(
    const disasm_view::workspace_context_t& context,
    std::uint64_t source_address)
{
    const auto typed = typed_source_address(context, source_address);
    const auto target = typed ? canonical_runtime_address(context, *typed) : source_address;
    aida::workbench::selection_context_t selection;
    selection.kind = aida::workbench::selection_kind_t::address;
    selection.has_address = true;
    selection.address = target;
    aida::workbench::document_local_cursor_t cursor;
    cursor.has_position = true;
    cursor.position = target;
    aida::workbench::workbench_shell_workspace_context_t workbench;
    static_cast<void>(
        aida::workbench::workbench_shell_runtime_t::instance()
            .navigate_document(context.workspace,
                aida::workbench::document_kind_t::disassembly,
                std::nullopt, selection, cursor, workbench));
    disasm_view::goto_address(target, context);
    const auto focus = aida::qt::analysis_bridge::view_focus_hook();
    if (focus)
        focus("document.disassembly");
}

void navigate_to_graph(const disasm_view::workspace_context_t& context,
                       std::uint64_t source_address)
{
    const auto typed = typed_source_address(context, source_address);
    const auto runtime = typed
        ? canonical_runtime_address(context, *typed) : source_address;
    const auto function = disasm_view::enclosing_function_start(runtime, context);
    if (function == 0)
        return;
    cfg_view::build_cfg(context, function);
    const auto focus = aida::qt::analysis_bridge::view_focus_hook();
    if (focus)
        focus("document.graph");
}

bool navigate_to_hex(const disasm_view::workspace_context_t& context,
                     const aida::analysis::address_t& address)
{
    std::string error;
    if (!hex_view::focus_address(context, address, &error))
        return false;
    const auto focus = aida::qt::analysis_bridge::view_focus_hook();
    if (focus)
        focus("document.hex");
    return true;
}

void persist_line_selection(
    const disasm_view::workspace_context_t& context,
    std::uint64_t document_address,
    std::string_view entity_locator,
    const aida::workbench::pseudocode_document::pseudocode_line_view_t& line,
    std::optional<std::uint64_t> source_address)
{
    aida::workbench::selection_context_t selection;
    selection.kind = source_address
        ? aida::workbench::selection_kind_t::address
        : aida::workbench::selection_kind_t::source;
    if (source_address) {
        const auto typed = typed_source_address(context, *source_address);
        selection.has_address = true;
        selection.address = typed ? canonical_runtime_address(context, *typed) : *source_address;
    } else {
        selection.entity_key = "pseudocode.line." +
            std::to_string(line.line_number);
    }
    aida::workbench::document_local_cursor_t cursor;
    cursor.has_position = true;
    cursor.position = line.line_number;
    aida::workbench::workbench_shell_workspace_context_t workbench;
    if (!entity_locator.empty()) {
        static_cast<void>(
            aida::workbench::workbench_shell_runtime_t::instance()
                .navigate_entity_document(context.workspace,
                    aida::workbench::document_kind_t::pseudocode,
                    entity_locator, selection, cursor, workbench));
    } else {
        static_cast<void>(
            aida::workbench::workbench_shell_runtime_t::instance()
                .navigate_document(context.workspace,
                    aida::workbench::document_kind_t::pseudocode,
                    document_address, selection, cursor, workbench));
    }
}

bool workbench_context(
    const disasm_view::workspace_context_t& context,
    aida::workbench::workbench_shell_workspace_context_t& output)
{
    output = {};
    if (!context || !context.workspace || context.workspace->closing() ||
        context.workspace->closed())
        return false;
    return static_cast<bool>(
        aida::workbench::workbench_shell_runtime_t::instance()
            .workspace_context(context.workspace, output));
}

aida::workbench::pseudocode_document::pseudocode_document_model_t*
document_model(const disasm_view::workspace_context_t& context)
{
    aida::workbench::workbench_shell_workspace_context_t workbench;
    if (!workbench_context(context, workbench) || !workbench.pseudocode_document)
        return nullptr;
    return workbench.pseudocode_document;
}

void refresh_tab_states(const disasm_view::workspace_context_t& context)
{
    aida::workbench::workbench_shell_workspace_context_t workbench;
    auto state = state_for(context);
    if (!state || !workbench_context(context, workbench) ||
        !workbench.pseudocode_document)
        return;
    refresh_tab_states(*workbench.pseudocode_document, state);
}

bool any_tab_requesting(const disasm_view::workspace_context_t& context)
{
    auto state = state_for(context);
    if (!state)
        return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    for (const auto& tab : state->tabs) {
        if (tab.resolve_ticket != 0)
            return true;
        if (tab.has_request && tab.state == pseudocode_cache_state_t::requesting)
            return true;
    }
    return false;
}

void set_line_selection(const disasm_view::workspace_context_t& context,
                        int line, std::uint32_t token_begin, std::uint32_t token_end)
{
    auto state = state_for(context);
    if (!state)
        return;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->selected_line = line;
    state->selected_token_begin = token_begin;
    state->selected_token_end = token_end;
}

line_selection_t line_selection(const disasm_view::workspace_context_t& context)
{
    line_selection_t result;
    auto state = state_for(context);
    if (!state)
        return result;
    std::lock_guard<std::mutex> lock(state->mutex);
    result.selected_line = state->selected_line;
    result.selected_token_begin = state->selected_token_begin;
    result.selected_token_end = state->selected_token_end;
    return result;
}

std::vector<tab_view_t> snapshot_tab_views(
    const disasm_view::workspace_context_t& context)
{
    std::vector<tab_view_t> output;
    aida::workbench::workbench_shell_workspace_context_t workbench;
    auto state = state_for(context);
    if (!state || !workbench_context(context, workbench))
        return output;
    synchronize_tabs(context, workbench, state);
    std::lock_guard<std::mutex> lock(state->mutex);
    output.reserve(state->tabs.size());
    for (const auto& tab : state->tabs) {
        tab_view_t view;
        view.address = tab.address;
        view.entity_locator = tab.entity_locator;
        view.label = tab.label;
        view.request = tab.request;
        view.has_request = tab.has_request;
        view.job_id = tab.job_id;
        view.state = tab.state;
        view.error = tab.error;
        view.error_acknowledged = tab.error_acknowledged;
        view.resolve_ticket = tab.resolve_ticket;
        output.push_back(std::move(view));
    }
    return output;
}

int active_tab_index(const disasm_view::workspace_context_t& context)
{
    auto state = state_for(context);
    if (!state)
        return -1;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->active;
}

std::optional<tab_view_t> active_tab_view(
    const disasm_view::workspace_context_t& context)
{
    auto state = state_for(context);
    if (!state)
        return std::nullopt;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->active < 0 ||
        static_cast<std::size_t>(state->active) >= state->tabs.size())
        return std::nullopt;
    const auto& tab = state->tabs[static_cast<std::size_t>(state->active)];
    tab_view_t view;
    view.address = tab.address;
    view.entity_locator = tab.entity_locator;
    view.label = tab.label;
    view.request = tab.request;
    view.has_request = tab.has_request;
    view.job_id = tab.job_id;
    view.state = tab.state;
    view.error = tab.error;
    view.error_acknowledged = tab.error_acknowledged;
    view.resolve_ticket = tab.resolve_ticket;
    return view;
}

void request_decompile(const disasm_view::workspace_context_t& context,
                       std::uint64_t address, bool force_refresh)
{
    if (!context || context.workspace->closing() || context.workspace->closed())
        return;
    const auto typed = function_address(context, address);
    if (!typed)
        return;
    const auto canonical_address = canonical_runtime_address(context, *typed);
    auto state = state_for(context);
    if (!state)
        return;
    aida::workbench::workbench_shell_workspace_context_t workbench;
    const auto activated =
        aida::workbench::workbench_shell_runtime_t::instance()
            .activate_document(context.workspace,
                aida::workbench::document_kind_t::pseudocode,
                canonical_address, workbench);
    if (!activated || !workbench.pseudocode_document)
        return;
    synchronize_tabs(context, workbench, state);

    aida::analysis::decompiler_entity_locator_t locator;
    locator.address = typed->value;
    std::uint64_t ticket = 0;
    const auto submitted = workbench.pseudocode_document->request_async(
        locator, aida::analysis::decompiler_profile_id_t::balanced,
        aida::workbench::pseudocode_document::
            k_pseudocode_document_default_timeout_ms,
        force_refresh, ticket);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto index = find_tab(*state, canonical_address);
        if (!index)
            return;
        auto& tab = state->tabs[*index];
        tab.error_acknowledged = false;
        if (!submitted) {
            tab.request = {};
            tab.has_request = false;
            tab.job_id = 0;
            tab.resolve_ticket = 0;
            tab.resolve_force_refresh = false;
            tab.state = pseudocode_cache_state_t::failed;
            tab.error = pseudocode_error_text(submitted);
            return;
        }
        tab.request = {};
        tab.has_request = false;
        tab.job_id = 0;
        tab.resolve_ticket = ticket;
        tab.resolve_force_refresh = force_refresh;
        tab.state = pseudocode_cache_state_t::requesting;
        tab.error.clear();
    }
}

void request_decompile(
    const disasm_view::workspace_context_t& context,
    const aida::analysis::decompiler_entity_locator_t& locator,
    std::string_view canonical_locator,
    bool force_refresh)
{
    if (!context || context.workspace->closing() || context.workspace->closed())
        return;
    const auto parsed =
        aida::workbench::pseudocode_document::
            parse_pseudocode_entity_locator(canonical_locator);
    const auto canonical =
        aida::workbench::pseudocode_document::
            canonical_pseudocode_entity_locator(locator);
    if (!parsed || !canonical || *canonical != canonical_locator ||
        parsed->token != locator.token ||
        parsed->artifact_ordinal != locator.artifact_ordinal ||
        parsed->expected_kind != locator.expected_kind)
        return;
    auto state = state_for(context);
    if (!state)
        return;
    aida::workbench::workbench_shell_workspace_context_t workbench;
    const auto activated =
        aida::workbench::workbench_shell_runtime_t::instance()
            .activate_entity_document(context.workspace,
                aida::workbench::document_kind_t::pseudocode,
                *canonical, workbench);
    if (!activated || !workbench.pseudocode_document)
        return;
    synchronize_tabs(context, workbench, state);

    std::uint64_t ticket = 0;
    const auto submitted = workbench.pseudocode_document->request_async(
        locator, aida::analysis::decompiler_profile_id_t::balanced,
        aida::workbench::pseudocode_document::
            k_pseudocode_document_default_timeout_ms,
        force_refresh, ticket);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto index = find_tab(*state, *canonical);
        if (!index)
            return;
        auto& tab = state->tabs[*index];
        tab.error_acknowledged = false;
        if (!submitted) {
            tab.request = {};
            tab.has_request = false;
            tab.job_id = 0;
            tab.resolve_ticket = 0;
            tab.resolve_force_refresh = false;
            tab.state = pseudocode_cache_state_t::failed;
            tab.error = pseudocode_error_text(submitted);
            return;
        }
        tab.request = {};
        tab.has_request = false;
        tab.job_id = 0;
        tab.resolve_ticket = ticket;
        tab.resolve_force_refresh = force_refresh;
        tab.state = pseudocode_cache_state_t::requesting;
        tab.error.clear();
    }
}

void request_decompile(std::uint64_t address, const DisasmFile*, bool force_refresh)
{
    request_decompile(selected_context(), address, force_refresh);
}

void close_tab_by_addr(const disasm_view::workspace_context_t& context,
                       std::uint64_t address)
{
    if (!context)
        return;
    const auto typed = function_address(context, address);
    if (!typed)
        return;
    const auto canonical_address = canonical_runtime_address(context, *typed);
    aida::workbench::workbench_shell_workspace_context_t workbench;
    if (workbench_context(context, workbench) && workbench.pseudocode_document) {
        auto state = state_for(context);
        if (state) {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto index = find_tab(*state, canonical_address);
            if (index) {
                if (state->tabs[*index].resolve_ticket != 0)
                    static_cast<void>(workbench.pseudocode_document->cancel_resolution(
                        state->tabs[*index].resolve_ticket));
                if (state->tabs[*index].has_request &&
                    state->tabs[*index].state == pseudocode_cache_state_t::requesting)
                    static_cast<void>(workbench.pseudocode_document->cancel(
                        state->tabs[*index].job_id));
            }
        }
    }
    static_cast<void>(
        aida::workbench::workbench_shell_runtime_t::instance()
            .close_document(context.workspace,
                aida::workbench::document_kind_t::pseudocode,
                canonical_address, workbench));
    if (auto state = state_for(context))
        synchronize_tabs(context, workbench, state);
}

void close_tab_by_entity(const disasm_view::workspace_context_t& context,
                         std::string_view canonical_locator)
{
    if (!context)
        return;
    const auto parsed =
        aida::workbench::pseudocode_document::
            parse_pseudocode_entity_locator(canonical_locator);
    const auto canonical = parsed
        ? aida::workbench::pseudocode_document::
            canonical_pseudocode_entity_locator(*parsed)
        : std::nullopt;
    if (!canonical || *canonical != canonical_locator)
        return;
    aida::workbench::workbench_shell_workspace_context_t workbench;
    if (workbench_context(context, workbench) && workbench.pseudocode_document) {
        auto state = state_for(context);
        if (state) {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto index = find_tab(*state, *canonical);
            if (index) {
                if (state->tabs[*index].resolve_ticket != 0)
                    static_cast<void>(workbench.pseudocode_document->cancel_resolution(
                        state->tabs[*index].resolve_ticket));
                if (state->tabs[*index].has_request &&
                    state->tabs[*index].state == pseudocode_cache_state_t::requesting)
                    static_cast<void>(workbench.pseudocode_document->cancel(
                        state->tabs[*index].job_id));
            }
        }
    }
    static_cast<void>(
        aida::workbench::workbench_shell_runtime_t::instance()
            .close_entity_document(context.workspace,
                aida::workbench::document_kind_t::pseudocode,
                *canonical, workbench));
    if (auto state = state_for(context))
        synchronize_tabs(context, workbench, state);
}

void close_active_tab(const disasm_view::workspace_context_t& context)
{
    auto state = state_for(context);
    if (!state)
        return;
    tab_t tab;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->active < 0 ||
            static_cast<std::size_t>(state->active) >= state->tabs.size())
            return;
        tab = state->tabs[static_cast<std::size_t>(state->active)];
    }
    if (!tab.entity_locator.empty())
        close_tab_by_entity(context, tab.entity_locator);
    else if (tab.address != 0)
        close_tab_by_addr(context, tab.address);
}

void close_all_tabs(const disasm_view::workspace_context_t& context)
{
    const auto tabs = snapshot_tabs(context);
    for (const auto& tab : tabs) {
        if (!tab.entity_locator.empty())
            close_tab_by_entity(context, tab.entity_locator);
        else
            close_tab_by_addr(context, tab.addr);
    }
}

void activate_tab_by_addr(const disasm_view::workspace_context_t& context,
                          std::uint64_t address)
{
    if (!context)
        return;
    const auto typed = function_address(context, address);
    if (!typed)
        return;
    const auto canonical_address = canonical_runtime_address(context, *typed);
    aida::workbench::workbench_shell_workspace_context_t workbench;
    const auto activated =
        aida::workbench::workbench_shell_runtime_t::instance()
            .activate_document(context.workspace,
                aida::workbench::document_kind_t::pseudocode,
                canonical_address, workbench);
    if (!activated)
        return;
    if (auto state = state_for(context)) {
        synchronize_tabs(context, workbench, state);
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto index = find_tab(*state, canonical_address);
        if (index && state->tabs[*index].has_request &&
            workbench.pseudocode_document)
            static_cast<void>(workbench.pseudocode_document->activate(
                state->tabs[*index].request));
    }
}

void activate_tab_by_entity(const disasm_view::workspace_context_t& context,
                            std::string_view canonical_locator)
{
    if (!context)
        return;
    const auto parsed =
        aida::workbench::pseudocode_document::
            parse_pseudocode_entity_locator(canonical_locator);
    const auto canonical = parsed
        ? aida::workbench::pseudocode_document::
            canonical_pseudocode_entity_locator(*parsed)
        : std::nullopt;
    if (!canonical || *canonical != canonical_locator)
        return;
    aida::workbench::workbench_shell_workspace_context_t workbench;
    const auto activated =
        aida::workbench::workbench_shell_runtime_t::instance()
            .activate_entity_document(context.workspace,
                aida::workbench::document_kind_t::pseudocode,
                *canonical, workbench);
    if (!activated)
        return;
    if (auto state = state_for(context)) {
        synchronize_tabs(context, workbench, state);
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto index = find_tab(*state, *canonical);
        if (index && state->tabs[*index].has_request &&
            workbench.pseudocode_document)
            static_cast<void>(workbench.pseudocode_document->activate(
                state->tabs[*index].request));
    }
}

void cancel_active_decompile(const disasm_view::workspace_context_t& context)
{
    aida::workbench::workbench_shell_workspace_context_t workbench;
    auto state = state_for(context);
    if (!state || !workbench_context(context, workbench) ||
        !workbench.pseudocode_document)
        return;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->active < 0 ||
        static_cast<std::size_t>(state->active) >= state->tabs.size())
        return;
    auto& tab = state->tabs[static_cast<std::size_t>(state->active)];
    if (tab.resolve_ticket != 0) {
        static_cast<void>(workbench.pseudocode_document->cancel_resolution(
            tab.resolve_ticket));
        tab.resolve_ticket = 0;
        tab.resolve_force_refresh = false;
        tab.state = pseudocode_cache_state_t::cancelled;
        return;
    }
    if (!tab.has_request || tab.state != pseudocode_cache_state_t::requesting)
        return;
    static_cast<void>(workbench.pseudocode_document->activate(tab.request));
    const auto cancelled = workbench.pseudocode_document->cancel(tab.job_id);
    if (cancelled)
        tab.state = pseudocode_cache_state_t::cancelled;
}

void refresh_active_tab(const disasm_view::workspace_context_t& context)
{
    auto state = state_for(context);
    if (!state)
        return;
    tab_t tab;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->active < 0 ||
            static_cast<std::size_t>(state->active) >= state->tabs.size())
            return;
        tab = state->tabs[static_cast<std::size_t>(state->active)];
    }
    if (!tab.entity_locator.empty()) {
        const auto locator =
            aida::workbench::pseudocode_document::
                parse_pseudocode_entity_locator(tab.entity_locator);
        if (locator)
            request_decompile(context, *locator, tab.entity_locator, true);
    } else if (tab.address != 0) {
        request_decompile(context, tab.address, true);
    }
}

void refresh_all_tabs(const disasm_view::workspace_context_t& context)
{
    const auto tabs = snapshot_tabs(context);
    for (const auto& tab : tabs) {
        if (!tab.entity_locator.empty()) {
            const auto locator =
                aida::workbench::pseudocode_document::
                    parse_pseudocode_entity_locator(tab.entity_locator);
            if (locator)
                request_decompile(
                    context, *locator, tab.entity_locator, true);
        } else {
            request_decompile(context, tab.addr, true);
        }
    }
}

bool has_active_tab(const disasm_view::workspace_context_t& context)
{
    aida::workbench::workbench_shell_workspace_context_t workbench;
    auto state = state_for(context);
    if (!state || !workbench_context(context, workbench))
        return false;
    synchronize_tabs(context, workbench, state);
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->active >= 0 &&
        static_cast<std::size_t>(state->active) < state->tabs.size();
}

bool has_tab_for(const disasm_view::workspace_context_t& context,
                 std::uint64_t address)
{
    const auto typed = function_address(context, address);
    aida::workbench::workbench_shell_workspace_context_t workbench;
    auto state = state_for(context);
    if (!typed || !state || !workbench_context(context, workbench))
        return false;
    synchronize_tabs(context, workbench, state);
    std::lock_guard<std::mutex> lock(state->mutex);
    return find_tab(*state, canonical_runtime_address(context, *typed)).has_value();
}

std::uint64_t active_tab_address(const disasm_view::workspace_context_t& context)
{
    aida::workbench::workbench_shell_workspace_context_t workbench;
    auto state = state_for(context);
    if (!state || !workbench_context(context, workbench))
        return 0;
    synchronize_tabs(context, workbench, state);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->active < 0 ||
        static_cast<std::size_t>(state->active) >= state->tabs.size())
        return 0;
    return state->tabs[static_cast<std::size_t>(state->active)].address;
}

int tab_count(const disasm_view::workspace_context_t& context)
{
    aida::workbench::workbench_shell_workspace_context_t workbench;
    auto state = state_for(context);
    if (!state || !workbench_context(context, workbench))
        return 0;
    synchronize_tabs(context, workbench, state);
    std::lock_guard<std::mutex> lock(state->mutex);
    return static_cast<int>((std::min)(state->tabs.size(),
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
}

std::vector<tab_info_t> snapshot_tabs(
    const disasm_view::workspace_context_t& context)
{
    std::vector<tab_info_t> output;
    aida::workbench::workbench_shell_workspace_context_t workbench;
    auto state = state_for(context);
    if (!state || !workbench_context(context, workbench))
        return output;
    synchronize_tabs(context, workbench, state);
    if (workbench.pseudocode_document)
        refresh_tab_states(*workbench.pseudocode_document, state);
    std::lock_guard<std::mutex> lock(state->mutex);
    output.reserve(state->tabs.size());
    for (const auto& tab : state->tabs) {
        tab_info_t info;
        info.addr = tab.address;
        info.label = tab.label;
        info.function_name = tab.label;
        info.entity_locator = tab.entity_locator;
        info.loaded = tab.state == pseudocode_cache_state_t::cached;
        info.decompiling = tab.state == pseudocode_cache_state_t::requesting;
        info.is_error = tab.state == pseudocode_cache_state_t::failed ||
                        tab.state == pseudocode_cache_state_t::stale;
        output.push_back(std::move(info));
    }
    return output;
}

void close_active_tab() { close_active_tab(selected_context()); }
void close_all_tabs() { close_all_tabs(selected_context()); }
void close_tab_by_addr(std::uint64_t address)
{
    close_tab_by_addr(selected_context(), address);
}
void activate_tab_by_addr(std::uint64_t address)
{
    activate_tab_by_addr(selected_context(), address);
}
void cancel_active_decompile() { cancel_active_decompile(selected_context()); }
void refresh_active_tab() { refresh_active_tab(selected_context()); }
void refresh_all_tabs() { refresh_all_tabs(selected_context()); }
bool has_active_tab() { return has_active_tab(selected_context()); }
bool has_tab_for(std::uint64_t address)
{
    return has_tab_for(selected_context(), address);
}
std::uint64_t active_tab_address()
{
    return active_tab_address(selected_context());
}
int tab_count() { return tab_count(selected_context()); }
std::vector<tab_info_t> snapshot_tabs()
{
    return snapshot_tabs(selected_context());
}

}
