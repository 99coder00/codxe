#pragma once

#include <cstdint>
#include <vector>

#include "core/texture_format.h"

// DXT1, DXT3, DXT5 and DXN decoding and encoding (the Python t4ff's dxt.py). The encoder is a range fit
// along the principal colour axis. It computes exactly what the numpy one does, operation for
// operation in double precision, so its output is byte identical.
namespace t4ff
{
// Texels, row by row, channels interleaved.
struct Pixels
{
    uint32_t width = 0, height = 0, channels = 4;
    std::vector<uint8_t> data;

    Pixels() = default;
    Pixels(uint32_t w, uint32_t h, uint32_t c) : width(w), height(h), channels(c), data(static_cast<size_t>(w) * h * c)
    {
    }
    uint8_t *at(uint32_t x, uint32_t y)
    {
        return data.data() + (static_cast<size_t>(y) * width + x) * channels;
    }
    const uint8_t *at(uint32_t x, uint32_t y) const
    {
        return data.data() + (static_cast<size_t>(y) * width + x) * channels;
    }
};

namespace dxt
{
// DXT or DXN data (linear PC layout) as RGBA. DXN gives red and green, blue rebuilt as z.
Pixels decode(const uint8_t *data, size_t size, uint32_t width, uint32_t height, Fmt fmt);
// RGBA as DXT1, DXT3, DXT5 or DXN (from red and green), linear PC layout.
std::vector<uint8_t> encode(const Pixels &rgba, Fmt fmt);
// A PC DXT5 normal map (x in alpha, y in the grey colour) as DXN: the alpha blocks stay, y is encoded
// again.
std::vector<uint8_t> dxt5_normal_to_dxn(const uint8_t *data, size_t size, uint32_t width, uint32_t height);
// 2x box filter (any channel count).
Pixels downscale(const Pixels &p);
// A8R8G8B8 bytes (B, G, R, A) as RGBA.
Pixels bgra_to_rgba(const uint8_t *data, size_t size, uint32_t width, uint32_t height);
} // namespace dxt
} // namespace t4ff
