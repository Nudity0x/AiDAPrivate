#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include "qt/bridge/aida_dialog.hpp"

class QPlainTextEdit;
class QPushButton;

namespace aida::qt::dialogs {

class AidaChatSelectDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    explicit AidaChatSelectDialog(QWidget* parent = nullptr);

    void showText(const QString& text);

private:
    QPlainTextEdit* edit_ = nullptr;
    QPushButton* copy_button_ = nullptr;
    QPushButton* close_button_ = nullptr;
};

class AidaChatSelectController : public QObject {
    Q_OBJECT
public:
    explicit AidaChatSelectController(QObject* parent = nullptr);

    void install(QWidget* dialog_parent);
    void showText(const QString& text);

private:
    QWidget* dialog_parent_ = nullptr;
    QPointer<AidaChatSelectDialog> dialog_;
};

AidaChatSelectController& chatSelectController();

}
