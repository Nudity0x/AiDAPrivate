#include "qt/network/websocket/ws_frame_store.hpp"

#include <utility>

namespace aida::qt::net {

WsFrameStore& WsFrameStore::instance() {
    static WsFrameStore store;
    return store;
}

void WsFrameStore::append(entry_t entry) {
    auto retained = std::make_shared<const entry_t>(std::move(entry));
    const std::uint64_t serial = append_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.push_back(stored_t{serial, std::move(retained)});
    if (frames_.size() > maxFrames()) {
        const std::size_t excess = frames_.size() - maxFrames();
        for (std::size_t i = 0; i < excess; ++i)
            frames_.pop_front();
        dropped_total_.fetch_add(static_cast<std::uint64_t>(excess), std::memory_order_acq_rel);
    }
}

WsFrameStore::fetch_result_t WsFrameStore::fetchAfter(std::uint64_t after_serial,
                                                      std::size_t max) const {
    fetch_result_t result;
    result.head_serial = after_serial;
    result.dropped_total = droppedTotal();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& stored : frames_) {
        if (stored.serial <= after_serial)
            continue;
        if (result.entries.size() >= max)
            break;
        result.entries.push_back(stored.frame);
        result.head_serial = stored.serial;
    }
    return result;
}

bool WsFrameStore::findPayload(std::uint64_t exchange_id, std::uint64_t timestamp,
                               std::size_t content_size,
                               std::vector<std::uint8_t>& bytes) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& stored : frames_) {
        const auto& frame = *stored.frame;
        if (frame.exchange_id == exchange_id && frame.timestamp == timestamp &&
            frame.payload.size() == content_size) {
            bytes = frame.payload;
            return true;
        }
    }
    return false;
}

void WsFrameStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
}

std::size_t WsFrameStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
}

namespace {

std::mutex g_hooks_mutex;
ws_frame_view_hooks_t g_hooks;
bool g_pending_filter_set = false;
std::string g_pending_filter_host;
bool g_pending_follow_toggle = false;

}

void set_ws_frame_view_hooks(ws_frame_view_hooks_t hooks) {
    std::lock_guard<std::mutex> lock(g_hooks_mutex);
    g_hooks = std::move(hooks);
}

void request_ws_filter_host(const std::string& host) {
    std::function<void(const std::string&)> hook;
    {
        std::lock_guard<std::mutex> lock(g_hooks_mutex);
        hook = g_hooks.set_filter_host;
        if (!hook) {
            g_pending_filter_set = true;
            g_pending_filter_host = host;
            return;
        }
    }
    hook(host);
}

void request_ws_toggle_follow() {
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(g_hooks_mutex);
        hook = g_hooks.toggle_follow;
        if (!hook) {
            g_pending_follow_toggle = !g_pending_follow_toggle;
            return;
        }
    }
    hook();
}

std::pair<bool, std::string> take_pending_ws_filter_host() {
    std::lock_guard<std::mutex> lock(g_hooks_mutex);
    const bool set = g_pending_filter_set;
    std::string host = g_pending_filter_host;
    g_pending_filter_set = false;
    g_pending_filter_host.clear();
    return {set, std::move(host)};
}

bool take_pending_ws_follow_toggle() {
    std::lock_guard<std::mutex> lock(g_hooks_mutex);
    const bool toggle = g_pending_follow_toggle;
    g_pending_follow_toggle = false;
    return toggle;
}

}
