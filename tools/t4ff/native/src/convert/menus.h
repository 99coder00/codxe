#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "convert/scripts.h"
#include "core/zone.h"

// The menus of a usermap on the console (the Python t4ff's menu passes of merge.py and menu.py).
//
// The console shows its own front end, so the mod's versions of its menus go (or get names of their
// own), and so do videos nothing plays; the map's own pause menu stays when it offers more. PC script
// menus answer keys a controller has not, and a controller moves the focus between items: they get
// buttons, their inert items become decorations and the D-pad moves between buttons as they are laid
// out. The options the mod's own menus choose on PC go to the map's options.txt and are asked in game.
namespace t4ff
{
class ConsoleLibrary;

// lowercase names of the menus the zone's scripts open, close or precache
std::set<std::string> scripted_menu_names(const Platform &p, Zone &zone);
// names of the menus a menu list (or any node) loads
std::vector<std::string> menu_names(Node *node);

// (the menus that keep their names, the menus that joined a script menu list)
std::pair<std::unordered_set<const Node *>, std::vector<std::string>> bind_pause_menu(const Platform &p, Zone &zone,
                                                                                      const std::vector<std::string> &ingame_menus, const Log &log);
std::vector<std::string> drop_frontend_menus(const Platform &p, Zone &zone, const std::function<bool(const std::string &)> &is_stock_menu,
                                             const Log &log, const std::function<bool(const std::string &)> &is_stock_list,
                                             const std::unordered_set<const Node *> &keep_menus);
std::vector<std::string> drop_unused_videos(const Platform &p, Zone &zone, const Log &log);
std::vector<std::string> rename_game_menus(Zone &zone, const std::function<bool(const std::string &)> &is_stock_menu,
                                           const std::set<std::string> &keep_names, const Log &log, const std::unordered_set<const Node *> &keep_menus);

std::vector<std::string> gamepad_script_menus(const Platform &p, Zone &zone, const Log &log);
std::map<std::string, int> decorate_inert_items(const Platform &p, Zone &zone, const Log &log, const std::set<std::string> *script_menus);
std::map<std::string, int> controller_navigation(const Platform &p, Zone &zone, const Log &log, const std::set<std::string> *script_menus);

// the options of the PC zones' menus whose dvars the map's scripts read (p: the PC platform)
std::vector<MapOption> menu_options(const Platform &p, const std::vector<Zone *> &zones, const std::set<std::string> &read_dvars,
                                    const MenuValues &menu_values);
std::optional<std::filesystem::path> write_options(const std::vector<MapOption> &options, const std::filesystem::path &out_dir, const Log &log);
// the in game menus of the options (the restart menu last); none without a menu list to hold them
std::vector<std::string> add_options_menus(const Platform &p, Zone &zone, ConsoleLibrary *library, const std::vector<MapOption> &options,
                                           const Log &log);

// the localized strings of a zone (key without "@" -> text), in the order of the zone
std::vector<std::pair<std::string, std::string>> localized_strings(const Platform &p, Zone &zone);
} // namespace t4ff
