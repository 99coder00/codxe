#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

// The bytes of a decompressed zone: in memory, or a mapped file of the zone cache (zone_cache.h).
class ZoneBytes
{
  public:
    virtual ~ZoneBytes() = default;
    const uint8_t *data() const
    {
        return data_;
    }
    size_t size() const
    {
        return size_;
    }

  protected:
    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
};

std::shared_ptr<const ZoneBytes> zone_bytes(std::vector<uint8_t> bytes);

// A node's bytes: a view into the zone it was read from (which the zone keeps alive), or bytes of its
// own, which changing them makes them (a conversion's).
class Bytes
{
  public:
    Bytes() = default;
    Bytes(const Bytes &o) : ptr_(o.ptr_), size_(o.size_), own_(o.own_ ? std::make_unique<std::vector<uint8_t>>(*o.own_) : nullptr)
    {
    }
    Bytes(Bytes &&) noexcept = default;
    Bytes &operator=(const Bytes &o)
    {
        if (this != &o)
            *this = Bytes(o);
        return *this;
    }
    Bytes &operator=(Bytes &&) noexcept = default;

    const uint8_t *data() const
    {
        return own_ ? own_->data() : ptr_;
    }
    size_t size() const
    {
        return own_ ? own_->size() : size_;
    }
    bool empty() const
    {
        return size() == 0;
    }
    const uint8_t *begin() const
    {
        return data();
    }
    const uint8_t *end() const
    {
        return data() + size();
    }
    uint8_t operator[](size_t i) const
    {
        return data()[i];
    }
    bool is_view() const
    {
        return !own_;
    }

    // Appends n bytes read at p: the view grows when they follow it in the same buffer.
    void append_view(const uint8_t *p, size_t n)
    {
        if (!own_ && (size_ == 0 || ptr_ + size_ == p))
        {
            if (size_ == 0)
                ptr_ = p;
            size_ += n;
            return;
        }
        owned().insert(owned().end(), p, p + n);
    }
    // The bytes as a vector of the node's own (copied out of the zone the first time).
    std::vector<uint8_t> &owned()
    {
        if (!own_)
        {
            own_ = std::make_unique<std::vector<uint8_t>>(ptr_, ptr_ + size_);
            ptr_ = nullptr;
            size_ = 0;
        }
        return *own_;
    }
    uint8_t *mutable_data()
    {
        return owned().data();
    }
    void assign(std::vector<uint8_t> bytes)
    {
        own_ = std::make_unique<std::vector<uint8_t>>(std::move(bytes));
        ptr_ = nullptr;
        size_ = 0;
    }

  private:
    const uint8_t *ptr_ = nullptr;
    size_t size_ = 0;
    std::unique_ptr<std::vector<uint8_t>> own_;
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
    Node *node = nullptr;
    Ptr *slot = nullptr;
    Node *owner = nullptr;
    uint32_t index = 0;
    uint32_t inner = 0;
    uint32_t offset = 0;
    Kind kind = Kind::Null;
    std::optional<uint32_t> addr;        // zone address of this pointer slot (normal blocks only)
    std::optional<uint32_t> insert_addr; // zone address of the reserved insert slot

    Node *target() const; // the node it ultimately refers to
};

// A node's pointers by offset, in the order they were set (as a Python dict). Searched in place while
// there are few; a hash index is made once there are many (most nodes have none or a handful).
class Relocs
{
  public:
    Relocs() = default;
    Relocs(const Relocs &o) : items(o.items)
    {
        if (o.index)
            build_index();
    }
    Relocs(Relocs &&) noexcept = default;
    Relocs &operator=(const Relocs &o)
    {
        items = o.items;
        index.reset();
        if (o.index)
            build_index();
        return *this;
    }
    Relocs &operator=(Relocs &&) noexcept = default;

    void set(uint32_t offset, Ptr *ptr);
    Ptr *get(uint32_t offset) const;
    void clear()
    {
        items.clear();
        index.reset();
    }
    // removes the pointer at offset (the others keep their order); returns it, or nullptr
    Ptr *erase(uint32_t offset);
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
    static constexpr size_t INDEX_FROM = 16;
    std::vector<std::pair<uint32_t, Ptr *>> items;
    std::unique_ptr<std::unordered_map<uint32_t, size_t>> index; // once there are many
    void build_index();
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
    Bytes data;
    Relocs relocs;
    std::vector<Node *> children;
    std::vector<Segment> segments;
    const char *asset = nullptr; // asset type name, for asset headers
    uint32_t count = 0;
    uint32_t offset = 0;       // block offset of the allocation (source platform while reading)
    uint32_t runtime_size = 0; // size of allocations in non streamed runtime blocks
    int8_t block = 0;
    int8_t push_before = -1; // block pushed before the allocation (-1: none)
    int8_t push_after = -1;  // block pushed after the data was read (asset members)
    bool insert = false;     // an insert slot is reserved right after the allocation
    bool string = false;

    // the Python node.extra entries the zone code uses
    bool delayed = false;
    Origin origin = Origin::None;
    std::optional<uint32_t> align;
    std::optional<uint32_t> new_offset; // set by the writer
    const std::string *origin_record = nullptr;
    const std::string *origin_field = nullptr;
    Ptr *ptr = nullptr;   // the pointer that made it (follow / insert)
    bool library = false; // a copy of a console library asset (the Python extra "library")

    uint32_t elem_size() const
    {
        return string ? 1 : type->size;
    }
    std::string repr() const;

    // origin == (kind, record[, field]) as the Python tuples compare
    bool origin_is(Origin kind, std::string_view record, std::string_view field = {}) const
    {
        return origin == kind && origin_record && *origin_record == record &&
               (field.empty() || (origin_field && *origin_field == field));
    }
    bool is_record(std::string_view name) const
    {
        return type && type->kind == TypeKind::Record && type->name == name;
    }

    // this node and every node it holds, depth first (the Python Node.walk)
    template <typename F> void walk(F &&f)
    {
        std::vector<Node *> stack{this};
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

// A string kept for the life of the program (origin names of nodes made by a conversion).
const std::string *intern(std::string_view s);

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

    // the bytes the nodes read from it view, and what else its nodes use: the bytes of zones its
    // copies view, the zones a conversion or a merge took nodes and pointers from
    std::shared_ptr<const ZoneBytes> source;
    std::vector<std::shared_ptr<const void>> keep_alive;

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
const char *asset_type_of_record(const std::string &rec_name); // the first asset type of a record, nullptr when none

// Reads a zone (the decompressed fastfile) of a platform. Its nodes view the bytes, which the zone
// keeps.
std::unique_ptr<Zone> read_zone(const Platform &p, std::shared_ptr<const ZoneBytes> bytes);
inline std::unique_ptr<Zone> read_zone(const Platform &p, std::vector<uint8_t> bytes)
{
    return read_zone(p, zone_bytes(std::move(bytes)));
}

// Writes a zone for a platform: header, streamed blocks and delayed data.
std::vector<uint8_t> write_zone(const Platform &p, Zone &zone);

// Best effort name of an asset header node.
std::string asset_name(const Platform &p, const Node &node);
} // namespace t4ff
