#pragma once

#include <QWidget>

#include "core/debugger/debugger_interaction_context.hpp"
#include "core/debugger/debugger_view.hpp"

namespace aida::qt::debugger::confirm_dialogs {

// Synchronously presented review confirms (QDialog::open, window-modal; never
// exec()). Each re-validates identity live (RevalidateScope on the context
// identity + stop generation) and queues the verbatim mutation body on accept
// through the backend. These replace the ImGui polled pending-context-mutation
// state machine.
void present_mutation(debugger_view::context_mutation_t mutation,
                      const debugger_interaction::context_t& context,
                      QWidget* parent);
void confirm_set_instruction_pointer(
    const debugger_interaction::context_t& context, QWidget* parent);
void confirm_terminate_thread(const debugger_interaction::context_t& context,
                              QWidget* parent);
void confirm_close_handle(const debugger_interaction::context_t& context,
                          QWidget* parent);
void confirm_apply_patch(const debugger_interaction::context_t& context,
                         QWidget* parent);
void confirm_revert_patch(const debugger_interaction::context_t& context,
                          QWidget* parent);
void confirm_remove_patch(const debugger_interaction::context_t& context,
                          QWidget* parent);
void confirm_revert_all_patches(
    const debugger_interaction::context_t& context, QWidget* parent);
void confirm_remove_watch(const debugger_interaction::context_t& context,
                          QWidget* parent);
void confirm_remove_bookmark(const debugger_interaction::context_t& context,
                             QWidget* parent);

}
