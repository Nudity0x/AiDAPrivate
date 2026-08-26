#include "qt/network/shared/event_bus_bridge.hpp"

#include <QMetaObject>
#include <Qt>

#include <utility>

namespace aida::qt::net {

NetworkEventBusBridge::NetworkEventBusBridge(QObject* parent)
    : QObject(parent) {
    registerInstance(this);
    exchange_sub_ = aida::events::subscribe(aida::burp::kExchangeObservedEvent,
        [this](const aida::burp::exchange_observed_t& exchange) {
            QMetaObject::invokeMethod(this, [this, exchange] {
                Q_EMIT exchangeObserved(exchange);
            }, Qt::QueuedConnection);
        });
    scope_sub_ = aida::events::subscribe(aida::burp::kScopeChangedEvent,
        [this](const aida::burp::scope_changed_t& change) {
            QMetaObject::invokeMethod(this, [this, change] {
                Q_EMIT scopeChanged(change);
            }, Qt::QueuedConnection);
        });
    cookie_sub_ = aida::events::subscribe(aida::burp::kCookieChangedEvent,
        [this](const aida::burp::cookie_changed_t& change) {
            QMetaObject::invokeMethod(this, [this, change] {
                Q_EMIT cookieChanged(change);
            }, Qt::QueuedConnection);
        });
}

NetworkEventBusBridge::~NetworkEventBusBridge() {
    clearInstance(this);
    if (exchange_sub_.valid())
        aida::events::unsubscribe(exchange_sub_);
    if (scope_sub_.valid())
        aida::events::unsubscribe(scope_sub_);
    if (cookie_sub_.valid())
        aida::events::unsubscribe(cookie_sub_);
}

}
