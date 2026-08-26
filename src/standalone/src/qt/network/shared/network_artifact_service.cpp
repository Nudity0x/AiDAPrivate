#include "qt/network/shared/network_artifact_service.hpp"

#include <utility>

namespace aida::qt::net {

NetworkArtifactActions::NetworkArtifactActions(QObject* parent)
    : QObject(parent) {}

bool NetworkArtifactActions::resolve(const network_view::artifact_identity_t& identity,
                                     network_view::artifact_snapshot_t& snapshot,
                                     QString& unavailableReason) {
    std::string reason;
    const bool ok = network_view::resolve_artifact(identity, snapshot, reason);
    unavailableReason = QString::fromStdString(reason);
    return ok;
}

bool NetworkArtifactActions::sendToRepeater(const network_view::artifact_identity_t& identity,
                                            QString& unavailableReason) {
    std::string reason;
    const bool ok = network_view::send_artifact_to_repeater(identity, reason);
    unavailableReason = QString::fromStdString(reason);
    return ok;
}

bool NetworkArtifactActions::sendToComparer(const network_view::artifact_identity_t& identity,
                                            QString& unavailableReason) {
    std::string reason;
    const bool ok = network_view::send_artifact_to_comparer(identity, reason);
    unavailableReason = QString::fromStdString(reason);
    return ok;
}

bool NetworkArtifactActions::addToChat(const network_view::artifact_identity_t& identity,
                                       QString& unavailableReason) {
    std::string reason;
    const bool ok = network_view::add_artifact_to_chat(identity, reason);
    unavailableReason = QString::fromStdString(reason);
    return ok;
}

bool NetworkArtifactActions::assignToAgent(const network_view::artifact_identity_t& identity,
                                           QString& unavailableReason) {
    std::string reason;
    const bool ok = network_view::assign_artifact_to_agent(identity, reason);
    unavailableReason = QString::fromStdString(reason);
    return ok;
}

bool NetworkArtifactActions::makeSitemapArtifact(std::uint64_t exchangeId,
                                                 network_view::artifact_kind_t kind,
                                                 network_view::artifact_identity_t& identity,
                                                 QString& unavailableReason) {
    std::string reason;
    const bool ok = network_view::make_sitemap_artifact(exchangeId, kind, identity, reason);
    unavailableReason = QString::fromStdString(reason);
    return ok;
}

bool NetworkArtifactActions::executeToolbarAction(const QString& actionId,
                                                  network_view::artifact_identity_t primary,
                                                  network_view::artifact_identity_t related,
                                                  QString& unavailableReason) {
    std::string reason;
    const bool ok = network_view::execute_retained_exchange_toolbar_action(
        actionId.toUtf8().constData(), std::move(primary), std::move(related), reason);
    unavailableReason = QString::fromStdString(reason);
    return ok;
}

void NetworkArtifactActions::publishSelection(const network_view::artifact_identity_t& identity,
                                              bool force) {
    network_view::publish_network_selection(identity, force);
}

void NetworkArtifactActions::clearStaleSelection(const QString& sourceViewId) {
    network_view::clear_stale_network_selection(sourceViewId.toStdString());
}

}
