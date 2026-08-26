#pragma once

#include <cstdint>

namespace aida::analysis_bridge {

inline constexpr std::uint64_t k_golden_gamma = 0x9E3779B97F4A7C15ull;

inline std::uint64_t combine_generation_revision(std::uint64_t generation,
                                                 std::uint64_t analysis_revision)
{
    return generation ^ (analysis_revision + k_golden_gamma + (generation << 6u) +
        (generation >> 2u));
}

inline std::uint64_t combine_extend(std::uint64_t seed, std::uint64_t value)
{
    return seed ^ (value + k_golden_gamma + (seed << 6u) + (seed >> 2u));
}

inline std::uint64_t format_page_key(std::uint64_t generation,
                                     std::uint64_t analysis_revision,
                                     std::uint64_t overlay_revision,
                                     std::uint64_t begin, std::uint64_t end)
{
    std::uint64_t value = generation;
    value ^= analysis_revision * 0x9E3779B185EBCA87ULL;
    value ^= overlay_revision * 0xC2B2AE3D27D4EB4FULL;
    value ^= begin * 0x165667B19E3779F9ULL;
    value ^= end * 0x85EBCA77C2B2AE63ULL;
    return value;
}

}
