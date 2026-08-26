#include "aida_palette.hpp"

#include "aida_tokens.hpp"

namespace aida::qt::theme {

QPalette build_dark_palette(const tokens_t& t)
{
    QPalette pal;

    const auto set_all = [&](QPalette::ColorRole role, const QColor& color) {
        pal.setColor(QPalette::Active, role, color);
        pal.setColor(QPalette::Inactive, role, color);
    };

    set_all(QPalette::Window, t.bg_base);
    set_all(QPalette::WindowText, t.text_primary);
    set_all(QPalette::Base, t.bg_elevated);
    set_all(QPalette::AlternateBase, t.alt_row);
    set_all(QPalette::Text, t.text_primary);
    set_all(QPalette::Button, t.panel_header);
    set_all(QPalette::ButtonText, t.text_primary);
    set_all(QPalette::ToolTipBase, t.bg_overlay);
    set_all(QPalette::ToolTipText, t.text_primary);
    set_all(QPalette::Highlight, t.accent);
    set_all(QPalette::HighlightedText, t.text_on_accent);
    set_all(QPalette::PlaceholderText, t.text_dim);
    set_all(QPalette::Link, t.info);
    set_all(QPalette::LinkVisited, t.link_visited);
    set_all(QPalette::BrightText, t.text_primary);
    set_all(QPalette::Light, t.shade_light);
    set_all(QPalette::Midlight, t.shade_midlight);
    set_all(QPalette::Mid, t.bg_base);
    set_all(QPalette::Dark, t.title_bar);
    set_all(QPalette::Shadow, t.shade_shadow);
    set_all(QPalette::Accent, t.accent);

    const int disabled_a = static_cast<int>(t.disabled_alpha * 255.0f + 0.5f);
    QColor disabled_fg = t.text_primary;
    disabled_fg.setAlpha(disabled_a);
    QColor disabled_highlight = t.accent;
    disabled_highlight.setAlpha(disabled_a);

    pal.setColor(QPalette::Disabled, QPalette::WindowText, disabled_fg);
    pal.setColor(QPalette::Disabled, QPalette::Text, disabled_fg);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, disabled_fg);
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled_fg);
    pal.setColor(QPalette::Disabled, QPalette::Highlight, disabled_highlight);
    pal.setColor(QPalette::Disabled, QPalette::Button, t.panel_header);
    pal.setColor(QPalette::Disabled, QPalette::Base, t.bg_elevated);
    pal.setColor(QPalette::Disabled, QPalette::AlternateBase, t.alt_row);
    pal.setColor(QPalette::Disabled, QPalette::Window, t.bg_base);
    pal.setColor(QPalette::Disabled, QPalette::Light, t.shade_light);
    pal.setColor(QPalette::Disabled, QPalette::Midlight, t.shade_midlight);
    pal.setColor(QPalette::Disabled, QPalette::Mid, t.bg_base);
    pal.setColor(QPalette::Disabled, QPalette::Dark, t.title_bar);
    pal.setColor(QPalette::Disabled, QPalette::Shadow, t.shade_shadow);
    pal.setColor(QPalette::Disabled, QPalette::ToolTipBase, t.bg_overlay);
    pal.setColor(QPalette::Disabled, QPalette::ToolTipText, disabled_fg);
    pal.setColor(QPalette::Disabled, QPalette::PlaceholderText, t.text_dim);
    pal.setColor(QPalette::Disabled, QPalette::BrightText, t.text_primary);
    pal.setColor(QPalette::Disabled, QPalette::Link, t.info);
    pal.setColor(QPalette::Disabled, QPalette::LinkVisited, t.link_visited);
    pal.setColor(QPalette::Disabled, QPalette::Accent, t.accent);

    return pal;
}

}
