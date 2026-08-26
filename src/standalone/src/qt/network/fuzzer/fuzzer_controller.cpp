#include "qt/network/fuzzer/fuzzer_controller.hpp"

#include <QMetaObject>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/infra/executor.hpp"
#include "core/network/mitm_proxy.hpp"
#include "core/ui/task_center.hpp"
#include "helpers/diag_log.hpp"
#include "qt/network/shared/network_format.hpp"

namespace aida::qt::net {

using network_view::g_state;
using network_view::state_t;
using network_view::payload_set_t;
using network_view::fuzzer_attack_mode_t;

namespace {

constexpr std::uint64_t k_fuzzer_retained_result_limit = 32768;
constexpr std::uint64_t k_fuzzer_retained_byte_limit = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t k_fuzzer_decoded_payload_budget = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t k_fuzzer_match_input_limit = 1024ULL * 1024ULL;
constexpr std::size_t k_fuzzer_extract_input_limit = 65536;

void publish_fuzzer_results_locked(state_t& state) {
    auto snapshot = std::make_shared<state_t::fuzzer_results_snapshot_t>();
    snapshot->pages.reserve(state.fuzz_result_pages.size() +
        (state.fuzz_result_pending.empty() ? 0U : 1U));
    for (const auto& page : state.fuzz_result_pages)
        snapshot->pages.push_back(page);
    if (!state.fuzz_result_pending.empty())
        snapshot->pages.push_back(std::make_shared<const state_t::fuzzer_result_page_t>(
            state_t::fuzzer_result_page_t{state.fuzz_result_pending, state.fuzz_pending_bytes}));
    snapshot->payload_catalog = state.fuzz_payload_catalog;
    snapshot->retained_count = state.fuzz_retained_count;
    snapshot->dropped_count = state.fuzz_dropped_count;
    snapshot->retained_bytes = state.fuzz_retained_bytes;
    snapshot->generation = ++state.fuzz_results_generation;
    snapshot->maximum_payload_columns = state.fuzz_maximum_payload_columns;
    snapshot->has_extracted_values = state.fuzz_has_extracted_values;
    snapshot->has_failures = state.fuzz_has_failures;
    std::atomic_store_explicit(&state.fuzz_results_snapshot,
        std::static_pointer_cast<const state_t::fuzzer_results_snapshot_t>(snapshot),
        std::memory_order_release);
    if (auto* controller = FuzzerController::instance()) {
        const std::uint64_t generation = snapshot->generation;
        QMetaObject::invokeMethod(controller,
            [controller, generation] { Q_EMIT controller->resultsPublished(generation); },
            Qt::QueuedConnection);
    }
}

void clear_fuzzer_results_locked(state_t& state) {
    state.fuzz_result_pages.clear();
    state.fuzz_result_pending.clear();
    state.fuzz_retained_count = 0;
    state.fuzz_dropped_count = 0;
    state.fuzz_retained_bytes = 0;
    state.fuzz_pending_bytes = 0;
    state.fuzz_payload_catalog.reset();
    state.fuzz_maximum_payload_columns = 1;
    state.fuzz_has_extracted_values = false;
    state.fuzz_has_failures = false;
    state.fuzz_has_selection = false;
    publish_fuzzer_results_locked(state);
}

void append_fuzzer_result(state_t& state, state_t::fuzzer_result_t result) {
    std::lock_guard<std::mutex> lock(state.fuzz_mutex);
    const std::uint64_t result_bytes = sizeof(state_t::fuzzer_result_t) +
        static_cast<std::uint64_t>(result.payload_indices.size()) * sizeof(std::uint32_t) +
        static_cast<std::uint64_t>(result.response_preview.size()) +
        static_cast<std::uint64_t>(result.extracted_value.size()) +
        static_cast<std::uint64_t>(result.error.size());
    while (!state.fuzz_result_pages.empty() &&
           (state.fuzz_retained_count >= k_fuzzer_retained_result_limit ||
            result_bytes > k_fuzzer_retained_byte_limit - state.fuzz_retained_bytes)) {
        const std::uint64_t removed = static_cast<std::uint64_t>(
            state.fuzz_result_pages.front()->rows.size());
        const std::uint64_t removed_bytes = state.fuzz_result_pages.front()->retained_bytes;
        state.fuzz_result_pages.pop_front();
        state.fuzz_retained_count -= removed;
        state.fuzz_dropped_count += removed;
        state.fuzz_retained_bytes -= removed_bytes;
    }
    if (state.fuzz_retained_count >= k_fuzzer_retained_result_limit ||
        result_bytes > k_fuzzer_retained_byte_limit - state.fuzz_retained_bytes) {
        ++state.fuzz_dropped_count;
        return;
    }
    state.fuzz_maximum_payload_columns = (std::max)(
        state.fuzz_maximum_payload_columns, result.payload_indices.size());
    state.fuzz_has_extracted_values = state.fuzz_has_extracted_values ||
        !result.extracted_value.empty();
    state.fuzz_has_failures = state.fuzz_has_failures || !result.error.empty();
    state.fuzz_result_pending.push_back(std::move(result));
    state.fuzz_pending_bytes += result_bytes;
    state.fuzz_retained_bytes += result_bytes;
    ++state.fuzz_retained_count;
    if (state.fuzz_result_pending.size() == k_fuzzer_page_size) {
        state.fuzz_result_pages.push_back(std::make_shared<const state_t::fuzzer_result_page_t>(
            state_t::fuzzer_result_page_t{
                std::move(state.fuzz_result_pending), state.fuzz_pending_bytes}));
        state.fuzz_result_pending.clear();
        state.fuzz_result_pending.reserve(k_fuzzer_page_size);
        state.fuzz_pending_bytes = 0;
    }
    if (state.fuzz_result_pending.size() == 1 ||
        state.fuzz_result_pending.size() % 32 == 0)
        publish_fuzzer_results_locked(state);
}

void finish_fuzzer_task(state_t& state,
                        aida::ui::task_center::task_state_t task_state,
                        std::string stage,
                        std::string summary) {
    std::string task_id;
    {
        std::lock_guard<std::mutex> lock(state.fuzz_mutex);
        publish_fuzzer_results_locked(state);
        task_id = state.fuzz_task_id;
        state.fuzz_last_stage = stage;
        state.fuzz_last_error =
            task_state == aida::ui::task_center::task_state_t::failed ||
            task_state == aida::ui::task_center::task_state_t::partial
                ? summary : std::string();
    }
    const std::uint64_t total = state.fuzz_total.load(std::memory_order_acquire);
    const std::uint64_t progress = state.fuzz_progress.load(std::memory_order_acquire);
    const float fraction = total == 0 ? 0.0f : static_cast<float>(
        static_cast<double>(progress) / static_cast<double>(total));
    if (!task_id.empty())
        (void)aida::ui::task_center::update_task(
            task_id, task_state, fraction, std::move(stage), std::move(summary));
    state.fuzz_running.store(false, std::memory_order_release);
    state.fuzz_cv.notify_all();
}

void run_fuzzer_thread(state_t& state) {
    state_t::fuzzer_entry_t cfg;
    std::string task_id;
    {
        std::lock_guard<std::mutex> lock(state.fuzz_mutex);
        cfg = state.fuzz_active_config;
        task_id = state.fuzz_task_id;
    }


    std::uint64_t decoded_payload_bytes = 0;
    auto append_payload = [&](std::vector<std::string>& values,
                              std::string value,
                              std::string& error) {
        const std::uint64_t bytes = static_cast<std::uint64_t>(value.size());
        if (bytes > k_fuzzer_decoded_payload_budget - decoded_payload_bytes) {
            error = "Decoded payload data exceeds the 64 MiB preparation budget";
            return false;
        }
        decoded_payload_bytes += bytes;
        values.push_back(std::move(value));
        return true;
    };

    auto load_set = [&](const payload_set_t& ps, std::string& error) -> std::vector<std::string> {
        std::vector<std::string> lines;
        auto push_line = [&](std::istream& is) {
            std::string line;
            while (std::getline(is, line)) {
                if (state.fuzz_cancel_requested.load(std::memory_order_acquire))
                    break;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() > 65535) {
                    error = "A payload exceeds the 65,535-byte request-editor limit";
                    break;
                }
                if (!line.empty() && !append_payload(lines, std::move(line), error))
                    break;
                if (lines.size() > cfg.maximum_requests) {
                    error = "Payload set exceeds the reviewed request maximum";
                    break;
                }
            }
        };
        if (ps.type == 0) {
            std::ifstream f(ps.source);
            if (!f.is_open()) error = "Unable to open payload wordlist: " + ps.source;
            else push_line(f);
        } else {
            std::istringstream ss(ps.source);
            push_line(ss);
        }
        return lines;
    };


    auto load_legacy_set = [&](std::string& error) -> std::vector<std::string> {
        payload_set_t tmp;
        tmp.type   = cfg.payload_type;
        tmp.source = cfg.payload_source;
        if (cfg.payload_type == 1) {
            std::vector<std::string> nums;
            long long start_n = 0, end_n = 100;
            if (sscanf(cfg.payload_source.c_str(), "%lld-%lld", &start_n, &end_n) < 1 ||
                end_n < start_n) {
                error = "Sequential payload range is invalid";
                return nums;
            }
            nums.reserve(static_cast<std::size_t>((std::min)(cfg.maximum_requests, 4096ULL)));
            for (long long n = start_n;;) {
                if (nums.size() >= cfg.maximum_requests) {
                    error = "Sequential payload range exceeds the reviewed request maximum";
                    nums.clear();
                    return nums;
                }
                if (!append_payload(nums, std::to_string(n), error)) {
                    nums.clear();
                    return nums;
                }
                if (n == end_n) break;
                if (n == (std::numeric_limits<long long>::max)()) {
                    error = "Sequential payload range overflowed";
                    nums.clear();
                    return nums;
                }
                ++n;
            }
            return nums;
        } else if (cfg.payload_type == 2) {
            std::string charset = cfg.payload_source.empty()
                ? "abcdefghijklmnopqrstuvwxyz0123456789" : cfg.payload_source;
            std::vector<std::string> v;
            const std::uint64_t chars = static_cast<std::uint64_t>(charset.size());
            if (chars != 0 && chars > (std::numeric_limits<std::uint64_t>::max)() / chars) {
                error = "Charset payload count overflowed";
                return v;
            }
            const std::uint64_t pairs = chars * chars;
            if (pairs > (std::numeric_limits<std::uint64_t>::max)() - chars) {
                error = "Charset payload count overflowed";
                return v;
            }
            const std::uint64_t count = chars + pairs;
            if (count > cfg.maximum_requests) {
                error = "Charset payloads exceed the reviewed request maximum";
                return v;
            }
            v.reserve(static_cast<std::size_t>(count));
            for (char c : charset)
                if (!append_payload(v, std::string(1, c), error)) return {};
            for (char a : charset)
                for (char b : charset)
                    if (!append_payload(v, std::string(1, a) + b, error)) return {};
            return v;
        }
        return load_set(tmp, error);
    };


    auto make_request_multi = [](const std::string& tmpl,
                                 const std::vector<std::string>& payloads,
                                 std::string_view marker,
                                 fuzzer_attack_mode_t mode,
                                 std::size_t active_position,
                                 std::string& result,
                                 std::string& error) {
        constexpr std::size_t maximum_request_bytes = 65535;
        result.clear();
        result.reserve((std::min)(maximum_request_bytes, tmpl.size()));
        auto append = [&](std::string_view value) {
            if (value.size() > maximum_request_bytes - result.size()) {
                error = "Expanded request exceeds the 65,535-byte request limit";
                return false;
            }
            result.append(value.data(), value.size());
            return true;
        };
        size_t pos = 0;
        size_t pi  = 0;
        while (pos < tmpl.size()) {
            size_t s = tmpl.find(marker, pos);
            if (s == std::string::npos) {
                if (!append(std::string_view(tmpl).substr(pos))) return false;
                break;
            }
            if (!append(std::string_view(tmpl).substr(pos, s - pos))) return false;
            const bool selected = mode != fuzzer_attack_mode_t::sniper || pi == active_position;
            const std::size_t payload_index = mode == fuzzer_attack_mode_t::sniper ? 0 : pi;
            if (selected && payload_index < payloads.size() &&
                !append(payloads[payload_index])) return false;
            pi++;
            pos = s + marker.size();
        }
        return true;
    };

    const std::string extract_literal(cfg.extract_literal);
    auto do_grep_extract = [&](const std::string& body) -> std::string {
        if (extract_literal.empty()) return {};
        const std::string_view bounded(body.data(),
            (std::min)(body.size(), k_fuzzer_extract_input_limit));
        if (bounded.find(extract_literal) != std::string_view::npos)
            return extract_literal;
        return {};
    };


    auto check_match = [&](int sc, std::string_view body, size_t len) -> bool {
        if (cfg.match_status > 0 && sc != cfg.match_status) return false;
        if (!cfg.match_body.empty() && body.find(cfg.match_body) == std::string::npos) return false;
        if (cfg.match_size_op != 0 && cfg.match_size < 0) return false;
        const std::size_t expected_size = static_cast<std::size_t>(cfg.match_size);
        if (cfg.match_size_op == 1 && len != expected_size) return false;
        if (cfg.match_size_op == 2 && len <= expected_size) return false;
        if (cfg.match_size_op == 3 && len >= expected_size) return false;
        return true;
    };


    using combo_t = std::vector<std::string>;
    std::vector<std::vector<std::string>> sets;
    std::string preparation_error;
    std::uint64_t total = 0;
    const fuzzer_template_shape_t template_shape = analyze_fuzzer_template(cfg.base_request);
    if (!cfg.maximum_requests_reviewed)
        preparation_error = "The maximum request count was not explicitly reviewed.";
    if (preparation_error.empty() &&
        (cfg.maximum_requests == 0 || cfg.maximum_requests > k_fuzzer_absolute_request_limit))
        preparation_error = "Reviewed request maximum is " +
            std::to_string(cfg.maximum_requests) + "; the supported range is 1 to 1,000,000.";
    if (preparation_error.empty() && cfg.payload_sets.size() > k_fuzzer_payload_set_limit)
        preparation_error = "Payload set count is " + std::to_string(cfg.payload_sets.size()) +
            "; the hard limit is 64.";
    if (extract_literal.size() > 255)
        preparation_error = "Global extraction literal exceeds 255 bytes.";
    if (preparation_error.empty() && !template_shape.error.empty())
        preparation_error = template_shape.error;
    if (preparation_error.empty() && cfg.attack_mode == fuzzer_attack_mode_t::sniper &&
        cfg.payload_sets.size() != 1)
        preparation_error = "Sniper requires exactly 1 payload set; " +
            std::to_string(cfg.payload_sets.size()) + " are configured.";
    if (preparation_error.empty() && cfg.attack_mode != fuzzer_attack_mode_t::sniper &&
        cfg.payload_sets.size() != template_shape.positions)
        preparation_error = "This mode requires exactly " +
            std::to_string(template_shape.positions) + " nonempty payload sets for " +
            std::to_string(template_shape.positions) + " injection positions; " +
            std::to_string(cfg.payload_sets.size()) + " are configured.";
    if (preparation_error.empty() && cfg.attack_mode == fuzzer_attack_mode_t::sniper &&
        (cfg.payload_type < 0 || cfg.payload_type > 2))
        preparation_error = "Sniper payload type " + std::to_string(cfg.payload_type) +
            " is invalid; supported values are 0 to 2.";
    if (preparation_error.empty() && cfg.attack_mode == fuzzer_attack_mode_t::sniper &&
        cfg.payload_type != 2 && cfg.payload_source.empty())
        preparation_error = "The Sniper payload source is empty.";
    if (preparation_error.empty() && cfg.attack_mode != fuzzer_attack_mode_t::sniper) {
        const auto empty_set = std::find_if(cfg.payload_sets.begin(), cfg.payload_sets.end(),
            [](const payload_set_t& set) { return set.source.empty(); });
        if (empty_set != cfg.payload_sets.end())
            preparation_error = "Payload set " + std::to_string(static_cast<std::size_t>(
                std::distance(cfg.payload_sets.begin(), empty_set)) + 1) +
                " has an empty source; every configured set must be nonempty.";
        const auto invalid_set = std::find_if(cfg.payload_sets.begin(), cfg.payload_sets.end(),
            [](const payload_set_t& set) { return set.type < 0 || set.type > 1; });
        if (preparation_error.empty() && invalid_set != cfg.payload_sets.end())
            preparation_error = "Payload set " + std::to_string(static_cast<std::size_t>(
                std::distance(cfg.payload_sets.begin(), invalid_set)) + 1) +
                " has an invalid source type; supported values are 0 and 1.";
    }

    if (preparation_error.empty()) switch (cfg.attack_mode) {

        case fuzzer_attack_mode_t::sniper: {
            sets.push_back(load_legacy_set(preparation_error));
            const std::uint64_t payload_count = static_cast<std::uint64_t>(sets.front().size());
            const std::uint64_t positions = static_cast<std::uint64_t>(template_shape.positions);
            if (preparation_error.empty()) {
                if (payload_count == 0)
                    preparation_error = "The Sniper payload source produced 0 nonempty payloads.";
                else if (positions != 0 && payload_count > cfg.maximum_requests / positions)
                    preparation_error = "Sniper cardinality is " + std::to_string(payload_count) +
                        " payloads x " + std::to_string(positions) +
                        " positions, which exceeds the reviewed maximum of " +
                        std::to_string(cfg.maximum_requests) + " requests.";
                else
                    total = payload_count * positions;
            }
            break;
        }

        case fuzzer_attack_mode_t::pitchfork: {
            if (cfg.payload_sets.empty()) preparation_error = "Pitchfork requires a payload set";
            sets.reserve(cfg.payload_sets.size());
            for (std::size_t set_index = 0; set_index < cfg.payload_sets.size(); ++set_index) {
                if (!preparation_error.empty()) break;
                sets.push_back(load_set(cfg.payload_sets[set_index], preparation_error));
                if (sets.back().empty() && preparation_error.empty())
                    preparation_error = "Pitchfork payload set " +
                        std::to_string(set_index + 1) + " produced 0 nonempty payloads.";
            }
            if (preparation_error.empty() && !sets.empty()) {
                for (std::size_t set_index = 1; set_index < sets.size(); ++set_index) {
                    if (sets[set_index].size() != sets.front().size()) {
                        preparation_error = "Pitchfork payload cardinality mismatch: set 1 has " +
                            std::to_string(sets.front().size()) + " nonempty payloads, but set " +
                            std::to_string(set_index + 1) + " has " +
                            std::to_string(sets[set_index].size()) + ".";
                        break;
                    }
                }
                if (preparation_error.empty())
                    total = static_cast<std::uint64_t>(sets.front().size());
            }
            break;
        }

        case fuzzer_attack_mode_t::clusterbomb: {
            if (cfg.payload_sets.empty()) preparation_error = "Clusterbomb requires a payload set";
            sets.reserve(cfg.payload_sets.size());
            for (std::size_t set_index = 0; set_index < cfg.payload_sets.size(); ++set_index) {
                if (!preparation_error.empty()) break;
                sets.push_back(load_set(cfg.payload_sets[set_index], preparation_error));
                if (sets.back().empty() && preparation_error.empty())
                    preparation_error = "Clusterbomb payload set " +
                        std::to_string(set_index + 1) + " produced 0 nonempty payloads.";
            }
            if (preparation_error.empty()) {
                total = 1;
                for (std::size_t set_index = 0; set_index < sets.size(); ++set_index) {
                    const auto& set = sets[set_index];
                    const std::uint64_t width = static_cast<std::uint64_t>(set.size());
                    if (width != 0 && total > cfg.maximum_requests / width) {
                        preparation_error = "Clusterbomb cardinality exceeds the reviewed maximum of " +
                            std::to_string(cfg.maximum_requests) + " requests at set " +
                            std::to_string(set_index + 1) + " with " +
                            std::to_string(width) + " nonempty payloads.";
                        break;
                    }
                    total *= width;
                }
            }
            break;
        }
    }

    if (state.fuzz_cancel_requested.load(std::memory_order_acquire)) {
        finish_fuzzer_task(state, aida::ui::task_center::task_state_t::cancelled,
            "Cancelled", "Cancelled while loading payload sets");
        return;
    }
    if (preparation_error.empty() && total == 0)
        preparation_error = "No payload combinations were produced";
    if (preparation_error.empty() && total > cfg.maximum_requests)
        preparation_error = "Payload combinations exceed the reviewed request maximum";
    if (!preparation_error.empty()) {
        diag::log_tagged_fmt("network", "fuzzer_prepare_failed mode=%d reason=%s",
            static_cast<int>(cfg.attack_mode), preparation_error.c_str());
        finish_fuzzer_task(state, aida::ui::task_center::task_state_t::failed,
            "Preparation failed", preparation_error);
        return;
    }

    state.fuzz_total.store(total);
    state.fuzz_progress.store(0);
    const auto payload_catalog =
        std::make_shared<const std::vector<std::vector<std::string>>>(std::move(sets));
    {
        std::lock_guard<std::mutex> lock(state.fuzz_mutex);
        state.fuzz_payload_catalog = payload_catalog;
        publish_fuzzer_results_locked(state);
    }
    const auto& retained_sets = *payload_catalog;

    if (!task_id.empty())
        (void)aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::running, 0.0f,
            "Sending reviewed requests", std::to_string(total) + " requests approved");

    struct generated_combination_t {
        combo_t payloads;
        std::vector<std::uint32_t> indices;
        std::uint32_t active_position = 0;
    };
    auto combination_at = [&](std::uint64_t index) {
        generated_combination_t result;
        result.payloads.reserve(retained_sets.size());
        result.indices.reserve(retained_sets.size());
        if (cfg.attack_mode == fuzzer_attack_mode_t::sniper) {
            const std::uint64_t positions = static_cast<std::uint64_t>(template_shape.positions);
            const std::uint64_t payload_index = index / positions;
            result.active_position = static_cast<std::uint32_t>(index % positions);
            result.payloads.push_back(retained_sets.front()[static_cast<std::size_t>(payload_index)]);
            result.indices.push_back(static_cast<std::uint32_t>(payload_index));
        } else if (cfg.attack_mode == fuzzer_attack_mode_t::pitchfork) {
            for (const auto& set : retained_sets) {
                result.payloads.push_back(set[static_cast<std::size_t>(index)]);
                result.indices.push_back(static_cast<std::uint32_t>(index));
            }
        } else {
            result.payloads.resize(retained_sets.size());
            result.indices.resize(retained_sets.size());
            for (std::size_t reverse = retained_sets.size(); reverse != 0; --reverse) {
                const std::size_t set_index = reverse - 1;
                const std::uint64_t width = static_cast<std::uint64_t>(retained_sets[set_index].size());
                const std::uint64_t payload_index = index % width;
                result.payloads[set_index] = retained_sets[set_index][static_cast<std::size_t>(payload_index)];
                result.indices[set_index] = static_cast<std::uint32_t>(payload_index);
                index /= width;
            }
        }
        return result;
    };

    std::atomic<std::uint64_t> next_index{0};
    std::atomic<std::uint64_t> matches{0};
    std::atomic<std::uint64_t> request_failures{0};
    std::atomic<std::uint64_t> last_task_update_ms{0};
    std::atomic<bool> execution_failed{false};
    std::mutex execution_error_mutex;
    std::string execution_error;
    int threads = std::min(std::max(cfg.thread_count, 1), 32);
    diag::log_tagged_fmt("network", "fuzzer_run_start host=%s:%u tls=%d mode=%d combos=%llu threads=%d",
        cfg.host.c_str(), static_cast<unsigned>(cfg.port), cfg.use_tls ? 1 : 0,
        static_cast<int>(cfg.attack_mode), static_cast<unsigned long long>(total), threads);

    auto worker = [&]() {
        while (!state.fuzz_cancel_requested.load(std::memory_order_acquire)) {
            const std::uint64_t idx = next_index.fetch_add(1, std::memory_order_acq_rel);
            if (idx >= total) break;

            generated_combination_t combo = combination_at(idx);
            std::string req_s;
            std::string request_error;
            if (!make_request_multi(cfg.base_request, combo.payloads, template_shape.marker,
                cfg.attack_mode, combo.active_position, req_s, request_error)) {
                bool expected = false;
                if (execution_failed.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel)) {
                    std::lock_guard<std::mutex> error_lock(execution_error_mutex);
                    execution_error = std::move(request_error);
                }
                state.fuzz_cancel_requested.store(true, std::memory_order_release);
                state.fuzz_cv.notify_all();
                break;
            }
            std::vector<uint8_t> raw_req(req_s.begin(), req_s.end());

            auto t0 = GetTickCount64();
            auto res = mitm_proxy::repeat_request(cfg.host, cfg.port, cfg.use_tls, raw_req);
            auto elapsed = GetTickCount64() - t0;

            state_t::fuzzer_result_t fr;
            fr.index     = idx;
            fr.payload_indices = std::move(combo.indices);
            fr.active_position = combo.active_position;
            fr.latency_ms = elapsed;

            if (res.success) {
                fr.status_code  = res.exchange.response.status_code;
                fr.response_len = res.exchange.raw_response.size();
                const std::size_t inspection_size = (std::min)(
                    res.exchange.raw_response.size(), k_fuzzer_match_input_limit);
                std::string body(res.exchange.raw_response.begin(),
                    res.exchange.raw_response.begin() + inspection_size);
                fr.response_preview = body.substr(0, (std::min)(std::size_t{200}, body.size()));
                fr.match = check_match(fr.status_code, body, fr.response_len);


                if (!extract_literal.empty()) {
                    fr.extracted_value = do_grep_extract(body);
                }
            } else {
                fr.error = res.error.empty() ? "Request failed without an error detail" : res.error;
                if (fr.error.size() > 160) fr.error.resize(160);
                request_failures.fetch_add(1, std::memory_order_acq_rel);
            }

            const bool matched = fr.match;
            if (matched)
                matches.fetch_add(1, std::memory_order_acq_rel);
            append_fuzzer_result(state, std::move(fr));
            const std::uint64_t completed = state.fuzz_progress.fetch_add(
                1, std::memory_order_acq_rel) + 1;
            const std::uint64_t now = network_now_ms();
            std::uint64_t prior = last_task_update_ms.load(std::memory_order_acquire);
            if (!task_id.empty() && (completed == total || now - prior >= 200) &&
                last_task_update_ms.compare_exchange_strong(prior, now, std::memory_order_acq_rel)) {
                const float fraction = static_cast<float>(
                    static_cast<double>(completed) / static_cast<double>(total));
                (void)aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::running, fraction,
                    "Sending reviewed requests",
                    std::to_string(completed) + " of " + std::to_string(total));
            }

            if (cfg.stop_on_match && matched) {
                state.fuzz_cancel_requested.store(true, std::memory_order_release);
                state.fuzz_cv.notify_all();
                break;
            }
            if (cfg.delay_ms > 0) {
                std::unique_lock<std::mutex> delay_lock(state.fuzz_cv_mutex);
                state.fuzz_cv.wait_for(delay_lock, std::chrono::milliseconds(cfg.delay_ms), [&state] {
                    return state.fuzz_cancel_requested.load(std::memory_order_acquire) ||
                           !state.fuzz_thread_alive.load(std::memory_order_acquire);
                });
            }
        }
    };

    auto guarded_worker = [&]() noexcept {
        try {
            worker();
        } catch (const std::exception& e) {
            bool expected = false;
            if (execution_failed.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
                std::lock_guard<std::mutex> error_lock(execution_error_mutex);
                execution_error = e.what();
            }
            state.fuzz_cancel_requested.store(true, std::memory_order_release);
            state.fuzz_cv.notify_all();
        } catch (...) {
            bool expected = false;
            if (execution_failed.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
                std::lock_guard<std::mutex> error_lock(execution_error_mutex);
                execution_error = "Unexpected fuzzer request worker failure";
            }
            state.fuzz_cancel_requested.store(true, std::memory_order_release);
            state.fuzz_cv.notify_all();
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads - 1));
    try {
        for (int t = 1; t < threads; ++t)
            workers.emplace_back(guarded_worker);
    } catch (const std::exception& e) {
        diag::log_tagged_fmt("network", "fuzzer_worker_creation_limited requested=%d created=%zu reason=%s",
            threads, workers.size() + 1, e.what());
    }
    guarded_worker();
    for (auto& thread : workers)
        if (thread.joinable()) thread.join();

    const std::uint64_t final_progress = state.fuzz_progress.load(std::memory_order_acquire);
    std::uint64_t retained_count = 0;
    std::uint64_t dropped_count = 0;
    {
        std::lock_guard<std::mutex> lk(state.fuzz_mutex);
        publish_fuzzer_results_locked(state);
        retained_count = state.fuzz_retained_count;
        dropped_count = state.fuzz_dropped_count;
    }
    const std::uint64_t match_count = matches.load(std::memory_order_acquire);
    const std::uint64_t failure_count = request_failures.load(std::memory_order_acquire);
    diag::log_tagged_fmt("network", "fuzzer_run_complete combos=%llu processed=%llu retained=%llu dropped=%llu matches=%llu failures=%llu cancelled=%d",
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(final_progress),
        static_cast<unsigned long long>(retained_count),
        static_cast<unsigned long long>(dropped_count),
        static_cast<unsigned long long>(match_count),
        static_cast<unsigned long long>(failure_count),
        state.fuzz_cancel_requested.load(std::memory_order_acquire) ? 1 : 0);

    const bool stop_on_match = cfg.stop_on_match && match_count != 0;
    const bool failed = execution_failed.load(std::memory_order_acquire);
    const bool cancelled = state.fuzz_cancel_requested.load(std::memory_order_acquire) &&
        !stop_on_match && !failed;
    const std::string summary = std::to_string(final_progress) + " processed, " +
        std::to_string(match_count) + " matched, " + std::to_string(failure_count) + " failed, " +
        std::to_string(retained_count) + " retained" +
        (dropped_count == 0 ? std::string() :
            ", " + std::to_string(dropped_count) + " older results discarded");
    if (failed) {
        std::string failure;
        {
            std::lock_guard<std::mutex> error_lock(execution_error_mutex);
            failure = execution_error;
        }
        finish_fuzzer_task(state,
            final_progress == 0 ? aida::ui::task_center::task_state_t::failed
                                : aida::ui::task_center::task_state_t::partial,
            "Request expansion failed", failure + "; " + summary);
    } else if (!cancelled && !stop_on_match && failure_count != 0) {
        finish_fuzzer_task(state, aida::ui::task_center::task_state_t::partial,
            "Completed with request failures", summary);
    } else {
        finish_fuzzer_task(state,
            cancelled ? aida::ui::task_center::task_state_t::cancelled
                      : aida::ui::task_center::task_state_t::completed,
            cancelled ? "Cancelled after in-flight request" :
                (stop_on_match ? "Stopped on match" : "Completed"),
            summary);
    }
}

}

fuzzer_template_shape_t analyze_fuzzer_template(std::string_view request) {
    fuzzer_template_shape_t shape;
    const std::string_view value_marker = "$value$";
    const std::string_view fuzz_marker = "FUZZ";
    auto count = [&](std::string_view marker) {
        std::size_t result = 0;
        std::size_t offset = 0;
        while ((offset = request.find(marker, offset)) != std::string_view::npos) {
            ++result;
            offset += marker.size();
        }
        return result;
    };
    const std::size_t values = count(value_marker);
    const std::size_t fuzzes = count(fuzz_marker);
    if (values != 0 && fuzzes != 0) {
        shape.error = "Mixed marker syntax is invalid: found " + std::to_string(values) +
            " $value$ markers and " + std::to_string(fuzzes) + " FUZZ markers.";
        return shape;
    }
    std::string residue(request);
    std::size_t complete = 0;
    while ((complete = residue.find(value_marker, complete)) != std::string::npos)
        residue.erase(complete, value_marker.size());
    std::string folded_residue = residue;
    std::transform(folded_residue.begin(), folded_residue.end(), folded_residue.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (folded_residue.find("$value") != std::string::npos ||
        folded_residue.find("value$") != std::string::npos) {
        shape.error = "Malformed injection marker: use the exact case-sensitive marker $value$ or FUZZ.";
        return shape;
    }
    shape.marker = values != 0 ? std::string(value_marker) : std::string(fuzz_marker);
    shape.positions = values != 0 ? values : fuzzes;
    if (shape.positions == 0)
        shape.error = "Add at least one $value$ or FUZZ injection marker.";
    else if (shape.positions > k_fuzzer_payload_set_limit)
        shape.error = "The request has " + std::to_string(shape.positions) +
            " injection positions; the hard limit is 64.";
    return shape;
}

FuzzerController::FuzzerController(QObject* parent)
    : QObject(parent) {}

FuzzerController::~FuzzerController() = default;

bool FuzzerController::workerAvailable() const {
    return g_state.fuzz_thread_alive.load(std::memory_order_acquire) &&
        !g_state.fuzz_thread_done.load(std::memory_order_acquire);
}

bool FuzzerController::startWorker() {
    auto& state = g_state;
    if (!state.fuzz_thread_done.load(std::memory_order_acquire) &&
        state.fuzz_thread_alive.load(std::memory_order_acquire)) {
        return true;
    }
    state.fuzz_thread_alive.store(true, std::memory_order_release);
    state.fuzz_thread_done.store(false, std::memory_order_release);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = "fuzzer";
    submission.thread_class = "long_running";
    submission.domain = aida::infra::executor::domain_t::long_running;
    submission.priority = 3;
    submission.body = []() {
        diag::log_tagged_fmt("network",
            "executor_task_enter name=%s domain=%s tid=%lu",
            "fuzzer",
            aida::infra::executor::domain_name(aida::infra::executor::domain_t::long_running),
            static_cast<unsigned long>(GetCurrentThreadId()));
        try {
            diag::log_tagged("network", "fuzzer_thread_started");
            while (true) {
                {
                    std::unique_lock<std::mutex> lk(g_state.fuzz_cv_mutex);
                    g_state.fuzz_cv.wait(lk, []() {
                        return g_state.fuzz_running.load() || !g_state.fuzz_thread_alive.load();
                    });
                }
                if (!g_state.fuzz_thread_alive.load())
                    break;
                run_fuzzer_thread(g_state);
            }
        } catch (const std::exception& e) {
            diag::log_tagged_fmt("network", "fuzzer_cpp_exception what=%s", e.what());
            finish_fuzzer_task(g_state, aida::ui::task_center::task_state_t::failed,
                "Execution failed", e.what());
        } catch (...) {
            diag::log_tagged("network", "fuzzer_unknown_exception");
            finish_fuzzer_task(g_state, aida::ui::task_center::task_state_t::failed,
                "Execution failed", "Unexpected fuzzer worker failure");
        }
        g_state.fuzz_thread_alive.store(false, std::memory_order_release);
        g_state.fuzz_thread_done.store(true, std::memory_order_release);
        g_state.fuzz_cv.notify_all();
        diag::log_tagged("network", "fuzzer_thread_exited");
        diag::log_tagged_fmt("network",
            "executor_task_exit name=%s domain=%s tid=%lu",
            "fuzzer",
            aida::infra::executor::domain_name(aida::infra::executor::domain_t::long_running),
            static_cast<unsigned long>(GetCurrentThreadId()));
    };
    bool ok = false;
    std::string reject_reason;
    try {
        auto submit_result = aida::infra::executor::submit(std::move(submission));
        ok = submit_result.submitted;
        reject_reason = submit_result.reject_reason;
    } catch (const std::exception& e) {
        diag::log_tagged_fmt("network", "executor_post_cpp_exception name=%s what=%s",
            "fuzzer", e.what());
        ok = false;
    } catch (...) {
        diag::log_tagged_fmt("network", "executor_post_unknown_exception name=%s", "fuzzer");
        ok = false;
    }
    diag::log_tagged_fmt("network", "executor_post name=%s domain=%s ok=%d reject=%s",
        "fuzzer",
        aida::infra::executor::domain_name(aida::infra::executor::domain_t::long_running),
        ok ? 1 : 0, reject_reason.empty() ? "<none>" : reject_reason.c_str());
    if (ok) {
        return true;
    }
    state.fuzz_thread_alive.store(false, std::memory_order_release);
    state.fuzz_thread_done.store(true, std::memory_order_release);
    diag::log_tagged("network", "fuzzer_thread_post_failed");
    return false;
}

void FuzzerController::shutdownWorker() {
    auto& state = g_state;
    state.fuzz_cancel_requested.store(true, std::memory_order_release);
    state.fuzz_running.store(false);
    state.fuzz_thread_alive.store(false);
    state.fuzz_cv.notify_all();
    const std::uint64_t begin = static_cast<std::uint64_t>(GetTickCount64());
    std::uint64_t next_report = 2500;
    while (!state.fuzz_thread_done.load(std::memory_order_acquire)) {
        const std::uint64_t elapsed = static_cast<std::uint64_t>(GetTickCount64()) - begin;
        if (elapsed >= next_report) {
            diag::log_tagged_fmt("network",
                "shutdown_dependency_drain_pending worker=%s elapsed_ms=%llu",
                "fuzzer", static_cast<unsigned long long>(elapsed));
            next_report = elapsed + 2500;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    diag::log_tagged_fmt("network",
        "shutdown_dependency_drain_done worker=%s elapsed_ms=%llu",
        "fuzzer", static_cast<unsigned long long>(
            static_cast<std::uint64_t>(GetTickCount64()) - begin));
    Q_EMIT workerStateChanged();
}

void FuzzerController::clearResults() {
    auto& state = g_state;
    std::lock_guard<std::mutex> lk(state.fuzz_mutex);
    const std::uint64_t previous = state.fuzz_retained_count;
    clear_fuzzer_results_locked(state);
    diag::log_tagged_fmt("network", "fuzzer_results_cleared prev=%llu",
        static_cast<unsigned long long>(previous));
}

void FuzzerController::beginRun(const network_view::state_t::fuzzer_entry_t& config,
                                const std::string& taskId) {
    auto& state = g_state;
    std::lock_guard<std::mutex> lk(state.fuzz_mutex);
    clear_fuzzer_results_locked(state);
    state.fuzz_active_config = config;
    state.fuzz_task_id = taskId;
    state.fuzz_last_stage = "Loading payload sets";
    state.fuzz_last_error.clear();
}

void FuzzerController::rejectRun() {
    auto& state = g_state;
    std::lock_guard<std::mutex> lock(state.fuzz_mutex);
    state.fuzz_task_id.clear();
    state.fuzz_last_stage = "Start rejected";
    state.fuzz_last_error = "Task Center rejected the fuzzer operation before execution";
}

void fuzzer_controller_start() {
    if (!FuzzerController::instance())
        FuzzerController::setInstance(new FuzzerController());
    FuzzerController::instance()->startWorker();
}

void fuzzer_controller_shutdown() {
    auto* controller = FuzzerController::instance();
    if (!controller)
        return;
    controller->shutdownWorker();
    FuzzerController::setInstance(nullptr);
    delete controller;
}

}
