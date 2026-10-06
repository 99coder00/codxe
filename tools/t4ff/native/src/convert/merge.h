#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "core/zone.h"

// Merging zones into one (a usermap's <map>_patch.ff and mod.ff into <map>.ff for the console; the
// Python t4ff's merge.py, its menu passes in step 5).
//
// Script strings of every zone are merged into one table and every script string field is remapped.
// Assets defined by more than one zone are kept once; later duplicates become name references. For
// scripts, string tables and localized strings the last zone's version is kept (it overrides the
// earlier ones on PC); for other assets the first one.
namespace t4ff
{
// offsets of every script string (u16) in one record / in the data of a node
const std::vector<uint32_t> &record_script_string_offsets(const Platform &p, const std::string &name, bool partial = false);
std::vector<uint32_t> node_script_string_offsets(const Platform &p, const Node &node);
void remap_script_strings(const Platform &p, Node *root, const std::vector<uint32_t> &mapping);

using Log = std::function<void(const std::string &)>;

// The zones merged into one (it keeps them: their nodes and pointers are its).
std::unique_ptr<Zone> merge_zones(const Platform &p, std::vector<std::unique_ptr<Zone>> zones, const Log &log);

// Removes top level name references of types nothing in the zone uses (technique sets the console
// lacks, which the PC zone lists). Returns how many.
int prune_references(const Platform &p, Zone &zone, const std::vector<std::string> &types, const Log &log);

struct AssetListEntry
{
    size_t index; // of the asset in the zone's list
    Ptr *ptr;
    Node *target;
};
// makes the zone's asset list keep (a subset of it, in order)
void set_asset_list(Zone &zone, const std::vector<AssetListEntry> &keep);

// Loads every named asset once: later nested copies point to the first one instead. key_of(record,
// name, node) says which assets are the same (default: type and name); nullopt leaves one alone.
using AssetKeyOf = std::function<std::optional<std::pair<std::string, std::string>>(const std::string &, const std::string &, Node *)>;
int dedupe_nested_assets(const Platform &p, Zone &zone, const Log &log, AssetKeyOf key_of = nullptr,
                         const std::string &what = "nested assets loaded by an earlier asset");

bool has_incoming_refs(Zone &zone, Node *target);
bool is_reference(const Platform &p, const Node *node);
} // namespace t4ff
