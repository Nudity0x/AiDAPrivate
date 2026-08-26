#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class QString;
class QColor;

namespace aida::qt::net {

std::string format_ip(const std::uint8_t* addr, std::uint8_t af);
const char* protocol_name(std::uint8_t proto);
const char* tcp_state_name(std::uint8_t state);
std::string format_bytes(std::uint64_t bytes);
std::string format_rate(float bytes_per_sec);
std::string format_timestamp(std::uint64_t ts);
bool filter_text_match(const char* filter, const std::string& text);
bool filter_text_match(const QString& filter, const std::string& text);
std::string payload_display_text(const std::vector<std::uint8_t>& bytes,
                                 std::size_t cap = 262144);
std::string capture_row_info_text(const std::string& summary);
std::uint64_t network_now_ms();

enum class net_semantic_t : std::uint8_t {
    neutral,
    success,
    warning,
    error,
    info,
    accent
};

net_semantic_t tcp_state_semantic(std::uint8_t state) noexcept;
net_semantic_t status_code_semantic(int code) noexcept;
net_semantic_t protocol_label_semantic(const std::string& label) noexcept;
QColor http_method_color(const char* method);
QColor status_code_color(int code);
const char* dns_query_type_name(std::uint16_t query_type) noexcept;

}
