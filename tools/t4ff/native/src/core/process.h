#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Running other programs (xma2encode, FFmpeg): no console window, their input given (or nothing), their
// output and errors captured.
namespace t4ff
{
struct ProcessResult
{
    int exit_code = -1;
    std::vector<uint8_t> out;
    std::string err;
    bool timed_out = false;
};

// args[0] is the program; timeout in seconds (0: none); input: what it reads (nullptr: nothing). Throws
// std::runtime_error when it cannot start.
ProcessResult run_process(const std::vector<std::wstring> &args, double timeout = 0, const std::vector<uint8_t> *input = nullptr);

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

// starts a program (args[0]) and leaves it running on its own; false when it cannot start
bool launch(const std::vector<std::wstring> &args);

// one argument of a command line, as CommandLineToArgvW reads it back
std::wstring quote_argument(const std::wstring &arg);

// Full speed, also when the program is not in front: Windows 11 runs background processes in
// "efficiency mode" (EcoQoS: slower cores, lower clocks) unless they opt out.
void opt_out_of_power_throttling();
} // namespace t4ff
