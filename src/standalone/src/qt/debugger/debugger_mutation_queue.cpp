#include "qt/debugger/debugger_mutation_queue.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QThread>

#include <algorithm>
#include <stdexcept>

#include "helpers/diag_log.hpp"

#include "core/infra/executor.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/ui/toast_notification.hpp"

namespace aida::qt::debugger {

namespace {
std::atomic<bool> g_protection_pending{false};
}

DebuggerMutationQueue& DebuggerMutationQueue::instance() {
    static QPointer<DebuggerMutationQueue> instance;
    if (!instance) {
        instance = new DebuggerMutationQueue();
    }
    return *instance;
}

DebuggerMutationQueue::DebuggerMutationQueue(QObject* parent)
    : QObject(parent) {
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
        [this] { sweepUndelivered(); });
}

bool DebuggerMutationQueue::queueMutation(
    const char* label, const char* action,
    debugger_interaction::context_t context,
    debugger_view::mutation_operation_t operation,
    bool advance_generation) {
    const bool queued = debugger_view::queue_debugger_mutation(label, action,
        std::move(context), std::move(operation), advance_generation);
    if (queued && !last_mutation_pending_) {
        last_mutation_pending_ = true;
        Q_EMIT mutationPendingChanged(true);
    }
    return queued;
}

bool DebuggerMutationQueue::executeCommand(
    debugger_view::execution_command_t command, std::string* error) {
    const bool started = debugger_view::execute_command(command, error);
    if (started && !last_command_pending_) {
        last_command_pending_ = true;
        Q_EMIT commandPendingChanged(true);
    }
    return started;
}

bool DebuggerMutationQueue::mutationPending() const noexcept {
    return debugger_view::target_mutation_pending();
}

bool DebuggerMutationQueue::commandPending() const noexcept {
    return debugger_view::execution_command_pending();
}

void DebuggerMutationQueue::noteTick() {
    const bool mutation = debugger_view::target_mutation_pending();
    const bool command = debugger_view::execution_command_pending();
    if (mutation != last_mutation_pending_) {
        last_mutation_pending_ = mutation;
        Q_EMIT mutationPendingChanged(mutation);
    }
    if (command != last_command_pending_) {
        last_command_pending_ = command;
        Q_EMIT commandPendingChanged(command);
    }
}

bool DebuggerMutationQueue::protectionPending() const noexcept {
    return g_protection_pending.load(std::memory_order_acquire);
}

bool DebuggerMutationQueue::changeProtection(
    const debugger_interaction::context_t& context, std::uint64_t address,
    std::uint64_t size, std::uint32_t new_protect) {
    bool expected = false;
    if (!g_protection_pending.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel))
        return false;
    auto state = std::make_shared<mutation_state_t>();
    state->context = context;
    state->label = "Change memory protection";
    state->release = [] {
        g_protection_pending.store(false, std::memory_order_release);
    };
    {
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        inflight_.push_back(state);
    }

    struct result_t {
        bool ok = false;
        bool verified = false;
        std::uint32_t old_protect = 0;
    };
    auto result = std::make_shared<result_t>();
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "debugger";
    submission.label = "Change memory protection";
    submission.thread_class = "debugger_target_mutation";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 2;
    submission.target_pid = context.target_pid;
    submission.generation = context.stop_generation;
    submission.ui_access_policy = "post_completion_only";
    submission.failure_policy = "fail_closed";
    DebuggerMutationQueue* queue = this;
    submission.body = [queue, context, address, size, new_protect, result, state]() {
        if (driver_bridge::attached_pid() == context.target_pid &&
            debugger_interaction::current_stop_generation() ==
                context.stop_generation) {
            result->ok = driver_bridge::protect_memory(address, size, new_protect,
                &result->old_protect);
            if (result->ok) {
                const auto regions = driver_bridge::enumerate_memory_regions();
                for (const auto& region : regions)
                    if (region.base == address) {
                        result->verified =
                            (region.protect & 0xFFu) == new_protect;
                        break;
                    }
            }
        }
        const bool posted = QMetaObject::invokeMethod(queue,
            [queue, result, new_protect, state]() {
                state->delivered.store(true, std::memory_order_release);
                if (result->verified) {
                    toast_notification::push(QString::asprintf(
                            "Protection changed 0x%X -> 0x%X", result->old_protect,
                            new_protect).toStdString(),
                        toast_notification::toast_type_t::success);
                    debugger_interaction::advance_stop_generation();
                } else {
                    toast_notification::push(
                        result->ok
                            ? "Protection write succeeded but readback did not match."
                            : "Failed to change protection.",
                        toast_notification::toast_type_t::error);
                }
                g_protection_pending.store(false, std::memory_order_release);
                Q_EMIT queue->protectionChangeCompleted(result->verified,
                    QString::asprintf("0x%X -> 0x%X", result->old_protect,
                        new_protect));
            }, Qt::QueuedConnection);
        if (!posted) {
            state->delivered.store(true, std::memory_order_release);
            if (result->verified)
                debugger_interaction::invalidate_stop_generation_async();
            g_protection_pending.store(false, std::memory_order_release);
            throw std::runtime_error(
                "Memory-protection completion could not be published to the UI thread");
        }
        if (!result->verified)
            throw std::runtime_error(result->ok
                ? "Memory-protection readback did not match"
                : "Memory-protection mutation failed");
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        state->delivered.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(inflight_mutex_);
            inflight_.erase(std::remove(inflight_.begin(), inflight_.end(), state),
                inflight_.end());
        }
        g_protection_pending.store(false, std::memory_order_release);
        toast_notification::push("Memory-protection queue rejected the task: " +
            submitted.reject_reason, toast_notification::toast_type_t::error);
        return false;
    }
    return true;
}

void DebuggerMutationQueue::sweepUndelivered() noexcept {
    std::vector<std::shared_ptr<mutation_state_t>> states;
    {
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        states.swap(inflight_);
    }
    for (const auto& state : states) {
        if (state->delivered.load(std::memory_order_acquire))
            continue;
        debugger_interaction::invalidate_stop_generation_async();
        if (state->release) {
            try {
                state->release();
            } catch (...) {
            }
        }
        diag::log_tagged_critical_fmt("debugger_context",
            "mutation_completion_dropped label='%s' pid=%u generation=%llu",
            state->label.c_str(), static_cast<unsigned>(state->context.target_pid),
            static_cast<unsigned long long>(state->context.stop_generation));
    }
}

}
