#include "core/zone.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <mutex>

namespace t4ff
{
namespace
{
const char *BLOCK_NAMES[] = {
    "XFILE_BLOCK_TEMP",    "XFILE_BLOCK_RUNTIME", "XFILE_BLOCK_LARGE_RUNTIME", "XFILE_BLOCK_PHYSICAL_RUNTIME",
    "XFILE_BLOCK_VIRTUAL", "XFILE_BLOCK_LARGE",   "XFILE_BLOCK_PHYSICAL",
};

const char *PC_ASSET_TYPES[] = {
    "xmodelpieces", "physpreset", "physconstraints", "destructibledef", "xanim", "xmodel", "material",
    "techset", "image", "sound", "loaded_sound", "clipmap", "clipmap_pvs", "comworld", "gameworld_sp",
    "gameworld_mp", "map_ents", "gfxworld", "lightdef", "ui_map", "font", "menulist", "menu", "localize",
    "weapon", "snddriverglobals", "fx", "impactfx", "aitype", "mptype", "character", "xmodelalias",
    "rawfile", "stringtable", "packindex",
};

struct AssetRecord
{
    const char *type;
    const char *record;
};
const AssetRecord ASSET_RECORDS[] = {
    {"physpreset", "PhysPreset"},
    {"physconstraints", "PhysConstraints"},
    {"destructibledef", "DestructibleDef"},
    {"xanim", "XAnimParts"},
    {"xmodel", "XModel"},
    {"material", "Material"},
    {"pixelshader", "MaterialPixelShader"},
    {"techset", "MaterialTechniqueSet"},
    {"image", "GfxImage"},
    {"sound", "snd_alias_list_t"},
    {"loaded_sound", "LoadedSound"},
    {"clipmap", "clipMap_t"},
    {"clipmap_pvs", "clipMap_t"},
    {"comworld", "ComWorld"},
    {"gameworld_sp", "GameWorldSp"},
    {"gameworld_mp", "GameWorldMp"},
    {"map_ents", "MapEnts"},
    {"gfxworld", "GfxWorld"},
    {"lightdef", "GfxLightDef"},
    {"font", "Font_s"},
    {"menulist", "MenuList"},
    {"menu", "menuDef_t"},
    {"localize", "LocalizeEntry"},
    {"weapon", "WeaponDef"},
    {"snddriverglobals", "SndDriverGlobals"},
    {"fx", "FxEffectDef"},
    {"impactfx", "FxImpactTable"},
    {"rawfile", "RawFile"},
    {"stringtable", "StringTable"},
    {"packindex", "PackIndex"},
};

bool indexed_block(int block)
{
    return block == BLOCK_VIRTUAL || block == BLOCK_LARGE || block == BLOCK_PHYSICAL;
}

std::string hex(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%#x", v);
    return buf;
}

const Record &rec_of(const TypeRef &t)
{
    if (!t.record)
        throw ZoneError("no record " + t.name);
    return *t.record;
}
} // namespace

int block_by_name(const std::string &name)
{
    for (int i = 0; i < BLOCK_COUNT; ++i)
        if (name == BLOCK_NAMES[i])
            return i;
    throw ZoneError("unknown block " + name);
}

const char *block_name(int block)
{
    return block >= 0 && block < BLOCK_COUNT ? BLOCK_NAMES[block] : "?";
}

const char *const *pc_asset_types(size_t *count)
{
    *count = std::size(PC_ASSET_TYPES);
    return PC_ASSET_TYPES;
}

const char *asset_type_of_record(const std::string &rec_name)
{
    for (const auto &a : ASSET_RECORDS)
        if (rec_name == a.record)
            return a.type;
    return nullptr;
}

const char *asset_record_name(const std::string &asset_type)
{
    for (const auto &a : ASSET_RECORDS)
        if (asset_type == a.type)
            return a.record;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Data model

Node *Ptr::target() const
{
    const Ptr *p = this;
    while (p && p->kind == Kind::Alias)
        p = p->slot;
    return p ? p->node : nullptr;
}

void Relocs::build_index()
{
    index = std::make_unique<std::unordered_map<uint32_t, size_t>>();
    index->reserve(items.size() * 2);
    for (size_t i = 0; i < items.size(); ++i)
        index->emplace(items[i].first, i);
}

void Relocs::set(uint32_t offset, Ptr *ptr)
{
    if (index)
    {
        auto [it, added] = index->emplace(offset, items.size());
        if (added)
            items.emplace_back(offset, ptr);
        else
            items[it->second].second = ptr;
        return;
    }
    for (auto &item : items)
    {
        if (item.first == offset)
        {
            item.second = ptr;
            return;
        }
    }
    items.emplace_back(offset, ptr);
    if (items.size() > INDEX_FROM)
        build_index(); // from now on: get never changes anything, as zones are read on several threads
}

Ptr *Relocs::erase(uint32_t offset)
{
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (items[i].first != offset)
            continue;
        Ptr *ptr = items[i].second;
        items.erase(items.begin() + i);
        if (index)
            build_index();
        return ptr;
    }
    return nullptr;
}

Ptr *Relocs::get(uint32_t offset) const
{
    if (index)
    {
        auto it = index->find(offset);
        return it == index->end() ? nullptr : items[it->second].second;
    }
    for (const auto &item : items)
        if (item.first == offset)
            return item.second;
    return nullptr;
}

const std::string *intern(std::string_view s)
{
    static std::mutex lock;
    static std::unordered_map<std::string_view, std::unique_ptr<std::string>> strings;
    std::lock_guard guard(lock);
    auto it = strings.find(s);
    if (it != strings.end())
        return it->second.get();
    auto owned = std::make_unique<std::string>(s);
    const std::string *p = owned.get();
    strings.emplace(*p, std::move(owned));
    return p;
}

std::string Node::repr() const
{
    std::string what = string ? "string" : type->repr();
    return "<Node " + what + " x" + std::to_string(count) + " blk=" + std::to_string(block) + " off=" + hex(offset) +
           " size=" + std::to_string(data.size()) + ">";
}

// ---------------------------------------------------------------------------
// Platform

Platform::Platform(std::string name_, bool big_endian_, std::unique_ptr<Layout> layout_, std::unique_ptr<Commands> commands_,
                   std::vector<std::string> asset_types_, std::vector<int> streamed_blocks)
    : name(std::move(name_)), big_endian(big_endian_), layout(std::move(layout_)), cmds(std::move(commands_)),
      asset_types(std::move(asset_types_))
{
    temp_padding = name == "pc" ? 16 : 0;
    for (int b : streamed_blocks)
        streamed_mask |= 1u << b;
    for (const auto &[rec, e] : cmds->assets)
        if (const Record *r = layout->find(rec))
            asset_records[r] = true;
    char_type = layout->scalar(Scalar::Char);
    uchar_type = layout->scalar(Scalar::UChar);
    uint_type = layout->scalar(Scalar::UInt);
    TypeRef *cp = layout->new_type();
    cp->kind = TypeKind::Pointer;
    cp->size = 4;
    cp->align = 4;
    cp->to = char_type;
    char_ptr_type = cp;
}

bool Platform::is_asset(const Record &rec) const
{
    return asset_records.count(&rec) != 0;
}

std::optional<int> Platform::type_block(const Record &rec) const
{
    const TypeInfo *info = cmds->find_type(rec.name);
    if (!info || !info->block)
        return std::nullopt;
    return block_by_name(*info->block);
}

uint32_t Platform::type_alloc_align(const std::string &rec_name, uint32_t fallback) const
{
    const TypeInfo *info = cmds->find_type(rec_name);
    if (info && info->allocalign)
        return static_cast<uint32_t>(info->allocalign->eval(nullptr));
    return fallback;
}

const MemberInfos *Platform::member_infos(const Record &rec, const Field &f) const
{
    auto it = infos_cache.find(&f);
    if (it != infos_cache.end())
        return it->second;
    const MemberInfos *infos = cmds->member_infos(rec.name, f.name);
    if (infos && infos->empty())
        infos = nullptr;
    infos_cache.emplace(&f, infos);
    return infos;
}

bool Platform::member_is_leaf(const Record &rec, const Field &f) const
{
    auto it = leaf_members.find(&f);
    if (it != leaf_members.end())
        return it->second;
    bool leaf;
    const MemberInfos *infos = member_infos(rec, f);
    bool never = false, dynamic = false;
    if (infos)
    {
        for (const auto &[ctx, info] : *infos)
        {
            if (info.condition && info.condition->is_never())
                never = true;
            if (info.arraysize || info.string)
                dynamic = true;
        }
    }
    if (never)
        leaf = true;
    else if (dynamic)
        leaf = false;
    else
        leaf = type_is_leaf(*f.type);
    leaf_members[&f] = leaf;
    return leaf;
}

bool Platform::type_is_leaf(const TypeRef &t) const
{
    switch (t.kind)
    {
    case TypeKind::Scalar:
    case TypeKind::Enum:
    case TypeKind::Void: return true;
    case TypeKind::Pointer: return false;
    case TypeKind::Array: return type_is_leaf(*t.elem);
    case TypeKind::Record: return record_is_leaf(rec_of(t));
    }
    return true;
}

bool Platform::record_is_leaf(const Record &rec) const
{
    auto it = leaf_records.find(&rec);
    if (it != leaf_records.end())
        return it->second;
    leaf_records[&rec] = true; // guards recursion through self referencing records
    bool all = true;
    for (const Field &f : rec.fields)
    {
        if (!member_is_leaf(rec, f))
        {
            all = false;
            break;
        }
    }
    leaf_records[&rec] = all;
    return all;
}

const Field *Platform::dynamic_member(const Record &rec) const
{
    auto it = dynamic_cache.find(&rec);
    if (it != dynamic_cache.end())
        return it->second;
    dynamic_cache[&rec] = nullptr;
    const Field *found = nullptr;
    for (const Field &f : rec.fields)
    {
        const MemberInfos *infos = member_infos(rec, f);
        bool arraysize = false;
        if (infos)
            for (const auto &[ctx, info] : *infos)
                if (info.arraysize)
                    arraysize = true;
        if (arraysize)
            found = &f;
        else if (f.type->kind == TypeKind::Record && dynamic_member(rec_of(*f.type)) != nullptr)
            found = &f;
    }
    dynamic_cache[&rec] = found;
    return found;
}

const std::vector<const Field *> &Platform::ordered_members(const Record &rec) const
{
    auto it = ordered_cache.find(&rec);
    if (it != ordered_cache.end())
        return it->second;
    std::vector<const Field *> fields;
    for (const Field &f : rec.fields)
        if (!f.name.empty())
            fields.push_back(&f);
    const TypeInfo *info = cmds->find_type(rec.name);
    if (info && info->reorder && !info->reorder->empty())
    {
        const std::vector<std::string> &order = *info->reorder;
        auto listed_in = [](const std::vector<std::string> &names, size_t from, const std::string &n) {
            return std::find(names.begin() + from, names.end(), n) != names.end();
        };
        std::vector<const Field *> result;
        if (order[0] == "...")
        {
            // OAT: declaration order up to and including the first listed member, then the other
            // listed members, then the remaining ones
            const std::string &first = order.at(1);
            std::vector<const Field *> rest;
            for (const Field *f : fields)
                if (!listed_in(order, 2, f->name))
                    rest.push_back(f);
            size_t split = 0;
            while (split < rest.size() && rest[split]->name != first)
                ++split;
            if (split == rest.size())
                throw ZoneError("reorder of " + rec.name + ": no member " + first);
            ++split;
            result.assign(rest.begin(), rest.begin() + split);
            for (size_t i = 2; i < order.size(); ++i)
                result.push_back(rec.field(order[i]));
            result.insert(result.end(), rest.begin() + split, rest.end());
        }
        else
        {
            for (const std::string &n : order)
                result.push_back(rec.field(n));
            for (const Field *f : fields)
                if (!listed_in(order, 0, f->name))
                    result.push_back(f);
        }
        fields = std::move(result);
    }
    return ordered_cache.emplace(&rec, std::move(fields)).first->second;
}

void Platform::prepare()
{
    for (const Record *rec : layout->all())
    {
        try
        {
            for (const Field &f : rec->fields)
            {
                member_infos(*rec, f);
                member_is_leaf(*rec, f);
            }
            record_is_leaf(*rec);
            dynamic_member(*rec);
            ordered_members(*rec);
        }
        catch (const std::exception &)
        {
            // a record the zone code cannot load (an unknown member type): reading one fails anyway
        }
    }
}

int64_t Platform::read_scalar(const uint8_t *p, const TypeRef &t) const
{
    auto u16 = [&]() -> uint16_t { return big_endian ? uint16_t(p[0] << 8 | p[1]) : uint16_t(p[1] << 8 | p[0]); };
    auto u64 = [&]() -> uint64_t {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= uint64_t(p[big_endian ? i : 7 - i]) << (8 * (7 - i));
        return v;
    };
    if (t.kind == TypeKind::Enum)
    {
        switch (t.size)
        {
        case 1: return p[0];
        case 2: return u16();
        case 4: return static_cast<int32_t>(u32(p));
        }
        throw ZoneError("cannot evaluate enum of size " + std::to_string(t.size));
    }
    if (t.kind == TypeKind::Pointer)
        return u32(p);
    if (t.kind != TypeKind::Scalar)
        throw ZoneError("cannot evaluate " + t.repr());
    switch (t.scalar)
    {
    case Scalar::Char:
    case Scalar::SChar: return static_cast<int8_t>(p[0]);
    case Scalar::UChar:
    case Scalar::Bool: return p[0];
    case Scalar::Short: return static_cast<int16_t>(u16());
    case Scalar::UShort: return u16();
    case Scalar::Int:
    case Scalar::Long: return static_cast<int32_t>(u32(p));
    case Scalar::UInt:
    case Scalar::ULong: return u32(p);
    case Scalar::LongLong: return static_cast<int64_t>(u64());
    case Scalar::ULongLong: return static_cast<int64_t>(u64());
    case Scalar::Float: {
        uint32_t bits = u32(p);
        float f;
        std::memcpy(&f, &bits, 4);
        return static_cast<int64_t>(f);
    }
    case Scalar::Double: {
        uint64_t bits = u64();
        double d;
        std::memcpy(&d, &bits, 8);
        return static_cast<int64_t>(d);
    }
    default: break;
    }
    throw ZoneError("cannot evaluate " + t.repr());
}

// ---------------------------------------------------------------------------
// Reader

namespace
{
struct Frame
{
    const Record *rec;
    Node *node;
    uint32_t base;
};

class Reader;

class Eval : public EvalContext
{
  public:
    Eval(const Platform &p, const std::vector<Frame> &stack, const std::string &context) : p(p), stack(stack), context(context)
    {
    }

    const Frame &frame_for(const std::string &rec_name) const
    {
        for (auto it = stack.rbegin(); it != stack.rend(); ++it)
            if (it->rec->name == rec_name)
                return *it;
        throw ZoneError("no " + rec_name + " instance on the load stack");
    }

    int64_t lookup(const std::vector<std::string> &path_in, const std::vector<int64_t> &indices) override
    {
        const Frame &ctx_frame = frame_for(context);
        const Frame *frame = &ctx_frame;
        size_t first = 0;
        if (path_in.size() > 1 && p.layout->find(path_in[0]) && ctx_frame.rec->field(path_in[0]) == nullptr)
        {
            frame = &frame_for(path_in[0]);
            first = 1;
        }
        const Record *rec = frame->rec;
        int64_t offset = frame->base;
        const TypeRef *t = nullptr;
        size_t next_index = 0;
        auto take_index = [&]() -> int64_t { return next_index < indices.size() ? indices[next_index++] : 0; };
        for (size_t i = first; i < path_in.size(); ++i)
        {
            const Field *f = rec->field(path_in[i]);
            if (!f)
                throw ZoneError(rec->name + " has no field " + path_in[i]);
            offset += f->offset;
            t = f->type;
            if (i + 1 < path_in.size())
            {
                while (t->kind == TypeKind::Array)
                {
                    offset += take_index() * t->elem->size;
                    t = t->elem;
                }
                rec = t->record;
                if (!rec)
                    throw ZoneError("no record " + t->name);
            }
        }
        while (t->kind == TypeKind::Array)
        {
            offset += take_index() * t->elem->size;
            t = t->elem;
        }
        if (offset < 0 || static_cast<size_t>(offset) + t->size > frame->node->data.size())
            throw ZoneError("expression reads past the data of " + frame->node->repr());
        return p.read_scalar(frame->node->data.data() + offset, *t);
    }

  private:
    const Platform &p;
    const std::vector<Frame> &stack;
    const std::string &context;
};

class Reader
{
  public:
    Reader(const Platform &p, const uint8_t *buf, size_t buf_size, Zone &zone) : p(p), buf(buf), buf_size(buf_size), zone(zone)
    {
    }

    void load();

  private:
    const Platform &p;
    const uint8_t *buf; // the zone's bytes, which the nodes view
    size_t buf_size;
    Zone &zone;
    size_t pos = 0;
    uint32_t offsets[BLOCK_COUNT] = {};
    std::vector<int> stack;
    std::vector<uint32_t> temp_saved;
    struct Index
    {
        std::vector<uint32_t> starts;
        std::vector<Node *> nodes;
    } block_index[BLOCK_COUNT];
    std::unordered_map<uint32_t, std::pair<Ptr *, uint32_t>> slots; // zone address of pointer slots -> (slot, insert?)
    std::vector<Frame> frames;
    std::vector<Node *> parents;
    std::vector<Node *> delayed;

    // -- raw stream
    const uint8_t *read(size_t size)
    {
        if (pos + size > buf_size)
            throw ZoneError("read past end of zone (" + hex(static_cast<uint32_t>(pos)) + " + " + hex(static_cast<uint32_t>(size)) + ")");
        const uint8_t *b = buf + pos;
        pos += size;
        return b;
    }
    uint32_t u32()
    {
        return p.u32(read(4));
    }

    // -- blocks
    int block() const
    {
        return stack.back();
    }
    void push(int b)
    {
        stack.push_back(b);
        if (b == BLOCK_TEMP)
            temp_saved.push_back(offsets[BLOCK_TEMP]);
    }
    void pop()
    {
        int b = stack.back();
        stack.pop_back();
        if (b == BLOCK_TEMP)
        {
            offsets[BLOCK_TEMP] = temp_saved.back();
            temp_saved.pop_back();
        }
    }
    void align(int b, uint32_t alignment)
    {
        if (alignment > 1)
            offsets[b] = (offsets[b] + alignment - 1) & ~(alignment - 1);
    }

    Node *new_node(const TypeRef *type, uint32_t count, uint32_t alignment)
    {
        int b = block();
        align(b, alignment);
        Node *node = zone.new_node();
        node->type = type;
        node->count = count;
        node->block = static_cast<int8_t>(b);
        node->offset = offsets[b];
        node->align = alignment;
        if (!parents.empty())
            parents.back()->children.push_back(node);
        if (indexed_block(b))
        {
            Index &ix = block_index[b];
            if (!ix.starts.empty() && ix.starts.back() > node->offset)
                throw ZoneError("block allocations out of order");
            ix.starts.push_back(node->offset);
            ix.nodes.push_back(node);
        }
        return node;
    }

    void load_into(Node *node, int64_t size, const TypeRef *seg_type = nullptr, int64_t seg_count = 1, bool partial = false)
    {
        int b = block();
        if (size < 0 || (p.streamed(b) && pos + static_cast<size_t>(size) > buf_size))
            throw ZoneError("invalid allocation size " + std::to_string(size) + " for " + (seg_type ? seg_type->repr() : "None"));
        if (!seg_type)
        {
            seg_type = p.uchar_type;
            seg_count = size;
        }
        node->segments.push_back({seg_type, static_cast<uint32_t>(seg_count), static_cast<uint32_t>(size), partial});
        if (offsets[b] != node->offset + node->data.size() + node->runtime_size || (!node->children.empty() && size))
            throw ZoneError("non contiguous load into " + node->repr());
        if (p.streamed(b))
        {
            // a node's bytes are one run of the stream (no other allocation can come between its
            // parts, as checked above): the node views them
            node->data.append_view(read(static_cast<size_t>(size)), static_cast<size_t>(size));
        }
        else
            node->runtime_size += static_cast<uint32_t>(size);
        offsets[b] += static_cast<uint32_t>(size);
    }

    uint32_t insert_slot()
    {
        align(BLOCK_VIRTUAL, 4);
        uint32_t addr = zone_addr(BLOCK_VIRTUAL, offsets[BLOCK_VIRTUAL]);
        offsets[BLOCK_VIRTUAL] += 4;
        return addr;
    }

    // -- pointers
    uint32_t ptr_value(const Node *node, uint32_t offset) const
    {
        if (offset + 4 > node->data.size())
            throw ZoneError("pointer past the data of " + node->repr());
        return p.u32(node->data.data() + offset);
    }

    void set_ptr(Node *node, uint32_t offset, Ptr *ptr)
    {
        ptr->owner = node;
        ptr->offset = offset;
        node->relocs.set(offset, ptr);
        if (indexed_block(node->block))
        {
            ptr->addr = zone_addr(node->block, node->offset + offset);
            slots[*ptr->addr] = {ptr, 0};
        }
    }

    Ptr *resolve_ref(uint32_t raw)
    {
        uint32_t addr = raw - 1;
        int b = static_cast<int>(addr >> BLOCK_SHIFT);
        uint32_t offset = addr & OFFSET_MASK;
        if (!indexed_block(b))
            throw ZoneError("reference into non referencable block " + std::to_string(b));
        const Index &ix = block_index[b];
        int64_t i = static_cast<int64_t>(std::upper_bound(ix.starts.begin(), ix.starts.end(), offset) - ix.starts.begin()) - 1;
        // several (empty) allocations can share a start offset: prefer the one holding the offset
        std::vector<Node *> candidates;
        for (int64_t j = i; j >= 0; --j)
        {
            Node *n = ix.nodes[static_cast<size_t>(j)];
            if (!(j == i || n->offset + n->data.size() >= offset))
                break;
            candidates.push_back(n);
        }
        for (int pass = 0; pass < 2; ++pass)
        {
            for (Node *n : candidates)
            {
                size_t end = n->offset + n->data.size();
                bool hit = pass == 0 ? (n->offset <= offset && offset < end) : offset == end;
                if (hit)
                {
                    uint32_t inner = offset - n->offset;
                    Ptr *ptr = zone.new_ptr(Ptr::Kind::Ref);
                    ptr->node = n;
                    ptr->index = inner / n->elem_size();
                    ptr->inner = inner % n->elem_size();
                    return ptr;
                }
            }
        }
        throw ZoneError("dangling reference " + hex(raw) + " (block " + std::to_string(b) + " offset " + hex(offset) + ")");
    }

    Ptr *resolve_alias(uint32_t raw)
    {
        uint32_t addr = raw - 1;
        auto it = slots.find(addr);
        if (it == slots.end())
            throw ZoneError("alias to unknown pointer slot " + hex(raw));
        Ptr *ptr = zone.new_ptr(Ptr::Kind::Alias);
        ptr->slot = it->second.first;
        ptr->index = it->second.second;
        return ptr;
    }

    Ptr *follow(Node *child, Ptr::Kind kind = Ptr::Kind::Follow)
    {
        Ptr *ptr = zone.new_ptr(kind);
        ptr->node = child;
        return ptr;
    }

    // -- strings
    void load_xstring(Node *node, uint32_t offset)
    {
        uint32_t raw = ptr_value(node, offset);
        if (raw == 0)
        {
            set_ptr(node, offset, zone.new_ptr(Ptr::Kind::Null));
            return;
        }
        if (raw == FOLLOW)
        {
            Node *child = new_node(p.char_type, 1, 1);
            child->string = true;
            const uint8_t *start = buf + pos;
            const uint8_t *nul = static_cast<const uint8_t *>(std::memchr(start, 0, buf_size - pos));
            if (!nul)
                throw ZoneError("unterminated string");
            int64_t length = (nul - start) + 1;
            load_into(child, length, p.char_type, length);
            child->count = static_cast<uint32_t>(child->data.size());
            set_ptr(node, offset, follow(child));
        }
        else
            set_ptr(node, offset, resolve_ref(raw));
    }

    // -- members
    const MemberInfo *pick_info(const Record &rec, const Field &f) const
    {
        const MemberInfos *infos = p.member_infos(rec, f);
        if (!infos)
            return nullptr;
        if (infos->size() == 1)
            return &infos->front().second;
        for (auto it = frames.rbegin(); it != frames.rend(); ++it)
            for (const auto &[ctx, info] : *infos)
                if (ctx == it->rec->name)
                    return &info;
        return &infos->front().second;
    }

    int64_t eval(const Expr *expr, const MemberInfo *info, const Record &rec)
    {
        Eval ctx(p, frames, info ? info->context : rec.name);
        return expr->eval(&ctx);
    }

    void load_asset_ptr(Node *node, uint32_t offset, const Record &rec);
    void load_struct(Node *node, const Record &rec, bool stream_start, uint32_t base = 0);
    void load_members(const Record &rec, Node *node, uint32_t base, bool after_partial);
    void load_member(const Record &rec, const Field &f, const MemberInfo *info, Node *node, uint32_t base, bool after_partial);
    void load_embedded_array(const Record &rec, const Field &f, const MemberInfo *info, Node *node, uint32_t offset, const TypeRef *t,
                             std::vector<int64_t> &index);
    void load_string_member(Node *node, uint32_t offset, const TypeRef *t, const MemberInfo *info, const Record &rec);
    void load_pointer(const Record &rec, const Field &f, const MemberInfo *info, Node *node, uint32_t offset, const TypeRef *pointee,
                      const std::vector<int64_t> &index);
    void load_pointer_array(Node *node, uint32_t offset, const TypeRef *pointee, int64_t count, bool reusable);
};

void Reader::load()
{
    uint32_t header[9];
    for (uint32_t &h : header)
        h = u32();
    zone.size = header[0];
    zone.external_size = header[1];
    zone.block_sizes.assign(header + 2, header + 9);

    Node *list_node = zone.new_node();
    list_node->type = p.uint_type;
    list_node->count = 4;
    list_node->block = -1;
    const uint8_t *head = read(16);
    list_node->data.append_view(head, 16);
    zone.root = list_node;
    uint32_t string_count = p.u32(head), strings_ptr = p.u32(head + 4), asset_count = p.u32(head + 8),
             assets_ptr = p.u32(head + 12);

    push(BLOCK_VIRTUAL);
    parents.push_back(list_node);

    if (strings_ptr)
    {
        if (strings_ptr != FOLLOW)
            throw ZoneError("script string list must follow");
        Node *script_node = new_node(p.char_ptr_type, string_count, 4);
        load_into(script_node, 4ll * string_count, script_node->type, string_count);
        parents.push_back(script_node);
        for (uint32_t i = 0; i < string_count; ++i)
        {
            load_xstring(script_node, 4 * i);
            Node *target = script_node->relocs.get(4 * i)->target();
            if (target)
                zone.script_strings.emplace_back(std::string(target->data.begin(), target->data.end() - 1));
            else
                zone.script_strings.emplace_back(std::nullopt);
        }
        parents.pop_back();
        zone.script_node = script_node;
    }

    if (assets_ptr)
    {
        if (assets_ptr != FOLLOW)
            throw ZoneError("asset list must follow");
        Node *assets_node = new_node(p.uint_type, 2 * asset_count, 4);
        load_into(assets_node, 8ll * asset_count, assets_node->type, 2ll * asset_count);
        parents.push_back(assets_node);
        for (uint32_t i = 0; i < asset_count; ++i)
        {
            uint32_t type_index = p.u32(assets_node->data.data() + 8 * i);
            if (type_index >= p.asset_types.size())
                throw ZoneError("asset " + std::to_string(i) + ": invalid type " + std::to_string(type_index));
            const std::string &type_name = p.asset_types[type_index];
            const char *rec_name = asset_record_name(type_name);
            if (!rec_name)
                throw ZoneError("asset " + std::to_string(i) + ": unsupported type " + type_name);
            try
            {
                load_asset_ptr(assets_node, 8 * i + 4, p.record(rec_name));
            }
            catch (const ZoneError &e)
            {
                throw ZoneError("asset " + std::to_string(i) + " (" + type_name + "): " + e.what());
            }
            Ptr *ptr = assets_node->relocs.get(8 * i + 4);
            ZoneAsset asset;
            asset.type = type_name;
            asset.ptr = ptr;
            if (Node *node = ptr->target())
            {
                node->asset = type_name.c_str();
                asset.name = asset_name(p, *node);
            }
            zone.assets.push_back(std::move(asset));
        }
        parents.pop_back();
        zone.assets_node = assets_node;
    }

    parents.pop_back();
    pop();

    for (Node *node : delayed)
    {
        push(node->block);
        align(node->block, node->align.value_or(1));
        node->offset = offsets[node->block];
        size_t size = static_cast<size_t>(node->count) * node->type->size;
        node->data.append_view(read(size), size);
        node->segments.push_back({node->type, node->count, static_cast<uint32_t>(node->data.size()), false});
        offsets[node->block] += static_cast<uint32_t>(node->data.size());
        pop();
    }

    zone.platform = p.name;
    if (pos != buf_size)
        throw ZoneError(std::to_string(buf_size - pos) + " unread bytes at end of zone");
}

void Reader::load_asset_ptr(Node *node, uint32_t offset, const Record &rec)
{
    uint32_t raw = ptr_value(node, offset);
    if (raw == 0)
    {
        set_ptr(node, offset, zone.new_ptr(Ptr::Kind::Null));
        return;
    }
    std::optional<int> tb = p.type_block(rec);
    bool in_temp = tb && *tb == BLOCK_TEMP;
    if (in_temp)
        push(BLOCK_TEMP);

    if (raw == FOLLOW || (in_temp && raw == INSERT))
    {
        Node *child = new_node(rec.self, 1, p.type_alloc_align(rec.name, rec.align));
        child->origin = Origin::Asset;
        child->origin_record = &rec.name;
        if (in_temp)
            child->push_before = BLOCK_TEMP;
        Ptr *ptr = follow(child, raw == FOLLOW ? Ptr::Kind::Follow : Ptr::Kind::Insert);
        child->ptr = ptr;
        if (raw == INSERT)
        {
            child->insert = true;
            ptr->insert_addr = insert_slot();
            slots[*ptr->insert_addr] = {ptr, 1};
        }
        load_struct(child, rec, true);
        set_ptr(node, offset, ptr);
    }
    else if (in_temp)
        set_ptr(node, offset, resolve_alias(raw));
    else
        set_ptr(node, offset, resolve_ref(raw));

    if (in_temp)
        pop();
}

void Reader::load_struct(Node *node, const Record &rec, bool stream_start, uint32_t base)
{
    if (stream_start)
    {
        const Field *dyn = p.dynamic_member(rec);
        uint32_t size = dyn ? dyn->offset : rec.size;
        load_into(node, size, rec.self, 1, dyn != nullptr);
    }

    std::optional<int> pushed;
    if (p.is_asset(rec))
        pushed = BLOCK_VIRTUAL;
    else
        pushed = p.type_block(rec);
    if (pushed)
    {
        push(*pushed);
        if (node->push_after < 0 && base == 0)
            node->push_after = static_cast<int8_t>(*pushed);
    }

    parents.push_back(node);
    frames.push_back({&rec, node, base});
    load_members(rec, node, base, stream_start);
    frames.pop_back();
    parents.pop_back();

    if (pushed)
        pop();
}

void Reader::load_members(const Record &rec, Node *node, uint32_t base, bool after_partial)
{
    const Field *dyn = after_partial ? p.dynamic_member(rec) : nullptr;

    if (rec.is_union)
    {
        std::vector<std::pair<const Field *, const MemberInfo *>> used;
        for (const Field *f : p.ordered_members(rec))
        {
            const MemberInfo *info = pick_info(rec, *f);
            if (info && info->condition && info->condition->is_never())
                continue;
            if (!dyn && p.member_is_leaf(rec, *f))
                continue;
            used.emplace_back(f, info);
        }
        for (auto [f, info] : used)
        {
            if (info && info->condition && !eval(info->condition, info, rec))
                continue;
            load_member(rec, *f, info, node, base, dyn != nullptr);
            break;
        }
        return;
    }

    for (const Field *f : p.ordered_members(rec))
    {
        const MemberInfo *info = pick_info(rec, *f);
        if (info && info->condition && info->condition->is_never())
            continue;
        bool is_after_partial = dyn && f->offset >= dyn->offset;
        if (!is_after_partial && p.member_is_leaf(rec, *f))
            continue;
        if (info && info->condition && !eval(info->condition, info, rec))
            continue;
        load_member(rec, *f, info, node, base, is_after_partial);
    }
}

void Reader::load_member(const Record &rec, const Field &f, const MemberInfo *info, Node *node, uint32_t base, bool after_partial)
{
    const TypeRef *t = f.type;
    uint32_t offset = base + f.offset;

    if (info && info->arraysize)
    {
        int64_t count = eval(info->arraysize, info, rec);
        const TypeRef *elem = t->kind == TypeKind::Array ? t->elem : t;
        if (node->data.size() != offset)
            throw ZoneError("dynamic member " + rec.name + "::" + f.name + " not at end of data");
        load_into(node, count * elem->size, elem, count);
        if (elem->kind == TypeKind::Record && !p.record_is_leaf(rec_of(*elem)))
        {
            for (int64_t i = 0; i < count; ++i)
                load_struct(node, rec_of(*elem), false, offset + static_cast<uint32_t>(i) * elem->size);
        }
        return;
    }

    if (info && info->string)
    {
        load_string_member(node, offset, t, info, rec);
        return;
    }

    if (t->kind == TypeKind::Record)
    {
        const Record &sub = rec_of(*t);
        if (after_partial)
        {
            // the embedded member is (or holds) the dynamic member: stream it now
            const Field *dyn = p.dynamic_member(sub);
            uint32_t size = dyn ? dyn->offset : sub.size;
            if (node->data.size() != offset)
                throw ZoneError("partial member " + rec.name + "::" + f.name + " not at end of data");
            load_into(node, size, t, 1, dyn != nullptr);
        }
        frames.push_back({&sub, node, offset});
        load_members(sub, node, offset, after_partial);
        frames.pop_back();
        return;
    }

    if (t->kind == TypeKind::Array)
    {
        if (after_partial)
            load_into(node, t->size, t->elem, t->count);
        std::vector<int64_t> index;
        load_embedded_array(rec, f, info, node, offset, t, index);
        return;
    }

    if (t->kind == TypeKind::Pointer)
    {
        if (after_partial)
            load_into(node, 4, t, 1);
        load_pointer(rec, f, info, node, offset, t->to, {});
        return;
    }

    if (after_partial)
        load_into(node, t->size, t, 1);
}

void Reader::load_embedded_array(const Record &rec, const Field &f, const MemberInfo *info, Node *node, uint32_t offset, const TypeRef *t,
                                 std::vector<int64_t> &index)
{
    const TypeRef *elem = t->elem;
    for (uint32_t i = 0; i < t->count; ++i)
    {
        uint32_t eoff = offset + i * elem->size;
        if (elem->kind == TypeKind::Array)
        {
            index.push_back(i);
            load_embedded_array(rec, f, info, node, eoff, elem, index);
            index.pop_back();
        }
        else if (elem->kind == TypeKind::Pointer)
        {
            index.push_back(i);
            load_pointer(rec, f, info, node, eoff, elem->to, index);
            index.pop_back();
        }
        else if (elem->kind == TypeKind::Record && !p.record_is_leaf(rec_of(*elem)))
        {
            const Record &sub = rec_of(*elem);
            frames.push_back({&sub, node, eoff});
            load_members(sub, node, eoff, false);
            frames.pop_back();
        }
    }
}

void Reader::load_string_member(Node *node, uint32_t offset, const TypeRef *t, const MemberInfo *info, const Record &rec)
{
    if (t->kind == TypeKind::Array)
    {
        for (uint32_t i = 0; i < t->count; ++i)
            load_xstring(node, offset + 4 * i);
        return;
    }
    if (t->kind != TypeKind::Pointer)
        throw ZoneError("string member is not a pointer");
    if (t->to->kind == TypeKind::Pointer)
    {
        // const char ** with a count: a pointer array of strings
        uint32_t raw = ptr_value(node, offset);
        if (raw == 0)
        {
            set_ptr(node, offset, zone.new_ptr(Ptr::Kind::Null));
            return;
        }
        if (raw != FOLLOW)
        {
            set_ptr(node, offset, resolve_ref(raw));
            return;
        }
        int64_t count = info->count ? eval(info->count, info, rec) : 1;
        Node *child = new_node(t->to, static_cast<uint32_t>(count), 4);
        load_into(child, 4 * count, t->to, count);
        set_ptr(node, offset, follow(child));
        parents.push_back(child);
        for (int64_t i = 0; i < count; ++i)
            load_xstring(child, static_cast<uint32_t>(4 * i));
        parents.pop_back();
        return;
    }
    load_xstring(node, offset);
}

void Reader::load_pointer(const Record &rec, const Field &f, const MemberInfo *info, Node *node, uint32_t offset, const TypeRef *pointee,
                          const std::vector<int64_t> &index)
{
    uint32_t raw = ptr_value(node, offset);
    if (raw == 0)
    {
        set_ptr(node, offset, zone.new_ptr(Ptr::Kind::Null));
        return;
    }

    if (pointee->kind == TypeKind::Record && pointee->record && p.is_asset(*pointee->record))
    {
        load_asset_ptr(node, offset, rec_of(*pointee));
        return;
    }

    const Expr *count_expr = nullptr;
    if (info)
    {
        if (!index.empty())
        {
            auto it = info->index_counts.find(index);
            if (it != info->index_counts.end())
                count_expr = it->second;
        }
        if (!count_expr)
            count_expr = info->count;
    }
    int64_t count = count_expr ? eval(count_expr, info, rec) : 1;

    if (info && info->delayed && (!info->delayed->condition || eval(info->delayed->condition, info, rec)))
    {
        // streamed after all assets (console image pixels): a placeholder now
        Node *child = zone.new_node();
        child->type = pointee;
        child->count = static_cast<uint32_t>(count);
        child->block = static_cast<int8_t>(block_by_name(info->delayed->block));
        child->align = static_cast<uint32_t>(info->delayed->alignment);
        child->delayed = true;
        child->origin = Origin::Member;
        child->origin_record = &rec.name;
        child->origin_field = &f.name;
        if (!parents.empty())
            parents.back()->children.push_back(child);
        delayed.push_back(child);
        set_ptr(node, offset, follow(child));
        return;
    }

    std::optional<int> member_block;
    if (info && info->block && !info->block->empty())
        member_block = block_by_name(*info->block);
    // OAT pushes every block but the default normal one; console zones also need an explicit VIRTUAL
    // push from inside the temp block (360 texture headers), which OAT cannot express
    bool pushed = member_block && (*member_block != BLOCK_VIRTUAL || block() != BLOCK_VIRTUAL);
    if (pushed)
        push(*member_block);

    struct PopGuard
    {
        Reader *r;
        bool active;
        ~PopGuard()
        {
            if (active)
                r->pop();
        }
    } guard{this, pushed};

    bool reusable = info && info->reusable;
    bool in_temp = member_block && *member_block == BLOCK_TEMP;
    if (reusable && in_temp && raw != FOLLOW && raw != INSERT)
    {
        set_ptr(node, offset, resolve_alias(raw));
        return;
    }
    if (reusable && !in_temp && raw != FOLLOW)
    {
        set_ptr(node, offset, resolve_ref(raw));
        return;
    }

    if (pointee->kind == TypeKind::Pointer)
    {
        load_pointer_array(node, offset, pointee, count, reusable);
        Node *child = node->relocs.get(offset)->node;
        child->origin = Origin::PtrArray;
        child->origin_record = &rec.name;
        child->origin_field = &f.name;
        return;
    }

    uint32_t alignment = pointee->align;
    if (info && info->allocalign)
        alignment = static_cast<uint32_t>(info->allocalign->eval(nullptr));
    else if (pointee->kind == TypeKind::Record)
        alignment = p.type_alloc_align(pointee->name, pointee->align);

    Node *child = new_node(pointee, static_cast<uint32_t>(count), alignment);
    child->origin = Origin::Member;
    child->origin_record = &rec.name;
    child->origin_field = &f.name;
    if (pushed)
        child->push_before = static_cast<int8_t>(*member_block);
    Ptr *ptr = follow(child, (in_temp && raw == INSERT) ? Ptr::Kind::Insert : Ptr::Kind::Follow);
    child->ptr = ptr;
    if (ptr->kind == Ptr::Kind::Insert)
    {
        child->insert = true;
        ptr->insert_addr = insert_slot();
        slots[*ptr->insert_addr] = {ptr, 1};
    }
    set_ptr(node, offset, ptr);

    bool runtime = !p.streamed(block());
    if (pointee->kind == TypeKind::Record && !p.record_is_leaf(rec_of(*pointee)) && !runtime)
    {
        const Record &prec = rec_of(*pointee);
        if (count == 1 && p.dynamic_member(prec) != nullptr)
            load_struct(child, prec, true);
        else
        {
            load_into(child, count * pointee->size, pointee, count);
            for (int64_t i = 0; i < count; ++i)
                load_struct(child, prec, false, static_cast<uint32_t>(i) * pointee->size);
        }
    }
    else
        load_into(child, count * pointee->size, pointee, count);
}

void Reader::load_pointer_array(Node *node, uint32_t offset, const TypeRef *pointee, int64_t count, bool reusable)
{
    Node *child = new_node(pointee, static_cast<uint32_t>(count), 4);
    set_ptr(node, offset, follow(child));
    load_into(child, 4 * count, pointee, count);
    const TypeRef *target = pointee->to;
    parents.push_back(child);
    for (int64_t i = 0; i < count; ++i)
    {
        uint32_t at = static_cast<uint32_t>(4 * i);
        uint32_t eraw = ptr_value(child, at);
        if (eraw == 0)
        {
            set_ptr(child, at, zone.new_ptr(Ptr::Kind::Null));
            continue;
        }
        if (target->kind == TypeKind::Record && target->record && p.is_asset(*target->record))
        {
            load_asset_ptr(child, at, rec_of(*target));
            continue;
        }
        if (reusable && eraw != FOLLOW)
        {
            set_ptr(child, at, resolve_ref(eraw));
            continue;
        }
        uint32_t alignment = target->kind == TypeKind::Record ? p.type_alloc_align(target->name, target->align) : target->align;
        Node *elem = new_node(target, 1, alignment);
        elem->origin = Origin::PtrElem;
        static const std::string EMPTY;
        elem->origin_record = target->kind == TypeKind::Record ? &target->name : &EMPTY;
        set_ptr(child, at, follow(elem));
        if (target->kind == TypeKind::Record && !p.record_is_leaf(rec_of(*target)))
            load_struct(elem, rec_of(*target), true);
        else
            load_into(elem, target->size, target, 1);
    }
    parents.pop_back();
}

// ---------------------------------------------------------------------------
// Writer

class Writer
{
  public:
    explicit Writer(const Platform &p) : p(p)
    {
    }

    std::vector<uint8_t> write(Zone &zone);

  private:
    const Platform &p;
    std::vector<uint8_t> out;
    uint32_t offsets[BLOCK_COUNT] = {};
    uint32_t temp_max = 0;
    std::vector<Node *> delayed;

    void align(int b, uint32_t alignment)
    {
        if (alignment > 1)
            offsets[b] = (offsets[b] + alignment - 1) & ~(alignment - 1);
    }

    uint32_t pointer_value(const Ptr *ptr) const
    {
        switch (ptr->kind)
        {
        case Ptr::Kind::Null: return 0;
        case Ptr::Kind::Follow: return FOLLOW;
        case Ptr::Kind::Insert: return INSERT;
        case Ptr::Kind::Ref: {
            const Node *n = ptr->node;
            if (!n->new_offset)
                throw ZoneError("reference to unwritten node " + n->repr());
            return zone_addr(n->block, *n->new_offset + ptr->index * n->elem_size() + ptr->inner) + 1;
        }
        case Ptr::Kind::Alias: {
            const Ptr *slot = ptr->slot;
            const std::optional<uint32_t> &addr = ptr->index == 1 ? slot->insert_addr : slot->addr;
            if (!addr)
                throw ZoneError("alias to an unwritten slot");
            return *addr + 1;
        }
        }
        return 0;
    }

    void emit(Node *node)
    {
        if (node->delayed)
        {
            delayed.push_back(node);
            return;
        }
        int b = node->block;
        std::optional<uint32_t> saved_temp;
        if (node->push_before == BLOCK_TEMP)
            saved_temp = offsets[BLOCK_TEMP];

        align(b, node->align.value_or(1));
        node->new_offset = offsets[b];

        if (node->insert)
        {
            align(BLOCK_VIRTUAL, 4);
            node->ptr->insert_addr = zone_addr(BLOCK_VIRTUAL, offsets[BLOCK_VIRTUAL]);
            offsets[BLOCK_VIRTUAL] += 4;
        }

        if (!node->relocs.empty())
        {
            bool normal = indexed_block(b);
            for (const auto &[off, ptr] : node->relocs.list())
            {
                if (normal)
                    ptr->addr = zone_addr(b, *node->new_offset + off);
                else
                    ptr->addr.reset();
            }
        }

        size_t start = out.size();
        bool streamed = p.streamed(b);
        if (streamed)
            out.insert(out.end(), node->data.begin(), node->data.end());
        offsets[b] += static_cast<uint32_t>(node->data.size()) + node->runtime_size;
        if (b == BLOCK_TEMP)
            temp_max = std::max(temp_max, offsets[BLOCK_TEMP]);

        for (Node *child : node->children)
            emit(child);

        // pointers are encoded once the children are written: a pointer can refer to data loaded while
        // this node's members were (techniques sharing technique 0)
        if (streamed && !node->relocs.empty())
            for (const auto &[off, ptr] : node->relocs.list())
                p.put_u32(out.data() + start + off, pointer_value(ptr));

        if (saved_temp)
            offsets[BLOCK_TEMP] = *saved_temp;
    }
};

std::vector<uint8_t> Writer::write(Zone &zone)
{
    uint32_t string_count = zone.script_node ? zone.script_node->count : 0;
    uint32_t asset_count = zone.assets_node ? zone.assets_node->count / 2 : 0;
    out.resize(16);
    p.put_u32(out.data(), string_count);
    p.put_u32(out.data() + 4, zone.script_node ? FOLLOW : 0);
    p.put_u32(out.data() + 8, asset_count);
    p.put_u32(out.data() + 12, zone.assets_node ? FOLLOW : 0);
    for (Node *child : zone.root->children)
        emit(child);

    // delayed data (360 image pixels) follows the assets and is not part of the zone size
    uint32_t size = static_cast<uint32_t>(out.size());
    for (Node *node : delayed)
    {
        align(node->block, node->align.value_or(1));
        node->new_offset = offsets[node->block];
        out.insert(out.end(), node->data.begin(), node->data.end());
        offsets[node->block] += static_cast<uint32_t>(node->data.size());
    }

    uint32_t block_sizes[BLOCK_COUNT];
    std::copy(std::begin(offsets), std::end(offsets), block_sizes);
    // the PC linker reserves room for the 16 byte asset list header in the temp block
    block_sizes[BLOCK_TEMP] = temp_max + p.temp_padding;
    std::vector<uint8_t> result(36);
    p.put_u32(result.data(), size);
    p.put_u32(result.data() + 4, zone.external_size);
    for (int i = 0; i < BLOCK_COUNT; ++i)
        p.put_u32(result.data() + 8 + 4 * i, block_sizes[i]);
    result.insert(result.end(), out.begin(), out.end());
    return result;
}
} // namespace

namespace
{
class HeapZoneBytes : public ZoneBytes
{
  public:
    explicit HeapZoneBytes(std::vector<uint8_t> bytes) : bytes_(std::move(bytes))
    {
        data_ = bytes_.data();
        size_ = bytes_.size();
    }

  private:
    std::vector<uint8_t> bytes_;
};
} // namespace

std::shared_ptr<const ZoneBytes> zone_bytes(std::vector<uint8_t> bytes)
{
    return std::make_shared<HeapZoneBytes>(std::move(bytes));
}

std::unique_ptr<Zone> read_zone(const Platform &p, std::shared_ptr<const ZoneBytes> bytes)
{
    auto zone = std::make_unique<Zone>();
    zone->source = std::move(bytes);
    Reader(p, zone->source->data(), zone->source->size(), *zone).load();
    return zone;
}

std::vector<uint8_t> write_zone(const Platform &p, Zone &zone)
{
    return Writer(p).write(zone);
}

std::string asset_name(const Platform &, const Node &node)
{
    const Record *rec = node.type->record;
    if (!rec)
        return "";
    auto string_at = [&](uint32_t offset) -> std::optional<std::string> {
        Ptr *ptr = node.relocs.get(offset);
        Node *target = ptr ? ptr->target() : nullptr;
        if (target && target->string)
            return std::string(target->data.begin(), target->data.end() - 1);
        return std::nullopt;
    };
    for (const char *candidate : {"name", "szInternalName", "aliasName", "fontName"})
    {
        const Field *f = rec->field(candidate);
        if (f && f->type->kind == TypeKind::Pointer)
            if (auto s = string_at(f->offset))
                return *s;
    }
    for (const Field &f : rec->fields)
    {
        if (f.type->kind != TypeKind::Record)
            continue;
        const Record *sub = f.type->record;
        const Field *nf = sub ? sub->field("name") : nullptr;
        if (nf && nf->type->kind == TypeKind::Pointer)
            if (auto s = string_at(f.offset + nf->offset))
                return *s;
    }
    return "";
}
} // namespace t4ff
