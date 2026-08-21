#pragma once

#include "../infra/executor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

namespace test_lab {

enum class bounded_run_status_t {
	completed,
	timed_out,
	saturated,
	post_failed,
	exception
};

struct bounded_run_result_t {
	bounded_run_status_t status = bounded_run_status_t::post_failed;
	std::string error;
};

struct bounded_run_context_t {
	std::shared_ptr<std::atomic<bool>> cancellation;

	bool cancellation_requested() const noexcept {
		return cancellation && cancellation->load(std::memory_order_acquire);
	}
};

namespace detail {

inline std::string describe_current_exception() {
	try {
		throw;
	} catch (const std::exception& ex) {
		return ex.what();
	} catch (...) {
		return "unknown exception";
	}
}

struct bounded_run_state_t {
	std::mutex mtx;
	std::condition_variable cv;
	bool done = false;
	std::string error;
	std::shared_ptr<std::atomic<bool>> cancellation = std::make_shared<std::atomic<bool>>(false);
};

struct bounded_run_lease_t {
	std::shared_ptr<std::atomic<std::uint32_t>> active;

	~bounded_run_lease_t() {
		active->fetch_sub(1u, std::memory_order_acq_rel);
	}
};

}

class bounded_runner_t {
public:
	explicit bounded_runner_t(std::uint32_t max_active)
		: active_(std::make_shared<std::atomic<std::uint32_t>>(0)),
		  max_active_(max_active == 0 ? 1u : max_active) {
	}

	template <typename Fn>
	bounded_run_result_t run(std::uint32_t timeout_ms, Fn&& fn) {
		auto active = active_;
		for (;;) {
			std::uint32_t current = active->load(std::memory_order_acquire);
			if (current >= max_active_)
				return { bounded_run_status_t::saturated, {} };
			if (active->compare_exchange_weak(current, current + 1u, std::memory_order_acq_rel))
				break;
		}

		auto state = std::make_shared<detail::bounded_run_state_t>();
		auto lease = std::make_shared<detail::bounded_run_lease_t>();
		lease->active = active;
		using fn_t = std::decay_t<Fn>;
		auto task = std::make_shared<fn_t>(std::forward<Fn>(fn));

		bool posted = false;
		std::uint64_t task_id = 0;
		try {
		aida::infra::executor::submission_t submission;
			submission.owner_subsystem = "test_lab";
			submission.label = "test_lab.bounded_runner";
			submission.thread_class = "testlab_bounded_runner";
			submission.domain = aida::infra::executor::domain_t::feature_worker;
			submission.priority = 2;
			submission.failure_policy = "reject_not_started";
			submission.cancel_hook = [state]() {
				state->cancellation->store(true, std::memory_order_release);
			};
			submission.body = [state, task, lease]() mutable {
				std::string error;
				try {
					if constexpr (std::is_invocable_v<fn_t&, bounded_run_context_t>)
						(*task)(bounded_run_context_t{ state->cancellation });
					else
						(*task)();
				} catch (...) {
					error = detail::describe_current_exception();
				}
				{
					std::lock_guard<std::mutex> lk(state->mtx);
					state->error = std::move(error);
					state->done = true;
				}
				state->cv.notify_all();
			};
			auto submit_result = aida::infra::executor::submit(std::move(submission));
			posted = submit_result.submitted;
			task_id = submit_result.task_id;
		} catch (...) {
			return { bounded_run_status_t::post_failed, detail::describe_current_exception() };
		}

		if (!posted) {
			return { bounded_run_status_t::post_failed, {} };
		}

		std::unique_lock<std::mutex> lk(state->mtx);
		if (!state->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&state]() { return state->done; })) {
			state->cancellation->store(true, std::memory_order_release);
			if (task_id != 0)
				aida::infra::executor::cancel(task_id);
			return { bounded_run_status_t::timed_out, {} };
		}

		if (!state->error.empty())
			return { bounded_run_status_t::exception, state->error };

		return { bounded_run_status_t::completed, {} };
	}

private:
	std::shared_ptr<std::atomic<std::uint32_t>> active_;
	std::uint32_t max_active_;
};

}
