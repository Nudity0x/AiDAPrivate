#include "aida_tool_button.hpp"
#include <algorithm>

#include <QEvent>

#include "../theme/aida_tokens.hpp"

namespace aida::qt::widgets {

AidaToolButton::AidaToolButton(QWidget* parent)
    : QToolButton(parent)
{
    setObjectName(QStringLiteral("aida.tool_button"));
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    setFocusPolicy(Qt::StrongFocus);
    setProperty("aidaRole", QStringLiteral("toolbar"));
    const int glyph = aida::qt::theme::tokens().control.icon_glyph;
    setIconSize(QSize(glyph, glyph));
}

AidaToolButton::AidaToolButton(const QIcon& icon, const QString& toolTip, QWidget* parent)
    : AidaToolButton(parent)
{
    setIcon(icon);
    setToolTip(toolTip);
}

void AidaToolButton::setActive(bool active)
{
    if (!isCheckable())
        setCheckable(true);
    setChecked(active);
}

bool AidaToolButton::isActive() const
{
    return isCheckable() && isChecked();
}

QSize AidaToolButton::sizeHint() const
{
    const QSize base = QToolButton::sizeHint();
    const auto& t = aida::qt::theme::tokens();
    const int min_side = t.control.icon_button;
    return QSize((std::max)(base.width(), min_side), (std::max)(base.height(), min_side));
}

void AidaToolButton::syncAccessibleName()
{
    if (!accessibleName().isEmpty() || !text().isEmpty() || toolTip().isEmpty())
        return;
    setAccessibleName(toolTip());
}

bool AidaToolButton::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTipChange)
        syncAccessibleName();
    return QToolButton::event(event);
}

}
