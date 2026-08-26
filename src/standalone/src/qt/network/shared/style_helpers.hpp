#pragma once

#include <QFontMetricsF>
#include <QString>
#include <QStyle>
#include <QWidget>

#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::net {

inline void set_aida_property(QWidget* widget, const char* name, const QString& value) {
    if (!widget)
        return;
    if (widget->property(name) == value)
        return;
    widget->setProperty(name, value);
    theme::stylesheet::repolish(widget);
}

inline void set_label_tone(QWidget* widget, const char* tone) {
    set_aida_property(widget, "aidaTone", QString::fromLatin1(tone));
}

inline void set_label_tone(QWidget* widget, const QString& tone) {
    set_aida_property(widget, "aidaTone", tone);
}

inline void set_label_variant(QWidget* widget, const char* variant) {
    set_aida_property(widget, "aidaVariant", QString::fromLatin1(variant));
}

inline int field_width_chars(const QWidget* widget, int chars) {
    const QFontMetricsF fm(widget->font());
    const int frame = widget->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, widget);
    const auto& t = theme::tokens();
    return qCeil(fm.horizontalAdvance(QString(chars, QLatin1Char('0')))) +
        2 * frame + 2 * t.spacing.xs + t.control.icon_glyph;
}

inline int spinbox_width_digits(const QWidget* widget, int digits) {
    return field_width_chars(widget, digits) + theme::tokens().control.icon_glyph +
        theme::tokens().spacing.xs;
}

inline int table_column_width_chars(const QWidget* view, int chars) {
    const QFontMetricsF fm(view->font());
    return qCeil(fm.horizontalAdvance(QString(chars, QLatin1Char('0')))) +
        2 * theme::tokens().table.cell_pad_x;
}

inline int editor_min_height_lines(const QWidget* widget, int lines) {
    const QFontMetricsF fm(widget->font());
    const int frame = widget->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, widget);
    const auto& t = theme::tokens();
    return qCeil(fm.height()) * lines + 2 * frame + 2 * t.spacing.xs;
}

inline int dialog_min_width_chars(const QWidget* widget, int chars) {
    const QFontMetricsF fm(widget->font());
    return qCeil(fm.horizontalAdvance(QString(chars, QLatin1Char('M')))) +
        2 * theme::tokens().panel.padding;
}

}

