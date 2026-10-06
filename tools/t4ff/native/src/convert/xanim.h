#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/zone.h"

// PC -> Xbox 360 animation (XAnimParts) data (the Python t4ff's xanim.py): PC animations have 10 part
// types, console ones 12 (precise and compact full quaternions); console quaternions are packed by
// their smallest components; bones are sorted by their console part type.
namespace t4ff
{
class ZoneConverter;

struct XAnimError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// (n, 4) quaternions as console integers: widths of the three stored components, low bits first
std::vector<uint64_t> pack_quats(const std::vector<int64_t> &quats, const int widths[3]);
std::vector<uint16_t> pack_half_quats(const std::vector<int64_t> &quats);

// rewrites the generically converted XAnimParts dst (and its data arrays) for the console
void convert_xanim_parts(ZoneConverter &conv, Node &src, Node &dst, const std::string &name);
} // namespace t4ff
