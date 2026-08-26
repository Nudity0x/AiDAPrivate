#pragma once

#include <QDialog>
#include <QString>

#include <functional>

#include "core/ai/standalone_chat.hpp"

class QDialogButtonBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace aida::qt::ai {

struct aida_confirm_request_t {
    QString verb;
    QString target;
    QString scope;
    QString effect;
    QString reversibility;
    QString prerequisite;
    QString confirm_label;
    bool destructive = false;
    bool confirm_enabled = true;
};

class AidaConfirmDialog : public QDialog {
    Q_OBJECT
public:
    AidaConfirmDialog(const aida_confirm_request_t& request, QWidget* parent = nullptr);

    static void request(const aida_confirm_request_t& request, QWidget* parent,
                        std::function<void()> on_confirm);

private:
    QPushButton* confirm_button_ = nullptr;
};

class AidaApplyChangeDialog : public QDialog {
    Q_OBJECT
public:
    AidaApplyChangeDialog(
        const aida::automation_ui::message_identity_t& identity, QWidget* parent = nullptr);

    static void request(const aida::automation_ui::message_identity_t& identity,
                        QWidget* parent, std::function<void(const QString& detail)> feedback);

private:
    void onConfirm();
    void populate();

    aida::automation_ui::message_identity_t identity_;
    std::function<void(const QString&)> feedback_;
    QPushButton* confirm_button_ = nullptr;
    QPlainTextEdit* before_ = nullptr;
    QPlainTextEdit* after_ = nullptr;
    QLabel* prerequisite_ = nullptr;
    bool reverse_linked_ = false;
};

}
