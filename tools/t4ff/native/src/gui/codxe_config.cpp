#include "gui/codxe_config.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "core/json.h"

namespace t4ff::gui
{
namespace
{
std::string utf8(const fs::path &p)
{
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
} // namespace

fs::path codxe_config_path(const fs::path &output, bool t4_layout)
{
    fs::path root = output / L"_codxe";
    return t4_layout ? root / L"t4" / L"codxe.json" : root / L"codxe.json";
}

std::string write_codxe_config(const fs::path &path, const CodxeConfig &c)
{
    json::Value doc;
    doc.kind = json::Value::Kind::Object;
    std::string newline = "\r\n"; // as CoD Xe's own file
    std::error_code ec;
    if (fs::exists(path, ec))
    {
        std::ifstream f(path, std::ios::binary);
        std::ostringstream text;
        text << f.rdbuf();
        std::string s = text.str();
        if (s.size() >= 3 && s.compare(0, 3, "\xEF\xBB\xBF") == 0)
            s.erase(0, 3);
        try
        {
            doc = json::parse(s);
        }
        catch (const std::exception &e)
        {
            throw std::runtime_error(utf8(path) + " is not valid JSON (" + e.what() + "): left as it is");
        }
        if (doc.kind != json::Value::Kind::Object)
            throw std::runtime_error(utf8(path) + " is not a JSON object: left as it is");
        if (s.find("\r\n") == std::string::npos && s.find('\n') != std::string::npos)
            newline = "\n";
    }
    std::string done;
    auto flag = [&](const char *key, bool on) {
        doc.set(key, json::Value::of(on));
        if (on)
            done += std::string(done.empty() ? "" : ", ") + key;
    };
    if (!c.active_mod.empty())
    {
        doc.set("active_mod", json::Value::of(c.active_mod));
        done += std::string(done.empty() ? "" : ", ") + "active_mod \"" + c.active_mod + "\"";
    }
    flag("dump_rawfile", c.dump_rawfile);
    flag("dump_map_ents", c.dump_map_ents);
    flag("log_console", c.log_console);
    flag("thread_watch", c.thread_watch);
    if (!c.start_map.empty())
    {
        std::string map = c.start_map.find(' ') == std::string::npos ? c.start_map : "\"" + c.start_map + "\"";
        std::string command = c.start_command + " " + map;
        doc.set("startup_command", json::Value::of(command));
        done += std::string(done.empty() ? "" : ", ") + "startup_command \"" + command + "\"";
    }
    else
        doc.erase("startup_command");

    fs::create_directories(path.parent_path(), ec);
    std::string text = json::dump(doc, newline);
    fs::path temp = path;
    temp += L".tmp";
    {
        std::ofstream f(temp, std::ios::binary);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f)
            throw std::runtime_error("cannot write " + utf8(temp));
    }
    fs::rename(temp, path, ec);
    if (ec)
    {
        fs::remove(temp, ec);
        throw std::runtime_error("cannot write " + utf8(path));
    }
    return utf8(path) + ": " + (done.empty() ? "debug switches off" : done) + (c.start_map.empty() ? ", no startup command" : "");
}
} // namespace t4ff::gui
