#include "qt/editor/syntax_roles.hpp"

#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::editor {

QColor token_color(syntax::token_type type, qreal alpha)
{
    const auto& t = theme::tokens();
    QColor color;
    switch (type) {
    case syntax::token_type::keyword:       color = t.syn_keyword; break;
    case syntax::token_type::type_name:     color = t.syn_type; break;
    case syntax::token_type::string_lit:    color = t.syn_string; break;
    case syntax::token_type::number:        color = t.syn_number; break;
    case syntax::token_type::comment_line:  color = t.syn_comment; break;
    case syntax::token_type::comment_block: color = t.syn_comment; break;
    case syntax::token_type::preprocessor:  color = t.syn_preprocessor; break;
    case syntax::token_type::operator_sym:  color = t.syn_operator; break;
    case syntax::token_type::function_call: color = t.syn_function; break;
    case syntax::token_type::identifier:    color = t.syn_identifier; break;
    case syntax::token_type::whitespace:    color = Qt::transparent; break;
    case syntax::token_type::punctuation:   color = t.syn_operator; break;
    case syntax::token_type::decorator:     color = t.syn_preprocessor; break;
    case syntax::token_type::boolean_lit:   color = t.syn_number; break;
    case syntax::token_type::register_name: color = t.syn_register; break;
    case syntax::token_type::directive:     color = t.syn_preprocessor; break;
    case syntax::token_type::COUNT:         color = t.syn_identifier; break;
    }
    if (type == syntax::token_type::punctuation) {
        color.setAlphaF(color.alphaF() * alpha * 0.85);
        return color;
    }
    color.setAlphaF(color.alphaF() * alpha);
    return color;
}

QColor token_color(syntax::token_type type)
{
    return token_color(type, 1.0);
}

QVector<QTextLayout::FormatRange> line_formats(
    const std::vector<syntax::token_t>& tokens, const QFont& font)
{
    QVector<QTextLayout::FormatRange> ranges;
    ranges.reserve(static_cast<qsizetype>(tokens.size()));
    for (const auto& token : tokens) {
        if (token.type == syntax::token_type::whitespace || token.length == 0)
            continue;
        QTextLayout::FormatRange range;
        range.start = static_cast<int>(token.start);
        range.length = static_cast<int>(token.length);
        range.format.setForeground(token_color(token.type));
        range.format.setFont(font);
        ranges.push_back(range);
    }
    return ranges;
}

}
