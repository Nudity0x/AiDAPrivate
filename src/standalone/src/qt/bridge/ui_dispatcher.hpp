#pragma once

#include <QObject>

namespace aida::qt {

class UiDispatcher : public QObject
{
    Q_OBJECT
public:
    explicit UiDispatcher(QObject* parent = nullptr);

    void installEventHooks();

public Q_SLOTS:
    void drainWakeSlot();
    void drainAboutToBlockSlot();
};

UiDispatcher* create_ui_dispatcher(QObject* parent);
UiDispatcher* ui_dispatcher_instance();

}
