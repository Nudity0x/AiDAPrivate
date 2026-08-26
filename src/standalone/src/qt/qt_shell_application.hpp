#pragma once

#include <QApplication>

namespace aida::qt {

void qt_message_handler(QtMsgType type, const QMessageLogContext& context, const QString& msg);
void install_qt_message_handler();

class AidaApplication : public QApplication
{
    Q_OBJECT
public:
    AidaApplication(int& argc, char** argv);

    bool notify(QObject* receiver, QEvent* event) override;
};

}
