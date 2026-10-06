#include "convert/library.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>

#include "convert/converter.h"
#include "convert/merge.h"
#include "core/threads.h"
#include "core/zone_cache.h"

namespace t4ff
{
namespace fs = std::filesystem;

namespace
{
const char *GAME_ZONES[] = {"code_pre_gfx", "code_post_gfx", "common", "patch"};
const char *UI_ZONES[] = {"ui", "patch_ui"};

std::string lower_ascii(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string stem_lower(const fs::path &p)
{
    return lower_latin1(p.stem().string());
}

// os.path.normcase(os.path.realpath(p))
std::wstring norm_key(const fs::path &p)
{
    std::error_code ec;
    fs::path real = fs::weakly_canonical(fs::absolute(p), ec);
    if (ec)
        real = fs::absolute(p).lexically_normal();
    std::wstring s = real.wstring();
    for (wchar_t &c : s)
    {
        if (c == L'/')
            c = L'\\';
        c = static_cast<wchar_t>(towlower(c));
    }
    return s;
}

bool inside(const fs::path &path, const fs::path &folder)
{
    std::wstring a = norm_key(path), b = norm_key(folder);
    while (!b.empty() && b.back() == L'\\')
        b.pop_back();
    return a == b || a.rfind(b + L"\\", 0) == 0;
}

std::string node_string(const Node *n)
{
    if (!n)
        return "";
    std::string s(n->data.begin(), n->data.end());
    while (!s.empty() && s.back() == '\0')
        s.pop_back();
    return s;
}

std::filesystem::path g_cache_dir = default_zone_cache_dir();
} // namespace

std::string lower_latin1(std::string s)
{
    for (char &ch : s)
    {
        unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
            ch = static_cast<char>(c + 0x20);
    }
    return s;
}

bool is_game_zone(const fs::path &path)
{
    std::string stem = stem_lower(path);
    for (const char *z : GAME_ZONES)
        if (stem == z || (stem.rfind("localized_", 0) == 0 && stem.substr(10) == z))
            return true;
    return false;
}

bool made_by_t4ff(const fs::path &path)
{
    return fs::exists(fs::absolute(path).parent_path() / T4FF_MARKER);
}

std::vector<fs::path> library_files(const std::vector<fs::path> &paths, const std::string &first, const std::vector<fs::path> &exclude,
                                    std::vector<fs::path> *skipped)
{
    std::vector<fs::path> files;
    std::set<std::wstring> seen;
    for (const fs::path &path : paths)
    {
        std::vector<std::wstring> found;
        if (fs::is_directory(path))
        {
            // as os.walk joins them: the folder as given, then "\name" for each level, sorted as strings
            std::function<void(const std::wstring &)> walk = [&](const std::wstring &dir) {
                std::vector<std::wstring> subdirs;
                for (const auto &e : fs::directory_iterator(fs::path(dir)))
                {
                    std::wstring name = e.path().filename().wstring();
                    std::wstring full = dir + L"\\" + name;
                    if (e.is_directory())
                        subdirs.push_back(full);
                    else
                    {
                        std::wstring lower = name;
                        for (wchar_t &c : lower)
                            c = static_cast<wchar_t>(towlower(c));
                        if (lower.size() >= 3 && lower.compare(lower.size() - 3, 3, L".ff") == 0)
                            found.push_back(full);
                    }
                }
                for (const std::wstring &d : subdirs)
                    walk(d);
            };
            walk(path.wstring());
            std::sort(found.begin(), found.end());
        }
        else
            found.push_back(path.wstring());
        for (const std::wstring &f : found)
        {
            std::wstring key = norm_key(f);
            if (!seen.insert(key).second)
                continue;
            bool excluded = made_by_t4ff(f);
            for (const fs::path &folder : exclude)
                if (!folder.empty() && inside(f, folder))
                    excluded = true;
            if (excluded)
            {
                if (skipped)
                    skipped->push_back(f);
                continue;
            }
            files.push_back(f);
        }
    }
    if (!first.empty())
    {
        std::string want = lower_latin1(first);
        std::stable_sort(files.begin(), files.end(),
                         [&](const fs::path &a, const fs::path &b) { return (stem_lower(a) == want) > (stem_lower(b) == want); });
    }
    return files;
}

// -- ConsoleLibrary

ConsoleLibrary::ConsoleLibrary(const Platform &p_, std::vector<fs::path> paths_, std::function<void(const std::string &)> log,
                               std::string first_, std::vector<fs::path> exclude_, fs::path cache_dir_)
    : p(p_), log_(std::move(log)), first(std::move(first_)), exclude(std::move(exclude_)), cache_dir(std::move(cache_dir_))
{
    for (auto &path : paths_)
        if (!path.empty())
            paths.push_back(path);
}

ConsoleLibrary::~ConsoleLibrary() = default;

void ConsoleLibrary::load()
{
    if (loaded)
        return;
    loaded = true;
    std::vector<fs::path> skipped;
    std::vector<fs::path> files = library_files(paths, first, exclude, &skipped);
    // maps' load zones hold their loading screens only (the loading screen writer reads them itself)
    files.erase(std::remove_if(files.begin(), files.end(),
                               [](const fs::path &f) {
                                   std::string n = lower_latin1(f.filename().string());
                                   return n.size() >= 8 && n.compare(n.size() - 8, 8, "_load.ff") == 0;
                               }),
                files.end());
    if (!skipped.empty())
        log("console library: " + std::to_string(skipped.size()) + " fastfiles t4ff converted left out (e.g. " +
            skipped[0].filename().string() + "): give the console's and CoD Xenon's fastfiles, not converted maps");

    // read on every processor, indexed in file order (the first that has an asset gives it)
    std::vector<std::unique_ptr<Zone>> read(files.size());
    std::vector<std::string> errors(files.size());
    parallel_for(files.size(), 0, [&](size_t i) {
        try
        {
            OpenedZone z = open_zone(files[i], cache_dir);
            if (!z.big_endian)
            {
                errors[i] = "not an Xbox 360 fastfile, ignored";
                return;
            }
            read[i] = read_zone(p, z.bytes);
        }
        catch (const std::exception &e)
        {
            errors[i] = std::string("cannot be read (") + e.what() + "), ignored";
        }
    });

    for (size_t i = 0; i < files.size(); ++i)
    {
        if (!read[i])
        {
            log("warning: " + files[i].string() + ": " + errors[i]);
            continue;
        }
        Zone *zone = read[i].get();
        zones_.push_back(std::move(read[i]));
        bool game = is_game_zone(files[i]);
        std::string stem = stem_lower(files[i]);
        bool ui = std::find(std::begin(UI_ZONES), std::end(UI_ZONES), stem) != std::end(UI_ZONES);
        bool treyarch = true;
        for (const auto &part : fs::absolute(files[i]))
            if (lower_ascii(part.string()) == "usermaps")
                treyarch = false;
        size_t count = 0;
        zone->root->walk([&](Node *node) {
            if (treyarch && node->is_record("MaterialVertexShader") && !node->string)
            {
                Ptr *ptr = node->relocs.get(0);
                Node *name = ptr ? ptr->target() : nullptr;
                if (name && name->string)
                    treyarch_shaders_.insert(lower_latin1(node_string(name)));
            }
            if (node->origin != Origin::Asset)
                return;
            std::string name = asset_display_name(p, *node);
            const std::string &rec = *node->origin_record;
            if (rec == "MaterialTechniqueSet" && !name.empty() && name[0] != ',')
                techsets_[lower_latin1(name)].emplace_back(zone, node);
            if (!name.empty() && name[0] != ',')
            {
                auto key = std::make_pair(rec, lower_latin1(name));
                auto [it, added] = index_.emplace(key, LibraryEntry{zone, node});
                count += added;
                if (game)
                    game_.emplace(key, LibraryEntry{zone, node});
            }
            if (ui && !name.empty() && rec == "menuDef_t")
                ui_menus_.insert(lower_latin1(name[0] == ',' ? name.substr(name.find_first_not_of(',')) : name));
        });
        log("console library: " + files[i].filename().string() + ": " + std::to_string(count) + " assets");
    }
}

namespace
{
std::string strip_commas(const std::string &name)
{
    size_t i = name.find_first_not_of(',');
    return i == std::string::npos ? std::string() : name.substr(i);
}
} // namespace

std::optional<LibraryEntry> ConsoleLibrary::find(const std::string &rec_name, const std::string &name)
{
    if (paths.empty())
        return std::nullopt;
    load();
    std::string key = lower_latin1(strip_commas(name));
    if (rec_name == "MaterialTechniqueSet")
    {
        // a copy every slot of which the game can draw with, when a zone has one (techset_safe)
        auto it = techsets_.find(key);
        if (it != techsets_.end())
            for (const LibraryEntry &e : it->second)
                if (techset_safe(*e.second))
                    return e;
    }
    auto it = index_.find({rec_name, key});
    if (it == index_.end())
        return std::nullopt;
    return it->second;
}

bool ConsoleLibrary::techset_safe(const Node &node)
{
    if (treyarch_shaders_.empty() || std::getenv("T4FF_ALLOW_UNSAFE_TECHSETS"))
        return true; // no zone of Treyarch's given: nothing to tell them apart with
    Ptr *name_ptr = node.relocs.get(0);
    std::string set_name = name_ptr && name_ptr->target() ? lower_latin1(node_string(name_ptr->target())) : "";
    {
        // effects draw with generic vertices (vertexShaderArray 0)
        std::string plain = strip_commas(set_name);
        size_t start = 0;
        while (true)
        {
            size_t end = plain.find('_', start);
            if (plain.substr(start, end == std::string::npos ? std::string::npos : end - start) == "effect")
                return true;
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    const Record &ts_rec = p.record("MaterialTechniqueSet"), &tech_rec = p.record("MaterialTechnique"), &pass_rec = p.record("MaterialPass");
    const Field *techs = ts_rec.field("techniques");
    uint32_t passes = tech_rec.field("passArray")->offset, count = tech_rec.field("passCount")->offset;
    uint32_t shaders = pass_rec.field("vertexShaderArray")->offset;
    for (uint32_t t = 0; t < techs->type->count; ++t)
    {
        Ptr *ptr = node.relocs.get(techs->offset + 4 * t);
        Node *tech = ptr ? ptr->target() : nullptr;
        if (!tech)
            continue;
        uint32_t pass_count = static_cast<uint32_t>(p.read_scalar(tech->data.data() + count, *p.layout->scalar(Scalar::UShort)));
        for (uint32_t k = 0; k < pass_count; ++k)
        {
            for (uint32_t slot = 1; slot < 16; ++slot)
            {
                Ptr *shader_ptr = tech->relocs.get(passes + k * pass_rec.size + shaders + 4 * slot);
                Node *shader = shader_ptr && shader_ptr->kind != Ptr::Kind::Null ? shader_ptr->target() : nullptr;
                Ptr *np = shader ? shader->relocs.get(0) : nullptr;
                Node *name = np ? np->target() : nullptr;
                if (name && !treyarch_shaders_.count(lower_latin1(node_string(name))))
                    return false;
            }
        }
    }
    return true;
}

std::vector<std::string> ConsoleLibrary::names(const std::string &rec_name)
{
    std::vector<std::string> out;
    if (paths.empty())
        return out;
    load();
    for (const auto &[key, entry] : index_)
        if (key.first == rec_name)
            out.push_back(key.second);
    std::sort(out.begin(), out.end());
    return out;
}

bool ConsoleLibrary::in_game_zones(const std::string &rec_name, const std::string &name)
{
    if (paths.empty())
        return false;
    load();
    return game_.count({rec_name, lower_latin1(strip_commas(name))}) != 0;
}

bool ConsoleLibrary::is_stock_menu(const std::string &name)
{
    if (paths.empty())
        return false;
    load();
    std::string n = lower_latin1(strip_commas(name));
    return ui_menus_.count(n) || game_.count({"menuDef_t", n});
}

std::vector<std::pair<std::string, Node *>> ConsoleLibrary::game_rawfiles()
{
    std::vector<std::pair<std::string, Node *>> out;
    if (paths.empty())
        return out;
    load();
    for (const auto &[key, entry] : game_)
        if (key.first == "RawFile")
            out.emplace_back(key.second, entry.second);
    return out;
}

std::optional<LibraryEntry> ConsoleLibrary::find_in_game_zones(const std::string &rec_name, const std::string &name)
{
    if (paths.empty())
        return std::nullopt;
    load();
    auto it = game_.find({rec_name, lower_latin1(strip_commas(name))});
    if (it == game_.end())
        return std::nullopt;
    return it->second;
}

void set_library_cache_dir(const fs::path &dir)
{
    g_cache_dir = dir;
}

ConsoleLibrary &shared_library(const Platform &p, const std::vector<fs::path> &paths, std::function<void(const std::string &)> log,
                               const std::string &first, const std::vector<fs::path> &exclude)
{
    static std::mutex lock;
    static std::map<std::wstring, std::unique_ptr<ConsoleLibrary>> libraries;
    std::wstring key = std::wstring(p.name.begin(), p.name.end()) + L"|";
    for (const auto &path : paths)
        key += path.wstring() + L";";
    key += L"|" + std::wstring(first.begin(), first.end()) + L"|";
    for (const auto &path : exclude)
        key += path.wstring() + L";";
    std::lock_guard guard(lock);
    auto &entry = libraries[key];
    if (!entry)
        entry = std::make_unique<ConsoleLibrary>(p, paths, std::move(log), first, exclude, g_cache_dir);
    return *entry;
}

// -- Cloner

Cloner::Cloner(const Platform &p_, Zone &out_, std::vector<std::optional<std::string>> &strings_, Replace replace_)
    : p(p_), out(out_), replace(std::move(replace_)), strings(strings_)
{
    for (size_t i = 0; i < strings.size(); ++i)
        if (strings[i])
            string_index[*strings[i]] = i; // {s: i}: the last of equal strings
}

void Cloner::register_asset(const std::string &rec_name, const std::string &name, Ptr *ptr)
{
    assets.emplace(std::make_pair(rec_name, lower_latin1(strip_commas(name))), ptr);
}

Node *Cloner::copy_asset(Zone &zone, Node *node)
{
    auto saved_copies = copies;
    auto saved_slots = slots;
    auto saved_assets = assets;
    try
    {
        if (kept.insert(&zone).second && zone.source)
            out.keep_alive.push_back(zone.source); // copies view the library zone's bytes
        return copy(zone, node);
    }
    catch (const LibraryError &)
    {
        copies = std::move(saved_copies);
        slots = std::move(saved_slots);
        assets = std::move(saved_assets);
        throw;
    }
}

uint16_t Cloner::script_string(Zone &zone, uint32_t index)
{
    if (index >= zone.script_strings.size() || !zone.script_strings[index])
        return 0;
    const std::string &text = *zone.script_strings[index];
    auto it = string_index.find(text);
    if (it == string_index.end())
    {
        it = string_index.emplace(text, strings.size()).first;
        strings.emplace_back(text);
    }
    return static_cast<uint16_t>(it->second);
}

Ptr *Cloner::make_ptr(Ptr::Kind kind, Node *owner, uint32_t offset, Node *node, uint32_t index, uint32_t inner, Ptr *slot)
{
    Ptr *ptr = out.new_ptr(kind);
    ptr->node = node;
    ptr->index = index;
    ptr->inner = inner;
    ptr->slot = slot;
    ptr->owner = owner;
    ptr->offset = offset;
    return ptr;
}

Node *Cloner::copy(Zone &zone, Node *src)
{
    Node *nw = out.new_node();
    nw->type = src->type;
    nw->count = src->count;
    nw->block = src->block;
    nw->data = src->data; // a view of the library zone until changed
    nw->push_before = src->push_before;
    nw->push_after = src->push_after;
    nw->string = src->string;
    nw->asset = src->asset;
    nw->runtime_size = src->runtime_size;
    nw->segments = src->segments;
    nw->align = src->align;
    nw->origin = src->origin;
    nw->origin_record = src->origin_record;
    nw->origin_field = src->origin_field;
    nw->delayed = src->delayed;
    copies[src] = nw;

    if (!nw->data.empty() && !nw->string)
    {
        for (uint32_t off : node_script_string_offsets(p, *src))
        {
            if (off + 2 > nw->data.size())
                continue;
            const uint8_t *d = nw->data.data() + off;
            uint32_t value = p.big_endian ? (d[0] << 8 | d[1]) : (d[1] << 8 | d[0]);
            uint16_t mapped = script_string(zone, value);
            uint8_t *w = nw->data.mutable_data() + off;
            if (p.big_endian)
                w[0] = uint8_t(mapped >> 8), w[1] = uint8_t(mapped);
            else
                w[1] = uint8_t(mapped >> 8), w[0] = uint8_t(mapped);
        }
    }
    for (const auto &[off, ptr] : src->relocs.list())
        nw->relocs.set(off, copy_ptr(zone, nw, off, ptr));
    return nw;
}

Ptr *Cloner::add_child(Node *owner, uint32_t offset, Ptr::Kind kind, Node *child)
{
    Ptr *ptr = make_ptr(kind, owner, offset, child);
    owner->children.push_back(child);
    if (kind == Ptr::Kind::Insert)
    {
        child->insert = true;
        child->ptr = ptr;
    }
    return ptr;
}

Ptr *Cloner::copy_ptr(Zone &zone, Node *owner, uint32_t offset, Ptr *ptr)
{
    switch (ptr->kind)
    {
    case Ptr::Kind::Null: return make_ptr(Ptr::Kind::Null, owner, offset);
    case Ptr::Kind::Follow:
    case Ptr::Kind::Insert: {
        Node *target = ptr->node;
        auto key = asset_key(*target);
        if (key)
        {
            auto it = assets.find(*key);
            if (it != assets.end())
            {
                // a nested asset already copied into this zone: point to its slot
                Ptr *first_ptr = it->second;
                slots[ptr] = first_ptr;
                return make_ptr(Ptr::Kind::Alias, owner, offset, nullptr, first_ptr->kind == Ptr::Kind::Insert ? 1 : 0, 0, first_ptr);
            }
        }
        Ptr::Kind kind = key ? Ptr::Kind::Insert : ptr->kind;
        Ptr *nw = add_child(owner, offset, kind, copy_or_replace(zone, target, key));
        slots[ptr] = nw;
        if (key)
            assets[*key] = nw;
        return nw;
    }
    case Ptr::Kind::Ref: {
        Node *target = ptr->node;
        auto it = copies.find(target);
        if (it != copies.end())
            return make_ptr(Ptr::Kind::Ref, owner, offset, it->second, ptr->index, ptr->inner);
        // shared with data outside the copied asset: copy that data here
        if ((ptr->index || ptr->inner) && (target->count > 1 || ptr->inner))
            throw LibraryError("offset pointer into the middle of " + target->repr());
        Node *child = copy(zone, target);
        return add_child(owner, offset, Ptr::Kind::Follow, child);
    }
    case Ptr::Kind::Alias: {
        Ptr *slot = ptr->slot;
        auto mine = slots.find(slot);
        if (mine != slots.end())
        {
            // without an insert slot, the pointer slot itself holds the asset once loaded
            uint32_t index = mine->second->kind == Ptr::Kind::Insert ? ptr->index : 0;
            return make_ptr(Ptr::Kind::Alias, owner, offset, nullptr, index, 0, mine->second);
        }
        Node *target = slot->kind == Ptr::Kind::Alias ? slot->target() : slot->node;
        if (!target)
            throw LibraryError("alias to an empty pointer slot");
        auto key = asset_key(*target);
        if (key)
        {
            auto it = assets.find(*key);
            if (it != assets.end())
            {
                Ptr *first_ptr = it->second;
                slots[slot] = first_ptr;
                return make_ptr(Ptr::Kind::Alias, owner, offset, nullptr, first_ptr->kind == Ptr::Kind::Insert ? 1 : 0, 0, first_ptr);
            }
        }
        // the pointed asset (or data) was loaded before the copied asset: load it here
        auto copied = copies.find(target);
        if (copied != copies.end())
        {
            if (key)
                throw LibraryError("no pointer loads the copied asset " + key->second);
            return make_ptr(Ptr::Kind::Ref, owner, offset, copied->second, 0, 0);
        }
        Ptr::Kind kind = key ? Ptr::Kind::Insert : Ptr::Kind::Follow;
        Ptr *nw = add_child(owner, offset, kind, copy_or_replace(zone, target, key));
        slots[slot] = nw;
        if (key)
            assets[*key] = nw;
        return nw;
    }
    }
    throw LibraryError("unsupported pointer");
}

Node *Cloner::copy_or_replace(Zone &zone, Node *target, const std::optional<std::pair<std::string, std::string>> &key)
{
    if (key && replace)
    {
        if (Node *substitute = replace(key->first, key->second, target))
        {
            copies[target] = substitute;
            return substitute;
        }
    }
    return copy(zone, target);
}

std::optional<std::pair<std::string, std::string>> Cloner::asset_key(const Node &node) const
{
    if (node.origin != Origin::Asset)
        return std::nullopt;
    std::string name = asset_display_name(p, node);
    if (name.empty())
        return std::nullopt;
    return std::make_pair(*node.origin_record, lower_latin1(name));
}
} // namespace t4ff
