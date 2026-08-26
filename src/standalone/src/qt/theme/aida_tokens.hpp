#pragma once

#include <QColor>
#include <QRgb>

#include <algorithm>

namespace aida::qt::theme {

struct tokens_t {
    QColor bg_base{ 27, 31, 38 };
    QColor bg_elevated{ 36, 41, 50 };
    QColor bg_overlay{ 40, 45, 56 };
    QColor panel_bg{ 32, 36, 44 };
    QColor panel_header{ 39, 44, 54 };
    QColor glass_tint{ 27, 31, 38 };
    QColor title_bar{ 21, 24, 30 };

    QColor border_subtle{ 55, 61, 73 };
    QColor border_strong{ 74, 82, 97 };
    QColor border_focus{ 94, 142, 222 };

    QColor text_primary{ 216, 222, 231 };
    QColor text_secondary{ 154, 164, 178 };
    QColor text_dim{ 108, 118, 132 };
    QColor text_address{ 216, 222, 231 };
    QColor text_lineno{ 93, 102, 115 };

    QColor hover_wash{ 228, 234, 242, 20 };
    QColor selection{ 94, 142, 222, 72 };
    QColor selection_strong{ 94, 142, 222 };
    float disabled_alpha = 0.46f;

    QColor accent{ 94, 142, 222 };
    QRgb accent_u32 = 0xFF5E8EDE;
    QColor accent_hover{ 122, 163, 232 };
    QColor accent_dim{ 94, 142, 222, 120 };
    QColor accent_glow{ 94, 142, 222 };
    QColor accent_grad_top{ 111, 156, 230 };
    QColor accent_grad_bot{ 76, 123, 198 };

    QColor scrollbar_bg{ 0, 0, 0, 0 };
    QColor scrollbar_grab{ 75, 82, 97 };
    QColor scrollbar_grab_hover{ 91, 99, 115 };
    QColor scrollbar_grab_active{ 108, 116, 133 };
    QColor frame_bg{ 52, 84, 132, 138 };

    QColor alt_row{ 39, 44, 53 };
    QColor link_visited{ 160, 170, 216 };

    QColor success{ 111, 174, 126 };
    QColor success_soft{ 35, 52, 40, 150 };
    QColor warning{ 196, 160, 95 };
    QColor warning_soft{ 59, 50, 32, 150 };
    QColor error{ 201, 114, 123 };
    QColor error_soft{ 62, 38, 43, 150 };
    QColor info{ 127, 163, 219 };
    QColor info_soft{ 38, 52, 74, 150 };
    QColor live{ 111, 174, 126 };
    QColor stale{ 196, 160, 95 };
    QColor breakpoint{ 201, 114, 123 };
    QColor changed{ 127, 163, 219 };
    QColor disabled{ 100, 110, 123 };

    QColor text_on_accent{ 255, 255, 255 };
    QColor sheen{ 255, 255, 255 };
    QColor neutral_soft{ 154, 164, 178, 26 };
    QColor success_fill{ 111, 174, 126, 56 };
    QColor success_edge{ 111, 174, 126, 140 };
    QColor warning_fill{ 196, 160, 95, 56 };
    QColor warning_edge{ 196, 160, 95, 140 };
    QColor error_fill{ 201, 114, 123, 56 };
    QColor error_edge{ 201, 114, 123, 140 };
    QColor info_fill{ 127, 163, 219, 56 };
    QColor info_edge{ 127, 163, 219, 140 };
    QColor accent_fill{ 94, 142, 222, 56 };
    QColor accent_edge{ 94, 142, 222, 140 };
    QColor accent_line{ 94, 142, 222, 52 };
    QColor neutral_fill{ 154, 164, 178, 48 };
    QColor neutral_edge{ 154, 164, 178, 112 };
    QColor panel_header_glass{ 39, 44, 54, 140 };
    QColor shade_light{ 59, 66, 80 };
    QColor shade_midlight{ 46, 51, 62 };
    QColor shade_shadow{ 0, 0, 0 };

    QColor syn_keyword{ 187, 143, 210 };
    QColor syn_type{ 86, 182, 194 };
    QColor syn_string{ 200, 150, 120 };
    QColor syn_number{ 181, 206, 168 };
    QColor syn_comment{ 114, 128, 147 };
    QColor syn_function{ 218, 214, 168 };
    QColor syn_identifier{ 199, 208, 220 };
    QColor syn_register{ 206, 140, 154 };
    QColor syn_address{ 127, 163, 219 };
    QColor syn_preprocessor{ 169, 140, 200 };
    QColor syn_operator{ 168, 178, 192 };

    struct spacing_t {
        int xxs = 2;
        int xs = 4;
        int sm = 8;
        int md = 12;
        int lg = 16;
        int xl = 20;
        int xxl = 24;
        int section = 32;
    } spacing;

    struct radius_t {
        int xs = 3;
        int sm = 4;
        int md = 6;
        int lg = 8;
        int modal = 10;
        int pill = 12;
    } radius;

    struct control_t {
        int height_sm = 28;
        int height_md = 32;
        int height_lg = 40;
        int icon_button = 28;
        int icon_glyph = 14;
        int toolbar_h = 36;
        int input_h = 32;
        int search_h = 32;
        int checkbox = 16;
        int focus_ring = 2;
        int arrow_glyph = 10;
        int indicator_gap = 6;
        int slider_handle = 14;
        int slider_handle_radius = 7;
        int slider_handle_margin = 6;
    } control;

    struct panel_t {
        int padding = 12;
        int padding_compact = 8;
        int header_h = 40;
        int view_header_h = 56;
        int overlay_margin = 32;
        int border = 1;
        int notice_bar = 3;
    } panel;

    struct tab_t {
        int inner_h = 28;
        int primary_h = 32;
        int document_h = 28;
        int underline = 2;
        int padding_x = 12;
    } tab;

    struct row_t {
        int compact = 24;
        int standard = 28;
        int inspector = 32;
        int property_label_w = 132;
    } row;

    struct table_t {
        int header_h = 30;
        int row_h = 28;
        int compact_row_h = 24;
        int cell_pad_x = 8;
        int cell_pad_y = 4;
    } table;

    struct toolbar_t {
        int height = 36;
        int group_gap = 8;
        int separator_h = 20;
        int padding_x = 8;
        int padding_y = 4;
    } toolbar;

    struct status_bar_t {
        int height = 24;
        int padding_x = 8;
        int item_gap = 8;
        int dot = 5;
    } status_bar;

    struct splitter_t {
        int thickness = 5;
        int visible = 1;
        int hit_padding = 2;
    } splitter;

    struct typography_t {
        qreal caption_scale = 0.86;
        qreal body_scale = 0.94;
        qreal title_scale = 1.12;
        qreal view_title_scale = 1.20;
        qreal code_line_height = 1.45;
    } typography;

    struct shell_t {
        qreal title_h = 40.0;
        qreal menu_h = 32.0;
        qreal splitter_w = 5.0;
        qreal corner_radius = 6.0;
        qreal panel_radius = 6.0;
        qreal control_radius = 4.0;
        qreal title_logo = 22.0;
        qreal title_control = 28.0;
        qreal menu_pad_x = 12.0;
        qreal menu_item_pad_x = 12.0;
        qreal activity_bar_w = 52.0;
        qreal activity_icon = 32.0;
        qreal activity_footer_h = 48.0;
        qreal bottom_tab_h = 28.0;
        qreal bottom_action_h = 28.0;
        qreal min_panel_w = 96.0;
    } shell;

    struct motion_t {
        int instant = 80;
        int fast = 140;
        int standard = 200;
        int emphasized = 280;
        int theme = 240;
        int xs = 80;
        int sm = 140;
        int md = 220;
        int lg = 320;
        int xl = 480;
        int xxl = 720;
        int hero = 1200;
        int spring_max = 4000;
    } motion;

    int grid = 4;
};

constexpr tokens_t default_tokens() { return tokens_t{}; }

inline tokens_t& tokens()
{
    static tokens_t instance = default_tokens();
    return instance;
}

inline int scale_logical(qreal value, qreal factor)
{
    const qreal scaled = value * factor;
    return static_cast<int>(scaled + 0.5);
}

inline void apply_density_geometry(tokens_t& t, bool comfortable)
{
    if (!comfortable)
        return;

    const qreal f = 1.15;

    t.spacing.xxs = scale_logical(t.spacing.xxs, f);
    t.spacing.xs = scale_logical(t.spacing.xs, f);
    t.spacing.sm = scale_logical(t.spacing.sm, f);
    t.spacing.md = scale_logical(t.spacing.md, f);
    t.spacing.lg = scale_logical(t.spacing.lg, f);
    t.spacing.xl = scale_logical(t.spacing.xl, f);
    t.spacing.xxl = scale_logical(t.spacing.xxl, f);
    t.spacing.section = scale_logical(t.spacing.section, f);

    t.radius.xs = scale_logical(t.radius.xs, f);
    t.radius.sm = scale_logical(t.radius.sm, f);
    t.radius.md = scale_logical(t.radius.md, f);
    t.radius.lg = scale_logical(t.radius.lg, f);
    t.radius.modal = scale_logical(t.radius.modal, f);

    t.control.height_sm = scale_logical(t.control.height_sm, f);
    t.control.height_md = scale_logical(t.control.height_md, f);
    t.control.height_lg = scale_logical(t.control.height_lg, f);
    t.control.icon_button = scale_logical(t.control.icon_button, f);
    t.control.icon_glyph = scale_logical(t.control.icon_glyph, f);
    t.control.toolbar_h = scale_logical(t.control.toolbar_h, f);
    t.control.input_h = scale_logical(t.control.input_h, f);
    t.control.search_h = scale_logical(t.control.search_h, f);
    t.control.checkbox = scale_logical(t.control.checkbox, f);
    t.control.focus_ring = (std::max)(1, scale_logical(t.control.focus_ring, f));
    t.control.arrow_glyph = scale_logical(t.control.arrow_glyph, f);
    t.control.indicator_gap = scale_logical(t.control.indicator_gap, f);
    t.control.slider_handle = scale_logical(t.control.slider_handle, f);
    t.control.slider_handle_radius = scale_logical(t.control.slider_handle_radius, f);
    t.control.slider_handle_margin = scale_logical(t.control.slider_handle_margin, f);

    t.panel.padding = scale_logical(t.panel.padding, f);
    t.panel.padding_compact = scale_logical(t.panel.padding_compact, f);
    t.panel.header_h = scale_logical(t.panel.header_h, f);
    t.panel.view_header_h = scale_logical(t.panel.view_header_h, f);
    t.panel.overlay_margin = scale_logical(t.panel.overlay_margin, f);
    t.panel.notice_bar = scale_logical(t.panel.notice_bar, f);

    t.tab.inner_h = scale_logical(t.tab.inner_h, f);
    t.tab.primary_h = scale_logical(t.tab.primary_h, f);
    t.tab.document_h = scale_logical(t.tab.document_h, f);
    t.tab.padding_x = scale_logical(t.tab.padding_x, f);

    t.row.compact = scale_logical(t.row.compact, f);
    t.row.standard = scale_logical(t.row.standard, f);
    t.row.inspector = scale_logical(t.row.inspector, f);
    t.row.property_label_w = scale_logical(t.row.property_label_w, f);

    t.table.header_h = scale_logical(t.table.header_h, f);
    t.table.row_h = scale_logical(t.table.row_h, f);
    t.table.compact_row_h = scale_logical(t.table.compact_row_h, f);
    t.table.cell_pad_x = scale_logical(t.table.cell_pad_x, f);
    t.table.cell_pad_y = scale_logical(t.table.cell_pad_y, f);

    t.toolbar.height = scale_logical(t.toolbar.height, f);
    t.toolbar.group_gap = scale_logical(t.toolbar.group_gap, f);
    t.toolbar.separator_h = scale_logical(t.toolbar.separator_h, f);
    t.toolbar.padding_x = scale_logical(t.toolbar.padding_x, f);
    t.toolbar.padding_y = scale_logical(t.toolbar.padding_y, f);

    t.status_bar.height = scale_logical(t.status_bar.height, f);
    t.status_bar.padding_x = scale_logical(t.status_bar.padding_x, f);
    t.status_bar.item_gap = scale_logical(t.status_bar.item_gap, f);

    t.splitter.thickness = scale_logical(t.splitter.thickness, f);
    t.splitter.hit_padding = scale_logical(t.splitter.hit_padding, f);

    t.shell.title_h *= f;
    t.shell.menu_h *= f;
    t.shell.splitter_w *= f;
    t.shell.corner_radius *= f;
    t.shell.panel_radius *= f;
    t.shell.control_radius *= f;
    t.shell.title_logo *= f;
    t.shell.title_control *= f;
    t.shell.menu_pad_x *= f;
    t.shell.menu_item_pad_x *= f;
    t.shell.activity_bar_w *= f;
    t.shell.activity_icon *= f;
    t.shell.activity_footer_h *= f;
    t.shell.bottom_tab_h *= f;
    t.shell.bottom_action_h *= f;
    t.shell.min_panel_w *= f;

    t.grid = scale_logical(t.grid, f);

    t.control.height_sm = 32;
    t.toolbar.height = 42;
    t.row.compact = 30;
    t.table.compact_row_h = 30;
    t.panel.padding_compact = 12;
    t.row.property_label_w = 148;
}

}
