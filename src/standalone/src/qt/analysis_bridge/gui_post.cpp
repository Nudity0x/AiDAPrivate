#include "qt/analysis_bridge/gui_post.hpp"

#include "helpers/diag_log.hpp"

#include <mutex>

namespace aida::qt {

namespace {

std::mutex& gui_post_mutex()
{
    static std::mutex value;
    return value;
}

gui_post_fn& gui_post_slot()
{
    static gui_post_fn value;
    return value;
}

}

void set_gui_post(gui_post_fn fn)
{
    std::lock_guard<std::mutex> lock(gui_post_mutex());
    gui_post_slot() = std::move(fn);
}

gui_post_fn gui_post()
{
    std::lock_guard<std::mutex> lock(gui_post_mutex());
    return gui_post_slot();
}

bool gui_post_installed()
{
    std::lock_guard<std::mutex> lock(gui_post_mutex());
    return static_cast<bool>(gui_post_slot());
}

void gui_post_or_run(std::function<void()> fn)
{
    const auto poster = gui_post();
    if (poster) {
        poster(std::move(fn));
        return;
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        diag::log_tagged("qt_gui_post",
            "gui_post_or_run without installed poster; executing inline");
    }
    fn();
}

}
