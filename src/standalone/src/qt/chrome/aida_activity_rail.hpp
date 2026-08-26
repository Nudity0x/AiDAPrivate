#pragma once

#include <QPixmap>
#include <QToolBar>

#include <vector>

class QAction;
class QActionGroup;
class QTimer;
class QToolButton;

namespace aida::qt::bridge {
class ActionBridge;
}

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::chrome {

enum class AidaRailGlyph {
    Analysis,
    Debugging,
    Memory,
    Types,
    Network,
    Programming,
    Automation,
    Explorer,
    Search,
    Recent,
    More,
    Settings
};

QPixmap railGlyphPixmap(AidaRailGlyph glyph, qreal dpr, const QColor& color, qreal logical_size);

class AidaActivityRail : public QToolBar {
    Q_OBJECT
public:
    AidaActivityRail(docking::AidaDockHost* host, bridge::ActionBridge* actions,
                     QWidget* parent = nullptr);

    void refreshActiveStates();
    void refreshVisibility();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    struct rail_entry_t {
        const char* id;
        const char* action_id;
        AidaRailGlyph glyph;
        QAction* action = nullptr;
    };

    void buildEntries();
    void applyOverflow();
    void refreshGlyphs();
    QWidget* activeButtonWidget() const;
    void syncActiveMarker();

    docking::AidaDockHost* host_ = nullptr;
    bridge::ActionBridge* actions_ = nullptr;
    QActionGroup* workspace_group_ = nullptr;
    std::vector<rail_entry_t> workspaces_;
    std::vector<rail_entry_t> utilities_;
    rail_entry_t settings_entry_{};
    QAction* more_action_ = nullptr;
    QToolButton* more_button_ = nullptr;
    QWidget* active_marker_ = nullptr;
    QTimer* visibility_timer_ = nullptr;
    bool overflow_ = false;
    bool visibility_applied_ = true;
    bool last_overflow_state_ = false;
    unsigned last_vis_mask_ = 0u;
};

}
