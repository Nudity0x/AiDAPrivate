#pragma once

#include "core/editor/syntax_highlight.hpp"

#include <QColor>
#include <QTextLayout>
#include <QVector>

namespace aida::qt::editor {

QColor token_color(syntax::token_type type);
QColor token_color(syntax::token_type type, qreal alpha);

QVector<QTextLayout::FormatRange> line_formats(
    const std::vector<syntax::token_t>& tokens, const QFont& font);

}
