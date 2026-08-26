#pragma once

#include <QObject>

#include <cstdint>
#include <functional>

class QTimer;

namespace aida::qt::analysis_bridge {

struct revision_sample_t {
    const void* workspace = nullptr;
    quint64 generation = 0;
    quint64 analysis_revision = 0;
    quint64 overlay_revision = 0;
    quint64 view_revision = 0;
    quint64 ui_serial = 0;
    bool valid = false;
};

class AidaRevisionPoller : public QObject {
    Q_OBJECT
public:
    explicit AidaRevisionPoller(QObject* parent = nullptr);
    ~AidaRevisionPoller() override;

    void set_source(std::function<revision_sample_t()> source);
    void set_polling(bool polling);
    bool polling() const noexcept { return polling_; }
    void poll_now();

Q_SIGNALS:
    void revisionChanged(quint64 combined, quint64 overlayRevision);
    void uiSerialChanged(quint64 serial);
    void sourceInvalidated();

private:
    void tick();

    QTimer* timer_ = nullptr;
    std::function<revision_sample_t()> source_;
    revision_sample_t last_;
    bool has_last_ = false;
    bool polling_ = false;
};

}
