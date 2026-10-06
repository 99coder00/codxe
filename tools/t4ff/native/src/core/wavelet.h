#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/texture_format.h"

// Wavelet compressed IWI images (formats 6 to 10), decoded as the PC game does (the Python t4ff's
// wavelet.py, ported from OpenAssetTools' IwiWaveletDecoder, GPL-3.0).
namespace t4ff::wavelet
{
struct WaveletError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

bool is_wavelet_format(int fmt_code);

struct Decoded
{
    Fmt format;
    std::vector<std::vector<uint8_t>> levels; // largest first, the faces of a cube map one after the other
};

// The levels of a wavelet IWI's payload (the data after its 28 byte header).
Decoded decode(int fmt_code, uint32_t width, uint32_t height, int faces, bool mipped, const uint8_t *payload, size_t size);
} // namespace t4ff::wavelet
