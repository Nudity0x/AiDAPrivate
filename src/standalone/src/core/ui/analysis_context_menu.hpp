#pragma once

#include "context_menu_contract.hpp"
#include "shell_host_contract.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>

class QPoint;
class QWidget;

namespace aida::ui::analysis_context_menu {

enum class menu_kind_t : std::uint8_t {
    instruction,
    pseudocode,
    graph,
    function,
    xref,
    metadata
};

struct action_slot_t {
    capability_state_t capability = capability_state_t::available();
    std::function<action_handler_result_t()> invoke;
    action_check_state_t check_state = action_check_state_t::not_checkable;
};

struct context_t {
    menu_kind_t kind = menu_kind_t::instruction;
    std::string entity_id;
    std::uint64_t generation = 0;
    std::function<std::uint64_t()> live_generation;
    std::function<capability_state_t()> validate_identity;
    std::map<std::string, action_slot_t> actions;
};

using retained_display_hook_t = std::function<void(
    application_ui::retained_entity_context_t context,
    context_menu_open_origin_t origin)>;

void set_retained_display_hook(retained_display_hook_t hook);

application_ui::retained_entity_context_t make_retained_context(context_t context);

void open(context_t context, context_menu_open_origin_t origin);
void open(context_t context, context_menu_open_origin_t origin,
          const QPoint& global_pos, QWidget* parent);
bool execute_shortcut(context_t context, const char* action_id);

}
