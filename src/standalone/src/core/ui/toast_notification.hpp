#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <utility>

namespace toast_notification
{
    enum class toast_type_t : int
    {
        info = 0,
        success,
        warning,
        error
    };

    struct action_t
    {
        std::string label;
        std::function<void()> on_click;
    };

    struct toast_t
    {
        std::uint64_t id = 0;
        std::string message;
        toast_type_t type = toast_type_t::info;
        float duration = 4.0f;
        float elapsed = 0.0f;
        float intro_progress = 0.0f;
        float fade_out = 1.0f;
        float current_x = 0.0f;
        float current_y = 0.0f;
        float target_x = 0.0f;
        float target_y = 0.0f;
        float velocity_x = 0.0f;
        float velocity_y = 0.0f;
        float swipe_offset = 0.0f;
        float swipe_velocity = 0.0f;
        float dismiss_alpha_decay = 1.0f;
        float hover_amount = 0.0f;
        float hover_velocity = 0.0f;
        bool dismissing = false;
        bool swipe_dismissing = false;
        bool initialized_position = false;
        bool was_dragging = false;
        bool drag_owned = false;
        bool press_started_inside = false;
        action_t action;
        bool has_action = false;
        bool action_clicked_this_frame = false;
    };

    inline constexpr std::size_t MAX_VISIBLE = 5;
    inline constexpr float DEDUP_WINDOW      = 3.0f;

    namespace detail
    {
        inline std::mutex            s_mtx;
        inline std::vector<toast_t>  s_toasts;
        inline std::uint64_t         s_next_id = 1;
    }

    using qt_forward_fn_t = std::function<void(const std::string& message, toast_type_t type,
        float duration, std::string action_label, std::function<void()> on_click)>;

    namespace detail
    {
        struct qt_forward_hook_t
        {
            std::mutex mtx;
            qt_forward_fn_t fn;
        };

        inline qt_forward_hook_t& qt_forward_hook()
        {
            static qt_forward_hook_t hook;
            return hook;
        }

        inline bool forward_to_qt(const std::string& message, toast_type_t type, float duration,
            std::string action_label, std::function<void()> on_click)
        {
            qt_forward_fn_t forward;
            {
                auto& hook = qt_forward_hook();
                std::lock_guard<std::mutex> lk(hook.mtx);
                forward = hook.fn;
            }
            if (!forward)
                return false;
            forward(message, type, duration, std::move(action_label), std::move(on_click));
            return true;
        }
    }

    inline void install_qt_forward(qt_forward_fn_t forward)
    {
        auto& hook = detail::qt_forward_hook();
        std::lock_guard<std::mutex> lk(hook.mtx);
        hook.fn = std::move(forward);
    }

    inline void push(const std::string& message,
                     toast_type_t type = toast_type_t::info,
                     float duration = 4.0f)
    {
        if (message.empty())
            return;

        if (detail::forward_to_qt(message, type, duration, std::string{}, nullptr))
            return;

        std::lock_guard<std::mutex> lk(detail::s_mtx);

        for (const auto& t : detail::s_toasts) {
            if (t.message == message && t.elapsed < DEDUP_WINDOW &&
                !t.dismissing && !t.swipe_dismissing)
                return;
        }

        toast_t nt;
        nt.id = detail::s_next_id++;
        nt.message = message;
        nt.type = type;
        nt.duration = duration;
        detail::s_toasts.push_back(std::move(nt));

        while (detail::s_toasts.size() > MAX_VISIBLE * std::size_t{2}) {
            detail::s_toasts.erase(detail::s_toasts.begin());
        }
    }

    inline void push_with_action(const std::string& message,
                                  toast_type_t type,
                                  action_t action,
                                  float duration = 6.0f)
    {
        if (message.empty())
            return;

        if (detail::forward_to_qt(message, type, duration, std::move(action.label),
                std::move(action.on_click)))
            return;

        std::lock_guard<std::mutex> lk(detail::s_mtx);

        for (const auto& t : detail::s_toasts) {
            if (t.message == message && t.elapsed < DEDUP_WINDOW &&
                !t.dismissing && !t.swipe_dismissing)
                return;
        }

        toast_t nt;
        nt.id = detail::s_next_id++;
        nt.message = message;
        nt.type = type;
        nt.duration = duration;
        nt.action = std::move(action);
        nt.has_action = !nt.action.label.empty();
        detail::s_toasts.push_back(std::move(nt));

        while (detail::s_toasts.size() > MAX_VISIBLE * std::size_t{2}) {
            detail::s_toasts.erase(detail::s_toasts.begin());
        }
    }

}
