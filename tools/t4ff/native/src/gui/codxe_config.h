#pragma once

#include <filesystem>
#include <string>
#include <vector>

// CoD Xe's settings file, codxe.json: CoD Xe reads _codxe\t4\codxe.json when the game starts (or
// _codxe\codxe.json without the t4 folder). The window writes its debug switches there with a map,
// and the command that starts a map as soon as the game is up, as the headless tests did
// (dev/xenia/README.md). The file's other settings stay as they are.
namespace t4ff::gui
{
namespace fs = std::filesystem;

struct CodxeConfig
{
    bool log_console = false;   // the game's console in the debug output (xenia.log, xbWatson)
    bool thread_watch = false;  // what each thread runs, for a map that hangs without an error
    bool dump_rawfile = false;  // the scripts the game loads, into _codxe\dump
    bool dump_map_ents = false; // the map's entities, into _codxe\dump
    std::string active_mod;     // empty: left as it is
    std::string start_map;      // started when the game is up; empty: no startup_command
    std::string start_command = "devmap";
};

// where CoD Xe reads it, for an output folder (t4_layout: _codxe\t4)
fs::path codxe_config_path(const fs::path &output, bool t4_layout);

// Writes config into the file at path, keeping the file's other settings, its order and line ends.
// Returns what it set, as a line for the log. Throws std::runtime_error when the file is not valid
// JSON (left untouched) or cannot be written.
std::string write_codxe_config(const fs::path &path, const CodxeConfig &config);
} // namespace t4ff::gui
