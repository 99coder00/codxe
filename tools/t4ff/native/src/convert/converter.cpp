#include "convert/converter.h"

#include <atomic>
#include <algorithm>
#include <cstring>
#include <sstream>

#include "convert/assets.h"
#include "convert/library.h"
#include "core/progress.h"
#include "core/resources.h"

namespace t4ff
{
namespace
{
// asset types a map uses by name reference when the game's own zones have them
bool game_reference_type(const std::string &t)
{
    return t == "lightdef";
}

// PC asset types stored under another type on console (single player maps: the PVS clip map type)
std::string x360_asset_type(const std::string &t)
{
    return t == "clipmap" ? "clipmap_pvs" : t;
}

std::set<std::string> load_verified_records()
{
    std::set<std::string> out;
    std::istringstream in{std::string(resource(Resource::VerifiedX360))};
    for (std::string line; std::getline(in, line);)
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[0] == '#')
            continue;
        out.insert(line.substr(start));
    }
    return out;
}

uint32_t identity(uint32_t off)
{
    return off;
}
} // namespace

std::string asset_display_name(const Platform &p, const Node &node)
{
    try
    {
        return asset_name(p, node);
    }
    catch (const std::exception &)
    {
        return "";
    }
}

std::string asset_type_of(const std::string &rec_name)
{
    if (const char *t = asset_type_of_record(rec_name))
        return t;
    throw ConvertError(rec_name + " is not an asset");
}

// -- ZoneConverter

ZoneConverter::ZoneConverter(std::unique_ptr<Zone> zone_, const Platform &src_, const Platform &dst_, ConvertOptions options_)
    : zone(*zone_), src(src_), dst(dst_), options(std::move(options_)), out_(std::make_unique<Zone>()), source_(std::move(zone_))
{
    verified = load_verified_records();
    out_->keep_alive.push_back(source_);
    if (!options.iwd_paths.empty())
        library = std::make_unique<IwdLibrary>(options.iwd_paths);
    if (!options.stock_paths.empty())
        stock_library = std::make_unique<IwdLibrary>(options.stock_paths);
    script_strings = zone.script_strings;
    if (!options.console_zones.empty())
    {
        // not the fastfiles of the folder this conversion writes to (sounds_dir: the map's output folder)
        std::vector<std::filesystem::path> exclude;
        if (!options.sounds_dir.empty())
            exclude.push_back(options.sounds_dir);
        console_library = &shared_library(dst, options.console_zones, options.log, options.map_name, exclude);
        cloner = std::make_unique<Cloner>(dst, *out_, script_strings,
                                          [this](const std::string &rec, const std::string &name, Node *library_node) -> Node * {
                                              // nested assets of library copies: textures the budget reduces are rebuilt
                                              if (rec == "GfxImage" && image_drop_levels.count(name) && image_drop_levels[name])
                                                  return rebuild_console_image(*this, name, library_node);
                                              return nullptr;
                                          });
    }
    register_hooks(*this);
}

ZoneConverter::~ZoneConverter() = default;

void ZoneConverter::log(const std::string &msg) const
{
    if (options.log)
        options.log(msg);
}

void ZoneConverter::warn(const std::string &msg)
{
    stats.warnings.push_back(msg);
    log("warning: " + msg);
}

Node *ZoneConverter::new_node(const TypeRef *type, uint32_t count, int block)
{
    Node *n = out_->new_node();
    n->type = type;
    n->count = count;
    n->block = static_cast<int8_t>(block);
    return n;
}

Ptr *ZoneConverter::new_ptr(Ptr::Kind kind)
{
    return out_->new_ptr(kind);
}

const TypeRef *ZoneConverter::dst_record_type(const std::string &name)
{
    const Record *rec = dst.layout->find(name);
    if (!rec)
        throw ConvertError("the console has no record " + name);
    return rec->self;
}

const TypeRef *ZoneConverter::dst_type(const TypeRef *t)
{
    if (t->kind == TypeKind::Record)
        return dst_record_type(t->name);
    if (t->kind != TypeKind::Array && t->kind != TypeKind::Pointer)
        return t;
    auto it = dst_types.find(t);
    if (it != dst_types.end())
        return it->second;
    TypeRef &out = type_store.emplace_back();
    if (t->kind == TypeKind::Array)
    {
        const TypeRef *elem = dst_type(t->elem);
        // keep typedef alignment (UShortVec: unsigned short[3] aligned to 4)
        out.kind = TypeKind::Array;
        out.name = t->name;
        out.size = elem->size * t->count;
        out.align = std::max(elem->align, t->align);
        out.elem = elem;
        out.count = t->count;
    }
    else
    {
        out.kind = TypeKind::Pointer;
        out.size = 4;
        out.align = 4;
        out.to = t->to && t->to->kind == TypeKind::Record ? dst_type(t->to) : t->to;
    }
    dst_types[t] = &out;
    return &out;
}

const RecordMap &ZoneConverter::record_map(const std::string &name, bool partial)
{
    auto key = std::make_pair(name, partial);
    auto it = record_maps.find(key);
    if (it == record_maps.end())
        it = record_maps.emplace(key, std::make_unique<RecordMap>(src, dst, name, partial)).first;
    return *it->second;
}

const TypeMap &ZoneConverter::type_map(const TypeRef *t, bool partial)
{
    if (t->kind == TypeKind::Record)
        return record_map(t->name, partial);
    if (t->kind == TypeKind::Pointer)
    {
        if (!pointer_map)
            pointer_map = std::make_unique<ScalarMap>(*src.uint_type);
        return *pointer_map;
    }
    auto it = other_maps.find(t);
    if (it != other_maps.end())
        return *it->second;
    std::unique_ptr<TypeMap> m;
    if (t->kind == TypeKind::Array)
        m = std::make_unique<ArrayMap>(type_map(t->elem), t->count);
    else
        m = std::make_unique<ScalarMap>(*t);
    return *other_maps.emplace(t, std::move(m)).first->second;
}

uint32_t ZoneConverter::dst_alignment(const Node &node, const TypeRef &dt)
{
    if (node.string)
        return 1;
    switch (node.origin)
    {
    case Origin::None: return node.align.value_or(1);
    case Origin::Asset: {
        const Record &rec = dst.record(*node.origin_record);
        return dst.type_alloc_align(rec.name, rec.align);
    }
    case Origin::PtrArray: return 4;
    case Origin::PtrElem:
        if (node.origin_record && !node.origin_record->empty())
        {
            const Record &rec = dst.record(*node.origin_record);
            return dst.type_alloc_align(rec.name, rec.align);
        }
        return std::max(dt.align, 1u);
    case Origin::Member: {
        if (const MemberInfos *infos = dst.cmds->member_infos(*node.origin_record, *node.origin_field))
        {
            for (const auto &[ctx, info] : *infos)
            {
                if (info.delayed && node.delayed)
                    return static_cast<uint32_t>(info.delayed->alignment);
                if (info.allocalign)
                    return static_cast<uint32_t>(info.allocalign->eval(nullptr));
            }
        }
        if (dt.kind == TypeKind::Record)
            return dst.type_alloc_align(dt.name, dt.align);
        return std::max(dt.align, 1u);
    }
    }
    return node.align.value_or(1);
}

bool ZoneConverter::dst_has_member(const std::string &rec, const std::string &member) const
{
    const Record *r = dst.layout->find(rec);
    return !r || r->field(member) != nullptr;
}

std::set<std::string> ZoneConverter::unverified_records(Node *node) const
{
    std::set<std::string> missing;
    std::vector<Node *> stack{node};
    while (!stack.empty())
    {
        Node *n = stack.back();
        stack.pop_back();
        // nested assets are converted (or referenced) on their own; members the console structure
        // does not have are dropped
        for (Node *child : n->children)
        {
            if (child->origin == Origin::Asset)
                continue;
            if (child->origin == Origin::Member && !dst_has_member(*child->origin_record, *child->origin_field))
                continue;
            stack.push_back(child);
        }
        for (const Segment &s : n->segments)
        {
            const TypeRef *t = s.type;
            while (t->kind == TypeKind::Array)
                t = t->elem;
            if (t->kind == TypeKind::Record && !verified.count(t->name))
                missing.insert(t->name);
        }
    }
    return missing;
}

uint32_t ZoneConverter::runtime_size(const Node &node)
{
    // runtime (non streamed) allocations: scaled by the element size of the console type
    const TypeRef *t = node.type, *d = dst_type(t);
    if (t->size && d->size)
        return node.runtime_size / t->size * d->size;
    return node.runtime_size;
}

Node *ZoneConverter::convert_node(Node *node)
{
    const TypeRef *dt = dst_type(node->type);
    Node *nw = new_node(dt, node->count, node->block);
    nw->push_before = node->push_before;
    nw->push_after = node->push_after;
    nw->insert = node->insert;
    nw->string = node->string;
    nw->asset = node->asset;
    nw->runtime_size = node->runtime_size;
    nw->origin = node->origin;
    nw->origin_record = node->origin_record;
    nw->origin_field = node->origin_field;
    nw->delayed = node->delayed;
    nw->align = dst_alignment(*node, *dt);

    // the data, segment by segment
    struct Seg
    {
        uint32_t s0, s1, d0;
        const TypeMap *map;
        uint32_t src_stride, dst_stride;
    };
    std::vector<Seg> segments;
    uint32_t src_pos = 0;
    std::vector<uint8_t> out;
    for (const Segment &s : node->segments)
    {
        if (node->runtime_size && node->data.empty())
            break;
        const TypeMap &m = type_map(s.type, s.partial);
        uint32_t src_stride = s.count ? m.src_size : 0;
        if (s.count && src_stride * s.count != s.size)
            src_stride = s.size / s.count;
        size_t available = src_pos <= node->data.size() ? std::min<size_t>(s.size, node->data.size() - src_pos) : 0;
        std::vector<uint8_t> chunk = m.convert(node->data.data() + src_pos, available, s.count, src_stride);
        uint32_t dst_stride = s.count ? static_cast<uint32_t>(chunk.size() / s.count) : 0;
        segments.push_back({src_pos, src_pos + s.size, static_cast<uint32_t>(out.size()), &m, src_stride, dst_stride});
        out.insert(out.end(), chunk.begin(), chunk.end());
        nw->segments.push_back({dst_type(s.type), s.count, static_cast<uint32_t>(chunk.size()), s.partial});
        src_pos += s.size;
    }
    const uint32_t out_size = static_cast<uint32_t>(out.size());
    nw->data.assign(std::move(out));
    if (node->runtime_size)
        nw->runtime_size = runtime_size(*node);

    auto translate = [segments, out_size, node](uint32_t off) -> uint32_t {
        for (const Seg &s : segments)
        {
            if ((s.s0 <= off && off < s.s1) || (off == s.s1 && s.s1 == s.s0))
            {
                uint32_t rel = off - s.s0;
                if (s.src_stride == 0)
                    return s.d0;
                return s.d0 + rel / s.src_stride * s.dst_stride + s.map->map_offset(rel % s.src_stride);
            }
        }
        if (!segments.empty() && off == segments.back().s1)
            return out_size;
        if (segments.empty())
            return off;
        throw ConvertError("offset " + std::to_string(off) + " outside of node " + node->repr());
    };
    auto keeps_pointer = [&segments](uint32_t off) {
        for (const Seg &s : segments)
            if (s.s0 <= off && off < s.s1)
                return s.map->keeps_pointer(s.src_stride ? (off - s.s0) % s.src_stride : 0);
        return true;
    };

    node_map[node] = nw;
    offset_maps[node] = translate;

    std::unordered_set<const Node *> dropped;
    for (const auto &[off, ptr] : node->relocs.list())
    {
        if (!keeps_pointer(off))
        {
            // the console structure has no such pointer (model collision triangles)
            if ((ptr->kind == Ptr::Kind::Follow || ptr->kind == Ptr::Kind::Insert) && ptr->node)
                dropped.insert(ptr->node);
            continue;
        }
        uint32_t new_off = translate(off);
        nw->relocs.set(new_off, ptr);
        ptrs.emplace_back(ptr, nw);
        ptr->offset = new_off;
    }
    for (Node *child : node->children)
        if (!dropped.count(child))
            nw->children.push_back(convert_child(child));
    return nw;
}

Node *ZoneConverter::convert_child(Node *child)
{
    if (child->asset || (child->type->kind == TypeKind::Record && child->origin == Origin::Asset))
    {
        std::string asset_type = child->asset ? std::string(child->asset) : asset_type_of(child->type->name);
        return convert_asset_node(asset_type, child);
    }
    return convert_node(child);
}

Node *ZoneConverter::convert_asset_node(const std::string &asset_type, Node *node)
{
    std::string name = asset_display_name(src, *node);
    const bool ref = !name.empty() && name[0] == ',';
    bool reduced = false;
    if (asset_type == "image")
    {
        auto it = image_drop_levels.find(ref ? name.substr(1) : name);
        reduced = it != image_drop_levels.end() && it->second;
    }
    bool stock_techset = asset_type == "techset" && options.reference_techsets;
    if (ref && !reduced && !stock_techset)
    {
        // the PC zone expects this asset from another zone: the console library may have it
        if (Node *copy = from_library(asset_type, name, node))
        {
            node_map[node] = copy;
            offset_maps[node] = identity;
            return copy;
        }
    }
    if (game_reference_type(asset_type) && !ref && console_library)
    {
        // the game's own version: the PC map's copy of a stock light definition has the PC
        // linker's lookup index, not the console's
        if (console_library->in_game_zones(asset_record_name(asset_type), name))
            return reference_asset(asset_type, node, name);
    }
    auto hook = hooks.find(asset_type);
    if (hook != hooks.end())
    {
        if (Node *result = hook->second(*this, asset_type, node, name))
        {
            node_map[node] = result;
            offset_maps.emplace(node, identity);
            bool name_ref = false;
            for (const auto &[off, p] : result->relocs.list())
                if (p->kind == Ptr::Kind::Follow && p->node->string && !p->node->data.empty() && p->node->data[0] == ',')
                    name_ref = true;
            if (!result->library && !name_ref)
            {
                ++stats.converted[asset_type];
                register_converted(asset_type, name, node);
            }
            return result;
        }
    }

    std::set<std::string> missing = unverified_records(node);
    if (!missing.empty() && !options.allow_unverified && !ref)
    {
        std::string list;
        for (const std::string &m : missing)
            list += (list.empty() ? "" : ", ") + m;
        warn(asset_type + " '" + name + "': console layout of " + list + " not verified, emitting a reference");
        return reference_asset(asset_type, node, name);
    }
    ++stats.converted[asset_type];
    register_converted(asset_type, name, node);
    return convert_node(node);
}

void ZoneConverter::register_converted(const std::string &asset_type, const std::string &name, Node *node)
{
    // library copies use converted assets of the same name instead of copying them again
    if (!cloner || name.empty() || name[0] == ',' || !asset_record_name(asset_type))
        return;
    Ptr *loader = node->ptr;
    if (!loader || (loader->kind != Ptr::Kind::Follow && loader->kind != Ptr::Kind::Insert))
        return;
    Node *owner = loader->owner;
    if (loader->kind == Ptr::Kind::Follow && (!owner || (owner->block != BLOCK_VIRTUAL && owner->block != 5 && owner->block != 6)))
        return; // the pointer slot is not addressable
    cloner->register_asset(asset_record_name(asset_type), name, loader);
}

Node *ZoneConverter::from_library(const std::string &asset_type, const std::string &name, Node *src_node, bool unsafe_ok)
{
    const char *rec = asset_record_name(asset_type);
    if (!console_library || !rec)
        return nullptr;
    if (console_library->in_game_zones(rec, name))
        return nullptr; // the game's own zones load it: a reference does (and costs no memory)
    auto found = console_library->find(rec, name);
    if (!found)
        return nullptr;
    if (asset_type == "techset" && !unsafe_ok && !console_library->techset_safe(*found->second))
        return nullptr; // CoD Xenon's own shaders the game cannot draw with (library.techset_safe)
    Node *copy;
    try
    {
        copy = cloner->copy_asset(*found->first, found->second);
    }
    catch (const LibraryError &e)
    {
        std::string plain = !name.empty() && name[0] == ',' ? name.substr(1) : name;
        warn(asset_type + " '" + plain + "': cannot be copied from the console library (" + e.what() + ")");
        return nullptr;
    }
    ++stats.copied[asset_type];
    copy->library = true;
    if (asset_type == "image")
        copy->walk([&](Node *n) {
            if (n->delayed)
                stats.texture_bytes += n->data.size();
        });
    else if (asset_type == "loaded_sound")
        copy->walk([&](Node *n) {
            if (n->origin_is(Origin::Member, "snd_asset", "data"))
                stats.sound_bytes += n->data.size();
        });
    Ptr *loader = src_node ? src_node->ptr : nullptr;
    if (loader && (loader->kind == Ptr::Kind::Follow || loader->kind == Ptr::Kind::Insert))
        cloner->register_asset(rec, name, loader); // later copies that use this asset alias its pointer
    if (src_node)
        map_name_string(*this, *src_node, asset_type, *copy);
    return copy;
}

Node *ZoneConverter::reference_asset(const std::string &asset_type, Node *node, const std::string &name)
{
    if (Node *copy = from_library(asset_type, name, node))
    {
        node_map[node] = copy;
        offset_maps[node] = identity;
        return copy;
    }
    ++stats.referenced[asset_type];
    std::string ref_name = !name.empty() && name[0] == ',' ? name : "," + name;
    Node *nw = build_reference(*this, asset_type, *node, ref_name);
    node_map[node] = nw;
    const uint32_t size = static_cast<uint32_t>(nw->data.size());
    offset_maps[node] = [size](uint32_t off) { return off < size ? off : 0; };
    return nw;
}

// -- the zone

std::unique_ptr<Zone> ZoneConverter::convert()
{
    Node *root = zone.root;
    Node *new_root = new_node(root->type, root->count, -1);
    new_root->data.assign(std::vector<uint8_t>(16));

    if (!textures_planned)
        plan_textures_shared({this});
    encode_loaded_sounds(*this);

    for (Node *child : root->children)
    {
        if (child == zone.assets_node)
            new_root->children.push_back(convert_assets_node(child));
        else
            new_root->children.push_back(convert_node(child));
    }
    fix_pointers(new_root);
    apply_string_edits(*this, new_root);

    Zone &z = *out_;
    z.platform = dst.name;
    z.script_strings = script_strings;
    z.root = new_root;
    auto mapped = [&](Node *n) -> Node * {
        if (!n)
            return nullptr;
        auto it = node_map.find(n);
        return it == node_map.end() ? nullptr : it->second;
    };
    z.script_node = mapped(zone.script_node);
    z.assets_node = mapped(zone.assets_node);
    for (const ZoneAsset &a : zone.assets)
    {
        ZoneAsset na;
        na.type = x360_asset_type(a.type);
        na.ptr = a.ptr;
        na.name = a.name;
        z.assets.push_back(std::move(na));
    }
    return std::move(out_);
}

Node *ZoneConverter::convert_assets_node(Node *node)
{
    Node *nw = new_node(node->type, node->count, node->block);
    nw->align = 4;
    nw->origin = Origin::None;
    std::vector<uint8_t> data(node->data.size());
    for (size_t i = 0; i < zone.assets.size(); ++i)
    {
        std::string t = x360_asset_type(zone.assets[i].type);
        auto it = std::find(dst.asset_types.begin(), dst.asset_types.end(), t);
        if (it == dst.asset_types.end())
            throw ConvertError("the console has no asset type " + t);
        dst.put_u32(data.data() + 8 * i, static_cast<uint32_t>(it - dst.asset_types.begin()));
    }
    nw->segments.push_back({src.uint_type, node->count, static_cast<uint32_t>(data.size()), false});
    nw->data.assign(std::move(data));
    node_map[node] = nw;
    offset_maps[node] = identity;
    for (const auto &[off, ptr] : node->relocs.list())
    {
        nw->relocs.set(off, ptr);
        ptrs.emplace_back(ptr, nw);
    }
    const std::string label = progress_label.empty() ? "Converting assets" : progress_label;
    const int count = static_cast<int>(node->children.size());
    for (int i = 0; i < count; ++i)
    {
        progress::step(label, i, count);
        nw->children.push_back(convert_child(node->children[i]));
    }
    progress::step(label, count, count);
    return nw;
}

void ZoneConverter::fix_pointers(Node *root)
{
    auto orphans = map_pointers(0);
    if (root)
        adopt_orphans(root, std::move(orphans));
    else if (!orphans.empty())
    {
        auto [ptr, owner] = *orphans.begin();
        throw ConvertError("reference to unconverted node " + ptr->node->repr() + " (from " + owner->repr() + ")");
    }
    // follow / insert pointers of asset hooks that were built directly
    for (auto &[ptr, owner] : ptrs)
    {
        if (ptr->kind == Ptr::Kind::Insert && ptr->node)
        {
            ptr->node->insert = true;
            ptr->node->ptr = ptr;
        }
    }
}

std::unordered_map<Ptr *, Node *> ZoneConverter::map_pointers(size_t start)
{
    std::unordered_map<Ptr *, Node *> orphans;
    for (size_t i = start; i < ptrs.size(); ++i)
    {
        auto [ptr, owner] = ptrs[i];
        ptr->owner = owner;
        if (ptr->kind == Ptr::Kind::Follow || ptr->kind == Ptr::Kind::Insert)
        {
            auto it = node_map.find(ptr->node);
            if (it == node_map.end())
                throw ConvertError("pointer to unconverted node " + (ptr->node ? ptr->node->repr() : std::string("None")));
            ptr->node = it->second;
            if (ptr->kind == Ptr::Kind::Insert)
                it->second->ptr = ptr;
        }
        else if (ptr->kind == Ptr::Kind::Ref)
        {
            Node *src_target = ptr->node;
            auto it = node_map.find(src_target);
            if (it == node_map.end())
            {
                orphans[ptr] = owner;
                continue;
            }
            uint32_t src_off = ptr->index * src_target->elem_size() + ptr->inner;
            ptr->node = it->second;
            ptr->index = 0;
            ptr->inner = offset_maps.at(src_target)(src_off);
        }
    }
    return orphans;
}

void ZoneConverter::adopt_orphans(Node *root, std::unordered_map<Ptr *, Node *> orphans)
{
    // The PC linker stores equal data once. An asset replaced by a reference to the console's can
    // hold data other assets use (strings, sound names, models and materials it loads that later
    // assets alias): the first pointer to such data, in load order, loads a copy of it; later ones
    // use that copy.
    std::unordered_set<const Node *> live;
    root->walk([&](Node *n) { live.insert(n); });
    if (orphans.empty())
    {
        bool all_live = true;
        root->walk([&](Node *n) {
            for (const auto &[off, p] : n->relocs.list())
                if (p->kind == Ptr::Kind::Alias && p->slot && !live.count(p->slot->owner))
                    all_live = false;
        });
        if (all_live)
            return;
    }
    std::unordered_map<Ptr *, Ptr *> loaders; // a slot nothing converted -> the pointer that now loads its asset
    auto adopt = [&](Ptr *ptr, Node *owner, Node *child, Ptr::Kind kind) {
        load_here(ptr, owner, child, kind);
        child->walk([&](Node *n) { live.insert(n); });
    };

    LoadOrderWalk walk(root);
    Ptr *ptr;
    Node *owner;
    while (walk.next(ptr, owner))
    {
        if (orphans.count(ptr))
        {
            Node *source = ptr->node;
            auto mapped = node_map.find(source);
            if (mapped == node_map.end())
            {
                if (source->string && source->relocs.empty() && (ptr->index || ptr->inner))
                {
                    adopt(ptr, owner, string_copy(*out_, ptr, source), Ptr::Kind::Follow);
                    continue;
                }
                if (ptr->index || ptr->inner)
                    throw ConvertError("reference to unconverted node " + source->repr() + " (from " + owner->repr() + ")");
                size_t count = ptrs.size();
                Node *copy = convert_child(source);
                auto more = map_pointers(count);
                orphans.insert(more.begin(), more.end());
                adopt(ptr, owner, copy, Ptr::Kind::Follow);
                continue;
            }
            uint32_t src_off = ptr->index * source->elem_size() + ptr->inner;
            ptr->node = mapped->second;
            ptr->index = 0;
            ptr->inner = offset_maps.at(source)(src_off);
        }
        else if (ptr->kind == Ptr::Kind::Alias && ptr->slot)
        {
            Ptr *slot = ptr->slot;
            while (slot->kind == Ptr::Kind::Alias && slot->slot && !live.count(slot->owner))
            {
                ptr->slot = slot->slot; // the same pointer, through its slot
                ptr->index = slot->index;
                slot = slot->slot;
            }
            if (live.count(slot->owner))
                continue;
            auto loader = loaders.find(slot);
            if (loader != loaders.end())
            {
                ptr->slot = loader->second;
                ptr->index = 1;
                continue;
            }
            if ((slot->kind != Ptr::Kind::Follow && slot->kind != Ptr::Kind::Insert) || !slot->node)
                throw ConvertError("alias to a pointer of unconverted data " + (slot->owner ? slot->owner->repr() : std::string("None")) +
                                   " (from " + owner->repr() + ")");
            Node *source = slot->node;
            if (node_map.count(source))
                throw ConvertError("alias to an asset loaded by unconverted data " + (slot->owner ? slot->owner->repr() : std::string("None")) +
                                   " (from " + owner->repr() + ")");
            // this pointer loads the asset now (and assets copied from the library alias it)
            ptr->kind = Ptr::Kind::Insert;
            ptr->slot = nullptr;
            ptr->index = 0;
            source->ptr = ptr;
            loaders[slot] = ptr;
            size_t count = ptrs.size();
            Node *copy = convert_child(source);
            auto more = map_pointers(count);
            orphans.insert(more.begin(), more.end());
            adopt(ptr, owner, copy, Ptr::Kind::Insert);
            copy->insert = true;
            copy->ptr = ptr;
        }
    }
}

// -- load order

LoadOrderWalk::LoadOrderWalk(Node *root)
{
    stack.push_back({root, {}, 0, false});
}

bool LoadOrderWalk::next(Ptr *&ptr, Node *&owner)
{
    if (last_ptr)
    {
        // what the pointer just handed out loads, as the consumer left it
        Ptr *p = last_ptr;
        Node *node = last_owner;
        last_ptr = nullptr;
        if ((p->kind == Ptr::Kind::Follow || p->kind == Ptr::Kind::Insert) && p->node && p->owner == node && !visited.count(p->node))
            stack.push_back({p->node, {}, 0, false});
    }
    while (!stack.empty())
    {
        Frame &f = stack.back();
        if (!f.started)
        {
            f.started = true;
            visited.insert(f.node);
            for (const auto &[off, p] : f.node->relocs.list())
                f.relocs.push_back(p);
        }
        if (f.at < f.relocs.size())
        {
            ptr = f.relocs[f.at++];
            owner = f.node;
            last_ptr = ptr;
            last_owner = owner;
            return true;
        }
        // data no pointer of its owner loads (the root's)
        Node *rest = nullptr;
        for (Node *c : f.node->children)
        {
            if (!visited.count(c))
            {
                rest = c;
                break;
            }
        }
        if (rest)
        {
            stack.push_back({rest, {}, 0, false});
            continue;
        }
        stack.pop_back();
    }
    return false;
}

Node *string_copy(Zone &zone, Ptr *ptr, const Node *source)
{
    uint32_t start = ptr->index * source->elem_size() + ptr->inner;
    Node *copy = zone.new_node();
    copy->type = source->type;
    copy->count = 0;
    copy->block = source->block;
    copy->string = true;
    std::vector<uint8_t> data;
    if (start < source->data.size())
        data.assign(source->data.begin() + start, source->data.end());
    if (data.empty() || data.back() != 0)
        data.push_back(0);
    copy->count = static_cast<uint32_t>(data.size());
    copy->segments.push_back({copy->type, copy->count, copy->count, false});
    copy->align = source->align.value_or(1);
    copy->data.assign(std::move(data));
    return copy;
}

void load_here(Ptr *ptr, Node *owner, Node *child, Ptr::Kind kind)
{
    std::unordered_map<const Node *, uint32_t> loaded_at;
    for (const auto &[off, p] : owner->relocs.list())
        if ((p->kind == Ptr::Kind::Follow || p->kind == Ptr::Kind::Insert) && p->node)
            loaded_at[p->node] = off;
    size_t position = owner->children.size();
    for (size_t i = 0; i < owner->children.size(); ++i)
    {
        auto it = loaded_at.find(owner->children[i]);
        if (it != loaded_at.end() && static_cast<int64_t>(it->second) > static_cast<int64_t>(ptr->offset))
        {
            position = i;
            break;
        }
    }
    owner->children.insert(owner->children.begin() + position, child);
    ptr->kind = kind;
    ptr->node = child;
    ptr->slot = nullptr;
    ptr->index = 0;
    ptr->inner = 0;
    ptr->owner = owner;
}
} // namespace t4ff
