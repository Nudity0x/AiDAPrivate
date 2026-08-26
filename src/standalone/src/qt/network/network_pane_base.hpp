#pragma once

#include <QWidget>

class QStackedLayout;
class QTimer;

namespace aida::qt::net {

// NetworkPaneBase is the shared pane plumbing. Visibility gating replaces the
// ImGui last_render_tick_ms/rendered_recently mechanism: showEvent/hideEvent
// (qwidget.h:697-698) drive onPaneShown/onPaneHidden, which start/stop the
// pane's timers and arm its snapshot requesters. Panes that need a driver
// target get the placeholder page ("No target attached ...") while
// analysis_session::has_active_target() is false.
class NetworkPaneBase : public QWidget {
    Q_OBJECT
public:
    explicit NetworkPaneBase(QWidget* parent = nullptr);
    ~NetworkPaneBase() override;

    void setRequiresTarget(bool requires);
    bool requiresTarget() const noexcept { return requires_target_; }
    bool targetAvailable() const noexcept { return target_available_; }

    void setContent(QWidget* content);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

    virtual void onPaneShown();
    virtual void onPaneHidden();

    bool paneVisible() const noexcept { return pane_visible_; }

private:
    void refreshTargetState();

    QStackedLayout* stack_ = nullptr;
    QWidget* content_ = nullptr;
    QWidget* placeholder_ = nullptr;
    QTimer* target_timer_ = nullptr;
    bool requires_target_ = false;
    bool target_available_ = false;
    bool pane_visible_ = false;
};

}
