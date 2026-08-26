#pragma once

#include <QString>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/network/burp/scanner_module.hpp"
#include "qt/bridge/aida_dialog.hpp"
#include "qt/widgets/aida_form_state.hpp"

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QVariant>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableView;

namespace aida::qt::net {

class QtHumanRequestEditor;
class QtScannerController;

class QtScannerModuleModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit QtScannerModuleModel(QObject* parent = nullptr);

    void adopt(std::vector<aida::burp::scanner::module_t> modules);
    std::size_t enabledCount() const noexcept;
    std::vector<std::string> enabledIds() const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

private:
    std::vector<aida::burp::scanner::module_t> modules_;
    std::set<std::string> disabled_;
};

// QtNewAuditDialog replaces render_new_audit_dialog: a modeless
// window-modal-less QDialog shown with open() (never exec() — no nested
// QEventLoop, qdialog.cpp:458-526,539-575), owning the embedded
// QtHumanRequestEditor (maxBytes 65535, the fixed-buffer replacement) and the
// verbatim validate_new_audit rule set. The accept button enable rule reads
// the editor's validityChanged/hasUnappliedPrettyChanged signals live.
class QtNewAuditDialog : public aida::qt::bridge::AidaDialog {
    Q_OBJECT
public:
    QtNewAuditDialog(QtScannerController* controller, QWidget* parent = nullptr);

    void openStaged(const QString& url, const QString& rawRequest, std::uint64_t generation);
    void setPending(bool pending);
    std::uint64_t generation() const noexcept { return generation_; }

Q_SIGNALS:
    void auditSubmitted(bool submitted);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void validateForm();
    void updateAcceptState();
    void onAccept();

    QtScannerController* controller_ = nullptr;
    std::uint64_t generation_ = 0;
    bool pending_ = false;

    QLineEdit* urlEdit_ = nullptr;
    QtHumanRequestEditor* requestEditor_ = nullptr;
    QCheckBox* scopeOnlyCheck_ = nullptr;
    QCheckBox* followRedirectsCheck_ = nullptr;
    QSpinBox* timeoutSpin_ = nullptr;
    QSpinBox* concurrencySpin_ = nullptr;
    QSpinBox* throttleSpin_ = nullptr;
    QSpinBox* moduleCapSpin_ = nullptr;
    QtScannerModuleModel* moduleModel_ = nullptr;
    QTableView* modulesView_ = nullptr;
    QLabel* urlError_ = nullptr;
    QLabel* requestError_ = nullptr;
    QLabel* timeoutError_ = nullptr;
    QLabel* concurrencyError_ = nullptr;
    QLabel* throttleError_ = nullptr;
    QLabel* moduleCapError_ = nullptr;
    QLabel* modulesError_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* noticeLabel_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    aida::qt::widgets::form_state_t form_;
};

}
