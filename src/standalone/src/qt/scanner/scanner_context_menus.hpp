#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

#include "core/scanner/memory_interaction_context.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/context_menu_contract.hpp"

class QEvent;
class QObject;
class QPoint;
class QTableView;
class QWidget;

namespace aida::qt::scanner {

void show_result_context_menu(
	const memory_interaction::context_t& focused,
	const memory_interaction::runtime_t& runtime,
	const QString& owner_view_id,
	aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos,
	QWidget* parent);

void show_address_context_menu(
	const memory_interaction::context_t& focused,
	const memory_interaction::runtime_t& runtime,
	const QString& owner_view_id,
	aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos,
	QWidget* parent);

int find_address_index(const memory_interaction::context_t& context);

bool forward_table_menu_key(QObject* watched, QEvent* event, QTableView* view,
	const std::function<void(const QPoint& global_pos, int row, int origin)>& handler);

}
