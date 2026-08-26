#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/network/mitm_proxy.hpp"
#include "core/network/network_view.hpp"

namespace aida::qt::net {

inline constexpr std::size_t k_network_export_limit = 256ULL * 1024ULL * 1024ULL;

bool serialize_pcap(const std::vector<network_view::packet_entry_t>& packets,
                    std::uint32_t filter_pid, std::uint8_t filter_protocol,
                    std::vector<std::uint8_t>& output, std::uint32_t& written,
                    std::string& error);
bool serialize_har_bounded(const std::vector<mitm_proxy::http_exchange>& history,
                           std::string& serialized, std::string& error);
bool atomic_write_export(const std::string& destination, const std::uint8_t* data,
                         std::size_t size, std::string& error);

}
