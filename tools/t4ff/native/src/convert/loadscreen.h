#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "convert/scripts.h"
#include "core/dxt.h"
#include "core/image.h"

// The loading screen zone of a map (<map>_load.ff) and the map's pictures and names for the map lists
// (the Python t4ff's loadscreen.py, and the map.json / preview files of menu.py).
//
// While a map loads, the game shows the material $levelbriefing of the map's load zone. CoD Xenon's
// maps all have the same load zone: a map's is made from one of theirs (among the console fastfiles
// given), with the map's own picture: one given for it, CoD Xenon's own when they converted the same
// map, the map's own PC loading screen, else a title card with the map's name.
namespace t4ff
{
struct LoadScreenError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// the base level of an image as RGBA
Pixels rgba_of(const ImageData &image);
// rgba scaled (Lanczos with FFmpeg, else bilinear)
Pixels resize(const Pixels &rgba, uint32_t width, uint32_t height);
// a picture file scaled, as RGBA
Pixels read_picture(const std::filesystem::path &path, uint32_t width, uint32_t height);

// the map's name: the longname of its entry in its .arena file (localized: the map's localized strings),
// else from its file name
std::string map_title(const IwdLibrary *map_files, const std::string &map_name, const std::vector<std::pair<std::string, std::string>> *localized);
std::string pretty_map_name(const std::string &map_name);
Pixels title_card(const std::string &title, uint32_t width, uint32_t height);

// <map>_load.ff into out_dir, from a CoD Xenon load zone among console_files; false when there is none
bool write_load_zone(const std::string &map_name, const std::filesystem::path &out_dir, const std::vector<std::filesystem::path> &console_files,
                     const IwdLibrary &map_files, const std::filesystem::path &pc_load_zone, const std::filesystem::path &picture_path, int jobs,
                     const Log &log, const std::string &title);
// the loading screen picture of a console load zone made like CoD Xenon's
std::optional<Pixels> load_zone_picture(const std::filesystem::path &path);
// preview.bin (the map's picture in t4ff's map list) from the map's loading screen
bool write_preview(const std::filesystem::path &map_dir, const std::string &map_name);
std::vector<uint8_t> preview_file(const Pixels &rgba);

// map.json from a description.txt, preview.dds from a picture, and both into a map's folder (CoD Xe's
// own custom maps list); returns the names of the files written
std::string map_json(const std::string &description);
std::vector<uint8_t> preview_dds(const Pixels &rgba);
std::vector<std::string> write_map_info(const std::filesystem::path &map_dir, const std::string &map_name, bool metadata = true, bool picture = true);
} // namespace t4ff
