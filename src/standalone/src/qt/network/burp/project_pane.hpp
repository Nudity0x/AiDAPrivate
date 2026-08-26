#pragma once

#include <QString>

#include <atomic>

#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;

namespace aida::qt::widgets {
class AidaButton;
}

namespace aida::qt::net {

class ProjectPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit ProjectPane(QWidget* parent = nullptr);
    ~ProjectPane() override;

private:
    void startOperation(bool save);
    void applyResult(bool save, bool ok, QString detail, std::uint64_t serial);

    QLineEdit* pathEdit_ = nullptr;
    QCheckBox* replaceCheck_ = nullptr;
    widgets::AidaButton* saveButton_ = nullptr;
    widgets::AidaButton* loadButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> operationSerial_{0};
    bool succeeded_ = false;
};

}
