#pragma once

#include <QModelIndex>

#include <vector>

#include "core/debugger/debugger_interaction_context.hpp"

class QItemSelectionModel;

namespace aida::qt::debugger {

class DebuggerTableModelBase;

// Maps QTableView selection onto debugger_interaction::select/select_set.
// Qt's ExtendedSelection provides shift-range/ctrl-toggle natively; this bridge
// converts the resulting selection into retained contexts (row -> stable ID ->
// context_t) with the 4,096-row cap and the verbatim cap toasts.
namespace selection_bridge {

// Publish the full selection of a pane's table: the current index is the
// focused context; selected rows form the set. Empty selection clears.
void publish_rows(const DebuggerTableModelBase& model,
                  const QItemSelectionModel* selection,
                  const QModelIndex& current);

// Single-row select (click) / right-click select+menu flow.
void publish_single(const DebuggerTableModelBase& model, int row);

// Direct context publish for the non-model-backed custom surfaces (disasm
// slice, stack quad).
void publish_context(debugger_interaction::context_t context);

void clear();

}

}
