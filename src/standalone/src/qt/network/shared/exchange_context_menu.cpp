#include "qt/network/shared/exchange_context_menu.hpp"

#include <QCursor>
#include <QWidget>

#include <utility>

#include "core/ui/context_menu_contract.hpp"
#include "core/ui/shell_host_contract.hpp"
#include "qt/documents/context_menu_hook.hpp"

namespace aida::qt::net {

ExchangeContextMenu::ExchangeContextMenu(QObject* parent)
    : QObject(parent) {}

void ExchangeContextMenu::show(QWidget* invoker, const QPoint& globalPos,
                               network_view::artifact_identity_t primary,
                               network_view::artifact_identity_t related,
                               network_view::exchange_context_origin_t origin,
                               bool includeInterceptActions) {
    currentInvoker_ = invoker;
    currentPos_ = globalPos.isNull()
        ? (invoker ? invoker->mapToGlobal(invoker->rect().center()) : QCursor::pos())
        : globalPos;
    network_view::open_exchange_context(std::move(primary), std::move(related), origin,
        includeInterceptActions);
}

void ExchangeContextMenu::installDisplaySink() {
    network_view::set_exchange_context_display(
        [this](aida::ui::application_ui::retained_entity_context_t context,
               aida::ui::context_menu_open_origin_t origin) {
            dispatchDisplay(std::move(context), origin);
        });
}

void ExchangeContextMenu::dispatchDisplay(
    aida::ui::application_ui::retained_entity_context_t context,
    aida::ui::context_menu_open_origin_t origin) {
    QWidget* invoker = currentInvoker_.data();
    const QPoint pos = currentPos_.isNull() ? QCursor::pos() : currentPos_;
    aida::qt::documents::show_retained_entity_menu(context, origin, pos, invoker);
}

ExchangeContextMenu& exchange_context_host() {
    static ExchangeContextMenu* host = [] {
        auto* created = new ExchangeContextMenu();
        created->installDisplaySink();
        return created;
    }();
    return *host;
}

}
