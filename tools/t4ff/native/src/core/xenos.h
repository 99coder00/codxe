#pragma once

#include <cstdint>
#include <vector>

#include "core/texture_format.h"

// Xbox 360 (Xenos) textures: formats, tiling, endian swapping and texture headers (the Python
// t4ff's xenos.py; the tiling math is CoD Xe's src/image/xenos_texture.cpp).
namespace t4ff::xenos
{
constexpr uint32_t GPUENDIAN_NONE = 0;
constexpr uint32_t GPUENDIAN_8IN16 = 1;
constexpr uint32_t GPUENDIAN_8IN32 = 2;
constexpr uint32_t GPUENDIAN_16IN32 = 3;

constexpr uint32_t GPUDIMENSION_2D = 1;
constexpr uint32_t GPUDIMENSION_CUBEMAP = 3;

constexpr int D3DFMT_SWIZZLE_SHIFT = 18;

struct Format
{
    Fmt name;
    uint32_t gpu;
    uint32_t d3d;
    uint32_t endian;
    uint32_t block;           // block width and height in texels
    uint32_t bytes_per_block;
    uint32_t swizzle;         // dword 3 swizzle bits (XYZW)
};

// The console format of a pixel format (nullptr when the console has none).
const Format *format(Fmt f);
// The format of a D3DFORMAT value, whatever swizzle it names.
const Format *format_of_d3d(uint32_t d3d);

uint32_t pitch_units(uint32_t width, const Format &fmt);

struct LevelLayout
{
    uint32_t wb, hb;             // blocks of the level
    uint32_t stored_w, stored_h; // blocks stored
    uint32_t size;               // bytes, 4 KiB aligned
};
LevelLayout level_layout(uint32_t width, uint32_t height, int level, const Format &fmt);

void endian_swap(uint8_t *data, size_t size, uint32_t endian);

// One mip level of linear (PC order) data tiled into Xenos memory order (endian swapped).
std::vector<uint8_t> tile_level(const uint8_t *linear, size_t size, uint32_t width, uint32_t height, int level, const Format &fmt);
// The inverse: linear PC order data.
std::vector<uint8_t> untile_level(const uint8_t *tiled, size_t size, uint32_t width, uint32_t height, int level, const Format &fmt);

// GPU texture fetch constant (6 dwords, little endian as stored in T4 zones).
void fetch_constant(uint8_t out[24], uint32_t width, uint32_t height, const Format &fmt, int levels, bool tiled = true,
                    uint32_t mip_address = 0, uint32_t dimension = GPUDIMENSION_2D);
// D3DBaseTexture (52 bytes, little endian) as stored in T4 360 zones.
std::vector<uint8_t> texture_header(uint32_t width, uint32_t height, const Format &fmt, int levels, uint32_t mip_address = 0,
                                    uint32_t dimension = GPUDIMENSION_2D);

int packed_mip_level(uint32_t width, uint32_t height);

struct Placement
{
    int level;
    uint32_t region;   // byte offset of the level's region
    uint32_t stored_w; // stored width of the region in blocks
    uint32_t bx, by;   // block offset inside the region (the packed mip tail)
    uint32_t face_stride;
};
struct MipChainLayout
{
    uint32_t base_size;
    std::vector<Placement> placements;
    uint32_t total;
};
// Where every level of a mip chain goes (see xenos.py's mip_chain_layout).
MipChainLayout mip_chain_layout(uint32_t width, uint32_t height, const Format &fmt, int levels, int faces = 1);

// A mip chain (linear PC data, largest level first; each level holds its faces one after the other)
// tiled. The base level must be larger than 16 texels in both dimensions when more than one level is
// given.
std::vector<uint8_t> tile_mip_chain(const std::vector<std::vector<uint8_t>> &levels, uint32_t width, uint32_t height, const Format &fmt,
                                    int faces = 1);
std::vector<std::vector<uint8_t>> untile_mip_chain(const uint8_t *data, size_t size, uint32_t width, uint32_t height, const Format &fmt,
                                                   int levels, int faces = 1);

// D3DBaseTexture360 for a texture with a mip chain (mip address relative to the base) or a cube map.
std::vector<uint8_t> texture_header_mips(uint32_t width, uint32_t height, const Format &fmt, int levels, int faces = 1);
} // namespace t4ff::xenos
