#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QString>

#include <cstdint>

#include "core/network/network_view.hpp"

class QLabel;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
}

namespace aida::qt::net {

// ReviewedContextBanner is shared by the Match/Replace and Session Handler
// panes (identical ImGui blocks at match_replace_view.cpp:346-397 and
// session_handler_view.cpp:214-265). It shows the staged reviewed artifact
// with a CURRENT/STALE status label, Recheck and Clear buttons, the identity
// line (scheme://host:port | rev N | hash HEX | N bytes) and the stale reason.
// Revalidation cadence: the ImGui 120-frame throttle (about 2 s at 60 fps)
// becomes a QElapsedTimer >= 2000 ms gate evaluated from a 500 ms
// visibility-gated QTimer; QElapsedTimer is monotonic
// (qelapsedtimer.cpp:174-199). resolve_artifact is a synchronous registry
// resolve called from the GUI timer slot, the same cost as the legacy
// render-path call.
class ReviewedContextBanner : public QFrame {
    Q_OBJECT
public:
    explicit ReviewedContextBanner(QWidget* parent = nullptr);

    void setCurrentText(const QString& text) { current_text_ = text; }
    void setStaleText(const QString& text) { stale_text_ = text; }
    void setIdentitySuffix(const QString& suffix) { identity_suffix_ = suffix; }

    void setContext(const network_view::artifact_identity_t& identity);
    void clearContext();
    bool hasContext() const noexcept { return context_.valid(); }
    bool current() const noexcept { return current_; }
    QString staleReason() const { return reason_; }

    void revalidateNow();

Q_SIGNALS:
    void cleared();
    void revalidated(bool current);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void revalidate(bool force);
    void refreshLabels();

    network_view::artifact_identity_t context_;
    bool current_ = false;
    QString reason_;
    QString current_text_ = QStringLiteral("CURRENT AT LAST CHECK");
    QString stale_text_ = QStringLiteral("STALE");
    QString identity_suffix_;
    QElapsedTimer validation_clock_;
    QTimer* timer_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* label_text_ = nullptr;
    QLabel* identity_label_ = nullptr;
    QLabel* reason_label_ = nullptr;
    widgets::AidaButton* recheck_button_ = nullptr;
    widgets::AidaButton* clear_button_ = nullptr;
};

}
