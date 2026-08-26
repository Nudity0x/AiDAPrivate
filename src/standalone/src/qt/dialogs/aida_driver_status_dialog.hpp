#pragma once

#include <QAbstractListModel>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/runtime/standalone_driver.hpp"
#include "qt/bridge/aida_dialog.hpp"

class QLabel;
class QListView;
class QPushButton;
class QTabWidget;
class QTimer;

namespace aida::qt::dialogs {

class AidaDriverModulesModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AidaDriverModulesModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void applySnapshot(std::vector<driver_bridge::module_info_t> modules, std::uint32_t pid);

private:
    std::vector<driver_bridge::module_info_t> modules_;
    std::uint32_t pid_ = 0;
};

class AidaDriverThreadsModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AidaDriverThreadsModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void applySnapshot(std::vector<driver_bridge::thread_info_t> threads, std::uint32_t pid);

private:
    std::vector<driver_bridge::thread_info_t> threads_;
    std::uint32_t pid_ = 0;
};

class AidaDriverStatusDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    explicit AidaDriverStatusDialog(QWidget* parent = nullptr);
    ~AidaDriverStatusDialog() override;

    void openFresh();

private:
    void tick();
    void onDetachReview();
    void refreshHeader();

    QLabel* status_label_ = nullptr;
    QLabel* process_label_ = nullptr;
    QLabel* pid_label_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    AidaDriverModulesModel* modules_model_ = nullptr;
    AidaDriverThreadsModel* threads_model_ = nullptr;
    QListView* modules_view_ = nullptr;
    QListView* threads_view_ = nullptr;
    QPushButton* detach_button_ = nullptr;
    QPushButton* close_button_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    bool header_attached_ = false;
    bool header_state_set_ = false;
};

}
