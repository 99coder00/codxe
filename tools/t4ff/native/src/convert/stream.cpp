#include "convert/stream.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>

#include "core/fastfile.h"
#include "core/pystr.h"
#include "core/xenos.h"

namespace t4ff
{
namespace fs = std::filesystem;

namespace
{
constexpr const char *HIGHMIP_DIRECTORY = "highmip"; // where earlier conversions put their .hi files
constexpr char PAK_MAGIC[8] = {'T', '4', 'F', 'F', 'P', 'A', 'K', '1'};
constexpr uint32_t PAK_VERSION = 2;
constexpr uint32_t PAK_VERSION_EIGHTH = 3;
constexpr uint32_t PAK_ALIGN = 4096;
constexpr uint32_t PAK_DEEP = 1;
constexpr uint32_t PAK_EIGHTH = 2;
constexpr uint32_t MAX_STREAM_BLOCK = 4 * 1024 * 1024;
constexpr uint32_t MIN_HIGHMIP_BYTES = 128 * 1024;
constexpr uint32_t MAPTYPE_2D = 3;
constexpr int SEMANTIC_WATER = 11;
constexpr double STREAM_DISTANCE = 1931.2;
constexpr double MAX_STREAM_GROWTH = 4096.0;
constexpr double DEFAULT_STREAM_GROWTH = STREAM_DISTANCE / 4;
constexpr std::array<double, 6> NO_STREAM_BOUNDS = {131072.0, 131072.0, 131072.0, -131072.0, -131072.0, -131072.0};
constexpr size_t LEAF_REFS = 32;
constexpr size_t USHORT_MAX = 0xFFFF;

using Box = std::array<double, 6>;

void put_be(std::vector<uint8_t> &out, uint64_t v, int bytes)
{
    for (int s = 8 * (bytes - 1); s >= 0; s -= 8)
        out.push_back(uint8_t(v >> s));
}

void put_bef(std::vector<uint8_t> &out, double v)
{
    float f = static_cast<float>(v);
    uint32_t u;
    std::memcpy(&u, &f, 4);
    put_be(out, u, 4);
}

// A map's images.pak, written in one pass (big endian, as the console reads it; see stream.py)
class PakWriter
{
  public:
    explicit PakWriter(const fs::path &path) : path(path), temp(path.wstring() + L".part"), file(temp, std::ios::binary)
    {
        if (!file)
            throw std::runtime_error("cannot write " + temp.string());
        std::vector<char> zeros(PAK_ALIGN);
        file.write(zeros.data(), zeros.size());
        pos = PAK_ALIGN;
    }

    void add(const std::string &name, const std::vector<uint8_t> &data, uint32_t flags = 0, uint32_t mip_offset = 0, uint32_t level2_offset = 0)
    {
        uint64_t offset = pos;
        file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        size_t pad = (PAK_ALIGN - data.size() % PAK_ALIGN) % PAK_ALIGN;
        std::vector<char> zeros(pad);
        file.write(zeros.data(), static_cast<std::streamsize>(pad));
        pos += data.size() + pad;
        entries[py::lower(name)] = Entry{offset, data.size(), flags, mip_offset, level2_offset};
    }

    // finishes the pack (or removes it when it is empty); returns its entry count
    size_t close()
    {
        std::vector<uint8_t> index, blob;
        bool eighth = false;
        for (const auto &[name, e] : entries)
        {
            put_be(index, entries.size() * 24 + blob.size(), 4);
            put_be(index, name.size(), 2);
            put_be(index, e.flags, 2);
            put_be(index, e.offset, 4);
            put_be(index, e.size, 4);
            put_be(index, e.mip_offset, 4);
            put_be(index, e.level2_offset, 4);
            blob.insert(blob.end(), name.begin(), name.end());
            eighth = eighth || (e.flags & PAK_EIGHTH);
        }
        uint64_t index_offset = pos;
        file.write(reinterpret_cast<const char *>(index.data()), static_cast<std::streamsize>(index.size()));
        file.write(reinterpret_cast<const char *>(blob.data()), static_cast<std::streamsize>(blob.size()));
        std::vector<uint8_t> header(PAK_MAGIC, PAK_MAGIC + 8);
        put_be(header, eighth ? PAK_VERSION_EIGHTH : PAK_VERSION, 4);
        put_be(header, entries.size(), 4);
        put_be(header, index_offset, 4);
        put_be(header, index.size() + blob.size(), 4);
        file.seekp(0);
        file.write(reinterpret_cast<const char *>(header.data()), static_cast<std::streamsize>(header.size()));
        file.close();
        std::error_code ec;
        if (fs::exists(path))
            fs::remove(path);
        if (!entries.empty())
            fs::rename(temp, path);
        else
            fs::remove(temp, ec);
        return entries.size();
    }

  private:
    struct Entry
    {
        uint64_t offset, size;
        uint32_t flags, mip_offset, level2_offset;
    };
    fs::path path, temp;
    std::ofstream file;
    uint64_t pos = 0;
    std::map<std::string, Entry> entries; // by lower case name, as the index is sorted
};

uint32_t field_off(const Platform &p, const std::string &rec, const std::string &path)
{
    // a dotted path of fields ("streamInfo.highMipBounds")
    uint32_t offset = 0;
    std::string record = rec;
    for (const std::string &part : py::split(path, "."))
    {
        const Field *f = p.record(record).field(part);
        if (!f)
            throw std::runtime_error("no field " + record + "." + part);
        offset += f->offset;
        record = f->type->name;
    }
    return offset;
}

Node *target_of(Node *node, uint32_t offset)
{
    Ptr *ptr = node ? node->relocs.get(offset) : nullptr;
    return ptr ? ptr->target() : nullptr;
}

uint32_t be16(const uint8_t *d)
{
    return uint32_t(d[0]) << 8 | d[1];
}
uint32_t be32(const uint8_t *d)
{
    return uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3];
}
float bef32(const uint8_t *d)
{
    uint32_t u = be32(d);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
void set_be(Node *node, uint32_t off, uint64_t v, int bytes)
{
    uint8_t *d = node->data.mutable_data() + off;
    for (int i = 0; i < bytes; ++i)
        d[i] = uint8_t(v >> (8 * (bytes - 1 - i)));
}
void set_bef(Node *node, uint32_t off, double v)
{
    float f = static_cast<float>(v);
    uint32_t u;
    std::memcpy(&u, &f, 4);
    set_be(node, off, u, 4);
}

// an IEEE half (numpy's float16) as a double
double half_value(uint16_t h)
{
    int sign = h >> 15, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    double v;
    if (exp == 0)
        v = std::ldexp(static_cast<double>(mant), -24);
    else if (exp == 31)
        v = mant ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    else
        v = std::ldexp(static_cast<double>(mant | 0x400), exp - 25);
    return sign ? -v : v;
}

bool is_asset(const Node *n, std::string_view rec)
{
    return n->origin == Origin::Asset && n->is_record(rec);
}

std::string type_name(const Node *n)
{
    return n->type ? n->type->name : "";
}

// id(node) -> the innermost asset node holding it
std::unordered_map<const Node *, Node *> innermost_assets(Zone &zone)
{
    std::unordered_map<const Node *, Node *> owner;
    std::vector<std::pair<Node *, Node *>> stack{{zone.root, nullptr}};
    while (!stack.empty())
    {
        auto [node, asset] = stack.back();
        stack.pop_back();
        if (node->origin == Origin::Asset)
            asset = node;
        if (asset)
            owner.emplace(node, asset);
        for (Node *child : node->children)
            stack.emplace_back(child, asset);
    }
    return owner;
}

// The images of the zone only materials use, whose every material only models and world surfaces use.
std::vector<Node *> streamable_images(Zone &zone)
{
    auto owner = innermost_assets(zone);
    std::vector<Node *> order; // the targets in the order they were first seen
    std::unordered_map<const Node *, std::set<std::string>> users;
    std::unordered_map<const Node *, std::set<const Node *>> materials_of;
    zone.walk([&](Node *node) {
        for (const auto &[off, ptr] : node->relocs.list())
        {
            Node *target = ptr->target();
            if (!target || (type_name(target) != "Material" && type_name(target) != "GfxImage"))
                continue;
            auto h = owner.find(node);
            Node *holder = h == owner.end() ? nullptr : h->second;
            if (!holder || holder == target) // the zone's asset list, or the asset itself
                continue;
            std::string kind = type_name(holder);
            if (kind == "GfxWorld") // which of the world's arrays
                kind += "." + type_name(node);
            auto [it, added] = users.try_emplace(target);
            if (added)
                order.push_back(target);
            it->second.insert(kind);
            if (type_name(target) == "GfxImage" && kind == "Material")
                materials_of[target].insert(holder);
        }
    });
    auto drawn_kind = [](const std::string &k) { return k == "XModel" || k == "GfxWorld.GfxSurface"; };
    std::unordered_set<const Node *> drawn;
    for (Node *n : order)
    {
        if (type_name(n) != "Material")
            continue;
        const auto &kinds = users[n];
        bool subset = true, any = false;
        for (const std::string &k : kinds)
        {
            subset = subset && (drawn_kind(k) || k == "GfxWorld.MaterialMemory");
            any = any || drawn_kind(k);
        }
        if (subset && any)
            drawn.insert(n);
    }
    std::vector<Node *> out;
    for (Node *n : order)
    {
        if (type_name(n) != "GfxImage")
            continue;
        const auto &kinds = users[n];
        if (kinds.size() != 1 || *kinds.begin() != "Material")
            continue;
        auto m = materials_of.find(n);
        if (m == materials_of.end() || m->second.empty())
            continue;
        bool all = true;
        for (const Node *mat : m->second)
            all = all && drawn.count(mat);
        if (all)
            out.push_back(n);
    }
    return out;
}

struct ImageParts
{
    Node *load_def, *header, *pixels;
    const xenos::Format *fmt;
    uint32_t width, height;
    int levels;
};

std::optional<ImageParts> image_parts(const Platform &p, Node *image)
{
    Node *load_def = nullptr, *header = nullptr, *pixels = nullptr;
    image->walk([&](Node *c) {
        if (!load_def && type_name(c) == "GfxImageLoadDef")
            load_def = c;
        if (!header && type_name(c) == "D3DBaseTexture360")
            header = c;
    });
    for (Node *c : image->children)
        if (c->delayed)
        {
            pixels = c;
            break;
        }
    if (!load_def || !header || !pixels)
        return std::nullopt;
    int levels = load_def->data[field_off(p, "GfxImageLoadDef", "levelCount")];
    const xenos::Format *fmt = xenos::format_of_d3d(be32(load_def->data.data() + field_off(p, "GfxImageLoadDef", "format")));
    const uint8_t *d = image->data.data();
    if (be32(d + field_off(p, "GfxImage", "mapType")) != MAPTYPE_2D || !fmt)
        return std::nullopt;
    return ImageParts{load_def, header, pixels, fmt, be16(d + field_off(p, "GfxImage", "width")), be16(d + field_off(p, "GfxImage", "height")),
                      levels};
}

struct Split
{
    std::vector<uint8_t> top, half, header;
};

std::vector<uint8_t> slice(const std::vector<uint8_t> &v, size_t a, size_t b)
{
    a = std::min(a, v.size());
    b = std::min(std::max(a, b), v.size());
    return std::vector<uint8_t>(v.begin() + static_cast<std::ptrdiff_t>(a), v.begin() + static_cast<std::ptrdiff_t>(b));
}

// (.hi data, pixels of the half size texture, its GPU header) of a tiled 2D texture, or nothing when it
// cannot stream
std::optional<Split> split(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, const xenos::Format &fmt, int levels,
                           uint32_t min_bytes = MIN_HIGHMIP_BYTES)
{
    if (levels < 2 || (width & (width - 1)) || (height & (height - 1)) || std::min(width, height) < 64)
        return std::nullopt;
    xenos::MipChainLayout layout = xenos::mip_chain_layout(width, height, fmt, levels);
    uint32_t base_size = layout.base_size, total = layout.total;
    if (base_size < min_bytes || total > pixels.size())
        return std::nullopt;
    xenos::LevelLayout half_layout = xenos::level_layout(width / 2, height / 2, 0, fmt);
    uint32_t full_pitch = xenos::level_layout(width, height, 0, fmt).stored_w;
    if (2 * half_layout.stored_w != full_pitch || 4ull * half_layout.size < base_size)
        return std::nullopt;
    auto chain = xenos::untile_mip_chain(pixels.data(), pixels.size(), width, height, fmt, levels);
    std::vector<std::vector<uint8_t>> rest(chain.begin() + 1, chain.end());
    std::vector<uint8_t> half = xenos::tile_mip_chain(rest, width / 2, height / 2, fmt);
    if (half != slice(pixels, base_size, total))
        return std::nullopt;
    Split s;
    s.top = slice(pixels, 0, base_size);
    s.top.resize(4ull * half_layout.size, 0);
    s.half = std::move(half);
    s.header = xenos::texture_header_mips(width / 2, height / 2, fmt, levels - 1);
    return s;
}

struct DeepSplit
{
    std::vector<uint8_t> data;
    uint32_t mip_offset, level2_offset;
    std::vector<uint8_t> kept, header;
};

std::optional<DeepSplit> deep_split(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, const xenos::Format &fmt, int levels,
                                    bool eighth = false)
{
    auto first = split(pixels, width, height, fmt, levels);
    if (!first)
        return std::nullopt;
    auto second = split(first->half, width / 2, height / 2, fmt, levels - 1, 0);
    xenos::MipChainLayout layout = xenos::mip_chain_layout(width, height, fmt, levels);
    uint32_t base_size = layout.base_size, total = layout.total;
    if (!second || base_size % PAK_ALIGN || total + (PAK_ALIGN - total % PAK_ALIGN) % PAK_ALIGN > MAX_STREAM_BLOCK)
        return std::nullopt;
    std::vector<uint8_t> kept = second->half, header = second->header;
    if (eighth)
    {
        if (levels < 4 || std::min(width, height) / 8 <= 16)
            return std::nullopt;
        auto chain = xenos::untile_mip_chain(pixels.data(), pixels.size(), width, height, fmt, levels);
        std::vector<std::vector<uint8_t>> rest(chain.begin() + 3, chain.end());
        kept = xenos::tile_mip_chain(rest, width / 8, height / 8, fmt);
        header = xenos::texture_header_mips(width / 8, height / 8, fmt, static_cast<int>(rest.size()));
    }
    uint32_t level2 = xenos::mip_chain_layout(width / 2, height / 2, fmt, levels - 1).base_size;
    return DeepSplit{slice(pixels, 0, total), base_size, level2 % PAK_ALIGN == 0 ? level2 : 0, std::move(kept), std::move(header)};
}

// {lower case name: the image assets of that name}, in the zone's order
std::vector<std::pair<std::string, std::vector<Node *>>> images_by_name(const Platform &p, Zone &zone)
{
    std::vector<std::pair<std::string, std::vector<Node *>>> out;
    std::unordered_map<std::string, size_t> index;
    zone.walk([&](Node *node) {
        if (!is_asset(node, "GfxImage"))
            return;
        std::string name = asset_name(p, *node);
        if (name.empty() || name[0] == ',')
            return;
        std::string key = py::lower(name);
        auto [it, added] = index.emplace(key, out.size());
        if (added)
            out.emplace_back(key, std::vector<Node *>());
        out[it->second].second.push_back(node);
    });
    return out;
}

std::string some(std::vector<std::string> names)
{
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    size_t n = names.size();
    std::vector<std::string> first(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(4, n)));
    return py::join(first, ", ") + (n > 4 ? ", ..." : "");
}

std::string some_sorted(std::vector<std::string> names)
{
    std::sort(names.begin(), names.end());
    size_t n = names.size();
    std::vector<std::string> first(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(4, n)));
    return py::join(first, ", ") + (n > 4 ? ", ..." : "");
}

std::string mib(double bytes)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%.1f", bytes / 1048576.0);
    return buf;
}

// Images copied from console fastfiles that stream already get their .hi file in the map's pack when one
// of highmip_dirs has it; the others stop streaming and stay a level smaller.
void settle_console_streaming(const Platform &p, Zone &zone, PakWriter &pak, const std::vector<fs::path> &highmip_dirs, const Log &log)
{
    uint32_t flag = field_off(p, "GfxImage", "streaming"), size_field = field_off(p, "GfxImage", "baseSize");
    std::vector<std::string> copied, unstreamed;
    for (auto &[name, nodes] : images_by_name(p, zone))
    {
        for (Node *node : nodes)
        {
            if (!node->data[flag])
                continue;
            uint64_t size = static_cast<uint64_t>(be32(node->data.data() + size_field)) * 1024;
            std::optional<fs::path> source;
            for (const fs::path &d : highmip_dirs)
            {
                fs::path f = d / (std::wstring(name.begin(), name.end()) + L".hi");
                std::error_code ec;
                if (fs::is_regular_file(f, ec) && fs::file_size(f, ec) == size)
                {
                    source = f;
                    break;
                }
            }
            if (source && nodes.size() == 1)
            {
                pak.add(name, read_file(*source));
                copied.push_back(name);
            }
            else
            {
                node->data.mutable_data()[flag] = 0;
                unstreamed.push_back(name);
            }
        }
    }
    if (!copied.empty() && log)
        log("streaming: " + std::to_string(copied.size()) + " console images keep streaming, their .hi files copied from the game's highmip folder");
    if (!unstreamed.empty() && log)
        log("streaming: " + std::to_string(unstreamed.size()) + " console images streamed from .hi files no folder given has stay a level smaller (" +
            some(unstreamed) + ")");
}

// -- streaming bounds

using FullSizes = std::unordered_map<const Node *, std::pair<uint32_t, uint32_t>>;

// the texels of the largest image of a material that streams (its full size), or 0
class MaterialTexels
{
  public:
    MaterialTexels(const Platform &p, const FullSizes &full) : full(full)
    {
        count = field_off(p, "Material", "textureCount");
        table = field_off(p, "Material", "textureTable");
        size = p.record("MaterialTextureDef").size;
        semantic = field_off(p, "MaterialTextureDef", "semantic");
        image = field_off(p, "MaterialTextureDef", "u.image");
        width = field_off(p, "GfxImage", "width");
        flag = field_off(p, "GfxImage", "streaming");
    }

    uint64_t operator()(Node *material)
    {
        if (!material)
            return 0;
        auto it = cache.find(material);
        if (it != cache.end())
            return it->second;
        uint64_t best = 0;
        Node *t = target_of(material, table);
        int n = t ? material->data[count] : 0;
        for (int i = 0; i < n; ++i)
        {
            if (t->data[i * size + semantic] == SEMANTIC_WATER)
                continue;
            Node *img = target_of(t, i * size + image);
            if (img && img->data[flag])
            {
                auto f = full.find(img);
                if (f != full.end()) // streamed here: its fastfile copy a half, quarter or eighth
                    best = std::max<uint64_t>(best, static_cast<uint64_t>(f->second.first) * f->second.second);
                else // streaming already (the console's): it holds its half size
                {
                    const uint8_t *d = img->data.data() + width;
                    best = std::max<uint64_t>(best, 4ull * be16(d) * be16(d + 2));
                }
            }
        }
        cache[material] = best;
        return best;
    }

  private:
    const FullSizes &full;
    uint32_t count, table, size, semantic, image, width, flag;
    std::unordered_map<const Node *, uint64_t> cache;
};

using Vec3 = std::array<double, 3>;

double nan_min(double a, double b)
{
    return std::isnan(a) ? a : std::isnan(b) ? b : std::min(a, b);
}
double nan_max(double a, double b)
{
    return std::isnan(a) ? a : std::isnan(b) ? b : std::max(a, b);
}

// The region where triangles need the top level of a texture of texels: each triangle's box grown by
// STREAM_DISTANCE over its texels per unit, times scale; nothing when no triangle has an area. Exactly
// numpy's float64 operations (its norm adds the squares in order, without fused multiplies).
std::optional<Box> grown_box(const std::vector<std::array<Vec3, 3>> &pos, const std::vector<std::array<std::array<double, 2>, 3>> &uvs, uint64_t texels,
                             double scale)
{
    bool any = false;
    Box out{};
    const double texel_count = static_cast<double>(texels);
    for (size_t i = 0; i < pos.size(); ++i)
    {
        const Vec3 &p0 = pos[i][0], &p1 = pos[i][1], &p2 = pos[i][2];
        Vec3 a{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, b{p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        double c0 = a[1] * b[2] - a[2] * b[1];
        double c1 = a[2] * b[0] - a[0] * b[2];
        double c2 = a[0] * b[1] - a[1] * b[0];
        double s0 = c0 * c0, s1 = c1 * c1, s2 = c2 * c2;
        double world = 0.5 * std::sqrt((s0 + s1) + s2);
        const auto &t0 = uvs[i][0], &t1 = uvs[i][1], &t2 = uvs[i][2];
        double f1x = t1[0] - t0[0], f1y = t1[1] - t0[1], f2x = t2[0] - t0[0], f2y = t2[1] - t0[1];
        double l = f1x * f2y, r = f1y * f2x;
        double uv = 0.5 * std::abs(l - r);
        bool valid = world > 1e-6 && std::isfinite(world);
        if (!valid)
            continue;
        double grow = DEFAULT_STREAM_GROWTH * scale;
        if (uv > 0 && std::isfinite(uv))
        {
            double v = (STREAM_DISTANCE * scale) / std::sqrt((uv * texel_count) / world);
            double cap = MAX_STREAM_GROWTH * scale;
            grow = std::isnan(v) ? v : std::min(v, cap);
        }
        Box box;
        for (int k = 0; k < 3; ++k)
        {
            box[k] = nan_min(nan_min(p0[k], p1[k]), p2[k]) - grow;
            box[k + 3] = nan_max(nan_max(p0[k], p1[k]), p2[k]) + grow;
        }
        if (!any)
            out = box;
        else
            for (int k = 0; k < 3; ++k)
            {
                out[k] = nan_min(out[k], box[k]);
                out[k + 3] = nan_max(out[k + 3], box[k + 3]);
            }
        any = true;
    }
    if (!any)
        return std::nullopt;
    return out;
}

struct VertexLayout
{
    uint32_t xyz, uv, size;
    bool half; // texture coordinates as float16 (else float32)
};

Vec3 vertex_xyz(const uint8_t *v, const VertexLayout &l)
{
    return {bef32(v + l.xyz), bef32(v + l.xyz + 4), bef32(v + l.xyz + 8)};
}
std::array<double, 2> vertex_uv(const uint8_t *v, const VertexLayout &l)
{
    if (l.half)
        return {half_value(static_cast<uint16_t>(be16(v + l.uv))), half_value(static_cast<uint16_t>(be16(v + l.uv + 2)))};
    return {bef32(v + l.uv), bef32(v + l.uv + 4)};
}

std::optional<Box> triangles_box(const uint8_t *verts, const VertexLayout &l, const std::vector<std::array<int64_t, 3>> &tris, uint64_t texels,
                                 double scale)
{
    std::vector<std::array<Vec3, 3>> pos(tris.size());
    std::vector<std::array<std::array<double, 2>, 3>> uvs(tris.size());
    for (size_t i = 0; i < tris.size(); ++i)
        for (int k = 0; k < 3; ++k)
        {
            const uint8_t *v = verts + static_cast<size_t>(tris[i][k]) * l.size;
            pos[i][k] = vertex_xyz(v, l);
            uvs[i][k] = vertex_uv(v, l);
        }
    return grown_box(pos, uvs, texels, scale);
}

bool box_streams(const Box &b)
{
    return b[0] <= b[3] && b[1] <= b[4] && b[2] <= b[5];
}

// The boxes of the surfaces of a model's first level of detail (model space); nothing when it has none.
std::optional<std::vector<Box>> model_stream_bounds(const Platform &p, Node *model, MaterialTexels &texels, double scale)
{
    uint32_t lod = field_off(p, "XModel", "lodInfo");
    const uint8_t *md = model->data.data();
    uint32_t at = lod + field_off(p, "XModelLodInfo", "numsurfs");
    uint32_t numsurfs = be16(md + at), first = be16(md + at + 2);
    if (!numsurfs)
        return std::nullopt;
    Node *surfs = target_of(model, field_off(p, "XModel", "surfs"));
    Node *materials = target_of(model, field_off(p, "XModel", "materialHandles"));
    uint32_t size = p.record("XSurface").size;
    uint32_t o_verts = field_off(p, "XSurface", "verts0"), o_tris = field_off(p, "XSurface", "triIndices"), o_count = field_off(p, "XSurface", "triCount");
    VertexLayout layout{field_off(p, "GfxPackedVertex", "xyz"), field_off(p, "GfxPackedVertex", "texCoord"), p.record("GfxPackedVertex").size, true};
    uint32_t mins_off = field_off(p, "XModel", "mins"), maxs_off = field_off(p, "XModel", "maxs");
    double mins[3], maxs[3];
    for (int k = 0; k < 3; ++k)
    {
        mins[k] = bef32(md + mins_off + 4 * k);
        maxs[k] = bef32(md + maxs_off + 4 * k);
    }
    std::vector<Box> boxes;
    for (uint32_t s = first; s < first + numsurfs; ++s)
    {
        uint64_t n = texels(target_of(materials, 4 * s));
        std::optional<Box> box;
        if (n && surfs)
        {
            Node *verts = target_of(surfs, s * size + o_verts), *tris = target_of(surfs, s * size + o_tris);
            uint32_t count = be16(surfs->data.data() + s * size + o_count);
            if (verts && tris && count)
            {
                size_t nverts = verts->data.size() / layout.size;
                if (tris->data.size() < 6ull * count)
                    throw std::runtime_error("buffer is smaller than requested size"); // as numpy's frombuffer
                std::vector<std::array<int64_t, 3>> t(count);
                int64_t top = 0;
                for (uint32_t i = 0; i < count; ++i)
                    for (int k = 0; k < 3; ++k)
                    {
                        t[i][k] = be16(tris->data.data() + 2 * (3 * i + k));
                        top = std::max(top, t[i][k]);
                    }
                if (nverts && top < static_cast<int64_t>(nverts))
                    box = triangles_box(verts->data.data(), layout, t, n, scale);
            }
            if (!box && mins[0] <= maxs[0] && mins[1] <= maxs[1] && mins[2] <= maxs[2])
            {
                double grow = DEFAULT_STREAM_GROWTH * scale;
                box = Box{mins[0] - grow, mins[1] - grow, mins[2] - grow, maxs[0] + grow, maxs[1] + grow, maxs[2] + grow};
            }
        }
        boxes.push_back(box ? *box : NO_STREAM_BOUNDS);
    }
    return boxes;
}

// The world's streaming tree and its leaf references of items (reference, box).
struct TreeNode
{
    Box box;
    std::vector<std::pair<int32_t, Box>> items;
    std::vector<std::unique_ptr<TreeNode>> kids;
    size_t first = 0, count = 0, child = 0;
};

std::unique_ptr<TreeNode> build_tree(std::vector<std::pair<int32_t, Box>> group)
{
    auto node = std::make_unique<TreeNode>();
    for (int k = 0; k < 6; ++k)
    {
        double v = group[0].second[k];
        for (const auto &[ref, b] : group)
            v = k < 3 ? nan_min(v, b[k]) : nan_max(v, b[k]);
        node->box[k] = v;
    }
    if (group.size() <= LEAF_REFS)
    {
        node->items = std::move(group);
        return node;
    }
    std::vector<std::array<double, 3>> centers(group.size());
    for (size_t i = 0; i < group.size(); ++i)
        for (int k = 0; k < 3; ++k)
            centers[i][k] = (group[i].second[k] + group[i].second[k + 3]) / 2;
    int axis = 0;
    double best = 0;
    for (int k = 0; k < 3; ++k)
    {
        double lo = centers[0][k], hi = centers[0][k];
        for (const auto &c : centers)
        {
            lo = nan_min(lo, c[k]);
            hi = nan_max(hi, c[k]);
        }
        double extent = hi - lo;
        if (k == 0 || extent > best)
        {
            best = extent;
            axis = k;
        }
    }
    std::vector<size_t> order(group.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return centers[a][axis] < centers[b][axis]; });
    size_t half = group.size() / 2;
    std::vector<std::pair<int32_t, Box>> left, right;
    for (size_t i = 0; i < order.size(); ++i)
        (i < half ? left : right).push_back(group[order[i]]);
    node->kids.push_back(build_tree(std::move(left)));
    node->kids.push_back(build_tree(std::move(right)));
    return node;
}

void assign_refs(TreeNode *node, std::vector<int32_t> &refs)
{
    node->first = refs.size();
    for (const auto &[ref, b] : node->items)
        refs.push_back(ref);
    for (auto &kid : node->kids)
        assign_refs(kid.get(), refs);
    node->count = refs.size() - node->first;
}

std::pair<std::vector<uint8_t>, std::vector<int32_t>> stream_tree(std::vector<std::pair<int32_t, Box>> items)
{
    std::unique_ptr<TreeNode> root = build_tree(std::move(items));
    std::vector<int32_t> refs;
    assign_refs(root.get(), refs);
    std::vector<TreeNode *> nodes{root.get()};
    for (size_t i = 0; i < nodes.size(); ++i) // breadth first: a node's children are consecutive
    {
        TreeNode *n = nodes[i];
        n->child = n->kids.empty() ? 0 : nodes.size();
        for (auto &kid : n->kids)
            nodes.push_back(kid.get());
    }
    std::vector<uint8_t> data;
    for (TreeNode *n : nodes)
    {
        put_be(data, n->first, 2);
        put_be(data, n->count, 2);
        put_be(data, n->child, 2);
        put_be(data, n->kids.size(), 2);
        for (double v : n->box)
            put_bef(data, v);
    }
    return {data, refs};
}

// GfxWorld.streamInfo.<member> points to a new array, placed in the stream where the console reads it
void set_world_array(const Platform &p, Zone &zone, Node *world, const std::string &member, const std::vector<uint8_t> &values, uint32_t count,
                     Scalar scalar)
{
    uint32_t offset = field_off(p, "GfxWorld", "streamInfo." + member);
    Node *old = target_of(world, offset);
    if (old)
    {
        auto it = std::find(world->children.begin(), world->children.end(), old);
        if (it != world->children.end())
            world->children.erase(it);
    }
    world->relocs.erase(offset);
    if (!count)
    {
        set_be(world, offset, 0, 4);
        return;
    }
    const TypeRef *t = p.layout->scalar(scalar);
    Node *node = zone.new_node();
    node->type = t;
    node->count = count;
    node->block = BLOCK_VIRTUAL;
    node->data.assign(values);
    node->segments.push_back({t, count, static_cast<uint32_t>(values.size()), false});
    node->align = 4;
    node->origin = Origin::Member;
    node->origin_record = intern("GfxWorldStreamInfo360");
    node->origin_field = intern(member);
    std::unordered_map<const Node *, uint32_t> at;
    for (const auto &[off, ptr] : world->relocs.list())
        if (Node *t2 = ptr->target())
            at[t2] = off; // the last pointer to a node gives its offset, as the Python's dict
    size_t pos = world->children.size();
    for (size_t i = 0; i < world->children.size(); ++i)
    {
        auto f = at.find(world->children[i]);
        if (f != at.end() && f->second > offset)
        {
            pos = i;
            break;
        }
    }
    Ptr *ptr = zone.new_ptr(Ptr::Kind::Follow);
    ptr->node = node;
    ptr->owner = world;
    ptr->offset = offset;
    world->relocs.set(offset, ptr);
    world->children.insert(world->children.begin() + static_cast<std::ptrdiff_t>(pos), node);
}

// The boxes that load streamed images: each model's surfaces', each world surface's and the world's
// tree of them and of the static models.
void write_stream_bounds(const Platform &p, Zone &zone, const Log &log, double scale, const FullSizes &full)
{
    MaterialTexels texels(p, full);
    int models = 0, static_models = 0;
    size_t surfaces_streamed = 0;
    std::unordered_map<const Node *, std::vector<Box>> model_boxes;
    uint32_t bounds_offset = field_off(p, "XModel", "streamInfo.highMipBounds");
    std::vector<Node *> model_nodes;
    Node *world = nullptr;
    zone.walk([&](Node *n) {
        if (is_asset(n, "XModel"))
            model_nodes.push_back(n);
        if (!world && is_asset(n, "GfxWorld"))
            world = n;
    });
    for (Node *model : model_nodes)
    {
        Node *node = target_of(model, bounds_offset);
        std::optional<std::vector<Box>> boxes = node ? model_stream_bounds(p, model, texels, scale) : std::nullopt;
        if (!boxes || boxes->size() != node->count)
            continue;
        std::vector<uint8_t> data;
        for (const Box &b : *boxes)
            for (double v : b)
                put_bef(data, v);
        node->data.assign(std::move(data));
        std::vector<Box> streaming;
        for (const Box &b : *boxes)
            if (box_streams(b))
                streaming.push_back(b);
        models += !streaming.empty();
        model_boxes[model] = std::move(streaming);
    }
    if (!world)
        return;
    std::vector<std::pair<int32_t, Box>> items;
    // the surfaces
    Node *surfaces = target_of(world, field_off(p, "GfxWorld", "dpvs.surfaces"));
    Node *vertices = target_of(world, field_off(p, "GfxWorld", "vd.vertices"));
    Node *indices = target_of(world, field_off(p, "GfxWorld", "indices"));
    int32_t surface_count = static_cast<int32_t>(be32(world->data.data() + field_off(p, "GfxWorld", "surfaceCount")));
    if (surfaces && vertices && indices)
    {
        uint32_t size = p.record("GfxSurface").size;
        uint32_t o_first = field_off(p, "GfxSurface", "tris.firstVertex"), o_tris = field_off(p, "GfxSurface", "tris.triCount"),
                 o_base = field_off(p, "GfxSurface", "tris.baseIndex"), o_box = field_off(p, "GfxSurface", "boundsCopy"),
                 o_material = field_off(p, "GfxSurface", "material"), o_bounds = field_off(p, "GfxSurface", "bounds");
        VertexLayout layout{field_off(p, "GfxWorldVertex", "xyz"), field_off(p, "GfxWorldVertex", "texCoord"), p.record("GfxWorldVertex").size, false};
        size_t nverts = vertices->data.size() / layout.size;
        size_t nindex = indices->data.size() / 2;
        int64_t n_surf = std::min<int64_t>(surface_count, static_cast<int64_t>(surfaces->data.size() / size));
        for (int64_t s = 0; s < n_surf; ++s)
        {
            uint32_t at = static_cast<uint32_t>(s) * size;
            uint64_t n = texels(target_of(surfaces, at + o_material));
            std::optional<Box> box;
            if (n)
            {
                const uint8_t *sd = surfaces->data.data() + at;
                int64_t first = static_cast<int32_t>(be32(sd + o_first));
                int64_t count = be16(sd + o_tris);
                int64_t base = static_cast<int32_t>(be32(sd + o_base));
                // index[base : base + 3 * count] (a Python slice)
                int64_t len = static_cast<int64_t>(nindex);
                auto clamp = [&](int64_t i) { return i < 0 ? std::max<int64_t>(i + len, 0) : std::min(i, len); };
                int64_t a = clamp(base), b = clamp(base + 3 * count);
                if (b - a == 3 * count && count)
                {
                    std::vector<std::array<int64_t, 3>> tris(static_cast<size_t>(count));
                    int64_t top = std::numeric_limits<int64_t>::min();
                    for (int64_t i = 0; i < count; ++i)
                        for (int k = 0; k < 3; ++k)
                        {
                            int64_t v = static_cast<int64_t>(be16(indices->data.data() + 2 * (a + 3 * i + k))) + first;
                            tris[static_cast<size_t>(i)][k] = v;
                            top = std::max(top, v);
                        }
                    if (top < static_cast<int64_t>(nverts))
                    {
                        for (auto &t : tris) // numpy's negative indices count from the end
                            for (auto &v : t)
                                if (v < 0)
                                    v += static_cast<int64_t>(nverts);
                        box = triangles_box(vertices->data.data(), layout, tris, n, scale);
                    }
                }
                if (!box)
                {
                    double grow = DEFAULT_STREAM_GROWTH * scale;
                    Box b2;
                    for (int k = 0; k < 3; ++k)
                    {
                        b2[k] = bef32(sd + o_bounds + 4 * k) - grow;
                        b2[k + 3] = bef32(sd + o_bounds + 12 + 4 * k) + grow;
                    }
                    box = b2;
                }
                items.emplace_back(static_cast<int32_t>(s), *box);
            }
            const Box &written = box ? *box : NO_STREAM_BOUNDS;
            for (int k = 0; k < 6; ++k)
                set_bef(surfaces, at + o_box + 4 * k, written[k]);
        }
        surfaces_streamed = items.size();
    }
    // the static models: their box in the world, a sphere around their origin holding their surfaces'
    Node *insts = target_of(world, field_off(p, "GfxWorld", "dpvs.smodelDrawInsts"));
    uint32_t smodel_count = be32(world->data.data() + field_off(p, "GfxWorld", "dpvs.smodelCount"));
    if (insts)
    {
        uint32_t size = p.record("GfxStaticModelDrawInst").size;
        uint32_t o_origin = field_off(p, "GfxStaticModelDrawInst", "origin"), o_model = field_off(p, "GfxStaticModelDrawInst", "model");
        uint32_t n = std::min<uint32_t>(smodel_count, static_cast<uint32_t>(insts->data.size() / size));
        for (uint32_t i = 0; i < n; ++i)
        {
            auto it = model_boxes.find(target_of(insts, i * size + o_model));
            if (it == model_boxes.end() || it->second.empty())
                continue;
            double radius = 0;
            bool first = true;
            for (const Box &box : it->second)
                for (int corner = 0; corner < 8; ++corner)
                {
                    double x[3];
                    for (int k = 0; k < 3; ++k)
                        x[k] = box[3 * ((corner >> k) & 1) + k];
                    double norm = std::sqrt((x[0] * x[0] + x[1] * x[1]) + x[2] * x[2]);
                    if (first || norm > radius)
                        radius = norm;
                    first = false;
                }
            const uint8_t *od = insts->data.data() + i * size + o_origin;
            Box b;
            for (int k = 0; k < 3; ++k)
            {
                double o = bef32(od + 4 * k);
                b[k] = o - radius;
                b[k + 3] = o + radius;
            }
            items.emplace_back(~static_cast<int32_t>(i), b);
            ++static_models;
        }
    }
    if (items.size() > USHORT_MAX)
    {
        if (log)
            log("warning: streaming: " + std::to_string(items.size()) + " surfaces and static models stream, the world's tree holds " +
                std::to_string(USHORT_MAX) + ": the rest keep their textures' top level unloaded");
        items.resize(USHORT_MAX);
    }
    bool any = !items.empty();
    std::pair<std::vector<uint8_t>, std::vector<int32_t>> tree;
    if (any)
        tree = stream_tree(items);
    std::vector<uint8_t> refs;
    for (int32_t r : tree.second)
        put_be(refs, static_cast<uint32_t>(r), 4);
    set_world_array(p, zone, world, "aabbTrees", tree.first, static_cast<uint32_t>(tree.first.size() / 4), Scalar::UInt);
    set_world_array(p, zone, world, "leafRefs", refs, static_cast<uint32_t>(tree.second.size()), Scalar::Int);
    set_be(world, field_off(p, "GfxWorld", "streamInfo.aabbTreeCount"), tree.first.size() / 32, 4);
    set_be(world, field_off(p, "GfxWorld", "streamInfo.leafRefCount"), tree.second.size(), 4);
    if (any && log)
        log("streaming: the world's tree has " + std::to_string(surfaces_streamed) + " surfaces and " + std::to_string(static_models) +
            " static models (" + std::to_string(tree.first.size() / 32) + " nodes); " + std::to_string(models) + " models have surfaces that stream");
}
} // namespace

std::vector<std::string> stream_textures(const Platform &p, Zone &zone, const fs::path &out_dir, const Log &log, const StreamOptions &o,
                                         StreamReport *report)
{
    uint32_t flag = field_off(p, "GfxImage", "streaming"), semantic = field_off(p, "GfxImage", "semantic");
    uint32_t ld_format = field_off(p, "GfxImageLoadDef", "format"), ld_levels = field_off(p, "GfxImageLoadDef", "levelCount"),
             ld_dims = field_off(p, "GfxImageLoadDef", "dimensions");
    // the files of an earlier conversion go: the zone written now is the one they must match
    fs::path directory = out_dir / HIGHMIP_DIRECTORY;
    if (fs::is_directory(directory))
    {
        for (const auto &e : fs::directory_iterator(directory))
            if (py::ends_with(py::lower(e.path().filename().string()), ".hi"))
                fs::remove(e.path());
        if (fs::is_empty(directory))
            fs::remove(directory);
    }
    PakWriter pak(out_dir / PAK_NAME);
    settle_console_streaming(p, zone, pak, o.highmip_dirs, log);
    // a .hi file is one name's: images sharing a name stay whole
    std::unordered_set<std::string> shared_names;
    for (auto &[name, nodes] : images_by_name(p, zone))
        if (nodes.size() > 1 || (o.is_game_image && o.is_game_image(name)))
            shared_names.insert(name);
    std::vector<std::string> streamed, upgraded, deepened, mipped, eighths;
    int64_t saved = 0;
    uint64_t upgrade_bytes = 0;
    FullSizes full;
    auto put_image = [&](Node *image, const char *field, uint64_t value) {
        uint32_t off = field_off(p, "GfxImage", field);
        int bytes = std::string_view(field) == "width" || std::string_view(field) == "height" || std::string_view(field) == "streamSlot" ? 2
                    : std::string_view(field) == "streaming"                                                                         ? 1
                                                                                                                                      : 4;
        set_be(image, off, value, bytes);
    };
    for (Node *image : streamable_images(zone))
    {
        std::string name = asset_name(p, *image);
        auto parts = image_parts(p, image);
        if (name.empty() || name[0] == ',' || !parts || py::contains(name, "\\") || py::contains(name, "/"))
            continue;
        if (image->data[flag] || shared_names.count(py::lower(name)))
            continue;
        Node *load_def = parts->load_def, *header = parts->header, *pixels = parts->pixels;
        const xenos::Format *fmt = parts->fmt;
        uint32_t width = parts->width, height = parts->height;
        int levels = parts->levels;
        std::vector<uint8_t> source(pixels->data.begin(), pixels->data.end());
        if (levels == 1 && xenos::mip_chain_layout(width, height, *fmt, 1).base_size >= MIN_HIGHMIP_BYTES)
        {
            // saved without mips: given them, it streams as the others
            auto made = with_mips(source.data(), source.size(), width, height, *fmt, o.mip_tail);
            if (made && split(made->pixels, width, height, *fmt, made->levels))
            {
                source = std::move(made->pixels);
                levels = made->levels;
                mipped.push_back(name);
            }
        }
        std::optional<Split> result = split(source, width, height, *fmt, levels);
        // a stock texture the console sized down: the PC game's, streamed, when it takes no more memory
        // than the console's copy or the budget has the difference
        bool wants_deep = o.deep_all || o.deep.count(py::lower(name));
        std::optional<ConsoleTexture> tex = o.stock_texture ? o.stock_texture(name, image->data[semantic]) : std::nullopt;
        if (tex && tex->faces == 1 && tex->width >= width && tex->height >= height &&
            static_cast<uint64_t>(tex->width) * tex->height > static_cast<uint64_t>(width) * height)
        {
            auto better = split(tex->pixels, tex->width, tex->height, *tex->format, tex->levels);
            std::optional<DeepSplit> whole =
                wants_deep && better ? deep_split(tex->pixels, tex->width, tex->height, *tex->format, tex->levels) : std::nullopt;
            if (better)
            {
                int64_t extra = static_cast<int64_t>(whole ? whole->kept.size() : better->half.size()) - static_cast<int64_t>(pixels->data.size());
                if (extra <= 0 || (o.upgrade_budget && upgrade_bytes + extra <= *o.upgrade_budget))
                {
                    upgrade_bytes += std::max<int64_t>(extra, 0);
                    result = std::move(better);
                    fmt = tex->format;
                    width = tex->width;
                    height = tex->height;
                    levels = tex->levels;
                    source = tex->pixels;
                    set_be(load_def, ld_format, fmt->d3d, 4);
                    upgraded.push_back(name);
                }
            }
        }
        if (!result)
            continue;
        std::vector<uint8_t> kept = std::move(result->half), kept_header = std::move(result->header);
        int steps = 1;
        uint64_t entry = result->top.size();
        bool wants_eighth = wants_deep && o.eighth.count(py::lower(name));
        std::optional<DeepSplit> whole = wants_eighth ? deep_split(source, width, height, *fmt, levels, true) : std::nullopt;
        uint32_t flags = whole ? (PAK_DEEP | PAK_EIGHTH) : PAK_DEEP;
        if (!whole && wants_deep)
            whole = deep_split(source, width, height, *fmt, levels);
        if (whole)
        {
            std::vector<uint8_t> data = std::move(whole->data);
            data.resize(data.size() + (PAK_ALIGN - data.size() % PAK_ALIGN) % PAK_ALIGN, 0); // the game reads 4 KiB multiples
            pak.add(name, data, flags, whole->mip_offset, whole->level2_offset);
            kept = std::move(whole->kept);
            kept_header = std::move(whole->header);
            steps = flags & PAK_EIGHTH ? 3 : 2;
            entry = data.size();
            deepened.push_back(name);
            if (steps == 3)
                eighths.push_back(name);
        }
        else
            pak.add(name, result->top);
        saved += static_cast<int64_t>(pixels->data.size()) - static_cast<int64_t>(kept.size());
        uint32_t kept_size = static_cast<uint32_t>(kept.size());
        pixels->data.assign(std::move(kept));
        pixels->count = kept_size;
        pixels->segments = {Segment{pixels->type, kept_size, kept_size, false}};
        header->data.assign(std::move(kept_header));
        full[image] = {width, height}; // for the streaming boxes
        set_be(load_def, ld_levels, static_cast<uint32_t>(levels - steps), 1);
        set_be(load_def, ld_dims, width >> steps, 2);
        set_be(load_def, ld_dims + 2, height >> steps, 2);
        put_image(image, "width", width >> steps);
        put_image(image, "height", height >> steps);
        put_image(image, "cardMemory", kept_size);
        put_image(image, "baseSize", entry / 1024);
        put_image(image, "streamSlot", 0xFFFF);
        put_image(image, "streaming", 1);
        streamed.push_back(name);
    }
    size_t entries = pak.close();
    // the stock textures that stream not: the PC game's whole, in the fastfile, while the budget has it
    std::vector<std::string> kept_whole;
    std::unordered_set<std::string> done;
    for (const std::string &n : streamed)
        done.insert(py::lower(n));
    if (o.stock_texture && o.upgrade_budget)
    {
        for (auto &[name, nodes] : images_by_name(p, zone))
        {
            Node *image = nodes[0];
            auto parts = image_parts(p, image);
            if (nodes.size() > 1 || done.count(name) || shared_names.count(name) || image->data[flag] || !parts || py::contains(name, "/"))
                continue;
            std::optional<ConsoleTexture> tex = o.stock_texture(name, image->data[semantic]);
            if (!tex || tex->faces != 1 || tex->width < parts->width || tex->height < parts->height ||
                static_cast<uint64_t>(tex->width) * tex->height <= static_cast<uint64_t>(parts->width) * parts->height)
                continue;
            int64_t extra = static_cast<int64_t>(tex->pixels.size()) - static_cast<int64_t>(parts->pixels->data.size());
            if (extra > 0 && upgrade_bytes + extra > *o.upgrade_budget)
                continue;
            upgrade_bytes += std::max<int64_t>(extra, 0);
            uint32_t size = static_cast<uint32_t>(tex->pixels.size());
            parts->pixels->data.assign(tex->pixels);
            parts->pixels->count = size;
            parts->pixels->segments = {Segment{parts->pixels->type, size, size, false}};
            parts->header->data.assign(tex->header);
            set_be(parts->load_def, ld_levels, static_cast<uint32_t>(tex->levels), 1);
            set_be(parts->load_def, ld_dims, tex->width, 2);
            set_be(parts->load_def, ld_dims + 2, tex->height, 2);
            set_be(parts->load_def, ld_format, tex->format->d3d, 4);
            put_image(image, "width", tex->width);
            put_image(image, "height", tex->height);
            put_image(image, "cardMemory", size);
            put_image(image, "baseSize", tex->base_size ? tex->base_size : size);
            kept_whole.push_back(name);
        }
    }
    upgraded.insert(upgraded.end(), kept_whole.begin(), kept_whole.end());
    if (!streamed.empty() && log)
        log("streaming: " + std::to_string(streamed.size()) + " textures keep their top level in " + PAK_NAME + " (" + std::to_string(entries) +
            " entries with the console's), loaded when what uses them is close (" + mib(static_cast<double>(saved)) + " MiB less in the fastfile)");
    if (!deepened.empty() && log)
        log("streaming: " + std::to_string(deepened.size()) + " of them stream two levels at once, the fastfile keeping a quarter of their size (" +
            some_sorted(deepened) + ")");
    if (!eighths.empty() && log)
        log("streaming: " + std::to_string(eighths.size()) + " of those keep an eighth of their size instead, to fit the memory target (three levels: " +
            some_sorted(eighths) + ")");
    if (!mipped.empty() && log)
        log("streaming: " + std::to_string(mipped.size()) + " of them had no mip levels (saved without), made for them so they stream (" +
            some_sorted(mipped) + ")");
    if (report)
    {
        report->upgrade_bytes = upgrade_bytes;
        std::unordered_set<std::string> deep_names(deepened.begin(), deepened.end()), eighth_names(eighths.begin(), eighths.end());
        report->steps.clear();
        for (const std::string &n : streamed)
            report->steps[py::lower(n)] = eighth_names.count(n) ? 3 : deep_names.count(n) ? 2 : 1;
    }
    if (!upgraded.empty() && log)
        log("streaming: " + std::to_string(upgraded.size()) + " stock textures the console has smaller are the PC game's (" +
            std::to_string(upgraded.size() - kept_whole.size()) + " streamed, " + std::to_string(kept_whole.size()) + " whole in the fastfile), " +
            mib(static_cast<double>(upgrade_bytes)) + " MiB more in the fastfile (" + some_sorted(upgraded) + ")");
    write_stream_bounds(p, zone, log, o.growth, full);
    return streamed;
}

// -- memory

std::vector<uint32_t> block_sizes(const std::vector<uint8_t> &zone)
{
    std::vector<uint32_t> sizes(7);
    for (int i = 0; i < 7; ++i)
        sizes[i] = be32(zone.data() + 8 + 4 * i);
    return sizes;
}

uint64_t memory_bytes(const std::vector<uint32_t> &sizes)
{
    uint64_t total = 0;
    for (uint32_t s : sizes)
        total += s;
    return total;
}

uint64_t texture_bytes(Zone &zone)
{
    uint64_t total = 0;
    zone.walk([&](Node *n) {
        if (n->delayed)
            total += n->data.size();
    });
    return total;
}

std::optional<int64_t> next_texture_budget(int64_t total, int64_t target, int64_t textures, int64_t budget, double efficiency)
{
    if (total <= target)
        return std::nullopt;
    double cut = static_cast<double>(total - target + MARGIN_MIB * static_cast<int64_t>(MIB)) / std::max(efficiency, 1.0 / 16);
    int64_t now = std::min(budget, textures) - static_cast<int64_t>(cut);
    int64_t floor = MIN_TEXTURE_BUDGET_MIB * static_cast<int64_t>(MIB);
    if (now < floor)
        now = floor;
    if (now < budget)
        return now;
    return std::nullopt;
}
} // namespace t4ff
