#include "core/zone_cache.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <functional>
#include <vector>

#include <windows.h>

#include "core/fastfile.h"
#include "zlib.h"

namespace t4ff
{
namespace
{
namespace fs = std::filesystem;

const char MAGIC[8] = {'T', '4', 'F', 'F', 'Z', 'C', '0', '1'};

#pragma pack(push, 1)
struct CacheHeader
{
    char magic[8];
    uint64_t path_hash;
    uint64_t source_size;
    uint64_t source_mtime; // FILETIME of the fastfile's last write
    uint64_t zone_size;
    uint32_t big_endian;
    uint8_t reserved[20];
};
#pragma pack(pop)
static_assert(sizeof(CacheHeader) == 64);

struct Handle
{
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE handle) : h(handle)
    {
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    ~Handle()
    {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    bool ok() const
    {
        return h && h != INVALID_HANDLE_VALUE;
    }
};

class MappedZoneBytes : public ZoneBytes
{
  public:
    MappedZoneBytes(HANDLE mapping, const void *view, size_t size) : mapping_(mapping), view_(view)
    {
        data_ = static_cast<const uint8_t *>(view) + sizeof(CacheHeader);
        size_ = size;
    }
    ~MappedZoneBytes() override
    {
        UnmapViewOfFile(view_);
        CloseHandle(mapping_);
    }

  private:
    HANDLE mapping_;
    const void *view_;
};

uint64_t path_hash(const fs::path &path)
{
    std::wstring s = fs::absolute(path).lexically_normal().wstring();
    uint64_t h = 1469598103934665603ull; // FNV-1a
    for (wchar_t c : s)
    {
        c = static_cast<wchar_t>(std::towlower(c == L'/' ? L'\\' : c));
        for (int i = 0; i < 2; ++i)
        {
            h ^= static_cast<uint8_t>(c >> (8 * i));
            h *= 1099511628211ull;
        }
    }
    return h;
}

bool source_info(const fs::path &path, uint64_t &size, uint64_t &mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
        return false;
    size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    mtime = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return true;
}

// The cache file mapped when it holds the zone expected.
std::shared_ptr<const ZoneBytes> map_cache(const fs::path &file, const CacheHeader &expect, bool &big_endian)
{
    Handle f(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                         nullptr));
    if (!f.ok())
        return nullptr;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(f.h, &size) || static_cast<uint64_t>(size.QuadPart) < sizeof(CacheHeader))
        return nullptr;
    CacheHeader h;
    DWORD got = 0;
    if (!ReadFile(f.h, &h, sizeof h, &got, nullptr) || got != sizeof h)
        return nullptr;
    if (std::memcmp(h.magic, MAGIC, 8) != 0 || h.path_hash != expect.path_hash || h.source_size != expect.source_size ||
        h.source_mtime != expect.source_mtime || static_cast<uint64_t>(size.QuadPart) != sizeof(CacheHeader) + h.zone_size)
        return nullptr;
    HANDLE mapping = CreateFileMappingW(f.h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping)
        return nullptr;
    const void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view)
    {
        CloseHandle(mapping);
        return nullptr;
    }
    big_endian = h.big_endian != 0;
    return std::make_shared<MappedZoneBytes>(mapping, view, static_cast<size_t>(h.zone_size));
}

void write_all(HANDLE h, const void *data, size_t size, const fs::path &file)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    while (size)
    {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 30)), wrote = 0;
        if (!WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0)
            throw std::runtime_error(file.string() + ": write failed (disk full?)");
        p += wrote;
        size -= wrote;
    }
}

// Inflates the zone of the fastfile ff a few MiB at a time, handing each piece to sink; returns whether
// it is the Xbox 360's.
bool stream_inflate(const fs::path &ff, const std::function<void(const uint8_t *, size_t)> &sink)
{
    Handle in(CreateFileW(ff.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!in.ok())
        throw FastFileError(ff.string() + ": cannot open");
    uint8_t head[12];
    DWORD got = 0;
    if (!ReadFile(in.h, head, 12, &got, nullptr))
        throw FastFileError(ff.string() + ": read failed");
    bool big_endian = fastfile_big_endian(head, got, ff.string());

    z_stream z{};
    if (inflateInit(&z) != Z_OK)
        throw FastFileError("inflateInit failed");
    std::vector<uint8_t> in_buf(1 << 20), out_buf(4 << 20);
    int rc = Z_OK;
    bool input_done = false;
    try
    {
        while (rc != Z_STREAM_END)
        {
            if (z.avail_in == 0 && !input_done)
            {
                DWORD n = 0;
                if (!ReadFile(in.h, in_buf.data(), static_cast<DWORD>(in_buf.size()), &n, nullptr))
                    throw FastFileError(ff.string() + ": read failed");
                input_done = n == 0;
                z.next_in = in_buf.data();
                z.avail_in = n;
            }
            z.next_out = out_buf.data();
            z.avail_out = static_cast<uInt>(out_buf.size());
            rc = inflate(&z, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END && !(rc == Z_BUF_ERROR && !input_done))
                throw FastFileError(ff.string() + (input_done ? ": truncated zone (incomplete zlib stream)"
                                                              : ": corrupt zone (zlib error " + std::to_string(rc) + ")"));
            sink(out_buf.data(), out_buf.size() - z.avail_out);
        }
    }
    catch (...)
    {
        inflateEnd(&z);
        throw;
    }
    inflateEnd(&z);
    return big_endian;
}

// Inflates the fastfile into the cache file tmp, header first; returns the header written.
CacheHeader inflate_to(const fs::path &ff, const fs::path &tmp, CacheHeader h)
{
    Handle out(CreateFileW(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!out.ok())
        throw std::runtime_error(tmp.string() + ": cannot create");
    CacheHeader blank{};
    write_all(out.h, &blank, sizeof blank, tmp);
    uint64_t total = 0;
    h.big_endian = stream_inflate(ff, [&](const uint8_t *data, size_t size) {
        write_all(out.h, data, size, tmp);
        total += size;
    }) ? 1 : 0;
    std::memcpy(h.magic, MAGIC, 8);
    h.zone_size = total;
    LARGE_INTEGER zero{};
    SetFilePointerEx(out.h, zero, nullptr, FILE_BEGIN);
    write_all(out.h, &h, sizeof h, tmp);
    return h;
}

// A zone in memory: address space reserved for the largest zone, committed as the zone is inflated
// (no buffer grown by copying, nothing committed past its end).
class ReservedZoneBytes : public ZoneBytes
{
  public:
    static constexpr size_t RESERVE = size_t(16) << 30;
    static constexpr size_t COMMIT_STEP = size_t(64) << 20;

    ReservedZoneBytes()
    {
        base_ = static_cast<uint8_t *>(VirtualAlloc(nullptr, RESERVE, MEM_RESERVE, PAGE_NOACCESS));
        if (!base_)
            throw std::runtime_error("cannot reserve address space for a zone");
        data_ = base_;
    }
    ~ReservedZoneBytes() override
    {
        VirtualFree(base_, 0, MEM_RELEASE);
    }

    void append(const uint8_t *data, size_t size)
    {
        if (size_ + size > committed_)
        {
            size_t want = (size_ + size + COMMIT_STEP - 1) / COMMIT_STEP * COMMIT_STEP;
            if (want > RESERVE || !VirtualAlloc(base_ + committed_, want - committed_, MEM_COMMIT, PAGE_READWRITE))
                throw std::runtime_error("out of memory for a zone");
            committed_ = want;
        }
        std::memcpy(base_ + size_, data, size);
        size_ += size;
    }
    // gives back the committed pages past the end
    void finish()
    {
        size_t used = (size_ + 4095) / 4096 * 4096;
        if (committed_ > used)
            VirtualFree(base_ + used, committed_ - used, MEM_DECOMMIT);
        committed_ = used;
    }

  private:
    uint8_t *base_ = nullptr;
    size_t committed_ = 0;
};
} // namespace

fs::path default_zone_cache_dir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    fs::path base = n && n < MAX_PATH ? fs::path(buf) : fs::temp_directory_path();
    return base / "t4ff" / "zone_cache";
}

OpenedZone open_zone(const fs::path &path, const fs::path &cache_dir)
{
    OpenedZone result;
    if (!cache_dir.empty())
    {
        try
        {
            CacheHeader expect{};
            expect.path_hash = path_hash(path);
            if (!source_info(path, expect.source_size, expect.source_mtime))
                throw FastFileError(path.string() + ": cannot open");
            char name[32];
            snprintf(name, sizeof name, "-%016llx.zone", static_cast<unsigned long long>(expect.path_hash));
            fs::path file = cache_dir / (path.stem().wstring() + fs::path(name).wstring());
            if (auto bytes = map_cache(file, expect, result.big_endian))
            {
                result.bytes = std::move(bytes);
                result.cached = result.cache_hit = true;
                return result;
            }
            fs::create_directories(cache_dir);
            fs::path tmp = file;
            tmp += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
            try
            {
                inflate_to(path, tmp, expect);
            }
            catch (...)
            {
                DeleteFileW(tmp.c_str());
                throw;
            }
            if (MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING))
                result.bytes = map_cache(file, expect, result.big_endian);
            else
            {
                // the old file is in use (another t4ff has it mapped): use ours, gone once unmapped
                result.bytes = map_cache(tmp, expect, result.big_endian);
                DeleteFileW(tmp.c_str());
            }
            if (result.bytes)
            {
                result.cached = true;
                return result;
            }
            result.cache_error = "the cache file just written could not be mapped";
        }
        catch (const FastFileError &)
        {
            throw; // the fastfile itself is bad
        }
        catch (const std::exception &e)
        {
            result.cache_error = e.what();
        }
    }
    auto bytes = std::make_shared<ReservedZoneBytes>();
    result.big_endian = stream_inflate(path, [&](const uint8_t *data, size_t size) { bytes->append(data, size); });
    bytes->finish();
    result.bytes = std::move(bytes);
    return result;
}
} // namespace t4ff
