#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "core/zone.h"

// Decompressed zones kept in files: a fastfile is inflated once, streamed straight to the cache (no
// buffer the zone's size), and from then on its zone is a mapped file. Nodes view the mapping, so only
// the pages the reader or a conversion touches are ever read from disk, and the system can drop them
// again: the console library's zones (mostly texture and sound data nobody reads unless an asset is
// copied) cost next to no memory. Repeat runs skip zlib entirely.
namespace t4ff
{
// %LOCALAPPDATA%\t4ff\zone_cache
std::filesystem::path default_zone_cache_dir();

struct OpenedZone
{
    bool big_endian = false;
    std::shared_ptr<const ZoneBytes> bytes;
    bool cached = false;     // a mapped cache file
    bool cache_hit = false;  // that was already there
    std::string cache_error; // why the cache could not be used (the zone is in memory then)
};

// The decompressed zone of the fastfile at path: from the cache folder when one is given (made when
// missing or older than the fastfile), else in memory.
OpenedZone open_zone(const std::filesystem::path &path, const std::filesystem::path &cache_dir = {});
} // namespace t4ff
