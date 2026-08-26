#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/network/network_view.hpp"

namespace aida::qt::net {

// WsFrameStore is the WebSocket capture ring moved out of network_view.cpp's
// state_t (ws_frames deque + ws_mutex + ws_max_frames 4096,
// network_view.hpp:501-503) with two additions per plan 11 section 13.1: a
// monotonic append_serial per frame and a dropped_total eviction counter.
// Entries are shared_ptr<const ws_frame_entry_t> so the pane's paged model
// fetches snapshots cheaply. The mitm set_ws_frame_callback lambda retargets
// here. Qt-free by design: both the core capture TU and the Qt pane link it.
class WsFrameStore {
public:
    using entry_t = network_view::state_t::ws_frame_entry_t;
    using entry_ptr_t = std::shared_ptr<const entry_t>;

    struct fetch_result_t {
        std::vector<entry_ptr_t> entries;
        std::uint64_t head_serial = 0;
        std::uint64_t dropped_total = 0;
    };

    static WsFrameStore& instance();

    void append(entry_t entry);
    fetch_result_t fetchAfter(std::uint64_t after_serial, std::size_t max) const;
    bool findPayload(std::uint64_t exchange_id, std::uint64_t timestamp,
                     std::size_t content_size, std::vector<std::uint8_t>& bytes) const;
    void clear();

    std::uint64_t appendSerial() const noexcept {
        return append_serial_.load(std::memory_order_acquire);
    }
    std::uint64_t droppedTotal() const noexcept {
        return dropped_total_.load(std::memory_order_acquire);
    }
    std::size_t size() const;
    static constexpr std::size_t maxFrames() noexcept { return 4096; }

private:
    struct stored_t {
        std::uint64_t serial = 0;
        entry_ptr_t frame;
    };

    mutable std::mutex mutex_;
    std::deque<stored_t> frames_;
    std::atomic<std::uint64_t> append_serial_{0};
    std::atomic<std::uint64_t> dropped_total_{0};
};

// View-state bridge for the retained context-menu actions
// network.websocket.filter_host / network.websocket.toggle_follow (the
// network_view.cpp action lambdas call the request_* functions; the WsPane
// installs the hooks and drains any state requested before it existed).
struct ws_frame_view_hooks_t {
    std::function<void(const std::string& host)> set_filter_host;
    std::function<void()> toggle_follow;
};

void set_ws_frame_view_hooks(ws_frame_view_hooks_t hooks);
void request_ws_filter_host(const std::string& host);
void request_ws_toggle_follow();
std::pair<bool, std::string> take_pending_ws_filter_host();
bool take_pending_ws_follow_toggle();

}
