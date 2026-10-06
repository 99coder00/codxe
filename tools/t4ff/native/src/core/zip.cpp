#include "core/zip.h"

#include <algorithm>
#include <cstring>

#include <windows.h>

#include "zlib.h"

namespace t4ff
{
namespace
{
uint16_t u16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | p[1] << 8);
}
uint32_t u32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
} // namespace

ZipArchive::ZipArchive(const std::filesystem::path &path) : path_(path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw ZipError("cannot open " + path.string());
    handle_ = h;
    LARGE_INTEGER size;
    GetFileSizeEx(h, &size);
    file_size_ = static_cast<uint64_t>(size.QuadPart);

    // the end of central directory record, within the last 64 KiB (its comment) and 22 bytes
    size_t tail = static_cast<size_t>(std::min<uint64_t>(file_size_, 65535 + 22));
    std::vector<uint8_t> end(tail);
    read_at(file_size_ - tail, end.data(), tail);
    size_t at = std::string::npos;
    for (size_t i = tail >= 22 ? tail - 22 + 1 : 0; i-- > 0;)
    {
        if (u32(end.data() + i) == 0x06054b50)
        {
            at = i;
            break;
        }
    }
    if (at == std::string::npos)
        throw ZipError(path.string() + ": not a zip file");
    const uint8_t *e = end.data() + at;
    uint64_t eocd = file_size_ - tail + at;
    uint32_t count = u16(e + 10), cd_size = u32(e + 12), cd_offset = u32(e + 16);
    if (cd_offset == 0xFFFFFFFF || count == 0xFFFF)
        throw ZipError(path.string() + ": zip64 archives are not supported");
    // data prepended to the archive moves every offset (as Python's zipfile allows)
    uint64_t concat = eocd - cd_size - cd_offset;
    std::vector<uint8_t> cd(cd_size);
    read_at(cd_offset + concat, cd.data(), cd_size);
    for (size_t pos = 0; pos + 46 <= cd.size();)
    {
        const uint8_t *c = cd.data() + pos;
        if (u32(c) != 0x02014b50)
            throw ZipError(path.string() + ": bad central directory");
        Entry entry;
        entry.method = u16(c + 10);
        entry.crc = u32(c + 16);
        entry.compressed_size = u32(c + 20);
        entry.size = u32(c + 24);
        uint16_t name_len = u16(c + 28), extra_len = u16(c + 30), comment_len = u16(c + 32);
        entry.header_offset = u32(c + 42) + concat;
        if (pos + 46 + name_len > cd.size())
            throw ZipError(path.string() + ": bad central directory");
        entry.name.assign(reinterpret_cast<const char *>(c + 46), name_len);
        entries_.push_back(std::move(entry));
        pos += 46 + name_len + extra_len + comment_len;
    }
}

ZipArchive::~ZipArchive()
{
    if (handle_)
        CloseHandle(handle_);
}

void ZipArchive::read_at(uint64_t offset, void *out, size_t size) const
{
    uint8_t *dst = static_cast<uint8_t *>(out);
    while (size)
    {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 30)), got = 0;
        if (!ReadFile(handle_, dst, chunk, &got, &ov) || got == 0)
            throw ZipError(path_.string() + ": read failed");
        dst += got;
        offset += got;
        size -= got;
    }
}

std::vector<uint8_t> ZipArchive::read(const Entry &e) const
{
    uint8_t local[30];
    read_at(e.header_offset, local, sizeof local);
    if (u32(local) != 0x04034b50)
        throw ZipError(path_.string() + ": bad local header of " + e.name);
    uint64_t data_at = e.header_offset + 30 + u16(local + 26) + u16(local + 28);
    std::vector<uint8_t> packed(static_cast<size_t>(e.compressed_size));
    read_at(data_at, packed.data(), packed.size());
    auto checked = [&](std::vector<uint8_t> data) {
        uLong crc = 0;
        for (size_t at = 0; at < data.size();)
        {
            uInt n = static_cast<uInt>(std::min<size_t>(data.size() - at, 1u << 30));
            crc = crc32(crc, data.data() + at, n);
            at += n;
        }
        if (crc != e.crc)
            throw ZipError(path_.string() + ": bad CRC of " + e.name);
        return data;
    };
    if (e.method == 0)
        return checked(std::move(packed));
    if (e.method != 8)
        throw ZipError(path_.string() + ": " + e.name + ": unsupported compression method " + std::to_string(e.method));
    std::vector<uint8_t> out(static_cast<size_t>(e.size));
    z_stream z{};
    if (inflateInit2(&z, -15) != Z_OK)
        throw ZipError("inflateInit2 failed");
    z.next_in = packed.data();
    z.avail_in = static_cast<uInt>(packed.size());
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    int rc = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (rc != Z_STREAM_END || z.total_out != out.size())
        throw ZipError(path_.string() + ": " + e.name + ": corrupt deflate data");
    return checked(std::move(out));
}
} // namespace t4ff
