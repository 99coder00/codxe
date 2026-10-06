#include "app/usermap.h"

#include <algorithm>
#include <cwctype>
#include <map>

namespace t4ff
{
namespace
{
std::wstring lower(std::wstring s)
{
    for (wchar_t &c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool ends_with(const std::wstring &s, const std::wstring &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::wstring &s, const std::wstring &prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}

std::string text(const fs::path &p)
{
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string text(const std::wstring &w)
{
    return text(fs::path(w));
}

// the names of a folder's files, in the order the system lists them (os.listdir)
std::vector<std::wstring> listdir(const fs::path &folder)
{
    std::vector<std::wstring> names;
    for (const auto &e : fs::directory_iterator(folder))
        names.push_back(e.path().filename().wstring());
    return names;
}

std::vector<fs::path> sorted_paths(std::vector<fs::path> paths)
{
    std::sort(paths.begin(), paths.end(), [](const fs::path &a, const fs::path &b) { return a.wstring() < b.wstring(); });
    return paths;
}
} // namespace

Usermap find_usermap(const fs::path &path)
{
    std::error_code ec;
    if (!fs::exists(path, ec))
        throw UserError(text(path) + ": not found");
    std::optional<std::wstring> name;
    fs::path folder = path;
    if (fs::is_regular_file(path, ec))
    {
        folder = fs::absolute(path).parent_path();
        std::wstring stem = path.stem().wstring();
        if (lower(stem) != L"mod")
        {
            for (const wchar_t *suffix : {L"_load", L"_patch"})
                if (ends_with(lower(stem), suffix))
                    stem = stem.substr(0, stem.size() - std::wcslen(suffix));
            name = stem;
        }
    }
    // {lower case name: path} of the folder's fastfiles, in the folder's order
    std::vector<std::pair<std::wstring, fs::path>> ffs;
    std::map<std::wstring, fs::path> by_name;
    for (const std::wstring &f : listdir(folder))
    {
        std::wstring l = lower(f);
        if (!ends_with(l, L".ff"))
            continue;
        if (by_name.emplace(l, folder / f).second)
            ffs.emplace_back(l, folder / f);
        else
            by_name[l] = folder / f;
    }
    if (!name)
    {
        std::vector<std::wstring> base;
        for (const auto &[l, p] : ffs)
            if (!ends_with(l, L"_load.ff") && !ends_with(l, L"_patch.ff") && l != L"mod.ff" && !starts_with(l, L"localized_"))
                base.push_back(l);
        if (base.size() != 1)
        {
            std::vector<std::wstring> sorted = base;
            std::sort(sorted.begin(), sorted.end());
            std::string list = "[";
            for (size_t i = 0; i < sorted.size(); ++i)
                list += (i ? ", '" : "'") + text(sorted[i]) + "'";
            throw UserError(text(folder) + ": expected exactly one map fastfile, found " + list + "]");
        }
        name = by_name[base[0]].stem().wstring();
    }
    std::wstring lname = lower(*name);
    auto found = by_name.find(lname + L".ff");
    if (found == by_name.end())
        throw UserError(text(folder) + ": " + text(*name) + ".ff not found");
    Usermap m;
    m.name = text(*name);
    m.map = found->second;
    auto role = [&](const std::wstring &f) -> std::optional<fs::path> {
        auto it = by_name.find(f);
        return it == by_name.end() ? std::nullopt : std::optional<fs::path>(it->second);
    };
    m.mod = role(L"mod.ff");
    m.patch = role(lname + L"_patch.ff");
    m.load = role(lname + L"_load.ff");
    std::vector<fs::path> iwds;
    for (const std::wstring &f : listdir(folder))
        if (ends_with(lower(f), L".iwd"))
            iwds.push_back(folder / f);
    m.iwds = sorted_paths(iwds);
    for (const auto &[l, p] : by_name) // sorted by lower case name
        if (starts_with(l, L"localized_"))
            m.localized.push_back(p);

    fs::path parent = fs::absolute(folder).parent_path();
    if (!m.mod && lower(parent.filename().wstring()) == L"usermaps")
    {
        fs::path mod_dir = parent.parent_path() / L"mods" / *name;
        if (fs::is_regular_file(mod_dir / L"mod.ff", ec))
        {
            m.mod = mod_dir / L"mod.ff";
            std::vector<fs::path> more, loc;
            for (const std::wstring &f : listdir(mod_dir))
            {
                std::wstring l = lower(f);
                if (ends_with(l, L".iwd"))
                    more.push_back(mod_dir / f);
                if (starts_with(l, L"localized_") && ends_with(l, L".ff"))
                    loc.push_back(mod_dir / f);
            }
            for (const fs::path &p : sorted_paths(more))
                m.iwds.push_back(p);
            for (const fs::path &p : sorted_paths(loc))
                m.localized.push_back(p);
        }
    }
    return m;
}
} // namespace t4ff
