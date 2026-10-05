#pragma once

#include "core/zone.h"

// The PC and Xbox 360 platforms (the Python t4ff's platforms.py), built on first use from the layouts
// and zone code commands compiled into the program.
namespace t4ff
{
const Platform &pc();
const Platform &x360();
inline const Platform &for_endian(bool big_endian)
{
    return big_endian ? x360() : pc();
}
} // namespace t4ff
