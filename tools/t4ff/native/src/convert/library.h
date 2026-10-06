#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/zone.h"

// Copying console assets from other Xbox 360 fastfiles (the Python t4ff's library.py).
//
// Some assets cannot be converted from PC data (technique sets carry compiled shaders; stock images
// or sounds may not be at hand as PC files): they are copied from Xbox 360 fastfiles instead, stock
// zones from the console game or maps converted by CoD Xenon. A copy stands alone: data the library
// asset shares with what was loaded before it in its zone is copied inline, and nested assets are
// copied at their first use; within the output zone, data copied once is shared by later copies.
namespace t4ff
{
struct LibraryError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// the game loads these zones itself, before the map
bool is_game_zone(const std::filesystem::path &path);
// written next to the fastfiles of every map t4ff converts
constexpr const char *T4FF_MARKER = "t4ff.txt";
bool made_by_t4ff(const std::filesystem::path &path);

// The fastfiles of paths (files, or folders searched in name order), each once, in the order they are
// read; first (the map being converted) comes first; t4ff's conversions and those in exclude are left
// out (into skipped).
std::vector<std::filesystem::path> library_files(const std::vector<std::filesystem::path> &paths, const std::string &first = "",
                                                 const std::vector<std::filesystem::path> &exclude = {},
                                                 std::vector<std::filesystem::path> *skipped = nullptr);

// Python's str.lower() of a latin-1 name
std::string lower_latin1(std::string s);

using LibraryEntry = std::pair<Zone *, Node *>;

// Assets of Xbox 360 fastfiles by type and name (loaded on first use, from the zone cache).
class ConsoleLibrary
{
  public:
    ConsoleLibrary(const Platform &p, std::vector<std::filesystem::path> paths, std::function<void(const std::string &)> log,
                   std::string first = "", std::vector<std::filesystem::path> exclude = {}, std::filesystem::path cache_dir = {});
    ~ConsoleLibrary();

    std::optional<LibraryEntry> find(const std::string &rec_name, const std::string &name);
    // whether every model and world vertex shader of a technique set is one of Treyarch's
    bool techset_safe(const Node &node);
    std::vector<std::string> names(const std::string &rec_name);
    bool in_game_zones(const std::string &rec_name, const std::string &name);
    bool is_stock_menu(const std::string &name);
    std::vector<std::pair<std::string, Node *>> game_rawfiles();
    std::optional<LibraryEntry> find_in_game_zones(const std::string &rec_name, const std::string &name);
    const std::vector<std::unique_ptr<Zone>> &zones()
    {
        load();
        return zones_;
    }

    // technique sets of the library that are safe, with their techset_info (assets._techset_substitute)
    std::optional<std::map<std::string, std::tuple<int, bool, bool>>> techset_candidates;

  private:
    const Platform &p;
    std::vector<std::filesystem::path> paths;
    std::function<void(const std::string &)> log_;
    std::string first;
    std::vector<std::filesystem::path> exclude;
    std::filesystem::path cache_dir;
    bool loaded = false;
    std::vector<std::unique_ptr<Zone>> zones_;
    std::map<std::pair<std::string, std::string>, LibraryEntry> index_; // (record, lower name)
    std::map<std::pair<std::string, std::string>, LibraryEntry> game_;
    std::unordered_set<std::string> ui_menus_;
    std::unordered_map<std::string, std::vector<LibraryEntry>> techsets_;
    std::unordered_set<std::string> treyarch_shaders_;

    void load();
    void log(const std::string &msg) const
    {
        if (log_)
            log_(msg);
    }
};

// The library of these paths, made once per process (shared by the zones of one run).
ConsoleLibrary &shared_library(const Platform &p, const std::vector<std::filesystem::path> &paths,
                               std::function<void(const std::string &)> log, const std::string &first = "",
                               const std::vector<std::filesystem::path> &exclude = {});

// The cache folder libraries use (empty: none, zones in memory). Default: the zone cache's.
void set_library_cache_dir(const std::filesystem::path &dir);

// Copies library assets into one output zone.
class Cloner
{
  public:
    using Replace = std::function<Node *(const std::string &rec, const std::string &name, Node *library_node)>;
    Cloner(const Platform &p, Zone &out, std::vector<std::optional<std::string>> &strings, Replace replace = nullptr);

    // ptr loads the asset name in the output zone
    void register_asset(const std::string &rec_name, const std::string &name, Ptr *ptr);
    Node *copy_asset(Zone &zone, Node *node);

  private:
    const Platform &p;
    Zone &out;
    Replace replace;
    std::vector<std::optional<std::string>> &strings; // script strings of the output zone (extended)
    std::unordered_map<std::string, size_t> string_index;
    std::unordered_map<const Node *, Node *> copies; // library node -> copy
    std::unordered_map<const Ptr *, Ptr *> slots;    // library pointer -> the pointer of the copy
    std::map<std::pair<std::string, std::string>, Ptr *> assets; // nested assets copied: (record, lower name) -> loader
    std::unordered_set<const Zone *> kept;

    uint16_t script_string(Zone &zone, uint32_t index);
    Node *copy(Zone &zone, Node *src);
    Ptr *add_child(Node *owner, uint32_t offset, Ptr::Kind kind, Node *child);
    Ptr *copy_ptr(Zone &zone, Node *owner, uint32_t offset, Ptr *ptr);
    Node *copy_or_replace(Zone &zone, Node *target, const std::optional<std::pair<std::string, std::string>> &key);
    std::optional<std::pair<std::string, std::string>> asset_key(const Node &node) const;
    Ptr *make_ptr(Ptr::Kind kind, Node *owner, uint32_t offset, Node *node = nullptr, uint32_t index = 0, uint32_t inner = 0,
                  Ptr *slot = nullptr);
};
} // namespace t4ff
