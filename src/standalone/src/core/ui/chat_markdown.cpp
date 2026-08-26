#include "chat_markdown.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>


namespace {

inline bool is_space_or_tab(char c) { return c == ' ' || c == '\t'; }
inline bool is_digit_ch(char c)     { return c >= '0' && c <= '9'; }

inline std::string strip_lr(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space_or_tab(s[a])) ++a;
    while (b > a && is_space_or_tab(s[b - 1])) --b;
    return s.substr(a, b - a);
}

inline size_t leading_spaces(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    return i;
}

inline std::vector<std::string> split_pipe_row(const std::string& line) {
    std::vector<std::string> cells;
    std::string body = strip_lr(line);
    if (!body.empty() && body.front() == '|') body.erase(body.begin());
    if (!body.empty() && body.back() == '|') body.pop_back();
    std::string cur;
    bool escape = false;
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (escape) { cur += c; escape = false; continue; }
        if (c == '\\') { escape = true; continue; }
        if (c == '|') { cells.push_back(strip_lr(cur)); cur.clear(); continue; }
        cur += c;
    }
    cells.push_back(strip_lr(cur));
    return cells;
}

inline bool looks_like_table_separator(const std::string& line) {
    std::string t = strip_lr(line);
    if (t.empty() || t.find('|') == std::string::npos) return false;
    for (char c : t) {
        if (c != '|' && c != '-' && c != ':' && c != ' ' && c != '\t') return false;
    }
    return t.find('-') != std::string::npos;
}

inline bool is_hrule(const std::string& line) {
    std::string t = strip_lr(line);
    if (t.size() < 3) return false;
    char first = t.front();
    if (first != '-' && first != '*' && first != '_') return false;
    int count = 0;
    for (char c : t) {
        if (c == first) ++count;
        else if (c != ' ' && c != '\t') return false;
    }
    return count >= 3;
}


struct inline_emit_t {
    chat_render::span_type type;
    std::string text;
    std::string url;
};

void parse_inline(const std::string& src, std::vector<inline_emit_t>& out) {
    size_t i = 0;
    size_t n = src.size();
    std::string accum;

    auto flush = [&]() {
        if (!accum.empty()) {
            inline_emit_t e;
            e.type = chat_render::span_type::text;
            e.text = accum;
            out.push_back(std::move(e));
            accum.clear();
        }
    };

    while (i < n) {
        if (i + 2 < n && src[i] == '`') {
            size_t end = src.find('`', i + 1);
            if (end != std::string::npos && end > i + 1) {
                flush();
                inline_emit_t e;
                e.type = chat_render::span_type::inline_code;
                e.text = src.substr(i + 1, end - i - 1);
                out.push_back(std::move(e));
                i = end + 1;
                continue;
            }
        }

        if (i + 1 < n && src[i] == '`') {
            size_t end = src.find('`', i + 1);
            if (end != std::string::npos) {
                flush();
                inline_emit_t e;
                e.type = chat_render::span_type::inline_code;
                e.text = src.substr(i + 1, end - i - 1);
                out.push_back(std::move(e));
                i = end + 1;
                continue;
            }
        }

        if (i + 4 < n && src[i] == '*' && src[i + 1] == '*' && src[i + 2] == '*') {
            size_t end = src.find("***", i + 3);
            if (end != std::string::npos) {
                flush();
                inline_emit_t e;
                e.type = chat_render::span_type::bold_italic;
                e.text = src.substr(i + 3, end - i - 3);
                out.push_back(std::move(e));
                i = end + 3;
                continue;
            }
        }

        if (i + 3 < n && src[i] == '*' && src[i + 1] == '*') {
            size_t end = src.find("**", i + 2);
            if (end != std::string::npos && end > i + 2) {
                flush();
                inline_emit_t e;
                e.type = chat_render::span_type::bold;
                e.text = src.substr(i + 2, end - i - 2);
                out.push_back(std::move(e));
                i = end + 2;
                continue;
            }
        }

        if (i + 1 < n && src[i] == '*' && src[i + 1] != '*') {
            size_t end = src.find('*', i + 1);
            if (end != std::string::npos && end > i + 1 &&
                (end + 1 >= n || src[end + 1] != '*')) {
                flush();
                inline_emit_t e;
                e.type = chat_render::span_type::italic;
                e.text = src.substr(i + 1, end - i - 1);
                out.push_back(std::move(e));
                i = end + 1;
                continue;
            }
        }

        if (i + 3 < n && src[i] == '~' && src[i + 1] == '~') {
            size_t end = src.find("~~", i + 2);
            if (end != std::string::npos && end > i + 2) {
                flush();
                inline_emit_t e;
                e.type = chat_render::span_type::strikethrough;
                e.text = src.substr(i + 2, end - i - 2);
                out.push_back(std::move(e));
                i = end + 2;
                continue;
            }
        }

        if (src[i] == '[') {
            size_t close_label = src.find(']', i + 1);
            if (close_label != std::string::npos &&
                close_label + 1 < n && src[close_label + 1] == '(') {
                size_t close_url = src.find(')', close_label + 2);
                if (close_url != std::string::npos) {
                    flush();
                    inline_emit_t e;
                    e.type = chat_render::span_type::link;
                    e.text = src.substr(i + 1, close_label - i - 1);
                    e.url  = src.substr(close_label + 2, close_url - close_label - 2);
                    out.push_back(std::move(e));
                    i = close_url + 1;
                    continue;
                }
            }
        }

        accum += src[i++];
    }

    flush();
}

}


std::vector<chat_render::span_t> chat_render::parse_markdown(const std::string& text)
{
    std::vector<span_t> spans;
    if (text.empty()) return spans;

    auto push_inline_run = [&](const std::string& src) {
        std::vector<inline_emit_t> parts;
        parse_inline(src, parts);
        for (auto& p : parts) {
            span_t s;
            s.type = p.type;
            s.text = std::move(p.text);
            s.url  = std::move(p.url);
            spans.push_back(std::move(s));
        }
    };

    size_t i = 0;
    size_t n = text.size();

    while (i < n) {
        if (i + 2 < n && text[i] == '`' && text[i + 1] == '`' && text[i + 2] == '`') {
            i += 3;
            std::string lang;
            while (i < n && text[i] != '\n' && text[i] != '\r') lang += text[i++];
            if (i < n && text[i] == '\r') ++i;
            if (i < n && text[i] == '\n') ++i;
            std::string code;
            while (i < n) {
                if (i + 2 < n && text[i] == '`' && text[i + 1] == '`' && text[i + 2] == '`') {
                    i += 3;
                    if (i < n && text[i] == '\r') ++i;
                    if (i < n && text[i] == '\n') ++i;
                    break;
                }
                code += text[i++];
            }
            while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) code.pop_back();
            std::string trimmed_lang = strip_lr(lang);
            span_t s;
            s.type = span_type::code_block;
            s.text = std::move(code);
            s.language = std::move(trimmed_lang);
            spans.push_back(std::move(s));
            continue;
        }

        if (i + 10 < n && text.compare(i, 11, "<tool_call>") == 0) {
            i += 11;
            size_t end = text.find("</tool_call>", i);
            std::string content;
            if (end != std::string::npos) { content = text.substr(i, end - i); i = end + 12; }
            else { content = text.substr(i); i = n; }
            span_t s; s.type = span_type::tool_call; s.text = std::move(content);
            spans.push_back(std::move(s));
            continue;
        }

        if (i + 12 < n && text.compare(i, 13, "<tool_result>") == 0) {
            i += 13;
            size_t end = text.find("</tool_result>", i);
            std::string content;
            if (end != std::string::npos) { content = text.substr(i, end - i); i = end + 14; }
            else { content = text.substr(i); i = n; }
            span_t s; s.type = span_type::tool_result; s.text = std::move(content);
            spans.push_back(std::move(s));
            continue;
        }

        size_t line_start = i;
        size_t line_end = line_start;
        while (line_end < n && text[line_end] != '\n' && text[line_end] != '\r') ++line_end;
        std::string line = text.substr(line_start, line_end - line_start);
        size_t consumed_to = line_end;
        if (consumed_to < n && text[consumed_to] == '\r') ++consumed_to;
        if (consumed_to < n && text[consumed_to] == '\n') ++consumed_to;

        std::string trimmed = strip_lr(line);

        if (trimmed.empty()) {
            span_t s; s.type = span_type::paragraph_break; s.text = "";
            if (spans.empty() || spans.back().type != span_type::paragraph_break)
                spans.push_back(std::move(s));
            i = consumed_to;
            continue;
        }

        if (is_hrule(line)) {
            span_t s; s.type = span_type::hrule; s.text = "";
            spans.push_back(std::move(s));
            i = consumed_to;
            continue;
        }

        if (!trimmed.empty() && trimmed.front() == '#') {
            size_t h = 0;
            while (h < trimmed.size() && trimmed[h] == '#' && h < 6) ++h;
            if (h >= 1 && h <= 3 && h < trimmed.size() && trimmed[h] == ' ') {
                std::string body = strip_lr(trimmed.substr(h + 1));
                span_t s;
                if (h == 1)      s.type = span_type::heading1;
                else if (h == 2) s.type = span_type::heading2;
                else             s.type = span_type::heading3;
                s.depth = (int)h;
                s.text = body;
                spans.push_back(std::move(s));
                i = consumed_to;
                continue;
            }
        }

        if (!trimmed.empty() && trimmed.front() == '>') {
            std::string body = trimmed.size() > 1 ? trimmed.substr(1) : std::string();
            if (!body.empty() && body.front() == ' ') body.erase(body.begin());
            span_t s; s.type = span_type::blockquote; s.text = body;
            spans.push_back(std::move(s));
            i = consumed_to;
            continue;
        }

        if (trimmed.size() >= 6 &&
            (trimmed.compare(0, 6, "- [ ] ") == 0 || trimmed.compare(0, 6, "* [ ] ") == 0)) {
            span_t s; s.type = span_type::task_unchecked;
            s.text = trimmed.substr(6);
            s.list_indent = (int)(leading_spaces(line) / 2);
            spans.push_back(std::move(s));
            i = consumed_to;
            continue;
        }
        if (trimmed.size() >= 6 &&
            (trimmed.compare(0, 6, "- [x] ") == 0 || trimmed.compare(0, 6, "- [X] ") == 0 ||
             trimmed.compare(0, 6, "* [x] ") == 0 || trimmed.compare(0, 6, "* [X] ") == 0)) {
            span_t s; s.type = span_type::task_checked;
            s.text = trimmed.substr(6);
            s.list_indent = (int)(leading_spaces(line) / 2);
            spans.push_back(std::move(s));
            i = consumed_to;
            continue;
        }

        if (trimmed.size() >= 2 &&
            (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') &&
            trimmed[1] == ' ') {
            span_t s; s.type = span_type::list_bullet;
            s.text = trimmed.substr(2);
            s.list_indent = (int)(leading_spaces(line) / 2);
            spans.push_back(std::move(s));
            i = consumed_to;
            continue;
        }

        {
            size_t p = 0;
            while (p < trimmed.size() && is_digit_ch(trimmed[p])) ++p;
            if (p > 0 && p < trimmed.size() && (trimmed[p] == '.' || trimmed[p] == ')') &&
                p + 1 < trimmed.size() && trimmed[p + 1] == ' ') {
                int idx = 0;
                for (size_t k = 0; k < p; ++k) idx = idx * 10 + (trimmed[k] - '0');
                span_t s; s.type = span_type::list_numbered;
                s.list_index = idx;
                s.text = trimmed.substr(p + 2);
                s.list_indent = (int)(leading_spaces(line) / 2);
                spans.push_back(std::move(s));
                i = consumed_to;
                continue;
            }
        }

        if (trimmed.find('|') != std::string::npos) {
            size_t scan = consumed_to;
            size_t scan_end = scan;
            while (scan_end < n && text[scan_end] != '\n' && text[scan_end] != '\r') ++scan_end;
            std::string maybe_sep = text.substr(scan, scan_end - scan);
            if (looks_like_table_separator(maybe_sep)) {
                span_t s; s.type = span_type::table;
                s.table_data.push_back(split_pipe_row(line));
                size_t nxt = scan_end;
                if (nxt < n && text[nxt] == '\r') ++nxt;
                if (nxt < n && text[nxt] == '\n') ++nxt;
                while (nxt < n) {
                    size_t row_end = nxt;
                    while (row_end < n && text[row_end] != '\n' && text[row_end] != '\r') ++row_end;
                    std::string row_line = text.substr(nxt, row_end - nxt);
                    std::string row_trim = strip_lr(row_line);
                    if (row_trim.empty() || row_trim.find('|') == std::string::npos) break;
                    s.table_data.push_back(split_pipe_row(row_line));
                    nxt = row_end;
                    if (nxt < n && text[nxt] == '\r') ++nxt;
                    if (nxt < n && text[nxt] == '\n') ++nxt;
                }
                spans.push_back(std::move(s));
                i = nxt;
                continue;
            }
        }

        push_inline_run(line);
        if (consumed_to < n) {
            span_t br; br.type = span_type::text; br.text = "\n";
            spans.push_back(std::move(br));
        }
        i = consumed_to;
    }

    return spans;
}


syntax::language_def_t chat_render::chat_language_def(const std::string& lang)
{
    if (lang.empty() || lang == "cpp" || lang == "c++" || lang == "c" || lang == "h" || lang == "hpp")
        return syntax::lang_cpp();
    if (lang == "asm" || lang == "assembly" || lang == "x86" || lang == "nasm" || lang == "masm")
        return syntax::lang_asm();
    if (lang == "python" || lang == "py")
        return syntax::lang_python();
    if (lang == "json")
        return syntax::lang_json();
    if (lang == "lua")
        return syntax::lang_lua();
    return syntax::lang_cpp();
}
