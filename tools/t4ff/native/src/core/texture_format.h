#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace t4ff
{
// Pixel formats of PC and console images (the Python t4ff's format names).
enum class Fmt : uint8_t
{
    A8R8G8B8,
    X8R8G8B8,
    R8G8B8,
    A8L8,
    L8,
    A8,
    DXT1,
    DXT3,
    DXT5,
    DXN,
};

const char *fmt_name(Fmt f);
std::optional<Fmt> fmt_from_name(std::string_view name);

inline bool is_block_compressed(Fmt f)
{
    return f == Fmt::DXT1 || f == Fmt::DXT3 || f == Fmt::DXT5 || f == Fmt::DXN;
}

// Bytes of one level (of one face).
uint32_t level_size(Fmt f, uint32_t width, uint32_t height);

// Levels of a full mip chain down to 1x1.
int mip_count(uint32_t width, uint32_t height);
} // namespace t4ff
