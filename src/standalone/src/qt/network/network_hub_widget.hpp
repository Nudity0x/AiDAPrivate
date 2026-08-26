#pragma once

#include <QWidget>

#include <functional>
#include <vector>

#include "core/network/network_view.hpp"

class QButtonGroup;
class QLabel;
class QStackedLayout;
class QTabBar;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
}

namespace aida::qt::net {

class NetworkPaneBase;

// Header row of the network shell: title, group status subtitle, the primary
// Start/Stop capture action and the secondary "Open Capture" jump, with the
// width-gated visibility rules preserved from the ImGui shell (620/680/840px).
class NetworkHeaderWidget : public QWidget {
    Q_OBJECT
public:
    explicit NetworkHeaderWidget(QWidget* parent = nullptr);

    void setSubtitle(const QString& subtitle);
    void setCaptureState(bool running, bool pending);

Q_SIGNALS:
    void primaryCaptureAction();
    void openCaptureRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void relayoutForWidth();

    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    widgets::AidaButton* primary_ = nullptr;
    widgets::AidaButton* secondary_ = nullptr;
    bool capture_running_ = false;
    bool capture_pending_ = false;
};

// Status row of the network shell: target pid, capture state, buffered packet
// count, active tool name; h=24 (status_bar token), width-gated at 620/820.
class NetworkStatusWidget : public QWidget {
    Q_OBJECT
public:
    explicit NetworkStatusWidget(QWidget* parent = nullptr);

    void refresh();
    void setToolName(const QString& name);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void relayoutForWidth();

    QLabel* target_ = nullptr;
    QLabel* capture_ = nullptr;
    QLabel* packets_ = nullptr;
    QLabel* tool_ = nullptr;
};

// NetworkHubWidget hosts the network shell: header, the 7-group chip row, the
// per-group QTabBar and the page stack. Manual QTabBar + QStackedLayout
// composition (not QTabWidget — corner-widget limits qtabwidget.cpp:581-608);
// StackOne sizes only the current page (qstackedlayout.cpp:440-455) and the
// synchronous hide/raise/show switch fires hideEvent/showEvent on panes,
// driving NetworkPaneBase visibility gating for free
// (qstackedlayout.cpp:262-320). Panes are created lazily on first selection.
class NetworkHubWidget : public QWidget {
    Q_OBJECT
public:
    explicit NetworkHubWidget(QWidget* parent = nullptr);

    NetworkPaneBase* currentPane() const noexcept { return current_pane_; }
    network_view::sub_tab_t currentTab() const noexcept { return current_tab_; }
    void setTab(network_view::sub_tab_t tab);

private:
    void activateGroup(int groupIndex);
    void activateTab(network_view::sub_tab_t tab);
    QWidget* ensurePage(network_view::sub_tab_t tab);
    void rebuildTabBar();
    void refreshHeader();

    NetworkHeaderWidget* header_ = nullptr;
    QWidget* chipRow_ = nullptr;
    QButtonGroup* chipGroup_ = nullptr;
    QTabBar* tabBar_ = nullptr;
    QWidget* stackHost_ = nullptr;
    QStackedLayout* stack_ = nullptr;
    NetworkStatusWidget* status_ = nullptr;
    QTimer* pollTimer_ = nullptr;

    std::vector<QWidget*> pages_;
    std::vector<bool> pageCreated_;
    int current_group_ = 0;
    network_view::sub_tab_t current_tab_ = network_view::sub_tab_t::connections;
    NetworkPaneBase* current_pane_ = nullptr;
    bool activating_ = false;
};

// The hub-content seam: install_network_domain registers the factory; the dock
// host's network hub calls create_network_hub_content when composing the
// aida.hub.network dock content (the one-line host seam per the wave report).
using hub_content_factory_t = std::function<QWidget*(QWidget* parent)>;
void register_network_hub_content_factory(hub_content_factory_t factory);
QWidget* create_network_hub_content(QWidget* parent);

}
