#include "qt/network/shared/network_format.hpp"

#include <QColor>
#include <QString>

#include "qt/theme/aida_tokens.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace aida::qt::net {

std::string format_ip(const std::uint8_t* addr, std::uint8_t af) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (af == 2) {
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
            static_cast<unsigned>(addr[0]), static_cast<unsigned>(addr[1]),
            static_cast<unsigned>(addr[2]), static_cast<unsigned>(addr[3]));
    } else if (af == 23) {
        inet_ntop(AF_INET6, addr, buf, sizeof(buf));
    }
    return buf;
}

const char* protocol_name(std::uint8_t proto) {
    switch (proto) {
        case 6:  return "TCP";
        case 17: return "UDP";
        default: return "???";
    }
}

const char* tcp_state_name(std::uint8_t state) {
    static const char* names[] = {
        "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD",
        "ESTABLISHED", "FIN_WAIT1", "FIN_WAIT2", "CLOSE_WAIT",
        "CLOSING", "LAST_ACK", "TIME_WAIT", "DELETE_TCB"
    };
    if (state < 12) return names[state];
    return "UNKNOWN";
}

std::string format_bytes(std::uint64_t bytes) {
    char buf[64];
    if (bytes < 1024)
        snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else if (bytes < 1024ULL * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::string format_rate(float bytes_per_sec) {
    if (bytes_per_sec < 1024.f) return format_bytes(static_cast<std::uint64_t>(bytes_per_sec)) + "/s";
    return format_bytes(static_cast<std::uint64_t>(bytes_per_sec)) + "/s";
}

std::string format_timestamp(std::uint64_t ts) {
    std::uint64_t sec = ts / 1000;
    std::uint64_t ms = ts % 1000;
    std::uint64_t h = (sec / 3600) % 24;
    std::uint64_t m = (sec / 60) % 60;
    std::uint64_t s = sec % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu.%03llu",
        static_cast<unsigned long long>(h), static_cast<unsigned long long>(m),
        static_cast<unsigned long long>(s), static_cast<unsigned long long>(ms));
    return buf;
}

bool filter_text_match(const char* filter, const std::string& text) {
    if (!filter || !filter[0]) return true;

    std::string lower_filter(filter);
    std::string lower_text = text;
    for (auto& c : lower_filter) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (auto& c : lower_text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return lower_text.find(lower_filter) != std::string::npos;
}

bool filter_text_match(const QString& filter, const std::string& text) {
    if (filter.isEmpty()) return true;
    return filter_text_match(filter.toUtf8().constData(), text);
}

std::string payload_display_text(const std::vector<std::uint8_t>& bytes, std::size_t cap) {
    std::string out;
    std::size_t n = std::min(bytes.size(), cap);
    out.reserve(n + 64);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t b = bytes[i];
        if (b == '\r' || b == '\n' || b == '\t' || (b >= 32 && b < 127))
            out.push_back(static_cast<char>(b));
        else
            out.push_back('.');
    }
    if (bytes.size() > n) {
        out += "\n[truncated ";
        out += std::to_string(bytes.size() - n);
        out += " bytes]";
    }
    return out;
}

std::string capture_row_info_text(const std::string& summary) {
    if (summary.size() <= 200)
        return summary;
    return summary.substr(0, 197) + "...";
}

std::uint64_t network_now_ms() {
    return static_cast<std::uint64_t>(GetTickCount64());
}

net_semantic_t tcp_state_semantic(std::uint8_t state) noexcept {
    switch (state) {
        case 4:  return net_semantic_t::success;
        case 1:  return net_semantic_t::info;
        case 2:  return net_semantic_t::accent;
        case 3:  return net_semantic_t::accent;
        case 5:  return net_semantic_t::warning;
        case 6:  return net_semantic_t::warning;
        case 7:  return net_semantic_t::warning;
        case 8:  return net_semantic_t::warning;
        case 9:  return net_semantic_t::warning;
        case 10: return net_semantic_t::info;
        case 0:  return net_semantic_t::error;
        case 11: return net_semantic_t::error;
        default: return net_semantic_t::neutral;
    }
}

net_semantic_t status_code_semantic(int code) noexcept {
    if (code >= 200 && code < 300) return net_semantic_t::success;
    if (code >= 300 && code < 400) return net_semantic_t::info;
    if (code >= 400 && code < 500) return net_semantic_t::warning;
    if (code >= 500)               return net_semantic_t::error;
    return net_semantic_t::neutral;
}

net_semantic_t protocol_label_semantic(const std::string& label) noexcept {
    if (label == "HTTP") return net_semantic_t::info;
    if (label == "TLS")  return net_semantic_t::success;
    if (label == "DNS")  return net_semantic_t::warning;
    if (label == "QUIC") return net_semantic_t::accent;
    if (label == "UDP")  return net_semantic_t::info;
    return net_semantic_t::neutral;
}

QColor http_method_color(const char* method) {
    const auto& t = aida::qt::theme::tokens();
    if (!method || !method[0]) return t.text_secondary;
    const char c0 = method[0];
    const char c1 = method[1] ? method[1] : 0;
    if (c0 == 'G') return t.success;
    if (c0 == 'P' && c1 == 'O') return t.info;
    if (c0 == 'P' && c1 == 'U') return t.warning;
    if (c0 == 'P' && c1 == 'A') return t.syn_keyword;
    if (c0 == 'D') return t.error;
    if (c0 == 'H') return t.text_secondary;
    if (c0 == 'O') return t.info;
    return t.text_secondary;
}

QColor status_code_color(int code) {
    const auto& t = aida::qt::theme::tokens();
    if (code >= 200 && code < 300) return t.success;
    if (code >= 300 && code < 400) return t.info;
    if (code >= 400 && code < 500) return t.warning;
    if (code >= 500)               return t.error;
    return t.text_dim;
}

const char* dns_query_type_name(std::uint16_t query_type) noexcept {
    switch (query_type) {
        case 1:  return "A";
        case 28: return "AAAA";
        case 5:  return "CNAME";
        default: return "?";
    }
}

}
