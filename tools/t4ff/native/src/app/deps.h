#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

// What t4ff needs besides itself (the Python t4ff's deps.py): FFmpeg (scaled pictures, compressed
// sounds) and xma2encode.exe, the XMA encoder of Microsoft's Xbox developer kits, which cannot be
// downloaded. The encoder is used where a kit installed it, or found in the Downloads folder (inside a
// .zip too), and one in a .zip is extracted with the DLLs next to it into the bin folder next to
// t4ff-cli.exe, where every run finds it.
namespace t4ff
{
namespace fs = std::filesystem;
using Log = std::function<void(const std::string &)>;

// the folder the encoder is installed into (next to the program)
fs::path bin_dir();
// a usable encoder: source (the .exe, a folder or a .zip holding it) or the one found on this computer,
// installed from a .zip when it is in one; nothing when there is none. Throws when source has none.
std::optional<fs::path> ensure_xma2encode(const fs::path &source, const Log &log);
// encodes a short tone to check the encoder runs
bool test_xma2encode(const fs::path &path, const Log &log);

struct SetupState
{
    std::optional<fs::path> ffmpeg, xma2encode;
};
// finds (and installs) what is missing, and reports it
SetupState setup(const fs::path &encoder_source, bool test, const Log &log);
} // namespace t4ff
