#pragma once

#include <QObject>
#include <QString>

#include <string>

#include "core/network/network_view.hpp"

namespace aida::qt::net {

// Qt-facing wrapper over the byte-verbatim network_view artifact service.
// Widgets never call into the global backend directly; every slot mirrors one
// backend entry point and reports the identical unavailable-reason strings.
class NetworkArtifactActions : public QObject {
    Q_OBJECT
public:
    explicit NetworkArtifactActions(QObject* parent = nullptr);

    Q_INVOKABLE bool resolve(const network_view::artifact_identity_t& identity,
                             network_view::artifact_snapshot_t& snapshot,
                             QString& unavailableReason);
    Q_INVOKABLE bool sendToRepeater(const network_view::artifact_identity_t& identity,
                                    QString& unavailableReason);
    Q_INVOKABLE bool sendToComparer(const network_view::artifact_identity_t& identity,
                                    QString& unavailableReason);
    Q_INVOKABLE bool addToChat(const network_view::artifact_identity_t& identity,
                               QString& unavailableReason);
    Q_INVOKABLE bool assignToAgent(const network_view::artifact_identity_t& identity,
                                   QString& unavailableReason);
    Q_INVOKABLE bool makeSitemapArtifact(std::uint64_t exchangeId,
                                         network_view::artifact_kind_t kind,
                                         network_view::artifact_identity_t& identity,
                                         QString& unavailableReason);
    Q_INVOKABLE bool executeToolbarAction(const QString& actionId,
                                          network_view::artifact_identity_t primary,
                                          network_view::artifact_identity_t related,
                                          QString& unavailableReason);
    Q_INVOKABLE void publishSelection(const network_view::artifact_identity_t& identity,
                                      bool force);
    Q_INVOKABLE void clearStaleSelection(const QString& sourceViewId);
};

}
