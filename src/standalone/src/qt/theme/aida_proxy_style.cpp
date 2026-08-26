#include "aida_proxy_style.hpp"

#include <QStyleFactory>

#include "aida_tokens.hpp"

namespace aida::qt::theme {

AidaProxyStyle::AidaProxyStyle(QStyle* base)
    : QProxyStyle(base ? base : QStyleFactory::create(QStringLiteral("fusion")))
{
}

AidaProxyStyle::AidaProxyStyle(const QString& key)
    : QProxyStyle(key)
{
}

int AidaProxyStyle::pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const
{
    const tokens_t& t = tokens();
    switch (metric) {
    case QStyle::PM_SplitterWidth:
        return t.splitter.thickness;
    case QStyle::PM_ScrollBarExtent:
        return t.spacing.sm;
    case QStyle::PM_TabBarTabHSpace:
        return t.tab.padding_x * 2;
    case QStyle::PM_TabBarTabVSpace:
        return t.spacing.sm;
    case QStyle::PM_MenuBarItemSpacing:
        return 0;
    case QStyle::PM_ToolTipLabelFrameWidth:
        return t.panel.border;
    case QStyle::PM_FocusFrameHMargin:
    case QStyle::PM_FocusFrameVMargin:
        return t.spacing.xxs;
    case QStyle::PM_DefaultFrameWidth:
        return t.panel.border;
    case QStyle::PM_IndicatorWidth:
    case QStyle::PM_IndicatorHeight:
    case QStyle::PM_ExclusiveIndicatorWidth:
    case QStyle::PM_ExclusiveIndicatorHeight:
        return t.control.checkbox;
    case QStyle::PM_CheckBoxLabelSpacing:
    case QStyle::PM_RadioButtonLabelSpacing:
        return t.control.indicator_gap;
    default:
        break;
    }
    return QProxyStyle::pixelMetric(metric, option, widget);
}

int AidaProxyStyle::styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* returnData) const
{
    switch (hint) {
    case QStyle::SH_MenuBar_MouseTracking:
    case QStyle::SH_Menu_MouseTracking:
    case QStyle::SH_Menu_SloppySubMenus:
    case QStyle::SH_ComboBox_ListMouseTracking:
        return 1;
    case QStyle::SH_ItemView_ActivateItemOnSingleClick:
        break;
    default:
        break;
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

void AidaProxyStyle::drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const
{
    if (element == QStyle::PE_FrameFocusRect)
        return;
    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

}
