#pragma once

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QModelIndex>
#include <QStringListModel>
#include <QVariant>

#include <memory>
#include <vector>

#include "core/network/burp/cookie_jar.hpp"
#include "qt/network/burp/cookie_jar_bridge.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/bridge/aida_dialog.hpp"

class QCheckBox;
class QComboBox;
class QFrame;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListView;
class QSplitter;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class CookieModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name = 0, Value, Secure, HttpOnly, SameSite, Expires, ColumnCount };

    explicit CookieModel(QObject* parent = nullptr);

    void adopt(std::shared_ptr<const std::vector<aida::burp::cookie_jar::parsed_cookie_t>> cookies,
               std::uint64_t generation);
    const aida::burp::cookie_jar::parsed_cookie_t* rowAt(int row) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QVariant cellData(const aida::burp::cookie_jar::parsed_cookie_t& row, int column,
                      int role) const;

    std::shared_ptr<const std::vector<aida::burp::cookie_jar::parsed_cookie_t>> cookies_;
    std::uint64_t generation_ = 0;
};

class CookieEditDialog : public aida::qt::bridge::AidaDialog {
    Q_OBJECT
public:
    CookieEditDialog(const QString& host, const aida::burp::cookie_jar::parsed_cookie_t& cookie,
                     QWidget* parent = nullptr);

private:
    QLineEdit* hostEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* valueEdit_ = nullptr;
    QLineEdit* domainEdit_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    QLineEdit* expiresEdit_ = nullptr;
    QCheckBox* secureCheck_ = nullptr;
    QCheckBox* httpOnlyCheck_ = nullptr;
    QComboBox* sameSiteCombo_ = nullptr;
};

class CookieJarPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit CookieJarPane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void refreshFromStore();
    void refreshBanner();
    void editSelected();
    void deleteSelected();
    void updateEmptyState();
    void showEditFor(const QString& host, const aida::burp::cookie_jar::parsed_cookie_t& cookie);

    QLineEdit* filterEdit_ = nullptr;
    QStringListModel* hostModel_ = nullptr;
    QListView* hostList_ = nullptr;
    CookieModel* cookieModel_ = nullptr;
    QTableView* cookieTable_ = nullptr;
    QStackedLayout* cookieStack_ = nullptr;
    widgets::AidaStateView* cookieEmptyView_ = nullptr;
    QSplitter* splitter_ = nullptr;
    widgets::AidaButton* editButton_ = nullptr;
    widgets::AidaButton* deleteButton_ = nullptr;
    widgets::AidaButton* clearAllButton_ = nullptr;
    QFrame* banner_ = nullptr;
    QLabel* bannerStatus_ = nullptr;
    QLabel* bannerLabel_ = nullptr;
    QLabel* bannerReason_ = nullptr;
    widgets::AidaButton* bannerRecheck_ = nullptr;
    widgets::AidaButton* bannerClear_ = nullptr;
    QTimer* bannerTimer_ = nullptr;
    QElapsedTimer bannerClock_;
    std::uint64_t generation_ = 0;
    QString currentHost_;
};

}
