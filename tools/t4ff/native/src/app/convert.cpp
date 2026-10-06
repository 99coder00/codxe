#include "app/convert.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_set>

#include <windows.h>
#include <psapi.h>

#include "audio/audio.h"
#include "convert/assets.h"
#include "convert/converter.h"
#include "convert/library.h"
#include "convert/loadscreen.h"
#include "convert/menus.h"
#include "convert/merge.h"
#include "convert/scripts.h"
#include "convert/techsets.h"
#include "core/progress.h"
#include "core/dump.h"
#include "core/fastfile.h"
#include "core/image.h"
#include "core/platforms.h"
#include "core/pystr.h"
#include "core/zone.h"
#include "core/zone_cache.h"

namespace t4ff
{
namespace
{
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string utf8(const fs::path &p)
{
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string mib_text(double bytes)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%.1f", bytes / 1048576.0);
    return buf;
}

struct pair_measure
{
    int64_t budget, used;
};

// committed (private) bytes now and at the peak, and the peak working set
std::string memory_text()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof pmc);
    char buf[160];
    snprintf(buf, sizeof buf, "private %.0f MiB now, %.0f MiB at the peak; peak working set %.0f MiB", pmc.PrivateUsage / 1048576.0,
             pmc.PeakPagefileUsage / 1048576.0, pmc.PeakWorkingSetSize / 1048576.0);
    return buf;
}

void write_text(const fs::path &out_path, const std::string &text)
{
    FILE *f = _wfopen(out_path.c_str(), L"wb");
    if (!f)
        throw std::runtime_error("cannot write " + utf8(out_path));
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

// the conversion's messages: to its log (a line each)
using LogFn = std::function<void(const std::string &)>;
thread_local const LogFn *g_log = nullptr;

struct Sink
{
    const LogFn *prev;
    explicit Sink(const LogFn &log) : prev(g_log)
    {
        g_log = &log;
    }
    ~Sink()
    {
        g_log = prev;
    }
};

void say(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::string text(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0)
        vsnprintf(text.data(), text.size() + 1, fmt, args);
    va_end(args);
    while (!text.empty() && text.back() == '\n')
        text.pop_back();
    if (g_log && *g_log)
        (*g_log)(text);
    else
    {
        printf("%s\n", text.c_str());
        fflush(stdout);
    }
}

// Writes a console zone's fastfile, then reads the zone back (the console's loading rules check it)
// and reports the memory of its blocks, as the Python's write_zone.
void write_console_zone(const std::vector<uint8_t> &zone, const fs::path &target, int jobs)
{
    std::string base = utf8(target.filename());
    progress::step("Writing " + base);
    write_fastfile(target, true, zone, 9, jobs);
    progress::step("Checking " + base);
    std::vector<uint32_t> sizes = read_zone(x360(), zone)->block_sizes;
    std::string blocks;
    uint64_t total = 0;
    for (size_t b = 0; b < sizes.size(); ++b)
    {
        total += sizes[b];
        if (!sizes[b])
            continue;
        std::string name = block_name(static_cast<int>(b));
        name = py::lower(name.substr(name.find("_BLOCK_") + 7));
        blocks += (blocks.empty() ? "" : ", ") + name + " " + mib_text(sizes[b]);
    }
    say("wrote %s (%.1f MiB compressed)", utf8(target).c_str(), fs::file_size(target) / 1048576.0);
    say("  memory: %s MiB (%s)", mib_text(static_cast<double>(total)).c_str(), blocks.c_str());
}


// The textures of models and world surfaces keep their top level in the map's images.pak (stream.h),
// as the Python _convert_map streams them with --stream-textures.
void stream_map_textures(const ConvertSettings &a, const ConvertOptions &o, const std::vector<std::unique_ptr<ZoneConverter>> &convs, Zone &zone,
                         const fs::path &out_dir, StreamReport &report)
{
    StreamOptions so;
    // the console fastfiles' own highmip folders (the disc's, next to its zones)
    std::set<std::wstring> zone_dirs;
    for (const fs::path &z : a.console_zones)
        zone_dirs.insert(fs::absolute(fs::is_directory(z) ? z : z.parent_path()).lexically_normal().make_preferred().wstring());
    std::vector<std::wstring> highmip;
    for (const std::wstring &d : zone_dirs)
        highmip.push_back((fs::path(d) / "highmip").wstring());
    std::sort(highmip.begin(), highmip.end());
    so.highmip_dirs.assign(highmip.begin(), highmip.end());
    ConsoleLibrary *library = convs[0]->console_library;
    if (library)
        so.is_game_image = [library](const std::string &name) { return library->in_game_zones("GfxImage", name); };
    // the PC game's version of a stock texture (--iwd), tiled for the console; none for the map's own
    std::unordered_set<std::string> own;
    for (const auto &c : convs)
        for (const auto &[name, source] : c->image_sources)
            if (source)
                own.insert(name);
    ZoneConverter *first = convs[0].get();
    bool compress = o.compress_textures, mip_tail = o.mip_tail;
    so.stock_texture = [first, own, compress, mip_tail](const std::string &name, int semantic) -> std::optional<ConsoleTexture> {
        if (own.count(name) || !first->stock_library)
            return std::nullopt;
        ImageRef source = stock_image_source(*first, name);
        if (!source || source->faces != 1)
            return std::nullopt;
        TextureOptions t;
        t.compress = compress;
        t.normal_map = pc_normal_map(*source, semantic);
        t.mip_tail = mip_tail;
        try
        {
            return build_console_texture(*source, t);
        }
        catch (const ImageError &)
        {
            return std::nullopt;
        }
    };
    if (a.deep_stream == "all")
        so.deep_all = true;
    else
        for (const std::string &n : py::split(a.deep_stream, ","))
            if (!py::strip(n).empty())
                so.deep.insert(py::lower(py::strip(n)));
    if (a.upgrade_budget_bytes)
        so.upgrade_budget = static_cast<uint64_t>(*a.upgrade_budget_bytes);
    so.mip_tail = o.mip_tail;
    so.eighth = first->eighth_images;
    so.growth = a.stream_growth_scale;
    stream_textures(x360(), zone, out_dir, o.log, so, &report);
}

// The passes of the Python _convert_map after the zones are merged: scripts and assets the map lacks,
// the mod's scripts and menus, the fixes for the console, the loaded sound limit, the map's options and
// scripts folder (into the output folder).
void finish_map(const ConvertSettings &a, const ConvertOptions &o, const std::vector<std::unique_ptr<ZoneConverter>> &convs,
                std::unique_ptr<Zone> &main_zone, const IwdLibrary &map_files, std::set<std::string> &mod_scripts, StreamReport &report)
{
    progress::step("Checking the scripts");
    auto quiet = [](const std::string &) {};
    ConsoleLibrary *library = convs[0]->console_library;
    fs::path out_dir = a.out_dir.empty() ? a.sounds_dir : a.out_dir;
    std::string level_script = "maps/" + o.map_name + ".gsc";
    IwdLibrary stock(a.stock_iwds);
    std::vector<const IwdLibrary *> sources{&map_files, &stock};
    std::unique_ptr<Zone> extra = missing_scripts_zone(x360(), *main_zone, sources, library, o.log,
                                                       {level_script, "clientscripts/" + o.map_name + ".csc"}, &mod_scripts);
    auto merge_in = [&](std::unique_ptr<Zone> z) {
        std::vector<std::unique_ptr<Zone>> two;
        two.push_back(std::move(main_zone));
        two.push_back(std::move(z));
        main_zone = merge_zones(x360(), std::move(two), quiet);
    };
    if (extra)
        merge_in(std::move(extra));
    // assets the game looks up by name (player body animations, shellshock files) no zone has
    extra = named_assets_zone(x360(), *main_zone, sources, library, o.log);
    if (extra)
        merge_in(std::move(extra));
    Zone &zone = *main_zone;
    // the mod's scripts that the game's own zones have too run under their own names
    std::map<std::string, std::string> renamed = keep_mod_scripts(x360(), zone, mod_scripts, library, level_script, o.log);
    // the options the mod's own front end menus set, before those menus go
    MenuValues menu_values = menu_dvar_values(zone);
    std::vector<std::string> pause_menus; // menus the map's own pause menu opens
    if (library)
    {
        auto ingame = library->find_in_game_zones("MenuList", "ui/ingame.txt");
        auto [kept, extras] = bind_pause_menu(x360(), zone, ingame ? menu_names(ingame->second) : std::vector<std::string>(), o.log);
        pause_menus = extras;
        drop_frontend_menus(
            x360(), zone, [&](const std::string &n) { return library->is_stock_menu(n); }, o.log,
            [&](const std::string &n) { return library->in_game_zones("MenuList", n); }, kept);
    }
    drop_unused_videos(x360(), zone, o.log);
    // PC script menus and hints name keyboard keys: the controller's buttons instead
    gamepad_script_menus(x360(), zone, o.log);
    std::set<std::string> script_menus = script_strings(x360(), zone); // the menus the scripts open are named in them
    script_menus.insert(pause_menus.begin(), pause_menus.end());
    decorate_inert_items(x360(), zone, o.log, &script_menus);
    controller_navigation(x360(), zone, o.log, &script_menus);
    use_key_hints(x360(), zone, o.log);
    fix_modder_help(x360(), zone, o.log);
    spawn_script_origins(x360(), zone, o.log);
    valid_cursor_hints(x360(), zone, o.log);
    local_client_effects(x360(), zone, o.log);
    precache_before_waits(x360(), zone, level_script, o.log);
    menu_dvar_defaults(x360(), zone, menu_values, level_script, o.log);
    speed_up_zombies_only(x360(), zone, o.log);
    zombie_idles_for_zombies(x360(), zone, o.log);
    // in splitscreen the map's own fog, not the game's yellow placeholder
    std::set<std::string> renamed_to;
    for (const auto &[old, now] : renamed)
        renamed_to.insert(now);
    splitscreen_fog(
        x360(), zone, level_script,
        [&](const std::string &name) { return renamed_to.count(name) || (library && library->find_in_game_zones("RawFile", name)); }, o.log);
    mounted_guns(x360(), zone, level_script, o.log);
    reset_sustain_ammo(x360(), zone, level_script, o.log);
    // technique sets copied from CoD Xenon's maps read the dynamic shadow texture before it is set
    fix_argument_sections(x360(), zone, o.log);
    prune_references(x360(), zone, {"techset"}, o.log);
    if (a.max_loaded_sounds || a.loaded_sound_memory)
    {
        progress::step("Checking the loaded sound limit");
        std::map<std::string, std::shared_ptr<XmaStream>> streams;
        for (const auto &[key, entry] : o.sound_cache->entries)
            if (entry.xma && entry.xma->stream)
                streams[lower_latin1(key.first)] = entry.xma->stream;
        limit_loaded_sounds(x360(), zone, a.max_loaded_sounds, streams, out_dir, o.log,
                            static_cast<uint64_t>(a.loaded_sound_memory * 1048576));
    }
    int fixed = sync_alias_types(x360(), zone);
    if (fixed)
        say("sound aliases: the type in the flags of %d aliases set to their sound file's\n", fixed);
    // options the mod's own front end menus choose: the Custom Maps menu shows them
    MenuValues pc_values;
    std::vector<Zone *> pc_zones;
    for (const auto &c : convs)
    {
        pc_zones.push_back(&c->zone);
        for (const auto &[dvar, values] : menu_dvar_values(c->zone))
            pc_values[dvar].insert(values.begin(), values.end());
    }
    std::vector<MapOption> map_options = menu_options(pc(), pc_zones, script_dvars(x360(), zone), pc_values);
    if (!out_dir.empty())
        write_options(map_options, out_dir, o.log);
    // and asked in game as the level starts
    if (!map_options.empty())
    {
        std::vector<std::string> menus = add_options_menus(x360(), zone, library, map_options, o.log);
        if (!menus.empty())
            options_script(x360(), zone, level_script, map_options, menus, o.log);
    }
    // the mod's versions of the game's scripts that keep no name of their own: the map's scripts folder
    std::map<std::string, std::string> folder = usermap_scripts(x360(), zone, mod_scripts, library, renamed);
    if (!out_dir.empty())
        write_usermap_scripts(folder, out_dir, o.log);
    if (a.stream_textures)
        stream_map_textures(a, o, convs, zone, out_dir, report);
}

// The map's files besides its zone, as the Python cmd_convert writes them: the t4ff marker, the map's
// name (description.txt), its loading screen zone and pictures for the map lists.
void write_map_files(const ConvertSettings &a, const ConvertOptions &o, const std::vector<std::unique_ptr<ZoneConverter>> &convs, Zone &main_zone,
                     const IwdLibrary &map_files, const fs::path &out_dir)
{
    const std::string &name = a.map_name;
    // later conversions do not take this map's fastfiles for console data
    write_text(out_dir / T4FF_MARKER,
               "Converted from the PC by t4ff (tools/t4ff): not console data, t4ff leaves these fastfiles out of its console fastfiles.\r\n");
    // the map's name in the map lists (CoD Xe reads the first line of description.txt)
    std::string given(py::strip(a.name));
    std::vector<std::pair<std::string, std::string>> localized = localized_strings(x360(), main_zone);
    std::string title = !given.empty() ? given : map_title(&map_files, name, &localized);
    fs::path description = out_dir / "description.txt";
    auto bare = [&]() {
        // only the map's file name, as older conversions wrote it: a better one replaces it
        std::vector<uint8_t> raw = read_file(description);
        std::string text(raw.begin(), raw.end());
        std::vector<std::string> lines;
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find_first_of("\r\n", start);
            std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!py::strip(line).empty())
                lines.emplace_back(py::strip(line));
            if (end == std::string::npos)
                break;
            start = end + (text.compare(end, 2, "\r\n") == 0 ? 2 : 1);
        }
        return lines.size() == 1 && lines[0] == name;
    };
    if (!given.empty() || !fs::exists(description) || bare())
    {
        write_text(description, py::replace(title + "\n", "\n", "\r\n"));
        say("map list name: \"%s\" (%s; change it there or with --name)\n", title.c_str(), utf8(description).c_str());
    }
    if (!a.no_load_zone)
    {
        // CoD Xe serves <map>_load.ff as the loading screen zone: made like CoD Xenon's, with the map's picture
        progress::step("Writing the loading screen");
        bool done = false;
        try
        {
            done = write_load_zone(name, out_dir, library_files(a.console_zones, name, {out_dir}), map_files, a.load_ff, a.loading_image, a.jobs, o.log, title);
        }
        catch (const LoadScreenError &e)
        {
            say("warning: %s\n", e.what());
        }
        if (!done && !a.load_ff.empty())
        {
            // no load zone of CoD Xenon's among the console fastfiles: the PC one converted
            ConvertOptions lo = o;
            lo.reference_techsets = true;
            lo.texture_budget = 0;
            FastFile ff = read_fastfile(a.load_ff);
            ZoneConverter conv(read_zone(pc(), std::move(ff.zone)), pc(), x360(), lo);
            std::vector<ZoneConverter *> one{&conv};
            prefetch_image_sources(one);
            plan_textures_shared(one);
            prebuild_textures(one);
            std::unique_ptr<Zone> zone = conv.convert();
            prune_references(x360(), *zone, {"techset"}, o.log);
            write_console_zone(write_zone(x360(), *zone), out_dir / a.load_ff.filename(), a.jobs);
        }
        else if (!done)
            say("loading screen: none written (add CoD Xenon's _codxe\\t4 folder to the console fastfiles): the game shows a checkerboard while the "
                   "map loads\n");
        // the same picture for the map list
        if (write_preview(out_dir, name))
            say("map list picture: preview.bin, from the loading screen\n");
    }
    // and for CoD Xe's own custom maps list: map.json and preview.dds
    std::vector<std::string> info = write_map_info(out_dir, name);
    if (!info.empty())
        say("CoD Xe's custom maps list: %s (from description.txt and the loading screen)\n", py::join(info, " and ").c_str());
    (void)convs;
}

// One conversion of the map (the Python _convert_map): its converters (the zone uses their types), its
// zone, the texture bytes its planner planned and what streaming did.
struct Conversion
{
    std::vector<std::unique_ptr<ZoneConverter>> convs;
    std::unique_ptr<Zone> zone;
    std::unique_ptr<IwdLibrary> map_files;
    uint64_t planned = 0;
    StreamReport report;
};

Conversion convert_map(const std::vector<fs::path> &paths, const ConvertSettings &a, const ConvertOptions &o)
{
    auto phase = Clock::now();
    auto lap = [&](const char *what) {
        if (a.timings)
            say("time: %s %.2f s\n", what, seconds_since(phase));
        phase = Clock::now();
    };
    Conversion result;
    std::vector<std::unique_ptr<ZoneConverter>> &convs = result.convs;
    for (size_t index = 0; index < paths.size(); ++index)
    {
        const fs::path &path = paths[index];
        progress::step("Reading PC fastfiles", static_cast<int>(index), static_cast<int>(paths.size()));
        FastFile ff = read_fastfile(path);
        if (ff.big_endian)
            throw std::runtime_error(utf8(path) + ": not a PC fastfile");
        convs.push_back(std::make_unique<ZoneConverter>(read_zone(pc(), std::move(ff.zone)), pc(), x360(), o));
        convs.back()->progress_label =
            "Converting " + utf8(path.filename()) +
            (paths.size() > 1 ? " (file " + std::to_string(index + 1) + " of " + std::to_string(paths.size()) + ")" : std::string());
    }
    std::vector<ZoneConverter *> raw;
    std::vector<Zone *> pc_zones;
    for (auto &c : convs)
    {
        raw.push_back(c.get());
        pc_zones.push_back(&c->zone);
    }
    lap("reading the PC fastfiles");
    // the map's own loose scripts win over those of its fastfiles, as on PC
    result.map_files = std::make_unique<IwdLibrary>(a.map_files);
    IwdLibrary &map_files = *result.map_files;
    override_scripts(pc(), pc_zones, map_files, o.log);
    // scripts the PC game takes from the mod (its mod.ff, its own files) over the game's own
    std::set<std::string> mod_scripts;
    for (size_t i = 0; i < convs.size(); ++i)
    {
        std::string base = lower_latin1(utf8(paths[i].filename()));
        bool mod_zone = base == "mod.ff" || base.rfind("localized_", 0) == 0;
        for (auto &[script, node] : rawfiles(pc(), convs[i]->zone))
        {
            std::string key = normalize_script(script);
            if ((script.empty() || script[0] != ',') && (mod_zone || map_files.read(key)))
                mod_scripts.insert(key);
        }
    }
    if (o.xma_encoder && !a.sounds_dir.empty())
    {
        // streamed sounds of the game's own the console's disc has not, from the PC game's files
        std::unique_ptr<IwdLibrary> stock = a.stock_iwds.empty() ? nullptr : std::make_unique<IwdLibrary>(a.stock_iwds);
        ship_stock_streams(pc(), pc_zones, stock.get(), a.sounds_dir, *o.xma_encoder, a.stream_rate, a.mono_streams, a.jobs, o.log);
        lap("stock streamed sounds");
    }
    progress::step("Planning texture memory");
    prefetch_image_sources(raw);
    lap("reading the images");
    plan_textures_shared(raw);
    lap("planning the textures (with the console library)");
    auto tb = Clock::now();
    prebuild_textures(raw);
    if (a.timings)
        say("textures built in %.2f s\n", seconds_since(tb));
    std::vector<std::unique_ptr<Zone>> zones;
    for (size_t i = 0; i < convs.size(); ++i)
    {
        auto t = Clock::now();
        ZoneConverter &c = *convs[i];
        say("%s: %zu assets\n", utf8(paths[i].filename()).c_str(), c.zone.assets.size());
        zones.push_back(c.convert());
        auto table = [](const std::map<std::string, int> &m) { // as Python prints a dict
            std::string s;
            for (const auto &[k, v] : m)
                s += (s.empty() ? "'" : ", '") + k + "': " + std::to_string(v);
            return "{" + s + "}";
        };
        say("  converted:  %s\n", table(c.stats.converted).c_str());
        if (!c.stats.copied.empty())
            say("  copied from console zones: %s\n", table(c.stats.copied).c_str());
        if (!c.stats.referenced.empty())
            say("  referenced: %s\n", table(c.stats.referenced).c_str());
        say("  textures:   %.1f MiB, loaded sounds: %.1f MiB (%.1fs)\n", c.stats.texture_bytes / 1048576.0, c.stats.sound_bytes / 1048576.0,
               seconds_since(t));
    }
    lap("converting");
    if (zones.size() > 1)
        progress::step("Merging into one fastfile");
    std::unique_ptr<Zone> main_zone = zones.size() == 1 ? std::move(zones[0]) : merge_zones(x360(), std::move(zones), o.log);
    lap("merging");
    finish_map(a, o, convs, main_zone, map_files, mod_scripts, result.report);
    lap("scripts and menus");
    result.zone = std::move(main_zone);
    result.planned = convs[0]->planned_texture_bytes;
    return result;
}

} // namespace

int convert_usermap(const std::vector<fs::path> &paths, const ConvertSettings &a)
{
    auto t0 = Clock::now();
    ConvertOptions o;
    o.log = a.log ? a.log : [](const std::string &msg) {
        printf("%s\n", msg.c_str());
        fflush(stdout);
    };
    Sink sink(o.log);
    o.iwd_paths = a.map_iwds;
    o.stock_paths = a.stock_iwds;
    o.console_zones = a.console_zones;
    o.map_name = a.map_name;
    // the memory plan: the automatic budget starts from full quality, streaming leaving most of the
    // memory to the others
    const bool auto_budget = a.texture_budget_auto;
    ConvertSettings run = a; // what the attempts change
    if (a.stream_textures)
        run.upgrade_budget_bytes = static_cast<int64_t>(a.upgrade_budget * MIB);
    run.stream_growth_scale = a.stream_growth ? a.stream_growth : (a.memory_target <= FREE_CONSOLE_MIB ? CONSOLE_STREAM_GROWTH : 1.0);
    if (a.memory_target > FREE_CONSOLE_MIB)
        say("warning: the %g MiB memory target is over the %.1f MiB a console has free for a map: the map will load in Xenia only (its patch "
               "enlarging the game's memory pool on)\n",
               a.memory_target, FREE_CONSOLE_MIB);
    o.texture_budget = auto_budget ? (a.stream_textures ? 4096 * MIB : static_cast<uint64_t>(static_cast<int64_t>(a.memory_target * MIB)))
                                   : static_cast<uint64_t>(static_cast<int64_t>(a.texture_budget * MIB));
    o.eighth_levels = !a.keep_quarter;
    o.max_texture_size = a.max_texture_size;
    o.keep_mips = !a.no_mips;
    o.compress_textures = !a.no_compress;
    o.allow_unverified = a.allow_unverified;
    o.reference_techsets = a.reference_techsets;
    o.sounds_dir = a.sounds_dir;
    o.jobs = a.jobs;
    o.sound_rate = static_cast<uint32_t>(a.sound_rate);
    o.mono_sounds = a.mono_sounds;
    set_library_cache_dir(a.no_zone_cache ? fs::path() : default_zone_cache_dir());
    if (!a.ffmpeg.empty())
        set_ffmpeg_path(a.ffmpeg);
    auto encoder = std::make_shared<XmaEncoder>(a.xma_encoder.empty() ? find_xma2encode() : a.xma_encoder, a.xma_quality,
                                                a.no_sound_cache ? fs::path() : default_sound_cache_dir());
    if (!a.no_sounds && encoder->available())
        o.xma_encoder = encoder;
    else if (!a.no_sounds)
        o.log("warning: xma2encode.exe not found: sounds are not converted, the map will reference console sounds (run t4ff-cli setup)");
    if (o.xma_encoder && !a.sounds_dir.empty())
    {
        // the map's own streamed sounds (its .iwd files), before the conversion
        IwdLibrary map_sounds(a.map_iwds);
        auto ts = Clock::now();
        StreamStats s = convert_streamed_sounds(map_sounds, a.sounds_dir, *encoder, a.stream_rate, a.mono_streams, o.log, a.jobs);
        if (s.sounds)
        {
            std::string took;
            if (a.timings)
            {
                char buf[32];
                snprintf(buf, sizeof buf, " (%.1f s)", seconds_since(ts));
                took = buf;
            }
            say("streamed sounds: %d/%d converted, %.1f MiB -> %.1f MiB%s\n", s.converted, s.sounds, s.input_bytes / 1048576.0,
                s.output_bytes / 1048576.0, took.c_str());
        }
    }

    // The automatic texture budget: the textures get the main memory the target leaves, measured on
    // the converted map (converted again with less when it is over; sounds are encoded once).
    const int64_t target = static_cast<int64_t>(a.memory_target * MIB);
    std::optional<pair_measure> measured; // (texture budget, memory) of the last conversion the budget changed
    const int attempts = 5;
    Conversion conv;
    std::vector<uint8_t> bytes;
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        bool last = attempt == attempts - 1;
        conv = Conversion();
        conv = convert_map(paths, run, o);
        progress::step("Measuring the memory");
        bytes = write_zone(x360(), *conv.zone);
        int64_t used = static_cast<int64_t>(memory_bytes(block_sizes(bytes)));
        // over the target: what the PC versions of stock textures added gives way first, then the
        // textures' mip tails, in one go when the upgrades are not enough
        int64_t upgrade = run.stream_textures ? static_cast<int64_t>(conv.report.upgrade_bytes) : 0;
        bool tail = auto_budget && o.mip_tail && !a.keep_mip_tail && used - target > upgrade;
        if (used > target && (upgrade || tail) && !last)
        {
            std::vector<std::string> changes;
            if (upgrade)
            {
                run.upgrade_budget_bytes = std::max<int64_t>(0, upgrade - (used - target));
                changes.push_back(mib_text(static_cast<double>(*run.upgrade_budget_bytes)) + " MiB for the PC versions of stock textures");
            }
            if (tail)
            {
                o.mip_tail = false;
                changes.push_back("textures without their mip levels of 16 texels or less (the packed mip tail)");
            }
            say("memory: %s MiB of main memory, over the %.0f MiB target: converting again with %s\n", mib_text(static_cast<double>(used)).c_str(),
                   target / 1048576.0, py::join(changes, " and ").c_str());
            continue;
        }
        if (!auto_budget)
            break;
        int64_t textures = static_cast<int64_t>(texture_bytes(*conv.zone));
        const auto &steps = conv.report.steps;
        if (used > target && !steps.empty() && o.stream_steps.empty() && !last)
        {
            // the budget counts what the textures keep in the fastfile from now on: those that keep the
            // most lose a level first
            int64_t cut = std::min<int64_t>(used - target + MARGIN_MIB * static_cast<int64_t>(MIB),
                                            std::max<int64_t>(textures - MIN_TEXTURE_BUDGET_MIB * static_cast<int64_t>(MIB), 0));
            say("memory: %s MiB of main memory, over the %.0f MiB target: converting again with %s MiB less of textures in the fastfile (%zu streamed)\n",
                   mib_text(static_cast<double>(used)).c_str(), target / 1048576.0, mib_text(static_cast<double>(cut)).c_str(), steps.size());
            o.stream_steps = std::unordered_map<std::string, int>(steps.begin(), steps.end());
            o.texture_cut = static_cast<uint64_t>(cut);
            measured.reset();
            continue;
        }
        // what a byte less of planned textures saves: measured between two budgets, else estimated from
        // the share of the planned textures the fastfile keeps
        int64_t planned = static_cast<int64_t>(conv.planned);
        int64_t effective = std::min<int64_t>(static_cast<int64_t>(o.texture_budget), planned);
        double efficiency;
        if (measured && measured->budget > effective && measured->used > used)
            efficiency = static_cast<double>(measured->used - used) / static_cast<double>(measured->budget - effective);
        else
            efficiency = planned ? std::min(1.0, static_cast<double>(textures) / static_cast<double>(planned)) : 1.0;
        measured = pair_measure{effective, used};
        std::optional<int64_t> budget =
            !last ? next_texture_budget(used, target, planned, static_cast<int64_t>(o.texture_budget), efficiency) : std::nullopt;
        if (!budget)
        {
            if (used > target)
                say("warning: the map needs %s MiB of main memory, over the %.0f MiB target (%s MiB without its textures); the game has about %s MiB "
                       "free for a map's zone on a console, %g in Xenia\n",
                       mib_text(static_cast<double>(used)).c_str(), target / 1048576.0, mib_text(static_cast<double>(used - textures)).c_str(),
                       mib_text(FREE_CONSOLE_MIB * 1048576.0).c_str(), FREE_MIB);
            else
                say("memory: %s MiB of main memory of the %.0f MiB target, %s MiB of it textures\n", mib_text(static_cast<double>(used)).c_str(),
                       target / 1048576.0, mib_text(static_cast<double>(textures)).c_str());
            break;
        }
        say("memory: %s MiB of main memory, over the %.0f MiB target: converting again with %s MiB of textures\n",
               mib_text(static_cast<double>(used)).c_str(), target / 1048576.0, mib_text(static_cast<double>(*budget)).c_str());
        o.texture_budget = static_cast<uint64_t>(*budget);
        o.texture_cut = 0;
    }
    std::vector<std::unique_ptr<ZoneConverter>> &convs = conv.convs;
    std::unique_ptr<Zone> &main_zone = conv.zone;
    IwdLibrary &map_files = *conv.map_files;
    auto phase = Clock::now();
    auto lap = [&](const char *what) {
        if (a.timings)
            say("time: %s %.2f s\n", what, seconds_since(phase));
        phase = Clock::now();
    };
    if (!a.out.empty())
        write_file(a.out, bytes);
    if (!a.dump.empty())
        write_text(a.dump, dump_text(main_zone.get()));
    if (!a.ff.empty())
        write_file(a.ff, fastfile_bytes(true, bytes));
    fs::path out_dir = a.out_dir.empty() ? a.sounds_dir : a.out_dir;
    if (!out_dir.empty())
        write_console_zone(bytes, out_dir / (a.map_name + ".ff"), a.jobs);
    lap("the fastfile");
    if (!out_dir.empty() && fs::is_directory(out_dir / "sounds"))
    {
        UpgradeStats up = upgrade_stream_files(out_dir / "sounds", o.log);
        if (up.upgraded)
            say("streamed sounds: %d kept from an earlier conversion rewritten in the game's layout (4 KiB blocks)\n", up.upgraded);
    }
    if (!out_dir.empty())
        write_map_files(a, o, convs, *main_zone, map_files, out_dir);
    lap("the map's files");
    if (a.timings)
    {
        if (o.xma_encoder)
            say("sound cache: %d encodings reused\n", o.xma_encoder->cache_hits());
        say("zone %.1f MiB in %.2f s; memory: %s\n", bytes.size() / 1048576.0, seconds_since(t0), memory_text().c_str());
    }
    return 0;
}

} // namespace t4ff

namespace t4ff
{
void write_console_zone(const std::vector<uint8_t> &zone, const fs::path &target, int jobs, const std::function<void(const std::string &)> &log)
{
    Sink sink(log);
    write_console_zone(zone, target, jobs);
}
} // namespace t4ff
