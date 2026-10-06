#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/zone.h"

// A dynamic, scrolling list of the usermaps, opened from the Nazi Zombies menu (the Python t4ff's
// menu.py, its menu command's part).
//
// CoD Xenon's patch_ui.ff lists their maps in the menu levels_unlock: the four stock maps, then one
// hand made row per converted map. make_dynamic keeps the stock rows and replaces the custom ones by one
// "Custom Maps" row, which opens a menu of its own (a copy of levels_unlock named levels_dev, a menu of
// the game nothing opens): rows whose text and command are dvars, which CoD Xe fills from the usermaps
// folder, and the preview of the focused map. Menu zones with CoD Xe's own list (codxe_usermaps, CoD
// Xenon's 0.3.0) are used as they are.
namespace t4ff
{
constexpr int USERMAPS_ROWS = 13;

// one of CoD Xenon's rows of converted maps
struct CustomRow
{
    std::string map;
    std::optional<std::string> title, description, image; // localized keys, preview picture
};

// makes the zone's map list dynamic; returns CoD Xenon's rows that were there (throws MenuError)
std::vector<CustomRow> make_dynamic(const Platform &p, Zone &zone, int rows = USERMAPS_ROWS);
// whether a menu zone has CoD Xe's own custom maps list
bool has_own_usermaps_list(const Zone &zone);
// why the zone is not CoD Xenon's patch_ui.ff make_dynamic starts from, nothing when it is
std::optional<std::string> not_cod_xenon_menu(const Platform &p, Zone &zone);
// the patch_ui.ff files of paths (fastfiles, or the folders of a game's or a release's _codxe\t4)
std::vector<std::filesystem::path> menu_zone_candidates(const std::vector<std::filesystem::path> &paths);
// a map's description.txt: its name, then its description
std::string description_text(const std::string &title, const std::string &description);
} // namespace t4ff
