#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "audio/audio.h"
#include "core/zone.h"

// Keeping a map within the console's loaded sound limit (the Python t4ff's soundbudget.py): identical
// sounds are loaded once, then the longest become streamed sounds the map's sounds folder serves.
namespace t4ff
{
constexpr int DEFAULT_MAX_LOADED_SOUNDS = 1500;
constexpr double DEFAULT_LOADED_SOUND_MIB = 32;

// sets the sound type in the flags of every alias to its sound file's; returns how many changed
int sync_alias_types(const Platform &p, Zone &zone);

struct SoundLimitStats
{
    int shared = 0, streamed = 0, count = 0, looping = 0;
    uint64_t stream_bytes = 0, bytes = 0;
};
// brings the loaded sounds of the zone to limit (0: none) and max_bytes (0: none); streams: the xma2encode
// stream of each converted loaded sound by lower case name, for those to stream
SoundLimitStats limit_loaded_sounds(const Platform &p, Zone &zone, int limit, const std::map<std::string, std::shared_ptr<XmaStream>> &streams,
                                    const std::filesystem::path &sounds_dir, const std::function<void(const std::string &)> &log,
                                    uint64_t max_bytes = 0);
} // namespace t4ff
