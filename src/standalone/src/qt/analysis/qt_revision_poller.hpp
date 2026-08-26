#pragma once

#include <QObject>
#include <QTimer>

#include <cstdint>
#include <memory>

namespace aida::analysis {
class analysis_workspace_t;
}

namespace aida::qt::analysis {

class QtWorkspaceContext;

// GUI-thread QTimer polling of the four workspace revision counters is the
// correctness channel for model invalidation (07 sec. 1.2). The workspace baseline
// publish observer is wired only as a latency accelerator (one queued pollNow).
class QtRevisionPoller : public QObject {
    Q_OBJECT
public:
    struct revision_tuple_t {
        quint64 generation = 0;
        quint64 analysis = 0;
        quint64 overlay = 0;
        quint64 symbol = 0;

        friend bool operator==(const revision_tuple_t& lhs,
                               const revision_tuple_t& rhs) noexcept {
            return lhs.generation == rhs.generation && lhs.analysis == rhs.analysis &&
                lhs.overlay == rhs.overlay && lhs.symbol == rhs.symbol;
        }
        friend bool operator!=(const revision_tuple_t& lhs,
                               const revision_tuple_t& rhs) noexcept {
            return !(lhs == rhs);
        }
    };

    explicit QtRevisionPoller(QtWorkspaceContext* parent);

    void arm();
    void disarm();
    bool armed() const noexcept { return timer_.isActive(); }

    revision_tuple_t last() const noexcept { return last_; }

Q_SIGNALS:
    void revisionsChanged(quint64 generation, quint64 analysisRevision,
                          quint64 overlayRevision, quint64 symbolRevision);
    void workspaceClosed();

public Q_SLOTS:
    void pollNow();

private:
    QTimer timer_;
    revision_tuple_t last_{};
    bool close_emitted_ = false;
};

}
