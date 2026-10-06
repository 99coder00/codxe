#include "core/xenos.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace t4ff
{
namespace
{
const char *FMT_NAMES[] = {"A8R8G8B8", "X8R8G8B8", "R8G8B8", "A8L8", "L8", "A8", "DXT1", "DXT3", "DXT5", "DXN"};
} // namespace

const char *fmt_name(Fmt f)
{
    return FMT_NAMES[static_cast<int>(f)];
}

std::optional<Fmt> fmt_from_name(std::string_view name)
{
    for (int i = 0; i < static_cast<int>(std::size(FMT_NAMES)); ++i)
        if (name == FMT_NAMES[i])
            return static_cast<Fmt>(i);
    return std::nullopt;
}

uint32_t level_size(Fmt f, uint32_t width, uint32_t height)
{
    uint32_t bw = std::max(1u, (width + 3) / 4), bh = std::max(1u, (height + 3) / 4);
    switch (f)
    {
    case Fmt::DXT1: return bw * bh * 8;
    case Fmt::DXT3:
    case Fmt::DXT5:
    case Fmt::DXN: return bw * bh * 16;
    case Fmt::A8R8G8B8:
    case Fmt::X8R8G8B8: return width * height * 4;
    case Fmt::R8G8B8: return width * height * 3;
    case Fmt::A8L8: return width * height * 2;
    case Fmt::L8:
    case Fmt::A8: return width * height;
    }
    return 0;
}

int mip_count(uint32_t width, uint32_t height)
{
    int n = 1;
    while (width > 1 || height > 1)
    {
        width = std::max(width >> 1, 1u);
        height = std::max(height >> 1, 1u);
        ++n;
    }
    return n;
}
} // namespace t4ff

namespace t4ff::xenos
{
namespace
{
constexpr uint32_t GPUTEXTUREFORMAT_8 = 2;
constexpr uint32_t GPUTEXTUREFORMAT_8_8_8_8 = 6;
constexpr uint32_t GPUTEXTUREFORMAT_8_8 = 10;
constexpr uint32_t GPUTEXTUREFORMAT_DXT1 = 18;
constexpr uint32_t GPUTEXTUREFORMAT_DXT2_3 = 19;
constexpr uint32_t GPUTEXTUREFORMAT_DXT4_5 = 20;
constexpr uint32_t GPUTEXTUREFORMAT_DXN = 49;

// Swizzles as the SDK's D3DFORMATs hold them (see xenos.py)
const Format FORMATS[] = {
    {Fmt::DXT1, GPUTEXTUREFORMAT_DXT1, 0x1A200152, GPUENDIAN_8IN16, 4, 8, 0x688},
    {Fmt::DXT3, GPUTEXTUREFORMAT_DXT2_3, 0x1A200153, GPUENDIAN_8IN16, 4, 16, 0x688},
    {Fmt::DXT5, GPUTEXTUREFORMAT_DXT4_5, 0x1A200154, GPUENDIAN_8IN16, 4, 16, 0x688},
    {Fmt::DXN, GPUTEXTUREFORMAT_DXN, 0x1A200171, GPUENDIAN_8IN16, 4, 16, 0x688},
    {Fmt::A8R8G8B8, GPUTEXTUREFORMAT_8_8_8_8, 0x18280186, GPUENDIAN_8IN32, 1, 4, 0x60A},
    {Fmt::L8, GPUTEXTUREFORMAT_8, 0x28000102, GPUENDIAN_NONE, 1, 1, 0xA00},
    {Fmt::A8L8, GPUTEXTUREFORMAT_8_8, 0x0800014A, GPUENDIAN_8IN16, 1, 2, 0x200},
};

uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

uint32_t next_pow2(uint32_t value)
{
    uint32_t result = 1;
    while (result < value)
        result <<= 1;
    return result;
}

int log2_bpb(uint32_t bpb)
{
    return static_cast<int>((bpb / 4) + ((bpb / 2) >> (bpb / 4)));
}

int log2_ceil(uint32_t value)
{
    int bits = 0;
    for (uint32_t v = value - 1; v; v >>= 1)
        ++bits;
    return bits;
}

// Tiled block index of block (x, y) of a level stored stored_w blocks wide.
int64_t tiled_index(int64_t x, int64_t y, uint32_t stored_w, int log2)
{
    int64_t macro = ((y / 32) * (stored_w / 32)) << (log2 + 7);
    int64_t micro = ((y & 6) << 2) << log2;
    int64_t row = macro + ((micro & ~int64_t(0xF)) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2)) + ((y & 1) << 4);
    macro = (x / 32) << (log2 + 7);
    micro = (x & 7) << log2;
    int64_t offset = row + macro + ((micro & ~int64_t(0xF)) << 1) + (micro & 0xF);
    int64_t tiled = ((offset & ~int64_t(0x1FF)) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
                    (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
    return tiled >> log2;
}

void level_blocks(uint32_t width, uint32_t height, int level, const Format &fmt, uint32_t &wb, uint32_t &hb)
{
    uint32_t mw = std::max(level < 32 ? width >> level : 0u, 1u);
    uint32_t mh = std::max(level < 32 ? height >> level : 0u, 1u);
    wb = std::max(1u, (mw + fmt.block - 1) / fmt.block);
    hb = std::max(1u, (mh + fmt.block - 1) / fmt.block);
}

// Calls copy(tiled block offset, linear block offset) for the wb x hb blocks of a level placed at
// (bx, by) in a region stored_w blocks wide and tiled_size bytes long.
template <typename F>
void each_block(size_t tiled_size, uint32_t wb, uint32_t hb, uint32_t bx, uint32_t by, uint32_t stored_w, uint32_t bpb, F copy)
{
    int log2 = log2_bpb(bpb);
    size_t blocks = tiled_size / bpb;
    for (uint32_t y = 0; y < hb; ++y)
    {
        for (uint32_t x = 0; x < wb; ++x)
        {
            int64_t t = tiled_index(x + bx, y + by, stored_w, log2);
            if (t < 0 || static_cast<size_t>(t) >= blocks)
                throw std::runtime_error("tiled block outside the level");
            copy(static_cast<size_t>(t) * bpb, (static_cast<size_t>(y) * wb + x) * bpb);
        }
    }
}

void tile_blocks(uint8_t *tiled, size_t tiled_size, const uint8_t *linear, uint32_t wb, uint32_t hb, uint32_t bx, uint32_t by,
                 uint32_t stored_w, uint32_t bpb)
{
    each_block(tiled_size, wb, hb, bx, by, stored_w, bpb, [&](size_t t, size_t l) { std::memcpy(tiled + t, linear + l, bpb); });
}

void untile_blocks(const uint8_t *tiled, size_t tiled_size, uint8_t *linear, uint32_t wb, uint32_t hb, uint32_t bx, uint32_t by,
                   uint32_t stored_w, uint32_t bpb)
{
    each_block(tiled_size, wb, hb, bx, by, stored_w, bpb, [&](size_t t, size_t l) { std::memcpy(linear + l, tiled + t, bpb); });
}
} // namespace

const Format *format(Fmt f)
{
    for (const Format &fmt : FORMATS)
        if (fmt.name == f)
            return &fmt;
    return nullptr;
}

const Format *format_of_d3d(uint32_t d3d)
{
    const uint32_t mask = (1u << D3DFMT_SWIZZLE_SHIFT) - 1;
    for (const Format &fmt : FORMATS)
        if ((fmt.d3d & mask) == (d3d & mask))
            return &fmt;
    return nullptr;
}

uint32_t pitch_units(uint32_t width, const Format &fmt)
{
    if (fmt.block > 1)
    {
        uint32_t blocks = std::max(1u, (width + fmt.block - 1) / fmt.block);
        return align_up(blocks, 32) / 8;
    }
    return align_up(width, 32) / 32;
}

LevelLayout level_layout(uint32_t width, uint32_t height, int level, const Format &fmt)
{
    LevelLayout l{};
    level_blocks(width, height, level, fmt, l.wb, l.hb);
    uint32_t row_pitch;
    if (level == 0)
    {
        uint32_t row_pitch_texels = pitch_units(width, fmt) << 5;
        row_pitch = std::max(1u, (row_pitch_texels + fmt.block - 1) / fmt.block) * fmt.bytes_per_block;
        l.stored_w = row_pitch / fmt.bytes_per_block;
        l.stored_h = align_up(l.hb, 32);
    }
    else
    {
        uint32_t mw = std::max(level < 32 ? next_pow2(width) >> level : 0u, 1u);
        uint32_t mh = std::max(level < 32 ? next_pow2(height) >> level : 0u, 1u);
        l.stored_w = align_up((mw + fmt.block - 1) / fmt.block, 32);
        l.stored_h = align_up((mh + fmt.block - 1) / fmt.block, 32);
        row_pitch = l.stored_w * fmt.bytes_per_block;
    }
    l.size = align_up(row_pitch * l.stored_h, 4096);
    return l;
}

void endian_swap(uint8_t *data, size_t size, uint32_t endian)
{
    switch (endian)
    {
    case GPUENDIAN_NONE: return;
    case GPUENDIAN_8IN16:
        if (size % 2)
            throw std::runtime_error("endian swap of an odd size");
        for (size_t i = 0; i < size; i += 2)
            std::swap(data[i], data[i + 1]);
        return;
    case GPUENDIAN_8IN32:
        if (size % 4)
            throw std::runtime_error("endian swap of a size not a multiple of 4");
        for (size_t i = 0; i < size; i += 4)
        {
            std::swap(data[i], data[i + 3]);
            std::swap(data[i + 1], data[i + 2]);
        }
        return;
    case GPUENDIAN_16IN32:
        if (size % 4)
            throw std::runtime_error("endian swap of a size not a multiple of 4");
        for (size_t i = 0; i < size; i += 4)
        {
            std::swap(data[i], data[i + 2]);
            std::swap(data[i + 1], data[i + 3]);
        }
        return;
    }
    throw std::runtime_error("unknown endian swap");
}

std::vector<uint8_t> tile_level(const uint8_t *linear, size_t size, uint32_t width, uint32_t height, int level, const Format &fmt)
{
    LevelLayout l = level_layout(width, height, level, fmt);
    if (size < static_cast<size_t>(l.wb) * l.hb * fmt.bytes_per_block)
        throw std::runtime_error("not enough texture data for level");
    std::vector<uint8_t> out(l.size);
    tile_blocks(out.data(), out.size(), linear, l.wb, l.hb, 0, 0, l.stored_w, fmt.bytes_per_block);
    endian_swap(out.data(), out.size(), fmt.endian);
    return out;
}

std::vector<uint8_t> untile_level(const uint8_t *tiled, size_t size, uint32_t width, uint32_t height, int level, const Format &fmt)
{
    LevelLayout l = level_layout(width, height, level, fmt);
    std::vector<uint8_t> swapped(tiled, tiled + std::min<size_t>(size, l.size));
    endian_swap(swapped.data(), swapped.size(), fmt.endian);
    std::vector<uint8_t> out(static_cast<size_t>(l.wb) * l.hb * fmt.bytes_per_block);
    untile_blocks(swapped.data(), swapped.size(), out.data(), l.wb, l.hb, 0, 0, l.stored_w, fmt.bytes_per_block);
    return out;
}

void fetch_constant(uint8_t out[24], uint32_t width, uint32_t height, const Format &fmt, int levels, bool tiled, uint32_t mip_address,
                    uint32_t dimension)
{
    uint32_t pitch = pitch_units(width, fmt);
    uint32_t d[6];
    d[0] = 2 | (pitch << 22) | (uint32_t(tiled) << 31);
    d[1] = fmt.gpu | (fmt.endian << 6);
    d[2] = (width - 1) | ((height - 1) << 13);
    d[3] = fmt.swizzle << 1;
    d[4] = ((static_cast<uint32_t>(levels) - 1) & 0xF) << 6;
    d[5] = (dimension << 9) | ((mip_address & 0xFFFFF) << 12);
    std::memcpy(out, d, sizeof d);
}

std::vector<uint8_t> texture_header(uint32_t width, uint32_t height, const Format &fmt, int levels, uint32_t mip_address, uint32_t dimension)
{
    const uint32_t head[7] = {3, 1, 0, 0, 0, 0xFFFF0000, 0xFFFF0000};
    std::vector<uint8_t> out(52);
    std::memcpy(out.data(), head, sizeof head);
    fetch_constant(out.data() + 28, width, height, fmt, levels, true, mip_address, dimension);
    return out;
}

int packed_mip_level(uint32_t width, uint32_t height)
{
    int log2_size = log2_ceil(std::min(width, height));
    return log2_size > 4 ? log2_size - 4 : 0;
}

namespace
{
void packed_mip_offset(uint32_t width, uint32_t height, int level, const Format &fmt, uint32_t &bx, uint32_t &by)
{
    int log2_w = log2_ceil(width), log2_h = log2_ceil(height);
    int log2_size = std::min(log2_w, log2_h);
    bx = by = 0;
    if (log2_size > 4 + level)
        return;
    int base = log2_size > 4 ? log2_size - 4 : 0;
    int packed = level - base;
    uint32_t x, y;
    if (packed < 3)
    {
        if (log2_w > log2_h)
            x = 0, y = 16u >> packed;
        else
            x = 16u >> packed, y = 0;
    }
    else
    {
        if (log2_w > log2_h)
            x = (1u << (log2_w - base)) >> (packed - 2), y = 0;
        else
            x = 0, y = (1u << (log2_h - base)) >> (packed - 2);
    }
    bx = x / fmt.block;
    by = y / fmt.block;
}
} // namespace

MipChainLayout mip_chain_layout(uint32_t width, uint32_t height, const Format &fmt, int levels, int faces)
{
    MipChainLayout m;
    uint32_t stride = level_layout(width, height, 0, fmt).size;
    m.base_size = stride * faces;
    int packed = packed_mip_level(width, height);
    uint32_t offset = m.base_size;
    bool have_region = false;
    uint32_t region_offset = 0, region_w = 0, region_size = 0;
    for (int level = 1; level < levels; ++level)
    {
        if (packed && level >= packed)
        {
            if (!have_region)
            {
                LevelLayout l = level_layout(width, height, packed, fmt);
                region_offset = offset, region_w = l.stored_w, region_size = l.size;
                have_region = true;
                offset += l.size * faces;
            }
            Placement p{level, region_offset, region_w, 0, 0, region_size};
            packed_mip_offset(width, height, level, fmt, p.bx, p.by);
            m.placements.push_back(p);
        }
        else
        {
            LevelLayout l = level_layout(width, height, level, fmt);
            m.placements.push_back({level, offset, l.stored_w, 0, 0, l.size});
            offset += l.size * faces;
        }
    }
    m.total = offset;
    return m;
}

std::vector<uint8_t> tile_mip_chain(const std::vector<std::vector<uint8_t>> &levels, uint32_t width, uint32_t height, const Format &fmt,
                                    int faces)
{
    MipChainLayout m = mip_chain_layout(width, height, fmt, static_cast<int>(levels.size()), faces);
    uint32_t stride = m.base_size / faces;
    const uint32_t bpb = fmt.bytes_per_block;
    std::vector<uint8_t> out(m.total);
    uint32_t wb, hb;
    level_blocks(width, height, 0, fmt, wb, hb);
    LevelLayout base = level_layout(width, height, 0, fmt);
    for (int face = 0; face < faces; ++face)
    {
        size_t face_bytes = static_cast<size_t>(wb) * hb * bpb;
        if (levels[0].size() < (face + 1) * face_bytes)
            throw std::runtime_error("not enough texture data for level");
        tile_blocks(out.data() + static_cast<size_t>(face) * stride, stride, levels[0].data() + face * face_bytes, wb, hb, 0, 0,
                    base.stored_w, bpb);
    }
    for (const Placement &p : m.placements)
    {
        level_blocks(width, height, p.level, fmt, wb, hb);
        size_t face_bytes = static_cast<size_t>(wb) * hb * bpb;
        const std::vector<uint8_t> &src = levels[p.level];
        for (int face = 0; face < faces; ++face)
        {
            if (src.size() < (face + 1) * face_bytes)
                throw std::runtime_error("not enough texture data for level");
            size_t start = p.region + static_cast<size_t>(face) * p.face_stride;
            tile_blocks(out.data() + start, p.face_stride, src.data() + face * face_bytes, wb, hb, p.bx, p.by, p.stored_w, bpb);
        }
    }
    // the GPU endian swap applies to the whole allocation
    endian_swap(out.data(), out.size(), fmt.endian);
    return out;
}

std::vector<std::vector<uint8_t>> untile_mip_chain(const uint8_t *data, size_t size, uint32_t width, uint32_t height, const Format &fmt,
                                                   int levels, int faces)
{
    MipChainLayout m = mip_chain_layout(width, height, fmt, levels, faces);
    uint32_t stride = m.base_size / faces;
    const uint32_t bpb = fmt.bytes_per_block;
    std::vector<uint8_t> raw(data, data + std::min<size_t>(size, m.total));
    endian_swap(raw.data(), raw.size(), fmt.endian);
    auto region = [&](size_t start, size_t length) {
        if (start > raw.size())
            throw std::runtime_error("texture data ends early");
        return std::min(length, raw.size() - start) / bpb * bpb;
    };

    std::vector<std::vector<uint8_t>> result;
    uint32_t wb, hb;
    level_blocks(width, height, 0, fmt, wb, hb);
    LevelLayout base = level_layout(width, height, 0, fmt);
    std::vector<uint8_t> first(static_cast<size_t>(wb) * hb * bpb * faces);
    for (int face = 0; face < faces; ++face)
    {
        size_t start = static_cast<size_t>(face) * stride;
        untile_blocks(raw.data() + start, region(start, std::min(stride, base.size)), first.data() + face * static_cast<size_t>(wb) * hb * bpb,
                      wb, hb, 0, 0, base.stored_w, bpb);
    }
    result.push_back(std::move(first));
    for (const Placement &p : m.placements)
    {
        level_blocks(width, height, p.level, fmt, wb, hb);
        size_t face_bytes = static_cast<size_t>(wb) * hb * bpb;
        std::vector<uint8_t> level(face_bytes * faces);
        for (int face = 0; face < faces; ++face)
        {
            size_t start = p.region + static_cast<size_t>(face) * p.face_stride;
            untile_blocks(raw.data() + start, region(start, p.face_stride), level.data() + face * face_bytes, wb, hb, p.bx, p.by, p.stored_w,
                          bpb);
        }
        result.push_back(std::move(level));
    }
    return result;
}

std::vector<uint8_t> texture_header_mips(uint32_t width, uint32_t height, const Format &fmt, int levels, int faces)
{
    MipChainLayout m = mip_chain_layout(width, height, fmt, levels, faces);
    bool packed = false;
    for (const Placement &p : m.placements)
        if (p.bx || p.by)
            packed = true;
    int pml = packed_mip_level(width, height);
    if (pml && levels > pml)
        packed = true;
    uint32_t dimension = faces == 6 ? GPUDIMENSION_CUBEMAP : GPUDIMENSION_2D;
    std::vector<uint8_t> header = texture_header(width, height, fmt, levels, levels > 1 ? (m.base_size >> 12) : 0, dimension);
    if (levels > 1 && packed)
    {
        uint32_t dword5;
        std::memcpy(&dword5, header.data() + 28 + 20, 4);
        dword5 |= 1u << 11;
        std::memcpy(header.data() + 28 + 20, &dword5, 4);
    }
    return header;
}
} // namespace t4ff::xenos
