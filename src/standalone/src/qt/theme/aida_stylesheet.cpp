#include "aida_stylesheet.hpp"

#include <QApplication>
#include <QFile>
#include <QStyle>
#include <QWidget>

#include "aida_tokens.hpp"
#include "aida_fonts.hpp"
#include "helpers/diag_log.hpp"

namespace aida::qt::theme::stylesheet {

namespace {

QString s_lastFingerprint;
QString s_lastSheet;

QString colorText(const QColor& color)
{
    if (color.alpha() >= 255)
        return color.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QString px(int value)
{
    return QString::number(value) + QStringLiteral("px");
}

QString pxq(qreal value)
{
    return QString::number(qRound64(value)) + QStringLiteral("px");
}

void insertColor(QHash<QString, QString>& map, const char* name, const QColor& color)
{
    map.insert(QString::fromLatin1(name), colorText(color));
}

void insertPx(QHash<QString, QString>& map, const char* name, int value)
{
    map.insert(QString::fromLatin1(name), px(value));
}

void insertPxQ(QHash<QString, QString>& map, const char* name, qreal value)
{
    map.insert(QString::fromLatin1(name), pxq(value));
}

QString readTemplateFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

}

QString liveDirectory()
{
    const QByteArray dir = qgetenv("AIDA_QSS_LIVE_DIR");
    if (dir.isEmpty())
        return QString();
    return QString::fromUtf8(dir);
}

QString loadTemplate()
{
    const QString live = liveDirectory();
    if (!live.isEmpty()) {
        const QString path = live + QStringLiteral("/aida_dark.qss");
        const QString text = readTemplateFile(path);
        if (text.isEmpty())
            diag::log_tagged_fmt("qt_theme", "qss_live_read_failed path=%s", path.toUtf8().constData());
        return text;
    }
    const QString text = readTemplateFile(QStringLiteral(":/qss/aida_dark.qss"));
    if (text.isEmpty())
        diag::log_tagged_fmt("qt_theme", "qss_resource_read_failed path=:/qss/aida_dark.qss");
    return text;
}

QHash<QString, QString> tokenSubstitutions(const tokens_t& t)
{
    QHash<QString, QString> map;

    insertColor(map, "bg_base", t.bg_base);
    insertColor(map, "bg_elevated", t.bg_elevated);
    insertColor(map, "bg_overlay", t.bg_overlay);
    insertColor(map, "panel_bg", t.panel_bg);
    insertColor(map, "panel_header", t.panel_header);
    insertColor(map, "glass_tint", t.glass_tint);
    insertColor(map, "title_bar", t.title_bar);
    insertColor(map, "border_subtle", t.border_subtle);
    insertColor(map, "border_strong", t.border_strong);
    insertColor(map, "border_focus", t.border_focus);
    insertColor(map, "text_primary", t.text_primary);
    insertColor(map, "text_secondary", t.text_secondary);
    insertColor(map, "text_dim", t.text_dim);
    insertColor(map, "text_address", t.text_address);
    insertColor(map, "text_lineno", t.text_lineno);
    insertColor(map, "hover_wash", t.hover_wash);
    insertColor(map, "selection", t.selection);
    insertColor(map, "selection_strong", t.selection_strong);
    insertColor(map, "accent", t.accent);
    insertColor(map, "accent_hover", t.accent_hover);
    insertColor(map, "accent_dim", t.accent_dim);
    insertColor(map, "accent_glow", t.accent_glow);
    insertColor(map, "accent_grad_top", t.accent_grad_top);
    insertColor(map, "accent_grad_bot", t.accent_grad_bot);
    insertColor(map, "scrollbar_bg", t.scrollbar_bg);
    insertColor(map, "scrollbar_grab", t.scrollbar_grab);
    insertColor(map, "scrollbar_grab_hover", t.scrollbar_grab_hover);
    insertColor(map, "scrollbar_grab_active", t.scrollbar_grab_active);
    insertColor(map, "frame_bg", t.frame_bg);
    insertColor(map, "alt_row", t.alt_row);
    insertColor(map, "link_visited", t.link_visited);
    insertColor(map, "success", t.success);
    insertColor(map, "success_soft", t.success_soft);
    insertColor(map, "warning", t.warning);
    insertColor(map, "warning_soft", t.warning_soft);
    insertColor(map, "error", t.error);
    insertColor(map, "error_soft", t.error_soft);
    insertColor(map, "info", t.info);
    insertColor(map, "info_soft", t.info_soft);
    insertColor(map, "live", t.live);
    insertColor(map, "stale", t.stale);
    insertColor(map, "breakpoint", t.breakpoint);
    insertColor(map, "changed", t.changed);
    insertColor(map, "disabled", t.disabled);
    map.insert(QStringLiteral("disabled_alpha"), QString::number(t.disabled_alpha));

    insertColor(map, "text_on_accent", t.text_on_accent);
    insertColor(map, "sheen", t.sheen);
    insertColor(map, "neutral_soft", t.neutral_soft);
    insertColor(map, "success_fill", t.success_fill);
    insertColor(map, "success_edge", t.success_edge);
    insertColor(map, "warning_fill", t.warning_fill);
    insertColor(map, "warning_edge", t.warning_edge);
    insertColor(map, "error_fill", t.error_fill);
    insertColor(map, "error_edge", t.error_edge);
    insertColor(map, "info_fill", t.info_fill);
    insertColor(map, "info_edge", t.info_edge);
    insertColor(map, "accent_fill", t.accent_fill);
    insertColor(map, "accent_edge", t.accent_edge);
    insertColor(map, "accent_line", t.accent_line);
    insertColor(map, "neutral_fill", t.neutral_fill);
    insertColor(map, "neutral_edge", t.neutral_edge);
    insertColor(map, "panel_header_glass", t.panel_header_glass);
    insertColor(map, "shade_light", t.shade_light);
    insertColor(map, "shade_midlight", t.shade_midlight);
    insertColor(map, "shade_shadow", t.shade_shadow);

    insertColor(map, "syn_keyword", t.syn_keyword);
    insertColor(map, "syn_type", t.syn_type);
    insertColor(map, "syn_string", t.syn_string);
    insertColor(map, "syn_number", t.syn_number);
    insertColor(map, "syn_comment", t.syn_comment);
    insertColor(map, "syn_function", t.syn_function);
    insertColor(map, "syn_identifier", t.syn_identifier);
    insertColor(map, "syn_register", t.syn_register);
    insertColor(map, "syn_address", t.syn_address);
    insertColor(map, "syn_preprocessor", t.syn_preprocessor);
    insertColor(map, "syn_operator", t.syn_operator);

    insertPx(map, "grid", t.grid);
    insertPx(map, "spacing_xxs", t.spacing.xxs);
    insertPx(map, "spacing_xs", t.spacing.xs);
    insertPx(map, "spacing_sm", t.spacing.sm);
    insertPx(map, "spacing_md", t.spacing.md);
    insertPx(map, "spacing_lg", t.spacing.lg);
    insertPx(map, "spacing_xl", t.spacing.xl);
    insertPx(map, "spacing_xxl", t.spacing.xxl);
    insertPx(map, "spacing_section", t.spacing.section);

    insertPx(map, "radius_xs", t.radius.xs);
    insertPx(map, "radius_sm", t.radius.sm);
    insertPx(map, "radius_md", t.radius.md);
    insertPx(map, "radius_lg", t.radius.lg);
    insertPx(map, "radius_modal", t.radius.modal);
    insertPx(map, "radius_pill", t.radius.pill);

    insertPx(map, "control_height_sm", t.control.height_sm);
    insertPx(map, "control_height_md", t.control.height_md);
    insertPx(map, "control_height_lg", t.control.height_lg);
    insertPx(map, "icon_button", t.control.icon_button);
    insertPx(map, "icon_glyph", t.control.icon_glyph);
    insertPx(map, "control_toolbar_h", t.control.toolbar_h);
    insertPx(map, "input_h", t.control.input_h);
    insertPx(map, "search_h", t.control.search_h);
    insertPx(map, "checkbox_size", t.control.checkbox);
    insertPx(map, "focus_ring", t.control.focus_ring);
    insertPx(map, "arrow_glyph", t.control.arrow_glyph);
    insertPx(map, "indicator_gap", t.control.indicator_gap);
    insertPx(map, "slider_handle", t.control.slider_handle);
    insertPx(map, "slider_handle_radius", t.control.slider_handle_radius);
    insertPx(map, "slider_handle_margin", t.control.slider_handle_margin);

    insertPx(map, "panel_padding", t.panel.padding);
    insertPx(map, "panel_padding_compact", t.panel.padding_compact);
    insertPx(map, "panel_header_h", t.panel.header_h);
    insertPx(map, "view_header_h", t.panel.view_header_h);
    insertPx(map, "overlay_margin", t.panel.overlay_margin);
    insertPx(map, "panel_border", t.panel.border);
    insertPx(map, "notice_bar", t.panel.notice_bar);

    insertPx(map, "tab_inner_h", t.tab.inner_h);
    insertPx(map, "tab_primary_h", t.tab.primary_h);
    insertPx(map, "tab_document_h", t.tab.document_h);
    insertPx(map, "tab_underline", t.tab.underline);
    insertPx(map, "tab_padding_x", t.tab.padding_x);

    insertPx(map, "row_compact", t.row.compact);
    insertPx(map, "row_standard", t.row.standard);
    insertPx(map, "row_inspector", t.row.inspector);
    insertPx(map, "property_label_w", t.row.property_label_w);

    insertPx(map, "table_header_h", t.table.header_h);
    insertPx(map, "table_row_h", t.table.row_h);
    insertPx(map, "table_compact_row_h", t.table.compact_row_h);
    insertPx(map, "cell_pad_x", t.table.cell_pad_x);
    insertPx(map, "cell_pad_y", t.table.cell_pad_y);

    insertPx(map, "toolbar_height", t.toolbar.height);
    insertPx(map, "toolbar_group_gap", t.toolbar.group_gap);
    insertPx(map, "toolbar_separator_h", t.toolbar.separator_h);
    insertPx(map, "toolbar_padding_x", t.toolbar.padding_x);
    insertPx(map, "toolbar_padding_y", t.toolbar.padding_y);

    insertPx(map, "statusbar_height", t.status_bar.height);
    insertPx(map, "statusbar_padding_x", t.status_bar.padding_x);
    insertPx(map, "statusbar_item_gap", t.status_bar.item_gap);
    insertPx(map, "statusbar_dot", t.status_bar.dot);

    insertPx(map, "splitter_thickness", t.splitter.thickness);
    insertPx(map, "splitter_visible", t.splitter.visible);
    insertPx(map, "splitter_hit_padding", t.splitter.hit_padding);

    insertPx(map, "control_content_h", t.control.height_md - 2 * t.panel.border - 2 * t.spacing.xs);
    insertPx(map, "tab_content_document_h", t.tab.document_h - 2 * t.spacing.xs - t.tab.underline);
    insertPx(map, "tab_content_primary_h", t.tab.primary_h - 2 * t.spacing.xs - t.tab.underline);
    insertPx(map, "icon_button_content", t.control.icon_button - 2 * t.spacing.xs);
    insertPx(map, "toolbar_content_h", t.toolbar.height - 2 * t.toolbar.padding_y);
    insertPxQ(map, "shell_activity_bar_content_w", t.shell.activity_bar_w - 2.0 * t.toolbar.padding_x);

    insertPxQ(map, "shell_title_h", t.shell.title_h);
    insertPxQ(map, "shell_menu_h", t.shell.menu_h);
    insertPxQ(map, "shell_splitter_w", t.shell.splitter_w);
    insertPxQ(map, "shell_corner", t.shell.corner_radius);
    insertPxQ(map, "shell_panel_radius", t.shell.panel_radius);
    insertPxQ(map, "shell_control_radius", t.shell.control_radius);
    insertPxQ(map, "shell_title_logo", t.shell.title_logo);
    insertPxQ(map, "shell_title_control", t.shell.title_control);
    insertPxQ(map, "shell_menu_pad_x", t.shell.menu_pad_x);
    insertPxQ(map, "shell_menu_item_pad_x", t.shell.menu_item_pad_x);
    insertPxQ(map, "shell_activity_bar_w", t.shell.activity_bar_w);
    insertPxQ(map, "shell_activity_icon", t.shell.activity_icon);
    insertPxQ(map, "shell_activity_footer_h", t.shell.activity_footer_h);
    insertPxQ(map, "shell_bottom_tab_h", t.shell.bottom_tab_h);
    insertPxQ(map, "shell_bottom_action_h", t.shell.bottom_action_h);
    insertPxQ(map, "shell_min_panel_w", t.shell.min_panel_w);

    map.insert(QStringLiteral("type_caption_scale"), QString::number(t.typography.caption_scale));
    map.insert(QStringLiteral("type_body_scale"), QString::number(t.typography.body_scale));
    map.insert(QStringLiteral("type_title_scale"), QString::number(t.typography.title_scale));
    map.insert(QStringLiteral("type_view_title_scale"), QString::number(t.typography.view_title_scale));
    map.insert(QStringLiteral("type_code_line_height"), QString::number(t.typography.code_line_height));

    const QStringList families = fonts::uiFamilies();
    QStringList quoted;
    quoted.reserve(families.size());
    for (int i = 0; i < families.size(); ++i) {
        const bool generic = (i == families.size() - 1) && families.at(i) == QStringLiteral("sans-serif");
        quoted.append(generic ? families.at(i)
            : QLatin1Char('"') + families.at(i) + QLatin1Char('"'));
    }
    map.insert(QStringLiteral("font_ui"), quoted.join(QStringLiteral(", ")));

    return map;
}

QString substituteTokens(const QString& templateText, const tokens_t& t)
{
    const QHash<QString, QString> map = tokenSubstitutions(t);
    QString out = templateText;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        out.replace(QLatin1Char('@') + it.key() + QLatin1Char('@'), it.value());
    }
    return out;
}

QString renderStylesheet(const tokens_t& t)
{
    const QString templateText = loadTemplate();
    if (templateText.isEmpty())
        return QString();

    QString fingerprintSource = templateText;
    const QHash<QString, QString> map = tokenSubstitutions(t);
    QStringList keys = map.keys();
    keys.sort();
    for (const QString& key : keys) {
        fingerprintSource.append(QLatin1Char('\x1f'));
        fingerprintSource.append(key);
        fingerprintSource.append(QLatin1Char('\x1e'));
        fingerprintSource.append(map.value(key));
    }
    const QString fingerprint = QString::number(qHash(fingerprintSource));

    if (!s_lastSheet.isEmpty() && fingerprint == s_lastFingerprint)
        return s_lastSheet;

    QString out = templateText;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        out.replace(QLatin1Char('@') + it.key() + QLatin1Char('@'), it.value());

    if (out.contains(QLatin1Char('@'))) {
        diag::log_tagged_fmt("qt_theme", "qss_unresolved_tokens_present=1");
    }

    s_lastFingerprint = fingerprint;
    s_lastSheet = out;
    return out;
}

void applyToApplication(const tokens_t& t)
{
    if (!qApp)
        return;
    const QString sheet = renderStylesheet(t);
    if (sheet.isEmpty()) {
        diag::log_tagged_fmt("qt_theme", "qss_apply_skipped reason=empty_sheet");
        return;
    }
    qApp->setStyleSheet(sheet);
}

void repolish(QWidget* w)
{
    if (!w)
        return;
    QStyle* style = w->style();
    if (!style)
        return;
    style->unpolish(w);
    style->polish(w);
}

void clearCaches()
{
    s_lastFingerprint.clear();
    s_lastSheet.clear();
}

}
