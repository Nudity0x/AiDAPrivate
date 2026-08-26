#pragma once

#include <QObject>

#include "core/network/burp/burp_events.hpp"

namespace aida::qt::net {

// NetworkEventBusBridge has GUI affinity and must be created on the GUI
// thread. The underlying event bus invokes subscribers synchronously on the
// publisher thread; the bridge's subscriber lambdas therefore do exactly one
// thing: re-post the payload to the bridge through a queued invokeMethod
// functor (compile-time metatype, no qRegisterMetaType needed;
// qmetaobject.cpp:1735-1754). Delivery to a destroyed bridge is dropped
// silently by Qt (qobject.cpp:201-202). Unsubscribe happens in the destructor
// so the bridge is gone before event_bus teardown (shutdown ordering).
class NetworkEventBusBridge : public QObject {
    Q_OBJECT
public:
    explicit NetworkEventBusBridge(QObject* parent = nullptr);
    ~NetworkEventBusBridge() override;

    NetworkEventBusBridge(const NetworkEventBusBridge&) = delete;
    NetworkEventBusBridge& operator=(const NetworkEventBusBridge&) = delete;

    static NetworkEventBusBridge* instance() noexcept { return instance_; }
    static void registerInstance(NetworkEventBusBridge* bridge) noexcept { instance_ = bridge; }
    static void clearInstance(NetworkEventBusBridge* bridge) noexcept {
        if (instance_ == bridge)
            instance_ = nullptr;
    }

private:
    static inline NetworkEventBusBridge* instance_ = nullptr;

Q_SIGNALS:
    void exchangeObserved(const aida::burp::exchange_observed_t& exchange);
    void scopeChanged(const aida::burp::scope_changed_t& change);
    void cookieChanged(const aida::burp::cookie_changed_t& change);

private:
    aida::events::subscription_handle_t exchange_sub_;
    aida::events::subscription_handle_t scope_sub_;
    aida::events::subscription_handle_t cookie_sub_;
};

}
