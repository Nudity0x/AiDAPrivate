#pragma once

#include <QColor>
#include <QFontMetricsF>

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::scanner {

inline int mono_cell_width()
{
	const auto& grid = theme::fonts::monoGrid();
	if (grid.valid && grid.cell_w > 0.0)
		return static_cast<int>(grid.cell_w + 0.5);
	return static_cast<int>(QFontMetricsF(theme::fonts::codeRegular())
		.horizontalAdvance(QLatin1Char('0')) + 0.5);
}

struct scanner_palette_t {
	QColor text_primary;
	QColor text_secondary;
	QColor text_dim;
	QColor text_address;
	QColor selection;
	QColor selection_strong;
	QColor hover_wash;
	QColor alt_row;
	QColor accent;
	QColor accent_dim;
	QColor accent_glow;
	QColor success;
	QColor error;
	QColor warning;
	QColor info;
	QColor panel_bg;
	QColor panel_header;
	QColor border_subtle;
	QColor border_strong;
};

inline scanner_palette_t make_scanner_palette()
{
	const auto& t = theme::tokens();
	scanner_palette_t palette;
	palette.text_primary = t.text_primary;
	palette.text_secondary = t.text_secondary;
	palette.text_dim = t.text_dim;
	palette.text_address = t.text_address;
	palette.selection = t.selection;
	palette.selection_strong = t.selection_strong;
	palette.hover_wash = t.hover_wash;
	palette.alt_row = t.alt_row;
	palette.accent = t.accent;
	palette.accent_dim = t.accent_dim;
	palette.accent_glow = t.accent_glow;
	palette.success = t.success;
	palette.error = t.error;
	palette.warning = t.warning;
	palette.info = t.info;
	palette.panel_bg = t.panel_bg;
	palette.panel_header = t.panel_header;
	palette.border_subtle = t.border_subtle;
	palette.border_strong = t.border_strong;
	return palette;
}

}
