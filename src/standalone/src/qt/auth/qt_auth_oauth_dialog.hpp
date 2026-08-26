#pragma once

#include <QDialog>

#include <QString>

#include <string>

class QFormLayout;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedLayout;
class QTimer;

namespace aida::qt::auth {

class AidaOAuthLoginDialog : public QDialog {
    Q_OBJECT
public:
    enum class Provider : int { Codex, ClaudeCode, Copilot };

    explicit AidaOAuthLoginDialog(Provider provider, QWidget* parent = nullptr);
    ~AidaOAuthLoginDialog() override;

    static void showFor(Provider provider, QWidget* parent);

    void reject() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void pollFlow();
    void refreshPhase();
    void onConfirm();

    Provider provider_;
    QLabel* phase_label_ = nullptr;
    QProgressBar* phase_bar_ = nullptr;
    QLabel* provider_value_ = nullptr;
    QLabel* flow_value_ = nullptr;
    QLabel* third_value_ = nullptr;
    QLabel* third_label_ = nullptr;
    QStackedLayout* copilot_stack_ = nullptr;
    QLineEdit* ghe_edit_ = nullptr;
    QLabel* ghe_error_ = nullptr;
    QLabel* device_code_label_ = nullptr;
    QPushButton* copy_code_button_ = nullptr;
    QPushButton* confirm_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QTimer* poll_timer_ = nullptr;
    QString auth_url_;
    QString verification_uri_;
    bool complete_ = false;
};

}
