#pragma once


#include <cstdint>
#include <string>
#include <vector>

#include "syntax_highlight.hpp"

namespace chat_render {


enum class span_type : int {
    text = 0,
    bold,
    italic,
    bold_italic,
    inline_code,
    code_block,
    tool_call,
    tool_result,
    heading1,
    heading2,
    heading3,
    list_bullet,
    list_numbered,
    blockquote,
    link,
    strikethrough,
    task_unchecked,
    task_checked,
    hrule,
    table,
    paragraph_break
};


struct span_t {
    span_type   type;
    std::string text;
    std::string language;
    std::string url;
    int         depth        = 0;
    int         list_index   = 0;
    int         list_indent  = 0;
    std::vector<std::vector<std::string>> table_data;
};


std::vector<span_t> parse_markdown(const std::string& text);


syntax::language_def_t chat_language_def(const std::string& lang);


}
