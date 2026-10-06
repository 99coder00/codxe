#include "audio/soundbudget.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "convert/assets.h"
#include "convert/converter.h"
#include "convert/library.h"
#include "convert/merge.h"
#include "core/fastfile.h"
#include "core/sha256.h"

namespace t4ff
{
namespace fs = std::filesystem;

namespace
{
constexpr uint32_t SAT_STREAMED = 2;
constexpr uint32_t ALIAS_TYPE_SHIFT = 13; // snd_alias_t.flags bits 13-14: the type of the alias' sound file
constexpr uint32_t ALIAS_TYPE_MASK = 3u << ALIAS_TYPE_SHIFT;
constexpr uint32_t ALIAS_LOOPING = 1; // snd_alias_t.flags bit 0

// a loaded sound's data (its format block, seek table and XMA packets), name aside
std::string sound_content(Node *node)
{
    Sha256 h;
    node->walk([&](Node *n) {
        if (!n->string)
            h.update(n->data.data(), n->data.size());
    });
    return h.hex();
}

uint64_t sound_bytes(Node *node)
{
    uint64_t total = 0;
    node->walk([&](Node *n) {
        if ((n->origin == Origin::Member || n->origin == Origin::PtrArray) && *n->origin_record == "snd_asset" && *n->origin_field == "data")
            total += n->data.size();
    });
    return total;
}

uint32_t field(const Platform &p, const char *rec, const char *name)
{
    return p.record(rec).field(name)->offset;
}

std::string strip_commas(const std::string &s)
{
    size_t i = s.find_first_not_of(',');
    return i == std::string::npos ? std::string() : s.substr(i);
}
} // namespace

int sync_alias_types(const Platform &p, Zone &zone)
{
    const Record &rec = p.record("snd_alias_t");
    uint32_t flags_off = rec.field("flags")->offset, file_off = rec.field("soundFile")->offset;
    int changed = 0;
    zone.root->walk([&](Node *node) {
        if (!node->is_record("snd_alias_t"))
            return;
        for (uint32_t i = 0; i < node->count; ++i)
        {
            uint32_t base = i * rec.size;
            Ptr *ptr = node->relocs.get(base + file_off);
            Node *sound_file = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
            if (!sound_file || sound_file->data.empty() || sound_file->data[0] < 1 || sound_file->data[0] > 3)
                continue;
            uint32_t flags = p.u32(node->data.data() + base + flags_off);
            uint32_t wanted = (flags & ~ALIAS_TYPE_MASK) | (uint32_t(sound_file->data[0]) << ALIAS_TYPE_SHIFT);
            if (wanted != flags)
            {
                p.put_u32(node->data.mutable_data() + base + flags_off, wanted);
                ++changed;
            }
        }
    });
    return changed;
}

SoundLimitStats limit_loaded_sounds(const Platform &p, Zone &zone, int limit, const std::map<std::string, std::shared_ptr<XmaStream>> &streams,
                                    const fs::path &sounds_dir, const std::function<void(const std::string &)> &log, uint64_t max_bytes)
{
    SoundLimitStats stats;
    stats.shared = dedupe_nested_assets(
        p, zone, nullptr,
        [](const std::string &record, const std::string &, Node *node) -> std::optional<std::pair<std::string, std::string>> {
            if (record != "LoadedSound")
                return std::nullopt;
            return std::make_pair(std::string("LoadedSound"), sound_content(node));
        });
    std::vector<Node *> sounds;
    zone.root->walk([&](Node *n) {
        if (n->origin == Origin::Asset && n->is_record("LoadedSound"))
            sounds.push_back(n);
    });
    int64_t count = static_cast<int64_t>(sounds.size());
    int64_t size = 0;
    for (Node *s : sounds)
        size += sound_bytes(s);
    if (stats.shared && log)
        log("loaded sounds: " + std::to_string(stats.shared) + " identical sounds shared");
    auto within = [&]() { return (!limit || count <= limit) && (!max_bytes || static_cast<uint64_t>(size) <= max_bytes); };
    if (within())
    {
        stats.count = static_cast<int>(count);
        stats.bytes = static_cast<uint64_t>(size);
        return stats;
    }

    uint32_t u = field(p, "SoundFile", "u");
    uint32_t fn = u + field(p, "StreamedSound", "filename");
    uint32_t hash_off = fn + field(p, "StreamFileName", "hash"), dir_off = fn + field(p, "StreamFileName", "dir");
    uint32_t name_off = fn + field(p, "StreamFileName", "name"), prime_off = u + field(p, "StreamedSound", "primeSnd");

    // who points where: the pointer loading each sound, aliases to it, pointers into it
    std::unordered_map<const Node *, Ptr *> loader;
    std::unordered_map<const Ptr *, std::vector<Ptr *>> aliases;
    std::unordered_map<const Node *, std::vector<Node *>> incoming;
    zone.root->walk([&](Node *n) {
        for (const auto &[off, ptr] : n->relocs.list())
        {
            if ((ptr->kind == Ptr::Kind::Follow || ptr->kind == Ptr::Kind::Insert) && ptr->node)
                loader[ptr->node] = ptr;
            else if (ptr->kind == Ptr::Kind::Alias && ptr->slot)
            {
                aliases[ptr->slot].push_back(ptr);
                if (ptr->slot->owner)
                    incoming[ptr->slot->owner].push_back(n);
            }
            else if (ptr->kind == Ptr::Kind::Ref && ptr->node)
                incoming[ptr->node].push_back(n);
        }
    });

    // the SoundFile entries playing sound, nothing when something else refers to it
    auto sound_files = [&](Node *sound) -> std::optional<std::vector<Node *>> {
        auto first = loader.find(sound);
        if (first == loader.end() || !first->second->owner || !first->second->owner->is_record("SoundFile") || first->second->offset != u)
            return std::nullopt;
        std::unordered_set<const Node *> inside;
        sound->walk([&](Node *x) { inside.insert(x); });
        for (const Node *target : inside)
        {
            auto it = incoming.find(target);
            if (it != incoming.end())
                for (Node *src : it->second)
                    if (!inside.count(src))
                        return std::nullopt;
        }
        std::vector<Node *> owners{first->second->owner};
        auto al = aliases.find(first->second);
        if (al != aliases.end())
            for (Ptr *alias : al->second)
            {
                if (!alias->owner || !alias->owner->is_record("SoundFile") || alias->offset != u)
                    return std::nullopt;
                owners.push_back(alias->owner);
            }
        return owners;
    };
    auto duration = [&](Node *sound) -> double {
        auto it = streams.find(lower_latin1(strip_commas(asset_display_name(p, *sound))));
        return it != streams.end() && it->second && it->second->rate ? static_cast<double>(it->second->valid_samples) / it->second->rate : -1.0;
    };

    // sounds a looping alias plays stay loaded: a looping stream holds one of the console's few stream
    // channels as long as it plays
    const Record &arec = p.record("snd_alias_t");
    uint32_t flags_off = arec.field("flags")->offset, file_off = arec.field("soundFile")->offset;
    std::unordered_set<const Node *> looping;
    zone.root->walk([&](Node *n) {
        if (!n->is_record("snd_alias_t"))
            return;
        for (uint32_t i = 0; i < n->count; ++i)
        {
            if (!(p.u32(n->data.data() + i * arec.size + flags_off) & ALIAS_LOOPING))
                continue;
            Ptr *ptr = n->relocs.get(i * arec.size + file_off);
            Node *sound_file = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
            Ptr *loaded = sound_file ? sound_file->relocs.get(u) : nullptr;
            Node *target = loaded && loaded->kind != Ptr::Kind::Null ? loaded->target() : nullptr;
            if (target)
                looping.insert(target);
        }
    });

    std::vector<Node *> candidates;
    for (Node *s : sounds)
        if (duration(s) > 0 && !looping.count(s))
            candidates.push_back(s);
    std::stable_sort(candidates.begin(), candidates.end(), [&](Node *a, Node *b) { return duration(a) > duration(b); });
    for (Node *sound : candidates)
    {
        if (within())
            break;
        auto owners = sound_files(sound);
        if (!owners)
            continue;
        std::string name = strip_commas(asset_display_name(p, *sound));
        const XmaStream &stream = *streams.at(lower_latin1(name));
        std::string slashed = name;
        std::replace(slashed.begin(), slashed.end(), '\\', '/');
        std::vector<std::string> parts;
        for (size_t start = 0;;)
        {
            size_t end = slashed.find('/', start);
            parts.push_back(slashed.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        std::string directory = "sounds";
        fs::path target = sounds_dir / "sounds";
        for (size_t i = 0; i + 1 < parts.size(); ++i)
        {
            directory += "\\" + parts[i];
            target /= parts[i];
        }
        const std::string &stem = parts.back();
        target /= stem + ".xma";
        fs::create_directories(target.parent_path());
        std::vector<uint8_t> data = write_sdns(stream);
        write_file(target, data);
        for (Node *sf : *owners)
        {
            Ptr *old = sf->relocs.erase(u);
            if (old && (old->kind == Ptr::Kind::Follow || old->kind == Ptr::Kind::Insert))
                sf->children.erase(std::remove(sf->children.begin(), sf->children.end(), old->node), sf->children.end());
            uint8_t *d = sf->data.mutable_data();
            d[0] = SAT_STREAMED;
            p.put_u32(d + hash_off, map_stream_hash(directory, stem));
            p.put_u32(d + dir_off, 0xFFFFFFFF);
            p.put_u32(d + name_off, 0xFFFFFFFF);
            p.put_u32(d + prime_off, 0);
            for (auto [off, text] : {std::make_pair(dir_off, directory), std::make_pair(name_off, stem)})
            {
                Node *node = zone.new_node();
                node->type = p.char_type;
                node->block = sf->block;
                node->string = true;
                std::vector<uint8_t> bytes(text.begin(), text.end());
                bytes.push_back(0);
                node->count = static_cast<uint32_t>(bytes.size());
                node->segments = {{node->type, node->count, node->count, false}};
                node->align = 1;
                node->data.assign(std::move(bytes));
                Ptr *ptr = zone.new_ptr(Ptr::Kind::Follow);
                ptr->node = node;
                ptr->owner = sf;
                ptr->offset = off;
                sf->relocs.set(off, ptr);
                sf->children.push_back(node);
            }
            Ptr *null_ptr = zone.new_ptr(Ptr::Kind::Null);
            null_ptr->owner = sf;
            null_ptr->offset = prime_off;
            sf->relocs.set(prime_off, null_ptr);
        }
        --count;
        size -= static_cast<int64_t>(sound_bytes(sound));
        ++stats.streamed;
        stats.stream_bytes += data.size();
    }
    // the aliases carry the type of their sound file too
    sync_alias_types(p, zone);
    stats.count = static_cast<int>(count);
    stats.bytes = static_cast<uint64_t>(size);
    for (Node *s : sounds)
        stats.looping += looping.count(s) ? 1 : 0;
    if (log)
    {
        std::string limits;
        if (limit)
            limits = std::to_string(limit) + " loaded sounds";
        if (max_bytes)
        {
            char buf[32];
            snprintf(buf, sizeof buf, "%.0f MiB", max_bytes / 1048576.0);
            limits += (limits.empty() ? "" : " and ") + std::string(buf);
        }
        char buf[512];
        snprintf(buf, sizeof buf, "loaded sounds: %d of the longest are streamed from the map's sounds folder (%.1f MiB) to stay within %s, looping ones stay loaded",
                 stats.streamed, stats.stream_bytes / 1048576.0, limits.c_str());
        std::string msg = buf;
        if (limit && count > limit)
            msg += "; " + std::to_string(count) + " remain, more than the limit: the map may not load";
        if (max_bytes && static_cast<uint64_t>(size) > max_bytes)
        {
            snprintf(buf, sizeof buf, "; %.1f MiB remain (lower --sound-rate or --mono-sounds for less)", size / 1048576.0);
            msg += buf;
        }
        log(msg);
    }
    return stats;
}
} // namespace t4ff
