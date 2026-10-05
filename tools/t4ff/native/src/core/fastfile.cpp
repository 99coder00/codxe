#include "core/fastfile.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#include "zlib.h"

namespace t4ff
{
namespace
{
const char MAGIC_UNSIGNED[] = "IWffu100";
constexpr size_t COMPRESS_CHUNK = 1 << 20;

[[noreturn]] void zlib_error(const char *what, int code)
{
    throw FastFileError(std::string(what) + ": zlib error " + std::to_string(code));
}

// The whole of data as one zlib stream (Python's zlib.compress).
std::vector<uint8_t> compress_whole(const uint8_t *data, size_t size, int level)
{
    uLongf bound = compressBound(static_cast<uLong>(size));
    std::vector<uint8_t> out(bound);
    int rc = compress2(out.data(), &bound, data, static_cast<uLong>(size), level);
    if (rc != Z_OK)
        zlib_error("compress", rc);
    out.resize(bound);
    return out;
}

// One raw deflate part: data[start:end], primed with the 32 KiB before it, sync flushed (finished
// when it is the last part).
std::vector<uint8_t> compress_part(const std::vector<uint8_t> &data, size_t start, int level)
{
    size_t end = std::min(start + COMPRESS_CHUNK, data.size());
    z_stream z{};
    int rc = deflateInit2(&z, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    if (rc != Z_OK)
        zlib_error("deflateInit2", rc);
    if (start)
    {
        size_t dict = std::min<size_t>(start, 32768);
        rc = deflateSetDictionary(&z, data.data() + start - dict, static_cast<uInt>(dict));
        if (rc != Z_OK)
            zlib_error("deflateSetDictionary", rc);
    }
    std::vector<uint8_t> out(deflateBound(&z, static_cast<uLong>(end - start)) + 64);
    z.next_in = const_cast<Bytef *>(data.data() + start);
    z.avail_in = static_cast<uInt>(end - start);
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    // as Python: everything with no flush, then the flush on its own call
    rc = deflate(&z, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_BUF_ERROR)
        zlib_error("deflate", rc);
    int flush = end == data.size() ? Z_FINISH : Z_SYNC_FLUSH;
    for (;;)
    {
        if (z.avail_out == 0)
        {
            size_t used = out.size();
            out.resize(used * 2);
            z.next_out = out.data() + used;
            z.avail_out = static_cast<uInt>(out.size() - used);
        }
        rc = deflate(&z, flush);
        if (flush == Z_FINISH ? rc == Z_STREAM_END : (rc == Z_OK && z.avail_out != 0))
            break;
        if (rc != Z_OK && rc != Z_BUF_ERROR)
            zlib_error("deflate", rc);
    }
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}
} // namespace

std::vector<uint8_t> read_file(const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw FastFileError(path.string() + ": cannot open");
    std::streamsize size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size && !f.read(reinterpret_cast<char *>(data.data()), size))
        throw FastFileError(path.string() + ": read failed");
    return data;
}

void write_file(const std::filesystem::path &path, const std::vector<uint8_t> &data)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size())))
        throw FastFileError(path.string() + ": write failed");
}

FastFile parse_fastfile(const std::vector<uint8_t> &file, const std::string &what)
{
    if (file.size() < 12 || std::memcmp(file.data(), MAGIC_UNSIGNED, 8) != 0)
        throw FastFileError(what + ": unsupported fastfile magic (only unsigned IWffu100 fastfiles are supported)");
    const uint8_t *v = file.data() + 8;
    uint32_t le = uint32_t(v[0]) | uint32_t(v[1]) << 8 | uint32_t(v[2]) << 16 | uint32_t(v[3]) << 24;
    uint32_t be = uint32_t(v[3]) | uint32_t(v[2]) << 8 | uint32_t(v[1]) << 16 | uint32_t(v[0]) << 24;
    FastFile ff;
    if (le == VERSION_T4)
        ff.big_endian = false;
    else if (be == VERSION_T4)
        ff.big_endian = true;
    else
    {
        char buf[16];
        snprintf(buf, sizeof buf, "%#x", le);
        throw FastFileError(what + ": not a World at War fastfile (version " + buf + ")");
    }

    z_stream z{};
    int rc = inflateInit(&z);
    if (rc != Z_OK)
        zlib_error("inflateInit", rc);
    size_t in_size = file.size() - 12;
    ff.zone.resize(std::max<size_t>(in_size * 4, 1 << 16));
    z.next_in = const_cast<Bytef *>(file.data() + 12);
    size_t in_left = in_size;
    size_t produced = 0;
    for (;;)
    {
        if (produced == ff.zone.size())
            ff.zone.resize(ff.zone.size() * 2);
        uInt in_chunk = static_cast<uInt>(std::min<size_t>(in_left, 1u << 30));
        uInt out_chunk = static_cast<uInt>(std::min<size_t>(ff.zone.size() - produced, 1u << 30));
        z.avail_in = in_chunk;
        z.next_out = ff.zone.data() + produced;
        z.avail_out = out_chunk;
        rc = inflate(&z, Z_NO_FLUSH);
        in_left -= in_chunk - z.avail_in;
        produced += out_chunk - z.avail_out;
        if (rc == Z_STREAM_END)
            break;
        if (rc == Z_BUF_ERROR && in_left == 0)
        {
            inflateEnd(&z);
            throw FastFileError(what + ": truncated zone (incomplete zlib stream)");
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR)
        {
            inflateEnd(&z);
            throw FastFileError(what + ": corrupt zone (zlib error " + std::to_string(rc) + ")");
        }
    }
    inflateEnd(&z);
    ff.zone.resize(produced);
    ff.zone.shrink_to_fit();
    return ff;
}

FastFile read_fastfile(const std::filesystem::path &path)
{
    return parse_fastfile(read_file(path), path.string());
}

std::vector<uint8_t> compress(const std::vector<uint8_t> &data, int level, int jobs)
{
    if (jobs <= 0)
        jobs = std::max(1u, std::thread::hardware_concurrency());
    if (jobs <= 1 || data.size() <= 2 * COMPRESS_CHUNK)
        return compress_whole(data.data(), data.size(), level);

    size_t parts = (data.size() + COMPRESS_CHUNK - 1) / COMPRESS_CHUNK;
    std::vector<std::vector<uint8_t>> out(parts);
    std::atomic<size_t> next{0};
    std::vector<std::thread> threads;
    std::exception_ptr error;
    std::mutex error_lock;
    for (int t = 0; t < std::min<int>(jobs, static_cast<int>(parts)); ++t)
    {
        threads.emplace_back([&] {
            for (size_t i; (i = next++) < parts;)
            {
                try
                {
                    out[i] = compress_part(data, i * COMPRESS_CHUNK, level);
                }
                catch (...)
                {
                    std::lock_guard lock(error_lock);
                    error = std::current_exception();
                }
            }
        });
    }
    for (auto &t : threads)
        t.join();
    if (error)
        std::rethrow_exception(error);

    std::vector<uint8_t> header = compress_whole(nullptr, 0, level);
    size_t total = 2 + 4;
    for (const auto &part : out)
        total += part.size();
    std::vector<uint8_t> result;
    result.reserve(total);
    result.insert(result.end(), header.begin(), header.begin() + 2);
    for (const auto &part : out)
        result.insert(result.end(), part.begin(), part.end());
    uLong adler = adler32(0L, Z_NULL, 0);
    for (size_t at = 0; at < data.size();)
    {
        uInt n = static_cast<uInt>(std::min<size_t>(data.size() - at, 1u << 30));
        adler = adler32(adler, data.data() + at, n);
        at += n;
    }
    result.push_back(uint8_t(adler >> 24));
    result.push_back(uint8_t(adler >> 16));
    result.push_back(uint8_t(adler >> 8));
    result.push_back(uint8_t(adler));
    return result;
}

std::vector<uint8_t> fastfile_bytes(bool big_endian, const std::vector<uint8_t> &zone, int level, int jobs)
{
    std::vector<uint8_t> out(MAGIC_UNSIGNED, MAGIC_UNSIGNED + 8);
    uint32_t v = VERSION_T4;
    if (big_endian)
        out.insert(out.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)});
    else
        out.insert(out.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)});
    std::vector<uint8_t> body = compress(zone, level, jobs);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void write_fastfile(const std::filesystem::path &path, bool big_endian, const std::vector<uint8_t> &zone, int level, int jobs)
{
    write_file(path, fastfile_bytes(big_endian, zone, level, jobs));
}
} // namespace t4ff
