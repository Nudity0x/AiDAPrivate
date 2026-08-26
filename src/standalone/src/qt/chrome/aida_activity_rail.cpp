#include "qt/chrome/aida_activity_rail.hpp"

#include <QAction>
#include <QActionGroup>
#include <QLayout>
#include <QMenu>
#include <QPainter>
#include <QIcon>
#include <QResizeEvent>
#include <QTimer>
#include <QToolButton>

#include <cmath>
#include <cstring>

#include "core/settings/standalone_settings.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/action_bridge.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/layout/workspace_persistence.hpp"
#include "qt/theme/aida_theme_controller.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::chrome {

namespace {

using widgets::with_alpha;

class RailActiveMarker : public QWidget {
public:
    explicit RailActiveMarker(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFixedWidth(theme::tokens().radius.xs);
    }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setPen(Qt::NoPen);
        p.setBrush(theme::tokens().accent);
        const qreal radius = theme::tokens().radius.xs * 0.5;
        p.drawRoundedRect(rect(), radius, radius);
    }
};

void glyph_stroke(QPainter& p, const QPointF& a, const QPointF& b, const QColor& col, qreal th)
{
    p.setPen(QPen(col, th, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(a, b);
}

void paint_rail_glyph(QPainter& p, AidaRailGlyph glyph, const QRectF& area, const QColor& color)
{
    const QPointF center = area.center();
    const qreal unit = area.width() / 24.0;
    const qreal stroke = (std::max)(1.25, 1.5 * unit);
    switch (glyph) {
    case AidaRailGlyph::Analysis:
        p.setPen(QPen(color, stroke));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, 7.0 * unit, 7.0 * unit);
        glyph_stroke(p, QPointF(center.x() + 5.0 * unit, center.y() + 5.0 * unit),
                     QPointF(center.x() + 10.0 * unit, center.y() + 10.0 * unit), color, stroke);
        glyph_stroke(p, QPointF(center.x() - 3.5 * unit, center.y()),
                     QPointF(center.x() + 3.5 * unit, center.y()), color, stroke);
        glyph_stroke(p, QPointF(center.x(), center.y() - 3.5 * unit),
                     QPointF(center.x(), center.y() + 3.5 * unit), color, stroke);
        break;
    case AidaRailGlyph::Debugging:
        p.setPen(QPen(color, stroke));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(center.x() - 6.0 * unit, center.y() - 6.0 * unit,
                                 12.0 * unit, 13.0 * unit), 3.0 * unit, 3.0 * unit);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(center.x() - 2.3 * unit, center.y() - 1.5 * unit),
                      1.1 * unit, 1.1 * unit);
        p.drawEllipse(QPointF(center.x() + 2.3 * unit, center.y() - 1.5 * unit),
                      1.1 * unit, 1.1 * unit);
        for (qreal offset : {-4.0, 0.0, 4.0}) {
            glyph_stroke(p, QPointF(center.x() - 9.0 * unit, center.y() + offset * unit),
                         QPointF(center.x() - 6.0 * unit, center.y() + offset * unit), color, stroke);
            glyph_stroke(p, QPointF(center.x() + 6.0 * unit, center.y() + offset * unit),
                         QPointF(center.x() + 9.0 * unit, center.y() + offset * unit), color, stroke);
        }
        break;
    case AidaRailGlyph::Memory:
        p.setPen(QPen(color, stroke));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(center.x() - 7.0 * unit, center.y() - 6.0 * unit,
                                 14.0 * unit, 12.0 * unit), 2.0 * unit, 2.0 * unit);
        for (qreal offset : {-4.0, 0.0, 4.0}) {
            glyph_stroke(p, QPointF(center.x() + offset * unit, center.y() - 9.0 * unit),
                         QPointF(center.x() + offset * unit, center.y() - 6.0 * unit), color, stroke);
            glyph_stroke(p, QPointF(center.x() + offset * unit, center.y() + 6.0 * unit),
                         QPointF(center.x() + offset * unit, center.y() + 9.0 * unit), color, stroke);
        }
        glyph_stroke(p, QPointF(center.x() - 3.5 * unit, center.y()),
                     QPointF(center.x() + 3.5 * unit, center.y()), color, stroke);
        break;
    case AidaRailGlyph::Types:
        p.setPen(QPen(color, stroke));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(center.x() - 8.0 * unit, center.y() - 8.0 * unit,
                                 16.0 * unit, 16.0 * unit), 2.0 * unit, 2.0 * unit);
        glyph_stroke(p, QPointF(center.x() - 2.0 * unit, center.y() - 8.0 * unit),
                     QPointF(center.x() - 2.0 * unit, center.y() + 8.0 * unit), color, stroke);
        glyph_stroke(p, QPointF(center.x() - 2.0 * unit, center.y() - 2.0 * unit),
                     QPointF(center.x() + 8.0 * unit, center.y() - 2.0 * unit), color, stroke);
        break;
    case AidaRailGlyph::Network:
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(center.x(), center.y() - 7.0 * unit), 2.2 * unit, 2.2 * unit);
        p.drawEllipse(QPointF(center.x() - 8.0 * unit, center.y() + 6.0 * unit), 2.2 * unit, 2.2 * unit);
        p.drawEllipse(QPointF(center.x() + 8.0 * unit, center.y() + 6.0 * unit), 2.2 * unit, 2.2 * unit);
        glyph_stroke(p, QPointF(center.x() - 1.2 * unit, center.y() - 5.0 * unit),
                     QPointF(center.x() - 6.8 * unit, center.y() + 4.0 * unit), color, stroke);
        glyph_stroke(p, QPointF(center.x() + 1.2 * unit, center.y() - 5.0 * unit),
                     QPointF(center.x() + 6.8 * unit, center.y() + 4.0 * unit), color, stroke);
        glyph_stroke(p, QPointF(center.x() - 5.5 * unit, center.y() + 6.0 * unit),
                     QPointF(center.x() + 5.5 * unit, center.y() + 6.0 * unit), color, stroke);
        break;
    case AidaRailGlyph::Programming:
        glyph_stroke(p, QPointF(center.x() - 2.5 * unit, center.y() - 7.0 * unit),
                     QPointF(center.x() - 8.0 * unit, center.y()), color, stroke);
        glyph_stroke(p, QPointF(center.x() - 8.0 * unit, center.y()),
                     QPointF(center.x() - 2.5 * unit, center.y() + 7.0 * unit), color, stroke);
        glyph_stroke(p, QPointF(center.x() + 2.5 * unit, center.y() - 7.0 * unit),
                     QPointF(center.x() + 8.0 * unit, center.y()), color, stroke);
        glyph_stroke(p, QPointF(center.x() + 8.0 * unit, center.y()),
                     QPointF(center.x() + 2.5 * unit, center.y() + 7.0 * unit), color, stroke);
        break;
    case AidaRailGlyph::Automation:
        p.setPen(QPen(color, stroke));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, 7.0 * unit, 7.0 * unit);
        for (int index = 0; index < 8; ++index) {
            const qreal x = index == 0 || index == 4 ? 0.0 : (index < 4 ? 1.0 : -1.0);
            const qreal y = index == 2 || index == 6 ? 0.0 : (index < 2 || index > 6 ? 1.0 : -1.0);
            glyph_stroke(p, QPointF(center.x() + x * 7.5 * unit, center.y() + y * 7.5 * unit),
                         QPointF(center.x() + x * 10.0 * unit, center.y() + y * 10.0 * unit),
                         color, stroke);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(center, 2.5 * unit, 2.5 * unit);
        break;
    case AidaRailGlyph::Explorer:
        p.setPen(QPen(color, stroke));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(center.x() - 8.0 * unit, center.y() - 5.0 * unit,
                                 16.0 * unit, 12.0 * unit), 2.0 * unit, 2.0 * unit);
        glyph_stroke(p, QPointF(center.x() - 7.0 * unit, center.y() - 5.0 * unit),
                     QPointF(center.x() - 3.0 * unit, center.y() - 9.0 * unit), color, stroke);
        glyph_stroke(p, QPointF(center.x() - 3.0 * unit, center.y() - 9.0 * unit),
                     QPointF(center.x() + 2.0 * unit, center.y() - 9.0 * unit), color, stroke);
        glyph_stroke(p, QPointF(center.x() + 2.0 * unit, center.y() - 9.0 * unit),
                     QPointF(center.x() + 4.0 * unit, center.y() - 5.0 * unit), color, stroke);
        break;
    case AidaRailGlyph::Search:
        p.setPen(QPen(color, stroke));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(center.x() - 2.0 * unit, center.y() - 2.0 * unit),
                      6.0 * unit, 6.0 * unit);
        glyph_stroke(p, QPointF(center.x() + 2.5 * unit, center.y() + 2.5 * unit),
                     QPointF(center.x() + 9.0 * unit, center.y() + 9.0 * unit), color, stroke);
        break;
    case AidaRailGlyph::Recent:
        p.setPen(QPen(color, stroke));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, 8.0 * unit, 8.0 * unit);
        glyph_stroke(p, center, QPointF(center.x(), center.y() - 5.0 * unit), color, stroke);
        glyph_stroke(p, center, QPointF(center.x() + 4.5 * unit, center.y() + 2.5 * unit), color, stroke);
        break;
    case AidaRailGlyph::More:
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        for (qreal offset : {-6.0, 0.0, 6.0})
            p.drawEllipse(QPointF(center.x() + offset * unit, center.y()), 1.6 * unit, 1.6 * unit);
        break;
    case AidaRailGlyph::Settings:
        p.setPen(QPen(color, stroke));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, 4.0 * unit, 4.0 * unit);
        p.drawEllipse(center, 8.0 * unit, 8.0 * unit);
        for (qreal offset : {-7.0, 7.0}) {
            glyph_stroke(p, QPointF(center.x() + offset * unit, center.y() - 3.0 * unit),
                         QPointF(center.x() + offset * unit, center.y() + 3.0 * unit), color, stroke);
            glyph_stroke(p, QPointF(center.x() - 3.0 * unit, center.y() + offset * unit),
                         QPointF(center.x() + 3.0 * unit, center.y() + offset * unit), color, stroke);
        }
        break;
    }
}

}

QPixmap railGlyphPixmap(AidaRailGlyph glyph, qreal dpr, const QColor& color, qreal logical_size)
{
    const qreal scale = dpr > 0.0 ? dpr : 1.0;
    const int px = (std::max)(1, static_cast<int>(logical_size * scale + 0.5));
    QPixmap pixmap(px, px);
    pixmap.fill(Qt::transparent);
    {
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        paint_rail_glyph(p, glyph, QRectF(0, 0, px, px), color);
    }
    pixmap.setDevicePixelRatio(scale);
    return pixmap;
}

AidaActivityRail::AidaActivityRail(docking::AidaDockHost* host, bridge::ActionBridge* actions,
                                   QWidget* parent)
    : QToolBar(QStringLiteral("Activity"), parent), host_(host), actions_(actions)
{
    setObjectName(QStringLiteral("aida.activity_rail"));
    setOrientation(Qt::Vertical);
    setMovable(false);
    setFloatable(false);
    setToolButtonStyle(Qt::ToolButtonIconOnly);

    active_marker_ = new RailActiveMarker(this);
    active_marker_->setObjectName(QStringLiteral("aida.activity_rail.active_marker"));
    active_marker_->hide();

    workspace_group_ = new QActionGroup(this);
    workspace_group_->setExclusive(true);

    diag::log_tagged_critical("activity_rail", "ctor_buildEntries_pre");
    buildEntries();
    diag::log_tagged_critical("activity_rail", "ctor_refreshGlyphs_pre");
    refreshGlyphs();
    diag::log_tagged_critical("activity_rail", "ctor_refreshActiveStates_pre");
    refreshActiveStates();
    diag::log_tagged_critical("activity_rail", "ctor_refreshVisibility_pre");
    refreshVisibility();
    diag::log_tagged_critical("activity_rail", "ctor_refreshes_done");

    visibility_timer_ = new QTimer(this);
    visibility_timer_->setInterval(500);
    visibility_timer_->setTimerType(Qt::CoarseTimer);
    connect(visibility_timer_, &QTimer::timeout, this, [this] { refreshVisibility(); });
    visibility_timer_->start();

    if (actions_)
        connect(actions_, &bridge::ActionBridge::actions_rebuilt, this, [this] {
            refreshGlyphs();
            applyOverflow();
            refreshActiveStates();
        });
    if (host_ && host_->persistence()) {
        connect(host_->persistence(), &layout::WorkspacePersistenceController::activeWorkspaceChanged,
                this, [this] {
            applyOverflow();
            refreshActiveStates();
        });
    }
    if (host_ && host_->registry()) {
        connect(host_->registry(), &registry::qt_view_registry_t::focusedInstanceChanged,
                this, [this] { refreshActiveStates(); });
    }
    connect(&theme::AidaThemeController::instance(),
            &theme::AidaThemeController::themeGenerationChanged,
            this, [this](quint64) {
        refreshGlyphs();
        applyOverflow();
        syncActiveMarker();
    });
}

void AidaActivityRail::buildEntries()
{
    workspaces_ = {
        {"workspace.analysis", "workspace.switch.analysis", AidaRailGlyph::Analysis, nullptr},
        {"workspace.debugging", "workspace.switch.debugging", AidaRailGlyph::Debugging, nullptr},
        {"workspace.memory", "workspace.switch.memory", AidaRailGlyph::Memory, nullptr},
        {"workspace.types", "workspace.switch.types-structures", AidaRailGlyph::Types, nullptr},
        {"workspace.network", "workspace.switch.network", AidaRailGlyph::Network, nullptr},
        {"workspace.programming", "workspace.switch.programming", AidaRailGlyph::Programming, nullptr},
        {"workspace.automation", "workspace.switch.automation-ai", AidaRailGlyph::Automation, nullptr},
    };
    utilities_ = {
        {"utility.explorer", "view.focus.view.project_explorer", AidaRailGlyph::Explorer, nullptr},
        {"utility.search", "view.focus.view.workspace_search", AidaRailGlyph::Search, nullptr},
        {"utility.recent", "view.focus.view.recent", AidaRailGlyph::Recent, nullptr},
    };
    settings_entry_ = {"utility.settings", "view.focus.view.settings", AidaRailGlyph::Settings, nullptr};

    for (auto& entry : workspaces_) {
        QAction* action = actions_
            ? actions_->surface_action(QString::fromLatin1(entry.action_id),
                                       aida::ui::action_invocation_source_t::activity_bar, this)
            : nullptr;
        entry.action = action;
        if (!action)
            continue;
        action->setCheckable(true);
        workspace_group_->addAction(action);
        addAction(action);
    }
    addSeparator();
    for (auto& entry : utilities_) {
        QAction* action = actions_
            ? actions_->surface_action(QString::fromLatin1(entry.action_id),
                                       aida::ui::action_invocation_source_t::activity_bar, this)
            : nullptr;
        entry.action = action;
        if (!action)
            continue;
        action->setCheckable(true);
        addAction(action);
    }

    more_action_ = new QAction(QStringLiteral("More"), this);
    more_action_->setObjectName(QStringLiteral("aida.activity_rail.more"));
    more_action_->setToolTip(QStringLiteral("More workspaces and tools"));
    more_action_->setCheckable(true);
    addAction(more_action_);
    more_button_ = qobject_cast<QToolButton*>(widgetForAction(more_action_));
    if (more_button_) {
        more_button_->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(more_button_);
        menu->setObjectName(QStringLiteral("aida.activity_rail.more.menu"));
        more_button_->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this, menu] {
            menu->clear();
            for (const auto& entry : workspaces_) {
                if (!entry.action || entry.action->isVisible())
                    continue;
                menu->addAction(entry.action);
            }
            bool any_hidden_workspace = false;
            for (const auto& entry : workspaces_)
                if (entry.action && !entry.action->isVisible())
                    any_hidden_workspace = true;
            if (any_hidden_workspace)
                menu->addSeparator();
            for (const auto& entry : utilities_) {
                if (entry.action)
                    menu->addAction(entry.action);
            }
        });
    }

    auto* bottom_spacer = new QWidget(this);
    bottom_spacer->setObjectName(QStringLiteral("aida.activity_rail.bottom_spacer"));
    bottom_spacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    bottom_spacer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    addWidget(bottom_spacer);

    addSeparator();
    {
        QAction* action = actions_
            ? actions_->surface_action(QString::fromLatin1(settings_entry_.action_id),
                                       aida::ui::action_invocation_source_t::activity_bar, this)
            : nullptr;
        settings_entry_.action = action;
        if (action) {
            action->setCheckable(true);
            addAction(action);
        }
    }
}

void AidaActivityRail::refreshGlyphs()
{
    const auto& t = theme::tokens();
    const qreal dpr = devicePixelRatioF();
    const qreal glyph_size = t.shell.activity_icon;
    auto apply = [this, &t, dpr, glyph_size](rail_entry_t& entry) {
        if (!entry.action)
            return;
        QIcon icon;
        icon.addPixmap(railGlyphPixmap(entry.glyph, dpr, t.text_secondary, glyph_size),
            QIcon::Normal, QIcon::Off);
        icon.addPixmap(railGlyphPixmap(entry.glyph, dpr, t.accent, glyph_size),
            QIcon::Normal, QIcon::On);
        icon.addPixmap(railGlyphPixmap(entry.glyph, dpr, t.text_dim, glyph_size),
            QIcon::Disabled, QIcon::Off);
        entry.action->setIcon(icon);
    };
    for (auto& entry : workspaces_)
        apply(entry);
    for (auto& entry : utilities_)
        apply(entry);
    apply(settings_entry_);
    if (more_action_) {
        QIcon more_icon;
        more_icon.addPixmap(railGlyphPixmap(AidaRailGlyph::More, dpr, t.text_secondary, glyph_size),
            QIcon::Normal, QIcon::Off);
        more_icon.addPixmap(railGlyphPixmap(AidaRailGlyph::More, dpr, t.accent, glyph_size),
            QIcon::Normal, QIcon::On);
        more_action_->setIcon(more_icon);
    }
}

void AidaActivityRail::refreshActiveStates()
{
    const auto* persistence = host_ ? host_->persistence() : nullptr;
    const auto* registry = host_ ? host_->registry() : nullptr;
    const auto focused = registry ? registry->focused_instance() : std::nullopt;
    const std::string focused_view = focused ? focused->view.value() : std::string();
    const bool utility_focused = focused_view == "view.project_explorer" ||
        focused_view == "view.workspace_search" || focused_view == "view.recent" ||
        focused_view == "view.settings";

    if (persistence) {
        const auto active = persistence->active_preset();
        static constexpr const char* preset_actions[] = {
            "workspace.switch.analysis", "workspace.switch.debugging", "workspace.switch.memory",
            "workspace.switch.types-structures", "workspace.switch.network",
            "workspace.switch.automation-ai", "workspace.switch.programming"
        };
        static constexpr docking::workspace_preset_t preset_order[] = {
            docking::workspace_preset_t::analysis, docking::workspace_preset_t::debugging,
            docking::workspace_preset_t::memory, docking::workspace_preset_t::types_structures,
            docking::workspace_preset_t::network, docking::workspace_preset_t::automation_ai,
            docking::workspace_preset_t::programming
        };
        for (auto& entry : workspaces_) {
            if (!entry.action)
                continue;
            bool entry_active = false;
            for (std::size_t i = 0; i < 7; ++i) {
                if (std::strcmp(entry.action_id, preset_actions[i]) == 0) {
                    entry_active = !utility_focused && active == preset_order[i];
                    break;
                }
            }
            entry.action->setChecked(entry_active);
            if (entry_active && !entry.action->isVisible())
                diag::log_tagged_fmt("activity_rail",
                    "active_workspace_hidden id=%s overflow=%d",
                    entry.action_id, overflow_ ? 1 : 0);
        }
    }
    for (auto& entry : utilities_) {
        if (!entry.action)
            continue;
        const char* mapped =
            std::strcmp(entry.id, "utility.explorer") == 0 ? "view.project_explorer" :
            std::strcmp(entry.id, "utility.search") == 0 ? "view.workspace_search" :
            "view.recent";
        entry.action->setChecked(focused_view == mapped);
    }
    if (settings_entry_.action)
        settings_entry_.action->setChecked(focused_view == "view.settings");
    if (more_action_) {
        bool overflow_active = false;
        if (overflow_) {
            for (const auto& entry : workspaces_)
                if (entry.action && !entry.action->isVisible() && entry.action->isChecked())
                    overflow_active = true;
            if (utility_focused && focused_view != "view.settings")
                overflow_active = true;
        }
        more_action_->setChecked(overflow_active);
    }
    syncActiveMarker();
    update();
}

void AidaActivityRail::refreshVisibility()
{
    const bool visible_setting = g_sa_settings.activity_bar_visible;
    if (visible_setting == visibility_applied_)
        return;
    visibility_applied_ = visible_setting;
    setVisible(visible_setting);
}

QWidget* AidaActivityRail::activeButtonWidget() const
{
    for (const auto& entry : workspaces_)
        if (entry.action && entry.action->isVisible() && entry.action->isChecked())
            return widgetForAction(entry.action);
    for (const auto& entry : utilities_)
        if (entry.action && entry.action->isVisible() && entry.action->isChecked())
            return widgetForAction(entry.action);
    if (settings_entry_.action && settings_entry_.action->isChecked())
        return widgetForAction(settings_entry_.action);
    return nullptr;
}

void AidaActivityRail::applyOverflow()
{
    const auto& t = theme::tokens();
    const auto measure = [this](const rail_entry_t& entry) -> int {
        if (!entry.action)
            return 0;
        const QWidget* button = widgetForAction(entry.action);
        return button ? (std::max)(button->sizeHint().height(), 1) : 0;
    };
    int button_h = 0;
    for (const auto& entry : workspaces_) {
        button_h = measure(entry);
        if (button_h > 0)
            break;
    }
    if (button_h <= 0)
        button_h = static_cast<int>(t.control.icon_button);
    const int item_gap = layout() ? (std::max)(layout()->spacing(), 0) : t.spacing.xs;
    const int stride = (std::max)(button_h + item_gap, 1);
    const int separator_h = t.panel.border + t.spacing.xs + t.spacing.xxs;
    const QMargins margins = layout() ? layout()->contentsMargins() : QMargins();
    const int usable = (std::max)(0, height() - margins.top() - margins.bottom());

    const int workspace_count = static_cast<int>(workspaces_.size());
    const int full_items = workspace_count + static_cast<int>(utilities_.size()) + 1;
    const int need_full = full_items * stride + 2 * separator_h;
    overflow_ = usable < need_full;

    int direct_capacity = workspace_count;
    if (overflow_) {
        const int reserved = 2 * separator_h + 2 * stride;
        direct_capacity = (std::max)(1,
            (std::min)(workspace_count, (usable - reserved) / stride));
    }

    std::vector<bool> direct(workspaces_.size(), false);
    for (int i = 0; i < direct_capacity; ++i)
        direct[static_cast<std::size_t>(i)] = true;

    int active_index = -1;
    if (overflow_) {
        std::vector<bool> must(workspaces_.size(), false);
        if (const auto* persistence = host_ ? host_->persistence() : nullptr) {
            const auto active = persistence->active_preset();
            static constexpr docking::workspace_preset_t order[] = {
                docking::workspace_preset_t::analysis, docking::workspace_preset_t::debugging,
                docking::workspace_preset_t::memory, docking::workspace_preset_t::types_structures,
                docking::workspace_preset_t::network, docking::workspace_preset_t::programming,
                docking::workspace_preset_t::automation_ai
            };
            for (std::size_t i = 0; i < workspaces_.size(); ++i)
                if (order[i] == active) {
                    active_index = static_cast<int>(i);
                    break;
                }
            if (active_index >= 0)
                must[static_cast<std::size_t>(active_index)] = true;
        }
        for (std::size_t i = 0; i < workspaces_.size(); ++i)
            if (workspaces_[i].action && workspaces_[i].action->isChecked())
                must[i] = true;
        for (std::size_t i = 0; i < workspaces_.size(); ++i) {
            if (!must[i] || direct[i])
                continue;
            for (int j = direct_capacity - 1; j >= 0; --j) {
                if (!must[static_cast<std::size_t>(j)]) {
                    direct[static_cast<std::size_t>(j)] = false;
                    break;
                }
            }
            direct[i] = true;
        }
    }

    unsigned vis_mask = 0;
    for (std::size_t i = 0; i < workspaces_.size(); ++i) {
        if (!workspaces_[i].action)
            continue;
        workspaces_[i].action->setVisible(direct[i]);
        if (direct[i])
            vis_mask |= 1u << i;
    }
    for (auto& entry : utilities_)
        if (entry.action)
            entry.action->setVisible(!overflow_);
    if (more_action_)
        more_action_->setVisible(overflow_);

    if (overflow_ != last_overflow_state_ || vis_mask != last_vis_mask_) {
        unsigned checked_mask = 0;
        for (std::size_t i = 0; i < workspaces_.size(); ++i)
            if (workspaces_[i].action && workspaces_[i].action->isChecked())
                checked_mask |= 1u << i;
        diag::log_tagged_fmt("activity_rail",
            "overflow_apply h=%d usable=%d stride=%d btn_h=%d sep_h=%d need_full=%d overflow=%d cap=%d active_idx=%d checked=0x%X vis=0x%X",
            height(), usable, stride, button_h, separator_h, need_full,
            overflow_ ? 1 : 0, direct_capacity, active_index, checked_mask, vis_mask);
        last_overflow_state_ = overflow_;
        last_vis_mask_ = vis_mask;
    }
}

void AidaActivityRail::resizeEvent(QResizeEvent* event)
{
    QToolBar::resizeEvent(event);
    applyOverflow();
    refreshActiveStates();
}

bool AidaActivityRail::event(QEvent* event)
{
    if (event->type() == QEvent::LayoutRequest)
        syncActiveMarker();
    return QToolBar::event(event);
}

void AidaActivityRail::syncActiveMarker()
{
    if (!active_marker_)
        return;
    QWidget* active = activeButtonWidget();
    if (!active || !active->isVisible()) {
        active_marker_->hide();
        return;
    }
    const auto& t = theme::tokens();
    active_marker_->setFixedWidth((std::max)(t.grid / 2, t.radius.xs));
    const int inset = (std::max)(t.spacing.xs, active->height() / 5);
    active_marker_->setGeometry(0, active->y() + inset, active_marker_->width(),
        (std::max)(t.spacing.sm, active->height() - inset * 2));
    active_marker_->show();
    active_marker_->raise();
}

}
