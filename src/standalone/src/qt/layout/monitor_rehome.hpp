#pragma once

#include <QObject>
#include <QPointer>
#include <QRect>

#include <vector>

class QMainWindow;
class QScreen;
class QWindow;
class QTimer;

namespace ads {
class CDockManager;
class CFloatingDockContainer;
}

namespace aida::qt::layout {

class MonitorRehomeController : public QObject {
    Q_OBJECT
public:
    MonitorRehomeController(QMainWindow* window, ads::CDockManager* manager,
                            QObject* parent = nullptr);
    ~MonitorRehomeController() override;

    void rehome_now();

Q_SIGNALS:
    void rehomed();

private:
    void attach_screen_signals();
    void attach_window_signals();
    void attach_floating_signals(ads::CFloatingDockContainer* floating);
    void schedule();
    void sweep();

    QMainWindow* window_ = nullptr;
    ads::CDockManager* manager_ = nullptr;
    QTimer* debounce_ = nullptr;
    std::vector<QPointer<QScreen>> screens_;
    QPointer<QWindow> window_handle_;
    std::vector<QPointer<ads::CFloatingDockContainer>> floating_attached_;
};

}
