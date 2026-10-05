// t4ff-cli: the command line of the native t4ff.
//
//   t4ff-cli info <fastfile> [--list]
//   t4ff-cli roundtrip [--compress] [--jobs N] <fastfile or folder>...
//   t4ff-cli dump <fastfile> <out.txt>
//   t4ff-cli rewrite <fastfile> <out.zone>
//   t4ff-cli bench [--jobs N] [--keep] <fastfile or folder>...
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
#include "core/platforms.h"
#include "core/threads.h"
#include "core/zone.h"

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

double peak_mib()
{
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    return pmc.PeakWorkingSetSize / 1048576.0;
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
        auto zone = read_zone(p, ff.zone);
        printf("%s: %s, zone %zu bytes, %zu script strings, %zu assets\n", utf8(path).c_str(), p.name.c_str(), ff.zone.size(),
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
    bool ok = false;
    size_t zone_size = 0;
    size_t nodes = 0;
    double t_inflate = 0, t_read = 0, t_write = 0, t_compress = 0;
};

int cmd_roundtrip(const std::vector<fs::path> &files, int jobs, bool check_compress, bool bench, bool keep)
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
            std::vector<uint8_t> file = read_file(files[i]);
            FastFile ff = parse_fastfile(file, r.path);
            r.t_inflate = seconds_since(t);
            r.zone_size = ff.zone.size();
            const Platform &p = for_endian(ff.big_endian);

            t = Clock::now();
            auto zone = read_zone(p, ff.zone);
            r.t_read = seconds_since(t);
            r.nodes = zone->node_store.size();

            if (!bench || !keep)
            {
                t = Clock::now();
                std::vector<uint8_t> written = write_zone(p, *zone);
                r.t_write = seconds_since(t);
                r.ok = written == ff.zone;
                if (!r.ok)
                {
                    // which header fields (size, external size, block sizes) differ, and whether the
                    // rest does
                    static const char *FIELDS[] = {"size", "external", "temp", "runtime", "large_runtime", "physical_runtime",
                                                   "virtual", "large", "physical"};
                    std::string fields;
                    for (int f = 0; f < 9; ++f)
                        if (written.size() >= 36 && ff.zone.size() >= 36 && p.u32(written.data() + 4 * f) != p.u32(ff.zone.data() + 4 * f))
                            fields += std::string(fields.empty() ? "" : " ") + FIELDS[f] + " " + std::to_string(p.u32(written.data() + 4 * f)) +
                                      "/" + std::to_string(p.u32(ff.zone.data() + 4 * f));
                    size_t at = 36;
                    while (at < std::min(written.size(), ff.zone.size()) && written[at] == ff.zone[at])
                        ++at;
                    bool body = written.size() == ff.zone.size() && at == written.size();
                    r.status = "DIFFERENT (header: " + (fields.empty() ? std::string("same") : fields) + "; body: " +
                               (body ? std::string("same") : "differs at " + std::to_string(at) + ", " + std::to_string(written.size()) + " vs " +
                                                                 std::to_string(ff.zone.size()) + " bytes") +
                               ")";
                }
                else
                    r.status = "identical";
                if (r.ok && check_compress)
                {
                    t = Clock::now();
                    // chunked as the Python t4ff does on any machine with more than one processor
                    std::vector<uint8_t> again = fastfile_bytes(ff.big_endian, written, 9, 2);
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
        printf("[%zu/%zu] %s: %s (inflate %.2f s, read %.2f s, write %.2f s%s)\n", n, files.size(), r.path.c_str(), r.status.c_str(),
               r.t_inflate, r.t_read, r.t_write, check_compress ? (", compress " + std::to_string(r.t_compress) + " s").c_str() : "");
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
    printf("wall %.2f s on %d threads (summed: inflate %.2f s, read %.2f s, write %.2f s); peak memory %.0f MiB\n", total, file_jobs,
           inflate, read, write, peak_mib());
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
    auto zone = read_zone(p, ff.zone);

    std::unordered_map<const Node *, size_t> ids;
    zone->walk([&](Node *n) { ids.emplace(n, ids.size()); });
    auto id = [&](const Node *n) { return n ? "N" + std::to_string(ids.at(n)) : std::string("None"); };
    auto addr = [](const std::optional<uint32_t> &a) { return a ? std::to_string(*a) : std::string("-"); };

    std::string out;
    out.reserve(ff.zone.size() / 2);
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
    auto zone = read_zone(p, ff.zone);
    write_file(out_path, write_zone(p, *zone));
    return 0;
}

int usage()
{
    fputs("usage:\n"
          "  t4ff-cli info <fastfile>... [--list]\n"
          "  t4ff-cli roundtrip [--compress] [--jobs N] <fastfile or folder>...\n"
          "  t4ff-cli dump <fastfile> <out.txt>\n"
          "  t4ff-cli bench [--jobs N] [--keep] <fastfile or folder>...\n",
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
    bool list = false, check_compress = false, keep = false;
    int jobs = 0;
    for (int i = 2; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--list")
            list = true;
        else if (a == L"--compress")
            check_compress = true;
        else if (a == L"--keep")
            keep = true;
        else if (a == L"--jobs" && i + 1 < argc)
            jobs = _wtoi(argv[++i]);
        else
            paths.emplace_back(a);
    }
    try
    {
        if (cmd == L"info" && !paths.empty())
            return cmd_info(paths, list);
        if (cmd == L"roundtrip" && !paths.empty())
            return cmd_roundtrip(fastfiles(paths), jobs, check_compress, false, false);
        if (cmd == L"bench" && !paths.empty())
            return cmd_roundtrip(fastfiles(paths), jobs, false, true, keep);
        if (cmd == L"dump" && paths.size() == 2)
            return cmd_dump(paths[0], paths[1]);
        if (cmd == L"rewrite" && paths.size() == 2)
            return cmd_rewrite(paths[0], paths[1]);
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return usage();
}
