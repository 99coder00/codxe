#include "gui/settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <sstream>

#include "convert/library.h"
#include "core/json.h"

namespace t4ff::gui
{
namespace
{
std::string format_g(double v)
{
    // Python's f"{v:g}"
    char buf[64];
    snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

std::string read_text(const fs::path &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

// -- reading

void read_string(const json::Value &v, const char *key, std::string &out)
{
    if (const json::Value *x = v.get(key); x && x->kind == json::Value::Kind::String)
        out = x->string;
}

void read_strings(const json::Value &v, const char *key, std::vector<std::string> &out)
{
    const json::Value *x = v.get(key);
    if (!x || x->kind != json::Value::Kind::Array)
        return;
    out.clear();
    for (const json::Value &item : x->array)
        if (item.kind == json::Value::Kind::String && !item.string.empty())
            out.push_back(item.string);
}

void read_bool(const json::Value &v, const char *key, bool &out)
{
    if (const json::Value *x = v.get(key); x && x->kind == json::Value::Kind::Bool)
        out = x->boolean;
}

void read_number(const json::Value &v, const char *key, double &out)
{
    if (const json::Value *x = v.get(key); x && x->kind == json::Value::Kind::Number && std::isfinite(x->number))
        out = x->number;
}

void read_int(const json::Value &v, const char *key, int &out)
{
    if (const json::Value *x = v.get(key); x && x->kind == json::Value::Kind::Number && std::isfinite(x->number))
        out = static_cast<int>(x->number);
}

void read_fields(const json::Value &v, Settings &s)
{
    read_string(v, "output", s.output);
    read_string(v, "xma_encoder", s.xma_encoder);
    read_strings(v, "console_zones", s.console_zones);
    read_strings(v, "iwds", s.iwds);
    if (const json::Value *x = v.get("texture_budget"))
    {
        // the Python window saved numbers too
        if (x->kind == json::Value::Kind::String)
            s.texture_budget = budget_text(x->string);
        else if (x->kind == json::Value::Kind::Number)
            s.texture_budget = budget_text(format_g(x->number));
    }
    read_number(v, "memory_target", s.memory_target);
    read_int(v, "max_texture_size", s.max_texture_size);
    read_bool(v, "no_mips", s.no_mips);
    read_bool(v, "no_compress", s.no_compress);
    read_bool(v, "stream_textures", s.stream_textures);
    read_bool(v, "keep_quarter", s.keep_quarter);
    read_bool(v, "keep_mip_tail", s.keep_mip_tail);
    read_number(v, "upgrade_budget", s.upgrade_budget);
    read_number(v, "stream_growth", s.stream_growth);
    read_string(v, "deep_stream", s.deep_stream);
    read_int(v, "sound_rate", s.sound_rate);
    read_int(v, "stream_rate", s.stream_rate);
    read_int(v, "xma_quality", s.xma_quality);
    read_int(v, "max_loaded_sounds", s.max_loaded_sounds);
    read_int(v, "jobs", s.jobs);
    read_number(v, "loaded_sound_memory", s.loaded_sound_memory);
    read_bool(v, "mono_sounds", s.mono_sounds);
    read_bool(v, "mono_streams", s.mono_streams);
    read_bool(v, "no_sounds", s.no_sounds);
    read_bool(v, "no_mod", s.no_mod);
    read_bool(v, "no_patch", s.no_patch);
    read_bool(v, "load_zone", s.load_zone);
    read_bool(v, "t4_layout", s.t4_layout);
    read_bool(v, "allow_unverified", s.allow_unverified);
    read_bool(v, "codxe_settings", s.codxe_settings);
    read_bool(v, "codxe_start_map", s.codxe_start_map);
    read_bool(v, "codxe_log_console", s.codxe_log_console);
    read_bool(v, "codxe_thread_watch", s.codxe_thread_watch);
    read_bool(v, "codxe_dump_rawfile", s.codxe_dump_rawfile);
    read_bool(v, "codxe_dump_map_ents", s.codxe_dump_map_ents);
    read_string(v, "codxe_start_command", s.codxe_start_command);
    read_string(v, "codxe_active_mod", s.codxe_active_mod);
    read_bool(v, "advanced", s.advanced);
    read_string(v, "theme", s.theme);
    read_int(v, "window_w", s.window_w);
    read_int(v, "window_h", s.window_h);
    read_bool(v, "maximized", s.maximized);
}

// -- writing

std::string quoted(const std::string &s)
{
    std::string out = "\"";
    for (char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            }
            else
                out += c;
        }
    }
    return out + "\"";
}

struct Writer
{
    std::string text = "{";
    bool first = true;

    void key(const char *k)
    {
        text += first ? "\n  " : ",\n  ";
        first = false;
        text += quoted(k) + ": ";
    }
    void put(const char *k, const std::string &v)
    {
        key(k);
        text += quoted(v);
    }
    void put(const char *k, const std::vector<std::string> &v)
    {
        key(k);
        if (v.empty())
        {
            text += "[]";
            return;
        }
        text += "[";
        for (size_t i = 0; i < v.size(); ++i)
            text += (i ? ",\n    " : "\n    ") + quoted(v[i]);
        text += "\n  ]";
    }
    void put(const char *k, bool v)
    {
        key(k);
        text += v ? "true" : "false";
    }
    void put(const char *k, int v)
    {
        key(k);
        text += std::to_string(v);
    }
    void put(const char *k, double v)
    {
        key(k);
        char buf[64];
        snprintf(buf, sizeof buf, "%.17g", v);
        text += buf;
        if (text.find_first_of(".eE", text.size() - strlen(buf)) == std::string::npos)
            text += ".0";
    }
};

fs::path appdata()
{
    const wchar_t *base = _wgetenv(L"APPDATA");
    if (base && *base)
        return base;
    const wchar_t *home = _wgetenv(L"USERPROFILE");
    return fs::path(home ? home : L".") / L".config";
}
} // namespace

fs::path path_of(const std::string &utf8)
{
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8_of(const fs::path &p)
{
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::wstring wide_of(const std::string &utf8)
{
    return path_of(utf8).wstring();
}

fs::path settings_path()
{
    return appdata() / L"t4ff" / L"window.json";
}

fs::path python_settings_path()
{
    return appdata() / L"t4ff" / L"gui.json";
}

Settings load_settings(const fs::path &path, const fs::path &python)
{
    Settings s;
    std::error_code ec;
    bool own = fs::exists(path, ec);
    std::string text = read_text(own ? path : python);
    if (text.empty())
        return s;
    json::Value v;
    try
    {
        v = json::parse(text);
    }
    catch (const std::exception &)
    {
        return s;
    }
    if (v.kind != json::Value::Kind::Object)
        return s;
    read_fields(v, s);
    if (!own)
    {
        // the Python window's migrations (gui.Settings.load)
        int version = static_cast<int>(v.integer_or("version", 1));
        if (version < 2)
            s.t4_layout = true; // saved before the t4 layout became the default
        if (version < 3)
        {
            s.texture_budget = "auto"; // and before the automatic texture budget and loading screens for every map
            s.load_zone = true;
        }
        if (version < 6)
            s.memory_target = MEMORY_TARGET_MIB; // the target had other meanings before
        // the Python window took the encoder it found as its setting: the window finds it each time
        s.xma_encoder.clear();
    }
    s.memory_target = std::clamp(s.memory_target, 64.0, 1024.0);
    s.xma_quality = std::clamp(s.xma_quality, 1, 100);
    s.max_loaded_sounds = std::max(0, s.max_loaded_sounds);
    s.jobs = std::max(0, s.jobs);
    if (s.theme != "dark" && s.theme != "light")
        s.theme = "system";
    if (s.codxe_start_command != "map")
        s.codxe_start_command = "devmap";
    return s;
}

void save_settings(const Settings &s, const fs::path &path)
{
    Writer w;
    w.put("output", s.output);
    w.put("xma_encoder", s.xma_encoder);
    w.put("console_zones", s.console_zones);
    w.put("iwds", s.iwds);
    w.put("texture_budget", budget_text(s.texture_budget));
    w.put("memory_target", s.memory_target);
    w.put("max_texture_size", s.max_texture_size);
    w.put("no_mips", s.no_mips);
    w.put("no_compress", s.no_compress);
    w.put("stream_textures", s.stream_textures);
    w.put("keep_quarter", s.keep_quarter);
    w.put("keep_mip_tail", s.keep_mip_tail);
    w.put("upgrade_budget", s.upgrade_budget);
    w.put("stream_growth", s.stream_growth);
    w.put("deep_stream", s.deep_stream);
    w.put("sound_rate", s.sound_rate);
    w.put("stream_rate", s.stream_rate);
    w.put("xma_quality", s.xma_quality);
    w.put("max_loaded_sounds", s.max_loaded_sounds);
    w.put("loaded_sound_memory", s.loaded_sound_memory);
    w.put("jobs", s.jobs);
    w.put("mono_sounds", s.mono_sounds);
    w.put("mono_streams", s.mono_streams);
    w.put("no_sounds", s.no_sounds);
    w.put("no_mod", s.no_mod);
    w.put("no_patch", s.no_patch);
    w.put("load_zone", s.load_zone);
    w.put("t4_layout", s.t4_layout);
    w.put("allow_unverified", s.allow_unverified);
    w.put("codxe_settings", s.codxe_settings);
    w.put("codxe_start_map", s.codxe_start_map);
    w.put("codxe_log_console", s.codxe_log_console);
    w.put("codxe_thread_watch", s.codxe_thread_watch);
    w.put("codxe_dump_rawfile", s.codxe_dump_rawfile);
    w.put("codxe_dump_map_ents", s.codxe_dump_map_ents);
    w.put("codxe_start_command", s.codxe_start_command);
    w.put("codxe_active_mod", s.codxe_active_mod);
    w.put("advanced", s.advanced);
    w.put("theme", s.theme);
    w.put("window_w", s.window_w);
    w.put("window_h", s.window_h);
    w.put("maximized", s.maximized);
    w.text += "\n}\n";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // a whole file or none: written next to it, then put in its place
    fs::path temp = path;
    temp += L".tmp";
    {
        std::ofstream f(temp, std::ios::binary);
        if (!f)
            return;
        f.write(w.text.data(), static_cast<std::streamsize>(w.text.size()));
        if (!f)
            return;
    }
    fs::rename(temp, path, ec);
}

std::string budget_text(const std::string &value)
{
    std::string text;
    for (char c : value)
        if (!std::isspace(static_cast<unsigned char>(c)))
            text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (text.empty() || text == "auto")
        return "auto";
    char *end = nullptr;
    double v = strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size())
        return "auto";
    return format_g(std::max(0.0, v));
}

std::vector<std::string> convert_args(const Settings &s, const MapEntry &m)
{
    // gui.convert_args, then the options the Python window does not have
    std::vector<std::string> args{"convert", m.input, "-o", s.output};
    if (!s.xma_encoder.empty())
        args.insert(args.end(), {"--xma-encoder", s.xma_encoder});
    for (const std::string &p : s.console_zones)
        args.insert(args.end(), {"--console-zone", p});
    for (const std::string &p : s.iwds)
        args.insert(args.end(), {"--iwd", p});
    if (budget_text(s.texture_budget) != "auto")
        args.insert(args.end(), {"--texture-budget", budget_text(s.texture_budget)});
    if (s.memory_target != MEMORY_TARGET_MIB)
        args.insert(args.end(), {"--memory-target", format_g(s.memory_target)});
    if (s.max_texture_size)
        args.insert(args.end(), {"--max-texture-size", std::to_string(s.max_texture_size)});
    if (s.sound_rate)
        args.insert(args.end(), {"--sound-rate", std::to_string(s.sound_rate)});
    if (s.stream_rate)
        args.insert(args.end(), {"--stream-rate", std::to_string(s.stream_rate)});
    if (s.xma_quality != 60)
        args.insert(args.end(), {"--xma-quality", std::to_string(s.xma_quality)});
    if (s.max_loaded_sounds != DEFAULT_MAX_LOADED_SOUNDS)
        args.insert(args.end(), {"--max-loaded-sounds", std::to_string(s.max_loaded_sounds)});
    std::string name = m.name;
    name.erase(0, name.find_first_not_of(" \t\r\n"));
    name.erase(name.find_last_not_of(" \t\r\n") + 1);
    if (!name.empty())
        args.insert(args.end(), {"--name", name});
    if (!m.loading_image.empty())
        args.insert(args.end(), {"--loading-image", m.loading_image});
    const std::pair<bool, const char *> flags[] = {{s.mono_sounds, "--mono-sounds"}, {s.mono_streams, "--mono-streams"}, {s.no_mips, "--no-mips"},
                                                   {s.no_compress, "--no-compress"}, {s.no_mod, "--no-mod"},             {s.no_patch, "--no-patch"},
                                                   {s.no_sounds, "--no-sounds"}};
    for (const auto &[on, flag] : flags)
        if (on)
            args.emplace_back(flag);
    if (!s.t4_layout)
        args.emplace_back("--no-t4-layout");
    if (!s.load_zone)
        args.emplace_back("--no-load-zone");
    // the native window's own: streaming, the loaded sound memory, threads, unverified assets
    if (s.stream_textures)
    {
        args.emplace_back("--stream-textures");
        if (s.upgrade_budget != 96)
            args.insert(args.end(), {"--upgrade-budget", format_g(s.upgrade_budget)});
        if (!s.deep_stream.empty())
            args.insert(args.end(), {"--deep-stream", s.deep_stream});
        if (s.stream_growth)
            args.insert(args.end(), {"--stream-growth", format_g(s.stream_growth)});
        if (s.keep_quarter)
            args.emplace_back("--keep-quarter");
    }
    if (s.keep_mip_tail)
        args.emplace_back("--keep-mip-tail");
    if (s.loaded_sound_memory != DEFAULT_LOADED_SOUND_MIB)
        args.insert(args.end(), {"--loaded-sound-memory", format_g(s.loaded_sound_memory)});
    if (s.jobs)
        args.insert(args.end(), {"--jobs", std::to_string(s.jobs)});
    if (s.allow_unverified)
        args.emplace_back("--allow-unverified");
    return args;
}

std::vector<std::string> check_settings(const Settings &s, const MapEntry &m)
{
    std::vector<std::string> problems;
    std::error_code ec;
    if (m.input.empty())
        problems.push_back("Choose the PC usermap folder (or its map .ff).");
    else if (!fs::exists(path_of(m.input), ec))
        problems.push_back("The usermap folder does not exist: " + m.input);
    if (s.output.empty())
        problems.push_back("Choose an output folder.");
    std::vector<std::string> paths = s.console_zones;
    paths.insert(paths.end(), s.iwds.begin(), s.iwds.end());
    if (!s.xma_encoder.empty())
        paths.push_back(s.xma_encoder);
    if (!m.loading_image.empty())
        paths.push_back(m.loading_image);
    for (const std::string &p : paths)
        if (!fs::exists(path_of(p), ec))
            problems.push_back("Not found: " + p);
    return problems;
}

namespace
{
bool has_game_zone(const std::string &path)
{
    std::error_code ec;
    fs::path p = path_of(path);
    if (!fs::is_directory(p, ec))
        return is_game_zone(p);
    for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
         it.increment(ec))
    {
        if (ec)
            break;
        std::wstring ext = it->path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".ff" && is_game_zone(it->path()))
            return true;
    }
    return false;
}
} // namespace

std::vector<std::string> advice(const Settings &s, bool encoder_found)
{
    std::vector<std::string> notes;
    if (s.console_zones.empty())
        notes.push_back("No Xbox 360 fastfiles given: technique sets (shaders) and stock assets are only referenced by name.");
    else if (std::none_of(s.console_zones.begin(), s.console_zones.end(), has_game_zone))
        notes.push_back("None of the Xbox 360 fastfiles is one of the game's own zones (common.ff, code_post_gfx.ff, patch.ff): add CoD Xenon's "
                        "_codxe\\t4\\zone folder (or their whole _codxe\\t4 folder), so what the game already has stays a reference and the player "
                        "animations maps lack are added.");
    if (s.xma_encoder.empty() && !encoder_found && !s.no_sounds)
        notes.push_back("No xma2encode.exe found: sounds are not encoded (loaded sounds come from the 360 fastfiles, if they have them).");
    return notes;
}
} // namespace t4ff::gui
