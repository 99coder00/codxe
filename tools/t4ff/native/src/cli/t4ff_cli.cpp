#include "cli/t4ff_cli.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

#include <windows.h>

#include "app/convert.h"
#include "app/deps.h"
#include "app/usermap.h"
#include "audio/audio.h"
#include "convert/loadscreen.h"
#include "convert/menu_editor.h"
#include "convert/menus.h"
#include "convert/usermaps_menu.h"
#include "core/fastfile.h"
#include "core/platforms.h"
#include "core/process.h"
#include "core/progress.h"
#include "core/pystr.h"
#include "core/zone.h"

namespace t4ff::cli
{
namespace
{
namespace fs = std::filesystem;

// a UTF-8 text as a path
fs::path from_utf8(const std::string &s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string text(const fs::path &p)
{
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string narrow(const std::wstring &w)
{
    return text(fs::path(w));
}

// a command line text in latin-1 (the Python writes names in latin-1): other characters are "?"
std::string latin1(const std::wstring &w)
{
    std::string out;
    for (wchar_t c : w)
        out += c < 256 ? static_cast<char>(static_cast<uint8_t>(c)) : '?';
    return out;
}

void say(const std::string &line)
{
    printf("%s\n", line.c_str());
    fflush(stdout);
}

// -- argparse, as far as t4ff's command line uses it

struct ArgError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Option
{
    std::vector<std::string> flags; // "-o", "--output"; a pair "--x" / "--no-x" for a boolean
    std::string dest;
    enum Kind
    {
        Flag,
        Value,
        Append,
        Bool,
    } kind = Flag;
    std::string metavar, help;
    bool required = false;
};

struct Parsed
{
    std::map<std::string, std::vector<std::wstring>> values;
    std::map<std::string, bool> flags;
    std::vector<std::wstring> positionals;

    bool flag(const std::string &dest, bool fallback = false) const
    {
        auto it = flags.find(dest);
        return it == flags.end() ? fallback : it->second;
    }
    std::optional<std::wstring> value(const std::string &dest) const
    {
        auto it = values.find(dest);
        if (it == values.end() || it->second.empty())
            return std::nullopt;
        return it->second.back();
    }
    std::vector<std::wstring> all(const std::string &dest) const
    {
        auto it = values.find(dest);
        return it == values.end() ? std::vector<std::wstring>() : it->second;
    }
};

struct Command
{
    std::string prog, usage, description;
    std::vector<Option> options;
    std::vector<std::string> positionals; // their names; the last can take many ("...")
    bool many = false;                    // the last positional takes one or more
};

bool looks_negative(const std::wstring &a)
{
    if (a.size() < 2 || a[0] != L'-')
        return false;
    wchar_t *end = nullptr;
    wcstod(a.c_str(), &end);
    return end && *end == 0;
}

void print_help(const Command &c)
{
    printf("usage: %s\n\n%s\n\n", c.usage.c_str(), c.description.c_str());
    if (!c.positionals.empty())
    {
        printf("positional arguments:\n");
        for (const std::string &p : c.positionals)
            printf("  %s\n", p.c_str());
        printf("\n");
    }
    printf("options:\n  -h, --help            show this help message and exit\n");
    for (const Option &o : c.options)
    {
        if (o.help == "SUPPRESS")
            continue;
        std::string names;
        for (const std::string &f : o.flags)
            names += (names.empty() ? "" : ", ") + f + (o.kind == Option::Value || o.kind == Option::Append ? " " + o.metavar : "");
        printf("  %s\n", names.c_str());
        if (!o.help.empty())
            printf("                        %s\n", o.help.c_str());
    }
}

// parses args for c: argparse's rules (prefixes of long options, --opt=value, -ovalue)
Parsed parse(const Command &c, const std::vector<std::wstring> &args)
{
    Parsed out;
    std::set<std::string> seen;
    bool only_positionals = false;
    auto find = [&](const std::string &name) -> std::pair<const Option *, std::string> {
        for (const Option &o : c.options)
            for (const std::string &f : o.flags)
                if (f == name)
                    return {&o, f};
        if (name.rfind("--", 0) == 0)
        {
            std::vector<std::pair<const Option *, std::string>> hits;
            for (const Option &o : c.options)
                for (const std::string &f : o.flags)
                    if (f.rfind(name, 0) == 0)
                        hits.emplace_back(&o, f);
            if ("--help" == name || std::string("--help").rfind(name, 0) == 0)
                hits.emplace_back(nullptr, "--help");
            if (hits.size() > 1)
            {
                std::string list;
                for (const auto &[o, f] : hits)
                    list += (list.empty() ? "" : ", ") + f;
                throw ArgError("ambiguous option: " + name + " could match " + list);
            }
            if (hits.size() == 1)
                return hits[0];
        }
        return {nullptr, ""};
    };
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::wstring &arg = args[i];
        if (only_positionals || arg.size() < 2 || arg[0] != L'-' || looks_negative(arg))
        {
            out.positionals.push_back(arg);
            continue;
        }
        if (arg == L"--")
        {
            only_positionals = true;
            continue;
        }
        std::string a = narrow(arg);
        std::string name = a;
        std::optional<std::string> explicit_value;
        if (a.rfind("--", 0) == 0)
        {
            size_t eq = a.find('=');
            if (eq != std::string::npos)
            {
                name = a.substr(0, eq);
                explicit_value = a.substr(eq + 1);
            }
        }
        else if (a.size() > 2 && a != "-h")
        {
            name = a.substr(0, 2); // -oVALUE
            explicit_value = a.substr(2);
        }
        if (name == "-h" || name == "--help")
        {
            print_help(c);
            std::exit(0);
        }
        auto [o, flag] = find(name);
        if (!o && flag == "--help")
        {
            print_help(c);
            std::exit(0);
        }
        if (!o)
            throw ArgError("unrecognized arguments: " + a);
        seen.insert(o->dest);
        switch (o->kind)
        {
        case Option::Flag:
            if (explicit_value)
                throw ArgError("argument " + flag + ": ignored explicit argument '" + *explicit_value + "'");
            out.flags[o->dest] = true;
            break;
        case Option::Bool:
            if (explicit_value)
                throw ArgError("argument " + flag + ": ignored explicit argument '" + *explicit_value + "'");
            out.flags[o->dest] = flag.rfind("--no-", 0) != 0;
            break;
        case Option::Value:
        case Option::Append: {
            std::wstring v;
            if (explicit_value)
                v = from_utf8(*explicit_value).wstring();
            else
            {
                if (i + 1 >= args.size() || (args[i + 1].size() > 1 && args[i + 1][0] == L'-' && !looks_negative(args[i + 1])))
                {
                    std::string names;
                    for (const std::string &f : o->flags)
                        names += (names.empty() ? "" : "/") + f;
                    throw ArgError("argument " + names + ": expected one argument");
                }
                v = args[++i];
            }
            if (o->kind == Option::Value)
                out.values[o->dest] = {v};
            else
                out.values[o->dest].push_back(v);
            break;
        }
        }
    }
    std::vector<std::string> missing;
    for (const Option &o : c.options)
        if (o.required && !seen.count(o.dest))
        {
            std::string names;
            for (const std::string &f : o.flags)
                names += (names.empty() ? "" : "/") + f;
            missing.push_back(names);
        }
    size_t wanted = c.positionals.size();
    if (out.positionals.size() < wanted)
    {
        std::vector<std::string> names(c.positionals.begin() + static_cast<std::ptrdiff_t>(out.positionals.size()), c.positionals.end());
        names.insert(names.end(), missing.begin(), missing.end());
        throw ArgError("the following arguments are required: " + py::join(names, ", "));
    }
    if (!missing.empty())
        throw ArgError("the following arguments are required: " + py::join(missing, ", "));
    if (out.positionals.size() > wanted && !c.many)
    {
        std::string extra;
        for (size_t k = wanted; k < out.positionals.size(); ++k)
            extra += (extra.empty() ? "" : " ") + narrow(out.positionals[k]);
        throw ArgError("unrecognized arguments: " + extra);
    }
    return out;
}

int fail(const Command &c, const std::string &message)
{
    fprintf(stderr, "usage: %s\n%s: error: %s\n", c.usage.c_str(), c.prog.c_str(), message.c_str());
    return 2;
}

int to_int(const std::string &dest, const std::wstring &v)
{
    std::string s(py::strip(narrow(v)));
    size_t used = 0;
    int n = 0;
    try
    {
        n = std::stoi(s, &used);
    }
    catch (const std::exception &)
    {
        used = 0;
    }
    if (s.empty() || used != s.size())
        throw ArgError("argument --" + py::replace(dest, "_", "-") + ": invalid int value: '" + narrow(v) + "'");
    return n;
}

double to_float(const std::string &dest, const std::wstring &v)
{
    std::string s(py::strip(narrow(v)));
    char *end = nullptr;
    double d = strtod(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size())
        throw ArgError("argument --" + py::replace(dest, "_", "-") + ": invalid float value: '" + narrow(v) + "'");
    return d;
}

Option flag(const std::string &name, const std::string &help)
{
    Option o;
    o.flags = {name};
    o.dest = py::replace(name.substr(2), "-", "_");
    o.kind = Option::Flag;
    o.help = help;
    return o;
}

Option value(const std::string &name, const std::string &help, Option::Kind kind = Option::Value)
{
    Option o;
    o.flags = {name};
    o.dest = py::replace(name.substr(2), "-", "_");
    o.kind = kind;
    o.metavar = py::upper(o.dest);
    o.help = help;
    return o;
}

Option boolean(const std::string &name, const std::string &help)
{
    Option o;
    o.flags = {name, "--no-" + name.substr(2)};
    o.dest = py::replace(name.substr(2), "-", "_");
    o.kind = Option::Bool;
    o.help = help;
    return o;
}

// -- the commands

Command info_command()
{
    Command c;
    c.prog = "t4ff-cli info";
    c.usage = "t4ff-cli info [-h] [--list] fastfiles [fastfiles ...]";
    c.description = "list the content of a fastfile";
    c.positionals = {"fastfiles"};
    c.many = true;
    c.options = {flag("--list", "list every asset")};
    return c;
}

int cmd_info(const Parsed &a)
{
    for (const std::wstring &arg : a.positionals)
    {
        fs::path path(arg);
        FastFile ff = read_fastfile(path);
        const Platform &p = for_endian(ff.big_endian);
        size_t size = ff.zone.size();
        auto zone = read_zone(p, std::move(ff.zone));
        say(narrow(arg) + ": " + p.name + ", zone " + std::to_string(size) + " bytes, " + std::to_string(zone->script_strings.size()) +
            " script strings, " + std::to_string(zone->assets.size()) + " assets");
        for (int b = 0; b < BLOCK_COUNT; ++b)
        {
            // {size:>12,}: thousands separated by commas
            std::string digits = std::to_string(zone->block_sizes[b]), grouped;
            for (size_t k = 0; k < digits.size(); ++k)
            {
                if (k && (digits.size() - k) % 3 == 0)
                    grouped += ',';
                grouped += digits[k];
            }
            char buf[96];
            snprintf(buf, sizeof buf, "  %-30s %12s", block_name(b), grouped.c_str());
            say(buf);
        }
        // Counter.most_common(): by count, ties in the order first seen
        std::vector<std::pair<std::string, int>> counts;
        for (const auto &asset : zone->assets)
        {
            auto it = std::find_if(counts.begin(), counts.end(), [&](const auto &c) { return c.first == asset.type; });
            if (it == counts.end())
                counts.emplace_back(asset.type, 1);
            else
                ++it->second;
        }
        std::stable_sort(counts.begin(), counts.end(), [](const auto &x, const auto &y) { return x.second > y.second; });
        std::vector<std::string> parts;
        for (const auto &[t, n] : counts)
            parts.push_back(t + ": " + std::to_string(n));
        say("  " + py::join(parts, ", "));
        if (a.flag("list"))
            for (const auto &asset : zone->assets)
            {
                char buf[64];
                snprintf(buf, sizeof buf, "    %-16s ", asset.type.c_str());
                say(buf + asset.name);
            }
    }
    return 0;
}

Command roundtrip_command()
{
    Command c;
    c.prog = "t4ff-cli roundtrip";
    c.usage = "t4ff-cli roundtrip [-h] fastfiles [fastfiles ...]";
    c.description = "read and rewrite fastfiles, checking the result is identical";
    c.positionals = {"fastfiles"};
    c.many = true;
    return c;
}

int cmd_roundtrip(const Parsed &a)
{
    bool ok = true;
    for (const std::wstring &arg : a.positionals)
    {
        FastFile ff = read_fastfile(fs::path(arg));
        const Platform &p = for_endian(ff.big_endian);
        std::vector<uint8_t> data = ff.zone;
        auto zone = read_zone(p, std::move(ff.zone));
        bool same = write_zone(p, *zone) == data;
        ok = ok && same;
        say(narrow(arg) + ": " + (same ? "identical" : "DIFFERENT"));
    }
    return ok ? 0 : 1;
}

Command convert_command()
{
    Command c;
    c.prog = "t4ff-cli convert";
    c.usage = "t4ff-cli convert [-h] -o OUTPUT [options] input";
    c.description = "convert a PC usermap to the Xbox 360";
    c.positionals = {"input"};
    Option output;
    output.flags = {"-o", "--output"};
    output.dest = "output";
    output.kind = Option::Value;
    output.metavar = "OUTPUT";
    output.required = true;
    output.help = "output folder (a _codxe folder is created inside)";
    c.options = {
        output,
        value("--iwd", "extra .iwd files or folders to take images/sounds from (e.g. the PC game's main folder)", Option::Append),
        value("--max-texture-size", "largest texture dimension, bigger textures are downscaled (default: no limit)"),
        value("--texture-budget", "texture memory budget in MiB, 0 for no limit, or auto (default): what the memory target leaves"),
        boolean("--stream-textures", "the textures of models and world surfaces keep their top mip level in the map's images.pak (default; "
                                     "needs a CoD Xe build serving it). --no-stream-textures: every texture whole in the fastfile"),
        value("--upgrade-budget", "with --stream-textures: MiB the PC versions of stock textures may add to the fastfile (default 96)"),
        value("--deep-stream", "with --stream-textures: images (comma separated names, or all) that stream two mip levels at once"),
        value("--memory-target", "main memory the map's zone may use in MiB (default 212)"),
        value("--stream-growth", "with --stream-textures: how far around a surface its streamed textures load, of the disc linker's"),
        flag("--keep-quarter", "deep streamed textures keep a quarter of their size when the map is over its memory target"),
        flag("--keep-mip-tail", "keep every texture's mip levels of 16 texels or less when the map is over its memory target"),
        value("--xma-encoder", "path to xma2encode.exe (Xbox 360 XDK); also read from XMA2ENCODE or XEDK"),
        value("--xma-quality", "xma2encode quality 1-100 (default 60)"),
        value("--stream-rate", "resample streamed sounds above this rate (e.g. 32000)"),
        flag("--mono-streams", "downmix streamed sounds to mono"),
        value("--sound-rate", "highest sample rate of loaded (in memory) sounds: 24000, 32000, 44100 or 48000 (default: keep)"),
        flag("--mono-sounds", "downmix loaded (in memory) sounds to mono"),
        value("--console-zone", "Xbox 360 fastfile (stock or already converted) to copy console only assets from; repeatable", Option::Append),
        flag("--no-sounds", "do not convert streamed sounds"),
        flag("--no-mod", "do not merge the usermap's mod.ff into the map fastfile"),
        flag("--no-patch", "do not merge the usermap's <map>_patch.ff into the map fastfile"),
        boolean("--t4-layout", "write _codxe/t4/usermaps/<map>, CoD Xe's newer layout (default); --no-t4-layout: _codxe/usermaps/<map>"),
        boolean("--load-zone", "write <map>_load.ff, the loading screen (default)"),
        value("--name", "the map's name in the map list and on its title card"),
        value("--loading-image", "picture for the loading screen (.png, .jpg, .bmp, .tga, .dds or .iwi)"),
        flag("--no-load", "SUPPRESS"),
        flag("--no-compress", "keep uncompressed textures uncompressed (they are DXT compressed by default)"),
        flag("--no-mips", "drop all mip levels (saves ~25% memory, textures shimmer at distance)"),
        flag("--allow-unverified", "also convert assets whose console layout is not verified (may crash the game)"),
        value("--max-loaded-sounds", "loaded sounds the map may have (default 1500; 0: no limit)"),
        value("--loaded-sound-memory", "memory of the loaded sounds in MiB (default 32; 0: no limit)"),
        value("--jobs", "sounds encoded / compression threads at a time (default: one per processor)"),
        // the native program's own, for checks against the Python (no Python option starts with --dev)
        value("--dev-zone", "also write the map's zone, uncompressed, to this file"),
        value("--dev-dump", "also write the dump of the zone's node tree to this file"),
        flag("--dev-timings", "report the time of each phase"),
        flag("--dev-no-zone-cache", "read the console fastfiles into memory, not through the zone cache"),
        flag("--dev-no-sound-cache", "encode every sound again"),
        value("--dev-ffmpeg", "the FFmpeg to use"),
    };
    return c;
}

int cmd_convert(const Parsed &a, bool no_install)
{
    ConvertSettings s;
    // the options first: an error stops before anything is written
    std::optional<std::wstring> budget = a.value("texture_budget");
    if (budget)
    {
        std::string b = py::lower(py::strip(narrow(*budget)));
        s.texture_budget_auto = b == "auto";
        if (!s.texture_budget_auto)
        {
            char *end = nullptr;
            double v = strtod(b.c_str(), &end);
            if (b.empty() || end != b.c_str() + b.size() || !(v >= 0))
                throw ArgError("argument --texture-budget: a number of MiB, 0 for no limit, or auto");
            s.texture_budget = v;
        }
    }
    if (auto v = a.value("max_texture_size"))
        s.max_texture_size = static_cast<uint32_t>(to_int("max_texture_size", *v));
    if (auto v = a.value("upgrade_budget"))
        s.upgrade_budget = to_float("upgrade_budget", *v);
    if (auto v = a.value("memory_target"))
        s.memory_target = to_float("memory_target", *v);
    if (auto v = a.value("stream_growth"))
        s.stream_growth = to_float("stream_growth", *v);
    if (auto v = a.value("xma_quality"))
        s.xma_quality = to_int("xma_quality", *v);
    if (auto v = a.value("stream_rate"))
        s.stream_rate = to_int("stream_rate", *v);
    if (auto v = a.value("sound_rate"))
        s.sound_rate = to_int("sound_rate", *v);
    if (auto v = a.value("max_loaded_sounds"))
        s.max_loaded_sounds = to_int("max_loaded_sounds", *v);
    if (auto v = a.value("loaded_sound_memory"))
        s.loaded_sound_memory = to_float("loaded_sound_memory", *v);
    if (auto v = a.value("jobs"))
        s.jobs = to_int("jobs", *v);

    Usermap m = find_usermap(fs::path(a.positionals[0]));
    say("usermap " + m.name + ":");
    auto role = [&](const char *name, const std::optional<fs::path> &p) {
        char buf[16];
        snprintf(buf, sizeof buf, "  %-6s ", (std::string(name) + ":").c_str());
        say(buf + (p ? text(*p) : std::string("not found")));
    };
    role("map", m.map);
    role("patch", m.patch);
    role("mod", m.mod);
    role("load", m.load);
    for (const fs::path &p : m.localized)
        say("  localized: " + text(p) + " (the mod's language zone, merged too)");
    for (const fs::path &p : m.iwds)
        say("  iwd:   " + text(p));
    // CoD Xe reads _codxe\t4 when it exists (its newer layout), else _codxe
    fs::path out_dir = fs::path(*a.value("output")) / "_codxe";
    if (a.flag("t4_layout", true))
        out_dir /= "t4";
    out_dir = out_dir / "usermaps" / from_utf8(m.name);
    fs::create_directories(out_dir);

    s.no_sounds = a.flag("no_sounds");
    if (auto v = a.value("xma_encoder"))
        s.xma_encoder = *v;
    if (!s.no_sounds && s.xma_encoder.empty() && find_xma2encode().empty() && !no_install)
    {
        // e.g. a download of it (or a .zip with it) sitting in the Downloads folder
        std::optional<fs::path> found;
        try
        {
            found = ensure_xma2encode({}, say);
        }
        catch (const std::exception &e)
        {
            say(std::string("warning: cannot use the xma2encode.exe found: ") + e.what());
        }
        if (found)
        {
            say("using " + text(*found));
            s.xma_encoder = *found;
        }
    }
    s.map_iwds = m.iwds;
    for (const std::wstring &v : a.all("iwd"))
        s.stock_iwds.emplace_back(v);
    for (const std::wstring &v : a.all("console_zone"))
        s.console_zones.emplace_back(v);
    s.map_name = m.name;
    s.no_mips = a.flag("no_mips");
    s.no_compress = a.flag("no_compress");
    s.allow_unverified = a.flag("allow_unverified");
    s.sounds_dir = out_dir;
    s.out_dir = out_dir;
    s.stream_textures = a.flag("stream_textures", true);
    s.keep_quarter = a.flag("keep_quarter");
    s.keep_mip_tail = a.flag("keep_mip_tail");
    if (auto v = a.value("deep_stream"))
        s.deep_stream = latin1(*v);
    s.mono_streams = a.flag("mono_streams");
    s.mono_sounds = a.flag("mono_sounds");
    if (m.load)
        s.load_ff = *m.load;
    if (auto v = a.value("loading_image"))
        s.loading_image = *v;
    if (auto v = a.value("name"))
        s.name = latin1(*v);
    s.no_load_zone = !a.flag("load_zone", true);
    if (auto v = a.value("dev_zone"))
        s.out = *v;
    if (auto v = a.value("dev_dump"))
        s.dump = *v;
    s.timings = a.flag("dev_timings");
    s.no_zone_cache = a.flag("dev_no_zone_cache");
    s.no_sound_cache = a.flag("dev_no_sound_cache");
    if (auto v = a.value("dev_ffmpeg"))
        s.ffmpeg = *v;

    // one fastfile: the mod's language zones, the map, its patch and the mod, merged in that order
    std::vector<fs::path> paths;
    if (!a.flag("no_mod"))
        paths = m.localized;
    paths.push_back(m.map);
    if (m.patch && !a.flag("no_patch"))
        paths.push_back(*m.patch);
    if (m.mod && !a.flag("no_mod"))
        paths.push_back(*m.mod);
    // the map's own files: the folders of its fastfile and .iwd files
    std::vector<fs::path> folders{fs::absolute(m.map).parent_path()};
    for (const fs::path &iwd : m.iwds)
        folders.push_back(fs::absolute(iwd).parent_path());
    for (const fs::path &f : folders)
        if (std::find(s.map_files.begin(), s.map_files.end(), f) == s.map_files.end())
            s.map_files.push_back(f);
    return convert_usermap(paths, s);
}

Command menu_command()
{
    Command c;
    c.prog = "t4ff-cli menu";
    c.usage = "t4ff-cli menu [-h] [--rows ROWS] [--no-streams] [--menu-zone PATH] folder";
    c.description = "make the Nazi Zombies map list of CoD Xenon's patch_ui.ff show every map of the usermaps folder";
    c.positionals = {"folder"};
    Option menu_zone = value("--menu-zone", "a patch_ui.ff with CoD Xe's own custom maps list (CoD Xenon's 0.3.0, or its folder)", Option::Append);
    menu_zone.metavar = "PATH";
    c.options = {value("--rows", "rows the list shows at a time (default 13, as CoD Xenon's)"),
                 flag("--no-streams", "do not check the streamed sounds (.xma) of the maps in usermaps (see the streams command)"), menu_zone};
    return c;
}

int upgrade_map_streams(const fs::path &folder)
{
    say("streamed sounds: checking the .xma files in " + text(folder));
    UpgradeStats stats = upgrade_stream_files(folder, say);
    if (stats.upgraded)
        say("streamed sounds: " + std::to_string(stats.upgraded) + " of " + std::to_string(stats.files) + " in " + text(folder) +
            " rewritten in the game's layout (4 KiB blocks and their table; before, they stopped after a split second)");
    else if (stats.files)
        say("streamed sounds: the " + std::to_string(stats.files) + " in " + text(folder) + " have the game's layout already");
    return stats.failed ? 1 : 0;
}

std::vector<std::wstring> sorted_names(const fs::path &folder)
{
    std::vector<std::wstring> names;
    std::error_code ec;
    if (fs::is_directory(folder, ec))
        for (const auto &e : fs::directory_iterator(folder))
            names.push_back(e.path().filename().wstring());
    std::sort(names.begin(), names.end());
    return names;
}

void write_crlf(const fs::path &path, const std::string &content)
{
    std::string out = py::replace(content, "\n", "\r\n");
    std::ofstream f(path, std::ios::binary);
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
}

std::unique_ptr<Zone> read_console_zone(const fs::path &path)
{
    FastFile ff = read_fastfile(path);
    return read_zone(x360(), std::move(ff.zone));
}

// t4ff's own custom maps list, made from CoD Xenon's 0.2.0 patch_ui.ff, with the names, descriptions and
// pictures of the maps. Returns 0, or 1 when it cannot be made.
int t4ff_menu(int rows, const fs::path &zone_dir, const fs::path &target, const fs::path &usermaps)
{
    fs::path original = target.wstring() + L".orig";
    // CoD Xenon's menu is kept as patch_ui.ff.orig and every run starts again from it
    fs::path source = fs::exists(original) ? original : target;
    if (!fs::exists(source))
    {
        say("error: " + text(target) + " not found (give CoD Xenon's _codxe\\t4 folder, or a menu zone with CoD Xe's own list with --menu-zone)");
        return 1;
    }
    std::unique_ptr<Zone> zone = read_console_zone(source);
    if (auto problem = not_cod_xenon_menu(x360(), *zone))
    {
        say("error: " + text(source) + " is not CoD Xenon's menu: " + *problem + ".");
        say("Put CoD Xenon's patch_ui.ff (_codxe\\t4\\zone\\patch_ui.ff of their 0.2.0 zip) in " + text(zone_dir) +
            (source == original ? " and delete " + text(original) : std::string()) + ", or give their 0.3.0 one with --menu-zone, then run this again.");
        return 1;
    }
    if (source == target)
    {
        fs::copy_file(target, original, fs::copy_options::overwrite_existing);
        say("kept CoD Xenon's menu as " + text(original));
    }
    std::vector<CustomRow> found;
    try
    {
        found = make_dynamic(x360(), *zone, rows);
    }
    catch (const menu_edit::MenuError &e)
    {
        say(std::string("error: ") + e.what());
        return 1;
    }
    write_console_zone(write_zone(x360(), *zone), target, 0, say);
    say(text(target) + ": \"Custom Maps\" in the Nazi Zombies menu lists the maps of the usermaps folder, " + std::to_string(rows) +
        " at a time (LB / RB: a page)");

    // names, descriptions and pictures of CoD Xenon's maps, whose rows the list replaces
    fs::path patch = zone_dir / "patch.ff";
    std::vector<std::pair<std::string, std::string>> strings;
    if (fs::exists(patch))
        strings = localized_strings(x360(), *read_console_zone(patch));
    auto lookup = [&](const std::optional<std::string> &key, const std::string &fallback) {
        std::string k = key.value_or("");
        k = k.substr(std::min(k.find_first_not_of('@'), k.size()));
        for (const auto &[n, v] : strings)
            if (n == k)
                return v;
        return fallback;
    };
    int written = 0;
    for (const CustomRow &row : found)
    {
        fs::path folder = usermaps / std::wstring(row.map.begin(), row.map.end()); // latin-1: a character a byte
        for (size_t k = 0; k < row.map.size(); ++k) // (chars are signed: the bytes over 127 as themselves)
            if (static_cast<unsigned char>(row.map[k]) > 127)
            {
                std::wstring w;
                for (char ch : row.map)
                    w += static_cast<wchar_t>(static_cast<unsigned char>(ch));
                folder = usermaps / w;
                break;
            }
        if (!fs::is_directory(folder))
            continue;
        std::string title = lookup(row.title, row.map);
        std::string description = lookup(row.description, "");
        fs::path path = folder / "description.txt";
        if (!fs::exists(path))
        {
            write_crlf(path, description_text(title, description));
            ++written;
        }
        if (row.image && !row.image->empty())
            write_crlf(folder / "preview.txt", *row.image + "\n");
    }
    if (written)
        say("wrote the names and descriptions of " + std::to_string(written) + " of CoD Xenon's maps (description.txt in their folders)");

    // pictures of the other maps, from their loading screens
    std::vector<std::string> pictures;
    for (const std::wstring &name : sorted_names(usermaps))
    {
        fs::path folder = usermaps / name;
        if (!fs::is_directory(folder) || fs::exists(folder / "preview.txt") || fs::exists(folder / "preview.bin"))
            continue;
        if (write_preview(folder, narrow(name)))
            pictures.push_back(narrow(name));
    }
    if (!pictures.empty())
        say("map list pictures (preview.bin) from the loading screens of " + py::join(pictures, ", "));
    return 0;
}

int cmd_menu(const Parsed &a)
{
    int rows = a.value("rows") ? to_int("rows", *a.value("rows")) : USERMAPS_ROWS;
    fs::path root(a.positionals[0]);
    fs::path zone_dir = fs::is_directory(root / "zone") ? root / "zone" : root;
    fs::path target = zone_dir / "patch_ui.ff";
    fs::path usermaps = root / "usermaps";
    auto own_list = [](const fs::path &path) {
        try
        {
            return has_own_usermaps_list(*read_console_zone(path));
        }
        catch (const std::exception &)
        {
            return false;
        }
    };
    bool own = fs::exists(target) && own_list(target);
    if (!own)
    {
        // a menu zone with CoD Xe's own list takes the place of the game's (the one before is kept)
        std::vector<fs::path> given;
        for (const std::wstring &v : a.all("menu_zone"))
            given.emplace_back(v);
        for (const fs::path &candidate : menu_zone_candidates(given))
        {
            if (fs::absolute(candidate) == fs::absolute(target) || !own_list(candidate))
                continue;
            fs::path backup = target.wstring() + L".bak";
            if (fs::exists(target) && !fs::exists(backup))
                fs::copy_file(target, backup);
            fs::copy_file(candidate, target, fs::copy_options::overwrite_existing);
            say(text(target) + ": the menu zone of " + text(candidate) + (fs::exists(backup) ? " (the one before is " + text(backup) + ")" : ""));
            own = true;
            break;
        }
    }
    if (own)
        say(text(target) + ": \"Custom Maps\" is CoD Xe's own list (the menu codxe_usermaps), which shows the maps of the usermaps folder "
                           "from their map.json and preview.dds (CoD Xe r351 or later)");
    else if (int status = t4ff_menu(rows, zone_dir, target, usermaps))
        return status;

    // map.json and preview.dds, which CoD Xe's own list reads, where a map has none
    std::vector<std::string> infos;
    for (const std::wstring &name : sorted_names(usermaps))
    {
        fs::path folder = usermaps / name;
        if (!fs::is_directory(folder))
            continue;
        std::vector<std::string> written =
            write_map_info(folder, narrow(name), !fs::exists(folder / "map.json"), !fs::exists(folder / "preview.dds"));
        if (!written.empty())
            infos.push_back(narrow(name) + " (" + py::join(written, ", ") + ")");
    }
    if (!infos.empty())
        say("CoD Xe's custom maps list: " + py::join(infos, ", "));
    // the streamed sounds of the maps (CoD Xenon's, older conversions) in the game's layout
    if (fs::is_directory(usermaps) && !a.flag("no_streams"))
        upgrade_map_streams(usermaps);
    return 0;
}

Command streams_command()
{
    Command c;
    c.prog = "t4ff-cli streams";
    c.usage = "t4ff-cli streams [-h] folders [folders ...]";
    c.description = "rewrite the streamed sounds (.xma) of converted maps in the game's layout";
    c.positionals = {"folders"};
    c.many = true;
    return c;
}

int cmd_streams(const Parsed &a)
{
    int status = 0;
    for (const std::wstring &f : a.positionals)
        status = std::max(status, upgrade_map_streams(fs::path(f)));
    return status;
}

Command setup_command()
{
    Command c;
    c.prog = "t4ff-cli setup";
    c.usage = "t4ff-cli setup [-h] [--xma2encode XMA2ENCODE] [--no-test]";
    c.description = "find (and install) xma2encode.exe and FFmpeg, and check them";
    c.options = {value("--xma2encode", "xma2encode.exe, a folder or a .zip containing it (default: search this computer)"),
                 flag("--no-test", "do not test the encoder")};
    return c;
}

int cmd_setup(const Parsed &a)
{
    std::optional<std::wstring> source = a.value("xma2encode");
    SetupState state = setup(source ? fs::path(*source) : fs::path(), !a.flag("no_test"), say);
    say("");
    if (state.xma2encode)
        say("Ready: everything t4ff needs is installed.");
    else
        say("Ready to convert, without sounds (xma2encode.exe is missing, see above).");
    return 0;
}

Command gui_command()
{
    Command c;
    c.prog = "t4ff-cli gui";
    c.usage = "t4ff-cli gui [-h]";
    c.description = "open the converter window";
    return c;
}

int cmd_gui()
{
    // the window is t4ff.exe, next to this program
    std::wstring exe(MAX_PATH, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
    fs::path window = fs::path(exe).parent_path() / L"t4ff.exe";
    std::error_code ec;
    if (!fs::is_regular_file(window, ec) || !launch({window.wstring()}))
    {
        fprintf(stderr, "t4ff-cli gui: cannot start %s (the window, next to t4ff-cli.exe)\n", text(window).c_str());
        return 1;
    }
    return 0;
}

int top_usage()
{
    fputs("usage: t4ff-cli [-h] [--no-install] [--progress-lines] {info,roundtrip,convert,menu,streams,gui,setup} ...\n\n"
          "World at War fastfile tools (PC -> Xbox 360 conversion for CoD Xe)\n\n"
          "commands:\n"
          "  info        list the content of a fastfile\n"
          "  roundtrip   read and rewrite fastfiles, checking the result is identical\n"
          "  convert     convert a PC usermap to the Xbox 360\n"
          "  menu        make the Nazi Zombies map list of CoD Xenon's patch_ui.ff show every map of the usermaps folder\n"
          "  streams     rewrite the streamed sounds (.xma) of converted maps in the game's layout\n"
          "  gui         open the converter window\n"
          "  setup       find (and install) xma2encode.exe and FFmpeg, and check them\n\n"
          "options:\n"
          "  -h, --help        show this help message and exit\n"
          "  --no-install      do not install xma2encode.exe automatically\n"
          "  --progress-lines  report progress as '@progress <done> <total> <step>' lines (for the window)\n\n"
          "t4ff-cli's developer commands (dump, rewrite, bench, textures, texbench, cache, regex, and convert with --out): t4ff-cli dev\n",
          stdout);
    return 0;
}
} // namespace

int run(const std::vector<std::wstring> &all)
{
    // the program's own options, before the command (unique prefixes too)
    bool no_install = false, lines = false;
    size_t i = 0;
    for (; i < all.size() && all[i].size() > 2 && all[i].rfind(L"--", 0) == 0; ++i)
    {
        std::wstring a = all[i];
        bool install = std::wstring(L"--no-install").rfind(a, 0) == 0, progress = std::wstring(L"--progress-lines").rfind(a, 0) == 0;
        bool help = std::wstring(L"--help").rfind(a, 0) == 0;
        if (help && !install && !progress)
            return top_usage();
        if (install && !progress)
            no_install = true;
        else if (progress && !install)
            lines = true;
        else
            break;
    }
    if (i < all.size() && (all[i] == L"-h"))
        return top_usage();
    if (i >= all.size())
        return -1;
    std::wstring command = all[i];
    std::vector<std::wstring> args(all.begin() + static_cast<std::ptrdiff_t>(i) + 1, all.end());
    // the developer forms stay t4ff-cli's: convert with --out (not -o / --output), roundtrip with its
    // options or folders, the other developer commands
    if (command == L"convert")
    {
        // the developer form: --out <zone> (the Python's own form writes into -o / --output)
        for (const std::wstring &a : args)
            if (a == L"--out" || a.rfind(L"--out=", 0) == 0)
                return -1;
    }
    else if (command == L"roundtrip")
    {
        for (const std::wstring &a : args)
            if ((a.rfind(L"-", 0) == 0 && a != L"-h" && a != L"--help") || fs::is_directory(fs::path(a)))
                return -1;
    }
    else if (command != L"info" && command != L"menu" && command != L"streams" && command != L"setup" && command != L"gui")
        return -1;
    if (i > 0 && command != L"convert" && command != L"info" && command != L"roundtrip" && command != L"menu" && command != L"streams" &&
        command != L"setup" && command != L"gui")
        return -1;
    progress::use_lines(lines);

    Command c;
    if (command == L"info")
        c = info_command();
    else if (command == L"roundtrip")
        c = roundtrip_command();
    else if (command == L"convert")
        c = convert_command();
    else if (command == L"menu")
        c = menu_command();
    else if (command == L"streams")
        c = streams_command();
    else if (command == L"setup")
        c = setup_command();
    else
        c = gui_command();
    Parsed parsed;
    try
    {
        parsed = parse(c, args);
    }
    catch (const ArgError &e)
    {
        return fail(c, e.what());
    }
    try
    {
        if (command == L"info")
            return cmd_info(parsed);
        if (command == L"roundtrip")
            return cmd_roundtrip(parsed);
        if (command == L"convert")
            return cmd_convert(parsed, no_install);
        if (command == L"menu")
            return cmd_menu(parsed);
        if (command == L"streams")
            return cmd_streams(parsed);
        if (command == L"gui")
            return cmd_gui();
        return cmd_setup(parsed);
    }
    catch (const ArgError &e)
    {
        return fail(c, e.what());
    }
    catch (const UserError &e)
    {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
} // namespace t4ff::cli
