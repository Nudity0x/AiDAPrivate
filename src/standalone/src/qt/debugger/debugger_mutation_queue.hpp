#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/debugger/debugger_interaction_context.hpp"
#include "core/debugger/debugger_view.hpp"

namespace aida::qt::debugger {

// Qt-facing facade over the backend mutation pipeline. The single-flight CAS
// (g_target_mutation_pending / g_execution_command_pending) semantics live in
// the backend (core/debugger/debugger_view.cpp); this queue forwards to it so
// the rejection toasts stay verbatim, and adds:
//  - pending-state change signals for the toolbar/status strip (driven from
//    the 250ms session tick; cheap atomic reads, never a blocking call),
//  - the dialog-scoped mutation completion channel with the receiver-death
//    gap closed (the queue is the app-lifetime invokeMethod receiver; a
//    shared mutation_state carries delivered/cas_flag/context so an
//    undelivered completion takes the terminal failure path at shutdown).
class DebuggerMutationQueue : public QObject {
    Q_OBJECT
public:
    static DebuggerMutationQueue& instance();

    struct mutation_state_t {
        debugger_interaction::context_t context;
        std::atomic<bool> delivered{false};
        std::string label;
        std::function<void()> release;
    };

    bool queueMutation(const char* label, const char* action,
                       debugger_interaction::context_t context,
                       debugger_view::mutation_operation_t operation,
                       bool advance_generation = true);
    bool executeCommand(debugger_view::execution_command_t command,
                        std::string* error = nullptr);

    bool mutationPending() const noexcept;
    bool commandPending() const noexcept;

    // Change-protection worker (ports the memory-map Change Protection apply:
    // protect_memory + enumerate_memory_regions readback verify +
    // "Protection changed 0x%X -> 0x%X" toast + refresh). Its own CAS; the
    // completion posts to this queue (app-lifetime), never to a dialog.
    bool changeProtection(const debugger_interaction::context_t& context,
                          std::uint64_t address, std::uint64_t size,
                          std::uint32_t new_protect);
    bool protectionPending() const noexcept;

    void noteTick();

Q_SIGNALS:
    void mutationPendingChanged(bool pending);
    void commandPendingChanged(bool pending);
    void protectionChangeCompleted(bool verified, QString message);

private:
    explicit DebuggerMutationQueue(QObject* parent = nullptr);
    void sweepUndelivered() noexcept;

    bool last_mutation_pending_ = false;
    bool last_command_pending_ = false;
    std::mutex inflight_mutex_;
    std::vector<std::shared_ptr<mutation_state_t>> inflight_;
};

}
