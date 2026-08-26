#include "qt/debugger/debugger_selection_bridge.hpp"

#include <QItemSelectionModel>

#include "helpers/diag_log.hpp"

#include "core/ui/toast_notification.hpp"

#include "qt/debugger/debugger_models.hpp"

namespace aida::qt::debugger::selection_bridge {

namespace {
constexpr std::size_t k_maximum_selected_contexts = 4096;
}

void publish_rows(const DebuggerTableModelBase& model,
                  const QItemSelectionModel* selection,
                  const QModelIndex& current) {
    const auto focused = current.isValid()
        ? model.contextForRow(current.row())
        : debugger_interaction::context_t{};
    if (!selection || !selection->hasSelection() ||
        focused.kind == debugger_interaction::kind_t::none) {
        if (!selection || !selection->hasSelection())
            debugger_interaction::clear();
        else
            debugger_interaction::select(focused);
        return;
    }
    std::vector<debugger_interaction::context_t> contexts;
    bool truncated = false;
    const auto ranges = selection->selection();
    for (const auto& range : ranges) {
        for (int row = range.top(); row <= range.bottom(); ++row) {
            auto context = model.contextForRow(row);
            if (context.kind == debugger_interaction::kind_t::none)
                continue;
            if (contexts.size() >= k_maximum_selected_contexts) {
                truncated = true;
                break;
            }
            contexts.push_back(std::move(context));
        }
        if (truncated)
            break;
    }
    if (truncated)
        toast_notification::push(
            "Debugger range selection is limited to 4,096 rows.",
            toast_notification::toast_type_t::warning);
    if (contexts.empty()) {
        debugger_interaction::select(focused);
        return;
    }
    debugger_interaction::select_set(std::move(contexts), focused);
}

void publish_single(const DebuggerTableModelBase& model, int row) {
    auto context = model.contextForRow(row);
    if (context.kind == debugger_interaction::kind_t::none)
        return;
    debugger_interaction::select(std::move(context));
}

void publish_context(debugger_interaction::context_t context) {
    if (context.kind == debugger_interaction::kind_t::none)
        return;
    debugger_interaction::select(std::move(context));
}

void clear() {
    debugger_interaction::clear();
}

}
