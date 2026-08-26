#include "analysis_context_menu_qt.hpp"

#include "qt/documents/context_menu_hook.hpp"

#include <QCursor>

#include <utility>

namespace aida::ui::analysis_context_menu {

void open(context_t context, context_menu_open_origin_t origin,
          const QPoint& global_pos, QWidget* parent) {
    aida::qt::documents::show_retained_entity_menu(
        make_retained_context(std::move(context)), origin, global_pos, parent);
}

}

namespace aida::qt::analysis {

void install_analysis_context_menu_display() {
    aida::ui::analysis_context_menu::set_retained_display_hook(
        [](aida::ui::application_ui::retained_entity_context_t context,
           aida::ui::context_menu_open_origin_t origin) {
            aida::qt::documents::show_retained_entity_menu(
                context, origin, QCursor::pos(), nullptr);
        });
}

}
