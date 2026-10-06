#include "app/deps.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>

#include <windows.h>

#include "audio/audio.h"
#include "core/zip.h"

namespace t4ff
{
namespace
{
constexpr const wchar_t *ENCODER_NAME = L"xma2encode.exe";

std::wstring lower(std::wstring s)
{
    for (wchar_t &c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

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

fs::path exe_dir()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path();
}

// xma2encode.exe under root (at most depth folders deep)
std::optional<fs::path> search(const fs::path &root, int depth)
{
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec))
        return std::nullopt;
    for (const wchar_t *sub : {L"bin\\win32", L"bin\\x64", L"bin\\x86", L"bin", L""})
    {
        fs::path p = fs::path(root) / sub / ENCODER_NAME;
        if (fs::is_regular_file(p, ec))
            return p;
    }
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
         it.increment(ec))
    {
        if (ec)
            break;
        if (it->is_regular_file(ec) && lower(it->path().filename().wstring()) == ENCODER_NAME)
            return it->path();
        if (it->is_directory(ec) && it.depth() + 1 >= depth)
            it.disable_recursion_pending();
    }
    return std::nullopt;
}

// the member of a .zip that is the encoder
std::optional<std::string> zip_member(const fs::path &path)
{
    try
    {
        ZipArchive zip(path);
        for (const auto &e : zip.entries())
        {
            std::string name = e.name;
            std::replace(name.begin(), name.end(), '\\', '/');
            std::string base = name.substr(name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1);
            std::transform(base.begin(), base.end(), base.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            if (base == "xma2encode.exe")
                return e.name;
        }
    }
    catch (const std::exception &)
    {
    }
    return std::nullopt;
}

fs::path downloads()
{
    const wchar_t *home = _wgetenv(L"USERPROFILE");
    return home ? fs::path(home) / L"Downloads" : fs::path();
}

// a folder holding unrelated files (Downloads, Desktop, the home folder)
bool common_folder(const fs::path &folder)
{
    const wchar_t *home = _wgetenv(L"USERPROFILE");
    if (!home)
        return false;
    std::wstring f = lower(fs::absolute(folder).lexically_normal().wstring());
    for (const wchar_t *sub : {L"", L"Downloads", L"Desktop"})
    {
        fs::path p = fs::path(home) / sub;
        if (lower(fs::absolute(p).lexically_normal().wstring()) == f || lower(fs::absolute(p).lexically_normal().wstring()) + L"\\" == f)
            return true;
    }
    return false;
}

// copies the encoder (and the DLLs next to it) from source into the bin folder
fs::path install(fs::path source, std::string member, const Log &log)
{
    fs::path bin = bin_dir();
    fs::create_directories(bin);
    fs::path target = bin / ENCODER_NAME;
    std::error_code ec;
    if (fs::is_directory(source, ec))
    {
        auto found = search(source, 6);
        if (!found)
            throw std::runtime_error("no xma2encode.exe in " + text(source));
        source = *found;
    }
    if (lower(source.extension().wstring()) == L".zip")
    {
        if (member.empty())
            member = zip_member(source).value_or("");
        std::replace(member.begin(), member.end(), '\\', '/');
        if (member.empty())
            throw std::runtime_error("no xma2encode.exe in " + text(source));
        std::string folder = member.substr(0, member.rfind('/') == std::string::npos ? 0 : member.rfind('/'));
        ZipArchive zip(source);
        for (const auto &e : zip.entries())
        {
            std::string clean = e.name;
            std::replace(clean.begin(), clean.end(), '\\', '/');
            std::string dir = clean.substr(0, clean.rfind('/') == std::string::npos ? 0 : clean.rfind('/'));
            if (dir != folder)
                continue;
            std::string base = clean.substr(clean.rfind('/') == std::string::npos ? 0 : clean.rfind('/') + 1);
            std::string lower_base = base;
            std::transform(lower_base.begin(), lower_base.end(), lower_base.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            fs::path out;
            if (clean == member)
                out = target;
            else if (lower_base.size() > 4 && lower_base.compare(lower_base.size() - 4, 4, ".dll") == 0)
                out = bin / from_utf8(base);
            else
                continue;
            std::vector<uint8_t> data = zip.read(e);
            std::ofstream f(out, std::ios::binary);
            f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        if (log)
            log("installed xma2encode.exe from " + text(source) + " into " + text(bin));
        return target;
    }
    if (!fs::is_regular_file(source, ec))
        throw std::runtime_error(text(source) + ": not found");
    if (fs::absolute(source) != fs::absolute(target))
    {
        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
        fs::path folder = fs::absolute(source).parent_path();
        if (!common_folder(folder))
            for (const auto &e : fs::directory_iterator(folder))
                if (lower(e.path().extension().wstring()) == L".dll")
                    fs::copy_file(e.path(), bin / e.path().filename(), fs::copy_options::overwrite_existing);
        if (log)
            log("installed xma2encode.exe from " + text(folder) + " into " + text(bin));
    }
    return target;
}
} // namespace

fs::path bin_dir()
{
    return exe_dir() / L"bin";
}

std::optional<fs::path> ensure_xma2encode(const fs::path &source_in, const Log &log)
{
    fs::path found = source_in;
    std::string member;
    if (found.empty())
    {
        found = find_xma2encode();
        if (found.empty())
        {
            // in a .zip of the Downloads folder
            fs::path dl = downloads();
            std::error_code ec;
            if (!dl.empty() && fs::is_directory(dl, ec))
            {
                std::vector<fs::path> zips;
                for (const auto &e : fs::directory_iterator(dl, ec))
                    if (lower(e.path().extension().wstring()) == L".zip")
                        zips.push_back(e.path());
                std::sort(zips.begin(), zips.end());
                for (const fs::path &z : zips)
                    if (auto m = zip_member(z))
                    {
                        found = z;
                        member = *m;
                        break;
                    }
            }
        }
        if (found.empty())
            return std::nullopt;
    }
    std::error_code ec;
    if (fs::is_directory(found, ec))
    {
        auto hit = search(found, 6);
        if (!hit)
            throw std::runtime_error("no xma2encode.exe in " + text(found));
        return hit;
    }
    if (!member.empty() || lower(found.extension().wstring()) == L".zip")
        return install(found, member, log);
    if (!fs::is_regular_file(found, ec))
        throw std::runtime_error(text(found) + ": not found");
    return found;
}

bool test_xma2encode(const fs::path &path, const Log &log)
{
    const int rate = 44100;
    Pcm pcm;
    pcm.rate = rate;
    pcm.channels = 1;
    for (int i = 0; i < rate / 4; ++i)
        pcm.samples.push_back(static_cast<int16_t>(std::sin(2 * 3.141592653589793 * 440 * i / rate) * 8000));
    XmaEncoder encoder(path);
    try
    {
        XmaStream stream = encoder.encode(std::move(pcm));
        size_t packets = stream.data.size() / XMA_PACKET_SIZE;
        if (!packets)
        {
            if (log)
                log("xma2encode.exe test failed: no XMA data");
            return false;
        }
        if (log)
            log("xma2encode.exe works (" + std::to_string(packets) + " XMA packet(s) for a 0.25 s test tone)");
        return true;
    }
    catch (const std::exception &e)
    {
        if (log)
            log(std::string("xma2encode.exe test failed: ") + e.what());
        return false;
    }
}

SetupState setup(const fs::path &encoder_source, bool test, const Log &log)
{
    SetupState state;
    fs::path ff = ffmpeg_exe();
    if (!ff.empty())
        state.ffmpeg = ff;
    log("FFmpeg: " + (ff.empty() ? std::string("MISSING (put ffmpeg.exe on PATH or next to t4ff-cli.exe; pictures are then scaled without it)") : text(ff)));
    try
    {
        state.xma2encode = ensure_xma2encode(encoder_source, log);
    }
    catch (const std::exception &e)
    {
        log("xma2encode.exe: cannot use " + (encoder_source.empty() ? std::string("the one found") : text(encoder_source)) + ": " + e.what());
    }
    if (!state.xma2encode)
        log("xma2encode.exe: NOT FOUND. xma2encode.exe is part of Microsoft's Xbox developer kits (Xbox 360 XDK, Xbox One XDK, or the Microsoft GDK "
            "with Xbox extensions) and cannot be downloaded automatically. Put it (or a .zip containing it) in your Downloads folder or in " +
            text(bin_dir()) + ", or give its path, and run setup again. Without it, sounds are not encoded.");
    else
        log("xma2encode.exe: " + text(*state.xma2encode));
    if (state.xma2encode && test && !test_xma2encode(*state.xma2encode, log))
        state.xma2encode.reset();
    return state;
}
} // namespace t4ff
