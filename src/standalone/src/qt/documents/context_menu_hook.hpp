#pragma once

#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/interaction_context.hpp"

#include <QPoint>

#include <functional>

class QWidget;

namespace aida::qt::documents {

using context_menu_display_fn = std::function<void(
    const aida::ui::stable_menu_id_t& menu,
    aida::ui::interaction_context_t snapshot,
    aida::ui::context_menu_open_origin_t origin,
    const QPoint& global_pos, QWidget* parent)>;

inline context_menu_display_fn& context_menu_display_slot()
{
    static context_menu_display_fn fn;
    return fn;
}

inline void set_context_menu_display(context_menu_display_fn fn)
{
    context_menu_display_slot() = std::move(fn);
}

inline bool has_context_menu_display()
{
    return static_cast<bool>(context_menu_display_slot());
}

inline void show_context_menu(const aida::ui::stable_menu_id_t& menu,
    aida::ui::interaction_context_t snapshot,
    aida::ui::context_menu_open_origin_t origin,
    const QPoint& global_pos, QWidget* parent)
{
    if (context_menu_display_slot())
        context_menu_display_slot()(menu, std::move(snapshot), origin, global_pos, parent);
}

using retained_entity_menu_fn = std::function<void(
    const aida::ui::application_ui::retained_entity_context_t& context,
    aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos, QWidget* parent)>;

inline retained_entity_menu_fn& retained_entity_menu_slot()
{
    static retained_entity_menu_fn fn;
    return fn;
}

inline void set_retained_entity_menu_display(retained_entity_menu_fn fn)
{
    retained_entity_menu_slot() = std::move(fn);
}

inline void show_retained_entity_menu(
    const aida::ui::application_ui::retained_entity_context_t& context,
    aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos, QWidget* parent)
{
    if (retained_entity_menu_slot())
        retained_entity_menu_slot()(context, origin, global_pos, parent);
}

struct menu_payload_placeholder_t {};

inline aida::ui::interaction_context_t make_menu_snapshot(
    aida::ui::stable_view_id_t active_view, aida::ui::stable_context_type_id_t payload_type)
{
    static const menu_payload_placeholder_t placeholder{};
    aida::ui::interaction_context_t context;
    context.active_view = std::move(active_view);
    context.payload = aida::ui::typed_context_ref_t::from(payload_type, placeholder);
    return context;
}

}
