#include "qt/qt_eventbus_bridge.hpp"

namespace aida::qt {

AidaEventBusBridge::AidaEventBusBridge(QObject* parent)
    : QObject(parent)
{
}

AidaEventBusBridge::~AidaEventBusBridge()
{
    unsubscribe_all();
}

void AidaEventBusBridge::unsubscribe_all()
{
    std::vector<aida::events::subscription_handle_t> handles;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        handles.swap(handles_);
    }
    for (const auto& handle : handles) {
        const bool removed = aida::events::unsubscribe(handle);
        diag::log_tagged_fmt("qt_eventbus",
            "unsubscribe_gui topic=%s id=%llu removed=%d",
            handle.type_name.c_str(),
            static_cast<unsigned long long>(handle.id),
            removed ? 1 : 0);
    }
}

}
