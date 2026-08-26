#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>

namespace aida::qt::widgets {

enum class selection_nav_key_t {
    none,
    down,
    up,
    home,
    end,
    page_down,
    page_up
};

class selection_model_t {
public:
    void clear();
    bool contains(std::uint64_t stable_id) const;
    std::size_t size() const;
    std::uint64_t focused() const;
    void select(std::uint64_t stable_id, std::size_t row_index, bool additive, bool range,
        const std::function<std::uint64_t(std::size_t)>& id_at, std::size_t row_count);
    bool handle_keyboard(selection_nav_key_t key, bool additive, bool range,
        std::size_t row_count, const std::function<std::uint64_t(std::size_t)>& id_at,
        bool page_navigation = true);

private:
    std::unordered_set<std::uint64_t> selected_;
    std::size_t anchor_index_ = static_cast<std::size_t>(-1);
    std::size_t focused_index_ = static_cast<std::size_t>(-1);
    std::uint64_t focused_id_ = 0;
};

inline void selection_model_t::clear()
{
    selected_.clear();
    anchor_index_ = static_cast<std::size_t>(-1);
    focused_index_ = static_cast<std::size_t>(-1);
    focused_id_ = 0;
}

inline bool selection_model_t::contains(std::uint64_t stable_id) const
{
    return selected_.count(stable_id) != 0;
}

inline std::size_t selection_model_t::size() const
{
    return selected_.size();
}

inline std::uint64_t selection_model_t::focused() const
{
    return focused_id_;
}

inline void selection_model_t::select(std::uint64_t stable_id, std::size_t row_index,
    bool additive, bool range, const std::function<std::uint64_t(std::size_t)>& id_at,
    std::size_t row_count)
{
    if (row_index >= row_count || !stable_id)
        return;
    if (range && anchor_index_ != static_cast<std::size_t>(-1) && id_at) {
        if (!additive)
            selected_.clear();
        const std::size_t first = anchor_index_ < row_index ? anchor_index_ : row_index;
        const std::size_t last = anchor_index_ > row_index ? anchor_index_ : row_index;
        for (std::size_t i = first; i <= last && i < row_count; ++i)
            selected_.insert(id_at(i));
    } else if (additive) {
        if (selected_.count(stable_id))
            selected_.erase(stable_id);
        else
            selected_.insert(stable_id);
        anchor_index_ = row_index;
    } else {
        selected_.clear();
        selected_.insert(stable_id);
        anchor_index_ = row_index;
    }
    focused_index_ = row_index;
    focused_id_ = stable_id;
}

inline bool selection_model_t::handle_keyboard(selection_nav_key_t key, bool additive,
    bool range, std::size_t row_count,
    const std::function<std::uint64_t(std::size_t)>& id_at, bool page_navigation)
{
    if (row_count == 0 || !id_at || key == selection_nav_key_t::none)
        return false;
    std::size_t next = focused_index_ == static_cast<std::size_t>(-1) ? 0 : focused_index_;
    bool moved = false;
    switch (key) {
    case selection_nav_key_t::down:
        if (next + 1 < row_count) { ++next; moved = true; }
        break;
    case selection_nav_key_t::up:
        if (next > 0) { --next; moved = true; }
        break;
    case selection_nav_key_t::home:
        next = 0; moved = true;
        break;
    case selection_nav_key_t::end:
        next = row_count - 1; moved = true;
        break;
    case selection_nav_key_t::page_down:
        if (page_navigation) {
            next = next + 10 < row_count - 1 ? next + 10 : row_count - 1;
            moved = true;
        }
        break;
    case selection_nav_key_t::page_up:
        if (page_navigation) {
            next = next > 10 ? next - 10 : 0;
            moved = true;
        }
        break;
    case selection_nav_key_t::none:
        break;
    }
    if (!moved)
        return false;
    select(id_at(next), next, additive, range, id_at, row_count);
    return true;
}

}
