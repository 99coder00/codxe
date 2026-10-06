#include "convert/assets.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include "convert/converter.h"
#include "convert/library.h"
#include "convert/techsets.h"
#include "convert/xanim.h"
#include "core/progress.h"
#include "core/threads.h"
#include "core/xenos.h"

namespace t4ff
{
namespace fs = std::filesystem;

namespace
{
// PC technique indices -> console technique indices: the console build has no instanced lit
// techniques (PC 0x24-0x2A) and no DEBUG_BUMPMAP_INSTANCED (PC 0x3A)
constexpr int X360_TECHNIQUE_COUNT = 51;

std::vector<std::pair<int, int>> pc_technique_to_x360()
{
    std::vector<std::pair<int, int>> m;
    for (int i = 0; i < 0x24; ++i)
        m.emplace_back(i, i);
    for (int i = 0x2B; i < 0x3A; ++i)
        m.emplace_back(i, i - 7);
    return m;
}

// high mip streaming bounds of a surface whose textures are never streamed (an empty box)
const float NO_STREAM_BOUNDS[6] = {131072.0f, 131072.0f, 131072.0f, -131072.0f, -131072.0f, -131072.0f};
const char *XMODEL_MEMBERS_AFTER_STREAM_INFO[] = {"physPreset", "physGeoms", "collmap", "physConstraints"};
constexpr uint32_t PRIMED_SOUND_SIZE = 0x8000;
constexpr uint32_t MIN_HIGHMIP_BYTES = 128 * 1024; // stream.MIN_HIGHMIP_BYTES

std::string strip_commas(const std::string &s)
{
    size_t i = s.find_first_not_of(',');
    return i == std::string::npos ? std::string() : s.substr(i);
}

bool starts_comma(const std::string &s)
{
    return !s.empty() && s[0] == ',';
}

std::string text_of(const Node &n)
{
    std::string s(n.data.begin(), n.data.end());
    while (!s.empty() && s.back() == '\0')
        s.pop_back();
    return s;
}

uint32_t identity(uint32_t off)
{
    return off;
}

// a node's origin is (any kind, record, field), as the Python origin[1:] tuples compare
bool origin_member(const Node &n, std::string_view rec, std::string_view field)
{
    return (n.origin == Origin::Member || n.origin == Origin::PtrArray) && n.origin_record && *n.origin_record == rec && n.origin_field &&
           *n.origin_field == field;
}

const Field &field_of(const Platform &p, const std::string &rec, const char *name)
{
    const Field *f = p.record(rec).field(name);
    if (!f)
        throw ConvertError(rec + " has no field " + name);
    return *f;
}

uint64_t read_uint(const Platform &p, const uint8_t *d, uint32_t size)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; ++i)
        v |= uint64_t(d[p.big_endian ? i : size - 1 - i]) << (8 * (size - 1 - i));
    return v;
}

void write_uint(const Platform &p, uint8_t *d, uint32_t size, uint64_t v)
{
    for (uint32_t i = 0; i < size; ++i)
        d[p.big_endian ? i : size - 1 - i] = uint8_t(v >> (8 * (size - 1 - i)));
}

// the value of a field of 1, 2 or 4 bytes ({1: "B", 2: "H", 4: "I"})
uint32_t get_field(const Platform &p, const std::string &rec, const Node &node, const char *name, uint32_t base = 0)
{
    const Field &f = field_of(p, rec, name);
    uint32_t size = f.type->kind == TypeKind::Array ? f.type->elem->size : f.type->size;
    if (base + f.offset + size > node.data.size())
        throw ConvertError(rec + "." + name + " outside of the data");
    return static_cast<uint32_t>(read_uint(p, node.data.data() + base + f.offset, size));
}

void put_field(const Platform &p, const std::string &rec, Node &node, const char *name, uint32_t value, uint32_t base = 0)
{
    const Field &f = field_of(p, rec, name);
    uint32_t size = f.type->kind == TypeKind::Array ? f.type->elem->size : f.type->size;
    write_uint(p, node.data.mutable_data() + base + f.offset, size, value);
}

template <typename F> void member_nodes(Node *root, std::string_view rec, std::string_view member, F f)
{
    root->walk([&](Node *n) {
        if (n->origin_is(Origin::Member, rec, member))
            f(n);
    });
}

// os.path.splitext's root
std::string splitext_root(const std::string &path)
{
    size_t sep = path.find_last_of("/\\");
    size_t base = sep == std::string::npos ? 0 : sep + 1;
    size_t first = path.find_first_not_of('.', base);
    if (first == std::string::npos)
        return path;
    size_t dot = path.rfind('.');
    if (dot == std::string::npos || dot < first)
        return path;
    return path.substr(0, dot);
}

std::string splitext_ext(const std::string &path)
{
    return path.substr(splitext_root(path).size());
}

Node *find_child(Node &n, const std::function<bool(const Node &)> &pred)
{
    for (Node *c : n.children)
        if (pred(*c))
            return c;
    return nullptr;
}

Node *reference_or_copy(ZoneConverter &conv, const std::string &asset_type, Node *node, const std::string &name);
} // namespace

// -- helpers

uint32_t name_offset(const Platform &p, const std::string &rec_name)
{
    std::vector<const char *> path;
    if (rec_name == "Material")
        path = {"info", "name"};
    else if (rec_name == "WeaponDef")
        path = {"szInternalName"};
    else if (rec_name == "snd_alias_list_t")
        path = {"aliasName"};
    else if (rec_name == "Font_s")
        path = {"fontName"};
    else if (rec_name == "menuDef_t")
        path = {"window", "name"};
    else
        path = {"name"};
    const Record *rec = &p.record(rec_name);
    uint32_t offset = 0;
    for (size_t i = 0; i < path.size(); ++i)
    {
        const Field *f = rec->field(path[i]);
        if (!f)
            throw std::out_of_range(rec_name + " has no name field");
        offset += f->offset;
        if (i + 1 < path.size())
            rec = &p.record(f->type->name);
    }
    return offset;
}

Node *string_node(Zone &zone, const Platform &p, const std::string &text)
{
    Node *node = zone.new_node();
    node->type = p.char_type;
    node->block = BLOCK_VIRTUAL;
    node->string = true;
    std::vector<uint8_t> data(text.begin(), text.end());
    data.push_back(0);
    node->count = static_cast<uint32_t>(data.size());
    node->align = 1;
    node->segments.push_back({p.char_type, node->count, node->count, false});
    node->data.assign(std::move(data));
    return node;
}

Ptr *follow(Zone &zone, Node *owner, uint32_t offset, Node *child)
{
    Ptr *ptr = zone.new_ptr(Ptr::Kind::Follow);
    ptr->node = child;
    ptr->owner = owner;
    ptr->offset = offset;
    owner->relocs.set(offset, ptr);
    owner->children.push_back(child);
    return ptr;
}

Node *asset_header(Zone &zone, const Platform &dst, const std::string &rec_name, std::optional<uint32_t> size)
{
    const Record &rec = dst.record(rec_name);
    std::optional<int> tb = dst.type_block(rec);
    bool in_temp = tb && *tb == BLOCK_TEMP;
    Node *node = zone.new_node();
    node->type = rec.self;
    node->count = 1;
    node->block = static_cast<int8_t>(in_temp ? BLOCK_TEMP : BLOCK_VIRTUAL);
    if (in_temp)
        node->push_before = BLOCK_TEMP;
    node->push_after = BLOCK_VIRTUAL;
    node->align = dst.type_alloc_align(rec_name, rec.align);
    node->origin = Origin::Asset;
    node->origin_record = &rec.name;
    node->data.assign(std::vector<uint8_t>(size.value_or(rec.size)));
    node->segments.push_back({rec.self, 1, static_cast<uint32_t>(node->data.size()), false});
    return node;
}

Node *build_reference(Zone &zone, const Platform &dst, const std::string &asset_type, const Node &src_node, const std::string &ref_name)
{
    const char *rec_name = asset_record_name(asset_type);
    if (!rec_name)
        throw ConvertError("not an asset type: " + asset_type);
    Node *node = asset_header(zone, dst, rec_name);
    node->asset = src_node.asset;
    Node *name = string_node(zone, dst, ref_name);
    follow(zone, node, name_offset(dst, rec_name), name);
    return node;
}

namespace
{
Zone &out_zone(ZoneConverter &conv);

// other assets can point into the name string of a rebuilt asset (the PC linker shares equal
// strings): those pointers resolve to the new name string
void map_name_string_to(ZoneConverter &conv, const Node &src_node, const std::string &rec_name, Node *new_string)
{
    Ptr *ptr;
    try
    {
        ptr = src_node.relocs.get(name_offset(conv.src, rec_name));
    }
    catch (const std::out_of_range &)
    {
        return;
    }
    Node *old = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
    if (!old || !old->string)
        return;
    std::string old_text = text_of(*old), new_text = text_of(*new_string);
    if (new_text.size() < old_text.size() || new_text.compare(new_text.size() - old_text.size(), old_text.size(), old_text) != 0)
        return;
    uint32_t shift = static_cast<uint32_t>(new_text.size() - old_text.size());
    conv.node_map.emplace(old, new_string);
    conv.offset_maps.emplace(old, [shift](uint32_t off) { return off + shift; });
}
} // namespace

Node *build_reference(ZoneConverter &conv, const std::string &asset_type, const Node &src_node, const std::string &ref_name)
{
    const char *rec_name = asset_record_name(asset_type);
    if (!rec_name)
        throw ConvertError("not an asset type: " + asset_type);
    Node *node = asset_header(out_zone(conv), conv.dst, rec_name);
    node->asset = src_node.asset;
    Node *name = string_node(out_zone(conv), conv.dst, ref_name);
    follow(out_zone(conv), node, name_offset(conv.dst, rec_name), name);
    map_name_string_to(conv, src_node, rec_name, name);
    return node;
}

void map_name_string(ZoneConverter &conv, const Node &src_node, const std::string &asset_type, Node &new_asset)
{
    const char *rec_name = asset_record_name(asset_type);
    if (!rec_name)
        return;
    Ptr *ptr;
    try
    {
        ptr = new_asset.relocs.get(name_offset(conv.dst, rec_name));
    }
    catch (const std::out_of_range &)
    {
        return;
    }
    if (ptr && ptr->kind == Ptr::Kind::Follow && ptr->node && ptr->node->string)
        map_name_string_to(conv, src_node, rec_name, ptr->node);
}

// -- images

bool pc_normal_map(const ImageData &source, int semantic)
{
    return semantic == TS_NORMAL_MAP && source.source != "console";
}

namespace
{
std::optional<Fmt> pc_d3d_format(uint32_t v)
{
    switch (v)
    {
    case 0x31545844: return Fmt::DXT1;
    case 0x33545844: return Fmt::DXT3;
    case 0x35545844: return Fmt::DXT5;
    case 21: return Fmt::A8R8G8B8;
    case 22: return Fmt::X8R8G8B8;
    case 50: return Fmt::L8;
    case 51: return Fmt::A8L8;
    case 28: return Fmt::A8;
    }
    return std::nullopt;
}

std::optional<ImageData> image_from_load_def(const Platform &p, const std::string &name, const Node &load_def)
{
    const Record &rec = p.record("GfxImageLoadDef");
    int level_count = load_def.data[0], flags = load_def.data[1];
    uint32_t dims = rec.field("dimensions")->offset;
    const uint8_t *d = load_def.data.data();
    uint32_t width = d[dims] | d[dims + 1] << 8, height = d[dims + 2] | d[dims + 3] << 8;
    uint32_t fmt_value = static_cast<uint32_t>(read_uint(p, d + rec.field("format")->offset, 4));
    std::optional<Fmt> fmt = pc_d3d_format(fmt_value);
    if (!fmt)
    {
        char buf[16];
        snprintf(buf, sizeof buf, "%#x", fmt_value);
        throw ImageError(name + ": unsupported PC texture format " + buf);
    }
    uint32_t data_off = rec.field("data")->offset;
    const uint8_t *data = d + std::min<size_t>(data_off, load_def.data.size());
    size_t size = load_def.data.size() > data_off ? load_def.data.size() - data_off : 0;
    if (flags & IWI_FLAG_CUBEMAP)
    {
        // a cube map (reflection probe) holds the whole mip chain of each face after the other; its
        // level count is 0 for a full chain
        int count = level_count ? level_count : mip_count(width, height);
        std::vector<size_t> sizes;
        size_t chain = 0;
        for (int i = 0; i < count; ++i)
        {
            sizes.push_back(level_size(*fmt, std::max(width >> i, 1u), std::max(height >> i, 1u)));
            chain += sizes.back();
        }
        if (chain * 6 > size)
            throw ImageError(name + ": truncated cube map (" + std::to_string(size) + " < " + std::to_string(chain * 6) + ")");
        std::vector<ImageData> faces;
        for (int f = 0; f < 6; ++f)
        {
            ImageData face;
            face.name = name;
            face.format = *fmt;
            face.width = width;
            face.height = height;
            face.flags = flags;
            face.source = "zone";
            size_t pos = f * chain;
            for (size_t s : sizes)
            {
                face.levels.emplace_back(data + pos, data + pos + s);
                pos += s;
            }
            faces.push_back(std::move(face));
        }
        return join_faces(faces);
    }
    ImageData image;
    image.name = name;
    image.format = *fmt;
    image.width = width;
    image.height = height;
    image.flags = flags;
    image.source = "zone";
    uint32_t w = width, h = height;
    size_t pos = 0;
    for (int i = 0; i < std::max(level_count, 1); ++i)
    {
        size_t s = level_size(*fmt, w, h);
        if (pos + s > size)
            break;
        image.levels.emplace_back(data + pos, data + pos + s);
        pos += s;
        w = std::max(w >> 1, 1u);
        h = std::max(h >> 1, 1u);
    }
    if (image.levels.empty())
        return std::nullopt;
    return image;
}

bool in_game_zones(ZoneConverter &conv, const std::string &rec_name, const std::string &name)
{
    return conv.console_library && conv.console_library->in_game_zones(rec_name, name);
}
} // namespace

namespace
{
// image_source's reading, the warnings it gives collected
std::optional<ImageData> read_image_source(ZoneConverter &conv, Node &node, const std::string &name, std::vector<std::string> &warnings)
{
    std::optional<ImageData> source;
    Node *load_def = find_child(node, [](const Node &c) { return c.is_record("GfxImageLoadDef"); });
    if (load_def)
    {
        const Field &f = field_of(conv.src, "GfxImageLoadDef", "resourceSize");
        if (read_uint(conv.src, load_def->data.data() + f.offset, 4))
        {
            try
            {
                source = image_from_load_def(conv.src, name, *load_def);
            }
            catch (const ImageError &e)
            {
                warnings.emplace_back(e.what());
            }
        }
    }
    if (!source && conv.library)
    {
        try
        {
            source = conv.library->image(name);
        }
        catch (const ImageError &e)
        {
            warnings.emplace_back(e.what());
        }
    }
    return source;
}
} // namespace

void prefetch_image_sources(const std::vector<ZoneConverter *> &convs)
{
    struct Job
    {
        ZoneConverter *conv;
        Node *node;
        std::string name;
    };
    std::vector<Job> jobs;
    for (ZoneConverter *conv : convs)
    {
        // the first image of each name, as the planner asks for them
        std::unordered_set<std::string> seen;
        conv->zone.root->walk([&](Node *node) {
            if (!node->is_record("GfxImage") || node->origin != Origin::Asset)
                return;
            std::string name = asset_display_name(conv->src, *node);
            if (name.empty() || starts_comma(name) || conv->image_sources.count(name))
                return;
            // the planner reads no volume image
            int map_type = node->data[conv->src.record("GfxImage").field("mapType")->offset];
            if ((map_type != MAPTYPE_2D && map_type != MAPTYPE_CUBE) || !seen.insert(name).second)
                return;
            jobs.push_back({conv, node, name});
        });
    }
    std::vector<ZoneConverter::PrefetchedImage> results(jobs.size());
    parallel_for(jobs.size(), convs.empty() ? 0 : convs[0]->options.jobs, [&](size_t i) {
        auto source = read_image_source(*jobs[i].conv, *jobs[i].node, jobs[i].name, results[i].warnings);
        if (source)
            results[i].source = std::make_shared<const ImageData>(std::move(*source));
    });
    for (size_t i = 0; i < jobs.size(); ++i)
        jobs[i].conv->prefetched_images[jobs[i].name] = std::move(results[i]);
}

ImageRef image_source(ZoneConverter &conv, Node &node, const std::string &name)
{
    auto cached = conv.image_sources.find(name);
    if (cached != conv.image_sources.end())
        return cached->second;
    auto prefetched = conv.prefetched_images.find(name);
    if (prefetched != conv.prefetched_images.end())
    {
        for (const std::string &w : prefetched->second.warnings)
            conv.warn(w);
        ImageRef ref = prefetched->second.source;
        conv.prefetched_images.erase(prefetched);
        conv.image_sources[name] = ref;
        return ref;
    }
    std::optional<ImageData> source;
    Node *load_def = find_child(node, [](const Node &c) { return c.is_record("GfxImageLoadDef"); });
    if (load_def)
    {
        const Field &f = field_of(conv.src, "GfxImageLoadDef", "resourceSize");
        if (read_uint(conv.src, load_def->data.data() + f.offset, 4))
        {
            try
            {
                source = image_from_load_def(conv.src, name, *load_def);
            }
            catch (const ImageError &e)
            {
                conv.warn(e.what());
            }
        }
    }
    if (!source && conv.library)
    {
        try
        {
            source = conv.library->image(name);
        }
        catch (const ImageError &e)
        {
            conv.warn(e.what());
        }
    }
    ImageRef ref = source ? std::make_shared<const ImageData>(std::move(*source)) : nullptr;
    conv.image_sources[name] = ref;
    return ref;
}

ImageRef stock_image_source(ZoneConverter &conv, const std::string &name)
{
    auto cached = conv.stock_images.find(name);
    if (cached != conv.stock_images.end())
        return cached->second;
    std::optional<ImageData> source;
    try
    {
        if (conv.stock_library)
            source = conv.stock_library->image(name);
    }
    catch (const ImageError &e)
    {
        conv.warn(e.what());
    }
    ImageRef ref = source ? std::make_shared<const ImageData>(std::move(*source)) : nullptr;
    conv.stock_images[name] = ref;
    return ref;
}

std::pair<std::string, ImageRef> image_choice(ZoneConverter &conv, Node &node, const std::string &name)
{
    if (ImageRef source = image_source(conv, node, name))
        return {"own", source};
    if (in_game_zones(conv, "GfxImage", name))
        return {"game", nullptr};
    if (conv.console_library && conv.console_library->find("GfxImage", name))
        return {"library", nullptr};
    if (ImageRef source = stock_image_source(conv, name))
        return {"stock", source};
    return {"missing", nullptr};
}

std::optional<ImageData> decode_console_image(const Platform &p, Node &node, const std::string &name)
{
    int map_type = static_cast<int>(get_field(p, "GfxImage", node, "mapType"));
    if (map_type != MAPTYPE_2D && map_type != MAPTYPE_CUBE)
        return std::nullopt;
    Node *load_def = nullptr;
    node.walk([&](Node *c) {
        if (!load_def && c->is_record("GfxImageLoadDef"))
            load_def = c;
    });
    Node *pixels = find_child(node, [](const Node &c) { return c.delayed || origin_member(c, "GfxImage", "pixels"); });
    if (!load_def || !pixels)
        return std::nullopt;
    const Record &ld = p.record("GfxImageLoadDef");
    int levels = load_def->data[ld.field("levelCount")->offset];
    int flags = load_def->data[ld.field("flags")->offset];
    uint32_t d3d = p.u32(load_def->data.data() + ld.field("format")->offset);
    const xenos::Format *fmt = xenos::format_of_d3d(d3d);
    uint32_t width = get_field(p, "GfxImage", node, "width"), height = get_field(p, "GfxImage", node, "height");
    if (!fmt || !width || !height)
        return std::nullopt;
    levels = std::max(levels, 1);
    int faces = map_type == MAPTYPE_CUBE ? 6 : 1;
    if (xenos::mip_chain_layout(width, height, *fmt, levels, faces).total > pixels->data.size())
        return std::nullopt;
    ImageData image;
    image.name = name;
    image.format = fmt->name;
    image.width = width;
    image.height = height;
    image.flags = flags;
    image.source = "console";
    image.faces = faces;
    if (levels > 1 || faces > 1)
        image.levels = xenos::untile_mip_chain(pixels->data.data(), pixels->data.size(), width, height, *fmt, levels, faces);
    else
        image.levels = {xenos::untile_level(pixels->data.data(), pixels->data.size(), width, height, 0, *fmt)};
    return image;
}

ImageRef console_image(ZoneConverter &conv, const std::string &name)
{
    auto cached = conv.console_images.find(name);
    if (cached != conv.console_images.end())
        return cached->second;
    std::optional<ImageData> result;
    if (conv.console_library)
    {
        if (auto found = conv.console_library->find("GfxImage", name))
        {
            try
            {
                result = decode_console_image(conv.dst, *found->second, name);
            }
            catch (const std::exception &)
            {
                result.reset();
            }
        }
    }
    ImageRef ref = result ? std::make_shared<const ImageData>(std::move(*result)) : nullptr;
    conv.console_images[name] = ref;
    return ref;
}

Node *build_console_image(Zone &zone, const Platform &dst, const std::string &name, const ConsoleTexture &tex, uint32_t semantic, uint32_t category,
                          int iwi_flags, Node **name_out)
{
    auto new_node = [&](const TypeRef *type, uint32_t count, int block) {
        Node *n = zone.new_node();
        n->type = type;
        n->count = count;
        n->block = static_cast<int8_t>(block);
        return n;
    };
    Node *image = asset_header(zone, dst, "GfxImage");
    image->asset = "image";
    put_field(dst, "GfxImage", *image, "mapType", tex.faces == 6 ? MAPTYPE_CUBE : MAPTYPE_2D);
    put_field(dst, "GfxImage", *image, "semantic", semantic);
    put_field(dst, "GfxImage", *image, "cardMemory", static_cast<uint32_t>(tex.pixels.size()));
    put_field(dst, "GfxImage", *image, "width", tex.width);
    put_field(dst, "GfxImage", *image, "height", tex.height);
    put_field(dst, "GfxImage", *image, "depth", 1);
    put_field(dst, "GfxImage", *image, "category", category);
    put_field(dst, "GfxImage", *image, "delayLoadPixels", 1);
    put_field(dst, "GfxImage", *image, "baseSize", tex.base_size ? tex.base_size : static_cast<uint32_t>(tex.pixels.size()));
    put_field(dst, "GfxImage", *image, "streamSlot", 0xFFFF);
    put_field(dst, "GfxImage", *image, "streaming", 0);

    // name, texture (load def + header), pixels: in the console load order
    Node *name_string = string_node(zone, dst, name);
    follow(zone, image, field_of(dst, "GfxImage", "name").offset, name_string);
    if (name_out)
        *name_out = name_string;

    const Record &ld_rec = dst.record("GfxImageLoadDef");
    Node *load_def = new_node(ld_rec.self, 1, BLOCK_TEMP);
    load_def->push_before = BLOCK_TEMP;
    load_def->align = ld_rec.align;
    load_def->origin = Origin::Member;
    load_def->origin_record = intern("GfxTexture");
    load_def->origin_field = intern("loadDef");
    std::vector<uint8_t> ld(ld_rec.size);
    int flags = (iwi_flags & 0x3) | (tex.faces == 6 ? IWI_FLAG_CUBEMAP : 0);
    ld[0] = static_cast<uint8_t>(tex.levels);
    ld[1] = static_cast<uint8_t>(flags);
    write_uint(dst, ld.data() + 2, 2, tex.width);
    write_uint(dst, ld.data() + 4, 2, tex.height);
    write_uint(dst, ld.data() + 6, 2, 1);
    write_uint(dst, ld.data() + 8, 4, tex.format->d3d);
    load_def->data.assign(std::move(ld));
    load_def->segments.push_back({ld_rec.self, 1, ld_rec.size, false});
    follow(zone, image, field_of(dst, "GfxImage", "texture").offset, load_def);

    const Record &hdr_rec = dst.record("D3DBaseTexture360");
    Node *header = new_node(hdr_rec.self, 1, BLOCK_VIRTUAL);
    header->push_before = BLOCK_VIRTUAL;
    header->align = hdr_rec.align;
    header->origin = Origin::Member;
    header->origin_record = intern("GfxImageLoadDef");
    header->origin_field = intern("texture");
    header->data.assign(tex.header);
    header->segments.push_back({hdr_rec.self, 1, static_cast<uint32_t>(tex.header.size()), false});
    follow(zone, load_def, ld_rec.field("texture")->offset, header);

    Node *pixels = new_node(dst.uchar_type, static_cast<uint32_t>(tex.pixels.size()), BLOCK_LARGE_RUNTIME);
    pixels->align = 4096;
    pixels->delayed = true;
    pixels->origin = Origin::Member;
    pixels->origin_record = intern("GfxImage");
    pixels->origin_field = intern("pixels");
    pixels->segments.push_back({pixels->type, pixels->count, pixels->count, false});
    pixels->data.assign(tex.pixels);
    follow(zone, image, field_of(dst, "GfxImage", "pixels").offset, pixels);
    return image;
}

namespace
{
Node *build_console_image(ZoneConverter &conv, const std::string &name, const ConsoleTexture &tex, uint32_t semantic, uint32_t category,
                          int iwi_flags, const Node *src_node = nullptr)
{
    Node *name_string = nullptr;
    Node *image = build_console_image(out_zone(conv), conv.dst, name, tex, semantic, category, iwi_flags, &name_string);
    if (src_node)
        map_name_string_to(conv, *src_node, "GfxImage", name_string);
    return image;
}

TextureOptions texture_options(const ConvertOptions &o, int drop, bool normal_map = false)
{
    TextureOptions t;
    t.max_size = o.max_texture_size;
    t.keep_mips = o.keep_mips;
    t.drop_levels = drop;
    t.compress = o.compress_textures;
    t.normal_map = normal_map;
    t.mip_tail = o.mip_tail;
    return t;
}

int drop_of(ZoneConverter &conv, const std::string &name)
{
    auto it = conv.image_drop_levels.find(name);
    return it == conv.image_drop_levels.end() ? 0 : it->second;
}
} // namespace

Node *rebuild_console_image(ZoneConverter &conv, const std::string &name, Node *library_node)
{
    ImageRef source = console_image(conv, name);
    if (!source)
    {
        if (auto decoded = decode_console_image(conv.dst, *library_node, name))
            source = std::make_shared<const ImageData>(std::move(*decoded));
    }
    if (!source)
        return nullptr;
    ConsoleTexture tex;
    try
    {
        tex = build_console_texture(*source, texture_options(conv.options, drop_of(conv, name)));
    }
    catch (const ImageError &)
    {
        return nullptr;
    }
    conv.stats.texture_bytes += tex.pixels.size();
    Node *image = build_console_image(conv, name, tex, get_field(conv.dst, "GfxImage", *library_node, "semantic"),
                                      get_field(conv.dst, "GfxImage", *library_node, "category"), source->flags);
    image->library = true;
    return image;
}

// -- planning texture memory

DropPlan choose_drops(const std::vector<std::string> &names, const std::function<uint64_t(const std::string &, int)> &size,
                      const std::function<bool(const std::string &, int)> &can_drop, uint64_t budget, const std::set<std::string> &last,
                      const std::function<int(const std::string &, int)> &tier)
{
    DropPlan plan;
    std::unordered_map<std::string, int64_t> sizes;
    int64_t total = 0;
    for (const auto &n : names)
    {
        plan.drops[n] = 0;
        sizes[n] = static_cast<int64_t>(size(n, 0));
        total += sizes[n];
    }
    // saving: a dict in the order of names (a removed name loses its place)
    std::vector<std::string> order;
    std::unordered_map<std::string, int64_t> saving;
    for (const auto &n : names)
        if (can_drop(n, 0))
        {
            order.push_back(n);
            saving[n] = sizes[n] - static_cast<int64_t>(size(n, 1));
        }
    auto rank = [&](const std::string &n) { return tier ? tier(n, plan.drops[n]) : 0; };
    while (total > static_cast<int64_t>(budget))
    {
        std::vector<const std::string *> candidates;
        for (const auto &n : order)
            if (saving.count(n) && !last.count(n) && saving[n] > 0)
                candidates.push_back(&n);
        if (candidates.empty())
            for (const auto &n : order)
                if (saving.count(n) && saving[n] > 0)
                    candidates.push_back(&n);
        if (candidates.empty())
        {
            plan.total = static_cast<uint64_t>(total);
            plan.fits = false;
            return plan;
        }
        int lowest = rank(*candidates[0]);
        for (const auto *n : candidates)
            lowest = std::min(lowest, rank(*n));
        const std::string *best = nullptr;
        for (const auto *n : candidates)
        {
            if (rank(*n) != lowest)
                continue;
            if (!best || std::make_pair(saving[*n], sizes[*n]) > std::make_pair(saving[*best], sizes[*best]))
                best = n; // max(): the first of the largest
        }
        const std::string b = *best;
        int k = ++plan.drops[b];
        int64_t nw = static_cast<int64_t>(size(b, k));
        total += nw - sizes[b];
        sizes[b] = nw;
        if (can_drop(b, k))
            saving[b] = nw - static_cast<int64_t>(size(b, k + 1));
        else
        {
            saving.erase(b);
            order.erase(std::find(order.begin(), order.end(), b));
        }
    }
    plan.total = static_cast<uint64_t>(total);
    return plan;
}

namespace
{
std::string asset_name_pc(ZoneConverter &conv, const Node &node)
{
    return asset_display_name(conv.src, node);
}

// names of the images the 2D materials (technique set "2d") of the zone use
std::set<std::string> ui_images(ZoneConverter &conv)
{
    const Record &rec = conv.src.record("Material");
    uint32_t techset_off = rec.field("techniqueSet")->offset, table_off = rec.field("textureTable")->offset;
    uint32_t count_off = rec.field("textureCount")->offset;
    const Record &entry = conv.src.record("MaterialTextureDef");
    uint32_t image_off = entry.field("u")->offset;
    std::set<std::string> names;
    conv.zone.root->walk([&](Node *node) {
        if (!node->is_record("Material") || node->origin != Origin::Asset)
            return;
        Ptr *ptr = node->relocs.get(techset_off);
        Node *techset = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
        if (!techset || strip_commas(asset_name_pc(conv, *techset)) != "2d")
            return;
        ptr = node->relocs.get(table_off);
        Node *table = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
        int count = table ? node->data[count_off] : 0;
        for (int i = 0; i < count; ++i)
        {
            Ptr *ip = table->relocs.get(i * entry.size + image_off);
            Node *image = ip && ip->kind != Ptr::Kind::Null ? ip->target() : nullptr;
            std::string name = image ? asset_name_pc(conv, *image) : "";
            if (!name.empty())
                names.insert(strip_commas(name));
        }
    });
    return names;
}

bool planned_format(Fmt f)
{
    switch (f)
    {
    case Fmt::DXT1:
    case Fmt::DXT3:
    case Fmt::DXT5:
    case Fmt::DXN:
    case Fmt::A8R8G8B8:
    case Fmt::R8G8B8:
    case Fmt::A8L8:
    case Fmt::A8:
    case Fmt::L8: return true;
    default: return false;
    }
}
} // namespace

void plan_textures_shared(const std::vector<ZoneConverter *> &convs)
{
    ConvertOptions &options = convs[0]->options;
    std::vector<std::string> order; // sources, in the order found (a dict)
    std::unordered_map<std::string, ImageRef> sources;
    std::set<std::string> normal_maps, fixed, copies;
    uint32_t map_type_off = convs[0]->src.record("GfxImage").field("mapType")->offset;
    uint32_t semantic_off = convs[0]->src.record("GfxImage").field("semantic")->offset;
    auto add_source = [&](const std::string &name, ImageRef src) {
        if (!sources.count(name))
            order.push_back(name);
        sources[name] = std::move(src);
    };
    for (ZoneConverter *conv : convs)
    {
        conv->zone.root->walk([&](Node *node) {
            if (!node->is_record("GfxImage") || node->origin != Origin::Asset)
                return;
            std::string name = asset_name_pc(*conv, *node);
            if (name.empty())
                return;
            std::string plain = strip_commas(name);
            int map_type = node->data[map_type_off];
            if (sources.count(plain) || (!starts_comma(name) && map_type != MAPTYPE_2D && map_type != MAPTYPE_CUBE))
                return; // volume maps are left to the console
            std::string kind;
            ImageRef src;
            if (starts_comma(name))
                kind = in_game_zones(*conv, "GfxImage", plain) ? "game" : "library";
            else
                std::tie(kind, src) = image_choice(*conv, *node, plain);
            if (kind == "library" && options.texture_budget)
            {
                src = console_image(*conv, plain);
                copies.insert(plain);
            }
            if (src && planned_format(src->format))
            {
                if (pc_normal_map(*src, node->data[semantic_off]))
                    normal_maps.insert(plain);
                // the world's own images (lightmaps, $outdoor) keep their size, and single level images
                // of formats reduce_image cannot scale down
                bool scalable = is_block_compressed(src->format) || src->format == Fmt::A8R8G8B8 || src->format == Fmt::X8R8G8B8;
                if (plain.substr(0, 1).find_first_of("*$") == 0 || plain.empty() || (src->levels.size() == 1 && !scalable))
                    fixed.insert(plain);
                add_source(plain, src);
            }
        });
    }

    // textures that console library copies of PC references (materials, models, effects...) bring along
    if (options.texture_budget)
    {
        for (ZoneConverter *conv : convs)
        {
            ConsoleLibrary *library = conv->console_library;
            if (!library)
                continue;
            conv->zone.root->walk([&](Node *node) {
                if (node->origin != Origin::Asset || *node->origin_record == "GfxImage")
                    return;
                std::string name = asset_name_pc(*conv, *node);
                if (name.empty() || !starts_comma(name) || library->in_game_zones(*node->origin_record, name))
                    return;
                auto found = library->find(*node->origin_record, name);
                if (!found)
                    return;
                found->second->walk([&](Node *nested) {
                    if (nested->origin != Origin::Asset || *nested->origin_record != "GfxImage")
                        return;
                    std::string image_name = asset_display_name(conv->dst, *nested);
                    if (!image_name.empty() && !sources.count(image_name))
                    {
                        if (ImageRef src = console_image(*conv, image_name))
                        {
                            add_source(image_name, src);
                            copies.insert(image_name);
                        }
                    }
                });
            });
        }
    }

    const auto &steps = options.stream_steps;
    auto steps_of = [&](const std::string &n) {
        auto it = steps.find(lower_latin1(n));
        return it == steps.end() ? 0 : it->second;
    };
    // a deep streamed image may keep an eighth of its size (more than 16 texels a side)
    auto eighth = [&](const std::string &n, int drop) {
        const ImageData &src = *sources.at(n);
        return options.eighth_levels && steps_of(n) >= 2 && std::min(src.width >> (drop + 3), src.height >> (drop + 3)) > 16;
    };
    // (top levels dropped, levels streamed) of image n after k steps
    auto state = [&](const std::string &n, int k) -> std::pair<int, int> {
        int streamed = steps_of(n);
        if (k && eighth(n, 0))
            return {k - 1, eighth(n, k - 1) ? 3 : 2};
        return {k, streamed};
    };
    auto size = [&](const std::string &n, int k) -> uint64_t {
        auto [drop, streamed] = state(n, k);
        const ImageData &src = *sources.at(n);
        bool normal = normal_maps.count(n) != 0;
        if (streamed)
        {
            TextureOptions o = texture_options(options, drop, normal);
            o.keep_mips = false;
            o.mip_tail = true;
            if (console_texture_size(src, o) >= MIN_HIGHMIP_BYTES)
                drop += streamed;
        }
        // pixel data is 4 KiB aligned in the zone
        return (static_cast<uint64_t>(console_texture_size(src, texture_options(options, drop, normal), streamed != 0)) + 4095) & ~uint64_t(4095);
    };
    auto can_drop = [&](const std::string &n, int k) {
        const ImageData &src = *sources.at(n);
        int drop = state(n, k + 1).first;
        return !fixed.count(n) && std::min(drop < 32 ? src.width >> drop : 0u, drop < 32 ? src.height >> drop : 0u) >= 64;
    };
    auto tier = [&](const std::string &n, int k) { return k == 0 && eighth(n, 0) ? 0 : 1; };

    uint64_t before = 0;
    for (const auto &n : order)
        before += size(n, 0);
    std::map<std::string, int> drops;
    for (const auto &n : order)
        drops[n] = 0;
    uint64_t total = before;
    uint64_t budget = options.texture_cut ? std::max<int64_t>(static_cast<int64_t>(before) - static_cast<int64_t>(options.texture_cut), 1)
                                          : options.texture_budget;
    if (budget)
    {
        std::set<std::string> last;
        for (ZoneConverter *conv : convs)
            for (const auto &n : ui_images(*conv))
                last.insert(n);
        // a console copy that streamed is the PC game's texture: it keeps its size
        for (const auto &n : copies)
            if (steps_of(n))
                last.insert(n);
        DropPlan plan = choose_drops(order, size, can_drop, budget, last, tier);
        drops = plan.drops;
        total = plan.total;
        if (!plan.fits)
        {
            char buf[160];
            snprintf(buf, sizeof buf, "textures need %.1f MiB, over the %.1f MiB budget, and cannot be reduced further", total / 1048576.0,
                     budget / 1048576.0);
            convs[0]->warn(buf);
        }
    }
    std::unordered_set<std::string> eighths;
    std::unordered_map<std::string, int> levels;
    for (const auto &[n, k] : drops)
    {
        auto [drop, streamed] = state(n, k);
        if (streamed == 3)
            eighths.insert(lower_latin1(n));
        levels[n] = drop;
    }
    for (ZoneConverter *conv : convs)
    {
        conv->eighth_images = eighths;
        conv->image_drop_levels = levels;
        conv->textures_planned = true;
        conv->planned_texture_bytes = total;
    }
    if (!order.empty())
    {
        int reduced = 0;
        for (const auto &[n, d] : levels)
            reduced += d != 0;
        char buf[160];
        snprintf(buf, sizeof buf, "textures: %zu images, %.1f MiB -> %.1f MiB (%d reduced)", order.size(), before / 1048576.0, total / 1048576.0,
                 reduced);
        convs[0]->log(buf);
    }
}

// -- hooks

namespace
{
Zone &out_zone(ZoneConverter &conv)
{
    return conv.out();
}

Node *reference_or_copy(ZoneConverter &conv, const std::string &asset_type, Node *node, const std::string &name_in)
{
    std::string name = name_in;
    if (Node *copy = conv.from_library(asset_type, name, node))
    {
        conv.node_map[node] = copy;
        conv.offset_maps[node] = identity;
        return copy;
    }
    if (asset_type == "techset" && !starts_comma(name))
    {
        std::optional<std::string> substitute;
        // the console technique set to use for a PC one no console fastfile has (techsets.py): the
        // closest of the same world vertex format
        ConsoleLibrary *library = conv.console_library;
        if (library && !library->in_game_zones("MaterialTechniqueSet", name))
        {
            if (!conv.material_techsets)
            {
                std::set<std::string> used;
                const Record &mat = conv.src.record("Material");
                uint32_t offset = mat.field("techniqueSet")->offset;
                conv.zone.root->walk([&](Node *material) {
                    if (!material->is_record("Material") || material->string)
                        return;
                    for (uint32_t i = 0; i < material->count; ++i)
                    {
                        Ptr *ptr = material->relocs.get(i * mat.size + offset);
                        Node *target = ptr ? ptr->target() : nullptr;
                        if (target)
                            used.insert(lower_latin1(strip_commas(asset_display_name(conv.src, *target))));
                    }
                });
                conv.material_techsets = std::move(used);
            }
            if (conv.material_techsets->count(lower_latin1(strip_commas(name))))
            {
                if (!library->techset_candidates)
                {
                    std::map<std::string, TechsetInfo> candidates;
                    for (const std::string &found : library->names("MaterialTechniqueSet"))
                    {
                        auto entry = library->find("MaterialTechniqueSet", found);
                        if (entry && library->techset_safe(*entry->second))
                            candidates[found] = techset_info(conv.dst, *entry->second);
                    }
                    library->techset_candidates = std::move(candidates);
                }
                int wvf = node->data[conv.src.record("MaterialTechniqueSet").field("worldVertFormat")->offset];
                substitute = closest_techset(name, wvf, *library->techset_candidates);
                if (substitute)
                {
                    bool found = library->find("MaterialTechniqueSet", name).has_value();
                    std::string why = found ? "the console fastfiles given have it only with shaders CoD Xenon compiled, which froze the game drawing them"
                                            : "no console fastfile given has it (the game would draw with its default, which lacks most vertex shaders)";
                    conv.warn("technique set '" + strip_commas(name) + "': " + why + ": using '" + *substitute +
                              "', the closest of the same vertex format");
                }
            }
        }
        if (substitute)
        {
            if (Node *copy = conv.from_library(asset_type, *substitute, node))
            {
                conv.node_map[node] = copy;
                conv.offset_maps[node] = identity;
                return copy;
            }
            name = *substitute; // the game's own zones have it: a reference by that name
        }
        else
        {
            // nothing safe to use instead: the copy there is (it may draw: Kino's effects do)
            if (Node *copy = conv.from_library(asset_type, name, node, true))
            {
                conv.node_map[node] = copy;
                conv.offset_maps[node] = identity;
                return copy;
            }
        }
    }
    ++conv.stats.referenced[asset_type];
    Node *nw = build_reference(conv, asset_type, *node, starts_comma(name) ? name : "," + name);
    conv.node_map[node] = nw;
    conv.offset_maps[node] = identity;
    return nw;
}

Node *techset_hook(ZoneConverter &conv, const std::string &asset_type, Node *node, const std::string &name)
{
    // PC techniques carry PC shaders: the console technique set of the same name is copied from the
    // console library, or referenced by name. Zones unloaded while the game runs only reference them.
    if (conv.options.reference_techsets)
    {
        ++conv.stats.referenced[asset_type];
        Node *nw = build_reference(conv, asset_type, *node, "," + strip_commas(name));
        conv.node_map[node] = nw;
        conv.offset_maps[node] = identity;
        return nw;
    }
    return reference_or_copy(conv, asset_type, node, name);
}

// which techniques the console technique set of a PC material has (nothing when unknown)
std::optional<std::vector<bool>> console_techniques(ZoneConverter &conv, Node *material)
{
    uint32_t off = conv.src.record("Material").field("techniqueSet")->offset;
    Ptr *ptr = material->relocs.get(off);
    Node *src = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
    if (!src)
        return std::nullopt;
    auto mapped = conv.node_map.find(src);
    if (mapped == conv.node_map.end())
        return std::nullopt;
    Node *techset = mapped->second;
    if (!techset->library)
    {
        // a reference to a technique set of the game's zones: the library may have it
        if (!conv.console_library)
            return std::nullopt;
        auto found = conv.console_library->find("MaterialTechniqueSet", asset_display_name(conv.dst, *techset));
        if (!found)
            return std::nullopt;
        techset = found->second;
    }
    uint32_t f = conv.dst.record("MaterialTechniqueSet").field("techniques")->offset;
    std::vector<bool> present;
    for (int i = 0; i < X360_TECHNIQUE_COUNT; ++i)
    {
        Ptr *p = techset->relocs.get(f + 4 * i);
        present.push_back(p && p->kind != Ptr::Kind::Null);
    }
    return present;
}

Node *material_hook(ZoneConverter &conv, const std::string &, Node *node, const std::string &name)
{
    Node *nw = conv.convert_node(node);
    const Field &sf = field_of(conv.src, "Material", "stateBitsEntry");
    const Field &df = field_of(conv.dst, "Material", "stateBitsEntry");
    std::vector<uint8_t> src_entries(node->data.begin() + sf.offset, node->data.begin() + sf.offset + sf.type->count);
    std::vector<uint8_t> dst_entries(X360_TECHNIQUE_COUNT, 0xFF);
    for (auto [pc_index, x_index] : pc_technique_to_x360())
        if (pc_index < static_cast<int>(src_entries.size()))
            dst_entries[x_index] = src_entries[pc_index];

    // with the console technique set at hand, keep the state bits of its techniques only
    if (auto present = console_techniques(conv, node))
    {
        for (int i = 0; i < X360_TECHNIQUE_COUNT; ++i)
        {
            if (!(*present)[i])
                dst_entries[i] = 0xFF;
            else if (dst_entries[i] == 0xFF)
            {
                uint8_t fallback = 0;
                for (uint8_t e : dst_entries)
                    if (e != 0xFF)
                    {
                        fallback = e;
                        break;
                    }
                conv.warn("material '" + name + "': no PC state bits for console technique " + std::to_string(i) + ", using state bits " +
                          std::to_string(fallback));
                dst_entries[i] = fallback;
            }
        }
    }

    // drop the state bits only the removed techniques used, numbering the rest in first use order (as
    // the console linker does)
    Node *table = find_child(*nw, [](const Node &c) { return origin_member(c, "Material", "stateBitsTable"); });
    if (table && table->count)
    {
        uint32_t size = static_cast<uint32_t>(table->data.size() / table->count);
        std::vector<uint8_t> order;
        for (uint8_t e : dst_entries)
            if (e != 0xFF && e < table->count && std::find(order.begin(), order.end(), e) == order.end())
                order.push_back(e);
        auto reorder = [&]() {
            std::unordered_map<uint8_t, uint8_t> remap;
            for (size_t i = 0; i < order.size(); ++i)
                remap[order[i]] = static_cast<uint8_t>(i);
            for (uint8_t &e : dst_entries)
            {
                auto it = remap.find(e);
                e = it == remap.end() ? 0xFF : it->second;
            }
            std::vector<uint8_t> data;
            for (uint8_t i : order)
                data.insert(data.end(), table->data.begin() + i * size, table->data.begin() + (i + 1) * size);
            table->data.assign(std::move(data));
        };
        if (!order.empty() && order.size() < table->count)
        {
            reorder();
            table->count = static_cast<uint32_t>(order.size());
            table->segments = {{table->type, table->count, static_cast<uint32_t>(table->data.size()), false}};
            nw->data.mutable_data()[field_of(conv.dst, "Material", "stateBitsCount").offset] = static_cast<uint8_t>(table->count);
        }
        else if (!order.empty())
        {
            bool moved = false;
            for (size_t i = 0; i < order.size(); ++i)
                moved |= order[i] != i;
            if (moved)
                reorder();
        }
    }
    std::copy(dst_entries.begin(), dst_entries.end(), nw->data.mutable_data() + df.offset);
    return nw;
}

// what the image hook does with an image asset: nothing (another path), a reference (reason), or a
// texture of source with options
struct ImagePlan
{
    bool handled = false;
    std::string name;
    uint32_t map_type = 0, semantic = 0, category = 0;
    ImageRef source;
    std::string reason; // set: a reference
    TextureOptions options;
    std::string key; // of the prebuilt texture
};

ImagePlan plan_image(ZoneConverter &conv, Node *node, const std::string &name_in)
{
    ImagePlan plan;
    plan.name = name_in;
    if (starts_comma(plan.name))
    {
        // a stock image: the console library copy is used as is, unless the texture budget reduces it
        // (then it is rebuilt here from the console texture)
        if (!(drop_of(conv, plan.name.substr(1)) && console_image(conv, plan.name.substr(1))))
            return plan;
        plan.name = plan.name.substr(1);
    }
    plan.handled = true;
    plan.map_type = get_field(conv.src, "GfxImage", *node, "mapType");
    plan.semantic = get_field(conv.src, "GfxImage", *node, "semantic");
    plan.category = get_field(conv.src, "GfxImage", *node, "category");

    auto [kind, source] = image_choice(conv, *node, plan.name);
    if (kind == "library" && drop_of(conv, plan.name))
        source = console_image(conv, plan.name); // a console library texture reduced by the budget
    plan.source = source;

    int faces = plan.map_type == MAPTYPE_CUBE ? 6 : 1;
    if (!source || (plan.map_type != MAPTYPE_2D && plan.map_type != MAPTYPE_CUBE) || source->faces != faces)
    {
        if (!source)
            plan.reason = "no pixel data found (add the .iwd that contains it)";
        else if (plan.map_type != MAPTYPE_2D && plan.map_type != MAPTYPE_CUBE)
            plan.reason = "volume image";
        else
            plan.reason = std::string(faces == 6 ? "cube map without" : "2D image with") + " six faces";
        return plan;
    }
    plan.options = texture_options(conv.options, drop_of(conv, plan.name), pc_normal_map(*source, plan.semantic));
    plan.key = plan.name + '\0' + std::to_string(reinterpret_cast<uintptr_t>(source.get())) + '\0' + std::to_string(plan.options.drop_levels) +
               (plan.options.normal_map ? "n" : "");
    return plan;
}

Node *image_hook(ZoneConverter &conv, const std::string &asset_type, Node *node, const std::string &name_in)
{
    ImagePlan plan = plan_image(conv, node, name_in);
    if (!plan.handled)
        return nullptr;
    const std::string &name = plan.name;
    if (!plan.reason.empty())
    {
        const std::string &reason = plan.reason;
        if (conv.options.reference_missing_images)
        {
            Node *nw = reference_or_copy(conv, asset_type, node, name);
            if (!nw->library && !in_game_zones(conv, "GfxImage", name))
                conv.warn("image '" + name + "': " + reason + ", emitting a reference to the console image");
            return nw;
        }
        throw ImageError("image '" + name + "': " + reason);
    }

    ConsoleTexture built;
    const ConsoleTexture *tex = nullptr;
    auto prebuilt = conv.prebuilt_textures.find(plan.key);
    if (prebuilt != conv.prebuilt_textures.end())
    {
        if (!prebuilt->second.error.empty())
        {
            conv.warn(prebuilt->second.error + ", emitting a reference");
            return reference_or_copy(conv, asset_type, node, name);
        }
        tex = &*prebuilt->second.texture;
    }
    else
    {
        try
        {
            built = build_console_texture(*plan.source, plan.options);
        }
        catch (const ImageError &e)
        {
            conv.warn(std::string(e.what()) + ", emitting a reference");
            return reference_or_copy(conv, asset_type, node, name);
        }
        tex = &built;
    }
    conv.stats.texture_bytes += tex->pixels.size();
    Node *nw = build_console_image(conv, name, *tex, plan.semantic, plan.category, plan.source->flags, node);
    conv.node_map[node] = nw;
    conv.offset_maps[node] = identity;
    return nw;
}

Node *xanim_hook(ZoneConverter &conv, const std::string &, Node *node, const std::string &name)
{
    if (starts_comma(name))
        return nullptr;
    Node *nw = conv.convert_node(node);
    convert_xanim_parts(conv, *node, *nw, name);
    return nw;
}

Node *xmodel_hook(ZoneConverter &conv, const std::string &, Node *node, const std::string &name)
{
    if (starts_comma(name))
        return nullptr;
    if (!conv.unverified_records(node).empty() && !conv.options.allow_unverified)
        return nullptr; // the generic path: a reference
    Node *nw = conv.convert_node(node);
    const Platform &dst = conv.dst;
    uint32_t lod0 = field_of(dst, "XModel", "lodInfo").offset;
    uint32_t numsurfs = static_cast<uint32_t>(read_uint(dst, nw->data.data() + lod0 + field_of(dst, "XModelLodInfo", "numsurfs").offset, 2));
    if (numsurfs)
    {
        const Record &bounds_rec = dst.record("XModelHighMipBounds");
        Node *bounds = conv.new_node(bounds_rec.self, numsurfs, BLOCK_VIRTUAL);
        bounds->align = bounds_rec.align;
        bounds->origin = Origin::Member;
        bounds->origin_record = intern("XModelStreamInfo");
        bounds->origin_field = intern("highMipBounds");
        // each surface the model's box: the streamer loads the top level of its streamed textures when
        // the view is close to it (stream.model_bounds)
        float box[6];
        uint32_t mins = field_of(dst, "XModel", "mins").offset, maxs = field_of(dst, "XModel", "maxs").offset;
        for (int i = 0; i < 3; ++i)
        {
            uint32_t a = static_cast<uint32_t>(read_uint(dst, nw->data.data() + mins + 4 * i, 4));
            uint32_t b = static_cast<uint32_t>(read_uint(dst, nw->data.data() + maxs + 4 * i, 4));
            std::memcpy(&box[i], &a, 4);
            std::memcpy(&box[3 + i], &b, 4);
        }
        if (!(box[0] <= box[3] && box[1] <= box[4] && box[2] <= box[5]))
            std::memcpy(box, NO_STREAM_BOUNDS, sizeof box);
        std::vector<uint8_t> data(24 * numsurfs);
        for (uint32_t s = 0; s < numsurfs; ++s)
            for (int i = 0; i < 6; ++i)
            {
                uint32_t bits;
                std::memcpy(&bits, &box[i], 4);
                write_uint(dst, data.data() + 24 * s + 4 * i, 4, bits);
            }
        bounds->segments.push_back({bounds_rec.self, numsurfs, static_cast<uint32_t>(data.size()), false});
        bounds->data.assign(std::move(data));
        uint32_t offset = field_of(dst, "XModel", "streamInfo").offset + field_of(dst, "XModelStreamInfo", "highMipBounds").offset;
        size_t pos = nw->children.size();
        for (size_t i = 0; i < nw->children.size(); ++i)
        {
            const Node &c = *nw->children[i];
            bool after = false;
            if (c.origin == Origin::Member && *c.origin_record == "XModel")
                for (const char *m : XMODEL_MEMBERS_AFTER_STREAM_INFO)
                    after |= *c.origin_field == m;
            if (c.origin == Origin::Asset && (*c.origin_record == "PhysPreset" || *c.origin_record == "PhysConstraints"))
                after = true;
            if (after)
            {
                pos = i;
                break;
            }
        }
        Ptr *ptr = conv.new_ptr(Ptr::Kind::Follow);
        ptr->node = bounds;
        ptr->owner = nw;
        ptr->offset = offset;
        nw->relocs.set(offset, ptr);
        nw->children.insert(nw->children.begin() + pos, bounds);
    }
    return nw;
}

Node *gfxworld_hook(ZoneConverter &conv, const std::string &, Node *node, const std::string &name)
{
    if (starts_comma(name))
        return nullptr;
    if (!conv.unverified_records(node).empty() && !conv.options.allow_unverified)
        return nullptr;
    Node *nw = conv.convert_node(node);

    // light grid rows: a (colStart, colCount, zStart, zCount) u16 header and a u32 first entry, then a
    // byte lookup table; rows start at 4 * rowDataStart[row]
    std::vector<Node *> src_rows_all, starts_all;
    member_nodes(node, "GfxLightGrid", "rawRowData", [&](Node *n) { src_rows_all.push_back(n); });
    member_nodes(node, "GfxLightGrid", "rowDataStart", [&](Node *n) { starts_all.push_back(n); });
    member_nodes(nw, "GfxLightGrid", "rawRowData", [&](Node *grid_rows) {
        Node *src_rows = nullptr;
        for (Node *n : src_rows_all)
            if (n->data.size() == grid_rows->data.size())
            {
                src_rows = n;
                break;
            }
        Node *starts = starts_all.empty() ? nullptr : starts_all[0];
        if (!src_rows || !starts)
            return;
        if (starts->data.size() % 2)
            throw ConvertError("light grid row starts of an odd size");
        std::vector<uint8_t> data(src_rows->data.begin(), src_rows->data.end());
        std::set<uint32_t> row_starts;
        for (size_t i = 0; i + 1 < starts->data.size(); i += 2)
            row_starts.insert(starts->data[i] | starts->data[i + 1] << 8);
        for (uint32_t start : row_starts)
        {
            size_t o = 4 * size_t(start);
            if (o + 12 > data.size())
                continue;
            for (int k = 0; k < 4; ++k)
                std::swap(data[o + 2 * k], data[o + 2 * k + 1]);
            std::reverse(data.begin() + o + 8, data.begin() + o + 12);
        }
        grid_rows->data.assign(std::move(data));
    });

    // vertex layer data: per vertex of a layered surface, the texture coordinates of its other layers,
    // some with a packed 4 byte value (RGBA, ARGB on the console)
    std::vector<Node *> src_layers;
    member_nodes(node, "GfxWorldVertexLayerData", "data", [&](Node *n) { src_layers.push_back(n); });
    member_nodes(nw, "GfxWorldVertexLayerData", "data", [&](Node *layer) {
        if (layer->data.size() < 8)
            return; // a map without layered vertices has a 4 byte stub, kept as is
        Node *src_layer = nullptr;
        for (Node *n : src_layers)
            if (n->data.size() == layer->data.size())
            {
                src_layer = n;
                break;
            }
        if (!src_layer || layer->data.size() % 4)
        {
            conv.warn("gfxworld '" + name + "': unexpected vertex layer data size " + std::to_string(layer->data.size()));
            return;
        }
        layer->data.assign(console_vertex_layer_data(src_layer->data.data(), src_layer->data.size()));
    });
    return nw;
}

Node *loaded_sound_data(Node &node)
{
    Node *data = find_child(node, [](const Node &c) { return origin_member(c, "snd_asset", "data"); });
    return data && !data->data.empty() ? data : nullptr;
}

bool has_encoder(ZoneConverter &conv)
{
    return conv.options.xma_encoder && conv.options.xma_encoder->available();
}

Node *build_loaded_sound(ZoneConverter &conv, const std::string &name, const LoadedXma &xma)
{
    const Platform &dst = conv.dst;
    Zone &zone = out_zone(conv);
    const Record &rec = dst.record("LoadedSound"), &snd = dst.record("snd_asset");
    Node *node = asset_header(zone, dst, "LoadedSound");
    node->asset = "loaded_sound";
    uint32_t base = rec.field("sound")->offset;
    uint8_t *d = node->data.mutable_data();
    write_uint(dst, d + base + snd.field("data_size")->offset, 4, xma.data.size());
    for (size_t i = 0; i < xma.format.size(); ++i)
        write_uint(dst, d + base + snd.field("format")->offset + 4 * i, 4, xma.format[i]);
    follow(zone, node, rec.field("name")->offset, string_node(zone, dst, name));

    Node *data = conv.new_node(dst.char_type, static_cast<uint32_t>(xma.data.size()), BLOCK_LARGE);
    data->align = 2048;
    data->origin = Origin::Member;
    data->origin_record = intern("snd_asset");
    data->origin_field = intern("data");
    data->segments.push_back({dst.char_type, data->count, data->count, false});
    data->data.assign(xma.data);
    follow(zone, node, base + snd.field("data")->offset, data);

    const Record &seek_rec = dst.record("XmaSeekTable360");
    Node *seek = conv.new_node(seek_rec.self, 1, BLOCK_VIRTUAL);
    std::vector<uint8_t> table(8 + 4 * xma.seek_table.size());
    write_uint(dst, table.data(), 4, 1);
    write_uint(dst, table.data() + 4, 4, xma.seek_table.size());
    for (size_t i = 0; i < xma.seek_table.size(); ++i)
        write_uint(dst, table.data() + 8 + 4 * i, 4, xma.seek_table[i]);
    seek->align = 4;
    seek->origin = Origin::Member;
    seek->origin_record = intern("snd_asset");
    seek->origin_field = intern("seekTable");
    seek->segments.push_back({seek_rec.self, 1, 8, true});
    seek->segments.push_back({dst.uint_type, static_cast<uint32_t>(xma.seek_table.size()), static_cast<uint32_t>(4 * xma.seek_table.size()), false});
    seek->data.assign(std::move(table));
    follow(zone, node, base + snd.field("seekTable")->offset, seek);
    return node;
}

Node *loaded_sound_hook(ZoneConverter &conv, const std::string &asset_type, Node *node, const std::string &name)
{
    std::string console_name = console_sound_name(name);
    if (starts_comma(name))
        return reference_or_copy(conv, asset_type, node, console_name);
    if (!has_encoder(conv))
    {
        if (!conv.warned_no_encoder)
        {
            conv.warned_no_encoder = true;
            conv.warn("loaded sounds need xma2encode.exe (Xbox 360 XDK, --xma-encoder): they are emitted as references to console sounds");
        }
        return reference_or_copy(conv, asset_type, node, console_name);
    }
    Node *data = loaded_sound_data(*node);
    if (!data)
        return reference_or_copy(conv, asset_type, node, console_name);
    SoundCache &cache = *conv.options.sound_cache;
    auto key = std::make_pair(console_name, data->data.size());
    SoundCache::Entry entry;
    bool cached;
    {
        std::lock_guard guard(cache.lock);
        auto it = cache.entries.find(key);
        cached = it != cache.entries.end();
        if (cached)
            entry = it->second;
    }
    if (!cached)
    {
        try
        {
            entry.xma = std::make_shared<LoadedXma>(encode_loaded_sound(std::vector<uint8_t>(data->data.begin(), data->data.end()),
                                                                        *conv.options.xma_encoder, static_cast<int>(conv.options.sound_rate),
                                                                        conv.options.mono_sounds));
        }
        catch (const AudioError &e)
        {
            entry.error = e.what();
        }
        std::lock_guard guard(cache.lock);
        cache.entries[key] = entry;
    }
    if (!entry.xma)
    {
        conv.warn("loaded sound '" + name + "': " + entry.error + ", emitting a reference");
        return reference_or_copy(conv, asset_type, node, console_name);
    }
    conv.stats.sound_bytes += entry.xma->data.size();
    return build_loaded_sound(conv, console_name, *entry.xma);
}

void fix_primed_sound(ZoneConverter &conv, Node *sf, uint32_t prime_off, const std::optional<std::string> &rel)
{
    // console primed buffers hold the start of the SDNS stream file (32 KB)
    Ptr *ptr = sf->relocs.get(prime_off);
    if (!ptr || (ptr->kind != Ptr::Kind::Follow && ptr->kind != Ptr::Kind::Insert))
        return;
    auto mapped = conv.node_map.find(ptr->node);
    Node *primed = mapped == conv.node_map.end() ? ptr->node : mapped->second;
    std::optional<std::vector<uint8_t>> sdns;
    if (rel && !conv.options.sounds_dir.empty())
    {
        fs::path path = conv.options.sounds_dir / "sounds";
        std::string root = splitext_root(*rel);
        size_t start = 0;
        while (true)
        {
            size_t end = root.find('\\', start);
            std::string part = root.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (end == std::string::npos)
            {
                path /= part + ".xma";
                break;
            }
            path /= part;
            start = end + 1;
        }
        std::ifstream f(path, std::ios::binary);
        if (f)
        {
            std::vector<uint8_t> buf(PRIMED_SOUND_SIZE);
            f.read(reinterpret_cast<char *>(buf.data()), buf.size());
            buf.resize(static_cast<size_t>(f.gcount()));
            sdns = std::move(buf);
        }
    }
    Node *buffer = find_child(*primed, [](const Node &c) { return origin_member(c, "PrimedSound", "buffer"); });
    if (!sdns || !buffer)
    {
        // no console stream to prime from: play without priming
        ptr->kind = Ptr::Kind::Null;
        ptr->node = nullptr;
        auto &c = sf->children;
        auto it = std::find(c.begin(), c.end(), primed);
        if (it != c.end())
            c.erase(it);
        return;
    }
    buffer->count = static_cast<uint32_t>(sdns->size());
    buffer->segments = {{buffer->type, buffer->count, buffer->count, false}};
    buffer->data.assign(std::move(*sdns));
    put_field(conv.dst, "PrimedSound", *primed, "size", buffer->count);
}

Node *sound_hook(ZoneConverter &conv, const std::string &, Node *node, const std::string &name)
{
    // sound alias lists: streamed file names and primed buffers follow the console conventions
    if (starts_comma(name))
        return nullptr;
    if (!conv.unverified_records(node).empty() && !conv.options.allow_unverified)
        return nullptr;
    Node *nw = conv.convert_node(node);
    const Platform &dst = conv.dst;
    uint32_t u = field_of(dst, "SoundFile", "u").offset;
    uint32_t fn = u + field_of(dst, "StreamedSound", "filename").offset;
    uint32_t hash_off = fn + field_of(dst, "StreamFileName", "hash").offset;
    uint32_t dir_off = fn + field_of(dst, "StreamFileName", "dir").offset;
    uint32_t name_off = fn + field_of(dst, "StreamFileName", "name").offset;
    uint32_t prime_off = u + field_of(dst, "StreamedSound", "primeSnd").offset;

    auto string_of = [&](Node *sf, uint32_t off) -> std::pair<Node *, std::optional<std::string>> {
        Ptr *ptr = sf->relocs.get(off);
        Node *target = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
        if (!target)
            return {nullptr, std::nullopt};
        auto mapped = conv.node_map.find(target);
        Node *converted = mapped == conv.node_map.end() ? target : mapped->second;
        return {converted, text_of(*converted)};
    };

    std::vector<Node *> files;
    nw->walk([&](Node *sf) { files.push_back(sf); });
    for (Node *sf : files)
    {
        if (!origin_member(*sf, "snd_alias_t", "soundFile") || sf->data.empty() || sf->data[0] != 2 || conv.sound_files_done.count(sf))
            continue;
        conv.sound_files_done.insert(sf);
        auto [dir_node, directory_opt] = string_of(sf, dir_off);
        auto [name_node, file_name] = string_of(sf, name_off);
        if (!file_name)
            continue;
        std::string directory = directory_opt.value_or("");
        std::replace(directory.begin(), directory.end(), '/', '\\');
        std::string stem = console_sound_name(*file_name);
        std::string rel = (directory.empty() ? "" : directory + "\\") + *file_name;
        // a sound of the map: its console stream was written next to the fastfile
        std::string slashed = rel;
        std::replace(slashed.begin(), slashed.end(), '\\', '/');
        std::string target = streamed_sound_target("sound/" + slashed);
        bool custom = false;
        if (!conv.options.sounds_dir.empty())
        {
            fs::path path = conv.options.sounds_dir;
            size_t start = 0;
            while (true)
            {
                size_t end = target.find('/', start);
                path /= target.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            custom = fs::exists(path);
        }
        std::string new_dir;
        uint32_t value;
        if (custom)
        {
            new_dir = directory.empty() ? "sounds" : "sounds\\" + directory;
            value = map_stream_hash(new_dir, stem);
        }
        else
        {
            new_dir = lower_latin1(directory);
            stem = lower_latin1(stem);
            value = stream_name_hash((new_dir.empty() ? "" : new_dir + "\\") + stem);
        }
        write_uint(dst, sf->data.mutable_data() + hash_off, 4, value);
        // the PC linker stores equal strings once: the new names are separate strings, set once the
        // zone is converted
        if (directory != new_dir || !dir_node)
            conv.string_edits.emplace_back(sf, dir_off, new_dir);
        if (*file_name != stem)
            conv.string_edits.emplace_back(sf, name_off, stem);
        fix_primed_sound(conv, sf, prime_off, custom ? std::optional<std::string>(rel) : std::nullopt);
    }
    return nw;
}
} // namespace

void register_hooks(ZoneConverter &conv)
{
    conv.hooks["techset"] = techset_hook;
    conv.hooks["material"] = material_hook;
    conv.hooks["image"] = image_hook;
    conv.hooks["xanim"] = xanim_hook;
    conv.hooks["xmodel"] = xmodel_hook;
    conv.hooks["gfxworld"] = gfxworld_hook;
    conv.hooks["loaded_sound"] = loaded_sound_hook;
    conv.hooks["sound"] = sound_hook;
}

void prebuild_textures(const std::vector<ZoneConverter *> &convs)
{
    struct Job
    {
        ZoneConverter *conv;
        std::string key;
        ImageRef source;
        TextureOptions options;
    };
    std::vector<Job> jobs;
    for (ZoneConverter *conv : convs)
    {
        std::unordered_set<std::string> keys;
        conv->zone.root->walk([&](Node *node) {
            if (!node->is_record("GfxImage") || node->origin != Origin::Asset)
                return;
            std::string name = asset_display_name(conv->src, *node);
            ImagePlan plan = plan_image(*conv, node, name);
            if (plan.handled && plan.reason.empty() && keys.insert(plan.key).second)
                jobs.push_back({conv, plan.key, plan.source, plan.options});
        });
    }
    std::vector<ZoneConverter::PrebuiltTexture> results(jobs.size());
    parallel_for(jobs.size(), convs.empty() ? 0 : convs[0]->options.jobs, [&](size_t i) {
        try
        {
            results[i].texture = build_console_texture(*jobs[i].source, jobs[i].options);
        }
        catch (const ImageError &e)
        {
            results[i].error = e.what();
        }
    });
    for (size_t i = 0; i < jobs.size(); ++i)
        jobs[i].conv->prebuilt_textures[jobs[i].key] = std::move(results[i]);
}

void encode_loaded_sounds(ZoneConverter &conv)
{
    // each one is a decode and an xma2encode run, which run on every processor; the results wait in
    // the sound cache for the hook
    if (!has_encoder(conv))
        return;
    SoundCache &cache = *conv.options.sound_cache;
    std::vector<std::pair<std::pair<std::string, size_t>, Node *>> jobs;
    std::set<std::pair<std::string, size_t>> queued;
    conv.zone.root->walk([&](Node *node) {
        if (node->origin != Origin::Asset || !node->is_record("LoadedSound"))
            return;
        std::string name = asset_display_name(conv.src, *node);
        Node *data = loaded_sound_data(*node);
        if (name.empty() || starts_comma(name) || !data)
            return;
        auto key = std::make_pair(console_sound_name(name), data->data.size());
        std::lock_guard guard(cache.lock);
        if (!cache.entries.count(key) && queued.insert(key).second)
            jobs.emplace_back(key, data);
    });
    std::vector<SoundCache::Entry> results(jobs.size());
    std::string label = "Encoding loaded sounds";
    if (conv.progress_label.rfind("Converting ", 0) == 0)
        label += " of " + conv.progress_label.substr(11);
    const int total = static_cast<int>(jobs.size());
    progress::step(label, 0, total);
    std::atomic<int> done{0};
    parallel_for(jobs.size(), conv.options.jobs, [&](size_t i) {
        try
        {
            const Bytes &wav = jobs[i].second->data;
            results[i].xma = std::make_shared<LoadedXma>(encode_loaded_sound(std::vector<uint8_t>(wav.begin(), wav.end()), *conv.options.xma_encoder,
                                                                             static_cast<int>(conv.options.sound_rate), conv.options.mono_sounds));
        }
        catch (const AudioError &e)
        {
            results[i].error = e.what();
        }
        progress::step(label, ++done, total);
    });
    std::lock_guard guard(cache.lock);
    for (size_t i = 0; i < jobs.size(); ++i)
        cache.entries[jobs[i].first] = std::move(results[i]);
}

// -- strings

void apply_string_edits(ZoneConverter &conv, Node *root)
{
    auto edits = std::move(conv.string_edits);
    conv.string_edits.clear();
    if (edits.empty())
        return;
    std::unordered_map<const Node *, size_t> order;
    std::unordered_map<const Ptr *, std::pair<Node *, uint32_t>> where;
    std::unordered_map<const Node *, std::vector<Ptr *>> users;
    std::unordered_map<const Ptr *, std::vector<Ptr *>> slot_aliases;
    size_t position = 0;
    root->walk([&](Node *n) {
        order[n] = position++;
        for (const auto &[offset, ptr] : n->relocs.list())
        {
            where[ptr] = {n, offset};
            if (ptr->kind == Ptr::Kind::Ref && ptr->node)
                users[ptr->node].push_back(ptr);
            else if (ptr->kind == Ptr::Kind::Alias && ptr->slot)
                slot_aliases[ptr->slot].push_back(ptr);
        }
    });
    // children of a node are the nodes its following pointers load, in pointer order
    auto rebuild_children = [](Node *node) {
        node->children.clear();
        for (const auto &[off, p] : node->relocs.list())
            if ((p->kind == Ptr::Kind::Follow || p->kind == Ptr::Kind::Insert) && p->node)
                node->children.push_back(p->node);
    };
    Zone &zone = out_zone(conv);
    auto set_string = [&](Node *owner, uint32_t offset, const std::string &text) {
        Ptr *old = owner->relocs.get(offset);
        Node *nw = string_node(zone, conv.dst, text);
        Ptr *ptr = zone.new_ptr(Ptr::Kind::Follow);
        ptr->node = nw;
        ptr->owner = owner;
        ptr->offset = offset;
        owner->relocs.set(offset, ptr);
        where[ptr] = {owner, offset};
        rebuild_children(owner);
        if (!old || old->kind != Ptr::Kind::Follow || !old->node)
            return;
        Node *shared = old->node;
        // pointers that relied on the old one: aliases of its slot and references into its string
        auto aliases = slot_aliases.find(old);
        if (aliases != slot_aliases.end())
        {
            for (Ptr *alias : aliases->second)
            {
                alias->kind = Ptr::Kind::Ref;
                alias->node = shared;
                alias->slot = nullptr;
                alias->index = 0;
                alias->inner = 0;
                users[shared].push_back(alias);
            }
            slot_aliases.erase(aliases);
        }
        std::vector<Ptr *> refs;
        for (Ptr *r : users[shared])
            if (r->kind == Ptr::Kind::Ref && r->node == shared && where.count(r))
                refs.push_back(r);
        std::stable_sort(refs.begin(), refs.end(), [&](Ptr *a, Ptr *b) {
            auto key = [&](Ptr *r) {
                auto o = order.find(where[r].first);
                return std::make_pair(o == order.end() ? size_t(0) : o->second, where[r].second);
            };
            return key(a) < key(b);
        });
        for (Ptr *r : refs)
        {
            Node *holder = where[r].first;
            if (r->index == 0 && r->inner == 0)
            {
                // the first one loads the string now; the others come after it
                r->kind = Ptr::Kind::Follow;
                rebuild_children(holder);
                return;
            }
            // a reference into the middle of the string, before any other: its own copy
            size_t start = r->index * shared->elem_size() + r->inner;
            std::string text_from = start < shared->data.size() ? std::string(shared->data.begin() + start, shared->data.end()) : "";
            while (!text_from.empty() && text_from.back() == '\0')
                text_from.pop_back();
            r->kind = Ptr::Kind::Follow;
            r->node = string_node(zone, conv.dst, text_from);
            r->index = 0;
            r->inner = 0;
            rebuild_children(holder);
        }
    };
    for (auto &[owner, offset, text] : edits)
        set_string(owner, offset, text);
}

// -- sounds

std::string console_sound_name(const std::string &name)
{
    std::string prefix = starts_comma(name) ? "," : "";
    std::string base = strip_commas(name);
    std::string ext = lower_latin1(splitext_ext(base));
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac" || ext == ".xma")
        return prefix + splitext_root(base);
    return prefix + base;
}

std::vector<std::string> streamed_sound_files(const Platform &p, const std::vector<Zone *> &zones)
{
    uint32_t u = p.record("SoundFile").field("u")->offset;
    uint32_t fn = u + p.record("StreamedSound").field("filename")->offset;
    const Record &sfn = p.record("StreamFileName");
    uint32_t dir_off = fn + sfn.field("dir")->offset, name_off = fn + sfn.field("name")->offset;
    auto text = [](Node *node, uint32_t off) -> std::string {
        Ptr *ptr = node->relocs.get(off);
        Node *target = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
        return target ? text_of(*target) : "";
    };
    std::map<std::string, std::string> files; // lower case -> as the first alias has it
    for (Zone *zone : zones)
        zone->root->walk([&](Node *node) {
            if (!origin_member(*node, "snd_alias_t", "soundFile") || node->data.empty() || node->data[0] != 2)
                return;
            std::string name = text(node, name_off);
            if (name.empty())
                return;
            std::string directory = text(node, dir_off);
            std::replace(directory.begin(), directory.end(), '/', '\\');
            std::string rel = (directory.empty() ? "" : directory + "\\") + name;
            files.emplace(lower_latin1(rel), rel);
        });
    std::vector<std::string> out;
    for (const auto &[lower, rel] : files)
        out.push_back(rel);
    return out; // sorted by their lower case names
}

StreamStats ship_stock_streams(const Platform &p, const std::vector<Zone *> &zones, const IwdLibrary *stock, const std::filesystem::path &out_dir,
                               XmaEncoder &encoder, int max_rate, bool mono, int jobs, const std::function<void(const std::string &)> &log)
{
    std::vector<std::string> wanted, missing;
    for (const std::string &rel : streamed_sound_files(p, zones))
    {
        std::string source = "sound/" + rel;
        std::replace(source.begin(), source.end(), '\\', '/');
        std::string target = streamed_sound_target_path(source);
        std::filesystem::path path = out_dir;
        for (size_t start = 0;;)
        {
            size_t end = target.find('/', start);
            path /= target.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        if (std::filesystem::exists(path))
            continue;
        (stock && stock->contains(source) ? wanted : missing).push_back(source);
    }
    StreamStats stats;
    if (!wanted.empty())
    {
        stats = convert_streamed_sounds(*stock, out_dir, encoder, max_rate, mono, log, jobs, &wanted, true);
        if (log)
        {
            char buf[256];
            snprintf(buf, sizeof buf, "streamed sounds: %d of the game's own the map uses taken from the PC game's files (%.1f MiB, e.g. %s)",
                     stats.converted, stats.output_bytes / 1048576.0, wanted[0].substr(6).c_str());
            log(buf);
        }
    }
    if (!missing.empty() && log)
    {
        std::string where = stock ? "nor in the PC game's files given" : "(give the PC game's main folder with --iwd to add them)";
        log("streamed sounds: " + std::to_string(missing.size()) + " the map uses are not in its files " + where +
            ": the console plays them only if its disc has them, which it has not for the downloadable maps' (e.g. " + missing[0].substr(6) + ")");
    }
    return stats;
}

uint32_t stream_name_hash(const std::string &path)
{
    uint32_t h = 5381;
    for (char c : lower_latin1(path))
        h = h * 0x1003F + static_cast<uint8_t>(c);
    return h;
}

uint32_t map_stream_hash(const std::string &directory, const std::string &name)
{
    uint32_t h = stream_name_hash((directory.empty() ? "" : directory + "\\") + name);
    return h ? h : 1;
}

std::string streamed_sound_target(const std::string &rel)
{
    std::string s = rel;
    std::replace(s.begin(), s.end(), '\\', '/');
    if (lower_latin1(s.substr(0, s.find('/'))) == "sound")
        s = s.find('/') == std::string::npos ? "" : s.substr(s.find('/') + 1);
    return "sounds/" + splitext_root(s) + ".xma";
}

std::vector<uint8_t> console_vertex_layer_data(const uint8_t *data, size_t size)
{
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i + 3 < size; i += 4)
    {
        uint32_t bits = uint32_t(data[i]) | uint32_t(data[i + 1]) << 8 | uint32_t(data[i + 2]) << 16 | uint32_t(data[i + 3]) << 24;
        float f;
        std::memcpy(&f, &bits, 4);
        float v = std::fabs(f);
        // texture coordinates are floats (byte swapped); packed values are not (their alpha byte makes
        // them NaN, huge or tiny) and go from RGBA to ARGB
        bool is_float = std::isfinite(v) && (v == 0 || (v > 1e-8f && v < 1e6f));
        if (is_float)
            for (int k = 0; k < 4; ++k)
                out[i + k] = data[i + 3 - k];
        else
        {
            out[i] = data[i + 3];
            out[i + 1] = data[i];
            out[i + 2] = data[i + 1];
            out[i + 3] = data[i + 2];
        }
    }
    return out;
}
} // namespace t4ff
