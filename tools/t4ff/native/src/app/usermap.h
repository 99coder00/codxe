#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The fastfiles and .iwd files of a PC usermap (the Python t4ff's find_usermap).
namespace t4ff
{
namespace fs = std::filesystem;

// a message for the user (the Python's SystemExit): printed, exit code 1
struct UserError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Usermap
{
    std::string name;
    fs::path map;
    std::optional<fs::path> mod, patch, load;
    std::vector<fs::path> localized; // the mod's language zones
    std::vector<fs::path> iwds;
};

// The map of a usermap folder or of one of its fastfiles. mod.ff is taken from the same folder, or
// from mods/<map> when the map is in usermaps/<map>; a mod's localized_*.ff takes the place of the
// game's language zone of that name.
Usermap find_usermap(const fs::path &path);
} // namespace t4ff
