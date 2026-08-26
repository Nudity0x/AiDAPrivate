#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "helpers/diag_log.hpp"

namespace aida::qt::debugger {

// Generation-tagged immutable snapshot store. Ports the ImGui-era
// render_snapshot_cache_t/refresh_render_snapshot (core/debugger/debugger_view.cpp)
// into a shared_ptr<const vector<T>> form so Qt models swap rows atomically
// without holding engine mutexes during paint. GUI-thread only.
template <typename T>
class SnapshotStore {
public:
    using container_t = std::vector<T>;
    using const_ptr_t = std::shared_ptr<const container_t>;

    struct poll_result_t {
        const_ptr_t items;
        std::uint64_t generation = 0;
        bool refreshed = false;
        bool lock_busy = false;
    };

    SnapshotStore() : items_(std::make_shared<const container_t>()) {}

    const_ptr_t current() const noexcept { return items_; }
    std::uint64_t generation() const noexcept { return generation_; }

    poll_result_t poll(std::mutex& mutex, const container_t& source,
                       const std::atomic<std::uint64_t>& generation,
                       const char* owner) {
        poll_result_t result;
        result.items = items_;
        result.generation = generation_;
        const std::uint64_t current_generation =
            generation.load(std::memory_order_acquire);
        if (generation_ == current_generation)
            return result;
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            ++counters_.lock_busy_count;
            result.lock_busy = true;
            return result;
        }
        const auto started = std::chrono::steady_clock::now();
        auto copy = std::make_shared<const container_t>(source);
        lock.unlock();
        items_ = std::move(copy);
        generation_ = current_generation;
        result.items = items_;
        result.generation = generation_;
        result.refreshed = true;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        ++counters_.refresh_count;
        counters_.copied_items += items_->size();
        counters_.copy_time_us += elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
        const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (elapsed >= 2000 || last_report_ms_ < 0 ||
            now_ms - last_report_ms_ >= 5000) {
            last_report_ms_ = now_ms;
            diag::log_tagged_fmt("ui_perf",
                "debugger_snapshot owner=%s generation=%llu rows=%zu object_bytes=%zu "
                "copy_us=%lld copy_us_total=%llu refreshes=%llu lock_busy=%llu",
                owner ? owner : "unknown",
                static_cast<unsigned long long>(current_generation), items_->size(),
                items_->size() * sizeof(T), static_cast<long long>(elapsed),
                static_cast<unsigned long long>(counters_.copy_time_us),
                static_cast<unsigned long long>(counters_.refresh_count),
                static_cast<unsigned long long>(counters_.lock_busy_count));
        }
        return result;
    }

    // Adopt an externally produced immutable publication (worker-published
    // shared_ptr stores such as the memory-map region store).
    void adopt(const_ptr_t items, std::uint64_t generation) {
        if (items && generation_ != generation) {
            items_ = std::move(items);
            generation_ = generation;
        }
    }

    void reset() {
        items_ = std::make_shared<const container_t>();
        generation_ = 0;
    }

private:
    struct counters_t {
        std::uint64_t lock_busy_count = 0;
        std::uint64_t refresh_count = 0;
        std::uint64_t copied_items = 0;
        std::uint64_t copy_time_us = 0;
    };

    const_ptr_t items_;
    std::uint64_t generation_ = 0;
    counters_t counters_;
    std::int64_t last_report_ms_ = -1;
};

}
