#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

// The fastfile container (the Python t4ff's fastfile.py): an "IWffu100" header, the T4 version in
// the platform's byte order, then the zone as one zlib stream.
namespace t4ff
{
constexpr uint32_t VERSION_T4 = 0x183;

struct FastFileError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct FastFile
{
    bool big_endian = false; // true: Xbox 360
    std::vector<uint8_t> zone;
};

std::vector<uint8_t> read_file(const std::filesystem::path &path);
void write_file(const std::filesystem::path &path, const std::vector<uint8_t> &data);

// Whether the 12 byte fastfile header (magic and version) is the Xbox 360's; throws when it is not a
// World at War fastfile.
bool fastfile_big_endian(const uint8_t *header, size_t size, const std::string &what);

FastFile read_fastfile(const std::filesystem::path &path);
FastFile parse_fastfile(const std::vector<uint8_t> &file, const std::string &what);

// One zlib stream of data, compressed on jobs threads (0: one per processor). As pigz does, 1 MiB
// chunks are deflated separately, each one primed with the 32 KiB before it and ended on a byte
// boundary, so their concatenation is one deflate stream: the bytes are the Python t4ff's.
std::vector<uint8_t> compress(const std::vector<uint8_t> &data, int level = 9, int jobs = 0);

std::vector<uint8_t> fastfile_bytes(bool big_endian, const std::vector<uint8_t> &zone, int level = 9, int jobs = 0);
void write_fastfile(const std::filesystem::path &path, bool big_endian, const std::vector<uint8_t> &zone, int level = 9, int jobs = 0);
} // namespace t4ff
