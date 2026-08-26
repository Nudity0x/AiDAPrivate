#pragma once

#include "core/disasm/disasm_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace aida::qt::disasm {

inline constexpr std::uint64_t k_presentation_scroll_milli_read_cap =
    1000000000000000ULL;
inline constexpr float k_presentation_scroll_write_cap = 1000000000000.0f;

inline std::string encode_presentation_line(
    const disasm_view::presentation_snapshot_t& snapshot)
{
    const auto selection = snapshot.selection.value_or(aida::analysis::address_t{});
    const auto scroll_milli = static_cast<unsigned long long>(
        std::llround((std::clamp)(snapshot.scroll_y, 0.0f,
            k_presentation_scroll_write_cap) * 1000.0f));
    char line[256]{};
    std::snprintf(line, sizeof(line),
        "Presentation=%d,%d,%d,%d,%llu,%d,%d,%llu,%d,%d,%llu",
        static_cast<int>(snapshot.addr_format), snapshot.show_bytes ? 1 : 0,
        snapshot.active_section, snapshot.display_image_base ? 1 : 0,
        static_cast<unsigned long long>(snapshot.display_image_base.value_or(0)),
        snapshot.selection ? 1 : 0, static_cast<int>(selection.space),
        static_cast<unsigned long long>(selection.value),
        static_cast<int>(selection.architecture),
        static_cast<int>(selection.mode), scroll_milli);
    return line;
}

inline bool decode_presentation_line(const std::string& line,
                                     disasm_view::presentation_snapshot_t& snapshot)
{
    int format = 0;
    int show_bytes = 0;
    int section = -1;
    int has_base = 0;
    unsigned long long base = 0;
    int has_selection = 0;
    int space = 0;
    unsigned long long address = 0;
    int architecture = 0;
    int mode = 0;
    unsigned long long scroll_milli = 0;
    if (std::sscanf(line.c_str(),
            "Presentation=%d,%d,%d,%d,%llu,%d,%d,%llu,%d,%d,%llu",
            &format, &show_bytes, &section, &has_base, &base,
            &has_selection, &space, &address, &architecture, &mode,
            &scroll_milli) != 11)
        return false;
    if (format < static_cast<int>(disasm_view::addr_format_t::va) ||
        format > static_cast<int>(disasm_view::addr_format_t::file_offset))
        return false;
    snapshot.addr_format = static_cast<disasm_view::addr_format_t>(format);
    snapshot.show_bytes = show_bytes != 0;
    snapshot.active_section = section;
    snapshot.scroll_y = static_cast<float>(
        (std::min)(scroll_milli, k_presentation_scroll_milli_read_cap)) / 1000.0f;
    snapshot.display_image_base = has_base != 0
        ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(base))
        : std::nullopt;
    if (has_selection != 0) {
        aida::analysis::address_t selection;
        selection.space = static_cast<aida::analysis::address_space_id_t>(space);
        selection.value = static_cast<std::uint64_t>(address);
        selection.architecture =
            static_cast<aida::analysis::architecture_id_t>(architecture);
        selection.mode = static_cast<aida::analysis::architecture_mode_t>(mode);
        snapshot.selection = selection;
    } else {
        snapshot.selection.reset();
    }
    return true;
}

}
