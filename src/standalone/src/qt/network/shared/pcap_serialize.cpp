#include "qt/network/shared/pcap_serialize.hpp"

#include "core/network/flow_serializer.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace aida::qt::net {

namespace {

std::atomic<std::uint64_t> g_export_temp_sequence{1};

template <typename Value>
bool append_export_value(std::vector<std::uint8_t>& output, const Value& value) {
    if (output.size() > k_network_export_limit - sizeof(Value))
        return false;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    output.insert(output.end(), bytes, bytes + sizeof(Value));
    return true;
}

bool append_export_bytes(std::vector<std::uint8_t>& output, const std::uint8_t* data,
                         std::size_t size) {
    if ((size != 0 && !data) || size > k_network_export_limit ||
        output.size() > k_network_export_limit - size)
        return false;
    if (size == 0)
        return true;
    output.insert(output.end(), data, data + size);
    return true;
}

}

bool serialize_pcap(const std::vector<network_view::packet_entry_t>& packets,
                    std::uint32_t filter_pid, std::uint8_t filter_protocol,
                    std::vector<std::uint8_t>& output, std::uint32_t& written,
                    std::string& error) {
    output.clear();
    output.reserve((std::min)(k_network_export_limit, static_cast<std::size_t>(24 + packets.size() * 128)));
    const std::uint32_t magic = 0xa1b2c3d4;
    const std::uint16_t major = 2;
    const std::uint16_t minor = 4;
    const std::int32_t timezone = 0;
    const std::uint32_t sigfigs = 0;
    const std::uint32_t snaplen = 65535;
    const std::uint32_t linktype = 1;
    if (!append_export_value(output, magic) || !append_export_value(output, major) ||
        !append_export_value(output, minor) || !append_export_value(output, timezone) ||
        !append_export_value(output, sigfigs) || !append_export_value(output, snaplen) ||
        !append_export_value(output, linktype)) {
        error = "PCAP header exceeds export limit";
        return false;
    }
    written = 0;
    for (const auto& packet : packets) {
        if (filter_pid != 0 && packet.pid != filter_pid)
            continue;
        if (filter_protocol != 0 && packet.protocol != filter_protocol)
            continue;
        const std::uint32_t transport_size = packet.protocol == 6 ? 20U : 8U;
        const std::size_t protocol_payload_limit = 65535U - 20U - transport_size;
        const std::size_t payload_size = (std::min)({packet.payload.size(),
            static_cast<std::size_t>(packet.payload_size), protocol_payload_limit});
        const std::uint32_t ip_size = static_cast<std::uint32_t>(20U + transport_size + payload_size);
        const std::uint32_t frame_size = 14U + ip_size;
        if (output.size() > k_network_export_limit - 16U - frame_size) {
            error = "PCAP export exceeds the 256 MiB safety limit";
            return false;
        }
        std::vector<std::uint8_t> frame(frame_size, 0);
        frame[12] = 0x08;
        frame[13] = 0x00;
        std::uint8_t* ip = frame.data() + 14;
        ip[0] = 0x45;
        ip[2] = static_cast<std::uint8_t>((ip_size >> 8) & 0xff);
        ip[3] = static_cast<std::uint8_t>(ip_size & 0xff);
        ip[8] = 64;
        ip[9] = packet.protocol;
        if (packet.direction == 1) {
            std::memcpy(ip + 12, packet.src_addr, 4);
            std::memcpy(ip + 16, packet.dst_addr, 4);
        } else {
            std::memcpy(ip + 12, packet.dst_addr, 4);
            std::memcpy(ip + 16, packet.src_addr, 4);
        }
        std::uint32_t checksum = 0;
        for (int index = 0; index < 20; index += 2)
            checksum += (static_cast<std::uint32_t>(ip[index]) << 8) | ip[index + 1];
        while (checksum >> 16)
            checksum = (checksum & 0xffff) + (checksum >> 16);
        const std::uint16_t ip_checksum = static_cast<std::uint16_t>(~checksum);
        ip[10] = static_cast<std::uint8_t>(ip_checksum >> 8);
        ip[11] = static_cast<std::uint8_t>(ip_checksum & 0xff);
        std::uint8_t* transport = ip + 20;
        const std::uint16_t source_port = packet.direction == 1 ? packet.src_port : packet.dst_port;
        const std::uint16_t destination_port = packet.direction == 1 ? packet.dst_port : packet.src_port;
        transport[0] = static_cast<std::uint8_t>(source_port >> 8);
        transport[1] = static_cast<std::uint8_t>(source_port & 0xff);
        transport[2] = static_cast<std::uint8_t>(destination_port >> 8);
        transport[3] = static_cast<std::uint8_t>(destination_port & 0xff);
        if (packet.protocol == 6) {
            transport[12] = 0x50;
            transport[13] = 0x18;
        } else {
            const std::uint16_t udp_size = static_cast<std::uint16_t>(8U + payload_size);
            transport[4] = static_cast<std::uint8_t>(udp_size >> 8);
            transport[5] = static_cast<std::uint8_t>(udp_size & 0xff);
        }
        if (payload_size != 0)
            std::memcpy(transport + transport_size, packet.payload.data(), payload_size);
        const std::uint32_t seconds = static_cast<std::uint32_t>(packet.timestamp / 1000);
        const std::uint32_t microseconds = static_cast<std::uint32_t>((packet.timestamp % 1000) * 1000);
        if (!append_export_value(output, seconds) || !append_export_value(output, microseconds) ||
            !append_export_value(output, frame_size) || !append_export_value(output, frame_size) ||
            !append_export_bytes(output, frame.data(), frame.size())) {
            error = "PCAP export exceeds the 256 MiB safety limit";
            return false;
        }
        ++written;
    }
    return true;
}

bool atomic_write_export(const std::string& destination, const std::uint8_t* data,
                         std::size_t size, std::string& error) {
    if (destination.empty() || (size != 0 && !data) || size > k_network_export_limit) {
        error = "Export destination or payload is invalid";
        return false;
    }
    const std::string temporary = destination + ".aida-tmp-" +
        std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(g_export_temp_sequence.fetch_add(1, std::memory_order_acq_rel));
    HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Cannot create temporary export file (Win32 " + std::to_string(GetLastError()) + ")";
        return false;
    }
    bool success = true;
    std::size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>((std::min)(size - offset, static_cast<std::size_t>(1024 * 1024)));
        DWORD wrote = 0;
        if (!WriteFile(file, data + offset, chunk, &wrote, nullptr) || wrote != chunk) {
            error = "Export write failed or was partial (Win32 " + std::to_string(GetLastError()) + ")";
            success = false;
            break;
        }
        offset += wrote;
    }
    LARGE_INTEGER exact_size{};
    if (success && (!FlushFileBuffers(file) || !GetFileSizeEx(file, &exact_size) ||
                    exact_size.QuadPart != static_cast<LONGLONG>(size))) {
        error = "Export flush or size verification failed (Win32 " + std::to_string(GetLastError()) + ")";
        success = false;
    }
    CloseHandle(file);
    if (success && !MoveFileExA(temporary.c_str(), destination.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Atomic export replacement failed (Win32 " + std::to_string(GetLastError()) + ")";
        success = false;
    }
    if (!success)
        DeleteFileA(temporary.c_str());
    return success;
}

bool serialize_har_bounded(const std::vector<mitm_proxy::http_exchange>& history,
                           std::string& serialized, std::string& error) {
    if (history.size() > 4096) {
        error = "HAR history exceeds the 4096-exchange safety limit";
        return false;
    }
    std::size_t source_bytes = 0;
    for (const auto& exchange : history) {
        const std::size_t exchange_bytes = exchange.raw_request.size() + exchange.raw_response.size();
        if (exchange_bytes > k_network_export_limit || source_bytes > k_network_export_limit - exchange_bytes) {
            error = "HAR source history exceeds the 256 MiB safety limit";
            return false;
        }
        source_bytes += exchange_bytes;
    }
    try {
        serialized = flow_serializer::export_har_1_2(history).dump(2);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    if (serialized.size() > k_network_export_limit) {
        serialized.clear();
        error = "Serialized HAR exceeds the 256 MiB safety limit";
        return false;
    }
    return true;
}

}
