#include "convert/scripts.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "convert/converter.h"
#include "convert/library.h"
#include "core/image.h"
#include "core/pyre.h"
#include "core/pystr.h"

namespace t4ff
{
namespace fs = std::filesystem;
using pyre::Regex;

namespace
{
#include "convert/script_templates.inc"

constexpr const char *ZOMBIE_ENGINE_SCRIPTS[] = {"clientscripts/_callbacks.csc", "clientscripts/_zombie_mode.csc"};

// a Python dict: keys in the order they were first set
template <typename V> class Dict
{
  public:
    V *get(const std::string &k)
    {
        auto it = index_.find(k);
        return it == index_.end() ? nullptr : &items_[it->second].second;
    }
    bool contains(const std::string &k) const
    {
        return index_.count(k) != 0;
    }
    V &operator[](const std::string &k)
    {
        auto [it, added] = index_.emplace(k, items_.size());
        if (added)
            items_.emplace_back(k, V{});
        return items_[it->second].second;
    }
    void setdefault(const std::string &k, V v)
    {
        if (!contains(k))
            (*this)[k] = std::move(v);
    }
    void erase(const std::string &k)
    {
        auto it = index_.find(k);
        if (it == index_.end())
            return;
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(it->second));
        index_.clear();
        for (size_t i = 0; i < items_.size(); ++i)
            index_[items_[i].first] = i;
    }
    size_t size() const
    {
        return items_.size();
    }
    bool empty() const
    {
        return items_.empty();
    }
    auto begin()
    {
        return items_.begin();
    }
    auto end()
    {
        return items_.end();
    }
    auto begin() const
    {
        return items_.begin();
    }
    auto end() const
    {
        return items_.end();
    }

  private:
    std::vector<std::pair<std::string, V>> items_;
    std::unordered_map<std::string, size_t> index_;
};

bool ends_with_any(std::string_view s, std::initializer_list<std::string_view> exts)
{
    for (auto e : exts)
        if (py::ends_with(s, e))
            return true;
    return false;
}

bool is_source_name(std::string_view lower_name)
{
    return ends_with_any(lower_name, {".gsc", ".csc", ".atr"});
}

const Regex &comments_re()
{
    static const Regex re(R"(/\*.*?\*/|//[^\n]*)", pyre::S);
    return re;
}

// the text without its comments
std::string strip_comments(std::string_view text)
{
    return comments_re().sub(text, std::string_view(""));
}

// the text with its comments made spaces (the rest keeps its offsets)
std::string blank_comments(std::string_view text)
{
    return comments_re().sub(text, [](const pyre::Match &m) { return std::string(m.end() - m.start(), ' '); });
}

// a latin-1 name as a path (a character per byte)
fs::path latin1_path(std::string_view name)
{
    std::wstring w;
    for (char c : name)
        w += static_cast<wchar_t>(static_cast<uint8_t>(c));
    return w;
}

std::string newline_of(std::string_view text)
{
    return py::contains(text, "\r\n") ? "\r\n" : "\n";
}

std::string join(const std::vector<std::string> &parts, std::string_view sep = ", ")
{
    return py::join(parts, sep);
}

template <typename C> std::vector<std::string> sorted(const C &c)
{
    std::vector<std::string> out(c.begin(), c.end());
    std::sort(out.begin(), out.end());
    return out;
}

uint32_t field_offset(const Platform &p, const std::string &rec_name, const std::string &field)
{
    const Field *f = p.record(rec_name).field(field);
    if (!f)
        throw std::runtime_error("no field " + rec_name + "." + field);
    return f->offset;
}

Ptr *make_ptr(Zone &zone, Ptr::Kind kind, Node *owner, uint32_t offset, Node *node)
{
    Ptr *ptr = zone.new_ptr(kind);
    ptr->owner = owner;
    ptr->offset = offset;
    ptr->node = node;
    return ptr;
}

Node *new_string(Zone &zone, const Platform &p, const std::string &text, int block)
{
    Node *node = zone.new_node();
    node->type = p.char_type;
    node->block = static_cast<int8_t>(block);
    node->string = true;
    std::vector<uint8_t> data(text.begin(), text.end());
    data.push_back(0);
    node->count = static_cast<uint32_t>(data.size());
    node->segments.push_back({p.char_type, node->count, node->count, false});
    node->align = 1;
    node->data.assign(std::move(data));
    return node;
}

// {normalized name: RawFile} of the scripts with a buffer (later ones of a name win)
Dict<Node *> script_nodes(const Platform &p, Zone &zone)
{
    Dict<Node *> nodes;
    for (auto &[name, node] : rawfiles(p, zone))
        if (!py::starts_with(name, ",") && rawfile_buffer(node))
            nodes[normalize_script(name)] = node;
    return nodes;
}

// (start, end) of the body (inside the braces) of the function name defined in text
std::optional<std::pair<size_t, size_t>> function_body(std::string_view text, const std::string &name)
{
    std::string source = blank_comments(text);
    Regex re("(?m)^[ \\t]*" + pyre::escape(name) + "[ \\t]*\\([^)]*\\)\\s*\\{", pyre::BYTES | pyre::I);
    auto m = re.search(source);
    if (!m)
        return std::nullopt;
    int depth = 1;
    size_t i = m->end();
    bool in_string = false;
    while (i < source.size() && depth)
    {
        char c = source[i];
        if (in_string)
        {
            if (c == '\\')
                ++i;
            else if (c == '"')
                in_string = false;
        }
        else if (c == '"')
            in_string = true;
        else if (c == '{')
            ++depth;
        else if (c == '}')
            --depth;
        ++i;
    }
    if (depth != 0)
        return std::nullopt;
    return std::make_pair(m->end(), i - 1);
}

// the offset of the brace closing the block opened just before start
size_t body_end(std::string_view source, size_t start)
{
    int depth = 1;
    size_t i = start;
    bool in_string = false;
    while (i < source.size() && depth)
    {
        char c = source[i];
        if (in_string)
        {
            if (c == '\\')
                ++i;
            else if (c == '"')
                in_string = false;
        }
        else if (c == '"')
            in_string = true;
        else if (c == '{')
            ++depth;
        else if (c == '}')
            --depth;
        ++i;
    }
    return i - 1;
}

std::string text_of(std::string_view s, size_t start, size_t end)
{
    start = std::min(start, s.size());
    end = std::min(std::max(end, start), s.size());
    return std::string(s.substr(start, end - start));
}

// the text of the string a pointer of the node at offset loads
std::optional<std::string> target_text(Node *node, uint32_t offset)
{
    Ptr *ptr = node->relocs.get(offset);
    Node *target = ptr ? ptr->target() : nullptr;
    if (!target)
        return std::nullopt;
    std::string s(target->data.begin(), target->data.end());
    return std::string(py::rstrip_chars(s, std::string_view("\0", 1)));
}

std::vector<std::string> map_entity_texts(const Platform &p, Zone &zone)
{
    std::vector<std::string> texts;
    uint32_t off = field_offset(p, "MapEnts", "entityString");
    zone.walk([&](Node *n) {
        if (n->is_record("MapEnts") && n->origin == Origin::Asset)
            if (auto t = target_text(n, off))
                texts.push_back(*t);
    });
    return texts;
}

std::set<std::string> actor_classnames(const Platform &p, Zone &zone)
{
    static const Regex re(R"re("classname"\s+"(actor_[^"]*)")re");
    std::set<std::string> names;
    for (const std::string &text : map_entity_texts(p, zone))
        re.for_each(text, [&](const pyre::Match &m) { names.insert(py::lower(m.group(1))); });
    return names;
}

// the entities (key -> value) of the zone's map entities
std::vector<std::map<std::string, std::string>> map_entities(const Platform &p, Zone &zone)
{
    static const Regex block(R"(\{[^{}]*\})");
    static const Regex pair(R"re("([^"]+)"\s+"([^"]*)")re");
    std::vector<std::map<std::string, std::string>> entities;
    for (const std::string &text : map_entity_texts(p, zone))
        block.for_each(text, [&](const pyre::Match &e) {
            std::map<std::string, std::string> entity;
            pair.for_each(e.group(), [&](const pyre::Match &m) { entity[m.str(1)] = m.str(2); });
            entities.push_back(std::move(entity));
        });
    return entities;
}

std::set<std::string> asset_names_lower(const Platform &p, Zone &zone, const char *rec_name)
{
    std::set<std::string> names;
    zone.walk([&](Node *n) {
        if (n->is_record(rec_name) && n->origin == Origin::Asset)
            names.insert(py::lower(asset_name(p, *n)));
    });
    return names;
}

std::string remove_spaces(std::string_view text)
{
    static const Regex re(R"(\s+)", pyre::BYTES);
    return re.sub(text, std::string_view(""));
}

// A zone loading assets ((asset type, name, header)), to merge into another: the zone made the
// headers (it owns them). strings: the script strings they use.
std::unique_ptr<Zone> zone_of_assets(const Platform &p, std::unique_ptr<Zone> zone, const std::vector<std::tuple<std::string, std::string, Node *>> &assets,
                                     const std::vector<std::optional<std::string>> &strings)
{
    Zone &z = *zone;
    size_t count = assets.size();
    Node *node = z.new_node();
    node->type = p.uint_type;
    node->count = static_cast<uint32_t>(2 * count);
    node->block = BLOCK_VIRTUAL;
    std::vector<uint8_t> data(8 * count);
    for (size_t i = 0; i < count; ++i)
    {
        const std::string &type = std::get<0>(assets[i]);
        auto it = std::find(p.asset_types.begin(), p.asset_types.end(), type);
        if (it == p.asset_types.end())
            throw std::runtime_error("no asset type " + type);
        p.put_u32(data.data() + 8 * i, static_cast<uint32_t>(it - p.asset_types.begin()));
        p.put_u32(data.data() + 8 * i + 4, FOLLOW);
    }
    node->segments.push_back({p.uint_type, node->count, static_cast<uint32_t>(data.size()), false});
    node->data.assign(std::move(data));
    node->align = 4;
    for (size_t i = 0; i < count; ++i)
    {
        auto &[type, name, header] = assets[i];
        uint32_t off = static_cast<uint32_t>(8 * i + 4);
        Ptr *ptr = make_ptr(z, Ptr::Kind::Follow, node, off, header);
        node->relocs.set(off, ptr);
        node->children.push_back(header);
        header->ptr = ptr;
        ZoneAsset asset;
        asset.type = type;
        asset.ptr = ptr;
        asset.name = name;
        z.assets.push_back(std::move(asset));
    }
    Node *root = z.new_node();
    root->type = p.uint_type;
    root->count = 4;
    root->block = -1;
    root->data.assign(std::vector<uint8_t>(16));
    root->children = {node};
    z.platform = p.name;
    z.script_strings = strings.empty() ? std::vector<std::optional<std::string>>{std::nullopt} : strings;
    z.assets_node = node;
    z.root = root;
    return zone;
}

// A console RawFile asset name holding text, laid out as template (another one), made in zone.
Node *make_rawfile(Zone &zone, const Platform &p, Node *tmpl, const std::string &name, const std::string &text)
{
    uint32_t name_off = field_offset(p, "RawFile", "name");
    uint32_t len_off = field_offset(p, "RawFile", "len");
    uint32_t buffer_off = field_offset(p, "RawFile", "buffer");
    Node *old_buffer = rawfile_buffer(tmpl);
    Node *node = zone.new_node();
    node->type = tmpl->type;
    node->count = 1;
    node->block = tmpl->block;
    node->push_before = tmpl->push_before;
    node->push_after = tmpl->push_after;
    node->segments = tmpl->segments;
    node->align = tmpl->align.value_or(4);
    node->origin = Origin::Asset;
    node->origin_record = intern("RawFile");
    std::vector<uint8_t> data(tmpl->data.size());
    p.put_u32(data.data() + name_off, FOLLOW);
    p.put_u32(data.data() + len_off, static_cast<uint32_t>(text.size()));
    p.put_u32(data.data() + buffer_off, FOLLOW);
    node->data.assign(std::move(data));

    Node *string = new_string(zone, p, name, old_buffer->block);

    Node *buffer = zone.new_node();
    buffer->type = old_buffer->type;
    buffer->count = static_cast<uint32_t>(text.size() + 1);
    buffer->block = old_buffer->block;
    std::vector<uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    buffer->data.assign(std::move(bytes));
    buffer->segments.push_back({old_buffer->segments.empty() ? old_buffer->type : old_buffer->segments[0].type, buffer->count, buffer->count, false});
    buffer->align = old_buffer->align.value_or(1);
    buffer->origin = Origin::Member;
    buffer->origin_record = intern("RawFile");
    buffer->origin_field = intern("buffer");

    node->relocs.set(name_off, make_ptr(zone, Ptr::Kind::Follow, node, name_off, string));
    Ptr *bptr = make_ptr(zone, Ptr::Kind::Follow, node, buffer_off, buffer);
    node->relocs.set(buffer_off, bptr);
    buffer->ptr = bptr;
    node->children = {string, buffer};
    return node;
}

// Raw files a map lacks, from the map's own files (sources[0]), the console library, then other PC
// files (sources[1:]: the game folder, the mod tools' raw folder). Copies go into out.
class RawfileFinder
{
  public:
    RawfileFinder(const Platform &p, Zone &out, Node *tmpl, const std::vector<const IwdLibrary *> &sources, ConsoleLibrary *library)
        : p(p), out(out), tmpl(tmpl), sources(sources), library(library)
    {
        strings.emplace_back(std::nullopt);
        if (library)
            cloner = std::make_unique<Cloner>(p, out, strings);
    }

    std::pair<Node *, std::string> find(const std::string &ref)
    {
        std::optional<std::vector<uint8_t>> data = sources.empty() ? std::nullopt : sources[0]->read(ref);
        if (data)
            return {make_rawfile(out, p, tmpl, ref, stripped(*data)), "map's files"};
        if (library)
        {
            if (auto found = library->find("RawFile", ref))
            {
                try
                {
                    return {cloner->copy_asset(*found->first, found->second), "Xbox 360 fastfiles"};
                }
                catch (const LibraryError &)
                {
                }
            }
        }
        for (size_t i = 1; i < sources.size(); ++i)
        {
            auto d = sources[i]->read(ref);
            if (!d || d->empty())
                d = sources[i]->read("raw/" + ref);
            if (d)
                return {make_rawfile(out, p, tmpl, ref, stripped(*d)), "PC files"};
        }
        return {nullptr, ""};
    }

    static std::string stripped(const std::vector<uint8_t> &data)
    {
        size_t n = data.size();
        while (n && data[n - 1] == 0)
            --n;
        return std::string(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));
    }

    const Platform &p;
    Zone &out;
    Node *tmpl;
    const std::vector<const IwdLibrary *> &sources;
    ConsoleLibrary *library;
    std::vector<std::optional<std::string>> strings;
    std::unique_ptr<Cloner> cloner;
};

// a regex for the script name (normalized) where scripts include or call it
Regex script_path(const std::string &name)
{
    std::vector<std::string> parts = py::split(name.substr(0, name.size() - 4), "/");
    std::string pattern = "(?i)(?<![\\w\\\\/])";
    for (size_t i = 0; i < parts.size(); ++i)
        pattern += (i ? "[\\\\/]" : "") + pyre::escape(parts[i]);
    pattern += "(?=\\s*(?:::|;))";
    return Regex(pattern, pyre::BYTES);
}

bool rename_rawfile(const Platform &p, Zone &zone, Node *node, const std::string &name)
{
    uint32_t off = field_offset(p, "RawFile", "name");
    Ptr *old = node->relocs.get(off);
    Node *old_string = old ? old->target() : nullptr;
    if (old_string)
    {
        bool shared = false;
        zone.walk([&](Node *other) {
            for (const auto &[o, ptr] : other->relocs.list())
                if (ptr != old && ptr->target() == old_string)
                    shared = true;
        });
        if (shared)
            return false;
    }
    Node *string = new_string(zone, p, name, old_string ? old_string->block : rawfile_buffer(node)->block);
    auto it = std::find(node->children.begin(), node->children.end(), old_string);
    if (old_string && it != node->children.end())
        *it = string;
    else
        node->children.insert(node->children.begin(), string); // the name is loaded before the buffer
    node->relocs.set(off, make_ptr(zone, Ptr::Kind::Follow, node, off, string));
    for (ZoneAsset &asset : zone.assets)
        if (asset.node() == node)
            asset.name = name;
    return true;
}

// the scripts a bare function name may come from: those the include statements name
std::set<std::string> include_references(const std::string &level_script, std::string_view source)
{
    static const Regex include(R"(#include\s+[\w\\/]+\s*;)", pyre::BYTES);
    std::vector<std::string> found;
    include.for_each(source, [&](const pyre::Match &m) { found.push_back(m.str()); });
    return script_references(level_script, py::join(found, "\n"));
}
} // namespace

// -- names and texts

std::string normalize_script(std::string_view name)
{
    size_t start = name.find_first_not_of(',');
    std::string s(start == std::string_view::npos ? std::string_view() : name.substr(start));
    std::replace(s.begin(), s.end(), '\\', '/');
    return py::lower(s);
}

bool is_script_name(std::string_view name)
{
    std::string lower = py::lower(name);
    return ends_with_any(lower, {".gsc", ".csc"});
}

std::vector<std::pair<std::string, Node *>> rawfiles(const Platform &p, Zone &zone)
{
    std::vector<std::pair<std::string, Node *>> out;
    zone.walk([&](Node *n) {
        if (n->origin == Origin::Asset && n->is_record("RawFile"))
            out.emplace_back(asset_name(p, *n), n);
    });
    return out;
}

Node *rawfile_buffer(Node *node)
{
    for (Node *c : node->children)
        if ((c->origin == Origin::Member || c->origin == Origin::PtrArray) && c->origin_record && *c->origin_record == "RawFile" &&
            c->origin_field && *c->origin_field == "buffer")
            return c;
    return nullptr;
}

std::string rawfile_text(Node *node)
{
    Node *buffer = rawfile_buffer(node);
    if (!buffer)
        return "";
    size_t n = buffer->data.size();
    while (n && buffer->data[n - 1] == 0)
        --n;
    return std::string(buffer->data.begin(), buffer->data.begin() + n);
}

void set_rawfile_text(const Platform &p, Node *node, const std::string &text)
{
    Node *buffer = rawfile_buffer(node);
    std::vector<uint8_t> data(text.begin(), text.end());
    data.push_back(0);
    buffer->count = static_cast<uint32_t>(data.size());
    const TypeRef *type = buffer->segments.empty() ? buffer->type : buffer->segments[0].type;
    buffer->segments = {Segment{type, buffer->count, buffer->count, false}};
    buffer->data.assign(std::move(data));
    p.put_u32(node->data.mutable_data() + field_offset(p, "RawFile", "len"), static_cast<uint32_t>(text.size()));
}

std::set<std::string> script_references(std::string_view name, std::string_view text)
{
    static const Regex include(R"(#include\s+([\w\\/]+)\s*;)");
    static const Regex call(R"(\b([A-Za-z_]\w*(?:[\\/]\w+)+)\s*::)");
    std::string ext = py::ends_with(py::lower(name), ".csc") ? ".csc" : ".gsc";
    std::string source = strip_comments(text);
    std::set<std::string> refs;
    include.for_each(source, [&](const pyre::Match &m) { refs.insert(normalize_script(m.group(1)) + ext); });
    call.for_each(source, [&](const pyre::Match &m) { refs.insert(normalize_script(m.group(1)) + ext); });
    return refs;
}

std::set<std::string> anim_tree_references(std::string_view text)
{
    static const Regex using_tree(R"re(#using_animtree\s*\(\s*"([\w\\/]+)"\s*\))re");
    std::set<std::string> refs;
    using_tree.for_each(strip_comments(text), [&](const pyre::Match &m) { refs.insert("animtrees/" + normalize_script(m.group(1)) + ".atr"); });
    return refs;
}

std::set<std::string> script_strings(const Platform &p, Zone &zone)
{
    static const Regex literal(R"re("([^"\\\n]{1,64})")re", pyre::BYTES);
    std::set<std::string> strings;
    for (auto &[name, node] : rawfiles(p, zone))
        if (!py::starts_with(name, ",") && is_script_name(name) && rawfile_buffer(node))
            literal.for_each(rawfile_text(node), [&](const pyre::Match &m) { strings.insert(py::lower(m.group(1))); });
    return strings;
}

namespace
{
const Regex &getdvar_re()
{
    static const Regex re(R"re(\bgetdvar(?:int|float)?\s*\(\s*"(\w+)"\s*\))re", pyre::BYTES | pyre::I);
    return re;
}
} // namespace

std::set<std::string> script_dvars(const Platform &p, Zone &zone)
{
    std::set<std::string> read;
    for (auto &[name, node] : rawfiles(p, zone))
        if (!py::starts_with(name, ",") && is_script_name(name) && rawfile_buffer(node))
            getdvar_re().for_each(blank_comments(rawfile_text(node)), [&](const pyre::Match &m) { read.insert(py::lower(m.group(1))); });
    return read;
}

bool is_zombie_map(const std::vector<std::pair<std::string, std::string>> &texts)
{
    for (const auto &[name, text] : texts)
        if (py::starts_with(name, "maps/_zombiemode") || py::contains(py::lower_ascii(text), "_zombiemode"))
            return true;
    return false;
}

MenuValues menu_dvar_values(Zone &zone)
{
    static const Regex setdvar(R"re("setdvar"\s+"(\w+)"\s+(?:"([^"]*)"|([^\s";]+))(\s*\()?)re", pyre::I);
    MenuValues values;
    zone.walk([&](Node *node) {
        if (!node->string)
            return;
        std::string raw(node->data.begin(), node->data.end());
        std::string text(py::rstrip_chars(raw, std::string_view("\0", 1)));
        if (!py::contains(py::lower(text), "setdvar"))
            return;
        setdvar.for_each(text, [&](const pyre::Match &m) {
            if (m.has(4) && !m.group(4).empty())
                return; // an expression ("dvarString" ( "other" ))
            std::string value = m.has(2) ? m.str(2) : m.str(3);
            values[py::lower(m.group(1))].insert(value);
        });
    });
    return values;
}

// -- the map's own scripts

int override_scripts(const Platform &p, const std::vector<Zone *> &zones, const IwdLibrary &loose, const Log &log)
{
    std::set<std::string> replaced;
    for (Zone *zone : zones)
    {
        for (auto &[name, node] : rawfiles(p, *zone))
        {
            if (py::starts_with(name, ",") || !is_source_name(py::lower(name)))
                continue;
            auto data = loose.read(normalize_script(name));
            Node *buffer = rawfile_buffer(node);
            if (!data || !buffer)
                continue;
            std::string text = RawfileFinder::stripped(*data);
            if (rawfile_text(node) == text)
                continue;
            set_rawfile_text(p, node, text);
            replaced.insert(normalize_script(name));
        }
    }
    if (!replaced.empty() && log)
        log("scripts: " + std::to_string(replaced.size()) + " taken from the map's own files, as the PC game does (" + join(sorted(replaced)) + ")");
    return static_cast<int>(replaced.size());
}

std::unique_ptr<Zone> missing_scripts_zone(const Platform &p, Zone &zone, const std::vector<const IwdLibrary *> &sources, ConsoleLibrary *library,
                                           const Log &log, const std::vector<std::string> &roots, std::set<std::string> *from_map)
{
    Dict<Node *> defined;
    Dict<std::string> texts;
    Node *tmpl = nullptr;
    for (auto &[name, node] : rawfiles(p, zone))
    {
        std::string key = normalize_script(name);
        if (py::starts_with(name, ","))
        {
            defined.setdefault(key, nullptr);
            continue;
        }
        defined[key] = node;
        if (!tmpl)
            tmpl = node;
        if (ends_with_any(key, {".gsc", ".csc"}))
            texts[key] = rawfile_text(node);
    }
    if (!tmpl)
        return nullptr;

    auto extra = std::make_unique<Zone>();
    std::vector<std::tuple<std::string, std::string, Node *>> added;
    Dict<std::string> origins, unknown;
    RawfileFinder finder(p, *extra, tmpl, sources, library);
    std::vector<std::pair<std::string, std::string>> queue(texts.begin(), texts.end());
    Dict<std::string> engine; // scripts the game loads by name, and where they came from
    auto lookup = [&](const std::string &ref) -> Node * {
        Node **n = defined.get(ref);
        return n ? *n : nullptr;
    };
    if (is_zombie_map(queue))
    {
        for (const char *ref : ZOMBIE_ENGINE_SCRIPTS)
        {
            if (lookup(ref))
                continue; // the map's own
            auto [node, origin] = finder.find(ref); // even when the game's own zones have one: not this one
            if (!node)
            {
                unknown[ref] = "the game";
                continue;
            }
            engine[ref] = origin;
            defined[ref] = node;
            added.emplace_back("rawfile", asset_name(p, *node), node);
            queue.emplace_back(ref, rawfile_text(node));
        }
    }
    // the player animation script is read against the multiplayer anim tree: a mod's version of the
    // script wants the mod's tree
    if (lookup(PLAYER_ANIM_SCRIPT))
        queue.emplace_back(PLAYER_ANIM_SCRIPT, "#using_animtree( \"multiplayer\" );");
    std::vector<std::string> by_name;
    for (const std::string &root : roots)
    {
        std::string ref = normalize_script(root);
        auto data = sources.empty() ? std::nullopt : sources[0]->read(ref);
        if (lookup(ref) || !data)
            continue;
        Node *node = make_rawfile(*extra, p, tmpl, ref, RawfileFinder::stripped(*data));
        by_name.push_back(ref);
        defined[ref] = node;
        added.emplace_back("rawfile", asset_name(p, *node), node);
        queue.emplace_back(ref, rawfile_text(node));
    }
    while (!queue.empty())
    {
        auto [user, text] = queue.back();
        queue.pop_back();
        std::set<std::string> refs = script_references(user, text);
        if (ends_with_any(user, {".gsc", ".csc"}) || user == PLAYER_ANIM_SCRIPT)
            for (const std::string &r : anim_tree_references(text))
                refs.insert(r);
        for (const std::string &ref : refs)
        {
            if (defined.contains(ref) || unknown.contains(ref))
                continue;
            bool in_game = library && library->in_game_zones("RawFile", ref);
            if (in_game && (sources.empty() || !sources[0]->read(ref)))
            {
                defined[ref] = nullptr; // the game's own zones have it, the map has no copy of its own
                continue;
            }
            auto [node, origin] = finder.find(ref);
            if (!node)
            {
                unknown[ref] = user;
                continue;
            }
            if (in_game && from_map)
                from_map->insert(ref);
            origins[ref] = origin;
            defined[ref] = node;
            added.emplace_back("rawfile", asset_name(p, *node), node);
            queue.emplace_back(ref, rawfile_text(node));
        }
    }
    if (log)
    {
        std::vector<std::pair<std::string, std::string>> e(engine.begin(), engine.end());
        std::sort(e.begin(), e.end());
        for (const auto &[ref, source] : e)
            log("scripts: added " + ref + " (the game loads it for zombie maps, the map has none) from the " + source);
        for (const std::string &ref : by_name)
            log("scripts: added " + ref + " (the game loads it by name for the map, no zone of it has it) from the map's files");
        std::vector<std::pair<std::string, std::string>> o(origins.begin(), origins.end());
        std::sort(o.begin(), o.end());
        for (const auto &[ref, source] : o)
            log("scripts: added " + ref + " (used by the map's scripts, not in its fastfiles) from the " + source);
    }
    for (const char *ref : ZOMBIE_ENGINE_SCRIPTS)
    {
        std::string *u = unknown.get(ref);
        if (u && *u == "the game")
        {
            unknown.erase(ref);
            if (log)
                log(std::string("warning: ") + ref +
                    ", which the game loads for zombie maps, is in none of the map's files nor the console fastfiles given: give a map converted by "
                    "CoD Xenon among them (--console-zone)");
        }
    }
    if (!unknown.empty() && log)
    {
        std::vector<std::string> names;
        for (const auto &[ref, user] : unknown)
            names.push_back(ref);
        std::sort(names.begin(), names.end());
        names.resize(std::min<size_t>(names.size(), 6));
        log("scripts: " + std::to_string(unknown.size()) + " scripts the map uses are left to the game's own zones (e.g. " + join(names) +
            "). Should the console stop with \"Could not find script\", add the Xbox 360 fastfile that has it.");
    }
    if (added.empty())
        return nullptr;
    return zone_of_assets(p, std::move(extra), added, finder.strings);
}

// -- assets the game looks up by name (named.py)

namespace
{
std::string without_block(const std::string &source, const std::string &name)
{
    Regex re("^\\s*" + name + "\\s*\\{", pyre::M | pyre::I);
    auto m = re.search(source);
    if (!m)
        return source;
    int depth = 1;
    size_t i = m->end();
    while (i < source.size() && depth)
    {
        depth += source[i] == '{' ? 1 : source[i] == '}' ? -1 : 0;
        ++i;
    }
    return source.substr(0, m->start()) + source.substr(i);
}

void add_unique(std::vector<std::string> &list, std::unordered_set<std::string> &seen, std::string s)
{
    if (seen.insert(s).second)
        list.push_back(std::move(s));
}

// the animations a player animation script plays as players move, in order (not its scriptevent block)
std::vector<std::string> player_animations(const std::string &text)
{
    static const Regex line(R"(^\s*(?:both|legs|torso|turret)\s+(\w+))", pyre::M);
    std::string source = without_block(strip_comments(text), "scriptevent");
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    line.for_each(source, [&](const pyre::Match &m) { add_unique(out, seen, py::lower(m.group(1))); });
    return out;
}

// the animations an anim tree names, in order: its leaves (a name followed by a block is a blend node)
std::vector<std::string> anim_tree_animations(const std::string &text)
{
    static const Regex token(R"([{}]|[\w.]+(?:\s*:\s*[\w ]+)?)");
    std::vector<std::string> tokens;
    token.for_each(strip_comments(text), [&](const pyre::Match &m) { tokens.push_back(m.str()); });
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const std::string &t = tokens[i];
        if (std::string_view("{}").find(t) != std::string_view::npos)
            continue;
        if (i + 1 < tokens.size() && tokens[i + 1] == "{")
            continue; // a blend node
        add_unique(names, seen, py::lower(py::strip(py::split(t, ":")[0])));
    }
    return names;
}

using Scripts = std::vector<std::pair<std::string, std::string>>;

std::vector<std::string> anim_trees_used(const Scripts &scripts)
{
    static const Regex re(R"re(#using_animtree\s*\(\s*"(\w+)"\s*\))re");
    std::set<std::string> names;
    for (const auto &[n, text] : scripts)
        re.for_each(strip_comments(text), [&](const pyre::Match &m) { names.insert(py::lower(m.group(1))); });
    std::vector<std::string> out;
    for (const std::string &n : names)
        out.push_back("animtrees/" + n + ".atr");
    return out;
}

std::unordered_set<std::string> played_animations(const Scripts &scripts)
{
    static const Regex re(R"(%\s*(\w+))");
    std::unordered_set<std::string> names;
    for (const auto &[n, text] : scripts)
        re.for_each(strip_comments(text), [&](const pyre::Match &m) { names.insert(py::lower(m.group(1))); });
    return names;
}

std::vector<std::string> shellshock_candidates(const Scripts &scripts)
{
    static const Regex re(R"re("(\w+)")re");
    std::set<std::string> names;
    for (const auto &[n, text] : scripts)
        re.for_each(strip_comments(text), [&](const pyre::Match &m) { names.insert(py::lower(m.group(1))); });
    std::vector<std::string> out;
    for (const std::string &n : names)
        out.push_back("shock/" + n + ".shock");
    return out;
}

std::string some(const std::vector<std::string> &names, size_t n)
{
    std::vector<std::string> first(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(std::min(n, names.size())));
    return join(first) + (names.size() > n ? ", ..." : "");
}
} // namespace

std::unique_ptr<Zone> named_assets_zone(const Platform &p, Zone &zone, const std::vector<const IwdLibrary *> &sources, ConsoleLibrary *library,
                                        const Log &log)
{
    static const Regex footstep(R"(^(?:q?step_(?:walk|run|sprint|prone|scrape)\w*|land_\w+|gear_\w+)$)", pyre::I);
    std::set<std::pair<std::string, std::string>> defined;
    Scripts scripts;
    Node *tmpl = nullptr;
    zone.walk([&](Node *node) {
        if (node->origin == Origin::Asset)
        {
            std::string name = asset_name(p, *node);
            size_t start = name.find_first_not_of(',');
            defined.emplace(node->origin_record ? *node->origin_record : "", py::lower(start == std::string::npos ? "" : name.substr(start)));
        }
    });
    for (auto &[name, node] : rawfiles(p, zone))
    {
        if (py::starts_with(name, ","))
            continue;
        if (!tmpl)
            tmpl = node;
        if (is_script_name(name))
            scripts.emplace_back(name, rawfile_text(node));
    }
    if (!tmpl)
        return nullptr;

    auto wanted = [&](const std::string &rec, const std::string &name) {
        if (defined.count({rec, py::lower(name)}))
            return false;
        return !library || !library->in_game_zones(rec, name);
    };
    auto extra = std::make_unique<Zone>();
    RawfileFinder finder(p, *extra, tmpl, sources, library);
    std::vector<std::tuple<std::string, std::string, Node *>> added;

    std::vector<std::string> shocks;
    for (const std::string &ref : shellshock_candidates(scripts))
    {
        if (!wanted("RawFile", ref))
            continue;
        auto [node, origin] = finder.find(ref);
        if (node)
        {
            added.emplace_back("rawfile", ref, node);
            shocks.push_back(ref);
        }
    }
    if (!shocks.empty() && log)
        log("shellshocks: added " + join(shocks) + " (used by the map's scripts, in none of its zones)");

    // the animations of the anim trees: the map's own trees, else those the game's zones have
    if (library)
    {
        std::map<std::string, std::string> trees;
        for (auto &[name, node] : rawfiles(p, zone))
            if (!py::starts_with(name, ",") && py::ends_with(py::lower(name), ".atr"))
                trees[py::lower(name)] = rawfile_text(node);
        for (const std::string &tree : anim_trees_used(scripts))
            if (!trees.count(tree))
                if (auto found = library->find_in_game_zones("RawFile", tree))
                    trees[tree] = rawfile_text(found->second);
        // only those a script plays: the map's, and the game's own it runs (its animscripts)
        Scripts all = scripts;
        for (auto &[n, node] : library->game_rawfiles())
            if (ends_with_any(n, {".gsc", ".csc"}))
                all.emplace_back(n, rawfile_text(node));
        std::unordered_set<std::string> played = played_animations(all);
        Dict<std::string> taken;
        std::vector<std::string> missing;
        int unplayed = 0;
        for (const auto &[tree, text] : trees)
        {
            for (const std::string &anim : anim_tree_animations(text))
            {
                if (taken.contains(anim) || !wanted("XAnimParts", anim))
                    continue;
                if (!played.count(anim))
                {
                    ++unplayed;
                    continue;
                }
                auto found = library->find("XAnimParts", anim);
                if (!found)
                {
                    missing.push_back(anim);
                    continue;
                }
                Node *node;
                try
                {
                    node = finder.cloner->copy_asset(*found->first, found->second);
                }
                catch (const LibraryError &)
                {
                    missing.push_back(anim);
                    continue;
                }
                added.emplace_back("xanim", anim, node);
                defined.emplace("XAnimParts", anim);
                taken[anim] = tree;
            }
        }
        if (log)
        {
            std::set<std::string> trees_taken;
            for (const auto &[a, t] : taken)
                trees_taken.insert(t);
            for (const std::string &tree : trees_taken)
            {
                std::vector<std::string> anims;
                for (const auto &[a, t] : taken)
                    if (t == tree)
                        anims.push_back(a);
                log("animations: added " + std::to_string(anims.size()) + " of " + tree +
                    " (the game loads them by name, none of the map's zones nor the game's have them: " + some(anims, 4) + ")");
            }
            if (!missing.empty())
            {
                std::vector<std::string> first(missing.begin(), missing.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(4, missing.size())));
                log("animations: " + std::to_string(missing.size()) + " the map's scripts play are in none of the console fastfiles given (e.g. " +
                    join(first) + "): the game reports them (\"Could not load xanim\") and plays none");
            }
            if (unplayed)
                log("animations: " + std::to_string(unplayed) +
                    " the map's anim trees name and no script plays are left out (the game reports them, \"Could not load xanim\")");
        }
    }

    // the footsteps: the game plays the sounds of steps, landings and gear by name
    if (library)
    {
        std::vector<std::string> steps;
        for (const std::string &alias : library->names("snd_alias_list_t"))
        {
            if (!footstep.match(alias) || !wanted("snd_alias_list_t", alias))
                continue;
            auto found = library->find("snd_alias_list_t", alias);
            Node *node;
            try
            {
                if (!found)
                    throw LibraryError("not found");
                node = finder.cloner->copy_asset(*found->first, found->second);
            }
            catch (const LibraryError &)
            {
                continue;
            }
            added.emplace_back("sound", alias, node);
            defined.emplace("snd_alias_list_t", alias);
            steps.push_back(alias);
        }
        if (!steps.empty() && log)
        {
            std::vector<std::string> first(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(4, steps.size())));
            log("footsteps: added " + std::to_string(steps.size()) +
                " sounds the game plays by name for steps, landings and gear, in none of the map's zones nor the game's (" + join(first) + ", ...)");
        }
    }

    // the player animation script: the map's own, else the game's
    std::optional<std::string> script;
    for (auto &[name, node] : rawfiles(p, zone))
        if (py::lower(name) == PLAYER_ANIM_SCRIPT)
        {
            script = rawfile_text(node);
            break;
        }
    if (!script && library)
        if (auto found = library->find_in_game_zones("RawFile", PLAYER_ANIM_SCRIPT))
            script = rawfile_text(found->second);
    if (script && library)
    {
        std::vector<std::string> anims;
        for (const std::string &anim : player_animations(*script))
        {
            if (!wanted("XAnimParts", anim))
                continue;
            auto found = library->find("XAnimParts", anim);
            if (!found)
                continue;
            Node *node;
            try
            {
                node = finder.cloner->copy_asset(*found->first, found->second);
            }
            catch (const LibraryError &)
            {
                continue;
            }
            added.emplace_back("xanim", anim, node);
            anims.push_back(anim);
        }
        if (!anims.empty() && log)
            log("player animations: added " + std::to_string(anims.size()) +
                " the player animation script plays, in none of the map's zones nor the game's (" + some(anims, 4) + ")");
    }
    if (added.empty())
        return nullptr;
    for (auto &a : added)
        std::get<1>(a) = asset_name(p, *std::get<2>(a));
    return zone_of_assets(p, std::move(extra), added, finder.strings);
}

// -- the mod's versions of the game's scripts

std::map<std::string, std::string> keep_mod_scripts(const Platform &p, Zone &zone, const std::set<std::string> &mod_scripts, ConsoleLibrary *library,
                                                    const std::string &level_script, const Log &log)
{
    static const char *const ENGINE_RUN[] = {"animscripts/", "clientscripts/", "character/", "aitype/", "mptype/", "xmodelalias/",
                                             "maps/_callbacksetup.gsc", "maps/_callbackglobal.gsc"};
    if (!library)
        return {};
    Dict<Node *> nodes = script_nodes(p, zone);
    std::map<std::string, std::string> texts;
    for (auto &[name, node] : nodes)
        if (py::ends_with(name, ".gsc"))
            texts[name] = rawfile_text(node);
    std::map<std::string, std::optional<std::string>> game_texts;
    auto game = [&](const std::string &name) -> const std::optional<std::string> & {
        auto it = game_texts.find(name);
        if (it == game_texts.end())
        {
            auto found = library->find_in_game_zones("RawFile", name);
            it = game_texts.emplace(name, found ? std::optional<std::string>(rawfile_text(found->second)) : std::nullopt).first;
        }
        return it->second;
    };
    std::vector<std::string> shadowed;
    for (const auto &[name, text] : texts)
        if (mod_scripts.count(name) && game(name) && remove_spaces(*game(name)) != remove_spaces(text))
            shadowed.push_back(name);
    if (shadowed.empty())
        return {};

    // the scripts the map runs, as the console has them: the game's copy of those both have
    std::map<std::string, bool> runs_game_copy;
    std::map<std::string, std::set<std::string>> calls;
    std::vector<std::string> queue{py::lower(level_script), "maps/_callbacksetup.gsc"};
    while (!queue.empty())
    {
        std::string name = queue.back();
        queue.pop_back();
        if (calls.count(name))
            continue;
        std::optional<std::string> text = game(name);
        runs_game_copy[name] = text.has_value();
        if (!text)
        {
            auto it = texts.find(name);
            if (it != texts.end())
                text = it->second;
        }
        calls[name] = text ? script_references(name, *text) : std::set<std::string>();
        for (const std::string &r : calls[name])
            if (!calls.count(r))
                queue.push_back(r);
    }

    std::map<std::string, std::string> renamed;
    for (const std::string &name : shadowed)
    {
        if (!calls.count(name))
            continue; // nothing the map runs uses it
        bool engine_run = false;
        for (const char *prefix : ENGINE_RUN)
            engine_run = engine_run || py::starts_with(name, prefix);
        if (engine_run)
        {
            if (log)
                log("scripts: " + name + " of the mod runs from the map's scripts folder: the engine runs it by name, and the game's own zones have one");
            continue;
        }
        std::vector<std::string> game_callers;
        for (const auto &[caller, refs] : calls)
            if (refs.count(name) && runs_game_copy[caller] && caller != name)
                game_callers.push_back(caller);
        if (!game_callers.empty())
        {
            if (log)
                log("scripts: " + name + " of the mod runs from the map's scripts folder: the game's own zones have one, which their " + game_callers[0] +
                    " calls too");
            continue;
        }
        std::string stem = name.substr(0, name.size() - 4) + "_mod";
        std::string candidate = stem + ".gsc";
        for (int i = 2; nodes.contains(candidate) || calls.count(candidate) || game(candidate); ++i)
            candidate = stem + std::to_string(i) + ".gsc";
        if (!rename_rawfile(p, zone, *nodes.get(name), candidate))
        {
            if (log)
                log("scripts: " + name + " of the mod runs from the map's scripts folder: its name is shared with other data of the zone");
            continue;
        }
        renamed[name] = candidate;
    }
    if (renamed.empty())
        return {};
    std::vector<std::pair<std::string, Regex>> paths;
    for (const auto &[old, now] : renamed)
        paths.emplace_back(py::replace(now.substr(0, now.size() - 4), "/", "\\"), script_path(old));
    for (auto &[name, node] : nodes)
    {
        if (!py::ends_with(name, ".gsc"))
            continue;
        std::string text = rawfile_text(node);
        std::string new_text = text;
        for (const auto &[path, re] : paths)
            new_text = re.sub(new_text, [&](const pyre::Match &) { return path; });
        if (new_text != text)
            set_rawfile_text(p, node, new_text);
    }
    if (log)
        for (const auto &[old, now] : renamed)
            log("scripts: " + old + " of the mod runs as " + now +
                ": the game's own zones have one too, which the console would run instead (the PC runs the mod's)");
    return renamed;
}

std::map<std::string, std::string> usermap_scripts(const Platform &p, Zone &zone, const std::set<std::string> &mod_scripts, ConsoleLibrary *library,
                                                   const std::map<std::string, std::string> &renamed)
{
    std::map<std::string, std::string> scripts;
    if (!library)
        return scripts;
    for (auto &[name, node] : rawfiles(p, zone))
    {
        std::string key = normalize_script(name);
        if (py::starts_with(name, ",") || !mod_scripts.count(key) || renamed.count(key) || !is_source_name(key) || !rawfile_buffer(node))
            continue;
        auto found = library->find_in_game_zones("RawFile", key);
        std::string text = rawfile_text(node);
        if (found && remove_spaces(rawfile_text(found->second)) != remove_spaces(text))
            scripts[key] = text;
    }
    return scripts;
}

std::vector<std::string> write_usermap_scripts(const std::map<std::string, std::string> &scripts, const fs::path &out_dir, const Log &log)
{
    fs::path folder = out_dir / USERMAP_SCRIPTS;
    if (fs::is_directory(folder))
        fs::remove_all(folder);
    std::vector<std::string> names;
    for (const auto &[name, text] : scripts)
    {
        fs::path path = folder;
        for (const std::string &part : py::split(name, "/"))
            path /= latin1_path(part);
        fs::create_directories(path.parent_path());
        std::ofstream f(path, std::ios::binary);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f)
            throw std::runtime_error("cannot write " + path.string());
        names.push_back(name);
    }
    if (!scripts.empty() && log)
    {
        std::vector<std::string> first(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(6, names.size())));
        log("scripts: " + std::to_string(scripts.size()) + " of the mod's versions of the game's scripts go to " + USERMAP_SCRIPTS +
            "/, which CoD Xe loads in place of the game's (" + join(first) + (names.size() > 6 ? ", ..." : "") + ")");
    }
    std::vector<std::string> trees;
    for (const std::string &n : names)
        if (py::ends_with(n, ".atr"))
            trees.push_back(n);
    if (!trees.empty() && log)
        log("warning: the map has its own " + join(trees) + ": it needs a CoD Xe build that loads " + USERMAP_SCRIPTS +
            "/ (older ones compile its scripts against the game's and stop with \"Server script compile error ... not defined in anim tree\")");
    return names;
}

// -- fixes for the console

std::vector<std::string> use_key_hints(const Platform &p, Zone &zone, const Log &log)
{
    static const Regex hint(R"re("(Press|Hold) F (?=[Tt]o ))re", pyre::BYTES);
    std::vector<std::string> changed;
    for (auto &[name, node] : rawfiles(p, zone))
    {
        if (py::starts_with(name, ",") || !py::ends_with(py::lower(name), ".gsc") || !rawfile_buffer(node))
            continue;
        std::string text = rawfile_text(node);
        if (!py::contains(py::lower_ascii(text), "sethintstring"))
            continue;
        int count = 0;
        std::string now = hint.sub(text, std::string_view("\"\\1 &&1 "), 0, &count);
        if (count)
        {
            set_rawfile_text(p, node, now);
            changed.push_back(normalize_script(name));
        }
    }
    if (!changed.empty() && log)
        log("scripts: hints naming the PC's use key (F) show the console's use button (" + join(changed) + ")");
    return changed;
}

std::vector<std::string> fix_modder_help(const Platform &p, Zone &zone, const Log &log)
{
    static const Regex modder(R"((?m)^(modderHelp[ \t]*\([ \t]*(\w+)[ \t]*,[ \t]*(\w+)[ \t]*\)[ \t]*\r?\n[ \t]*\{))", pyre::BYTES);
    static const std::string FIX = "// t4ff: a missing entity stops the setup without developer too";
    std::vector<std::string> changed;
    for (auto &[name, node] : rawfiles(p, zone))
    {
        if (py::starts_with(name, ",") || !py::ends_with(py::lower(name), ".gsc") || !rawfile_buffer(node))
            continue;
        std::string text = rawfile_text(node);
        if (!py::contains(text, "modderHelp") || py::contains(text, FIX))
            continue;
        std::string nl = newline_of(text);
        int count = 0;
        std::string now = modder.sub(
            text,
            [&](const pyre::Match &m) {
                return m.str(1) + nl + "\t" + FIX + nl + "\tif( !isDefined( " + m.str(2) + " ) && isDefined( " + m.str(3) +
                       " ) && getDvarInt( \"developer\" ) < 1 )" + nl + "\t\treturn true;" + nl;
            },
            0, &count);
        if (count)
        {
            set_rawfile_text(p, node, now);
            changed.push_back(normalize_script(name));
        }
    }
    if (!changed.empty() && log)
        log("scripts: the modding kit's check for missing entities works without developer too, so setups stop there (" + join(changed) + ")");
    return changed;
}

std::vector<std::string> spawn_script_origins(const Platform &p, Zone &zone, const Log &log)
{
    static const Regex spawn(R"re((\bspawn\s*\(\s*)"script_struct")re", pyre::BYTES | pyre::I);
    std::vector<std::string> changed;
    for (auto &[name, node] : rawfiles(p, zone))
    {
        if (py::starts_with(name, ",") || !is_script_name(name) || !rawfile_buffer(node))
            continue;
        std::string text = rawfile_text(node);
        if (!py::contains(text, "script_struct"))
            continue;
        int count = 0;
        std::string now = spawn.sub(text, std::string_view("\\1\"script_origin\""), 0, &count);
        if (count)
        {
            set_rawfile_text(p, node, now);
            changed.push_back(normalize_script(name));
        }
    }
    if (!changed.empty() && log)
        log("scripts: script_struct entities, which the console cannot spawn, are spawned as script_origin (" + join(changed) + ")");
    return changed;
}

std::vector<std::string> valid_cursor_hints(const Platform &p, Zone &zone, const Log &log)
{
    static const Regex cursor(R"re((\bsetcursorhint\s*\(\s*)"([^"]*)")re", pyre::BYTES | pyre::I);
    static const std::set<std::string> VALID = {"HINT_INHERIT", "HINT_NOICON",   "HINT_SEAT",     "HINT_ACTIVATE",
                                                "HINT_HEALTH",  "HINT_FRIENDLY", "HINT_SPECTATOR"};
    std::vector<std::string> changed;
    for (auto &[name, node] : rawfiles(p, zone))
    {
        if (py::starts_with(name, ",") || !py::ends_with(py::lower(name), ".gsc") || !rawfile_buffer(node))
            continue;
        std::string text = rawfile_text(node);
        if (!py::contains(py::lower_ascii(text), "setcursorhint"))
            continue;
        std::string now = cursor.sub(text, [&](const pyre::Match &m) {
            if (VALID.count(py::upper(m.group(2))))
                return m.str();
            return m.str(1) + "\"HINT_NOICON\"";
        });
        if (now != text)
        {
            set_rawfile_text(p, node, now);
            changed.push_back(normalize_script(name));
        }
    }
    if (!changed.empty() && log)
        log("scripts: hint types the console has not are HINT_NOICON (its SetCursorHint crashes on others) (" + join(changed) + ")");
    return changed;
}

std::vector<std::string> local_client_effects(const Platform &p, Zone &zone, const Log &log)
{
    static const Regex call(R"(\b(playfx|playfxontag|playviewmodelfx|playsound|playloopsound|spawnfx|stopfx|deletefx)(\s*\(\s*)([A-Za-z_]\w*)(\s*,))",
                            pyre::BYTES | pyre::I);
    static const Regex param(R"(^(?:local_?)?client_?num$)", pyre::BYTES | pyre::I);
    static const Regex params(R"((?m)^[ \t]*([A-Za-z_]\w*)[ \t]*\(([^)]*)\)\s*\{)", pyre::BYTES);
    static const Regex counter(R"(\bfor\s*\(\s*([A-Za-z_]\w*)\s*=[^;]*;([^;]*);)", pyre::BYTES | pyre::I);
    std::vector<std::string> changed;
    for (auto &[name, node] : rawfiles(p, zone))
    {
        if (py::starts_with(name, ",") || !py::ends_with(py::lower(name), ".csc") || !rawfile_buffer(node))
            continue;
        std::string text = rawfile_text(node);
        std::string source = blank_comments(text);
        std::vector<std::tuple<size_t, size_t, std::string>> edits;
        params.for_each(source, [&](const pyre::Match &m) {
            std::optional<std::string> local;
            for (const std::string &part : py::split(m.group(2), ","))
            {
                std::string prm(py::bstrip(part));
                if (param.match(prm))
                {
                    local = prm;
                    break;
                }
            }
            if (!local)
                return;
            size_t start = m.end(), end = body_end(source, m.end());
            std::string body = text_of(source, start, end);
            std::set<std::string> counters;
            counter.for_each(body, [&](const pyre::Match &c) {
                if (!py::contains(py::lower_ascii(c.group(2)), "player"))
                    counters.insert(py::lower_ascii(c.group(1)));
            });
            call.for_each(body, [&](const pyre::Match &c) {
                std::string arg = py::lower_ascii(c.group(3));
                if (counters.count(arg) && arg != py::lower_ascii(*local))
                    edits.emplace_back(start + c.start(3), start + c.end(3), *local);
            });
        });
        if (edits.empty())
            continue;
        std::sort(edits.begin(), edits.end(), [](const auto &a, const auto &b) { return a > b; });
        std::string now = text;
        for (const auto &[s, e, replacement] : edits)
        {
            size_t a = std::min(s, now.size());
            size_t b = std::min(std::max(e, a), now.size());
            now.replace(a, b - a, replacement);
        }
        set_rawfile_text(p, node, now);
        changed.push_back(normalize_script(name));
    }
    if (!changed.empty() && log)
        log("scripts: client effects for local clients that are loop counters are for the script's own local client "
            "(the console's other split screen clients froze it: UGX's Thundergun) (" +
            join(changed) + ")");
    return changed;
}

std::vector<std::string> precache_before_waits(const Platform &p, Zone &zone, const std::string &level_script_name, const Log &log)
{
    static const Regex precache(R"re(\b(precache(?:item|model|shader|shellshock|rumble|menu|string))\s*\(\s*(&?"[^"\n]*")\s*\)\s*;)re",
                                pyre::BYTES | pyre::I);
    static const Regex called(R"((?:\b([A-Za-z_]\w*(?:[\\/]\w+)+)\s*::\s*)?\b([A-Za-z_]\w*)\s*\()", pyre::BYTES);
    static const Regex set_model(R"re(\bsetmodel\s*\(\s*"([^"\n]+)"\s*\))re", pyre::BYTES | pyre::I);
    static const std::string MARK = "// t4ff: the precaches of the setup below, before the level script's first wait";
    Dict<Node *> nodes = script_nodes(p, zone);
    std::string level_script = py::lower(level_script_name);
    if (!nodes.contains(level_script))
        return {};
    std::string level_text = rawfile_text(*nodes.get(level_script));
    if (py::contains(level_text, MARK))
        return {};
    auto body = function_body(level_text, "main");
    if (!body)
        return {};
    std::string source = blank_comments(level_text);
    std::vector<std::string> includes{level_script};
    for (const std::string &r : include_references(level_script, source))
        includes.push_back(r);
    std::vector<std::string> wanted;
    std::set<std::pair<std::string, std::string>> seen_calls;
    Dict<std::vector<std::pair<size_t, size_t>>> moved; // the calls made at the start instead, by script
    std::string main_body = text_of(source, body->first, body->second);
    called.for_each(main_body, [&](const pyre::Match &m) {
        std::string function = m.str(2);
        std::string lower = py::lower(function);
        for (const char *skip : {"main", "if", "while", "for", "switch", "return", "wait", "thread"})
            if (lower == skip)
                return;
        std::vector<std::string> candidates = m.has(1) && !m.group(1).empty() ? std::vector<std::string>{normalize_script(m.group(1)) + ".gsc"} : includes;
        for (const std::string &script : candidates)
        {
            if (seen_calls.count({script, lower}) || !nodes.contains(script))
                continue;
            seen_calls.emplace(script, lower);
            std::string text = blank_comments(rawfile_text(*nodes.get(script)));
            auto found = function_body(text, function);
            if (!found)
                continue;
            precache.for_each(
                text,
                [&](const pyre::Match &c) {
                    std::string call = c.str();
                    if (std::find(wanted.begin(), wanted.end(), call) == wanted.end())
                        wanted.push_back(call);
                    moved[script].emplace_back(c.start(), c.end());
                },
                found->first, found->second);
            break;
        }
    });
    std::set<std::string> main_precaches;
    precache.for_each(main_body, [&](const pyre::Match &c) { main_precaches.insert(py::lower_ascii(c.group())); });
    std::vector<std::string> kept;
    for (const std::string &c : wanted)
        if (!main_precaches.count(py::lower_ascii(c)))
            kept.push_back(c);
    wanted = kept;
    // the setup's own calls, after the wait, would stop it: they are comments
    for (auto &[script, spans] : moved)
    {
        std::string text = rawfile_text(*nodes.get(script));
        std::set<std::pair<size_t, size_t>> unique(spans.begin(), spans.end());
        for (auto it = unique.rbegin(); it != unique.rend(); ++it)
        {
            auto [start, end] = *it;
            text = text_of(text, 0, start) + "/* t4ff: made first in the level script: " + text_of(text, start, end) + " */" +
                   text_of(text, end, std::string::npos);
        }
        set_rawfile_text(p, *nodes.get(script), text);
    }
    level_text = rawfile_text(*nodes.get(level_script));
    body = function_body(level_text, "main");
    // models the scripts set by name that nothing precaches
    std::set<std::string> loaded;
    zone.walk([&](Node *n) {
        if (n->is_record("XModel") && n->origin == Origin::Asset)
            loaded.insert(py::lower(asset_name(p, *n)));
    });
    std::set<std::string> precached;
    std::vector<std::string> set_by_name;
    for (auto &[name, node] : nodes)
    {
        if (!py::ends_with(name, ".gsc"))
            continue;
        std::string text = blank_comments(rawfile_text(node));
        precache.for_each(text, [&](const pyre::Match &c) {
            if (py::lower_ascii(c.group(1)) == "precachemodel")
                precached.insert(py::lower_ascii(py::strip_chars(c.group(2), "\"&")));
        });
        set_model.for_each(text, [&](const pyre::Match &m) {
            std::string model = m.str(1);
            if (std::find(set_by_name.begin(), set_by_name.end(), model) == set_by_name.end())
                set_by_name.push_back(model);
        });
    }
    for (const std::string &model : set_by_name)
    {
        std::string call = "PrecacheModel( \"" + model + "\" );";
        if (!precached.count(py::lower_ascii(model)) && loaded.count(py::lower(model)) &&
            std::find(wanted.begin(), wanted.end(), call) == wanted.end())
            wanted.push_back(call);
    }
    if (wanted.empty())
        return {};
    std::string nl = newline_of(level_text);
    std::string insert = nl + "\t" + MARK;
    for (const std::string &c : wanted)
        insert += nl + "\t" + c;
    insert += nl;
    set_rawfile_text(p, *nodes.get(level_script), text_of(level_text, 0, body->first) + insert + text_of(level_text, body->first, std::string::npos));
    if (log)
    {
        std::vector<std::string> first(wanted.begin(), wanted.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(4, wanted.size())));
        log("scripts: " + std::to_string(wanted.size()) + " precaches of the setup " + level_script +
            " runs are made before its first wait too, as the console needs (" + join(first) + ")");
    }
    return wanted;
}

std::map<std::string, std::string> menu_dvar_defaults(const Platform &p, Zone &zone, const MenuValues &menu_values, const std::string &level_script_name,
                                                      const Log &log)
{
    static const Regex engine(R"(^(?:cg|ui|r|g|sv|cl|com|con|snd|bg|player|scr|xblive|party|dw|fs|net|sys|ai|compass|hud|in|vid|)"
                              R"(developer|onlinegame|systemlink|splitscreen|credits)(?:_|$))",
                              pyre::I);
    static const std::string MARK = "// t4ff: the options the map's own menus set on PC, which the console does not show";
    Dict<Node *> nodes = script_nodes(p, zone);
    std::string level_script = py::lower(level_script_name);
    if (!nodes.contains(level_script) || menu_values.empty())
        return {};
    std::string level_text = rawfile_text(*nodes.get(level_script));
    if (py::contains(level_text, MARK))
        return {};
    std::set<std::string> read;
    for (auto &[name, node] : nodes)
        if (ends_with_any(name, {".gsc", ".csc"}))
            getdvar_re().for_each(blank_comments(rawfile_text(node)), [&](const pyre::Match &m) { read.insert(py::lower(m.group(1))); });
    std::map<std::string, std::string> defaults;
    for (const auto &[dvar, values] : menu_values)
        if (read.count(dvar) && values.size() == 1 && !engine.match(dvar))
            defaults[dvar] = *values.begin();
    auto body = function_body(level_text, "main");
    if (defaults.empty() || !body)
        return {};
    std::string nl = newline_of(level_text);
    std::vector<std::string> lines{"\t" + MARK};
    for (const auto &[dvar, value] : defaults)
    {
        lines.push_back("\tif( GetDvar( \"" + dvar + "\" ) == \"\" )");
        lines.push_back("\t\tSetDvar( \"" + dvar + "\", \"" + value + "\" );");
    }
    std::string insert = nl + py::join(lines, nl) + nl;
    set_rawfile_text(p, *nodes.get(level_script), text_of(level_text, 0, body->first) + insert + text_of(level_text, body->first, std::string::npos));
    if (log)
    {
        std::vector<std::string> parts;
        for (const auto &[d, v] : defaults)
            parts.push_back(d + " " + v);
        log("scripts: dvars the map's own menus set on PC get their value when unset (" + join(parts) + ")");
    }
    return defaults;
}

bool options_script(const Platform &p, Zone &zone, const std::string &level_script_name, const std::vector<MapOption> &options,
                    const std::vector<std::string> &menus, const Log &log)
{
    static const Regex open_menu(R"((?<![\w:])OpenMenu(?=\s*\())", pyre::BYTES | pyre::I);
    static const std::string MARK = "// t4ff: the map's options";
    Dict<Node *> nodes = script_nodes(p, zone);
    std::string level_script = py::lower(level_script_name);
    if (!nodes.contains(level_script) || menus.size() != options.size() + 1)
        return false;
    std::string text = rawfile_text(*nodes.get(level_script));
    auto body = function_body(text, "main");
    if (py::contains(text, MARK) || !body)
        return false;
    size_t waiting = open_menu.finditer(blank_comments(text)).size();
    text = open_menu.sub(text, std::string_view("t4ff_open_menu"));
    body = function_body(text, "main");
    std::string nl = newline_of(text);
    std::vector<std::string> start{"\t" + MARK + ": asked as the level starts (t4ff_options)"};
    for (const std::string &menu : menus)
        start.push_back("\tPrecacheMenu( \"" + menu + "\" );");
    start.push_back("\tlevel.t4ff_asking = GetDvar( \"t4ff_options_restart\" ) != \"1\";");
    start.push_back("\tlevel thread t4ff_options();");
    std::string questions;
    for (size_t i = 0; i < options.size(); ++i)
        questions += "\tif( t4ff_ask( player, \"" + menus[i] + "\", \"" + options[i].dvar + "\" ) )\n\t\tchanged = true;\n";
    std::string functions = py::replace(py::replace(OPTIONS_FUNCTIONS, "%(questions)s", questions), "%(restart)s", menus.back());
    text = text_of(text, 0, body->first) + nl + py::join(start, nl) + nl + text_of(text, body->first, std::string::npos);
    if (!py::ends_with(text, "\n"))
        text += nl;
    set_rawfile_text(p, *nodes.get(level_script), text + py::replace(functions, "\n", nl));
    if (log)
    {
        std::vector<std::string> labels;
        for (const MapOption &o : options)
            labels.push_back(o.label);
        log("scripts: " + level_script + " asks the map's options as it starts (" + join(labels) + "); the " + std::to_string(waiting) +
            " menus it opens wait for them");
    }
    return true;
}

std::vector<std::string> speed_up_zombies_only(const Platform &p, Zone &zone, const Log &log)
{
    static const Regex speed(
        R"re((zombie_stragglers\s*=\s*GetAiArray\s*\(\s*\"axis\"\s*\)\s*;\s*for\s*\([^)]*zombie_stragglers\.size[^)]*\)\s*\{))re",
        pyre::BYTES | pyre::I);
    static const std::string FIX = "// t4ff: only zombies";
    std::vector<std::string> changed;
    for (auto &[name, node] : rawfiles(p, zone))
    {
        if (py::starts_with(name, ",") || !py::ends_with(py::lower(name), ".gsc") || !rawfile_buffer(node))
            continue;
        std::string text = rawfile_text(node);
        if (!py::contains(text, "zombie_stragglers") || py::contains(text, FIX))
            continue;
        std::string nl = newline_of(text);
        std::string fix = nl + "\t\t" + FIX + nl + "\t\tif( !IsDefined( zombie_stragglers[i].is_zombie ) || !zombie_stragglers[i].is_zombie )" + nl +
                          "\t\t\tcontinue;" + nl;
        int count = 0;
        std::string now = speed.sub(text, [&](const pyre::Match &m) { return m.str(1) + fix; }, 0, &count);
        if (count)
        {
            set_rawfile_text(p, node, now);
            changed.push_back(normalize_script(name));
        }
    }
    if (!changed.empty() && log)
        log("scripts: speed_up_zombies() hurries zombies only, not the map's other AI (" + join(changed) + ")");
    return changed;
}

std::vector<std::string> zombie_idles_for_zombies(const Platform &p, Zone &zone, const Log &log)
{
    static const Regex idle(
        R"re(anim\.idleAnimArray\s*\[\s*"(?:stand|crouch)"\s*\]\s*\[\s*\d+\s*\]\s*\[\s*\d+\s*\]\s*=\s*%\s*ai_zombie_idle)re",
        pyre::BYTES | pyre::I);
    static const Regex pose(R"re((anim\.idleAnim(?:Array|Weights)\s*\[\s*)"(stand|crouch)")re", pyre::BYTES | pyre::I);
    static const Regex generic_human(R"re(#using_animtree\s*\(\s*"generic_human"\s*\))re", pyre::BYTES | pyre::I);
    static const Regex header(R"((?m)^[ \t]*([A-Za-z_]\w*)[ \t]*\([^)]*\)\s*\{)", pyre::BYTES);
    static const std::string MARK = "t4ff_zombie_idles";
    bool soldiers = false;
    for (const std::string &name : actor_classnames(p, zone))
        soldiers = soldiers || !py::contains(name, "zombie");
    if (!soldiers)
        return {};
    std::vector<std::string> changed;
    for (auto &[name, node] : rawfiles(p, zone))
    {
        if (py::starts_with(name, ",") || !py::ends_with(py::lower(name), ".gsc") || !rawfile_buffer(node))
            continue;
        std::string text = rawfile_text(node);
        if (py::contains(text, MARK) || !generic_human.search(text))
            continue;
        std::string source = blank_comments(text);
        auto m = idle.search(source);
        if (!m)
            continue;
        // the function setting the zombie idles
        std::optional<std::string> function_name;
        header.for_each(source, [&](const pyre::Match &h) {
            if (h.start() < m->start())
                function_name = h.str(1);
        });
        auto body = function_name ? function_body(text, *function_name) : std::nullopt;
        if (!body || !(body->first <= m->start() && m->start() < body->second))
            continue;
        std::string nl = newline_of(text);
        std::string function = pose.sub(text_of(text, body->first, body->second), std::string_view("\\1\"zombie_\\2\""));
        function = nl + "\tlevel thread t4ff_zombie_idles();" + function;
        text = text_of(text, 0, body->first) + function + text_of(text, body->second, std::string::npos);
        if (!py::ends_with(text, "\n"))
            text += nl;
        set_rawfile_text(p, node, text + py::replace(ZOMBIE_IDLE_FUNCTIONS, "\n", nl));
        changed.push_back(normalize_script(name));
    }
    if (!changed.empty() && log)
        log("scripts: the map has soldiers, and only its zombies idle as zombies (" + join(changed) + ")");
    return changed;
}

namespace
{
// the word ending at end in source, past the spaces before it
std::string word_before(std::string_view source, size_t end)
{
    while (end > 0 && py::bytes_space(static_cast<uint8_t>(source[end - 1])))
        --end;
    size_t start = end;
    while (start > 0 && (py::ascii_alnum(static_cast<uint8_t>(source[start - 1])) || source[start - 1] == '_'))
        --start;
    return std::string(source.substr(start, end - start));
}

// whether the call at start of source (comments blanked) has no entity it is called on
bool global_call(std::string_view source, size_t start)
{
    size_t i = start;
    while (i > 0 && py::bytes_space(static_cast<uint8_t>(source[i - 1])))
        --i;
    if (i == 0)
        return true;
    char before = source[i - 1];
    if (before == ']')
        return false;
    if (before == ')')
    {
        int depth = 0;
        while (i > 0)
        {
            --i;
            if (source[i] == ')')
                ++depth;
            else if (source[i] == '(')
            {
                --depth;
                if (depth == 0)
                    break;
            }
        }
        std::string w = py::lower_ascii(word_before(source, i));
        return w == "if" || w == "while" || w == "for" || w == "foreach" || w == "switch";
    }
    if (py::ascii_alnum(static_cast<uint8_t>(before)) || before == '_')
    {
        std::string w = py::lower_ascii(word_before(source, i));
        return w == "else" || w == "return";
    }
    return true;
}
} // namespace

std::vector<std::string> splitscreen_fog(const Platform &p, Zone &zone, const std::string &level_script_name,
                                         const std::function<bool(const std::string &)> &is_game_script, const Log &log)
{
    static const Regex set_vol_fog(R"((?<![\w:\\/])SetVolFog(?=\s*\())", pyre::BYTES | pyre::I);
    static const Regex own(R"(\bset_splitscreen_fog\s*\(|\bt4ff_vol_fog\b)", pyre::BYTES | pyre::I);
    static const std::string MARK = "// t4ff: in splitscreen, the map's fog is its own";
    Dict<Node *> nodes = script_nodes(p, zone);
    std::string level_script = py::lower(level_script_name);
    std::vector<std::pair<std::string, Node *>> by_name(nodes.begin(), nodes.end());
    std::sort(by_name.begin(), by_name.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::string> fog_scripts;
    size_t calls = 0;
    for (auto &[name, node] : by_name)
    {
        if (!py::ends_with(name, ".gsc") || is_game_script(name))
            continue;
        std::string text = rawfile_text(node);
        std::string source = blank_comments(text);
        if (own.search(source))
            continue; // its fog has its splitscreen fog already
        std::vector<size_t> starts;
        set_vol_fog.for_each(source, [&](const pyre::Match &m) {
            if (global_call(source, m.start()))
                starts.push_back(m.start());
        });
        if (starts.empty())
            continue;
        for (auto it = starts.rbegin(); it != starts.rend(); ++it)
            text = text_of(text, 0, *it) + "t4ff_vol_fog" + text_of(text, *it + 9, std::string::npos);
        std::string nl = newline_of(text);
        text = std::string(py::brstrip(text)) + nl;
        set_rawfile_text(p, node, text + py::replace(SPLITSCREEN_FOG_FUNCTION, "\n", nl));
        fog_scripts.push_back(name);
        calls += starts.size();
    }
    std::vector<std::string> changed = fog_scripts;
    std::string text = nodes.contains(level_script) ? rawfile_text(*nodes.get(level_script)) : "";
    auto body = function_body(text, "main");
    if (body && !py::contains(text, MARK))
    {
        std::string nl = newline_of(text);
        std::string insert = nl + "\t" + MARK + ": no placeholder fog of maps/_load.gsc" + nl + "\tlevel.splitscreen_fog = true;" + nl;
        set_rawfile_text(p, *nodes.get(level_script), text_of(text, 0, body->first) + insert + text_of(text, body->first, std::string::npos));
        if (std::find(changed.begin(), changed.end(), level_script) == changed.end())
            changed.push_back(level_script);
        if (log)
            log("scripts: in splitscreen the map keeps its own fog, not the yellow placeholder of maps/_load.gsc");
    }
    if (!fog_scripts.empty() && log)
        log("scripts: in splitscreen the map's " + std::to_string(calls) + " SetVolFog call" + (calls > 1 ? "s" : "") + " set" + (calls > 1 ? "" : "s") +
            " the game's splitscreen fog (" + join(fog_scripts) + ")");
    return changed;
}

namespace
{
// Python's float(): the number, nullopt when it is not one
std::optional<double> parse_float(const std::string &s)
{
    std::string t(py::strip(s));
    if (t.empty())
        return std::nullopt;
    char *end = nullptr;
    double v = strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size())
        return std::nullopt;
    return v;
}

// Python's format(x, "g")
std::string format_g(double v)
{
    if (std::isinf(v))
        return v > 0 ? "inf" : "-inf";
    if (std::isnan(v))
        return "nan";
    char buf[64];
    snprintf(buf, sizeof buf, "%g", v);
    return buf;
}
} // namespace

int mounted_guns(const Platform &p, Zone &zone, const std::string &level_script_name, const Log &log)
{
    static const Regex turrets(R"(^mg42_bipod_(stand|crouch|prone)$)", pyre::I);
    static const std::string MARK = "// t4ff: the MG42 turrets are held guns";
    Dict<Node *> nodes = script_nodes(p, zone);
    std::string level_script = py::lower(level_script_name);
    std::vector<std::pair<std::string, std::string>> gsc;
    for (auto &[name, node] : nodes)
        if (py::ends_with(name, ".gsc"))
            gsc.emplace_back(name, rawfile_text(node));
    if (!nodes.contains(level_script) || !is_zombie_map(gsc))
        return 0;
    std::string text = rawfile_text(*nodes.get(level_script));
    auto body = function_body(text, "main");
    if (!body || py::contains(text, MARK))
        return 0;
    std::vector<std::pair<std::array<double, 3>, std::string>> guns;
    for (auto &entity : map_entities(p, zone))
    {
        auto get = [&](const char *key) {
            auto it = entity.find(key);
            return it == entity.end() ? std::string() : it->second;
        };
        std::string weaponinfo = get("weaponinfo"); // the match views it
        auto m = turrets.match(weaponinfo);
        std::vector<std::string> origin = py::split(get("origin"));
        if (py::lower(get("classname")) == "misc_turret" && m && origin.size() == 3)
        {
            std::array<double, 3> xyz{};
            for (int k = 0; k < 3; ++k)
            {
                auto v = parse_float(origin[k]);
                if (!v)
                    throw std::runtime_error("could not convert string to float: '" + origin[k] + "'");
                xyz[k] = *v;
            }
            guns.emplace_back(xyz, py::lower(m->group(1)));
        }
    }
    if (guns.empty())
        return 0;
    std::set<std::string> weapons = asset_names_lower(p, zone, "WeaponDef");
    std::string gun;
    for (const char *w : {"mg42", "mg42_bipod"})
        if (weapons.count(w))
        {
            gun = w;
            break;
        }
    if (gun.empty())
    {
        if (log)
            log("scripts: the map's " + std::to_string(guns.size()) +
                " MG42 turrets show the console no world once used, and the map has no portable MG42 to hold instead");
        return 0;
    }
    std::string nl = newline_of(text);
    std::vector<std::string> start{"\t" + MARK + " (t4ff_mounted_guns)", "\tPrecacheItem( \"" + gun + "\" );", "\tlevel thread t4ff_mounted_guns();"};
    text = text_of(text, 0, body->first) + nl + py::join(start, nl) + nl + text_of(text, body->first, std::string::npos);
    std::string lines;
    for (size_t i = 0; i < guns.size(); ++i)
    {
        const auto &[xyz, stance] = guns[i];
        std::string k = std::to_string(i);
        lines += "\tguns[" + k + "] = ( " + format_g(xyz[0]) + ", " + format_g(xyz[1]) + ", " + format_g(xyz[2]) + " );\n\tstances[" + k + "] = \"" +
                 stance + "\";\n";
    }
    std::string functions = py::replace(py::replace(MOUNTED_GUN_FUNCTIONS, "%(guns)s", lines), "%(gun)s", gun);
    set_rawfile_text(p, *nodes.get(level_script), std::string(py::brstrip(text)) + nl + py::replace(functions, "\n", nl));
    if (log)
        log("scripts: the map's " + std::to_string(guns.size()) + " MG42 turrets show the console no world once used: a player on one holds the gun instead (" +
            gun + ", see t4ff_mounted_guns)");
    return static_cast<int>(guns.size());
}

bool reset_sustain_ammo(const Platform &p, Zone &zone, const std::string &level_script_name, const Log &log)
{
    static const Regex on(R"re(\bSet(?:Saved)?Dvar\s*\(\s*"player_sustainammo"\s*,(?!\s*"?0"?\s*\)))re", pyre::BYTES | pyre::I);
    static const std::string MARK = "// t4ff: no endless ammo left on by an earlier game";
    Dict<Node *> nodes = script_nodes(p, zone);
    std::vector<std::string> users;
    for (auto &[name, node] : nodes)
        if (py::ends_with(name, ".gsc") && name != "maps/_cheat.gsc" && on.search(blank_comments(rawfile_text(node))))
            users.push_back(name);
    std::sort(users.begin(), users.end());
    std::string level_script = py::lower(level_script_name);
    if (users.empty() || !nodes.contains(level_script))
        return false;
    std::string text = rawfile_text(*nodes.get(level_script));
    auto body = function_body(text, "main");
    if (!body || py::contains(text, MARK))
        return false;
    std::string nl = newline_of(text);
    std::string insert = nl + "\t" + MARK + " (player_sustainammo)" + nl + "\tSetSavedDvar( \"player_sustainammo\", 0 );" + nl;
    set_rawfile_text(p, *nodes.get(level_script), text_of(text, 0, body->first) + insert + text_of(text, body->first, std::string::npos));
    if (log)
        log("scripts: the level script turns endless ammo (player_sustainammo) off as it starts: " + join(users) +
            " turn it on for a while, and a game ending meanwhile left it on for the next one");
    return true;
}
} // namespace t4ff
