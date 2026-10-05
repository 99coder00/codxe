#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/commands.h"
#include "core/layout.h"

// The generic T4 zone (decompressed fastfile) reader and writer (the Python t4ff's zone.py).
//
// The reader interprets OpenAssetTools' zone code commands against a platform's structure layouts,
// exactly as the game's DB_Load* functions do. It produces a tree of nodes: each one allocation the
// loader makes (an asset header, an array, a string...) with the bytes streamed for it and the
// pointers inside them. The writer replays the tree for a platform, recomputing block offsets,
// alignment and pointer encodings; reading a zone and writing it back for the same platform gives the
// original bytes.
namespace t4ff
{
constexpr uint32_t FOLLOW = 0xFFFFFFFF;
constexpr uint32_t INSERT = 0xFFFFFFFE;

enum Block : int
{
    BLOCK_TEMP = 0,
    BLOCK_RUNTIME = 1,
    BLOCK_LARGE_RUNTIME = 2,
    BLOCK_PHYSICAL_RUNTIME = 3,
    BLOCK_VIRTUAL = 4,
    BLOCK_LARGE = 5,
    BLOCK_PHYSICAL = 6,
    BLOCK_COUNT = 7,
};
constexpr int BLOCK_SHIFT = 29;
constexpr uint32_t OFFSET_MASK = (1u << BLOCK_SHIFT) - 1;

int block_by_name(const std::string &name); // "XFILE_BLOCK_VIRTUAL" -> BLOCK_VIRTUAL
const char *block_name(int block);

inline uint32_t zone_addr(int block, uint32_t offset)
{
    return (static_cast<uint32_t>(block) << BLOCK_SHIFT) | (offset & OFFSET_MASK);
}

struct ZoneError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Node;

// A pointer stored inside a node's data.
struct Ptr
{
    enum class Kind : uint8_t
    {
        Null,   // nullptr
        Follow, // data follows in the stream (-1); node is the loaded child
        Insert, // like follow, an alias slot reserved in the insert block (-2)
        Ref,    // offset into data loaded before: node + index / inner
        Alias,  // offset to a pointer slot loaded before: slot
    };
    Kind kind = Kind::Null;
    Node *node = nullptr;
    uint32_t index = 0;
    uint32_t inner = 0;
    Ptr *slot = nullptr;
    Node *owner = nullptr;
    uint32_t offset = 0;
    std::optional<uint32_t> addr;        // zone address of this pointer slot (normal blocks only)
    std::optional<uint32_t> insert_addr; // zone address of the reserved insert slot

    Node *target() const; // the node it ultimately refers to
};

// A node's pointers by offset, in the order they were set (as a Python dict).
class Relocs
{
  public:
    void set(uint32_t offset, Ptr *ptr);
    Ptr *get(uint32_t offset) const;
    bool empty() const
    {
        return items.empty();
    }
    size_t size() const
    {
        return items.size();
    }
    const std::vector<std::pair<uint32_t, Ptr *>> &list() const
    {
        return items;
    }

  private:
    std::vector<std::pair<uint32_t, Ptr *>> items;
    std::unordered_map<uint32_t, size_t> index; // once there are many
};

struct Segment
{
    const TypeRef *type;
    uint32_t count;
    uint32_t size;
    bool partial;
};

enum class Origin : uint8_t
{
    None,
    Asset,    // (asset, record)
    Member,   // (member, record, field)
    PtrArray, // (ptrarray, record, field)
    PtrElem,  // (ptrelem, record name or "")
};

// One allocation made by the zone loader.
struct Node
{
    const TypeRef *type = nullptr; // element type
    uint32_t count = 0;
    std::vector<uint8_t> data;
    int block = 0;
    Relocs relocs;
    std::vector<Node *> children;
    int push_before = -1; // block pushed before the allocation (-1: none)
    int push_after = -1;  // block pushed after the data was read (asset members)
    bool insert = false;  // an insert slot is reserved right after the allocation
    bool string = false;
    const char *asset = nullptr; // asset type name, for asset headers
    uint32_t offset = 0;         // block offset of the allocation (source platform while reading)
    uint32_t runtime_size = 0;   // size of allocations in non streamed runtime blocks
    std::vector<Segment> segments;

    // the Python node.extra entries the zone code uses
    std::optional<uint32_t> align;
    Origin origin = Origin::None;
    const std::string *origin_record = nullptr;
    const std::string *origin_field = nullptr;
    Ptr *ptr = nullptr; // the pointer that made it (follow / insert)
    bool delayed = false;
    std::optional<uint32_t> new_offset; // set by the writer

    uint32_t elem_size() const
    {
        return string ? 1 : type->size;
    }
    std::string repr() const;
};

struct ZoneAsset
{
    std::string type; // asset type name
    Ptr *ptr = nullptr;
    std::string name;
    Node *node() const
    {
        return ptr ? ptr->target() : nullptr;
    }
};

struct Zone
{
    std::string platform;
    std::vector<std::optional<std::string>> script_strings;
    std::vector<ZoneAsset> assets;
    std::vector<uint32_t> block_sizes;
    uint32_t size = 0;
    uint32_t external_size = 0;
    Node *script_node = nullptr;
    Node *assets_node = nullptr;
    Node *root = nullptr; // the asset list header (the Python extra_root)

    // owns every node and pointer of the zone
    std::deque<Node> node_store;
    std::deque<Ptr> ptr_store;
    Node *new_node()
    {
        return &node_store.emplace_back();
    }
    Ptr *new_ptr(Ptr::Kind kind)
    {
        Ptr *p = &ptr_store.emplace_back();
        p->kind = kind;
        return p;
    }

    // every node, depth first (the Python walk)
    template <typename F> void walk(F &&f) const
    {
        std::vector<Node *> stack{root};
        while (!stack.empty())
        {
            Node *n = stack.back();
            stack.pop_back();
            f(n);
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                stack.push_back(*it);
        }
    }
};

// A platform: its layouts, zone code commands and asset types.
class Platform
{
  public:
    Platform(std::string name, bool big_endian, std::unique_ptr<Layout> layout, std::unique_ptr<Commands> commands,
             std::vector<std::string> asset_types, std::vector<int> streamed_blocks);

    std::string name;
    bool big_endian;
    std::unique_ptr<Layout> layout;
    std::unique_ptr<Commands> cmds;
    std::vector<std::string> asset_types;
    uint32_t temp_padding;
    const TypeRef *char_type;     // TypeRef("scalar", "char", 1, 1)
    const TypeRef *uchar_type;    // TypeRef("scalar", "uchar", 1, 1)
    const TypeRef *uint_type;     // TypeRef("scalar", "uint", 4, 4)
    const TypeRef *char_ptr_type; // TypeRef("pointer", to=char)

    const Record &record(const std::string &rec_name) const
    {
        return layout->record(rec_name);
    }
    bool streamed(int block) const
    {
        return block >= 0 && (streamed_mask >> block) & 1;
    }
    bool is_asset(const Record &rec) const;
    std::optional<int> type_block(const Record &rec) const;
    uint32_t type_alloc_align(const std::string &rec_name, uint32_t fallback) const;

    const MemberInfos *member_infos(const Record &rec, const Field &f) const;
    bool member_is_leaf(const Record &rec, const Field &f) const;
    bool type_is_leaf(const TypeRef &t) const;
    bool record_is_leaf(const Record &rec) const;
    const Field *dynamic_member(const Record &rec) const;
    const std::vector<const Field *> &ordered_members(const Record &rec) const;

    uint32_t u32(const uint8_t *p) const
    {
        return big_endian ? (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3])
                          : (uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]);
    }
    void put_u32(uint8_t *p, uint32_t v) const
    {
        if (big_endian)
        {
            p[0] = uint8_t(v >> 24);
            p[1] = uint8_t(v >> 16);
            p[2] = uint8_t(v >> 8);
            p[3] = uint8_t(v);
        }
        else
        {
            p[0] = uint8_t(v);
            p[1] = uint8_t(v >> 8);
            p[2] = uint8_t(v >> 16);
            p[3] = uint8_t(v >> 24);
        }
    }
    int64_t read_scalar(const uint8_t *data, const TypeRef &t) const;

    // Computes the lazily cached answers about every record, after which the platform is read only
    // and zones can be read on several threads at once.
    void prepare();

  private:
    uint32_t streamed_mask = 0;
    std::unordered_map<const Record *, bool> asset_records;
    mutable std::unordered_map<const Field *, const MemberInfos *> infos_cache;
    mutable std::unordered_map<const Field *, bool> leaf_members;
    mutable std::unordered_map<const Record *, bool> leaf_records;
    mutable std::unordered_map<const Record *, const Field *> dynamic_cache;
    mutable std::unordered_map<const Record *, std::vector<const Field *>> ordered_cache;
};

const char *const *pc_asset_types(size_t *count);
const char *asset_record_name(const std::string &asset_type); // ASSET_RECORDS, nullptr when unsupported

// Reads a zone (the decompressed fastfile) of a platform.
std::unique_ptr<Zone> read_zone(const Platform &p, const std::vector<uint8_t> &data);

// Writes a zone for a platform: header, streamed blocks and delayed data.
std::vector<uint8_t> write_zone(const Platform &p, Zone &zone);

// Best effort name of an asset header node.
std::string asset_name(const Platform &p, const Node &node);
} // namespace t4ff
