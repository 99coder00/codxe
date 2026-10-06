#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "core/zone.h"

// Technique sets (the Python t4ff's techsets.py): the shader argument sections of copied technique
// sets, and the console technique set closest to a PC one no console fastfile has.
namespace t4ff
{
// Moves the shader arguments of the zone's technique sets to the sections the console game reads them
// from. Returns the number of passes changed.
int fix_argument_sections(const Platform &p, Zone &zone, const std::function<void(const std::string &)> &log);

using TechsetInfo = std::tuple<int, bool, bool>; // (worldVertFormat, world shaders, model shaders)
TechsetInfo techset_info(const Platform &p, const Node &node);

struct TechsetFeatures
{
    std::vector<std::string> words;
    std::set<std::string> textures, modes;
};
TechsetFeatures techset_features(const std::string &name);

// the technique set of candidates to draw what name (world vertex format wvf) would
std::optional<std::string> closest_techset(const std::string &name, int wvf, const std::map<std::string, TechsetInfo> &candidates);
} // namespace t4ff
