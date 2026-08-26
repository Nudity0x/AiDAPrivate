#pragma once

#include <QObject>
#include <QPoint>
#include <QPointer>

#include "core/network/network_view.hpp"

class QWidget;

namespace aida::qt::net {

// Host for the exchange artifact context menu. Panes call show() with the
// invoking widget and the position; the byte-verbatim backend
// (network_view::open_exchange_context) builds the retained context and the
// installed display sink routes it to the composed menu display
// (qt/documents/context_menu_hook.hpp -> MenuBridge::show_retained_entity_menu,
// composed lazily in QMenu::aboutToShow per the menu bridge contract).
class ExchangeContextMenu : public QObject {
    Q_OBJECT
public:
    explicit ExchangeContextMenu(QObject* parent = nullptr);

    void show(QWidget* invoker, const QPoint& globalPos,
              network_view::artifact_identity_t primary,
              network_view::artifact_identity_t related,
              network_view::exchange_context_origin_t origin,
              bool includeInterceptActions = false);

    void installDisplaySink();

private:
    void dispatchDisplay(aida::ui::application_ui::retained_entity_context_t context,
                         aida::ui::context_menu_open_origin_t origin);

    QPointer<QWidget> currentInvoker_;
    QPoint currentPos_;
};

// The domain-lifetime host, created on first use on the GUI thread.
ExchangeContextMenu& exchange_context_host();

}
