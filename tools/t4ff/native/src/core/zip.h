#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Reading zip archives (.iwd files): stored and deflated entries, read on any thread.
namespace t4ff
{
struct ZipError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class ZipArchive
{
  public:
    struct Entry
    {
        std::string name; // as stored ('/' separated)
        uint16_t method = 0;
        uint32_t crc = 0;
        uint64_t compressed_size = 0;
        uint64_t size = 0;
        uint64_t header_offset = 0;
    };

    explicit ZipArchive(const std::filesystem::path &path);
    ~ZipArchive();
    ZipArchive(const ZipArchive &) = delete;
    ZipArchive &operator=(const ZipArchive &) = delete;

    const std::filesystem::path &path() const
    {
        return path_;
    }
    const std::vector<Entry> &entries() const
    {
        return entries_;
    }
    std::vector<uint8_t> read(const Entry &e) const;

  private:
    std::filesystem::path path_;
    void *handle_ = nullptr;
    uint64_t file_size_ = 0;
    std::vector<Entry> entries_;

    void read_at(uint64_t offset, void *out, size_t size) const;
};
} // namespace t4ff
