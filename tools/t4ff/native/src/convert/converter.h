#pragma once

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "audio/audio.h"
#include "convert/record_map.h"
#include "core/image.h"
#include "core/zone.h"

// PC -> Xbox 360 zone conversion (the Python t4ff's convert.py).
//
// The generic path converts every node of the PC zone tree into the console layout of the same
// record (record_map.h); asset types whose console format differs have a hook (assets.h). Only record
// layouts verified against real console fastfiles are converted by default: assets that would need
// others become name references (",name") the game resolves against its own zones.
namespace t4ff
{
class ConsoleLibrary;
class Cloner;

// encoded loaded sounds (or the error encoding them), shared by the zones converted together: by
// (console name, PC data size)
struct SoundCache
{
    struct Entry
    {
        std::shared_ptr<LoadedXma> xma;
        std::string error;
    };
    std::mutex lock;
    std::map<std::pair<std::string, size_t>, Entry> entries;
};

struct ConvertOptions
{
    bool allow_unverified = false;
    uint32_t max_texture_size = 0; // images: largest side, 0 for no limit
    uint64_t texture_budget = 0;   // bytes of textures, 0 for no limit
    bool keep_mips = true;
    bool mip_tail = true; // false: textures without their packed mip tail
    bool compress_textures = true;
    std::vector<std::filesystem::path> iwd_paths;   // the map's own .iwd files and folders
    std::vector<std::filesystem::path> stock_paths; // the PC game's own files (stock textures)
    bool reference_missing_images = true;
    // sounds: without an encoder loaded sounds are references to console sounds
    std::shared_ptr<XmaEncoder> xma_encoder;
    std::shared_ptr<SoundCache> sound_cache = std::make_shared<SoundCache>();
    uint32_t sound_rate = 0;
    bool mono_sounds = false;
    std::filesystem::path sounds_dir; // the map's output folder
    std::vector<std::filesystem::path> console_zones;
    std::string map_name;
    std::function<void(const std::string &)> log;
    int jobs = 0;
    bool reference_techsets = false;
    // images an earlier conversion streamed (lower case name -> levels streamed, 2 deep)
    std::unordered_map<std::string, int> stream_steps;
    uint64_t texture_cut = 0;
    bool eighth_levels = true;
};

struct ConvertStats
{
    std::map<std::string, int> converted, referenced, copied;
    uint64_t texture_bytes = 0;
    uint64_t sound_bytes = 0;
    std::vector<std::string> warnings;
};

class ZoneConverter;
using AssetHook = Node *(*)(ZoneConverter &conv, const std::string &asset_type, Node *node, const std::string &name);
using OffsetMap = std::function<uint32_t(uint32_t)>;

class ZoneConverter
{
  public:
    ZoneConverter(std::unique_ptr<Zone> zone, const Platform &src, const Platform &dst, ConvertOptions options);
    ~ZoneConverter();

    // the console zone (the converter cannot be used again)
    std::unique_ptr<Zone> convert();
    // the console zone being made (where the nodes the hooks make go)
    Zone &out()
    {
        return *out_;
    }

    // -- what the hooks use
    Zone &zone; // the PC zone (its pointers become the console zone's)
    const Platform &src, &dst;
    ConvertOptions options;
    ConvertStats stats;
    std::unordered_map<const Node *, Node *> node_map; // PC node -> console node
    std::unordered_map<const Node *, OffsetMap> offset_maps;
    std::vector<std::pair<Ptr *, Node *>> ptrs; // pointers to fix up (pointer, owning console node)
    std::unordered_map<std::string, AssetHook> hooks;
    std::unordered_map<std::string, int> image_drop_levels;
    std::unordered_set<std::string> eighth_images; // lower case names
    bool textures_planned = false;
    uint64_t planned_texture_bytes = 0;
    std::unique_ptr<IwdLibrary> library, stock_library;
    std::vector<std::optional<std::string>> script_strings;
    ConsoleLibrary *console_library = nullptr;
    std::unique_ptr<Cloner> cloner;
    std::string progress_label;

    // state of the hooks (the Python conv.__dict__ entries)
    std::unordered_map<std::string, std::shared_ptr<const ImageData>> image_sources, stock_images, console_images;
    // console textures built before the conversion, on every processor (assets.prebuild_textures)
    struct PrebuiltTexture
    {
        std::optional<ConsoleTexture> texture;
        std::string error;
    };
    std::unordered_map<std::string, PrebuiltTexture> prebuilt_textures;
    // image sources read before they are asked for, on every processor (assets.prefetch_image_sources):
    // the source and the warnings reading it gave, said when it is first asked for
    struct PrefetchedImage
    {
        std::shared_ptr<const ImageData> source;
        std::vector<std::string> warnings;
    };
    std::unordered_map<std::string, PrefetchedImage> prefetched_images;
    std::vector<std::tuple<Node *, uint32_t, std::string>> string_edits;
    std::unordered_set<const Node *> sound_files_done;
    bool warned_no_encoder = false;
    std::optional<std::set<std::string>> material_techsets;

    void log(const std::string &msg) const;
    void warn(const std::string &msg);

    Node *new_node(const TypeRef *type, uint32_t count, int block);
    Ptr *new_ptr(Ptr::Kind kind);
    const TypeRef *dst_type(const TypeRef *t);
    const TypeRef *dst_record_type(const std::string &name);
    const TypeMap &type_map(const TypeRef *t, bool partial = false);
    const RecordMap &record_map(const std::string &name, bool partial = false);
    uint32_t dst_alignment(const Node &node, const TypeRef &dst_type);

    std::set<std::string> unverified_records(Node *node) const;
    Node *convert_node(Node *node);
    Node *convert_child(Node *child);
    Node *convert_asset_node(const std::string &asset_type, Node *node);
    Node *from_library(const std::string &asset_type, const std::string &name, Node *src_node = nullptr, bool unsafe_ok = false);
    Node *reference_asset(const std::string &asset_type, Node *node, const std::string &name);
    void register_converted(const std::string &asset_type, const std::string &name, Node *node);
    void fix_pointers(Node *root);

  private:
    std::unique_ptr<Zone> out_; // the console zone being made: it owns the nodes made
    std::shared_ptr<Zone> source_;
    std::set<std::string> verified;
    std::map<std::pair<std::string, bool>, std::unique_ptr<RecordMap>> record_maps;
    std::unordered_map<const TypeRef *, std::unique_ptr<TypeMap>> other_maps;
    std::unordered_map<const TypeRef *, const TypeRef *> dst_types;
    std::deque<TypeRef> type_store;
    std::unique_ptr<ScalarMap> pointer_map;

    bool dst_has_member(const std::string &rec, const std::string &member) const;
    uint32_t runtime_size(const Node &node);
    Node *convert_assets_node(Node *node);
    std::unordered_map<Ptr *, Node *> map_pointers(size_t start);
    void adopt_orphans(Node *root, std::unordered_map<Ptr *, Node *> orphans);
};

// The pointers of root and all it loads, in the order the game loads them: the data a pointer loads
// comes right after it, before the next pointer of its owner. Data loaded while walking (load_here)
// is walked too: whether a pointer loads what follows is decided when the walk goes on past it.
class LoadOrderWalk
{
  public:
    explicit LoadOrderWalk(Node *root);
    bool next(Ptr *&ptr, Node *&owner);

  private:
    struct Frame
    {
        Node *node;
        std::vector<Ptr *> relocs;
        size_t at = 0;
        bool started = false;
    };
    std::vector<Frame> stack;
    std::unordered_set<const Node *> visited;
    Ptr *last_ptr = nullptr;
    Node *last_owner = nullptr;
};

// A copy of the string ptr points into, from where it points.
Node *string_copy(Zone &zone, Ptr *ptr, const Node *source);
// Makes ptr of owner load child: among the data owner loads, in the order of their pointers.
void load_here(Ptr *ptr, Node *owner, Node *child, Ptr::Kind kind = Ptr::Kind::Follow);

// asset_name, "" when it fails
std::string asset_display_name(const Platform &p, const Node &node);
// the asset type of an asset record (the first of ASSET_RECORDS)
std::string asset_type_of(const std::string &rec_name);
} // namespace t4ff
