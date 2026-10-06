#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Running other programs (xma2encode, FFmpeg): no console window, nothing on their input, their output
// and errors captured.
namespace t4ff
{
struct ProcessResult
{
    int exit_code = -1;
    std::vector<uint8_t> out;
    std::string err;
    bool timed_out = false;
};

// args[0] is the program; timeout in seconds (0: none). Throws std::runtime_error when it cannot start.
ProcessResult run_process(const std::vector<std::wstring> &args, double timeout = 0);

// A folder of its own under %TEMP%, removed with what it holds when the object goes.
class TempDir
{
  public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
    const std::filesystem::path &path() const
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

// the program on PATH (with .exe added), empty when none
std::filesystem::path find_on_path(const std::wstring &name);
} // namespace t4ff
