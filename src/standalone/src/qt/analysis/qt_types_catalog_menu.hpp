#pragma once

#include <QPoint>
#include <QString>

#include <memory>
#include <string>

class QWidget;

namespace aida::qt::analysis {

class QtTypesCatalogView;
class QtWorkspaceContext;

// Full retained-entity catalog menu (07 sec. 6.1; ports
// types_hub_view::render_catalog_context_menu verbatim).
void show_types_catalog_menu(QtTypesCatalogView* view, QWidget* parent,
                             const QPoint& global_pos, int view_row);

// Review Global Type Declaration dialog (07 sec. 6.1/sec. 7.x): modal open(), never
// exec(). Drives disasm_view::queue_type_declaration on confirm.
void open_type_declaration_review(QtWorkspaceContext* context, std::string name,
                                  std::string declaration, QWidget* parent);

}
