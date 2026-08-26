#pragma once

#include "qt/registry/qt_view_descriptor.hpp"

#include "core/disasm/disasm_view.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace aida::qt::registry {

class surface_state_store_t {
public:
    struct entry_t {
        bool pinned = false;
        bool restore_open = false;
        bool presentation_pending = false;
        disasm_view::presentation_snapshot_t presentation;
    };

    entry_t* find(const view_instance_id_t& id) noexcept;
    const entry_t* find(const view_instance_id_t& id) const noexcept;
    entry_t& ensure(const view_instance_id_t& id) noexcept;
    entry_t& ensure_key(const std::string& identity) noexcept;
    entry_t* find_hub(hub_kind_t hub) noexcept;
    const entry_t* find_hub(hub_kind_t hub) const noexcept;
    entry_t& ensure_hub(hub_kind_t hub) noexcept;
    void erase(const view_instance_id_t& id) noexcept;
    void clear() noexcept;
    void for_each(const std::function<void(const std::string& identity, const entry_t&)>& visitor) const;

    void set_hub_subview(hub_kind_t hub, int subview) noexcept;
    std::optional<int> hub_subview(hub_kind_t hub) const noexcept;
    const std::map<hub_kind_t, int>& hub_subviews() const noexcept { return hub_subviews_; }

    std::uint64_t revision() const noexcept { return revision_; }
    void note_changed() noexcept { ++revision_; }

private:
    std::map<std::string, entry_t> entries_;
    std::map<hub_kind_t, int> hub_subviews_;
    std::uint64_t revision_ = 0;
};

struct surface_identity_parse_t {
    stable_view_id_t view;
    stable_view_instance_key_t instance;
    hub_kind_t hub = hub_kind_t::none;
    bool valid = false;
};

surface_identity_parse_t parse_surface_identity(std::string_view key);

std::string capture_surface_state_json(const surface_state_store_t& store);
bool load_surface_state_json(std::string_view json_text, surface_state_store_t& store,
                             const std::function<bool(const stable_view_id_t&)>& catalog_membership);

}
