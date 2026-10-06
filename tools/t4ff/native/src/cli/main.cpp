// t4ff-cli: the command line of the native t4ff.
//
//   t4ff-cli info <fastfile> [--list]
//   t4ff-cli roundtrip [--compress] [--jobs N] [--zone-cache | --zone-cache-dir DIR] <fastfile or folder>...
//   t4ff-cli dump <fastfile> <out.txt>
//   t4ff-cli rewrite <fastfile> <out.zone>
//   t4ff-cli bench [--jobs N] [--keep] [--zone-cache | --zone-cache-dir DIR] <fastfile or folder>...
//   t4ff-cli textures [--jobs N] <out.txt> <iwd or folder>...
//   t4ff-cli texbench [--jobs N] <iwd or folder>...
//   t4ff-cli cache [--clear] [--zone-cache-dir DIR]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include "core/fastfile.h"
#include "core/image.h"
#include "core/platforms.h"
#include "core/threads.h"
#include "core/zone.h"
#include "core/zone_cache.h"
#include "zlib.h"

namespace fs = std::filesystem;
using namespace t4ff;

namespace
{
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string utf8(const fs::path &p)
{
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Memory: committed (private) bytes now and at the peak, and the peak working set (which counts
// mapped cache pages the system can drop at any time).
std::string memory_text()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof pmc);
    char buf[160];
    snprintf(buf, sizeof buf, "private %.0f MiB now, %.0f MiB at the peak; peak working set %.0f MiB", pmc.PrivateUsage / 1048576.0,
             pmc.PeakPagefileUsage / 1048576.0, pmc.PeakWorkingSetSize / 1048576.0);
    return buf;
}

// Fastfiles of the arguments: files as given, folders searched recursively in name order.
std::vector<fs::path> fastfiles(const std::vector<fs::path> &args)
{
    std::vector<fs::path> files;
    for (const fs::path &a : args)
    {
        if (fs::is_directory(a))
        {
            std::vector<fs::path> found;
            for (const auto &e : fs::recursive_directory_iterator(a))
            {
                std::wstring ext = e.path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                if (e.is_regular_file() && ext == L".ff")
                    found.push_back(e.path());
            }
            std::sort(found.begin(), found.end());
            files.insert(files.end(), found.begin(), found.end());
        }
        else
            files.push_back(a);
    }
    return files;
}

// -- info

int cmd_info(const std::vector<fs::path> &files, bool list)
{
    for (const fs::path &path : files)
    {
        FastFile ff = read_fastfile(path);
        const Platform &p = for_endian(ff.big_endian);
        auto zone = read_zone(p, std::move(ff.zone));
        printf("%s: %s, zone %zu bytes, %zu script strings, %zu assets\n", utf8(path).c_str(), p.name.c_str(), zone->source->size(),
               zone->script_strings.size(), zone->assets.size());
        for (int b = 0; b < BLOCK_COUNT; ++b)
            printf("  %-30s %12u\n", block_name(b), zone->block_sizes[b]);
        std::map<std::string, int> counts;
        for (const auto &a : zone->assets)
            ++counts[a.type];
        std::vector<std::pair<int, std::string>> sorted;
        for (const auto &[t, n] : counts)
            sorted.emplace_back(-n, t);
        std::sort(sorted.begin(), sorted.end());
        std::string line;
        for (const auto &[n, t] : sorted)
            line += (line.empty() ? "" : ", ") + t + ": " + std::to_string(-n);
        printf("  %s\n", line.c_str());
        if (list)
            for (const auto &a : zone->assets)
                printf("    %-16s %s\n", a.type.c_str(), a.name.c_str());
    }
    return 0;
}

// -- roundtrip / bench

struct FileResult
{
    std::string path;
    std::string status;
    std::string cache;
    bool ok = false;
    size_t zone_size = 0;
    size_t nodes = 0;
    double t_inflate = 0, t_read = 0, t_write = 0, t_compress = 0;
};

int cmd_roundtrip(const std::vector<fs::path> &files, int jobs, bool check_compress, bool bench, bool keep, const fs::path &cache_dir)
{
    auto t0 = Clock::now();
    pc();
    double t_pc = seconds_since(t0);
    auto t1 = Clock::now();
    x360();
    double t_x360 = seconds_since(t1);
    printf("platforms: pc %.2f s, x360 %.2f s\n", t_pc, t_x360);

    std::vector<FileResult> results(files.size());
    std::vector<std::unique_ptr<Zone>> kept(keep ? files.size() : 0);
    std::mutex print_lock;
    std::atomic<size_t> done{0};
    auto start = Clock::now();
    // files are read on several threads; a zone's own compression uses one thread each then
    int file_jobs = job_count(jobs);
    parallel_for(files.size(), file_jobs, [&](size_t i) {
        FileResult &r = results[i];
        r.path = utf8(files[i]);
        try
        {
            auto t = Clock::now();
            std::vector<uint8_t> file; // the fastfile, to compare with when compressing again
            OpenedZone opened;
            if (check_compress)
            {
                file = read_file(files[i]);
                FastFile ff = parse_fastfile(file, r.path);
                opened.big_endian = ff.big_endian;
                opened.bytes = zone_bytes(std::move(ff.zone));
            }
            else
                opened = open_zone(files[i], cache_dir);
            r.t_inflate = seconds_since(t);
            if (opened.cached)
                r.cache = opened.cache_hit ? ", cached" : ", cache made";
            else if (!opened.cache_error.empty())
                r.cache = ", cache failed: " + opened.cache_error;
            const uint8_t *original = opened.bytes->data();
            const size_t original_size = opened.bytes->size();
            r.zone_size = original_size;
            const Platform &p = for_endian(opened.big_endian);

            t = Clock::now();
            auto zone = read_zone(p, opened.bytes);
            r.t_read = seconds_since(t);
            r.nodes = zone->node_store.size();

            if (!bench || !keep)
            {
                t = Clock::now();
                std::vector<uint8_t> written = write_zone(p, *zone);
                r.t_write = seconds_since(t);
                r.ok = written.size() == original_size && std::memcmp(written.data(), original, original_size) == 0;
                if (!r.ok)
                {
                    // which header fields (size, external size, block sizes) differ, and whether the
                    // rest does
                    static const char *FIELDS[] = {"size", "external", "temp", "runtime", "large_runtime", "physical_runtime",
                                                   "virtual", "large", "physical"};
                    std::string fields;
                    for (int f = 0; f < 9; ++f)
                        if (written.size() >= 36 && original_size >= 36 && p.u32(written.data() + 4 * f) != p.u32(original + 4 * f))
                            fields += std::string(fields.empty() ? "" : " ") + FIELDS[f] + " " + std::to_string(p.u32(written.data() + 4 * f)) +
                                      "/" + std::to_string(p.u32(original + 4 * f));
                    size_t at = 36;
                    while (at < std::min(written.size(), original_size) && written[at] == original[at])
                        ++at;
                    bool body = written.size() == original_size && at == written.size();
                    r.status = "DIFFERENT (header: " + (fields.empty() ? std::string("same") : fields) + "; body: " +
                               (body ? std::string("same") : "differs at " + std::to_string(at) + ", " + std::to_string(written.size()) + " vs " +
                                                                 std::to_string(original_size) + " bytes") +
                               ")";
                }
                else
                    r.status = "identical";
                if (r.ok && check_compress)
                {
                    t = Clock::now();
                    // chunked as the Python t4ff does on any machine with more than one processor
                    std::vector<uint8_t> again = fastfile_bytes(opened.big_endian, written, 9, 2);
                    r.t_compress = seconds_since(t);
                    r.status += again == file ? ", fastfile identical" : ", fastfile recompressed differently";
                }
            }
            else
            {
                r.ok = true;
                r.status = "read";
            }
            if (keep)
                kept[i] = std::move(zone);
        }
        catch (const std::exception &e)
        {
            r.status = std::string("ERROR: ") + e.what();
        }
        size_t n = ++done;
        std::lock_guard lock(print_lock);
        printf("[%zu/%zu] %s: %s (inflate %.2f s, read %.2f s, write %.2f s%s%s)\n", n, files.size(), r.path.c_str(), r.status.c_str(),
               r.t_inflate, r.t_read, r.t_write, check_compress ? (", compress " + std::to_string(r.t_compress) + " s").c_str() : "",
               r.cache.c_str());
        fflush(stdout);
    });
    double total = seconds_since(start);

    size_t ok = 0, nodes = 0, bytes = 0;
    double inflate = 0, read = 0, write = 0;
    for (const auto &r : results)
    {
        ok += r.ok;
        nodes += r.nodes;
        bytes += r.zone_size;
        inflate += r.t_inflate;
        read += r.t_read;
        write += r.t_write;
    }
    printf("\n%zu/%zu fastfiles %s; %.1f MiB of zones, %zu nodes\n", ok, files.size(), bench && keep ? "read" : "identical",
           bytes / 1048576.0, nodes);
    printf("wall %.2f s on %d threads (summed: inflate %.2f s, read %.2f s, write %.2f s)\nmemory: %s\n", total, file_jobs, inflate,
           read, write, memory_text().c_str());
    return ok == files.size() ? 0 : 1;
}

// -- dump: a canonical text form of the node tree, compared with the Python reader's

std::string opt(int v)
{
    return v < 0 ? "-" : std::to_string(v);
}

std::string origin_text(const Node &n)
{
    auto s = [](const std::string *p) { return p ? *p : std::string(); };
    switch (n.origin)
    {
    case Origin::Asset: return "asset:" + s(n.origin_record);
    case Origin::Member: return "member:" + s(n.origin_record) + "." + s(n.origin_field);
    case Origin::PtrArray: return "ptrarray:" + s(n.origin_record) + "." + s(n.origin_field);
    case Origin::PtrElem: return "ptrelem:" + s(n.origin_record);
    default: return "-";
    }
}

int cmd_dump(const fs::path &path, const fs::path &out_path)
{
    FastFile ff = read_fastfile(path);
    const Platform &p = for_endian(ff.big_endian);
    auto zone = read_zone(p, std::move(ff.zone));

    std::unordered_map<const Node *, size_t> ids;
    zone->walk([&](Node *n) { ids.emplace(n, ids.size()); });
    auto id = [&](const Node *n) { return n ? "N" + std::to_string(ids.at(n)) : std::string("None"); };
    auto addr = [](const std::optional<uint32_t> &a) { return a ? std::to_string(*a) : std::string("-"); };

    std::string out;
    out.reserve(zone->source->size() / 2);
    out += "zone " + zone->platform + " size=" + std::to_string(zone->size) + " external=" + std::to_string(zone->external_size) +
           " blocks=";
    for (size_t b = 0; b < zone->block_sizes.size(); ++b)
        out += (b ? "," : "") + std::to_string(zone->block_sizes[b]);
    out += "\n";
    for (size_t i = 0; i < zone->script_strings.size(); ++i)
        out += "string " + std::to_string(i) + " " + zone->script_strings[i].value_or("(null)") + "\n";
    for (size_t i = 0; i < zone->assets.size(); ++i)
        out += "asset " + std::to_string(i) + " " + zone->assets[i].type + " " + zone->assets[i].name + "\n";
    zone->walk([&](Node *n) {
        std::string flags;
        if (n->string)
            flags += "s";
        if (n->insert)
            flags += "i";
        if (n->delayed)
            flags += "d";
        out += id(n) + " " + (n->string ? std::string("string") : n->type->repr()) + " x" + std::to_string(n->count) +
               " blk=" + std::to_string(n->block) + " off=" + std::to_string(n->offset) + " size=" + std::to_string(n->data.size()) +
               " rt=" + std::to_string(n->runtime_size) + " align=" + (n->align ? std::to_string(*n->align) : "-") +
               " pb=" + opt(n->push_before) + " pa=" + opt(n->push_after) + " flags=" + (flags.empty() ? "-" : flags) +
               " asset=" + (n->asset ? n->asset : "-") + " origin=" + origin_text(*n) + " children=" + std::to_string(n->children.size()) +
               "\n";
        for (const Segment &s : n->segments)
            out += " seg " + s.type->repr() + " " + std::to_string(s.count) + " " + std::to_string(s.size) + " " + (s.partial ? "1" : "0") +
                   "\n";
        for (const auto &[off, ptr] : n->relocs.list())
        {
            out += " ptr " + std::to_string(off) + " addr=" + addr(ptr->addr) + " ";
            switch (ptr->kind)
            {
            case Ptr::Kind::Null: out += "null"; break;
            case Ptr::Kind::Follow: out += "follow " + id(ptr->node); break;
            case Ptr::Kind::Insert: out += "insert " + id(ptr->node) + " ia=" + addr(ptr->insert_addr); break;
            case Ptr::Kind::Ref:
                out += "ref " + id(ptr->node) + " " + std::to_string(ptr->index) + " " + std::to_string(ptr->inner);
                break;
            case Ptr::Kind::Alias:
                out += "alias " + id(ptr->slot->owner) + "@" + std::to_string(ptr->slot->offset) + " " + std::to_string(ptr->index);
                break;
            }
            out += "\n";
        }
    });
    FILE *f = _wfopen(out_path.c_str(), L"wb");
    if (!f)
        throw std::runtime_error("cannot write " + utf8(out_path));
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    printf("%s: %zu nodes\n", utf8(out_path).c_str(), ids.size());
    return 0;
}

// rewrite: the zone as the writer writes it back (compared with the Python writer's)
int cmd_rewrite(const fs::path &path, const fs::path &out_path)
{
    FastFile ff = read_fastfile(path);
    const Platform &p = for_endian(ff.big_endian);
    auto zone = read_zone(p, std::move(ff.zone));
    write_file(out_path, write_zone(p, *zone));
    return 0;
}

// textures: a battery of conversions of every IWI of the libraries given, as CRCs (compared with the
// Python t4ff's, native/tools/py_reference.py textures)

std::string crc_text(const std::vector<std::vector<uint8_t>> &parts)
{
    uLong crc = crc32(0L, Z_NULL, 0);
    size_t size = 0;
    for (const auto &p : parts)
    {
        crc = crc32(crc, p.data(), static_cast<uInt>(p.size()));
        size += p.size();
    }
    char buf[32];
    snprintf(buf, sizeof buf, "%08lx/%zu", crc, size);
    return buf;
}

std::string texture_report(const ImageData &image, const char *label, const TextureOptions &o, bool with_mips_check)
{
    std::string line = image.name + " " + label + " ";
    try
    {
        ConsoleTexture tex = build_console_texture(image, o);
        line += std::string(fmt_name(tex.format->name)) + " " + std::to_string(tex.width) + "x" + std::to_string(tex.height) +
                " levels=" + std::to_string(tex.levels) + " drop=" + std::to_string(tex.dropped_levels) + " faces=" + std::to_string(tex.faces) +
                " base=" + std::to_string(tex.base_size) + " header=" + crc_text({tex.header}) + " pixels=" + crc_text({tex.pixels});
        std::vector<std::vector<uint8_t>> linear;
        if (tex.levels > 1 || tex.faces > 1)
            linear = xenos::untile_mip_chain(tex.pixels.data(), tex.pixels.size(), tex.width, tex.height, *tex.format, tex.levels, tex.faces);
        else
            linear = {xenos::untile_level(tex.pixels.data(), tex.pixels.size(), tex.width, tex.height, 0, *tex.format)};
        line += " untile=" + crc_text(linear);
        if (with_mips_check)
        {
            line += " withmips=";
            if (tex.levels == 1 && tex.faces == 1)
            {
                auto m = with_mips(tex.pixels.data(), tex.pixels.size(), tex.width, tex.height, *tex.format, o.mip_tail);
                line += m ? crc_text({m->pixels}) + ":" + std::to_string(m->levels) : "none";
            }
            else
                line += "-";
        }
    }
    catch (const std::exception &)
    {
        line += "error";
    }
    try
    {
        line += " size=" + std::to_string(console_texture_size(image, o)) + "/" + std::to_string(console_texture_size(image, o, true));
    }
    catch (const std::exception &)
    {
        line += " size=error";
    }
    return line;
}

int cmd_textures(const fs::path &out_path, const std::vector<fs::path> &paths, int jobs)
{
    auto t0 = Clock::now();
    IwdLibrary library(paths);
    std::vector<std::string> names;
    for (const std::string &n : library.names("images/"))
        if (n.size() > 11 && n.compare(n.size() - 4, 4, ".iwi") == 0)
            names.push_back(n.substr(7, n.size() - 11));
    std::sort(names.begin(), names.end());
    printf("%zu images\n", names.size());

    std::vector<std::string> lines(names.size());
    std::atomic<size_t> done{0};
    parallel_for(names.size(), jobs, [&](size_t i) {
        const std::string &name = names[i];
        std::string out;
        std::optional<ImageData> image;
        try
        {
            image = library.image(name);
        }
        catch (const std::exception &)
        {
        }
        if (!image)
            out = name + " parse error\n";
        else
        {
            out = name + " parse " + fmt_name(image->format) + " " + std::to_string(image->width) + "x" + std::to_string(image->height) +
                  " faces=" + std::to_string(image->faces) + " flags=" + std::to_string(image->flags) + " levels=" +
                  std::to_string(image->levels.size()) + " crc=" + crc_text(image->levels) + "\n";
            TextureOptions a;
            out += texture_report(*image, "A", a, false) + "\n";
            TextureOptions b;
            b.drop_levels = 2;
            b.normal_map = true;
            b.mip_tail = false;
            out += texture_report(*image, "B", b, false) + "\n";
            TextureOptions c;
            c.max_size = 64;
            c.keep_mips = false;
            c.compress = false;
            out += texture_report(*image, "C", c, true) + "\n";
            TextureOptions d;
            d.drop_levels = 20;
            out += texture_report(*image, "D", d, false) + "\n";
        }
        lines[i] = std::move(out);
        size_t n = ++done;
        if (n % 1000 == 0)
        {
            printf("  %zu/%zu\n", n, names.size());
            fflush(stdout);
        }
    });
    std::string all;
    for (const auto &l : lines)
        all += l;
    FILE *f = _wfopen(out_path.c_str(), L"wb");
    if (!f)
        throw std::runtime_error("cannot write " + utf8(out_path));
    fwrite(all.data(), 1, all.size(), f);
    fclose(f);
    printf("%s: %zu images in %.2f s\n", utf8(out_path).c_str(), names.size(), seconds_since(t0));
    return 0;
}

// texbench: parse and convert (default options) every IWI of the libraries given, timed
int cmd_texbench(const std::vector<fs::path> &paths, int jobs)
{
    IwdLibrary library(paths);
    std::vector<std::string> names;
    for (const std::string &n : library.names("images/"))
        if (n.size() > 11 && n.compare(n.size() - 4, 4, ".iwi") == 0)
            names.push_back(n.substr(7, n.size() - 11));
    std::sort(names.begin(), names.end());
    std::atomic<size_t> bytes{0};
    auto t0 = Clock::now();
    parallel_for(names.size(), jobs, [&](size_t i) {
        try
        {
            if (auto image = library.image(names[i]))
                bytes += build_console_texture(*image).pixels.size();
        }
        catch (const std::exception &)
        {
        }
    });
    printf("%zu images, %.1f MiB of console textures in %.2f s on %d threads\n", names.size(), bytes / 1048576.0, seconds_since(t0),
           job_count(jobs));
    return 0;
}

// cache: the zone cache's files and size, or (--clear) delete them
int cmd_cache(const fs::path &dir, bool clear)
{
    size_t files = 0;
    uintmax_t bytes = 0;
    if (fs::is_directory(dir))
    {
        for (const auto &e : fs::directory_iterator(dir))
        {
            std::wstring ext = e.path().extension().wstring();
            if (!e.is_regular_file() || (ext != L".zone" && ext != L".tmp"))
                continue;
            uintmax_t size = e.file_size();
            if (clear)
            {
                std::error_code ec;
                if (!fs::remove(e.path(), ec))
                {
                    printf("in use, kept: %s\n", utf8(e.path().filename()).c_str());
                    continue;
                }
            }
            ++files;
            bytes += size;
        }
    }
    printf("%s: %zu zones, %.1f MiB%s\n", utf8(dir).c_str(), files, bytes / 1048576.0, clear ? " deleted" : "");
    return 0;
}

int usage()
{
    fputs("usage:\n"
          "  t4ff-cli info <fastfile>... [--list]\n"
          "  t4ff-cli roundtrip [--compress] [--jobs N] [--zone-cache | --zone-cache-dir DIR] <fastfile or folder>...\n"
          "  t4ff-cli dump <fastfile> <out.txt>\n"
          "  t4ff-cli bench [--jobs N] [--keep] [--zone-cache | --zone-cache-dir DIR] <fastfile or folder>...\n"
          "  t4ff-cli textures [--jobs N] <out.txt> <iwd or folder>...\n"
          "  t4ff-cli texbench [--jobs N] <iwd or folder>...\n"
          "  t4ff-cli cache [--clear] [--zone-cache-dir DIR]\n",
          stderr);
    return 2;
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
        return usage();
    std::wstring cmd = argv[1];
    std::vector<fs::path> paths;
    bool list = false, check_compress = false, keep = false, clear = false;
    int jobs = 0;
    fs::path cache_dir;
    for (int i = 2; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--list")
            list = true;
        else if (a == L"--compress")
            check_compress = true;
        else if (a == L"--clear")
            clear = true;
        else if (a == L"--keep")
            keep = true;
        else if (a == L"--jobs" && i + 1 < argc)
            jobs = _wtoi(argv[++i]);
        else if (a == L"--zone-cache")
            cache_dir = default_zone_cache_dir();
        else if (a == L"--zone-cache-dir" && i + 1 < argc)
            cache_dir = argv[++i];
        else
            paths.emplace_back(a);
    }
    try
    {
        if (cmd == L"info" && !paths.empty())
            return cmd_info(paths, list);
        if (cmd == L"roundtrip" && !paths.empty())
            return cmd_roundtrip(fastfiles(paths), jobs, check_compress, false, false, cache_dir);
        if (cmd == L"bench" && !paths.empty())
            return cmd_roundtrip(fastfiles(paths), jobs, false, true, keep, cache_dir);
        if (cmd == L"dump" && paths.size() == 2)
            return cmd_dump(paths[0], paths[1]);
        if (cmd == L"rewrite" && paths.size() == 2)
            return cmd_rewrite(paths[0], paths[1]);
        if (cmd == L"cache" && paths.empty())
            return cmd_cache(cache_dir.empty() ? default_zone_cache_dir() : cache_dir, clear);
        if (cmd == L"texbench" && !paths.empty())
            return cmd_texbench(paths, jobs);
        if (cmd == L"textures" && paths.size() >= 2)
            return cmd_textures(paths[0], std::vector<fs::path>(paths.begin() + 1, paths.end()), jobs);
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return usage();
}
