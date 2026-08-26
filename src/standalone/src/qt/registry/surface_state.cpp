#include "qt/registry/surface_state.hpp"
#include "qt/registry/view_catalog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace aida::qt::registry {

namespace {

constexpr std::uint64_t k_scroll_milli_read_cap = 1000000000000000ULL;
constexpr float k_scroll_write_cap = 1000000000000.0f;

bool is_disassembly_side_key(const stable_view_id_t& view,
                             const stable_view_instance_key_t& instance) noexcept {
    if (view.value() != "document.disassembly")
        return false;
    const std::string& key = instance.value();
    return key == "side.1" || key == "side.2" || key == "side.3";
}

std::string identity_string(const stable_view_id_t& view,
                            const stable_view_instance_key_t& instance) {
    std::string key = "aida.";
    key.append(view.value());
    if (!instance.empty()) {
        key.push_back('.');
        key.append(instance.value());
    }
    return key;
}

void write_presentation(nlohmann::json& target,
                        const disasm_view::presentation_snapshot_t& snapshot) {
    nlohmann::json presentation = nlohmann::json::object();
    presentation["format"] = static_cast<int>(snapshot.addr_format);
    presentation["show_bytes"] = snapshot.show_bytes ? 1 : 0;
    presentation["section"] = snapshot.active_section;
    if (snapshot.display_image_base)
        presentation["base"] = static_cast<std::uint64_t>(*snapshot.display_image_base);
    else
        presentation["base"] = nullptr;
    if (snapshot.selection) {
        const auto& selection = *snapshot.selection;
        presentation["selection"] = {
            {"space", static_cast<int>(selection.space)},
            {"value", static_cast<std::uint64_t>(selection.value)},
            {"architecture", static_cast<int>(selection.architecture)},
            {"mode", static_cast<int>(selection.mode)}
        };
    } else {
        presentation["selection"] = nullptr;
    }
    const auto scroll_milli = static_cast<unsigned long long>(
        std::llround((std::clamp)(snapshot.scroll_y, 0.0f, k_scroll_write_cap) * 1000.0f));
    presentation["scroll_milli"] = scroll_milli;
    target = std::move(presentation);
}

bool read_presentation(const nlohmann::json& source,
                       disasm_view::presentation_snapshot_t& snapshot) {
    try {
        if (!source.is_object())
            return false;
        if (!source.contains("format") || !source["format"].is_number_integer())
            return false;
        const int format = source["format"].get<int>();
        if (format < static_cast<int>(disasm_view::addr_format_t::va) ||
            format > static_cast<int>(disasm_view::addr_format_t::file_offset))
            return false;
        snapshot.addr_format = static_cast<disasm_view::addr_format_t>(format);
        snapshot.show_bytes = source.value("show_bytes", 1) != 0;
        snapshot.active_section = source.value("section", -1);
        snapshot.display_image_base.reset();
        if (source.contains("base") && source["base"].is_number_unsigned())
            snapshot.display_image_base = source["base"].get<std::uint64_t>();
        snapshot.selection.reset();
        if (source.contains("selection") && source["selection"].is_object()) {
            const auto& selection_json = source["selection"];
            aida::analysis::address_t selection;
            selection.space = static_cast<aida::analysis::address_space_id_t>(
                selection_json.value("space", 0));
            selection.value = selection_json.value("value",
                static_cast<std::uint64_t>(0));
            selection.architecture = static_cast<aida::analysis::architecture_id_t>(
                selection_json.value("architecture", 0));
            selection.mode = static_cast<aida::analysis::architecture_mode_t>(
                selection_json.value("mode", 0));
            snapshot.selection = selection;
        }
        std::uint64_t scroll_milli = 0;
        if (source.contains("scroll_milli") && source["scroll_milli"].is_number_unsigned())
            scroll_milli = source["scroll_milli"].get<std::uint64_t>();
        snapshot.scroll_y = static_cast<float>(
            (std::min)(scroll_milli, k_scroll_milli_read_cap)) / 1000.0f;
        return true;
    } catch (...) {
        return false;
    }
}

}

surface_state_store_t::entry_t* surface_state_store_t::find(const view_instance_id_t& id) noexcept {
    const auto found = entries_.find(identity_string(id.view, id.instance));
    return found == entries_.end() ? nullptr : &found->second;
}

const surface_state_store_t::entry_t* surface_state_store_t::find(
    const view_instance_id_t& id) const noexcept {
    const auto found = entries_.find(identity_string(id.view, id.instance));
    return found == entries_.end() ? nullptr : &found->second;
}

surface_state_store_t::entry_t& surface_state_store_t::ensure(const view_instance_id_t& id) noexcept {
    return entries_[identity_string(id.view, id.instance)];
}

surface_state_store_t::entry_t& surface_state_store_t::ensure_key(
    const std::string& identity) noexcept {
    return entries_[identity];
}

surface_state_store_t::entry_t* surface_state_store_t::find_hub(hub_kind_t hub) noexcept {
    if (hub == hub_kind_t::none)
        return nullptr;
    const auto found = entries_.find(hub_object_name(hub));
    return found == entries_.end() ? nullptr : &found->second;
}

const surface_state_store_t::entry_t* surface_state_store_t::find_hub(
    hub_kind_t hub) const noexcept {
    if (hub == hub_kind_t::none)
        return nullptr;
    const auto found = entries_.find(hub_object_name(hub));
    return found == entries_.end() ? nullptr : &found->second;
}

surface_state_store_t::entry_t& surface_state_store_t::ensure_hub(hub_kind_t hub) noexcept {
    return entries_[hub_object_name(hub)];
}

void surface_state_store_t::erase(const view_instance_id_t& id) noexcept {
    entries_.erase(identity_string(id.view, id.instance));
}

void surface_state_store_t::clear() noexcept {
    entries_.clear();
    hub_subviews_.clear();
}

void surface_state_store_t::for_each(
    const std::function<void(const std::string& identity, const entry_t&)>& visitor) const {
    if (!visitor)
        return;
    for (const auto& entry : entries_)
        visitor(entry.first, entry.second);
}

void surface_state_store_t::set_hub_subview(hub_kind_t hub, int subview) noexcept {
    if (hub == hub_kind_t::none || subview < 0)
        return;
    hub_subviews_[hub] = subview;
    ++revision_;
}

std::optional<int> surface_state_store_t::hub_subview(hub_kind_t hub) const noexcept {
    const auto found = hub_subviews_.find(hub);
    if (found == hub_subviews_.end())
        return std::nullopt;
    return found->second;
}

surface_identity_parse_t parse_surface_identity(std::string_view key) {
    surface_identity_parse_t result;
    if (key.compare(0, 5, "aida.") != 0)
        return result;
    const std::string_view stem = key.substr(5);
    if (stem.compare(0, 4, "hub.") == 0) {
        const std::string_view hub_name = stem.substr(4);
        for (const hub_kind_t hub : {hub_kind_t::analysis, hub_kind_t::scan,
                hub_kind_t::types, hub_kind_t::debugger, hub_kind_t::network}) {
            if (hub_name == hub_kind_name(hub)) {
                result.hub = hub;
                result.valid = true;
                return result;
            }
        }
        return result;
    }
    const catalog_entry_t* best = nullptr;
    std::size_t matched_stem_length = 0;
    for (const auto& entry : k_catalog) {
        const std::string_view id(entry.id);
        if (stem == id) {
            best = &entry;
            matched_stem_length = id.size();
            break;
        }
        if (stem.size() > id.size() && stem.compare(0, id.size(), id) == 0 &&
            stem[id.size()] == '.' && id.size() > matched_stem_length) {
            best = &entry;
            matched_stem_length = id.size();
        }
    }
    if (!best) {
        for (const auto& entry : k_catalog) {
            if (!entry.persistence_alias)
                continue;
            const std::string_view alias(entry.persistence_alias);
            if (stem == alias) {
                best = &entry;
                matched_stem_length = alias.size();
                break;
            }
            if (stem.size() > alias.size() && stem.compare(0, alias.size(), alias) == 0 &&
                stem[alias.size()] == '.') {
                best = &entry;
                matched_stem_length = alias.size();
                break;
            }
        }
    }
    if (!best)
        return result;
    const std::string_view canonical(best->id);
    std::string_view remainder;
    if (stem.size() > matched_stem_length)
        remainder = stem.substr(matched_stem_length + 1);
    if (!remainder.empty() &&
        !aida::ui::is_valid_stable_instance_key(std::string(remainder)))
        return result;
    const stable_view_id_t view{std::string(canonical)};
    const stable_view_instance_key_t instance{std::string(remainder)};
    if (!instance.empty() && !is_disassembly_side_key(view, instance) &&
        view.value() != "document.code")
        return result;
    result.view = view;
    result.instance = instance;
    result.valid = true;
    return result;
}

std::string capture_surface_state_json(const surface_state_store_t& store) {
    nlohmann::json root = nlohmann::json::object();
    root["version"] = 1;
    nlohmann::json surfaces = nlohmann::json::object();
    store.for_each([&](const std::string& identity, const surface_state_store_t::entry_t& entry) {
        if (!entry.pinned && !entry.restore_open)
            return;
        nlohmann::json record = nlohmann::json::object();
        if (entry.pinned)
            record["pinned"] = true;
        if (entry.restore_open)
            record["open"] = true;
        if (entry.restore_open && entry.presentation_pending) {
            nlohmann::json presentation;
            write_presentation(presentation, entry.presentation);
            record["presentation"] = std::move(presentation);
        }
        surfaces[identity] = std::move(record);
    });
    root["surfaces"] = std::move(surfaces);
    nlohmann::json hubs = nlohmann::json::object();
    for (const auto& entry : store.hub_subviews())
        hubs[hub_kind_name(entry.first)] = entry.second;
    root["hubs"] = std::move(hubs);
    return root.dump();
}

bool load_surface_state_json(std::string_view json_text, surface_state_store_t& store,
                             const std::function<bool(const stable_view_id_t&)>& catalog_membership) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json_text.begin(), json_text.end());
    } catch (...) {
        return false;
    }
    if (!root.is_object())
        return false;
    store.clear();
    try {
        if (root.contains("surfaces") && root["surfaces"].is_object()) {
            for (auto iterator = root["surfaces"].begin();
                 iterator != root["surfaces"].end(); ++iterator) {
                if (!iterator.value().is_object())
                    continue;
                const surface_identity_parse_t identity = parse_surface_identity(iterator.key());
                if (!identity.valid)
                    continue;
                std::string canonical_key;
                if (identity.hub != hub_kind_t::none) {
                    canonical_key = hub_object_name(identity.hub);
                } else {
                    if (catalog_membership && !catalog_membership(identity.view))
                        continue;
                    canonical_key = identity_string(identity.view, identity.instance);
                }
                const auto& record = iterator.value();
                surface_state_store_t::entry_t entry;
                entry.pinned = record.contains("pinned") &&
                    record["pinned"].is_boolean() && record["pinned"].get<bool>();
                entry.restore_open = record.contains("open") &&
                    record["open"].is_boolean() && record["open"].get<bool>();
                if (record.contains("presentation") &&
                    read_presentation(record["presentation"], entry.presentation))
                    entry.presentation_pending = true;
                store.ensure_key(canonical_key) = entry;
            }
        }
        if (root.contains("hubs") && root["hubs"].is_object()) {
            for (auto iterator = root["hubs"].begin(); iterator != root["hubs"].end(); ++iterator) {
                if (!iterator.value().is_number_integer())
                    continue;
                for (const hub_kind_t hub : {hub_kind_t::analysis, hub_kind_t::scan,
                        hub_kind_t::types, hub_kind_t::debugger, hub_kind_t::network}) {
                    if (iterator.key() != hub_kind_name(hub))
                        continue;
                    const int subview = iterator.value().get<int>();
                    if (subview >= 0 && subview < static_cast<int>(hub_member_count(hub)))
                        store.set_hub_subview(hub, subview);
                }
            }
        }
    } catch (...) {
        return false;
    }
    store.note_changed();
    return true;
}

}
