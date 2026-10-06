#include "convert/merge.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "convert/assets.h"
#include "convert/converter.h"
#include "convert/library.h"

namespace t4ff
{
namespace
{
// self contained asset types for which the last zone's version wins (mod.ff and <map>_patch.ff
// replace scripts of the map on PC)
bool later_wins(const std::string &t)
{
    return t == "rawfile" || t == "stringtable" || t == "localize";
}

bool loads(const Ptr *ptr)
{
    return ptr && (ptr->kind == Ptr::Kind::Follow || ptr->kind == Ptr::Kind::Insert);
}

std::vector<uint8_t> content(Node *node)
{
    std::vector<uint8_t> out;
    node->walk([&](Node *n) { out.insert(out.end(), n->data.begin(), n->data.end()); });
    return out;
}

std::string strip_commas(const std::string &name)
{
    size_t i = name.find_first_not_of(',');
    return i == std::string::npos ? std::string() : name.substr(i);
}

uint16_t get_u16(const Platform &p, const uint8_t *d)
{
    return p.big_endian ? uint16_t(d[0] << 8 | d[1]) : uint16_t(d[1] << 8 | d[0]);
}

void put_u16(const Platform &p, uint8_t *d, uint16_t v)
{
    if (p.big_endian)
        d[0] = uint8_t(v >> 8), d[1] = uint8_t(v);
    else
        d[1] = uint8_t(v >> 8), d[0] = uint8_t(v);
}
} // namespace

const std::vector<uint32_t> &record_script_string_offsets(const Platform &p, const std::string &name, bool partial)
{
    static std::mutex lock;
    static std::map<std::tuple<std::string, std::string, bool>, std::unique_ptr<std::vector<uint32_t>>> cache;
    auto key = std::make_tuple(p.name, name, partial);
    {
        std::lock_guard guard(lock);
        auto it = cache.find(key);
        if (it != cache.end())
            return *it->second;
        cache.emplace(key, std::make_unique<std::vector<uint32_t>>()); // guards recursion
    }
    const Record &rec = p.record(name);
    int64_t limit = -1;
    if (partial)
    {
        const Field *dyn = p.dynamic_member(rec);
        if (dyn)
            limit = dyn->offset;
    }
    std::vector<uint32_t> result;
    for (const Field &f : rec.fields)
    {
        if (f.name.empty() || (limit >= 0 && f.offset >= limit))
            continue;
        const MemberInfos *infos = p.cmds->member_infos(name, f.name);
        bool scriptstring = false;
        if (infos)
            for (const auto &[ctx, info] : *infos)
                scriptstring |= info.scriptstring;
        const TypeRef *t = f.type;
        if (scriptstring && t->kind != TypeKind::Pointer)
        {
            uint32_t count = t->kind == TypeKind::Array ? t->count : 1;
            for (uint32_t i = 0; i < count; ++i)
                result.push_back(f.offset + 2 * i);
            continue;
        }
        // embedded records and arrays of records
        uint32_t count = 1;
        while (t->kind == TypeKind::Array)
        {
            count *= t->count;
            t = t->elem;
        }
        if (t->kind == TypeKind::Record && !rec.is_union)
        {
            const std::vector<uint32_t> &inner = record_script_string_offsets(p, t->name);
            for (uint32_t i = 0; i < count; ++i)
                for (uint32_t o : inner)
                    result.push_back(f.offset + i * t->size + o);
        }
    }
    std::lock_guard guard(lock);
    *cache[key] = std::move(result);
    return *cache[key];
}

std::vector<uint32_t> node_script_string_offsets(const Platform &p, const Node &node)
{
    std::vector<uint32_t> result;
    if (node.origin == Origin::Member)
    {
        const MemberInfos *infos = p.cmds->member_infos(*node.origin_record, *node.origin_field);
        bool scriptstring = false;
        if (infos)
            for (const auto &[ctx, info] : *infos)
                scriptstring |= info.scriptstring;
        if (scriptstring)
        {
            for (uint32_t off = 0; off + 1 < node.data.size(); off += 2)
                result.push_back(off);
            return result;
        }
    }
    uint32_t pos = 0;
    for (const Segment &s : node.segments)
    {
        if (s.type->kind == TypeKind::Record && s.count)
        {
            uint32_t stride = s.size / s.count;
            const std::vector<uint32_t> &inner = record_script_string_offsets(p, s.type->name, s.partial);
            if (!inner.empty())
                for (uint32_t i = 0; i < s.count; ++i)
                    for (uint32_t o : inner)
                        result.push_back(pos + i * stride + o);
        }
        pos += s.size;
    }
    return result;
}

void remap_script_strings(const Platform &p, Node *root, const std::vector<uint32_t> &mapping)
{
    root->walk([&](Node *node) {
        if (node->data.empty())
            return;
        for (uint32_t off : node_script_string_offsets(p, *node))
        {
            if (off + 2 > node->data.size())
                continue;
            uint16_t value = get_u16(p, node->data.data() + off);
            if (value < mapping.size())
                put_u16(p, node->data.mutable_data() + off, static_cast<uint16_t>(mapping[value]));
        }
    });
}

bool is_reference(const Platform &p, const Node *node)
{
    if (!node || node->origin != Origin::Asset)
        return false;
    std::string name = asset_display_name(p, *node);
    return !name.empty() && name[0] == ',';
}

bool has_incoming_refs(Zone &zone, Node *target)
{
    std::unordered_set<const Node *> inside;
    target->walk([&](Node *n) { inside.insert(n); });
    bool found = false;
    zone.root->walk([&](Node *node) {
        if (found || inside.count(node))
            return;
        for (const auto &[off, ptr] : node->relocs.list())
        {
            if (ptr->kind == Ptr::Kind::Ref && inside.count(ptr->node))
                found = true;
            if (ptr->kind == Ptr::Kind::Alias && ptr->slot && ptr->slot->owner && inside.count(ptr->slot->owner))
                found = true;
        }
    });
    return found;
}

std::unique_ptr<Zone> merge_zones(const Platform &p, std::vector<std::unique_ptr<Zone>> zones, const Log &log)
{
    auto merged = std::make_unique<Zone>();
    Zone &mz = *merged;
    std::vector<std::optional<std::string>> strings = zones[0]->script_strings;
    if (strings.empty())
        strings.emplace_back(std::nullopt);
    std::unordered_map<std::string, uint32_t> index;
    for (size_t i = 0; i < strings.size(); ++i)
        if (strings[i])
            index[*strings[i]] = static_cast<uint32_t>(i);

    std::vector<ZoneAsset> merged_assets;
    std::set<std::pair<std::string, std::string>> seen;
    std::vector<Node *> asset_children;
    std::vector<Ptr *> asset_relocs;
    int duplicates = 0, overridden = 0;
    struct Defined
    {
        Zone *zone;
        Ptr *ptr;
        size_t child;
    };
    std::map<std::pair<std::string, std::string>, Defined> defined;

    for (size_t zone_index = 0; zone_index < zones.size(); ++zone_index)
    {
        Zone &zone = *zones[zone_index];
        std::vector<uint32_t> mapping;
        for (const auto &s : zone.script_strings)
        {
            if (!s)
            {
                mapping.push_back(0);
                continue;
            }
            auto it = index.find(*s);
            if (it == index.end())
            {
                it = index.emplace(*s, static_cast<uint32_t>(strings.size())).first;
                strings.push_back(*s);
            }
            mapping.push_back(it->second);
        }
        if (zone_index && zone.assets_node)
            remap_script_strings(p, zone.assets_node, mapping);
        if (!zone.assets_node)
            continue;
        Node *node = zone.assets_node;
        // children of the asset list node are the followed asset headers in asset order
        std::unordered_map<const Ptr *, Node *> child_for_ptr;
        {
            std::vector<std::pair<uint32_t, Ptr *>> sorted(node->relocs.list().begin(), node->relocs.list().end());
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
            size_t next_child = 0;
            for (const auto &[off, ptr] : sorted)
            {
                if (loads(ptr))
                {
                    if (next_child >= node->children.size())
                        throw ConvertError("merge: asset list children run out");
                    child_for_ptr[ptr] = node->children[next_child++];
                }
            }
        }
        for (size_t i = 0; i < zone.assets.size(); ++i)
        {
            ZoneAsset asset = zone.assets[i];
            Ptr *ptr = node->relocs.get(static_cast<uint32_t>(8 * i + 4));
            auto key = std::make_pair(asset.type, strip_commas(asset.name));
            bool ref_name = !asset.name.empty() && asset.name[0] == ',';
            if (!ref_name && loads(ptr) && is_reference(p, ptr->node))
            {
                // a name reference (left by an earlier merge) under the plain name
                asset.name = "," + asset.name;
                ref_name = true;
            }
            bool duplicate = seen.count(key) && !ref_name;
            if (loads(ptr) && duplicate)
            {
                Node *target = ptr->node;
                if (!has_incoming_refs(zone, target))
                {
                    auto earlier = defined.find(key);
                    if (later_wins(asset.type) && earlier != defined.end() && content(earlier->second.ptr->node) != content(target))
                    {
                        // a later zone overrides the script / table (as mod.ff and <map>_patch.ff do on
                        // PC): its version takes the place of the earlier one
                        Defined &e = earlier->second;
                        if (!has_incoming_refs(*e.zone, e.ptr->node))
                        {
                            Node *old = e.ptr->node;
                            e.ptr->node = target;
                            if (e.ptr->kind == Ptr::Kind::Insert)
                            {
                                target->insert = true;
                                target->ptr = e.ptr;
                            }
                            asset_children[e.child] = target;
                            target = old;
                            ++overridden;
                        }
                    }
                    Node *ref = build_reference(mz, p, asset.type, *target, "," + asset.name);
                    ptr->node = ref;
                    child_for_ptr[ptr] = ref;
                    ++duplicates;
                    asset.name = "," + asset.name;
                }
            }
            else if (loads(ptr) && !ref_name && !defined.count(key))
                defined.emplace(key, Defined{&zone, ptr, asset_children.size()});
            seen.insert(key);
            ZoneAsset out;
            out.type = asset.type;
            out.ptr = ptr;
            out.name = asset.name;
            merged_assets.push_back(std::move(out));
            asset_relocs.push_back(ptr);
            auto it = ptr ? child_for_ptr.find(ptr) : child_for_ptr.end();
            if (it != child_for_ptr.end())
                asset_children.push_back(it->second);
        }
    }
    if (duplicates && log)
        log("merge: " + std::to_string(duplicates) + " assets defined by several zones are kept once (" + std::to_string(overridden) +
            " scripts/tables taken from the later zone)");

    // script string table
    Node *script_node = mz.new_node();
    script_node->type = p.char_ptr_type;
    script_node->count = static_cast<uint32_t>(strings.size());
    script_node->block = BLOCK_VIRTUAL;
    script_node->align = 4;
    script_node->data.assign(std::vector<uint8_t>(4 * strings.size()));
    script_node->segments.push_back({script_node->type, script_node->count, static_cast<uint32_t>(4 * strings.size()), false});
    for (size_t i = 0; i < strings.size(); ++i)
    {
        Ptr *ptr;
        if (!strings[i])
            ptr = mz.new_ptr(Ptr::Kind::Null);
        else
        {
            Node *child = string_node(mz, p, *strings[i]);
            script_node->children.push_back(child);
            ptr = mz.new_ptr(Ptr::Kind::Follow);
            ptr->node = child;
        }
        ptr->owner = script_node;
        ptr->offset = static_cast<uint32_t>(4 * i);
        script_node->relocs.set(ptr->offset, ptr);
    }

    // asset list
    Node *assets_node = mz.new_node();
    assets_node->type = p.uint_type;
    assets_node->count = static_cast<uint32_t>(2 * merged_assets.size());
    assets_node->block = BLOCK_VIRTUAL;
    assets_node->align = 4;
    std::vector<uint8_t> data(8 * merged_assets.size());
    for (size_t i = 0; i < merged_assets.size(); ++i)
    {
        auto t = std::find(p.asset_types.begin(), p.asset_types.end(), merged_assets[i].type);
        if (t == p.asset_types.end())
            throw ConvertError("merge: unknown asset type " + merged_assets[i].type);
        p.put_u32(data.data() + 8 * i, static_cast<uint32_t>(t - p.asset_types.begin()));
        Ptr *ptr = asset_relocs[i];
        if (!ptr)
        {
            ptr = mz.new_ptr(Ptr::Kind::Null);
            merged_assets[i].ptr = ptr;
        }
        ptr->owner = assets_node;
        ptr->offset = static_cast<uint32_t>(8 * i + 4);
        assets_node->relocs.set(ptr->offset, ptr);
    }
    assets_node->segments.push_back({assets_node->type, assets_node->count, static_cast<uint32_t>(data.size()), false});
    assets_node->data.assign(std::move(data));
    assets_node->children = asset_children;

    Node *root = mz.new_node();
    root->type = p.uint_type;
    root->count = 4;
    root->block = -1;
    root->data.assign(std::vector<uint8_t>(16));
    root->children = {script_node, assets_node};

    mz.platform = p.name;
    mz.script_strings = strings;
    mz.assets = std::move(merged_assets);
    mz.script_node = script_node;
    mz.assets_node = assets_node;
    mz.root = root;
    for (auto &z : zones)
        mz.keep_alive.push_back(std::shared_ptr<Zone>(std::move(z)));
    dedupe_nested_assets(p, mz, log);
    return merged;
}

int prune_references(const Platform &p, Zone &zone, const std::vector<std::string> &types, const Log &log)
{
    Node *node = zone.assets_node;
    if (!node)
        return 0;
    std::unordered_set<const Ptr *> used;
    zone.root->walk([&](Node *n) {
        for (const auto &[off, ptr] : n->relocs.list())
            if (ptr->kind == Ptr::Kind::Alias && ptr->slot)
                used.insert(ptr->slot);
    });
    std::vector<AssetListEntry> keep;
    int removed = 0;
    for (size_t i = 0; i < zone.assets.size(); ++i)
    {
        const ZoneAsset &asset = zone.assets[i];
        Ptr *ptr = node->relocs.get(static_cast<uint32_t>(8 * i + 4));
        Node *target = loads(ptr) ? ptr->node : nullptr;
        if (std::find(types.begin(), types.end(), asset.type) != types.end() && target && is_reference(p, target) && !used.count(ptr))
        {
            ++removed;
            continue;
        }
        keep.push_back({i, ptr, target});
    }
    if (!removed)
        return 0;
    set_asset_list(zone, keep);
    if (log)
        log("removed " + std::to_string(removed) + " unused technique set references");
    return removed;
}

void set_asset_list(Zone &zone, const std::vector<AssetListEntry> &keep)
{
    Node *node = zone.assets_node;
    std::vector<uint8_t> data(8 * keep.size());
    std::vector<Node *> children;
    std::vector<ZoneAsset> assets;
    Relocs relocs;
    for (size_t i = 0; i < keep.size(); ++i)
    {
        const AssetListEntry &e = keep[i];
        const size_t j = e.index;
        std::copy(node->data.begin() + 8 * j, node->data.begin() + 8 * j + 4, data.begin() + 8 * i);
        if (e.ptr)
        {
            e.ptr->offset = static_cast<uint32_t>(8 * i + 4);
            relocs.set(e.ptr->offset, e.ptr);
        }
        if (e.target)
            children.push_back(e.target);
        assets.push_back(zone.assets[j]);
    }
    node->data.assign(std::move(data));
    node->relocs = std::move(relocs);
    node->children = std::move(children);
    node->count = static_cast<uint32_t>(2 * keep.size());
    node->segments = {{node->type, node->count, static_cast<uint32_t>(node->data.size()), false}};
    zone.assets = std::move(assets);
}

int dedupe_nested_assets(const Platform &p, Zone &zone, const Log &log, AssetKeyOf key_of, const std::string &what)
{
    if (!key_of)
        key_of = [](const std::string &rec, const std::string &name, Node *) -> std::optional<std::pair<std::string, std::string>> {
            return std::make_pair(rec, lower_latin1(name));
        };
    std::unordered_map<const Node *, std::vector<const Node *>> incoming;
    std::unordered_map<const Ptr *, std::vector<Ptr *>> alias_by_slot;
    zone.root->walk([&](Node *n) {
        for (const auto &[off, ptr] : n->relocs.list())
        {
            if (ptr->kind == Ptr::Kind::Ref && ptr->node)
                incoming[ptr->node].push_back(n);
            else if (ptr->kind == Ptr::Kind::Alias && ptr->slot)
            {
                alias_by_slot[ptr->slot].push_back(ptr);
                if (ptr->slot->owner)
                    incoming[ptr->slot->owner].push_back(n);
            }
        }
    });

    std::map<std::pair<std::string, std::string>, Ptr *> first;
    int removed = 0;
    struct Item
    {
        Node *node;
        Ptr *ptr;
        Node *parent;
    };
    // depth first in stream order
    std::vector<Item> stack{{zone.root, nullptr, nullptr}};
    while (!stack.empty())
    {
        Item item = stack.back();
        stack.pop_back();
        Node *node = item.node;
        Ptr *ptr = item.ptr;
        if (loads(ptr) && node->origin == Origin::Asset)
        {
            std::string name = asset_display_name(p, *node);
            std::optional<std::pair<std::string, std::string>> key;
            if (!name.empty() && name[0] != ',')
                key = key_of(*node->origin_record, name, node);
            if (key)
            {
                auto kept = first.find(*key);
                if (kept == first.end())
                {
                    if (ptr->kind == Ptr::Kind::Insert || (item.parent && (item.parent->block == 4 || item.parent->block == 5 || item.parent->block == 6)))
                        first.emplace(*key, ptr);
                }
                else
                {
                    std::unordered_set<const Node *> inside;
                    node->walk([&](Node *x) { inside.insert(x); });
                    bool outside = false;
                    for (const Node *target : inside)
                    {
                        auto it = incoming.find(target);
                        if (it != incoming.end())
                            for (const Node *src : it->second)
                                if (!inside.count(src))
                                    outside = true;
                    }
                    if (!outside)
                    {
                        Ptr *k = kept->second;
                        auto aliases = alias_by_slot.find(ptr);
                        if (aliases != alias_by_slot.end())
                            for (Ptr *alias : aliases->second)
                                if (alias->index == 1)
                                {
                                    alias->slot = k;
                                    alias->index = k->kind == Ptr::Kind::Insert ? 1 : 0;
                                }
                        ptr->kind = Ptr::Kind::Alias;
                        ptr->node = nullptr;
                        ptr->slot = k;
                        ptr->index = k->kind == Ptr::Kind::Insert ? 1 : 0;
                        auto &c = item.parent->children;
                        c.erase(std::remove(c.begin(), c.end(), node), c.end());
                        ++removed;
                        continue;
                    }
                }
            }
        }
        std::unordered_map<const Node *, Ptr *> loaders;
        for (const auto &[off, q] : node->relocs.list())
            if (loads(q) && q->node)
                loaders[q->node] = q;
        for (auto it = node->children.rbegin(); it != node->children.rend(); ++it)
        {
            auto l = loaders.find(*it);
            stack.push_back({*it, l == loaders.end() ? nullptr : l->second, node});
        }
    }
    if (removed && log)
        log("merge: " + std::to_string(removed) + " " + what + " are shared");
    return removed;
}
} // namespace t4ff
