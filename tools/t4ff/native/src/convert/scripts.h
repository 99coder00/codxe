#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/zone.h"

// Scripts of a usermap on the console (the Python t4ff's scripts.py and named.py).
//
// On PC, with the map's mod active, the game reads a script from the mod's own files before the
// fastfiles, and the scripts can use those of the game's own zones; the console reads scripts from
// fastfiles only, and the game's own zones there win over the map's. So the scripts of the zones take
// the content of the map's loose ones, scripts the map's scripts use but no zone of it has are added
// (from the map's files, the console fastfiles given or the PC game's files), the mod's versions of
// the game's scripts get names of their own, and the assets the game looks up by name are added. Then
// the scripts are fixed for the console (see each pass).
namespace t4ff
{
class ConsoleLibrary;
class IwdLibrary;
using Log = std::function<void(const std::string &)>;

constexpr const char *PLAYER_ANIM_SCRIPT = "mp/playeranim.script";
constexpr const char *USERMAP_SCRIPTS = "scripts";

// a script's name as scripts name it: no reference comma, forward slashes, lowercase
std::string normalize_script(std::string_view name);
bool is_script_name(std::string_view name); // .gsc or .csc (any case)

// the RawFile assets of a zone, (asset name, header), in walk order
std::vector<std::pair<std::string, Node *>> rawfiles(const Platform &p, Zone &zone);
Node *rawfile_buffer(Node *node);
std::string rawfile_text(Node *node);
void set_rawfile_text(const Platform &p, Node *node, const std::string &text);

// scripts the text includes or calls into (normalized names with the script's extension)
std::set<std::string> script_references(std::string_view name, std::string_view text);
std::set<std::string> anim_tree_references(std::string_view text);
// the string literals of the zone's scripts, lowercase (the menus they open among them)
std::set<std::string> script_strings(const Platform &p, Zone &zone);
// the dvars the zone's scripts read by name, lowercase
std::set<std::string> script_dvars(const Platform &p, Zone &zone);
bool is_zombie_map(const std::vector<std::pair<std::string, std::string>> &texts);

// {dvar: values} the strings of the zone (menu scripts) set with literal values
using MenuValues = std::map<std::string, std::set<std::string>>;
MenuValues menu_dvar_values(Zone &zone);

// an option of the map, which its own menus choose on PC (menu.menu_options)
struct MapOption
{
    std::string dvar, label, default_value;
    std::vector<std::pair<std::string, std::string>> choices; // (value, name)
};

// -- the passes (each returns what it changed, and logs it)
int override_scripts(const Platform &p, const std::vector<Zone *> &zones, const IwdLibrary &loose, const Log &log);
std::unique_ptr<Zone> missing_scripts_zone(const Platform &p, Zone &zone, const std::vector<const IwdLibrary *> &sources, ConsoleLibrary *library,
                                           const Log &log, const std::vector<std::string> &roots, std::set<std::string> *from_map);
std::unique_ptr<Zone> named_assets_zone(const Platform &p, Zone &zone, const std::vector<const IwdLibrary *> &sources, ConsoleLibrary *library,
                                        const Log &log);
std::map<std::string, std::string> keep_mod_scripts(const Platform &p, Zone &zone, const std::set<std::string> &mod_scripts, ConsoleLibrary *library,
                                                    const std::string &level_script, const Log &log);
std::vector<std::string> use_key_hints(const Platform &p, Zone &zone, const Log &log);
std::vector<std::string> fix_modder_help(const Platform &p, Zone &zone, const Log &log);
std::vector<std::string> spawn_script_origins(const Platform &p, Zone &zone, const Log &log);
std::vector<std::string> valid_cursor_hints(const Platform &p, Zone &zone, const Log &log);
std::vector<std::string> local_client_effects(const Platform &p, Zone &zone, const Log &log);
std::vector<std::string> precache_before_waits(const Platform &p, Zone &zone, const std::string &level_script, const Log &log);
std::map<std::string, std::string> menu_dvar_defaults(const Platform &p, Zone &zone, const MenuValues &menu_values, const std::string &level_script,
                                                      const Log &log);
std::vector<std::string> speed_up_zombies_only(const Platform &p, Zone &zone, const Log &log);
std::vector<std::string> zombie_idles_for_zombies(const Platform &p, Zone &zone, const Log &log);
std::vector<std::string> splitscreen_fog(const Platform &p, Zone &zone, const std::string &level_script,
                                         const std::function<bool(const std::string &)> &is_game_script, const Log &log);
int mounted_guns(const Platform &p, Zone &zone, const std::string &level_script, const Log &log);
bool reset_sustain_ammo(const Platform &p, Zone &zone, const std::string &level_script, const Log &log);
bool options_script(const Platform &p, Zone &zone, const std::string &level_script, const std::vector<MapOption> &options,
                    const std::vector<std::string> &menus, const Log &log);
// the mod's versions of the game's scripts that keep no name of their own: {name: text}
std::map<std::string, std::string> usermap_scripts(const Platform &p, Zone &zone, const std::set<std::string> &mod_scripts, ConsoleLibrary *library,
                                                   const std::map<std::string, std::string> &renamed);
std::vector<std::string> write_usermap_scripts(const std::map<std::string, std::string> &scripts, const std::filesystem::path &out_dir,
                                               const Log &log);
} // namespace t4ff
