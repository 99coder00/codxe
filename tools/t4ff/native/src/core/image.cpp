#include "core/image.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "core/dxt.h"
#include "core/fastfile.h"
#include "core/wavelet.h"
#include "core/zip.h"

namespace t4ff
{
namespace
{
// IWI v6 (World at War) image formats
std::optional<Fmt> iwi_format(int code)
{
    switch (code)
    {
    case 0x01: return Fmt::A8R8G8B8;
    case 0x02: return Fmt::R8G8B8;
    case 0x03: return Fmt::A8L8;
    case 0x04: return Fmt::L8;
    case 0x05: return Fmt::A8;
    case 0x0B: return Fmt::DXT1;
    case 0x0C: return Fmt::DXT3;
    case 0x0D: return Fmt::DXT5;
    }
    return std::nullopt;
}

std::string lower_rel(std::string s)
{
    for (char &c : s)
    {
        if (c == '\\')
            c = '/';
        else if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

uint32_t half(uint32_t v)
{
    return std::max(v >> 1, 1u);
}

ImageData with_levels(const ImageData &like, Fmt format, uint32_t width, uint32_t height, std::vector<std::vector<uint8_t>> levels,
                      int faces = 1)
{
    ImageData out;
    out.name = like.name;
    out.format = format;
    out.width = width;
    out.height = height;
    out.levels = std::move(levels);
    out.flags = like.flags;
    out.source = like.source;
    out.faces = faces;
    return out;
}

// fn applied to every face of a cube map (to the image itself when it is 2D)
ImageData each_face(const ImageData &image, const std::function<ImageData(const ImageData &)> &fn)
{
    if (image.faces == 1)
        return fn(image);
    std::vector<ImageData> faces;
    for (int i = 0; i < image.faces; ++i)
        faces.push_back(fn(image.face(i)));
    return join_faces(faces);
}

std::vector<uint8_t> rgba_bytes(const Pixels &p)
{
    return p.data;
}

// RGBA as A8R8G8B8 bytes (B, G, R, A)
std::vector<uint8_t> rgba_to_bgra(const Pixels &p)
{
    std::vector<uint8_t> out(p.data.size());
    for (size_t i = 0; i < out.size(); i += 4)
    {
        out[i] = p.data[i + 2];
        out[i + 1] = p.data[i + 1];
        out[i + 2] = p.data[i];
        out[i + 3] = p.data[i + 3];
    }
    return out;
}

int drop_count(const ImageData &image, uint32_t max_size, int drop_levels)
{
    int count = 0;
    uint32_t w = image.width, h = image.height;
    while (std::min(w, h) > 4 && ((max_size && (w > max_size || h > max_size)) || count < drop_levels))
    {
        ++count;
        w = half(w);
        h = half(h);
    }
    return count;
}

// Whether compress makes image DXT: cube maps up to 64 texels stay uncompressed.
bool compresses(const ImageData &image)
{
    return image.faces == 1 || std::max(image.width, image.height) > 64;
}
} // namespace

ImageData ImageData::face(int index) const
{
    std::vector<std::vector<uint8_t>> out;
    uint32_t w = width, h = height;
    for (const auto &level : levels)
    {
        size_t size = level_size(format, w, h);
        size_t start = std::min(level.size(), index * size), end = std::min(level.size(), (index + 1) * size);
        out.emplace_back(level.begin() + start, level.begin() + end);
        w = half(w);
        h = half(h);
    }
    return with_levels(*this, format, width, height, std::move(out));
}

ImageData join_faces(const std::vector<ImageData> &faces)
{
    const ImageData &first = faces.at(0);
    std::vector<std::vector<uint8_t>> levels(first.levels.size());
    for (size_t i = 0; i < levels.size(); ++i)
        for (const ImageData &f : faces)
            levels[i].insert(levels[i].end(), f.levels.at(i).begin(), f.levels.at(i).end());
    return with_levels(first, first.format, first.width, first.height, std::move(levels), static_cast<int>(faces.size()));
}

ImageData parse_iwi(const std::string &name, const uint8_t *data, size_t size)
{
    if (size < 3 || std::memcmp(data, "IWi", 3) != 0)
        throw ImageError(name + ": not an IWI file");
    if (size < 12)
        throw ImageError(name + ": truncated IWI header");
    int version = data[3];
    if (version != 6)
        throw ImageError(name + ": unsupported IWI version " + std::to_string(version));
    int fmt_code = data[4], flags = data[5];
    uint32_t width = data[6] | data[7] << 8, height = data[8] | data[9] << 8;
    if (flags & IWI_FLAG_VOLMAP)
        throw ImageError(name + ": volume maps are not supported yet");
    // a cube map stores its six faces one after the other in every level
    int faces = flags & IWI_FLAG_CUBEMAP ? 6 : 1;
    const uint8_t *payload = data + std::min<size_t>(size, 28);
    size_t payload_size = size > 28 ? size - 28 : 0;

    ImageData image;
    image.name = name;
    image.width = width;
    image.height = height;
    image.flags = flags;
    image.source = "iwi";
    image.faces = faces;
    if (wavelet::is_wavelet_format(fmt_code))
    {
        try
        {
            wavelet::Decoded d = wavelet::decode(fmt_code, width, height, faces, !(flags & IWI_FLAG_NOMIPMAPS), payload, payload_size);
            image.format = d.format;
            image.levels = std::move(d.levels);
        }
        catch (const wavelet::WaveletError &e)
        {
            throw ImageError(name + ": wavelet IWI: " + e.what());
        }
        return image;
    }
    std::optional<Fmt> fmt = iwi_format(fmt_code);
    if (!fmt)
    {
        char buf[16];
        snprintf(buf, sizeof buf, "%#x", fmt_code);
        throw ImageError(name + ": unsupported IWI format " + buf);
    }
    image.format = *fmt;

    int count = flags & IWI_FLAG_NOMIPMAPS ? 1 : mip_count(width, height);
    std::vector<size_t> sizes;
    size_t total = 0;
    uint32_t w = width, h = height;
    for (int i = 0; i < count; ++i)
    {
        sizes.push_back(static_cast<size_t>(level_size(*fmt, w, h)) * faces);
        total += sizes.back();
        w = half(w);
        h = half(h);
    }
    if (payload_size < total)
    {
        // some IWIs only carry the base level
        if (payload_size >= sizes[0])
        {
            image.levels.emplace_back(payload, payload + sizes[0]);
            return image;
        }
        throw ImageError(name + ": truncated IWI (" + std::to_string(payload_size) + " < " + std::to_string(total) + ")");
    }
    // IWI files store the mip chain from the smallest level to the largest
    size_t end = total;
    for (size_t s : sizes)
    {
        image.levels.emplace_back(payload + end - s, payload + end);
        end -= s;
    }
    return image;
}

// -- IwdLibrary

IwdLibrary::IwdLibrary(const std::vector<std::filesystem::path> &paths)
{
    for (const auto &p : paths)
        add(p);
}

IwdLibrary::~IwdLibrary() = default;

void IwdLibrary::add_entry(std::string rel, Entry entry)
{
    if (entries.emplace(rel, std::move(entry)).second)
        order.push_back(std::move(rel));
}

void IwdLibrary::add(const std::filesystem::path &path)
{
    namespace fs = std::filesystem;
    if (fs::is_directory(path))
    {
        // as os.walk: a folder's files, then its folders
        std::function<void(const fs::path &)> walk = [&](const fs::path &dir) {
            std::vector<fs::path> files, dirs;
            for (const auto &e : fs::directory_iterator(dir))
                (e.is_directory() ? dirs : files).push_back(e.path());
            for (const fs::path &f : files)
            {
                std::string fname = lower_rel(f.filename().string());
                if (fname.size() >= 4 && fname.compare(fname.size() - 4, 4, ".iwd") == 0)
                    add(f);
                else
                {
                    Entry entry;
                    entry.file = f;
                    add_entry(lower_rel(fs::relative(f, path).string()), std::move(entry));
                }
            }
            for (const fs::path &d : dirs)
                walk(d);
        };
        walk(path);
        return;
    }
    std::unique_ptr<ZipArchive> archive;
    try
    {
        archive = std::make_unique<ZipArchive>(path);
    }
    catch (const ZipError &e)
    {
        throw ImageError("cannot open " + path.string() + ": " + e.what());
    }
    for (size_t i = 0; i < archive->entries().size(); ++i)
    {
        Entry entry;
        entry.archive = archive.get();
        entry.index = i;
        add_entry(lower_rel(archive->entries()[i].name), std::move(entry));
    }
    archives.push_back(std::move(archive));
}

std::optional<std::vector<uint8_t>> IwdLibrary::read(const std::string &rel) const
{
    auto it = entries.find(lower_rel(rel));
    if (it == entries.end())
        return std::nullopt;
    const Entry &e = it->second;
    if (!e.archive)
        return read_file(e.file);
    return e.archive->read(e.archive->entries()[e.index]);
}

bool IwdLibrary::contains(const std::string &rel) const
{
    return entries.count(lower_rel(rel)) != 0;
}

std::vector<std::string> IwdLibrary::names(const std::string &prefix) const
{
    std::string p = lower_rel(prefix);
    std::vector<std::string> out;
    for (const std::string &n : order)
        if (n.compare(0, p.size(), p) == 0)
            out.push_back(n);
    return out;
}

std::optional<ImageData> IwdLibrary::image(const std::string &name) const
{
    auto data = read("images/" + name + ".iwi");
    if (!data)
        return std::nullopt;
    return parse_iwi(name, data->data(), data->size());
}

// -- console textures

std::optional<Fmt> to_console_format(Fmt f)
{
    switch (f)
    {
    case Fmt::DXT1:
    case Fmt::DXT3:
    case Fmt::DXT5:
    case Fmt::DXN:
    case Fmt::A8R8G8B8:
    case Fmt::A8L8:
    case Fmt::L8: return f;
    default: return std::nullopt;
    }
}

ImageData convert_rgb24(const ImageData &image)
{
    std::vector<std::vector<uint8_t>> levels;
    for (const auto &level : image.levels)
    {
        if (level.size() % 3)
            throw ImageError(image.name + ": R8G8B8 level of a size not a multiple of 3");
        std::vector<uint8_t> bgra(level.size() / 3 * 4);
        for (size_t i = 0, o = 0; i < level.size(); i += 3, o += 4)
        {
            std::memcpy(bgra.data() + o, level.data() + i, 3);
            bgra[o + 3] = 255;
        }
        levels.push_back(std::move(bgra));
    }
    return with_levels(image, Fmt::A8R8G8B8, image.width, image.height, std::move(levels));
}

ImageData expand_a8(const ImageData &image)
{
    std::vector<std::vector<uint8_t>> levels;
    for (const auto &level : image.levels)
    {
        std::vector<uint8_t> la(level.size() * 2);
        for (size_t i = 0; i < level.size(); ++i)
        {
            la[2 * i] = 255;
            la[2 * i + 1] = level[i];
        }
        levels.push_back(std::move(la));
    }
    return with_levels(image, Fmt::A8L8, image.width, image.height, std::move(levels));
}

ImageData compress_image(const ImageData &image_in)
{
    if (image_in.format != Fmt::A8R8G8B8 && image_in.format != Fmt::X8R8G8B8 && image_in.format != Fmt::R8G8B8)
        return image_in;
    const ImageData image = image_in.format == Fmt::R8G8B8 ? convert_rgb24(image_in) : image_in;
    std::vector<Pixels> rgba_levels;
    uint32_t w = image.width, h = image.height;
    bool opaque = true;
    for (const auto &level : image.levels)
    {
        Pixels rgba = dxt::bgra_to_rgba(level.data(), level.size(), w, h);
        if (image.format == Fmt::X8R8G8B8)
            for (size_t i = 3; i < rgba.data.size(); i += 4)
                rgba.data[i] = 255;
        for (size_t i = 3; i < rgba.data.size() && opaque; i += 4)
            opaque = rgba.data[i] >= 255;
        rgba_levels.push_back(std::move(rgba));
        w = half(w);
        h = half(h);
    }
    Fmt fmt = opaque ? Fmt::DXT1 : Fmt::DXT5;
    std::vector<std::vector<uint8_t>> levels;
    for (const Pixels &lv : rgba_levels)
        levels.push_back(dxt::encode(lv, fmt));
    return with_levels(image, fmt, image.width, image.height, std::move(levels));
}

ImageData reduce_image(const ImageData &image_in, int count)
{
    ImageData image = image_in;
    std::vector<std::vector<uint8_t>> levels = image.levels;
    uint32_t w = image.width, h = image.height;
    for (int i = 0; i < count; ++i)
    {
        if (std::min(w, h) <= 4)
            break;
        if (levels.size() > 1)
            levels.erase(levels.begin());
        else if (is_block_compressed(image.format))
        {
            Pixels rgba = dxt::downscale(dxt::decode(levels[0].data(), levels[0].size(), w, h, image.format));
            Fmt fmt = image.format == Fmt::DXT3 ? Fmt::DXT5 : image.format;
            levels = {dxt::encode(rgba, fmt)};
            image = with_levels(image, fmt, w, h, levels);
        }
        else if (image.format == Fmt::A8R8G8B8 || image.format == Fmt::X8R8G8B8)
        {
            Pixels rgba = dxt::downscale(dxt::bgra_to_rgba(levels[0].data(), levels[0].size(), w, h));
            levels = {rgba_to_bgra(rgba)};
        }
        else
            break;
        w = half(w);
        h = half(h);
    }
    return with_levels(image, image.format, w, h, std::move(levels));
}

ImageData normal_map_to_dxn(const ImageData &image)
{
    if (image.format != Fmt::DXT5 && image.format != Fmt::A8R8G8B8 && image.format != Fmt::A8L8)
        return image;
    std::vector<std::vector<uint8_t>> levels;
    uint32_t w = image.width, h = image.height;
    for (const auto &level : image.levels)
    {
        if (image.format == Fmt::DXT5)
            levels.push_back(dxt::dxt5_normal_to_dxn(level.data(), level.size(), w, h));
        else if (image.format == Fmt::A8L8)
        {
            if (level.size() < static_cast<size_t>(w) * h * 2)
                throw ImageError(image.name + ": not enough A8L8 data");
            Pixels xy(w, h, 4);
            for (size_t t = 0; t < static_cast<size_t>(w) * h; ++t)
            {
                xy.data[4 * t] = level[2 * t + 1];
                xy.data[4 * t + 1] = level[2 * t];
                xy.data[4 * t + 2] = 0;
                xy.data[4 * t + 3] = 255;
            }
            levels.push_back(dxt::encode(xy, Fmt::DXN));
        }
        else
        {
            Pixels rgba = dxt::bgra_to_rgba(level.data(), level.size(), w, h);
            Pixels swapped(w, h, 4); // channels 3, 1, 2, 0: x from alpha, y from green
            for (size_t i = 0; i < rgba.data.size(); i += 4)
            {
                swapped.data[i] = rgba.data[i + 3];
                swapped.data[i + 1] = rgba.data[i + 1];
                swapped.data[i + 2] = rgba.data[i + 2];
                swapped.data[i + 3] = rgba.data[i];
            }
            levels.push_back(dxt::encode(swapped, Fmt::DXN));
        }
        w = half(w);
        h = half(h);
    }
    return with_levels(image, Fmt::DXN, image.width, image.height, std::move(levels));
}

ConsoleTexture build_console_texture(const ImageData &source, const TextureOptions &o)
{
    const int faces = source.faces;
    ImageData image = source;
    if (o.normal_map && faces == 1)
        image = normal_map_to_dxn(image);
    if (o.compress && compresses(image))
        image = each_face(image, compress_image);
    if (image.format == Fmt::R8G8B8)
        image = each_face(image, convert_rgb24);
    if (image.format == Fmt::A8)
        image = each_face(image, expand_a8);
    if (image.format == Fmt::X8R8G8B8)
        image.format = Fmt::A8R8G8B8;
    std::optional<Fmt> cfmt = to_console_format(image.format);
    if (!cfmt)
        throw ImageError(image.name + ": no console equivalent for " + fmt_name(image.format));
    const xenos::Format &fmt = *xenos::format(*cfmt);

    int drop = drop_count(image, o.max_size, o.drop_levels);
    if (drop)
        image = each_face(image, [drop](const ImageData &face) { return reduce_image(face, drop); });
    const uint32_t width = image.width, height = image.height;

    std::vector<std::vector<uint8_t>> levels{image.levels.at(0)};
    if (o.keep_mips && std::min(width, height) > 16)
    {
        uint32_t w = width, h = height;
        for (size_t i = 1; i < image.levels.size(); ++i)
        {
            w = half(w);
            h = half(h);
            // stop before levels smaller than one compression block
            if (std::min(w, h) < fmt.block || !(o.mip_tail || keeps_level(w, h)))
                break;
            levels.push_back(image.levels[i]);
        }
    }

    ConsoleTexture tex;
    tex.format = &fmt;
    tex.width = width;
    tex.height = height;
    tex.levels = static_cast<int>(levels.size());
    if (levels.size() > 1 || faces > 1)
    {
        tex.pixels = xenos::tile_mip_chain(levels, width, height, fmt, faces);
        tex.header = xenos::texture_header_mips(width, height, fmt, tex.levels, faces);
    }
    else
    {
        tex.pixels = xenos::tile_level(levels[0].data(), levels[0].size(), width, height, 0, fmt);
        tex.header = xenos::texture_header(width, height, fmt, 1);
    }
    tex.dropped_levels = drop;
    tex.faces = faces;
    tex.base_size = xenos::mip_chain_layout(width, height, fmt, 1, faces).base_size;
    return tex;
}

uint32_t console_texture_size(const ImageData &image, const TextureOptions &o, bool make_mips)
{
    Fmt f = image.format;
    if (o.normal_map && image.faces == 1 && (f == Fmt::DXT5 || f == Fmt::A8R8G8B8 || f == Fmt::A8L8))
        f = Fmt::DXN;
    if (o.compress && compresses(image) && (f == Fmt::A8R8G8B8 || f == Fmt::X8R8G8B8 || f == Fmt::R8G8B8))
        f = Fmt::DXT5; // upper bound, DXT1 when opaque
    if (f == Fmt::R8G8B8 || f == Fmt::X8R8G8B8)
        f = Fmt::A8R8G8B8;
    else if (f == Fmt::A8)
        f = Fmt::A8L8;
    std::optional<Fmt> cfmt = to_console_format(f);
    if (!cfmt)
        return 0;
    const xenos::Format &fmt = *xenos::format(*cfmt);
    int drop = drop_count(image, o.max_size, o.drop_levels);
    uint32_t width = std::max(drop < 32 ? image.width >> drop : 0u, 1u), height = std::max(drop < 32 ? image.height >> drop : 0u, 1u);
    int count = 1;
    int available = make_mips ? mip_count(width, height) : std::max(static_cast<int>(image.levels.size()) - drop, 1);
    if (o.keep_mips && std::min(width, height) > 16)
    {
        uint32_t w = width, h = height;
        for (int i = 0; i < available - 1; ++i)
        {
            w = half(w);
            h = half(h);
            if (std::min(w, h) < fmt.block || !(o.mip_tail || keeps_level(w, h)))
                break;
            ++count;
        }
    }
    return xenos::mip_chain_layout(width, height, fmt, count, image.faces).total;
}

std::optional<WithMips> with_mips(const uint8_t *pixels, size_t size, uint32_t width, uint32_t height, const xenos::Format &fmt,
                                  bool mip_tail)
{
    if (std::min(width, height) <= 16)
        return std::nullopt;
    std::vector<uint8_t> top = xenos::untile_level(pixels, size, width, height, 0, fmt);
    Pixels texels;
    const bool block = is_block_compressed(fmt.name);
    if (block)
        texels = dxt::decode(top.data(), top.size(), width, height, fmt.name);
    else
    {
        uint32_t channels = fmt.name == Fmt::A8R8G8B8 ? 4 : fmt.name == Fmt::A8L8 ? 2 : fmt.name == Fmt::L8 ? 1 : 0;
        if (!channels)
            return std::nullopt;
        texels = Pixels(width, height, channels);
        if (top.size() < texels.data.size())
            throw ImageError("not enough texture data for level");
        std::copy(top.begin(), top.begin() + texels.data.size(), texels.data.begin());
    }
    std::vector<std::vector<uint8_t>> levels{top};
    uint32_t w = width, h = height;
    while (std::max(w, h) > 1)
    {
        w = half(w);
        h = half(h);
        if (std::min(w, h) < fmt.block || !(mip_tail || keeps_level(w, h)))
            break;
        texels = dxt::downscale(texels);
        levels.push_back(block ? dxt::encode(texels, fmt.name) : rgba_bytes(texels));
    }
    if (levels.size() < 2)
        return std::nullopt;
    WithMips out;
    out.levels = static_cast<int>(levels.size());
    out.pixels = xenos::tile_mip_chain(levels, width, height, fmt);
    return out;
}
} // namespace t4ff
