#include "convert/loadscreen.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_set>

#include "audio/audio.h"
#include "convert/assets.h"
#include "core/fastfile.h"
#include "core/platforms.h"
#include "core/process.h"
#include "core/pyre.h"
#include "core/pystr.h"

namespace t4ff
{
namespace fs = std::filesystem;
using pyre::Regex;

namespace
{
#include "convert/loadscreen_tables.inc"

constexpr uint32_t PREVIEW_WIDTH = 512, PREVIEW_HEIGHT = 288;
constexpr uint32_t PREVIEW_DDS_WIDTH = 512, PREVIEW_DDS_HEIGHT = 256;

std::wstring wide(const std::string &s)
{
    return std::wstring(s.begin(), s.end());
}

// FFmpeg's output (nullopt when it is not there or fails)
std::optional<std::vector<uint8_t>> run_ffmpeg(const std::vector<std::wstring> &args, const std::vector<uint8_t> *input = nullptr)
{
    fs::path exe = ffmpeg_exe();
    if (exe.empty())
        return std::nullopt;
    std::vector<std::wstring> cmd{exe.wstring(), L"-hide_banner", L"-loglevel", L"error"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    ProcessResult r = run_process(cmd, 0, input);
    if (r.exit_code != 0)
        return std::nullopt;
    return std::move(r.out);
}

Pixels resize_bilinear(const Pixels &rgba, uint32_t width, uint32_t height)
{
    uint32_t h = rgba.height, w = rgba.width;
    std::vector<double> ys(height), xs(width);
    for (uint32_t i = 0; i < height; ++i)
        ys[i] = std::clamp((i + 0.5) * h / height - 0.5, 0.0, static_cast<double>(h - 1));
    for (uint32_t i = 0; i < width; ++i)
        xs[i] = std::clamp((i + 0.5) * w / width - 0.5, 0.0, static_cast<double>(w - 1));
    Pixels out(width, height, 4);
    for (uint32_t y = 0; y < height; ++y)
    {
        uint32_t y0 = static_cast<uint32_t>(std::floor(ys[y])), y1 = std::min(y0 + 1, h - 1);
        double fy = ys[y] - y0;
        for (uint32_t x = 0; x < width; ++x)
        {
            uint32_t x0 = static_cast<uint32_t>(std::floor(xs[x])), x1 = std::min(x0 + 1, w - 1);
            double fx = xs[x] - x0;
            for (int c = 0; c < 4; ++c)
            {
                double top = static_cast<float>(rgba.at(x0, y0)[c]) * (1 - fx) + static_cast<float>(rgba.at(x1, y0)[c]) * fx;
                double bottom = static_cast<float>(rgba.at(x0, y1)[c]) * (1 - fx) + static_cast<float>(rgba.at(x1, y1)[c]) * fx;
                double v = std::clamp(top * (1 - fy) + bottom * fy + 0.5, 0.0, 255.0);
                out.at(x, y)[c] = static_cast<uint8_t>(v);
            }
        }
    }
    return out;
}

const Platform &console()
{
    return x360();
}

std::vector<std::pair<std::string, Node *>> assets_of(const Platform &p, Zone &zone, const char *rec)
{
    std::vector<std::pair<std::string, Node *>> out;
    zone.walk([&](Node *n) {
        if (n->is_record(rec) && n->origin == Origin::Asset)
            out.emplace_back(asset_name(p, *n), n);
    });
    return out;
}

// the image of $levelbriefing (named loadscreen_*)
Node *picture_image(const Platform &p, Zone &zone)
{
    for (auto &[name, n] : assets_of(p, zone, "GfxImage"))
        if (!name.empty() && py::starts_with(py::lower(name), "loadscreen_"))
            return n;
    return nullptr;
}

Node *pixels_of(Node *image)
{
    for (Node *c : image->children)
        if (c->delayed || ((c->origin == Origin::Member || c->origin == Origin::PtrArray) && c->origin_record && *c->origin_record == "GfxImage" &&
                           c->origin_field && *c->origin_field == "pixels"))
            return c;
    return nullptr;
}

Node *string_field(const Platform &p, Node *node, const char *rec, const char *field)
{
    Ptr *ptr = node->relocs.get(p.record(rec).field(field)->offset);
    Node *target = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
    return target && target->string ? target : nullptr;
}

void rename(Node *node, const std::string &text)
{
    if (!node)
        return;
    std::vector<uint8_t> data(text.begin(), text.end());
    data.push_back(0);
    node->count = static_cast<uint32_t>(data.size());
    node->segments = {Segment{node->type, node->count, node->count, false}};
    node->data.assign(std::move(data));
}

// CoD Xenon's load zones among the console fastfiles, the map's own first
std::vector<fs::path> template_files(const std::vector<fs::path> &files, const std::string &map_name)
{
    std::vector<fs::path> loads;
    for (const fs::path &f : files)
        if (py::ends_with(py::lower(f.string()), "_load.ff"))
            loads.push_back(f);
    std::string own = py::lower(map_name + "_load.ff");
    std::stable_sort(loads.begin(), loads.end(), [&](const fs::path &a, const fs::path &b) {
        bool a_own = py::lower(a.filename().string()) == own, b_own = py::lower(b.filename().string()) == own;
        return a_own && !b_own;
    });
    return loads;
}

// a load zone made like CoD Xenon's: $levelbriefing with a loadscreen_* image
std::unique_ptr<Zone> read_template(const fs::path &path)
{
    const Platform &p = console();
    std::unique_ptr<Zone> zone;
    try
    {
        FastFile ff = read_fastfile(path);
        if (!ff.big_endian)
            return nullptr;
        zone = read_zone(p, std::move(ff.zone));
    }
    catch (const std::exception &)
    {
        return nullptr;
    }
    bool briefing = false;
    for (auto &[name, n] : assets_of(p, *zone, "Material"))
        briefing = briefing || name == "$levelbriefing";
    if (!briefing || !picture_image(p, *zone))
        return nullptr;
    return zone;
}

std::pair<uint32_t, uint32_t> image_size(const Platform &p, const Node *image)
{
    const Record &rec = p.record("GfxImage");
    auto get = [&](const char *f) {
        const uint8_t *d = image->data.data() + rec.field(f)->offset;
        return static_cast<uint32_t>(d[0] << 8 | d[1]);
    };
    return {get("width"), get("height")};
}

std::vector<uint8_t> opaque_bgra(const Pixels &rgba)
{
    std::vector<uint8_t> bgra(rgba.data.size());
    for (size_t i = 0; i + 3 < rgba.data.size(); i += 4)
    {
        bgra[i] = rgba.data[i + 2];
        bgra[i + 1] = rgba.data[i + 1];
        bgra[i + 2] = rgba.data[i];
        bgra[i + 3] = 255;
    }
    return bgra;
}

// put the picture (at the template's size) in the load zone
void set_picture(const Platform &p, Zone &zone, const Pixels &rgba)
{
    Node *image = picture_image(p, zone);
    Node *pixels = pixels_of(image);
    std::optional<ImageData> tmpl = decode_console_image(p, *image, "template");
    if (!tmpl || !pixels)
        throw LoadScreenError("the template's loading screen image is not a 2D texture");
    uint32_t width = tmpl->width, height = tmpl->height;
    ImageData img;
    img.name = "loadscreen";
    img.format = Fmt::A8R8G8B8;
    img.width = width;
    img.height = height;
    img.levels = {opaque_bgra(rgba)};
    TextureOptions o;
    o.keep_mips = tmpl->levels.size() > 1;
    ConsoleTexture tex = build_console_texture(img, o);
    if (tex.format->name != tmpl->format || tex.pixels.size() != pixels->data.size())
        throw LoadScreenError(std::string("the template's loading screen image is ") + fmt_name(tmpl->format) + " " + std::to_string(width) + "x" +
                              std::to_string(height) + ", which this picture cannot replace");
    pixels->data.assign(std::move(tex.pixels));
}

std::vector<uint8_t> build_load_zone(Zone &tmpl, const std::string &map_name, const std::optional<Pixels> &rgba)
{
    const Platform &p = console();
    Node *image = picture_image(p, tmpl);
    if (rgba)
        set_picture(p, tmpl, *rgba);
    // not loadscreen_<map>: the map's own zone often has an image of that name
    rename(string_field(p, image, "GfxImage", "name"), "loadscreen_" + map_name + "_codxe");
    for (auto &[n, raw] : assets_of(p, tmpl, "RawFile"))
        rename(string_field(p, raw, "RawFile", "name"), map_name + "_load");
    return write_zone(p, tmpl);
}

// the map's own PC loading screen: the loadscreen_* image of its PC load zone (or named after the map)
std::optional<ImageData> pc_picture(const IwdLibrary &map_files, const fs::path &pc_load_zone, const std::string &map_name)
{
    std::vector<std::string> names{"loadscreen_" + map_name};
    if (!pc_load_zone.empty())
    {
        try
        {
            FastFile ff = read_fastfile(pc_load_zone);
            auto zone = read_zone(pc(), std::move(ff.zone));
            std::vector<std::string> found;
            for (auto &[n, node] : assets_of(pc(), *zone, "GfxImage"))
                if (!n.empty() && py::contains(py::lower(n), "loadscreen"))
                    found.push_back(n.substr(std::min(n.find_first_not_of(','), n.size())));
            found.insert(found.end(), names.begin(), names.end());
            names = found;
        }
        catch (const std::exception &)
        {
        }
    }
    std::unordered_set<std::string> seen;
    for (const std::string &name : names)
    {
        if (!seen.insert(name).second)
            continue;
        std::optional<ImageData> picture;
        try
        {
            picture = map_files.image(name);
        }
        catch (const ImageError &)
        {
        }
        if (picture)
            return picture;
    }
    return std::nullopt;
}

const char *const *glyph(char c)
{
    for (const auto &g : FONT)
        if (g.c == c)
            return g.rows;
    return glyph('?');
}

void write_bytes(const fs::path &path, const std::vector<uint8_t> &data)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f)
        throw std::runtime_error("cannot write " + path.string());
}

void write_text(const fs::path &path, const std::string &text)
{
    write_bytes(path, std::vector<uint8_t>(text.begin(), text.end()));
}

// a text file as Python's open(path, "r") reads it: universal newlines
std::string read_universal(const fs::path &path)
{
    std::vector<uint8_t> raw = read_file(path);
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        char c = static_cast<char>(raw[i]);
        if (c == '\r')
        {
            out += '\n';
            if (i + 1 < raw.size() && raw[i + 1] == '\n')
                ++i;
        }
        else
            out += c;
    }
    return out;
}

// str.splitlines() of a text with universal newlines
std::vector<std::string> splitlines(const std::string &text)
{
    std::vector<std::string> lines;
    std::string line;
    for (size_t i = 0; i < text.size(); ++i)
    {
        uint8_t c = static_cast<uint8_t>(text[i]);
        if (c == '\n' || c == '\r' || c == 0x0b || c == 0x0c || c == 0x1c || c == 0x1d || c == 0x1e || c == 0x85)
        {
            lines.push_back(line);
            line.clear();
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
        }
        else
            line += static_cast<char>(c);
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

std::string ascii(const std::string &text)
{
    std::string out;
    for (char ch : text)
    {
        uint8_t c = static_cast<uint8_t>(ch);
        if (c < 128)
            out += ch;
        else
            out += ASCII_OF[c];
    }
    return out;
}

// a JSON string as Python's json.dumps writes it (ensure_ascii)
std::string json_string(const std::string &s)
{
    std::string out = "\"";
    for (char ch : s)
    {
        uint8_t c = static_cast<uint8_t>(ch);
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (c < 0x20 || c >= 0x7f)
            {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            }
            else
                out += ch;
        }
    }
    return out + "\"";
}
} // namespace

// -- pictures

Pixels rgba_of(const ImageData &image)
{
    uint32_t w = image.width, h = image.height;
    const std::vector<uint8_t> &data = image.levels.at(0);
    switch (image.format)
    {
    case Fmt::DXT1:
    case Fmt::DXT3:
    case Fmt::DXT5:
        return dxt::decode(data.data(), data.size(), w, h, image.format);
    case Fmt::A8R8G8B8:
    case Fmt::X8R8G8B8: {
        Pixels rgba = dxt::bgra_to_rgba(data.data(), data.size(), w, h);
        if (image.format == Fmt::X8R8G8B8)
            for (size_t i = 3; i < rgba.data.size(); i += 4)
                rgba.data[i] = 255;
        return rgba;
    }
    case Fmt::R8G8B8:
        return rgba_of(convert_rgb24(image));
    case Fmt::L8:
    case Fmt::A8L8: {
        size_t channels = w && h ? data.size() / (static_cast<size_t>(w) * h) : 1;
        Pixels rgba(w, h, 4);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
        {
            uint8_t l = data[i * channels];
            rgba.data[4 * i] = rgba.data[4 * i + 1] = rgba.data[4 * i + 2] = l;
            rgba.data[4 * i + 3] = channels > 1 ? data[i * channels + 1] : 255;
        }
        return rgba;
    }
    default:
        throw LoadScreenError(image.name + ": " + fmt_name(image.format) + " pictures are not supported");
    }
}

Pixels resize(const Pixels &rgba, uint32_t width, uint32_t height)
{
    if (rgba.width == width && rgba.height == height)
        return rgba;
    std::wstring size = std::to_wstring(rgba.width) + L"x" + std::to_wstring(rgba.height);
    std::wstring scale = L"scale=" + std::to_wstring(width) + L":" + std::to_wstring(height) + L":flags=lanczos";
    auto out = run_ffmpeg({L"-f", L"rawvideo", L"-pix_fmt", L"rgba", L"-s", size, L"-i", L"-", L"-vf", scale, L"-f", L"rawvideo", L"-pix_fmt", L"rgba", L"-"},
                          &rgba.data);
    if (out && out->size() == static_cast<size_t>(width) * height * 4)
    {
        Pixels p(width, height, 4);
        p.data = std::move(*out);
        return p;
    }
    return resize_bilinear(rgba, width, height);
}

Pixels read_picture(const fs::path &path, uint32_t width, uint32_t height)
{
    if (py::ends_with(py::lower(path.string()), ".iwi"))
    {
        std::vector<uint8_t> data = read_file(path);
        return resize(rgba_of(parse_iwi(path.filename().string(), data.data(), data.size())), width, height);
    }
    if (!fs::is_regular_file(path))
        throw LoadScreenError("loading screen picture not found: " + path.string());
    std::wstring scale = L"scale=" + std::to_wstring(width) + L":" + std::to_wstring(height) + L":flags=lanczos";
    auto out = run_ffmpeg({L"-i", path.wstring(), L"-frames:v", L"1", L"-vf", scale, L"-f", L"rawvideo", L"-pix_fmt", L"rgba", L"-"});
    size_t size = static_cast<size_t>(width) * height * 4;
    if (!out || out->size() < size)
        throw LoadScreenError("cannot read the picture " + path.string() + " (FFmpeg reads .png, .jpg, .bmp, .tga, .dds and .webp)");
    Pixels p(width, height, 4);
    p.data.assign(out->begin(), out->begin() + static_cast<std::ptrdiff_t>(size));
    return p;
}

// -- names

std::string pretty_map_name(const std::string &map_name)
{
    std::vector<std::string> words;
    for (const std::string &word : py::split(py::replace(map_name, "_", " ")))
        words.push_back(py::upper(word.substr(0, 1)) + word.substr(1));
    return py::join(words, " ");
}

std::string map_title(const IwdLibrary *map_files, const std::string &map_name, const std::vector<std::pair<std::string, std::string>> *localized)
{
    static const Regex block_re(R"(\{([^}]*)\})");
    static const Regex map_re(R"re(\bmap\s+"([^"]+)")re", pyre::I);
    static const Regex longname_re(R"re(longname\s+"([^"]+)")re", pyre::I);
    static const Regex key_re(R"(@?[A-Z0-9_]+)");
    static const Regex color_re(R"(\^.)");
    static const Regex bare_re(R"([A-Z0-9_]+)");
    std::vector<std::string> arenas;
    if (map_files)
        for (const std::string &n : map_files->names(""))
            if (py::ends_with(n, ".arena"))
                arenas.push_back(n);
    for (const std::string &rel : arenas)
    {
        auto data = map_files->read(rel);
        std::string text = data ? std::string(data->begin(), data->end()) : "";
        std::vector<std::string> blocks;
        block_re.for_each(text, [&](const pyre::Match &m) { blocks.push_back(m.str(1)); });
        if (blocks.empty())
            blocks.push_back(text);
        for (const std::string &block : blocks)
        {
            auto entry_map = map_re.search(block);
            auto found = longname_re.search(block);
            if (!found || (entry_map && py::lower(entry_map->group(1)) != py::lower(map_name)))
                continue;
            std::string title(py::strip(found->group(1)));
            if (key_re.fullmatch(title))
            {
                std::string key = title.substr(title[0] == '@' ? 1 : 0);
                std::string value;
                if (localized)
                    for (const auto &[k, v] : *localized)
                        if (k == key)
                            value = v;
                title = value;
            }
            title = std::string(py::strip(color_re.sub(title, std::string_view(""))));
            if (!title.empty() && !bare_re.fullmatch(title) && py::lower(title) != py::lower(map_name))
                return title;
        }
    }
    return pretty_map_name(map_name);
}

Pixels title_card(const std::string &title, uint32_t width, uint32_t height)
{
    // a faint red band in the middle, in float32 as the numpy picture
    std::vector<float> rgba(static_cast<size_t>(width) * height * 4);
    double step = height > 1 ? 1.0 / (height - 1) : 0.0;
    for (uint32_t y = 0; y < height; ++y)
    {
        double v = height > 1 && y == height - 1 ? 1.0 : y * step;
        float red = static_cast<float>(10 + 22 * (1 - std::abs(v - 0.5) * 2));
        for (uint32_t x = 0; x < width; ++x)
        {
            float *px = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            px[0] = red;
            px[1] = 8;
            px[2] = 8;
            px[3] = 255;
        }
    }
    auto to_pixels = [&]() {
        Pixels out(width, height, 4);
        for (size_t i = 0; i < rgba.size(); ++i)
            out.data[i] = static_cast<uint8_t>(std::clamp(rgba[i], 0.0f, 255.0f));
        return out;
    };
    std::vector<std::string> lines;
    std::string line;
    for (const std::string &word : py::split(py::upper(title))) // at most 16 characters a line
    {
        if (!line.empty() && line.size() + 1 + word.size() > 16)
        {
            lines.push_back(line);
            line = word;
        }
        else
            line = std::string(py::strip(line + " " + word));
    }
    if (!line.empty())
        lines.push_back(line);
    if (lines.size() > 3)
        lines.resize(3);
    if (lines.empty())
        return to_pixels();
    // characters are 6x9 cells of 5x7 glyphs; drawn at 4x the final scale and scaled down
    size_t longest = 0;
    for (const std::string &l : lines)
        longest = std::max(longest, l.size());
    int scale = std::max(1, std::min(static_cast<int>(width * 0.8 / (6 * longest)), static_cast<int>(height * 0.6 / (9 * lines.size()))));
    const int big = 4;
    uint32_t mask_h = static_cast<uint32_t>(9 * lines.size() * big), mask_w = static_cast<uint32_t>(6 * longest * big);
    Pixels mask(mask_w, mask_h, 4);
    for (size_t row = 0; row < lines.size(); ++row)
    {
        const std::string &text = lines[row];
        size_t x0 = (longest - text.size()) * 3 * big;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char *const *g = glyph(text[i]);
            size_t top = row * 9 * big + big, left = x0 + i * 6 * big;
            for (int gy = 0; gy < 7 * big; ++gy)
                for (int gx = 0; gx < 5 * big; ++gx)
                {
                    uint8_t v = g[gy / big][gx / big] == '#' ? 255 : 0;
                    uint8_t *px = mask.at(static_cast<uint32_t>(left + gx), static_cast<uint32_t>(top + gy));
                    px[0] = px[1] = px[2] = px[3] = v;
                }
        }
    }
    uint32_t mh = mask_h * scale / big, mw = mask_w * scale / big;
    Pixels small_px = resize(mask, mw, mh);
    std::vector<float> small(static_cast<size_t>(mw) * mh);
    for (size_t i = 0; i < small.size(); ++i)
        small[i] = static_cast<float>(small_px.data[4 * i]) / 255.0f;
    int64_t top = (static_cast<int64_t>(height) - mh) / 2, left = (static_cast<int64_t>(width) - mw) / 2;
    if (static_cast<int64_t>(height) - mh < 0 && (static_cast<int64_t>(height) - mh) % 2)
        --top; // Python's floor division
    if (static_cast<int64_t>(width) - mw < 0 && (static_cast<int64_t>(width) - mw) % 2)
        --left;
    struct Layer
    {
        int d;
        float color[3];
        float weight;
    };
    const Layer layers[] = {{scale / 2 + 1, {0, 0, 0}, 0.8f}, {0, {178, 16, 16}, 1.0f}}; // the shadow, then the letters
    for (const Layer &layer : layers)
    {
        int64_t y_start = top + layer.d, x_start = left + layer.d;
        int64_t y_end = std::min<int64_t>(y_start + mh, height), x_end = std::min<int64_t>(x_start + mw, width);
        for (int64_t y = std::max<int64_t>(y_start, 0); y < y_end; ++y)
            for (int64_t x = std::max<int64_t>(x_start, 0); x < x_end; ++x)
            {
                float alpha = small[static_cast<size_t>(y - y_start) * mw + static_cast<size_t>(x - x_start)] * layer.weight;
                float *px = &rgba[(static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4];
                for (int c = 0; c < 3; ++c)
                {
                    float kept = px[c] * (1.0f - alpha);
                    float added = layer.color[c] * alpha;
                    px[c] = kept + added;
                }
            }
    }
    return to_pixels();
}

// -- the load zone

bool write_load_zone(const std::string &map_name, const fs::path &out_dir, const std::vector<fs::path> &console_files, const IwdLibrary &map_files,
                     const fs::path &pc_load_zone, const fs::path &picture_path, int jobs, const Log &log, const std::string &title_in)
{
    const Platform &p = console();
    fs::path target = out_dir / (map_name + "_load.ff");
    for (const fs::path &path : template_files(console_files, map_name))
    {
        std::unique_ptr<Zone> tmpl = read_template(path);
        if (!tmpl)
            continue;
        bool own = py::lower(path.filename().string()) == py::lower(map_name + "_load.ff");
        auto [width, height] = image_size(p, picture_image(p, *tmpl));
        std::optional<Pixels> rgba;
        std::string source;
        if (!picture_path.empty())
        {
            rgba = read_picture(picture_path, width, height);
            source = picture_path.string();
        }
        else if (own)
        {
            fs::copy_file(path, target, fs::copy_options::overwrite_existing);
            if (log)
                log("loading screen: CoD Xenon's own for this map (" + path.string() + ")");
            return true;
        }
        else
        {
            std::optional<ImageData> picture = pc_picture(map_files, pc_load_zone, map_name);
            if (picture)
            {
                rgba = resize(rgba_of(*picture), width, height);
                source = "the map's own " + picture->name;
            }
            else
            {
                std::string title = title_in.empty() ? map_title(&map_files, map_name, nullptr) : title_in;
                rgba = title_card(title, width, height);
                source = "a title card \"" + title + "\" (the map has no loading screen picture: give one with --loading-image)";
            }
        }
        std::vector<uint8_t> out = build_load_zone(*tmpl, map_name, rgba);
        write_fastfile(target, true, out, 9, jobs);
        if (log)
            log("loading screen: " + source + ", " + std::to_string(width) + "x" + std::to_string(height) + " (made like CoD Xenon's, from " +
                path.filename().string() + ")");
        return true;
    }
    return false;
}

std::optional<Pixels> load_zone_picture(const fs::path &path)
{
    std::unique_ptr<Zone> zone = read_template(path);
    if (!zone)
        return std::nullopt;
    std::optional<ImageData> image = decode_console_image(console(), *picture_image(console(), *zone), "loadscreen");
    if (!image)
        return std::nullopt;
    return rgba_of(*image);
}

std::vector<uint8_t> preview_file(const Pixels &rgba)
{
    Pixels picture = resize(rgba, PREVIEW_WIDTH, PREVIEW_HEIGHT);
    ImageData img;
    img.name = "codxe_map_preview";
    img.format = Fmt::A8R8G8B8;
    img.width = PREVIEW_WIDTH;
    img.height = PREVIEW_HEIGHT;
    img.levels = {opaque_bgra(picture)};
    TextureOptions o;
    o.keep_mips = false;
    ConsoleTexture tex = build_console_texture(img, o);
    std::vector<uint8_t> out{'C', 'X', 'P', 'V'};
    auto u16 = [&](uint32_t v) {
        out.push_back(uint8_t(v >> 8));
        out.push_back(uint8_t(v));
    };
    auto u32 = [&](uint32_t v) {
        for (int s = 24; s >= 0; s -= 8)
            out.push_back(uint8_t(v >> s));
    };
    u16(1);
    u16(tex.width);
    u16(tex.height);
    u16(0);
    u32(tex.format->gpu);
    u32(static_cast<uint32_t>(tex.pixels.size()));
    out.insert(out.end(), tex.pixels.begin(), tex.pixels.end());
    return out;
}

bool write_preview(const fs::path &map_dir, const std::string &map_name)
{
    std::optional<Pixels> rgba = load_zone_picture(map_dir / (map_name + "_load.ff"));
    if (!rgba)
        return false;
    write_bytes(map_dir / "preview.bin", preview_file(*rgba));
    return true;
}

// -- CoD Xe's own custom maps list

std::string map_json(const std::string &description)
{
    std::vector<std::string> lines;
    for (const std::string &l : splitlines(description))
        lines.emplace_back(py::strip(l));
    std::string name;
    size_t at = 0;
    for (size_t i = 0; i < lines.size(); ++i)
        if (!lines[i].empty())
        {
            name = lines[i];
            at = i;
            break;
        }
    std::vector<std::string> rest;
    if (!name.empty())
        for (size_t i = at + 1; i < lines.size(); ++i)
            if (!lines[i].empty())
                rest.push_back(lines[i]);
    return "{\n  \"version\": 1,\n  \"name\": " + json_string(ascii(name)) + ",\n  \"description\": " + json_string(ascii(py::join(rest, " "))) + "\n}\n";
}

std::vector<uint8_t> preview_dds(const Pixels &rgba)
{
    const uint32_t width = PREVIEW_DDS_WIDTH, height = PREVIEW_DDS_HEIGHT;
    uint32_t h = rgba.height, w = rgba.width;
    double scale = std::max(static_cast<double>(width) / w, static_cast<double>(height) / h);
    uint32_t sw = std::max<uint32_t>(width, static_cast<uint32_t>(std::nearbyint(w * scale)));
    uint32_t sh = std::max<uint32_t>(height, static_cast<uint32_t>(std::nearbyint(h * scale)));
    Pixels picture = resize(rgba, sw, sh);
    uint32_t x = (sw - width) / 2, y = (sh - height) / 2;
    Pixels cut(width, height, 4);
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(cut.at(0, row), picture.at(x, y + row), static_cast<size_t>(width) * 4);
    for (size_t i = 3; i < cut.data.size(); i += 4)
        cut.data[i] = 255;
    std::vector<uint8_t> pixels = dxt::encode(cut, Fmt::DXT1);
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t v) {
        for (int s = 0; s < 32; s += 8)
            out.push_back(uint8_t(v >> s));
    };
    out.insert(out.end(), {'D', 'D', 'S', ' '});
    u32(124);
    u32(0x1 | 0x2 | 0x4 | 0x1000 | 0x80000); // caps, height, width, pixel format, linear size
    u32(height);
    u32(width);
    u32(static_cast<uint32_t>(pixels.size()));
    u32(0);
    u32(0);
    out.insert(out.end(), 44, 0);
    u32(32); // the pixel format: a four CC
    u32(0x4);
    out.insert(out.end(), {'D', 'X', 'T', '1'});
    for (int i = 0; i < 5; ++i)
        u32(0);
    u32(0x1000); // caps: a texture
    for (int i = 0; i < 4; ++i)
        u32(0);
    out.insert(out.end(), pixels.begin(), pixels.end());
    return out;
}

std::vector<std::string> write_map_info(const fs::path &map_dir, const std::string &map_name, bool metadata, bool picture)
{
    std::vector<std::string> written;
    fs::path description = map_dir / "description.txt";
    if (metadata && fs::exists(description))
    {
        std::string text = py::replace(map_json(read_universal(description)), "\n", "\r\n");
        write_text(map_dir / "map.json", text);
        written.push_back("map.json");
    }
    if (picture)
    {
        std::optional<Pixels> rgba = load_zone_picture(map_dir / (map_name + "_load.ff"));
        if (rgba)
        {
            write_bytes(map_dir / "preview.dds", preview_dds(*rgba));
            written.push_back("preview.dds");
        }
    }
    return written;
}
} // namespace t4ff
