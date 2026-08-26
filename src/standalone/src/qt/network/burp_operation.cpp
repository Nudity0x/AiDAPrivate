#include "qt/network/burp_operation.hpp"

#include <QMetaObject>

#include <exception>
#include <string>
#include <utility>

#include "core/infra/executor.hpp"
#include "core/ui/task_center.hpp"

namespace aida::qt::net {

namespace {

QString to_q(const std::string& value) {
    return QString::fromStdString(value);
}

}

BurpOperationRunner::BurpOperationRunner(const QString& taskSource, QObject* parent)
    : QObject(parent), task_source_(taskSource) {}

bool BurpOperationRunner::submit(BurpRequest request) {
    if (!request.execute || request.owner.isEmpty() || request.label.isEmpty())
        return false;
    bool expected = false;
    if (!pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;
    const std::uint64_t generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_request_ = request;
        completion_.reset();
    }
    const std::string owner = request.owner.toStdString();
    const std::string label = request.label.toStdString();
    const std::string task_id = "burp.ui." + owner + "." +
        std::to_string(generation) + "." +
        std::to_string(aida::infra::executor::now_ms());
    aida::ui::task_center::task_registration_t registration;
    registration.id = task_id;
    registration.source = task_source_.toStdString();
    registration.owner = owner;
    registration.owner_view = request.ownerView.toStdString();
    registration.owner_action = request.ownerAction.toStdString();
    registration.target = request.target.toStdString();
    registration.label = label;
    registration.stage = "Queued";
    registration.affected_entity = request.affectedEntity.toStdString();
    registration.progress = 0.0f;
    registration.callbacks.retry = [this]() { return retry(); };
    if (!aida::ui::task_center::register_task(std::move(registration))) {
        pending_.store(false, std::memory_order_release);
        return false;
    }
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = owner.c_str();
    submission.label = label.c_str();
    submission.thread_class = "bounded_task";
    submission.domain = request.domain;
    submission.priority = request.priority;
    submission.body = [this, generation, task_id, execute = std::move(request.execute)]() mutable {
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::running, 0.1f, "Running"));
        aida::burp::ui_operation::result_t result;
        try {
            result = execute();
        } catch (const std::exception& exception) {
            result.message = exception.what();
        } catch (...) {
            result.message = "The operation failed with an unknown exception.";
        }
        if (result.message.empty())
            result.message = result.success ? "Operation completed." : "Operation failed.";
        auto completion = std::make_shared<aida::burp::ui_operation::completion_t>();
        completion->generation = generation;
        completion->result = result;
        std::shared_ptr<const aida::burp::ui_operation::completion_t> immutable = completion;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completion_ = immutable;
        }
        pending_.store(false, std::memory_order_release);
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            result.success
                ? result.partial ? aida::ui::task_center::task_state_t::partial
                                 : aida::ui::task_center::task_state_t::completed
                : aida::ui::task_center::task_state_t::failed,
            1.0f, result.success ? "Completed" : "Failed", result.message));
        QMetaObject::invokeMethod(this,
            [this, immutable]() {
                Q_EMIT completed(immutable->generation, immutable->result.success,
                    immutable->result.partial, to_q(immutable->result.message));
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        pending_.store(false, std::memory_order_release);
        aida::burp::ui_operation::result_t result;
        result.message = "The bounded operation queue rejected the request.";
        auto completion = std::make_shared<aida::burp::ui_operation::completion_t>();
        completion->generation = generation;
        completion->result = result;
        std::shared_ptr<const aida::burp::ui_operation::completion_t> immutable = completion;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completion_ = immutable;
        }
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Submission rejected", result.message));
        QMetaObject::invokeMethod(this,
            [this, immutable]() {
                Q_EMIT completed(immutable->generation, immutable->result.success,
                    immutable->result.partial, to_q(immutable->result.message));
            }, Qt::QueuedConnection);
        return false;
    }
    Q_EMIT submitted(generation);
    return true;
}

bool BurpOperationRunner::retry() {
    BurpRequest request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_request_ || pending_.load(std::memory_order_acquire))
            return false;
        request = *last_request_;
    }
    return submit(std::move(request));
}

std::shared_ptr<const aida::burp::ui_operation::completion_t>
BurpOperationRunner::completion() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completion_;
}

}
