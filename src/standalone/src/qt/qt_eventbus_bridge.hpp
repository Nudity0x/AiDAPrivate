#pragma once

#include <QMetaObject>
#include <QObject>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "core/infra/event_bus.hpp"
#include "helpers/diag_log.hpp"

namespace aida::qt {

class AidaEventBusBridge : public QObject
{
    Q_OBJECT
public:
    explicit AidaEventBusBridge(QObject* parent = nullptr);
    ~AidaEventBusBridge() override;

    template <typename Payload>
    aida::events::subscription_handle_t subscribe_gui(
        const aida::events::event_def_t<Payload>& def,
        std::function<void(const Payload&)> functor)
    {
        const char* topic = def.type_name ? def.type_name : "<null>";
        if (!functor) {
            diag::log_tagged_critical_fmt("qt_eventbus",
                "subscribe_gui_rejected topic=%s reason=null_functor",
                topic);
            return {};
        }
        QObject* context = this;
        aida::events::subscription_handle_t handle = aida::events::subscribe(def,
            [context, functor](const Payload& payload) {
                Payload copy = payload;
                const bool posted = QMetaObject::invokeMethod(context,
                    [copy = std::move(copy), functor]() mutable { functor(copy); },
                    Qt::QueuedConnection);
                if (!posted) {
                    diag::log_tagged_fmt("qt_eventbus",
                        "gui_delivery_failed ctx=0x%llX tid=%lu",
                        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(context)),
                        static_cast<unsigned long>(::GetCurrentThreadId()));
                }
            });
        if (!handle.valid()) {
            diag::log_tagged_critical_fmt("qt_eventbus",
                "subscribe_gui_failed topic=%s error=%s",
                topic,
                aida::events::last_error().c_str());
            return handle;
        }
        {
            std::lock_guard<std::mutex> lock(mtx_);
            handles_.push_back(handle);
        }
        diag::log_tagged_fmt("qt_eventbus",
            "subscribe_gui_ok topic=%s id=%llu",
            topic,
            static_cast<unsigned long long>(handle.id));
        return handle;
    }

    void unsubscribe_all();

private:
    std::mutex mtx_;
    std::vector<aida::events::subscription_handle_t> handles_;
};

}
