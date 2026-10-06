#include "core/dxt.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

// The encoder follows numpy's float64 arithmetic exactly (dxt.py): sums of integers are exact in any
// order; numpy 2.x's einsum adds three products as (p0 + p2) + p1 (its two lane SIMD loop); norms and
// distance sums add left to right; np.round rounds half to even (nearbyint in the default rounding
// mode); argmin takes the first smallest.
namespace t4ff::dxt
{
namespace
{
uint32_t blocks_of(uint32_t texels)
{
    return std::max(1u, (texels + 3) / 4);
}

void rgb565(uint32_t c, int out[3])
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = static_cast<int>((r << 3) | (r >> 2));
    out[1] = static_cast<int>((g << 2) | (g >> 4));
    out[2] = static_cast<int>((b << 3) | (b >> 2));
}

uint16_t to_565(const double v[3])
{
    uint32_t c[3];
    for (int i = 0; i < 3; ++i)
        c[i] = static_cast<uint32_t>(std::clamp(std::nearbyint(v[i]), 0.0, 255.0));
    return static_cast<uint16_t>(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255));
}

// -- decoding

void decode_color(const uint8_t *b, bool allow_punchthrough, uint8_t rgba[16][4], bool set_alpha)
{
    uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    uint32_t idx = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
    int pal[4][3];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    bool four = c0 > c1 || !allow_punchthrough;
    for (int c = 0; c < 3; ++c)
    {
        pal[2][c] = four ? (2 * pal[0][c] + pal[1][c]) / 3 : (pal[0][c] + pal[1][c]) / 2;
        pal[3][c] = four ? (pal[0][c] + 2 * pal[1][c]) / 3 : 0;
    }
    for (int t = 0; t < 16; ++t)
    {
        uint32_t i = (idx >> (2 * t)) & 3;
        for (int c = 0; c < 3; ++c)
            rgba[t][c] = static_cast<uint8_t>(pal[i][c]);
        if (set_alpha)
            rgba[t][3] = (i == 3 && !four) ? 0 : 255;
    }
}

void decode_alpha5(const uint8_t *b, int out[16])
{
    int a0 = b[0], a1 = b[1];
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= uint64_t(b[2 + i]) << (8 * i);
    bool eight = a0 > a1;
    int pal[8] = {a0, a1};
    for (int i = 1; i < 7; ++i)
        pal[1 + i] = eight ? ((7 - i) * a0 + i * a1) / 7 : 0;
    for (int i = 1; i < 5; ++i)
        pal[1 + i] = eight ? pal[1 + i] : ((5 - i) * a0 + i * a1) / 5;
    if (!eight)
    {
        pal[6] = 0;
        pal[7] = 255;
    }
    for (int t = 0; t < 16; ++t)
        out[t] = pal[(bits >> (3 * t)) & 7];
}

// -- encoding

void encode_color(const double rgb[16][3], const bool *transparent, uint8_t out[8])
{
    double mean[3];
    for (int c = 0; c < 3; ++c)
    {
        double s = 0;
        for (int i = 0; i < 16; ++i)
            s += rgb[i][c];
        mean[c] = s / 16;
    }
    double centered[16][3];
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 3; ++c)
            centered[i][c] = rgb[i][c] - mean[c];
    double cov[3][3];
    for (int j = 0; j < 3; ++j)
    {
        for (int k = 0; k < 3; ++k)
        {
            double s = 0;
            for (int i = 0; i < 16; ++i)
                s += centered[i][j] * centered[i][k];
            cov[j][k] = s;
        }
    }

    // principal axis by power iteration
    const double inv = 1.0 / std::sqrt(3.0);
    double axis[3] = {inv, inv, inv};
    for (int it = 0; it < 4; ++it)
    {
        double na[3];
        for (int j = 0; j < 3; ++j)
            na[j] = (cov[j][0] * axis[0] + cov[j][2] * axis[2]) + cov[j][1] * axis[1];
        double norm = std::sqrt((na[0] * na[0] + na[1] * na[1]) + na[2] * na[2]);
        for (int j = 0; j < 3; ++j)
            axis[j] = norm > 1e-9 ? na[j] / std::max(norm, 1e-9) : inv;
    }
    double pmin = std::numeric_limits<double>::infinity(), pmax = -pmin;
    for (int i = 0; i < 16; ++i)
    {
        double proj = (centered[i][0] * axis[0] + centered[i][2] * axis[2]) + centered[i][1] * axis[1];
        pmin = std::min(pmin, proj);
        pmax = std::max(pmax, proj);
    }
    double lo[3], hi[3];
    for (int c = 0; c < 3; ++c)
    {
        lo[c] = mean[c] + axis[c] * pmin;
        hi[c] = mean[c] + axis[c] * pmax;
    }

    uint16_t c_hi = to_565(hi), c_lo = to_565(lo);
    bool punch = false;
    if (transparent)
        for (int i = 0; i < 16; ++i)
            punch |= transparent[i];
    // 4 colour mode needs c0 > c1, 3 colour (punch through) mode c0 <= c1
    uint16_t c0 = punch ? std::min(c_hi, c_lo) : std::max(c_hi, c_lo);
    uint16_t c1 = punch ? std::max(c_hi, c_lo) : std::min(c_hi, c_lo);
    int i0[3], i1[3];
    rgb565(c0, i0);
    rgb565(c1, i1);
    double pal[4][3];
    for (int c = 0; c < 3; ++c)
    {
        double p0 = i0[c], p1 = i1[c];
        pal[0][c] = p0;
        pal[1][c] = p1;
        pal[2][c] = punch ? (p0 + p1) / 2 : (2 * p0 + p1) / 3;
        pal[3][c] = punch ? std::numeric_limits<double>::infinity() : (p0 + 2 * p1) / 3;
    }
    uint32_t bits = 0;
    bool same = c0 == c1 && !punch; // identical endpoints in 4 colour mode would switch modes: index 0
    for (int i = 0; i < 16; ++i)
    {
        uint32_t best = 0;
        double best_dist = 0;
        for (uint32_t k = 0; k < 4; ++k)
        {
            double d0 = rgb[i][0] - pal[k][0], d1 = rgb[i][1] - pal[k][1], d2 = rgb[i][2] - pal[k][2];
            double dist = (d0 * d0 + d1 * d1) + d2 * d2;
            if (k == 0 || dist < best_dist)
            {
                best = k;
                best_dist = dist;
            }
        }
        if (transparent && transparent[i])
            best = 3;
        if (same)
            best = 0;
        bits |= best << (2 * i);
    }
    out[0] = static_cast<uint8_t>(c0 & 0xFF);
    out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1 & 0xFF);
    out[3] = static_cast<uint8_t>(c1 >> 8);
    for (int i = 0; i < 4; ++i)
        out[4 + i] = static_cast<uint8_t>(bits >> (8 * i));
}

void encode_alpha5(const int alpha[16], uint8_t out[8])
{
    int a0 = alpha[0], a1 = alpha[0];
    for (int i = 1; i < 16; ++i)
    {
        a0 = std::max(a0, alpha[i]);
        a1 = std::min(a1, alpha[i]);
    }
    if (a0 == a1)
        a0 = std::min(a0 + 1, 255);
    if (a0 == a1)
        a1 = a1 - 1;
    double pal[8] = {double(a0), double(a1)};
    for (int i = 1; i < 7; ++i)
        pal[1 + i] = double((7 - i) * a0 + i * a1) / 7.0;
    uint64_t bits = 0;
    for (int t = 0; t < 16; ++t)
    {
        uint64_t best = 0;
        double best_dist = 0;
        for (uint64_t k = 0; k < 8; ++k)
        {
            double d = std::abs(double(alpha[t]) - pal[k]);
            if (k == 0 || d < best_dist)
            {
                best = k;
                best_dist = d;
            }
        }
        bits |= best << (3 * t);
    }
    out[0] = static_cast<uint8_t>(a0);
    out[1] = static_cast<uint8_t>(a1);
    for (int i = 0; i < 6; ++i)
        out[2 + i] = static_cast<uint8_t>(bits >> (8 * i));
}
} // namespace

Pixels decode(const uint8_t *data, size_t size, uint32_t width, uint32_t height, Fmt fmt)
{
    const size_t bpb = fmt == Fmt::DXT1 ? 8 : 16;
    const uint32_t bw = blocks_of(width), bh = blocks_of(height);
    if (size < static_cast<size_t>(bw) * bh * bpb)
        throw std::runtime_error("not enough DXT data");
    Pixels out(width, height, 4);
    uint8_t rgba[16][4];
    for (uint32_t by = 0; by < bh; ++by)
    {
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            const uint8_t *b = data + (static_cast<size_t>(by) * bw + bx) * bpb;
            if (fmt == Fmt::DXN)
            {
                // two DXT5 alpha style blocks: red (x), then green (y); blue gets z back
                int x[16], y[16];
                decode_alpha5(b, x);
                decode_alpha5(b + 8, y);
                for (int t = 0; t < 16; ++t)
                {
                    double fx = x[t] / 127.5 - 1, fy = y[t] / 127.5 - 1;
                    double z = std::nearbyint((std::sqrt(std::clamp(1 - fx * fx - fy * fy, 0.0, 1.0)) + 1) * 127.5);
                    rgba[t][0] = static_cast<uint8_t>(x[t]);
                    rgba[t][1] = static_cast<uint8_t>(y[t]);
                    rgba[t][2] = static_cast<uint8_t>(z);
                    rgba[t][3] = 255;
                }
            }
            else if (fmt == Fmt::DXT1)
                decode_color(b, true, rgba, true);
            else
            {
                decode_color(b + 8, false, rgba, false);
                if (fmt == Fmt::DXT3)
                {
                    for (int t = 0; t < 16; ++t)
                        rgba[t][3] = static_cast<uint8_t>(((b[t / 2] >> (4 * (t & 1))) & 15) * 17);
                }
                else
                {
                    int a[16];
                    decode_alpha5(b, a);
                    for (int t = 0; t < 16; ++t)
                        rgba[t][3] = static_cast<uint8_t>(a[t]);
                }
            }
            for (int t = 0; t < 16; ++t)
            {
                uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                if (x < width && y < height)
                    std::memcpy(out.at(x, y), rgba[t], 4);
            }
        }
    }
    return out;
}

std::vector<uint8_t> encode(const Pixels &rgba, Fmt fmt)
{
    if (rgba.channels != 4)
        throw std::runtime_error("DXT encoding needs RGBA");
    const uint32_t bw = (rgba.width + 3) / 4, bh = (rgba.height + 3) / 4;
    const size_t bpb = fmt == Fmt::DXT1 ? 8 : 16;
    if (fmt != Fmt::DXT1 && fmt != Fmt::DXT3 && fmt != Fmt::DXT5 && fmt != Fmt::DXN)
        throw std::runtime_error("not a DXT format");
    std::vector<uint8_t> out(static_cast<size_t>(bw) * bh * bpb);
    for (uint32_t by = 0; by < bh; ++by)
    {
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            // the block, padded by repeating the last row and column
            double rgb[16][3];
            int alpha[16];
            for (int t = 0; t < 16; ++t)
            {
                uint32_t x = std::min(bx * 4 + (t & 3), rgba.width - 1), y = std::min(by * 4 + (t >> 2), rgba.height - 1);
                const uint8_t *px = rgba.at(x, y);
                rgb[t][0] = px[0];
                rgb[t][1] = px[1];
                rgb[t][2] = px[2];
                alpha[t] = px[3];
            }
            uint8_t *o = out.data() + (static_cast<size_t>(by) * bw + bx) * bpb;
            switch (fmt)
            {
            case Fmt::DXT1: {
                bool transparent[16];
                for (int t = 0; t < 16; ++t)
                    transparent[t] = alpha[t] < 128;
                encode_color(rgb, transparent, o);
                break;
            }
            case Fmt::DXT3:
                // 4 bits of alpha a texel, two a byte, the first texel in the low bits
                for (int j = 0; j < 8; ++j)
                {
                    auto nibble = [&](int t) {
                        return static_cast<uint8_t>(std::clamp(std::nearbyint(alpha[t] / 17.0), 0.0, 15.0));
                    };
                    o[j] = static_cast<uint8_t>(nibble(2 * j) | (nibble(2 * j + 1) << 4));
                }
                encode_color(rgb, nullptr, o + 8);
                break;
            case Fmt::DXT5:
                encode_alpha5(alpha, o);
                encode_color(rgb, nullptr, o + 8);
                break;
            default: { // DXN
                int r[16], g[16];
                for (int t = 0; t < 16; ++t)
                {
                    r[t] = static_cast<int>(rgb[t][0]);
                    g[t] = static_cast<int>(rgb[t][1]);
                }
                encode_alpha5(r, o);
                encode_alpha5(g, o + 8);
                break;
            }
            }
        }
    }
    return out;
}

std::vector<uint8_t> dxt5_normal_to_dxn(const uint8_t *data, size_t size, uint32_t width, uint32_t height)
{
    const size_t n = static_cast<size_t>(blocks_of(width)) * blocks_of(height);
    if (size < n * 16)
        throw std::runtime_error("not enough DXT data");
    std::vector<uint8_t> out(n * 16);
    uint8_t rgba[16][4];
    for (size_t i = 0; i < n; ++i)
    {
        const uint8_t *b = data + i * 16;
        std::memcpy(out.data() + i * 16, b, 8);
        decode_color(b + 8, false, rgba, false);
        int y[16];
        for (int t = 0; t < 16; ++t)
            y[t] = rgba[t][1];
        encode_alpha5(y, out.data() + i * 16 + 8);
    }
    return out;
}

Pixels downscale(const Pixels &p)
{
    uint32_t h = p.height > 1 ? p.height / 2 : p.height;
    uint32_t w = p.width > 1 ? p.width / 2 : p.width;
    const bool rows = p.height > 1, cols = p.width > 1;
    Pixels out(w, h, p.channels);
    for (uint32_t y = 0; y < h; ++y)
    {
        for (uint32_t x = 0; x < w; ++x)
        {
            for (uint32_t c = 0; c < p.channels; ++c)
            {
                auto row = [&](uint32_t sx) {
                    if (!rows)
                        return double(p.at(sx, y)[c]);
                    return (double(p.at(sx, 2 * y)[c]) + double(p.at(sx, 2 * y + 1)[c])) / 2;
                };
                double v = cols ? (row(2 * x) + row(2 * x + 1)) / 2 : row(x);
                out.at(x, y)[c] = static_cast<uint8_t>(std::clamp(std::nearbyint(v), 0.0, 255.0));
            }
        }
    }
    return out;
}

Pixels bgra_to_rgba(const uint8_t *data, size_t size, uint32_t width, uint32_t height)
{
    Pixels out(width, height, 4);
    if (size < out.data.size())
        throw std::runtime_error("not enough A8R8G8B8 data");
    for (size_t i = 0; i < out.data.size(); i += 4)
    {
        out.data[i] = data[i + 2];
        out.data[i + 1] = data[i + 1];
        out.data[i + 2] = data[i];
        out.data[i + 3] = data[i + 3];
    }
    return out;
}
} // namespace t4ff::dxt
