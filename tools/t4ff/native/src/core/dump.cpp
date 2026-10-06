#include "core/dump.h"

#include <unordered_map>

namespace t4ff
{
namespace
{

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

} // namespace

std::string dump_text(Zone *zone, size_t *node_count)
{
    std::unordered_map<const Node *, size_t> ids;
    zone->walk([&](Node *n) { ids.emplace(n, ids.size()); });
    auto id = [&](const Node *n) {
        if (!n)
            return std::string("None");
        auto it = ids.find(n);
        return it == ids.end() ? std::string("N?") : "N" + std::to_string(it->second);
    };
    auto addr = [](const std::optional<uint32_t> &a) { return a ? std::to_string(*a) : std::string("-"); };

    std::string out;
    if (zone->source)
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
    if (node_count)
        *node_count = ids.size();
    return out;
}
} // namespace t4ff
