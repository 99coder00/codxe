#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "audio/soundbudget.h"
#include "convert/stream.h"

// Converting a PC usermap for the Xbox 360: the Python t4ff's convert command from the map's fastfiles
// on (_convert_map, the memory plan, the files of the map's folder). The command lines (t4ff-cli
// convert, in both its forms) and the window fill the settings.
namespace t4ff
{
namespace fs = std::filesystem;

struct ConvertSettings
{
    fs::path out, dump, ff;
    std::vector<fs::path> map_iwds, stock_iwds, console_zones;
    std::string map_name;
    double texture_budget = 0;
    bool texture_budget_auto = true; // --texture-budget auto (the default)
    // streaming and the memory plan
    bool stream_textures = false, keep_quarter = false, keep_mip_tail = false;
    double upgrade_budget = 96.0, memory_target = MEMORY_TARGET_MIB, stream_growth = 0;
    std::string deep_stream;
    std::optional<int64_t> upgrade_budget_bytes; // what the attempts allow (set by convert_usermap)
    double stream_growth_scale = 1.0;
    // where the messages go (default: stdout); the timings of the phases in them
    std::function<void(const std::string &)> log;
    bool timings = false;
    uint32_t max_texture_size = 0;
    bool no_mips = false, no_compress = false, allow_unverified = false, reference_techsets = false, no_zone_cache = false;
    fs::path sounds_dir; // the map's output folder: its sounds folder gets the streamed sounds
    fs::path out_dir;    // the map's output folder: options.txt, scripts/...
    std::vector<fs::path> map_files; // the folders of the map's own files (its fastfiles' and .iwd files')
    fs::path load_ff, loading_image; // the map's PC load zone; a picture for its loading screen
    std::string name;                // the map's name in the map lists
    bool no_load_zone = false;
    // sounds
    fs::path xma_encoder, ffmpeg;
    int xma_quality = 60, sound_rate = 0, stream_rate = 0, jobs = 0;
    bool mono_sounds = false, mono_streams = false, no_sounds = false, no_sound_cache = false;
    int max_loaded_sounds = DEFAULT_MAX_LOADED_SOUNDS;
    double loaded_sound_memory = DEFAULT_LOADED_SOUND_MIB;
};

// Converts the map's PC fastfiles (paths: the mod's language zones, the map, its patch, the mod, in that
// order). Returns the exit code.
int convert_usermap(const std::vector<fs::path> &paths, const ConvertSettings &settings);

// Writes a console zone's fastfile, reads it back (the console's loading rules check it) and reports
// the memory of its blocks, as the Python's write_zone.
void write_console_zone(const std::vector<uint8_t> &zone, const fs::path &target, int jobs, const std::function<void(const std::string &)> &log);
} // namespace t4ff
