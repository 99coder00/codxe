#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "audio/soundbudget.h"
#include "convert/stream.h"

// The window's settings (the Python t4ff's gui.Settings), remembered between runs in
// %APPDATA%\t4ff\window.json. The fields the Python window has keep its names, and its gui.json is read
// when window.json does not exist yet. Paths and texts are UTF-8.
namespace t4ff::gui
{
namespace fs = std::filesystem;

struct Settings
{
    std::string output, xma_encoder; // xma2encode.exe: empty for the one found on this computer
    std::vector<std::string> console_zones, iwds;
    // memory and textures
    std::string texture_budget = "auto"; // MiB, 0 for no limit, or auto (what the memory target leaves)
    double memory_target = MEMORY_TARGET_MIB;
    int max_texture_size = 0;
    bool no_mips = false, no_compress = false;
    bool stream_textures = true, keep_quarter = false, keep_mip_tail = false;
    double upgrade_budget = 96, stream_growth = 0;
    std::string deep_stream;
    // sounds
    int sound_rate = 0, stream_rate = 0, xma_quality = 60, max_loaded_sounds = DEFAULT_MAX_LOADED_SOUNDS, jobs = 0;
    double loaded_sound_memory = DEFAULT_LOADED_SOUND_MIB;
    bool mono_sounds = false, mono_streams = false, no_sounds = false;
    // the map's fastfiles and folder
    bool no_mod = false, no_patch = false, load_zone = true, t4_layout = true, allow_unverified = false;
    // CoD Xe's settings (codxe.json, codxe_config.h), written after each map converted
    bool codxe_settings = false, codxe_start_map = false;
    bool codxe_log_console = false, codxe_thread_watch = false, codxe_dump_rawfile = false, codxe_dump_map_ents = false;
    std::string codxe_start_command = "devmap", codxe_active_mod;
    // the window
    bool advanced = false;
    std::string theme = "system"; // system, dark or light
    int window_w = 0, window_h = 0;
    bool maximized = false;
};

// What belongs to one map (the Python window does not save these two): its name in the map lists and
// the picture of its loading screen.
struct MapEntry
{
    std::string input, name, loading_image;
};

fs::path settings_path();        // %APPDATA%\t4ff\window.json
fs::path python_settings_path(); // %APPDATA%\t4ff\gui.json (the Python window's)

// the settings of path; when it does not exist, those of the Python window (python); else the defaults
Settings load_settings(const fs::path &path, const fs::path &python);
void save_settings(const Settings &s, const fs::path &path);

// a texture budget setting as the command line takes it: "auto" or a number of MiB
std::string budget_text(const std::string &value);
// the command line of t4ff-cli (python -m t4ff) for converting the map with these settings
std::vector<std::string> convert_args(const Settings &s, const MapEntry &m);
// problems that prevent the conversion (none: it can run)
std::vector<std::string> check_settings(const Settings &s, const MapEntry &m);
// hints shown before a conversion (encoder_found: an xma2encode.exe was found on this computer)
std::vector<std::string> advice(const Settings &s, bool encoder_found);

// UTF-8 texts and paths
fs::path path_of(const std::string &utf8);
std::string utf8_of(const fs::path &p);
std::wstring wide_of(const std::string &utf8);
} // namespace t4ff::gui
