#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "convert/scripts.h"
#include "core/image.h"
#include "core/zone.h"

// Texture streaming (the Python t4ff's stream.py): the top mip level of the textures of models and
// world surfaces goes to the map's images.pak, the fastfile keeping them a level (or two, three)
// smaller, and the boxes that load them back are written into the models and the world.
//
// The console keeps a streamed image one level smaller in the fastfile and reads its top level from
// highmip\<image>.hi when something using it is drawn close by; CoD Xe serves those files from the
// map's pack. An image streams only when the fastfile's data is laid out as the full texture's mip
// levels are, byte for byte (split).
namespace t4ff
{
constexpr const char *PAK_NAME = "images.pak";
// the boxes of a map for a console's memory: half the disc's growth
constexpr double CONSOLE_STREAM_GROWTH = 0.5;

// what stream_textures did, for the memory plan
struct StreamReport
{
    uint64_t upgrade_bytes = 0;            // the bytes the PC versions of stock textures added
    std::map<std::string, int> steps;      // the levels each image streams (lower case names)
};

struct StreamOptions
{
    std::vector<std::filesystem::path> highmip_dirs; // the console fastfiles' own highmip folders
    std::function<bool(const std::string &)> is_game_image;
    // the PC game's version of a stock texture, tiled for the console (nullopt: none)
    std::function<std::optional<ConsoleTexture>(const std::string &name, int semantic)> stock_texture;
    bool deep_all = false;                     // every image streams two levels where it can
    std::unordered_set<std::string> deep;      // else these (lower case names)
    std::optional<uint64_t> upgrade_budget;    // bytes the stock upgrades may add (nullopt: none)
    bool mip_tail = true;
    std::unordered_set<std::string> eighth;    // the deep ones keeping an eighth of their size
    double growth = 1.0;                       // the boxes' growth, of the disc linker's
};

// Streams the textures (see above) into out_dir/images.pak; returns the names of the images streamed.
std::vector<std::string> stream_textures(const Platform &p, Zone &zone, const std::filesystem::path &out_dir, const Log &log,
                                         const StreamOptions &options, StreamReport *report);

// -- the memory a converted map may use on the console (the Python t4ff's memory.py)
constexpr uint64_t MIB = 1024 * 1024;
constexpr double MEMORY_TARGET_MIB = 212;
constexpr double FREE_MIB = 286.7;
constexpr double FREE_CONSOLE_MIB = FREE_MIB - (0x1E000000 - 0x19E00000) / (1024.0 * 1024.0);
constexpr int64_t MIN_TEXTURE_BUDGET_MIB = 24;
constexpr int64_t MARGIN_MIB = 1;

// the block sizes of a written (uncompressed) zone, from its header; the memory they take
std::vector<uint32_t> block_sizes(const std::vector<uint8_t> &zone);
uint64_t memory_bytes(const std::vector<uint32_t> &sizes);
// memory of the textures of a console zone (their pixel data)
uint64_t texture_bytes(Zone &zone);
// the texture budget for another conversion, nullopt when it fits or the textures cannot shrink
std::optional<int64_t> next_texture_budget(int64_t total, int64_t target, int64_t textures, int64_t budget, double efficiency = 1.0);
} // namespace t4ff
