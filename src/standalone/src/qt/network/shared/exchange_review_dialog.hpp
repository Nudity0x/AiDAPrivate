#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include "core/network/network_view.hpp"
#include "qt/bridge/aida_dialog.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QWidget;

namespace aida::qt::widgets {
class AidaButton;
}

namespace aida::qt::net {

// ExchangeReviewDialog ports render_exchange_review_dialogs: one modeless
// window-modal dialog per review kind, shown with open() (never exec() — no
// nested QEventLoop, qdialog.cpp:549,573-575). Accept re-resolves the retained
// identity (AidaDialog::RevalidateScope auto-rejects when it goes stale); the
// submit calls route into the byte-verbatim backend entry points.
class ExchangeReviewDialog : public aida::qt::bridge::AidaDialog {
    Q_OBJECT
public:
    ExchangeReviewDialog(const network_view::exchange_review_presented_t& presented,
                         QWidget* parent = nullptr);

private:
    void buildIssue();
    void buildReplay();
    void buildRemove();
    void onConfirm();

    network_view::exchange_review_presented_t presented_;

    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    QPlainTextEdit* remediationEdit_ = nullptr;
    QComboBox* severityCombo_ = nullptr;
    QComboBox* confidenceCombo_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QPushButton* confirmButton_ = nullptr;
};

// ExchangeRemoveReceiptDialog ports the removal receipt: Undo Removal +
// Close, with the backend completion state re-presented through the display
// hook (operation_pending/restored/error refresh in place).
class ExchangeRemoveReceiptDialog : public aida::qt::bridge::AidaDialog {
    Q_OBJECT
public:
    ExchangeRemoveReceiptDialog(const network_view::exchange_remove_receipt_t& receipt,
                                QWidget* parent = nullptr);

    void refresh(const network_view::exchange_remove_receipt_t& receipt);

private:
    void rebuildUi();

    network_view::exchange_remove_receipt_t receipt_;
    QLabel* titleLabel_ = nullptr;
    QLabel* bodyLabel_ = nullptr;
    QLabel* pendingLabel_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    widgets::AidaButton* undoButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
};

// ExchangeReviewHost owns the live dialogs and the display hooks. Created by
// install_network_domain on the GUI thread with domain lifetime.
class ExchangeReviewHost : public QObject {
    Q_OBJECT
public:
    explicit ExchangeReviewHost(QObject* parent = nullptr);

    void installHooks();

private:
    void presentReview(const network_view::exchange_review_presented_t& presented);
    void presentReceipt(const network_view::exchange_remove_receipt_t& receipt);

    QPointer<ExchangeReviewDialog> reviewDialog_;
    QPointer<ExchangeRemoveReceiptDialog> receiptDialog_;
};

}
