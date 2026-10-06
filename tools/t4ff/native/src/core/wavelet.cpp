#include "core/wavelet.h"

#include <cstring>
#include <string>

namespace t4ff::wavelet
{
namespace
{
constexpr int ESCAPE = -32768;
constexpr int LOOKUP_BITS = 12;

struct Codeword
{
    uint32_t code;
    int bits;
    int value;
};

#include "core/wavelet_tables.inc"

// Value and bit count for every 12 bit window (the codes are prefix free and complete), and how an
// escaped value is read.
struct Code
{
    int values[1 << LOOKUP_BITS];
    uint8_t bits[1 << LOOKUP_BITS];
    int escape_bits;
    int bias;

    template <size_t N> Code(const Codeword (&words)[N], int escape_bits_, int bias_) : escape_bits(escape_bits_), bias(bias_)
    {
        std::memset(bits, 0, sizeof bits);
        for (const Codeword &w : words)
        {
            for (uint32_t index = w.code; index < (1u << LOOKUP_BITS); index += 1u << w.bits)
            {
                values[index] = w.value;
                bits[index] = static_cast<uint8_t>(w.bits);
            }
        }
    }
};

const Code &blue()
{
    static const Code code(BLUE_CODES, 9, 0xFF);
    return code;
}
const Code &red_green()
{
    static const Code code(RED_GREEN_CODES, 10, 0x1FE);
    return code;
}
const Code &alpha()
{
    static const Code code(ALPHA_CODES, 9, 0xFF);
    return code;
}

struct FormatInfo
{
    Fmt name;
    int channels; // channels coded
    int texel;    // bytes a texel
};

bool format_info(int fmt_code, FormatInfo &info)
{
    switch (fmt_code)
    {
    case 0x06: info = {Fmt::A8R8G8B8, 4, 4}; return true; // B, G, R, A
    case 0x07: info = {Fmt::X8R8G8B8, 3, 4}; return true; // B, G, R, then 255
    case 0x08: info = {Fmt::A8L8, 2, 2}; return true;     // L, A
    case 0x09: info = {Fmt::L8, 1, 1}; return true;
    case 0x0A: info = {Fmt::A8, 1, 1}; return true;
    }
    return false;
}

class Reader
{
  public:
    Reader(const uint8_t *data, size_t size) : buf(data, data + size), size(size)
    {
        buf.resize(size + 16, 0); // room to peek past the end
    }

    const uint8_t *raw(size_t count)
    {
        if (bit_mode || byte + count > size)
            throw WaveletError("raw texels after coded ones, or past the end");
        const uint8_t *out = buf.data() + byte;
        byte += count;
        return out;
    }

    // count times the symbols of plan (nullptr: a single raw bit), in order
    void symbols(size_t count, const std::vector<const Code *> &plan, std::vector<int> &out)
    {
        if (!bit_mode)
        {
            bit = byte * 8;
            bit_mode = true;
        }
        out.resize(count * plan.size());
        size_t pos = bit;
        size_t i = 0;
        for (size_t n = 0; n < count; ++n)
        {
            for (const Code *code : plan)
            {
                uint32_t word = peek(pos);
                if (!code)
                {
                    out[i] = word & 1;
                    pos += 1;
                }
                else
                {
                    uint32_t k = word & 0xFFF;
                    int value = code->values[k];
                    pos += code->bits[k];
                    if (value == ESCAPE)
                    {
                        value = static_cast<int>(peek(pos) & ((1u << code->escape_bits) - 1)) - code->bias;
                        pos += code->escape_bits;
                    }
                    out[i] = value;
                }
                ++i;
            }
        }
        if (pos > size * 8)
            throw WaveletError("coded data ends early");
        bit = pos;
    }

  private:
    std::vector<uint8_t> buf;
    size_t size;
    size_t byte = 0;
    bool bit_mode = false;
    size_t bit = 0;

    uint32_t peek(size_t pos) const
    {
        size_t at = pos >> 3;
        if (at + 4 > buf.size())
            throw WaveletError("coded data ends early");
        uint32_t word;
        std::memcpy(&word, buf.data() + at, 4);
        return word >> (pos & 7);
    }
};

struct PlanChannel
{
    int channel, parity_at, at;
    bool add_first;
};

void block_plan(int channels, std::vector<const Code *> &plan, std::vector<PlanChannel> &layout)
{
    if (channels != 1)
    {
        layout.push_back({0, static_cast<int>(plan.size()), static_cast<int>(plan.size()) + 1, false});
        plan.insert(plan.end(), {nullptr, &blue(), &blue(), &blue()});
        if (channels >= 3)
        {
            for (int channel : {1, 2})
            {
                layout.push_back({channel, static_cast<int>(plan.size()), static_cast<int>(plan.size()) + 1, true});
                plan.insert(plan.end(), {nullptr, &red_green(), &red_green(), &red_green()});
            }
        }
    }
    if (channels != 3)
    {
        layout.push_back({channels - 1, static_cast<int>(plan.size()), static_cast<int>(plan.size()) + 1, false});
        plan.insert(plan.end(), {nullptr, &alpha(), &alpha(), &alpha()});
    }
}

// One level (height x width texels of texel bytes) from the next smaller one.
std::vector<uint8_t> decode_level(Reader &reader, const std::vector<uint8_t> *smaller, uint32_t width, uint32_t height, int channels,
                                  int texel)
{
    std::vector<uint8_t> level(static_cast<size_t>(width) * height * texel, 255);
    if (width <= 1 || height <= 1)
    {
        const uint8_t *raw = reader.raw(static_cast<size_t>(width) * height * channels);
        for (size_t t = 0; t < static_cast<size_t>(width) * height; ++t)
            std::memcpy(level.data() + t * texel, raw + t * channels, channels);
        return level;
    }
    const uint32_t hw = width / 2, hh = height / 2;
    std::vector<int> source(static_cast<size_t>(hw) * hh * texel);
    for (size_t i = 0; i < source.size(); ++i)
        source[i] = (*smaller)[i];
    std::vector<int> syms;
    reader.symbols(1, {nullptr}, syms);
    if (syms[0])
    {
        std::vector<int> deltas;
        reader.symbols(static_cast<size_t>(hw) * hh * channels, {&alpha()}, deltas);
        for (size_t t = 0; t < static_cast<size_t>(hw) * hh; ++t)
            for (int c = 0; c < channels; ++c)
                source[t * texel + c] = (source[t * texel + c] + deltas[t * channels + c]) & 0xFF;
    }
    std::vector<const Code *> plan;
    std::vector<PlanChannel> layout;
    block_plan(channels, plan, layout);
    std::vector<int> blocks;
    reader.symbols(static_cast<size_t>(hw) * hh, plan, blocks);
    const size_t stride = plan.size();
    for (uint32_t y = 0; y < hh; ++y)
    {
        for (uint32_t x = 0; x < hw; ++x)
        {
            const int *b = blocks.data() + (static_cast<size_t>(y) * hw + x) * stride;
            int first[3] = {0, 0, 0};
            for (const PlanChannel &pc : layout)
            {
                int h = b[pc.at], v = b[pc.at + 1], d = b[pc.at + 2];
                if (pc.channel == 0 && channels != 1)
                {
                    first[0] = h;
                    first[1] = v;
                    first[2] = d;
                }
                if (pc.add_first)
                {
                    h += first[0];
                    v += first[1];
                    d += first[2];
                }
                int base = 2 * source[(static_cast<size_t>(y) * hw + x) * texel + pc.channel];
                auto put = [&](uint32_t px, uint32_t py, int value) {
                    level[(static_cast<size_t>(py) * width + px) * texel + pc.channel] = static_cast<uint8_t>(value & 0xFF);
                };
                put(2 * x, 2 * y, b[pc.parity_at] + ((d + v + h + base) >> 1));
                put(2 * x + 1, 2 * y, (h + base - d - v) >> 1);
                put(2 * x, 2 * y + 1, (v - d + base - h) >> 1);
                put(2 * x + 1, 2 * y + 1, (base - h - v + d) >> 1);
            }
        }
    }
    return level;
}
} // namespace

bool is_wavelet_format(int fmt_code)
{
    FormatInfo info;
    return format_info(fmt_code, info);
}

Decoded decode(int fmt_code, uint32_t width, uint32_t height, int faces, bool mipped, const uint8_t *payload, size_t size)
{
    FormatInfo info;
    if (!format_info(fmt_code, info))
        throw WaveletError("not a wavelet format: " + std::to_string(fmt_code));
    if ((width & (width - 1)) || (height & (height - 1)))
        throw WaveletError("wavelet textures are powers of two");
    if (!mipped && width > 1 && height > 1)
        throw WaveletError("wavelet textures larger than 1x1 have mip levels");
    std::vector<std::pair<uint32_t, uint32_t>> sizes{{width, height}};
    while (mipped && sizes.back() != std::pair<uint32_t, uint32_t>(1, 1))
    {
        auto [w, h] = sizes.back();
        sizes.emplace_back(std::max(w >> 1, 1u), std::max(h >> 1, 1u));
    }
    Reader reader(payload, size);
    std::vector<std::vector<std::vector<uint8_t>>> levels(sizes.size());
    for (size_t index = sizes.size(); index-- > 0;)
    {
        auto [w, h] = sizes[index];
        for (int face = 0; face < faces; ++face)
        {
            const std::vector<uint8_t> *smaller = index + 1 < sizes.size() ? &levels[index + 1][face] : nullptr;
            levels[index].push_back(decode_level(reader, smaller, w, h, info.channels, info.texel));
        }
    }
    Decoded out{info.name, {}};
    for (auto &faces_of_level : levels)
    {
        std::vector<uint8_t> joined;
        for (auto &f : faces_of_level)
            joined.insert(joined.end(), f.begin(), f.end());
        out.levels.push_back(std::move(joined));
    }
    return out;
}
} // namespace t4ff::wavelet
