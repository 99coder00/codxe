#pragma once

#include "pch.h"

namespace t4
{
namespace sp
{
// An image's entry in the active custom map's images.pak (false when the map has none): its data's
// offset and size, flags (IMAGES_PAK_DEEP: the whole texture, its level 1 at mipOffset, a texture of
// half its size whose own mips start level2Offset on, 0: no such steps; IMAGES_PAK_EIGHTH with it:
// the fastfile's copy is an eighth of its size, not a quarter), and the pack as the game opens it.
const unsigned int IMAGES_PAK_DEEP = 1;
const unsigned int IMAGES_PAK_EIGHTH = 2;
struct ImagesPakImage
{
    unsigned int offset;
    unsigned int size;
    unsigned int flags;
    unsigned int mipOffset;
    unsigned int level2Offset;
};
bool GetImagesPakImage(const char *imageName, ImagesPakImage *image);
std::string GetImagesPakPath();

class FastFiles : public Module
{
  public:
    FastFiles();
    ~FastFiles();
};
} // namespace sp
} // namespace t4
