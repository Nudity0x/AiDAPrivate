#pragma once

#include <QString>

#include "qt/network/network_pane_base.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaPill;
}

namespace aida::qt::net {

class PcapExportPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit PcapExportPane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void refreshState();
    void refreshProgress();

    QLineEdit* pathEdit_ = nullptr;
    widgets::AidaButton* browseButton_ = nullptr;
    QSpinBox* filterPid_ = nullptr;
    QComboBox* filterProtocol_ = nullptr;
    QLabel* availableLabel_ = nullptr;
    widgets::AidaButton* exportButton_ = nullptr;
    widgets::AidaPill* writingPill_ = nullptr;
    widgets::AidaPill* errorPill_ = nullptr;
    widgets::AidaPill* donePill_ = nullptr;
    QLabel* proxyCountLabel_ = nullptr;
    widgets::AidaButton* harButton_ = nullptr;
    widgets::AidaPill* harErrorPill_ = nullptr;
    widgets::AidaPill* harDonePill_ = nullptr;
    QTimer* progressTimer_ = nullptr;
    bool refreshing_ = false;
};

}
