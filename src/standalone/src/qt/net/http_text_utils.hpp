#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace aida::qt::net::http_text {

inline int utf8_decode_one(unsigned int& out_char, const char* in_text,
                           const char* in_text_end) noexcept
{
    static const unsigned char kLengths[32] = {
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 3, 3, 4, 0
    };
    static const unsigned int kMasks[5] = { 0x00u, 0x7fu, 0x1fu, 0x0fu, 0x07u };
    static const unsigned int kMins[5] = { 0x400000u, 0u, 0x80u, 0x800u, 0x10000u };
    static const int kShiftC[5] = { 0, 18, 12, 6, 0 };
    static const int kShiftE[5] = { 0, 6, 4, 2, 0 };
    const int len = kLengths[static_cast<unsigned char>(*in_text) >> 3];
    int wanted = len + (len != 0 ? 0 : 1);
    unsigned char s[4];
    s[0] = in_text + 0 < in_text_end ? static_cast<unsigned char>(in_text[0]) : 0;
    s[1] = in_text + 1 < in_text_end ? static_cast<unsigned char>(in_text[1]) : 0;
    s[2] = in_text + 2 < in_text_end ? static_cast<unsigned char>(in_text[2]) : 0;
    s[3] = in_text + 3 < in_text_end ? static_cast<unsigned char>(in_text[3]) : 0;
    unsigned int codepoint = (static_cast<unsigned int>(s[0]) & kMasks[len]) << 18;
    codepoint |= (static_cast<unsigned int>(s[1] & 0x3f)) << 12;
    codepoint |= (static_cast<unsigned int>(s[2] & 0x3f)) << 6;
    codepoint |= static_cast<unsigned int>(s[3] & 0x3f);
    codepoint >>= kShiftC[len];
    out_char = codepoint;
    int e = (codepoint < kMins[len]) << 6;
    e |= ((codepoint >> 11) == 0x1bu) << 7;
    e |= (codepoint > 0x10FFFFu) << 8;
    e |= (s[1] & 0xc0) >> 2;
    e |= (s[2] & 0xc0) >> 4;
    e |= (s[3]) >> 6;
    e ^= 0x2a;
    e >>= kShiftE[len];
    if (e != 0) {
        const int available = (s[0] != 0 ? 1 : 0) + (s[1] != 0 ? 1 : 0) +
            (s[2] != 0 ? 1 : 0) + (s[3] != 0 ? 1 : 0);
        wanted = (std::min)(wanted, available);
        out_char = 0xFFFDu;
    }
    return wanted;
}

inline bool contains_binary_bytes(std::string_view value) noexcept
{
    if (value.empty())
        return false;
    const char* cursor = value.data();
    const char* const end = cursor + value.size();
    while (cursor < end) {
        unsigned int codepoint = 0;
        const int consumed = utf8_decode_one(codepoint, cursor, end);
        if (consumed <= 0 || cursor + consumed > end ||
            codepoint == 0xFFFDu ||
            (codepoint < 0x20u && codepoint != '\r' && codepoint != '\n' &&
             codepoint != '\t') || codepoint == 0x7Fu)
            return true;
        cursor += consumed;
    }
    return false;
}

inline bool valid_header_name(std::string_view name) noexcept
{
    if (name.empty())
        return false;
    return std::all_of(name.begin(), name.end(), [](const unsigned char value) {
        return std::isalnum(value) != 0 || value == '!' || value == '#' || value == '$' ||
            value == '%' || value == '&' || value == '\'' || value == '*' || value == '+' ||
            value == '-' || value == '.' || value == '^' || value == '_' || value == '`' ||
            value == '|' || value == '~';
    });
}

inline bool validate_request_line(std::string_view line) noexcept
{
    const auto first = line.find(' ');
    if (first == std::string_view::npos || first == 0)
        return false;
    const auto second = line.find(' ', first + 1);
    if (second == std::string_view::npos || second == first + 1 || second + 1 >= line.size())
        return false;
    return line.find_first_of("\r\n", second + 1) == std::string_view::npos &&
        line.substr(second + 1, 5) == "HTTP/";
}

inline bool validate_headers(std::string_view headers, std::string& error)
{
    if (!headers.empty() && (headers.back() == '\r' || headers.back() == '\n')) {
        error = "Header fields must not include the blank-line body separator.";
        return false;
    }
    std::size_t offset = 0;
    while (offset < headers.size()) {
        const auto end = headers.find('\n', offset);
        const auto count = (end == std::string_view::npos ? headers.size() : end) - offset;
        std::string_view line = headers.substr(offset, count);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty()) {
            error = "Header fields cannot contain an embedded blank line.";
            return false;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || !valid_header_name(line.substr(0, colon))) {
            error = "Each header must use a valid HTTP field name followed by a colon.";
            return false;
        }
        if (end == std::string_view::npos)
            break;
        offset = end + 1;
    }
    return true;
}

struct parsed_request_t {
    std::string request_line;
    std::string headers;
    std::string body;
    std::string line_ending = "\r\n";
    bool ok = false;
    bool oversized = false;
    bool binary = false;
    std::string error;
};

inline parsed_request_t parse_pretty(std::string_view authority, std::size_t max_bytes)
{
    parsed_request_t parsed;
    parsed.oversized = authority.size() > max_bytes;
    parsed.binary = contains_binary_bytes(authority);
    if (parsed.oversized) {
        parsed.error = "The request exceeds this editor's bounded capacity.";
        return parsed;
    }
    if (parsed.binary) {
        parsed.error = "The request contains binary or invalid UTF-8 bytes and cannot be represented by the text editor.";
        return parsed;
    }
    const auto crlf_boundary = authority.find("\r\n\r\n");
    const auto lf_boundary = authority.find("\n\n");
    const bool use_crlf = crlf_boundary != std::string_view::npos &&
        (lf_boundary == std::string_view::npos || crlf_boundary <= lf_boundary);
    const auto boundary = use_crlf ? crlf_boundary : lf_boundary;
    const std::size_t separator_size = use_crlf ? 4U : 2U;
    const auto header_section = boundary == std::string_view::npos
        ? authority : authority.substr(0, boundary);
    parsed.line_ending = use_crlf ? "\r\n" : "\n";
    const auto first_line_end = header_section.find(parsed.line_ending);
    const auto request_line = first_line_end == std::string_view::npos
        ? header_section : header_section.substr(0, first_line_end);
    const auto headers = first_line_end == std::string_view::npos
        ? std::string_view{} : header_section.substr(first_line_end + parsed.line_ending.size());
    const auto body = boundary == std::string_view::npos
        ? std::string_view{} : authority.substr(boundary + separator_size);
    if (!validate_request_line(request_line)) {
        parsed.error = "The raw request does not contain a valid HTTP request line.";
        return parsed;
    }
    if (!validate_headers(headers, parsed.error))
        return parsed;
    parsed.request_line.assign(request_line);
    parsed.headers.assign(headers);
    parsed.body.assign(body);
    parsed.ok = true;
    return parsed;
}

inline std::uint64_t fnv1a64(const void* data, std::size_t size) noexcept
{
    std::uint64_t hash = 14695981039346656037ULL;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<std::uint64_t>(size);
    hash *= 1099511628211ULL;
    return hash == 0 ? 1 : hash;
}

inline std::uint64_t fnv1a64(std::string_view text) noexcept
{
    return fnv1a64(text.data(), text.size());
}

inline std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes) noexcept
{
    return fnv1a64(bytes.data(), bytes.size());
}

}
