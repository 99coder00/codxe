#include "convert/menus.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>

#include "convert/assets.h"
#include "convert/converter.h"
#include "convert/library.h"
#include "convert/merge.h"
#include "core/pyre.h"
#include "core/pystr.h"

namespace t4ff
{
namespace fs = std::filesystem;
using pyre::Regex;

namespace
{
constexpr const char *PAUSE_MENU = "pausedmenu";
constexpr const char *CONSOLE_OPTIONS_MENU = "ingameoptions";
constexpr uint32_t WINDOW_DECORATION = 0x00100000;

const Regex &open_re()
{
    static const Regex re(R"re("open"\s+"([^"]+)")re", pyre::I);
    return re;
}

uint32_t offset_of(const Platform &p, const std::string &rec, const std::string &field)
{
    const Field *f = p.record(rec).field(field);
    if (!f)
        throw std::runtime_error("no field " + rec + "." + field);
    return f->offset;
}

std::string node_text(const Node *n)
{
    return std::string(n->data.begin(), n->data.end());
}

// the text before the first NUL
std::string first_text(const Node *n)
{
    std::string s = node_text(n);
    size_t z = s.find('\0');
    return z == std::string::npos ? s : s.substr(0, z);
}

// the text without its trailing NULs
std::string stripped_text(const Node *n)
{
    std::string s = node_text(n);
    return std::string(py::rstrip_chars(s, std::string_view("\0", 1)));
}

std::string lstrip_commas(const std::string &s)
{
    size_t start = s.find_first_not_of(',');
    return start == std::string::npos ? "" : s.substr(start);
}

void set_text(Node *s, const std::string &text_with_nul)
{
    s->data.assign(std::vector<uint8_t>(text_with_nul.begin(), text_with_nul.end()));
    s->count = static_cast<uint32_t>(s->data.size());
    s->segments = {Segment{s->type, s->count, s->count, false}};
}

// the string a pointer of node at off loads (not a null pointer), nullopt when none
std::optional<std::string> text_of(Node *node, uint32_t off)
{
    Ptr *ptr = node->relocs.get(off);
    Node *target = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
    if (!target || !target->string)
        return std::nullopt;
    return stripped_text(target);
}

Node *menu_name_string(Node *menu)
{
    Ptr *ptr = menu->relocs.get(0); // menuDef_t.window.name
    Node *string = ptr ? ptr->target() : nullptr;
    return string && string->string ? string : nullptr;
}

bool is_type(const Node *n, std::string_view name)
{
    return n->type && n->type->name == name;
}

bool loads(const Ptr *ptr)
{
    return ptr && (ptr->kind == Ptr::Kind::Follow || ptr->kind == Ptr::Kind::Insert);
}

Ptr *new_ptr(Zone &zone, Ptr::Kind kind, Node *owner, uint32_t offset, Node *node = nullptr, uint32_t index = 0, uint32_t inner = 0,
             Ptr *slot = nullptr)
{
    Ptr *ptr = zone.new_ptr(kind);
    ptr->owner = owner;
    ptr->offset = offset;
    ptr->node = node;
    ptr->index = index;
    ptr->inner = inner;
    ptr->slot = slot;
    return ptr;
}

// a string like template's (its type and block)
Node *string_like(Zone &zone, const std::string &text, const Node *tmpl)
{
    Node *node = zone.new_node();
    node->type = tmpl->type;
    node->block = tmpl->block;
    node->string = true;
    std::vector<uint8_t> data(text.begin(), text.end());
    data.push_back(0);
    node->count = static_cast<uint32_t>(data.size());
    node->segments.push_back({node->type, node->count, node->count, false});
    node->align = 1;
    node->data.assign(std::move(data));
    return node;
}

void put_be32(Node *node, uint32_t off, uint32_t v)
{
    uint8_t *p = node->data.mutable_data() + off;
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

uint32_t be32(const Node *node, uint32_t off)
{
    const uint8_t *p = node->data.data() + off;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

float bef32(const Node *node, uint32_t off)
{
    uint32_t u = be32(node, off);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

void put_bef32(Node *node, uint32_t off, float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    put_be32(node, off, u);
}

// children: the nodes the follow / insert pointers load, in the order of the pointers
void rebuild_children(Node *node)
{
    node->children.clear();
    for (const auto &[off, ptr] : node->relocs.list())
        if (loads(ptr) && ptr->node)
            node->children.push_back(ptr->node);
}

// children in the order of their fields
void sorted_children(Node *node)
{
    std::vector<std::pair<uint32_t, Ptr *>> items(node->relocs.list().begin(), node->relocs.list().end());
    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    node->children.clear();
    for (const auto &[off, ptr] : items)
        if (loads(ptr) && ptr->node)
            node->children.push_back(ptr->node);
}

void set_string_sorted(Zone &zone, Node *owner, uint32_t off, const std::string &text, const Node *tmpl)
{
    Node *string = string_like(zone, text, tmpl);
    Ptr *ptr = new_ptr(zone, Ptr::Kind::Follow, owner, off, string);
    owner->relocs.set(off, ptr);
    string->ptr = ptr;
    put_be32(owner, off, FOLLOW);
    sorted_children(owner);
}

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

std::string join(const std::vector<std::string> &parts, std::string_view sep = ", ")
{
    return py::join(parts, sep);
}

void copy_extra(Node *to, const Node *from)
{
    to->align = from->align;
    to->origin = from->origin;
    to->origin_record = from->origin_record;
    to->origin_field = from->origin_field;
}

// keep the zone's asset list but the assets at drop
void drop_assets(Zone &zone, const std::set<size_t> &drop)
{
    std::vector<AssetListEntry> keep;
    for (size_t i = 0; i < zone.assets.size(); ++i)
    {
        if (drop.count(i))
            continue;
        Ptr *ptr = zone.assets_node->relocs.get(static_cast<uint32_t>(8 * i + 4));
        keep.push_back(AssetListEntry{i, ptr, loads(ptr) ? ptr->node : nullptr});
    }
    set_asset_list(zone, keep);
}
} // namespace

// -- merge.py's menu passes

std::set<std::string> scripted_menu_names(const Platform &p, Zone &zone)
{
    static const Regex re(R"re((?i)(?:openmenu|precachemenu|closemenu)\s*\(\s*"([^"]+)")re");
    std::set<std::string> names;
    for (auto &[name, raw] : rawfiles(p, zone))
        if (is_script_name(name))
            re.for_each(rawfile_text(raw), [&](const pyre::Match &m) { names.insert(py::lower(m.group(1))); });
    return names;
}

std::vector<std::string> menu_names(Node *node)
{
    std::vector<std::string> names;
    std::unordered_set<const Node *> seen;
    node->walk([&](Node *n) {
        std::vector<Node *> menus{n};
        for (const auto &[off, ptr] : n->relocs.list())
            menus.push_back(ptr->target());
        for (Node *menu : menus)
        {
            if (!menu || !is_type(menu, "menuDef_t") || !seen.insert(menu).second)
                continue;
            if (Node *string = menu_name_string(menu))
                names.push_back(lstrip_commas(first_text(string)));
        }
    });
    return names;
}

std::pair<std::unordered_set<const Node *>, std::vector<std::string>> bind_pause_menu(const Platform &p, Zone &zone,
                                                                                      const std::vector<std::string> &ingame_menus, const Log &log)
{
    std::set<std::string> ingame;
    for (const std::string &n : ingame_menus)
        ingame.insert(py::lower(n));
    struct Found
    {
        Node *menu;
        Ptr *ptr;
        size_t list;
    };
    std::map<std::string, std::vector<Found>> menus;
    for (size_t i = 0; i < zone.assets.size(); ++i)
    {
        const ZoneAsset &asset = zone.assets[i];
        if (asset.type != "menulist" || !asset.node())
            continue;
        asset.node()->walk([&](Node *n) {
            for (const auto &[off, ptr] : n->relocs.list())
            {
                Node *menu = loads(ptr) ? ptr->node : nullptr;
                if (menu && is_type(menu, "menuDef_t"))
                    if (Node *string = menu_name_string(menu))
                        menus[py::lower(first_text(string))].push_back(Found{menu, ptr, i});
            }
        });
    }
    if (!menus.count(PAUSE_MENU) || ingame.empty())
        return {};
    // the mod's in-game list's pause menu, else the first one
    Node *pause = menus[PAUSE_MENU][0].menu;
    for (const Found &f : menus[PAUSE_MENU])
        if (py::contains(py::lower(zone.assets[f.list].name), "ingame"))
        {
            pause = f.menu;
            break;
        }
    auto opened = [&](Node *menu) {
        std::vector<std::string> out;
        menu->walk([&](Node *s) {
            if (s->string)
                open_re().for_each(node_text(s), [&](const pyre::Match &m) { out.push_back(py::lower(m.group(1))); });
        });
        return out;
    };
    std::vector<std::string> extras, todo;
    for (const std::string &m : opened(pause))
        if (!ingame.count(m) && !py::contains(m, "options"))
            todo.push_back(m);
    while (!todo.empty())
    {
        std::string name = todo.front();
        todo.erase(todo.begin());
        if (std::find(extras.begin(), extras.end(), name) != extras.end() || ingame.count(name) || name == PAUSE_MENU)
            continue;
        if (!menus.count(name))
        {
            if (log)
                log("menus: the mod's pause menu opens '" + name + "', which the map has not: the console's pause menu stays");
            return {};
        }
        extras.push_back(name);
        for (const std::string &m : opened(menus[name][0].menu))
            todo.push_back(m);
    }
    if (extras.empty())
        return {};
    std::set<std::string> scripted = scripted_menu_names(p, zone);
    size_t last_owner = 0;
    for (const std::string &name : extras)
        last_owner = std::max(last_owner, menus[name][0].list);
    std::optional<size_t> host_index;
    for (size_t i = 0; i < zone.assets.size() && !host_index; ++i)
    {
        const ZoneAsset &a = zone.assets[i];
        std::string lower = py::lower(a.name);
        if (a.type != "menulist" || !a.node() || i <= last_owner || !py::starts_with(lower, "ui/scriptmenus/"))
            continue;
        std::string rest = lower.substr(15);
        size_t dot = rest.rfind('.');
        if (scripted.count(dot == std::string::npos ? rest : rest.substr(0, dot)))
            host_index = i;
    }
    if (!host_index)
    {
        if (log)
            log("menus: the mod's pause menu opens " + join(extras) +
                ", but no menu list the scripts precache comes after them: the console's pause menu stays");
        return {};
    }
    ZoneAsset &host = zone.assets[*host_index];

    // its options menus: the console's
    pause->walk([&](Node *s) {
        if (!s->string)
            return;
        std::string text = node_text(s);
        std::string now = open_re().sub(text, [&](const pyre::Match &m) {
            std::string target = py::lower(m.group(1));
            if (py::contains(target, "options") && !ingame.count(target))
                return std::string("\"open\" \"") + CONSOLE_OPTIONS_MENU + "\"";
            return m.str();
        });
        if (now != text)
            set_text(s, now);
    });
    // the menus it opens: in the precached list too (pointing to the copies the zone loads)
    Ptr *array_ptr = host.node()->relocs.get(offset_of(p, "MenuList", "menus"));
    Node *array = array_ptr ? array_ptr->node : nullptr;
    if (!array)
        return {};
    for (const std::string &name : extras)
    {
        uint32_t offset = static_cast<uint32_t>(array->data.size());
        std::vector<uint8_t> &data = array->data.owned();
        data.insert(data.end(), 4, 0);
        array->count += 1;
        array->relocs.set(offset, new_ptr(zone, Ptr::Kind::Alias, array, offset, nullptr, 0, 0, menus[name][0].ptr));
        // PC hints name the keyboard: the controller's button closes them
        menus[name][0].menu->walk([&](Node *s) {
            if (s->string && py::contains(node_text(s), "Press ESC"))
                set_text(s, py::replace(node_text(s), "Press ESC", "Press B"));
        });
    }
    array->segments = {Segment{array->segments[0].type, array->count, static_cast<uint32_t>(array->data.size()), false}};
    p.put_u32(host.node()->data.mutable_data() + offset_of(p, "MenuList", "menuCount"), array->count);
    if (log)
        log("menus: the mod's pause menu stays the map's own (its " + join(extras) + "), its options are the console's; " + join(extras) +
            " load with " + host.name + ", which the scripts precache");
    std::unordered_set<const Node *> kept{pause};
    for (const std::string &name : extras)
        kept.insert(menus[name][0].menu);
    return {kept, extras};
}

std::vector<std::string> drop_frontend_menus(const Platform &p, Zone &zone, const std::function<bool(const std::string &)> &is_stock_menu,
                                             const Log &log, const std::function<bool(const std::string &)> &is_stock_list,
                                             const std::unordered_set<const Node *> &keep_menus)
{
    Node *node = zone.assets_node;
    if (!node)
        return {};
    std::set<std::string> scripted = scripted_menu_names(p, zone);
    std::map<size_t, std::unordered_set<const Node *>> candidates;
    std::map<size_t, Node *> game_lists; // the mod's copies of the game's own lists
    for (size_t i = 0; i < zone.assets.size(); ++i)
    {
        const ZoneAsset &asset = zone.assets[i];
        Ptr *ptr = node->relocs.get(static_cast<uint32_t>(8 * i + 4));
        Node *target = loads(ptr) ? ptr->node : nullptr;
        if (asset.type != "menulist" || !target)
            continue;
        std::vector<std::string> menus;
        target->walk([&](Node *n) {
            if (is_type(n, "menuDef_t"))
                menus.push_back(py::lower(lstrip_commas(asset_display_name(p, *n))));
        });
        size_t stock = 0;
        for (const std::string &m : menus)
            stock += is_stock_menu(m) ? 1 : 0;
        std::string list_name = lstrip_commas(asset.name);
        bool game_list = is_stock_list && is_stock_list(list_name);
        // a menu file the scripts load by name stays
        std::string base = py::lower(list_name);
        base = base.substr(base.rfind('/') == std::string::npos ? 0 : base.rfind('/') + 1);
        if (base.rfind('.') != std::string::npos)
            base = base.substr(0, base.rfind('.'));
        bool scripted_menu = scripted.count(base) != 0;
        for (const std::string &m : menus)
            scripted_menu = scripted_menu || scripted.count(m);
        if (scripted_menu)
            continue;
        if (menus.empty() || 2 * stock > menus.size() || game_list)
        {
            std::unordered_set<const Node *> members;
            target->walk([&](Node *n) { members.insert(n); });
            candidates[i] = std::move(members);
            if (game_list)
                game_lists[i] = target;
        }
    }
    if (candidates.empty())
    {
        rename_game_menus(zone, is_stock_menu, scripted, log, keep_menus);
        return {};
    }
    // a list something that stays points into stays too
    std::unordered_map<const Node *, size_t> list_of;
    for (const auto &[i, members] : candidates)
        for (const Node *m : members)
            list_of[m] = i;
    std::vector<std::pair<size_t, std::optional<size_t>>> pointers;
    zone.walk([&](Node *n) {
        for (const auto &[off, ptr] : n->relocs.list())
        {
            const Node *target = ptr->kind == Ptr::Kind::Ref ? ptr->node : ptr->kind == Ptr::Kind::Alias && ptr->slot ? ptr->slot->owner : nullptr;
            if (!target)
                continue;
            auto t = list_of.find(target);
            if (t == list_of.end())
                continue;
            auto s = list_of.find(n);
            std::optional<size_t> source = s == list_of.end() ? std::nullopt : std::optional<size_t>(s->second);
            if (source != t->second)
                pointers.emplace_back(t->second, source);
        }
    });
    for (;;)
    {
        std::set<size_t> kept;
        for (const auto &[i, source] : pointers)
            if (candidates.count(i) && (!source || !candidates.count(*source)))
                kept.insert(i);
        if (kept.empty())
            break;
        for (size_t i : kept)
            candidates.erase(i);
    }
    // a copy of a game's list something else needs stays under a name of its own
    std::vector<std::string> renamed;
    std::set<size_t> pinned;
    for (const auto &[i, list] : game_lists)
        if (!candidates.count(i))
            pinned.insert(i);
    std::unordered_map<const Node *, size_t> names;
    for (size_t i : pinned)
    {
        Ptr *np = game_lists[i]->relocs.get(0);
        if (np && np->node)
            names[np->node] = i;
    }
    std::set<size_t> shared;
    if (!names.empty())
        zone.walk([&](Node *n) {
            for (const auto &[off, ptr] : n->relocs.list())
            {
                Node *string = ptr->target();
                auto it = string ? names.find(string) : names.end();
                if (it != names.end() && !(off == 0 && n == game_lists[it->second]))
                    shared.insert(it->second);
            }
        });
    for (size_t i : pinned)
    {
        Ptr *name_ptr = game_lists[i]->relocs.get(0); // MenuList.name
        std::string old = lstrip_commas(zone.assets[i].name);
        if (!name_ptr || !name_ptr->node || !name_ptr->node->string || shared.count(i))
        {
            if (log)
                log("warning: menus: the mod's " + old + " stays and takes the place of the game's (its name is shared)");
            continue;
        }
        size_t dot = old.rfind('.');
        std::string now = dot != std::string::npos ? old.substr(0, dot) + "_mod." + old.substr(dot + 1) : old + "_mod";
        set_text(name_ptr->node, now + std::string(1, '\0'));
        zone.assets[i].name = now;
        renamed.push_back(old + " as " + now);
    }
    if (!renamed.empty() && log)
        log("menus: the mod's versions of the game's own menu lists stay under names of their own (" + join(renamed) +
            "): its menus point into them, and the console's own lists (its pause menu, its HUD) stay the game's");
    std::vector<std::string> dropped;
    for (const auto &[i, members] : candidates)
        dropped.push_back(zone.assets[i].name);
    if (!candidates.empty())
    {
        std::set<size_t> drop;
        for (const auto &[i, members] : candidates)
            drop.insert(i);
        drop_assets(zone, drop);
        if (log)
            log("menus: left out " + join(dropped) + ", the mod's versions of menus the console has (its main menu, lobbies, pause menu...)");
    }
    rename_game_menus(zone, is_stock_menu, scripted, log, keep_menus);
    return dropped;
}

std::vector<std::string> drop_unused_videos(const Platform &p, Zone &zone, const Log &log)
{
    Node *node = zone.assets_node;
    if (!node)
        return {};
    std::vector<size_t> videos;
    for (size_t i = 0; i < zone.assets.size(); ++i)
        if (zone.assets[i].type == "rawfile" && py::ends_with(py::lower(zone.assets[i].name), ".bik"))
            videos.push_back(i);
    if (videos.empty())
        return {};
    std::vector<std::string> texts;
    for (auto &[name, raw] : rawfiles(p, zone))
        if (!py::ends_with(py::lower(name), ".bik"))
            texts.push_back(py::lower_ascii(rawfile_text(raw)));
    for (size_t i = 0; i < zone.assets.size(); ++i)
    {
        Ptr *ptr = node->relocs.get(static_cast<uint32_t>(8 * i + 4));
        Node *target = loads(ptr) ? ptr->node : nullptr;
        if ((zone.assets[i].type == "menu" || zone.assets[i].type == "menulist") && target)
            target->walk([&](Node *n) {
                if (n->string)
                    texts.push_back(py::lower_ascii(node_text(n)));
            });
    }
    std::set<size_t> unused;
    for (size_t i : videos)
    {
        std::string name = py::replace(lstrip_commas(zone.assets[i].name), "\\", "/");
        name = name.substr(name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1);
        std::string stem = py::lower(name.substr(0, name.size() >= 4 ? name.size() - 4 : 0));
        bool used = false;
        for (const std::string &t : texts)
            if (py::contains(t, stem))
            {
                used = true;
                break;
            }
        if (!used)
            unused.insert(i);
    }
    if (unused.empty())
        return {};
    std::vector<std::string> dropped;
    for (size_t i : unused)
        dropped.push_back(lstrip_commas(zone.assets[i].name));
    drop_assets(zone, unused);
    if (log)
        log("videos: left out " + join(dropped) + ", which no menu or script of the map plays (the mod's main menu's)");
    return dropped;
}

std::vector<std::string> rename_game_menus(Zone &zone, const std::function<bool(const std::string &)> &is_stock_menu,
                                           const std::set<std::string> &keep_names_in, const Log &log, const std::unordered_set<const Node *> &keep_menus)
{
    std::set<std::string> keep_names;
    for (const std::string &n : keep_names_in)
        keep_names.insert(py::lower(n));
    struct Menu
    {
        Node *menu;
        Node *string;
        std::string name;
    };
    std::vector<Menu> menus;
    std::unordered_set<const Node *> seen;
    for (const ZoneAsset &asset : zone.assets)
    {
        if (asset.type != "menulist" || !asset.node())
            continue;
        asset.node()->walk([&](Node *n) {
            if (!is_type(n, "menuDef_t") || seen.count(n) || keep_menus.count(n))
                return;
            seen.insert(n);
            Ptr *ptr = n->relocs.get(0); // menuDef_t.window.name
            Node *string = ptr ? ptr->target() : nullptr;
            if (!string || !string->string)
                return;
            std::string name = first_text(string);
            if (!name.empty() && name[0] != ',' && !keep_names.count(py::lower(name)) && is_stock_menu(name))
                menus.push_back(Menu{n, string, name});
        });
    }
    if (menus.empty())
        return {};
    std::unordered_set<const Node *> owned, strings;
    for (const Menu &m : menus)
    {
        m.menu->walk([&](Node *x) { owned.insert(x); });
        strings.insert(m.string);
    }
    std::unordered_set<const Node *> shared;
    zone.walk([&](Node *n) {
        for (const auto &[off, ptr] : n->relocs.list())
        {
            Node *target = ptr->target();
            if (target && strings.count(target) && !owned.count(n))
                shared.insert(target);
        }
    });
    std::vector<std::string> renamed, kept;
    for (const Menu &m : menus)
    {
        if (shared.count(m.string))
        {
            kept.push_back(m.name);
            continue;
        }
        std::string wanted = m.name + "_mod" + std::string(1, '\0');
        if (!py::starts_with(node_text(m.string), wanted)) // menus sharing one name string
            set_text(m.string, wanted);
        renamed.push_back(m.name);
    }
    if (!renamed.empty() && log)
    {
        std::set<std::string> unique(renamed.begin(), renamed.end());
        std::vector<std::string> names(unique.begin(), unique.end());
        std::vector<std::string> first(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(8, names.size())));
        log("menus: " + std::to_string(names.size()) + " of the mod's menus have the name of one of the console's and would take its place (" +
            join(first) + (names.size() > 8 ? ", ..." : "") + "): they are named <name>_mod, the console's own stay");
    }
    if (!kept.empty() && log)
    {
        std::set<std::string> unique(kept.begin(), kept.end());
        log("warning: menus: " + join(std::vector<std::string>(unique.begin(), unique.end())) +
            " keep the console's names (their name strings are shared) and take the place of its own");
    }
    return renamed;
}

// -- PC script menus on a controller

namespace
{
constexpr int KEY_BUTTON_A = 1, KEY_BUTTON_B = 2, KEY_BUTTON_X = 3, KEY_BUTTON_Y = 4, KEY_LSHLDR = 5, KEY_RSHLDR = 6;
constexpr int KEY_DPAD_UP = 20, KEY_DPAD_DOWN = 21, KEY_DPAD_LEFT = 22, KEY_DPAD_RIGHT = 23;
constexpr int KEY_APAD_UP = 28, KEY_APAD_DOWN = 29, KEY_APAD_LEFT = 30, KEY_APAD_RIGHT = 31;

std::pair<int, const char *> gamepad_for_digit(char digit)
{
    switch (digit)
    {
    case '1':
        return {KEY_BUTTON_A, "A"};
    case '2':
        return {KEY_BUTTON_X, "X"};
    case '3':
        return {KEY_BUTTON_Y, "Y"};
    case '4':
        return {KEY_LSHLDR, "LB"};
    case '5':
        return {KEY_RSHLDR, "RB"};
    case '6':
        return {KEY_DPAD_UP, "Up"};
    case '7':
        return {KEY_DPAD_DOWN, "Down"};
    case '8':
        return {KEY_DPAD_LEFT, "Left"};
    default:
        return {KEY_DPAD_RIGHT, "Right"};
    }
}

// Change the text of label strings in place; a string another pointer refers into keeps its text.
void set_label_texts(Zone &zone, const std::vector<std::pair<Node *, std::string>> &labels, const Log &log)
{
    if (labels.empty())
        return;
    std::unordered_set<const Node *> targets, shared;
    for (const auto &[n, t] : labels)
        targets.insert(n);
    zone.walk([&](Node *node) {
        for (const auto &[off, ptr] : node->relocs.list())
            if (ptr->kind == Ptr::Kind::Ref && targets.count(ptr->node) && (ptr->index || ptr->inner))
                shared.insert(ptr->node);
    });
    for (const auto &[node, text] : labels)
    {
        if (shared.count(node))
        {
            if (log)
                log("menu label \"" + stripped_text(node) + "\" left as it is (shared with another string)");
            continue;
        }
        set_text(node, text + std::string(1, '\0'));
    }
}

std::vector<Node *> menus_of(Zone &zone)
{
    std::vector<Node *> menus;
    zone.walk([&](Node *n) {
        if (is_type(n, "menuDef_t"))
            menus.push_back(n);
    });
    return menus;
}

const Node *first_string(Node *root)
{
    const Node *found = nullptr;
    root->walk([&](Node *n) {
        if (!found && n->string)
            found = n;
    });
    return found;
}
} // namespace

std::vector<std::string> gamepad_script_menus(const Platform &p, Zone &zone, const Log &log)
{
    static const Regex digit_label(R"(^((?:\^\d)?\s*)([1-9])(?=\s*(?:\^\d)?\s*[:.)\-]))");
    static const Regex esc_label(R"(\b(?:ESC|Esc|ESCAPE|Escape)\b)");
    uint32_t on_key = offset_of(p, "menuDef_t", "onKey"), on_esc = offset_of(p, "menuDef_t", "onESC");
    uint32_t items_off = offset_of(p, "menuDef_t", "items"), count_off = offset_of(p, "menuDef_t", "itemCount");
    uint32_t name_off = offset_of(p, "menuDef_t", "window") + offset_of(p, "windowDef_t", "name");
    uint32_t text_off = offset_of(p, "itemDef_s", "text");
    uint32_t key_off = offset_of(p, "ItemKeyHandler", "key"), action_off = offset_of(p, "ItemKeyHandler", "action"),
             next_off = offset_of(p, "ItemKeyHandler", "next");

    auto append_handler = [&](std::vector<Node *> &chain, int key, const std::string &action, const Node *string_template) {
        Node *tmpl = chain.back();
        Node *handler = zone.new_node();
        handler->type = tmpl->type;
        handler->count = 1;
        handler->block = tmpl->block;
        handler->data.assign(std::vector<uint8_t>(tmpl->data.size()));
        put_be32(handler, key_off, static_cast<uint32_t>(key));
        put_be32(handler, action_off, FOLLOW);
        handler->segments = tmpl->segments;
        copy_extra(handler, tmpl);
        Node *text = string_like(zone, action, string_template);
        Ptr *ap = new_ptr(zone, Ptr::Kind::Follow, handler, action_off, text);
        handler->relocs.set(action_off, ap);
        text->ptr = ap;
        handler->relocs.set(next_off, new_ptr(zone, Ptr::Kind::Null, handler, next_off));
        rebuild_children(handler);
        Ptr *np = new_ptr(zone, Ptr::Kind::Follow, tmpl, next_off, handler);
        tmpl->relocs.set(next_off, np);
        handler->ptr = np;
        put_be32(tmpl, next_off, FOLLOW);
        rebuild_children(tmpl);
        chain.push_back(handler);
    };

    std::vector<std::string> changed;
    std::vector<std::pair<Node *, std::string>> labels;
    for (const ZoneAsset &asset : zone.assets)
    {
        if (asset.type != "menulist" || !asset.ptr)
            continue;
        Node *root = asset.ptr->target();
        if (!root)
            continue;
        std::vector<Node *> menus;
        root->walk([&](Node *n) {
            if (is_type(n, "menuDef_t"))
                menus.push_back(n);
        });
        for (Node *menu : menus)
        {
            std::vector<Node *> chain;
            Ptr *ptr = menu->relocs.get(on_key);
            while (ptr && ptr->kind != Ptr::Kind::Null && ptr->target())
            {
                chain.push_back(ptr->target());
                ptr = chain.back()->relocs.get(next_off);
            }
            std::map<int, Node *> keys;
            for (Node *h : chain)
                keys[static_cast<int32_t>(be32(h, key_off))] = h;
            std::map<char, Node *> digits;
            for (const auto &[k, h] : keys)
                if (k >= '1' && k <= '9')
                    digits[static_cast<char>(k)] = h;
            if (digits.empty())
                continue;
            const Node *string_template = first_string(menu);
            std::map<char, std::string> names; // digit -> button now doing its action
            for (const auto &[digit, handler] : digits)
            {
                auto [key, button] = gamepad_for_digit(digit);
                std::optional<std::string> action = text_of(handler, action_off);
                if (!keys.count(key) && action)
                {
                    append_handler(chain, key, *action, string_template);
                    names[digit] = button;
                }
            }
            std::optional<std::string> escape = text_of(menu, on_esc);
            bool back = escape && !keys.count(KEY_BUTTON_B);
            if (back)
                append_handler(chain, KEY_BUTTON_B, *escape, string_template);
            else if (names.empty())
                continue;
            Ptr *items_ptr = menu->relocs.get(items_off);
            Node *array = items_ptr && items_ptr->kind != Ptr::Kind::Null ? items_ptr->target() : nullptr;
            int32_t count = static_cast<int32_t>(be32(menu, count_off));
            for (int32_t i = 0; i < (array ? count : 0); ++i)
            {
                Ptr *item_ptr = array->relocs.get(static_cast<uint32_t>(4 * i));
                Node *item = item_ptr && item_ptr->kind != Ptr::Kind::Null ? item_ptr->target() : nullptr;
                Ptr *text_ptr = item ? item->relocs.get(text_off) : nullptr;
                std::optional<std::string> text = item ? text_of(item, text_off) : std::nullopt;
                if (!text || text_ptr->kind != Ptr::Kind::Follow)
                    continue;
                std::string now = digit_label.sub(*text, [&](const pyre::Match &m) {
                    char d = m.group(2)[0];
                    return names.count(d) ? m.str(1) + names[d] : m.str();
                });
                if (back)
                    now = esc_label.sub(now, std::string_view("B"));
                if (now != *text)
                    labels.emplace_back(text_ptr->node, now);
            }
            std::optional<std::string> menu_name = text_of(menu, name_off);
            std::string name = menu_name && !menu_name->empty() ? *menu_name : asset.name;
            std::vector<std::string> buttons;
            for (const auto &[d, b] : names)
                buttons.push_back(b + " (" + std::string(1, d) + ")");
            if (back)
                buttons.push_back("B (Escape)");
            if (log)
                log("menu " + name + ": controller buttons for its keys: " + join(buttons));
            changed.push_back(name);
        }
    }
    set_label_texts(zone, labels, log);
    return changed;
}

std::map<std::string, int> decorate_inert_items(const Platform &p, Zone &zone, const Log &log, const std::set<std::string> *script_menus)
{
    static const char *const HANDLERS[] = {"mouseEnterText", "mouseExitText", "mouseEnter", "mouseExit", "action", "onAccept", "onFocus",
                                           "leaveFocus",     "dvar",          "dvarTest",   "onListboxSelectionChange", "onKey", "enableDvar"};
    uint32_t name_off = offset_of(p, "menuDef_t", "window") + offset_of(p, "windowDef_t", "name");
    uint32_t items_off = offset_of(p, "menuDef_t", "items"), count_off = offset_of(p, "menuDef_t", "itemCount");
    uint32_t flags_off = offset_of(p, "itemDef_s", "window") + offset_of(p, "windowDef_t", "staticFlags");
    uint32_t type_off = offset_of(p, "itemDef_s", "type");
    std::vector<uint32_t> handlers;
    for (const char *f : HANDLERS)
        if (const Field *field = p.record("itemDef_s").field(f))
            handlers.push_back(field->offset);
    std::map<std::string, int> changed;
    for (Node *menu : menus_of(zone))
    {
        Ptr *ptr = menu->relocs.get(items_off);
        Node *array = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
        uint32_t count = array ? p.u32(menu->data.data() + count_off) : 0;
        int focusable = 0;
        std::vector<std::pair<Node *, uint32_t>> inert;
        for (uint32_t i = 0; i < count; ++i)
        {
            Ptr *item_ptr = array->relocs.get(4 * i);
            Node *item = item_ptr && item_ptr->kind != Ptr::Kind::Null ? item_ptr->target() : nullptr;
            if (!item || item->data.size() < flags_off + 4)
                continue;
            uint32_t flags = p.u32(item->data.data() + flags_off);
            if (flags & WINDOW_DECORATION)
                continue;
            bool handled = false;
            for (uint32_t off : handlers)
            {
                Ptr *h = item->relocs.get(off);
                if (h && h->kind != Ptr::Kind::Null)
                    handled = true;
            }
            if (handled)
                ++focusable;
            else
            {
                uint32_t type = p.u32(item->data.data() + type_off);
                if (type == 0 || type == 1) // ITEM_TYPE_TEXT, ITEM_TYPE_BUTTON
                    inert.emplace_back(item, flags);
            }
        }
        if (inert.empty() || !focusable)
            continue; // nothing to reach, nothing in the way
        std::optional<std::string> name = text_of(menu, name_off);
        if (script_menus && !script_menus->count(py::lower(name.value_or(""))))
            continue;
        for (auto &[item, flags] : inert)
            p.put_u32(item->data.mutable_data() + flags_off, flags | WINDOW_DECORATION);
        changed[name && !name->empty() ? *name : "?"] = static_cast<int>(inert.size());
    }
    if (!changed.empty() && log)
    {
        std::vector<std::string> parts;
        for (const auto &[n, c] : changed)
            parts.push_back(n + " " + std::to_string(c));
        log("menus: items that do nothing are decorations, which the controller's focus skips (" + join(parts) + ")");
    }
    return changed;
}

namespace
{
constexpr size_t MAX_FOCUS_CHAIN = 24;
constexpr double SAFE_MARGIN_X = 32.0, SAFE_MARGIN_Y = 24.0;

struct Rect
{
    double x, y, w, h;
};

Rect screen_rect(const Node *item, uint32_t rect_off)
{
    Rect r{bef32(item, rect_off), bef32(item, rect_off + 4), bef32(item, rect_off + 8), bef32(item, rect_off + 12)};
    int32_t horz = static_cast<int32_t>(be32(item, rect_off + 16)), vert = static_cast<int32_t>(be32(item, rect_off + 20));
    r.x += horz == 1 ? SAFE_MARGIN_X : horz == 2 ? 320.0 : horz == 3 ? 640.0 - SAFE_MARGIN_X : 0.0;
    r.y += vert == 1 ? SAFE_MARGIN_Y : vert == 2 ? 240.0 : vert == 3 ? 480.0 - SAFE_MARGIN_Y : 0.0;
    return r;
}

bool on_screen(const Rect &r)
{
    return r.x >= -2.0 && r.y >= -2.0 && r.x + r.w <= 642.0 && r.y + r.h <= 482.0;
}

using Point = std::pair<double, double>;

// the items that way from i, nearest first
std::vector<size_t> ordered(const std::vector<Point> &centers, size_t i, Point d)
{
    std::vector<std::pair<double, size_t>> scored;
    for (size_t j = 0; j < centers.size(); ++j)
    {
        double vx = centers[j].first - centers[i].first, vy = centers[j].second - centers[i].second;
        double along = vx * d.first + vy * d.second;
        if (j == i || along <= 0.5)
            continue;
        double across = std::abs(vx * d.second - vy * d.first);
        scored.emplace_back(along + 2 * across + (across <= along ? 0 : 100000), j);
    }
    std::sort(scored.begin(), scored.end());
    std::vector<size_t> out;
    for (const auto &[s, j] : scored)
        out.push_back(j);
    return out;
}

// the items the other way from i, farthest first
std::vector<size_t> wrapped(const std::vector<Point> &centers, size_t i, Point d)
{
    std::vector<std::tuple<int, double, double, size_t>> scored;
    for (size_t j = 0; j < centers.size(); ++j)
    {
        double vx = centers[j].first - centers[i].first, vy = centers[j].second - centers[i].second;
        double back = -(vx * d.first + vy * d.second);
        if (j == i || back <= 0.5)
            continue;
        double across = std::abs(vx * d.second - vy * d.first);
        scored.emplace_back(across <= 25.0 ? 0 : 1, -back, across, j);
    }
    std::sort(scored.begin(), scored.end());
    std::vector<size_t> out;
    for (const auto &s : scored)
        out.push_back(std::get<3>(s));
    return out;
}

std::string focus_chain(const std::vector<std::string> &names)
{
    std::string out;
    size_t n = std::min(names.size(), MAX_FOCUS_CHAIN);
    for (size_t k = n; k-- > 0;)
        out += "\"setfocus\" \"" + names[k] + "\" ; ";
    return out;
}

std::string color_text(const std::array<double, 4> &rgba)
{
    std::vector<std::string> parts;
    for (double c : rgba)
        parts.push_back(format_g(c));
    return join(parts, " ");
}

// Python's round(x, 1)
double round1(double v)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%.1f", v);
    return strtod(buf, nullptr);
}

using Key = std::pair<const Node *, uint32_t>;

// a dict keyed by (owner, offset), in the order the keys were first set
class Scripts
{
  public:
    void set(Key k, std::string v)
    {
        auto it = index.find(k);
        if (it != index.end())
        {
            items[it->second].second = std::move(v);
            return;
        }
        index[k] = items.size();
        items.emplace_back(k, std::move(v));
    }
    bool contains(Key k) const
    {
        auto it = index.find(k);
        return it != index.end() && alive[it->second];
    }
    void pop(Key k)
    {
        auto it = index.find(k);
        if (it != index.end())
        {
            alive[it->second] = false;
            index.erase(it);
        }
    }
    std::vector<Key> keys() const
    {
        std::vector<Key> out;
        for (size_t i = 0; i < items.size(); ++i)
            if (alive.size() <= i || alive[i])
                out.push_back(items[i].first);
        return out;
    }
    void finish()
    {
        alive.resize(items.size(), true);
    }
    std::vector<std::pair<Key, std::string>> items;
    std::vector<bool> alive;
    std::map<Key, size_t> index;
};
} // namespace

std::map<std::string, int> controller_navigation(const Platform &p, Zone &zone, const Log &log, const std::set<std::string> *script_menus)
{
    static const Regex numbers(R"(\d+)");
    static const std::pair<Point, std::array<int, 2>> DIRECTIONS[] = {
        {{0.0, -1.0}, {KEY_DPAD_UP, KEY_APAD_UP}},
        {{0.0, 1.0}, {KEY_DPAD_DOWN, KEY_APAD_DOWN}},
        {{-1.0, 0.0}, {KEY_DPAD_LEFT, KEY_APAD_LEFT}},
        {{1.0, 0.0}, {KEY_DPAD_RIGHT, KEY_APAD_RIGHT}},
    };
    const std::array<double, 3> FALLBACK{1.0, 0.85, 0.3};
    uint32_t menu_name = offset_of(p, "menuDef_t", "window") + offset_of(p, "windowDef_t", "name");
    uint32_t items_off = offset_of(p, "menuDef_t", "items"), count_off = offset_of(p, "menuDef_t", "itemCount");
    uint32_t window = offset_of(p, "itemDef_s", "window");
    uint32_t name_off = window + offset_of(p, "windowDef_t", "name"), flags_off = window + offset_of(p, "windowDef_t", "staticFlags");
    uint32_t rect_off = window + offset_of(p, "windowDef_t", "rect");
    uint32_t action_off = offset_of(p, "itemDef_s", "action"), focus_off = offset_of(p, "itemDef_s", "onFocus");
    uint32_t leave_off = offset_of(p, "itemDef_s", "leaveFocus"), on_key = offset_of(p, "itemDef_s", "onKey");
    const Record &krec = p.record("ItemKeyHandler");
    uint32_t key_off = offset_of(p, "ItemKeyHandler", "key"), key_action = offset_of(p, "ItemKeyHandler", "action"),
             key_next = offset_of(p, "ItemKeyHandler", "next");
    uint32_t open_off = offset_of(p, "menuDef_t", "onOpen"), focus_color_off = offset_of(p, "menuDef_t", "focusColor");
    uint32_t fore_off = window + offset_of(p, "windowDef_t", "foreColor");
    // strings other pointers refer into stay where they are
    std::unordered_set<const Node *> shared;
    std::unordered_map<const Node *, std::vector<Key>> pointers; // string -> (owner, offset) of every pointer to it
    zone.walk([&](Node *node) {
        for (const auto &[off, ptr] : node->relocs.list())
        {
            if ((ptr->kind == Ptr::Kind::Ref || ptr->kind == Ptr::Kind::Alias) && ptr->node)
                shared.insert(ptr->node);
            if (ptr->node && ptr->node->string)
                pointers[ptr->node].emplace_back(node, off);
        }
    });
    std::map<std::string, int> changed;
    for (Node *menu : menus_of(zone))
    {
        if (script_menus && !script_menus->count(py::lower(text_of(menu, menu_name).value_or(""))))
            continue;
        Ptr *ptr = menu->relocs.get(items_off);
        Node *array = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
        uint32_t count = array ? p.u32(menu->data.data() + count_off) : 0;
        std::vector<Node *> buttons;
        for (uint32_t i = 0; i < count; ++i)
        {
            Ptr *item_ptr = array->relocs.get(4 * i);
            Node *item = item_ptr && item_ptr->kind != Ptr::Kind::Null ? item_ptr->target() : nullptr;
            if (!item || item->data.size() < on_key + 4 || (p.u32(item->data.data() + flags_off) & WINDOW_DECORATION))
                continue;
            if (text_of(item, action_off))
                buttons.push_back(item);
        }
        std::vector<std::string> names;
        std::vector<std::string> named;
        for (Node *item : buttons)
        {
            names.push_back(text_of(item, name_off).value_or(""));
            if (!names.back().empty())
                named.push_back(names.back());
        }
        std::set<std::string> taken(named.begin(), named.end());
        if (buttons.size() < 2 || taken.size() != named.size())
            continue; // nothing to move between, or names "setfocus" cannot tell apart
        const Node *tmpl = first_string(menu);
        for (size_t i = 0; i < buttons.size(); ++i)
        {
            if (!names[i].empty())
                continue;
            size_t n = i;
            while (taken.count("t4ff_focus_" + std::to_string(n)))
                ++n;
            names[i] = "t4ff_focus_" + std::to_string(n);
            taken.insert(names[i]);
            set_string_sorted(zone, buttons[i], name_off, names[i], tmpl);
        }
        std::vector<Point> centers;
        std::vector<bool> shown;
        for (Node *item : buttons)
        {
            Rect r = screen_rect(item, rect_off);
            centers.emplace_back(r.x + r.w / 2, r.y + r.h / 2);
            shown.push_back(on_screen(r));
        }
        if (std::none_of(shown.begin(), shown.end(), [](bool s) { return s; }))
            shown.assign(buttons.size(), true); // a layout of its own: all of it
        for (size_t i = 0; i < buttons.size(); ++i)
        {
            Node *item = buttons[i];
            std::vector<Node *> chain;
            Ptr *kp = item->relocs.get(on_key);
            while (kp && kp->kind != Ptr::Kind::Null && kp->target())
            {
                chain.push_back(kp->target());
                kp = chain.back()->relocs.get(key_next);
            }
            std::set<int32_t> bound;
            for (Node *h : chain)
                bound.insert(static_cast<int32_t>(be32(h, key_off)));
            for (const auto &[direction, keys] : DIRECTIONS)
            {
                std::vector<std::string> targets;
                for (size_t j : ordered(centers, i, direction))
                    if (shown[j])
                        targets.push_back(names[j]);
                for (size_t j : wrapped(centers, i, direction))
                    if (shown[j])
                        targets.push_back(names[j]);
                std::string script = focus_chain(targets.empty() ? std::vector<std::string>{names[i]} : targets);
                for (int key : keys)
                {
                    if (bound.count(key))
                        continue;
                    Node *handler = zone.new_node();
                    handler->type = krec.self;
                    handler->count = 1;
                    handler->block = item->block;
                    handler->data.assign(std::vector<uint8_t>(krec.size));
                    handler->align = 4;
                    put_be32(handler, key_off, static_cast<uint32_t>(key));
                    set_string_sorted(zone, handler, key_action, script, tmpl);
                    handler->relocs.set(key_next, new_ptr(zone, Ptr::Kind::Null, handler, key_next));
                    Node *owner = chain.empty() ? item : chain.back();
                    uint32_t off = chain.empty() ? on_key : key_next;
                    Ptr *fp = new_ptr(zone, Ptr::Kind::Follow, owner, off, handler);
                    owner->relocs.set(off, fp);
                    handler->ptr = fp;
                    put_be32(owner, off, FOLLOW);
                    sorted_children(owner);
                    chain.push_back(handler);
                }
            }
        }
        // choices confirmed by a button: the choice follows the focus, A confirms it too
        std::vector<std::string> actions;
        for (Node *item : buttons)
            actions.push_back(text_of(item, action_off).value_or(""));
        std::vector<std::pair<std::string, std::vector<size_t>>> groups;
        for (size_t i = 0; i < actions.size(); ++i)
        {
            std::string lower = py::lower(actions[i]);
            if (py::contains(lower, "scriptmenuresponse") && py::contains(lower, "setlocalvar"))
            {
                std::string key = numbers.sub(actions[i], std::string_view("#"));
                auto it = std::find_if(groups.begin(), groups.end(), [&](const auto &g) { return g.first == key; });
                if (it == groups.end())
                    groups.emplace_back(key, std::vector<size_t>{i});
                else
                    it->second.push_back(i);
            }
        }
        std::vector<size_t> choices;
        for (const auto &[k, members] : groups)
            if (members.size() > choices.size())
                choices = members;
        auto is_choice = [&](size_t i) { return std::find(choices.begin(), choices.end(), i) != choices.end(); };
        std::vector<size_t> confirms;
        for (size_t i = 0; i < actions.size(); ++i)
            if (!is_choice(i) && py::contains(py::lower(actions[i]), "scriptmenuresponse"))
                confirms.push_back(i);
        for (size_t i : choices)
        {
            Ptr *ap = buttons[i]->relocs.get(action_off);
            if (shared.count(ap->node) || text_of(buttons[i], focus_off))
            {
                choices.clear(); // a shared string, or choices that already do something on focus
                break;
            }
        }
        if (choices.size() >= 2 && confirms.size() == 1)
        {
            for (size_t i : choices)
            {
                set_string_sorted(zone, buttons[i], focus_off, actions[i], tmpl);
                set_string_sorted(zone, buttons[i], action_off, actions[confirms[0]], tmpl);
            }
        }
        else
            choices.clear();

        // the scripts to add to
        Scripts scripts;
        std::map<Key, Key> partner;
        std::map<std::array<double, 4>, std::vector<size_t>> stacked;
        auto rounded = [&](const Node *item) {
            Rect r = screen_rect(item, rect_off);
            return std::array<double, 4>{round1(r.x), round1(r.y), round1(r.w), round1(r.h)};
        };
        for (size_t i = 0; i < buttons.size(); ++i)
            stacked[rounded(buttons[i])].push_back(i);
        std::array<double, 3> focus_rgb{bef32(menu, focus_color_off), bef32(menu, focus_color_off + 4), bef32(menu, focus_color_off + 8)};
        for (size_t i = 0; i < buttons.size(); ++i)
        {
            Node *item = buttons[i];
            std::array<double, 4> fore{bef32(item, fore_off), bef32(item, fore_off + 4), bef32(item, fore_off + 8), bef32(item, fore_off + 12)};
            double diff = 0;
            for (int k = 0; k < 3; ++k)
                diff = std::max(diff, std::abs(focus_rgb[k] - fore[k]));
            const std::array<double, 3> &rgb = diff > 0.15 ? focus_rgb : FALLBACK;
            Key on{item, focus_off}, off{item, leave_off};
            scripts.set(on, "\"setitemcolor\" \"" + names[i] + "\" \"forecolor\" " + color_text({rgb[0], rgb[1], rgb[2], fore[3]}) + " ; ");
            scripts.set(off, "\"setitemcolor\" \"" + names[i] + "\" \"forecolor\" " + color_text(fore) + " ; ");
            partner[on] = off;
            partner[off] = on;
            // a button shown in the place of others: after its action the focus goes to the one shown
            std::vector<std::string> others;
            for (size_t j : stacked[rounded(item)])
                if (j != i)
                    others.push_back(names[j]);
            if (!others.empty() && !is_choice(i))
                scripts.set({item, action_off}, focus_chain(others));
        }
        // the menu opens on its top-most button shown (then the left-most)
        std::vector<size_t> top;
        for (size_t i = 0; i < buttons.size(); ++i)
            if (shown[i])
                top.push_back(i);
        std::stable_sort(top.begin(), top.end(), [&](size_t a, size_t b) {
            return std::make_pair(centers[a].second, centers[a].first) < std::make_pair(centers[b].second, centers[b].first);
        });
        std::vector<std::string> top_names;
        for (size_t i : top)
            top_names.push_back(names[i]);
        scripts.set({menu, open_off}, focus_chain(top_names));
        scripts.finish();
        // a string another pointer refers into is replaced only when that pointer's is too
        bool settled = false;
        while (!settled)
        {
            settled = true;
            for (const Key &key : scripts.keys())
            {
                Ptr *sp = const_cast<Node *>(key.first)->relocs.get(key.second);
                if (!sp || sp->kind != Ptr::Kind::Follow || !sp->node)
                    continue;
                auto it = pointers.find(sp->node);
                bool all = true;
                if (it != pointers.end())
                    for (const Key &other : it->second)
                        all = all && scripts.contains(other);
                if (all)
                    continue;
                scripts.pop(key);
                auto pt = partner.find(key);
                if (pt != partner.end())
                    scripts.pop(pt->second);
                settled = false;
            }
        }
        for (size_t k = 0; k < scripts.items.size(); ++k)
        {
            if (!scripts.alive[k])
                continue;
            auto &[key, script] = scripts.items[k];
            Node *owner = const_cast<Node *>(key.first);
            set_string_sorted(zone, owner, key.second, text_of(owner, key.second).value_or("") + script, tmpl);
        }
        std::optional<std::string> mname = text_of(menu, menu_name);
        changed[mname && !mname->empty() ? *mname : "?"] = static_cast<int>(buttons.size());
    }
    if (!changed.empty() && log)
    {
        std::vector<std::string> parts;
        for (const auto &[n, c] : changed)
            parts.push_back(n + " " + std::to_string(c));
        log("menus: the D-pad moves between buttons as they are laid out (" + join(parts) + ")");
    }
    return changed;
}

// -- the map's options

namespace
{
constexpr uint32_t ITEM_TYPE_TEXT = 0, ITEM_TYPE_MULTI = 12;
const char *const GAME_SETTING_PREFIXES[] = {"cg_",     "r_",     "snd_", "ui_",  "g_",   "sv_",  "cl_",  "com_", "bg_",         "player_",
                                             "in_",     "input_", "gpad_", "hud_", "con_", "loc_", "sensitivity", "m_"};

std::string format_value(double value)
{
    if (value == std::trunc(value) && std::abs(value) < 9.2e18)
        return std::to_string(static_cast<long long>(value));
    return format_g(value);
}

float pf32(const Platform &p, const Node *n, uint32_t off)
{
    uint32_t u = p.u32(n->data.data() + off);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
} // namespace

std::vector<MapOption> menu_options(const Platform &p, const std::vector<Zone *> &zones, const std::set<std::string> &read_dvars,
                                    const MenuValues &menu_values)
{
    static const Regex value_re(R"([\w.\-]{1,63})");
    uint32_t items_off = offset_of(p, "menuDef_t", "items"), count_off = offset_of(p, "menuDef_t", "itemCount");
    uint32_t rect_off = offset_of(p, "itemDef_s", "window") + offset_of(p, "windowDef_t", "rect");
    uint32_t type_off = offset_of(p, "itemDef_s", "type"), text_off = offset_of(p, "itemDef_s", "text"), dvar_off = offset_of(p, "itemDef_s", "dvar"),
             data_off = offset_of(p, "itemDef_s", "typeData");
    uint32_t names_off = offset_of(p, "multiDef_s", "dvarList"), strs_off = offset_of(p, "multiDef_s", "dvarStr"),
             values_off = offset_of(p, "multiDef_s", "dvarValue");
    uint32_t count_off_m = offset_of(p, "multiDef_s", "count"), strdef_off = offset_of(p, "multiDef_s", "strDef");
    std::map<std::string, MapOption> options;
    std::map<std::string, std::string> labels;
    for (Zone *zone : zones)
    {
        for (Node *menu : menus_of(*zone))
        {
            Ptr *ptr = menu->relocs.get(items_off);
            Node *array = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
            uint32_t count = array ? p.u32(menu->data.data() + count_off) : 0;
            std::vector<Node *> items;
            for (uint32_t i = 0; i < count; ++i)
            {
                Ptr *ip = array->relocs.get(4 * i);
                if (ip && ip->kind != Ptr::Kind::Null && ip->target())
                    items.push_back(ip->target());
            }
            std::vector<std::pair<Node *, std::optional<std::string>>> texts;
            for (Node *item : items)
                if (p.u32(item->data.data() + type_off) == ITEM_TYPE_TEXT)
                    texts.emplace_back(item, text_of(item, text_off));
            for (Node *item : items)
            {
                if (p.u32(item->data.data() + type_off) != ITEM_TYPE_MULTI)
                    continue;
                std::string dvar = py::lower(text_of(item, dvar_off).value_or(""));
                Ptr *mp = item->relocs.get(data_off);
                Node *multi = mp && mp->kind != Ptr::Kind::Null ? mp->target() : nullptr;
                bool game_setting = false;
                for (const char *prefix : GAME_SETTING_PREFIXES)
                    game_setting = game_setting || py::starts_with(dvar, prefix);
                if (!read_dvars.count(dvar) || !multi || game_setting)
                    continue;
                uint32_t n = p.u32(multi->data.data() + count_off_m);
                bool strings = p.u32(multi->data.data() + strdef_off) != 0;
                std::vector<std::pair<std::string, std::string>> choices;
                for (uint32_t k = 0; k < std::min<uint32_t>(n, 32); ++k)
                {
                    std::optional<std::string> name = text_of(multi, names_off + 4 * k);
                    std::optional<std::string> value;
                    if (strings)
                        value = text_of(multi, strs_off + 4 * k);
                    else
                        value = format_value(pf32(p, multi, values_off + 4 * k));
                    if (name && !name->empty() && value && value_re.fullmatch(*value))
                        choices.emplace_back(*value, *name);
                }
                if (!options.count(dvar) && choices.size() >= 2)
                {
                    MapOption o;
                    o.dvar = dvar;
                    o.choices = choices;
                    options[dvar] = std::move(o);
                }
                // its label: a text of the same row ending with ":"
                double y = pf32(p, item, rect_off + 4), h = pf32(p, item, rect_off + 12);
                for (const auto &[other, text] : texts)
                {
                    double oy = pf32(p, other, rect_off + 4);
                    if (text && !text->empty() && py::ends_with(py::rstrip(*text), ":") && !py::starts_with(*text, "@") &&
                        std::abs(oy - y) <= std::max(h, 1.0))
                    {
                        std::string_view r = py::rstrip(*text);
                        if (!labels.count(dvar))
                            labels[dvar] = std::string(py::strip(r.substr(0, r.size() - 1)));
                    }
                }
            }
        }
    }
    std::vector<MapOption> result;
    for (auto &[dvar, option] : options)
    {
        std::vector<std::string> set_by_menus;
        auto mv = menu_values.find(dvar);
        if (mv != menu_values.end())
            for (const std::string &v : mv->second)
                for (const auto &[value, name] : option.choices)
                    if (value == v)
                    {
                        set_by_menus.push_back(v);
                        break;
                    }
        option.default_value = set_by_menus.size() == 1 ? set_by_menus[0] : option.choices[0].first;
        auto l = labels.find(dvar);
        option.label = l != labels.end() && !l->second.empty() ? l->second : py::title(py::replace(dvar, "_", " "));
        result.push_back(option);
    }
    return result;
}

std::optional<fs::path> write_options(const std::vector<MapOption> &options, const fs::path &out_dir, const Log &log)
{
    fs::path path = out_dir / "options.txt";
    if (options.empty())
    {
        std::error_code ec;
        if (fs::exists(path))
            fs::remove(path, ec);
        return std::nullopt;
    }
    std::vector<std::string> lines;
    for (const MapOption &o : options)
    {
        lines.push_back("option " + o.dvar + " " + o.default_value + " " + o.label);
        for (const auto &[value, name] : o.choices)
            lines.push_back("choice " + value + " " + name);
    }
    std::string text = py::join(lines, "\n") + "\n";
    std::ofstream f(path, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (log)
        for (const MapOption &o : options)
        {
            std::vector<std::string> names;
            std::string default_name;
            for (const auto &[value, name] : o.choices)
            {
                names.push_back(name);
                if (default_name.empty() && value == o.default_value)
                    default_name = name;
            }
            log("options: " + o.label + " (" + o.dvar + ": " + join(names) + "; " + default_name + " by default) in the Custom Maps menu, X to change");
        }
    return path;
}

// -- the options in game: copies of the game's difficulty list

namespace
{
constexpr const char *OPTIONS_TEMPLATE = "popmenu_difficulty";
constexpr const char *OPTIONS_RESTART_MENU = "t4ff_options_restart";
constexpr double OPTIONS_ROW_STEP = 24.0;

struct MenuError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// a copy of node and of the nodes it loads; pointers to anything else keep pointing there
Node *clone(Zone &zone, Node *node, std::unordered_map<const Node *, Node *> &memo)
{
    Node *n = zone.new_node();
    n->type = node->type;
    n->count = node->count;
    n->block = node->block;
    n->data.assign(std::vector<uint8_t>(node->data.begin(), node->data.end()));
    n->push_before = node->push_before;
    n->push_after = node->push_after;
    n->string = node->string;
    n->asset = node->asset;
    n->runtime_size = node->runtime_size;
    n->segments = node->segments;
    copy_extra(n, node);
    n->delayed = node->delayed;
    memo[node] = n;
    for (const auto &[off, ptr] : node->relocs.list())
    {
        if (loads(ptr))
        {
            if (ptr->kind == Ptr::Kind::Insert)
            {
                // a nested asset: the copy uses the one the original loads
                n->relocs.set(off, new_ptr(zone, Ptr::Kind::Alias, n, off, nullptr, 1, 0, ptr));
                continue;
            }
            Node *child = clone(zone, ptr->node, memo);
            Ptr *fp = new_ptr(zone, Ptr::Kind::Follow, n, off, child);
            n->relocs.set(off, fp);
            child->ptr = fp;
        }
        else if (ptr->kind == Ptr::Kind::Ref)
        {
            auto it = memo.find(ptr->node);
            n->relocs.set(off, new_ptr(zone, Ptr::Kind::Ref, n, off, it != memo.end() ? it->second : ptr->node, ptr->index, ptr->inner));
        }
        else if (ptr->kind == Ptr::Kind::Alias)
            n->relocs.set(off, new_ptr(zone, Ptr::Kind::Alias, n, off, nullptr, ptr->index, 0, ptr->slot));
        else
            n->relocs.set(off, new_ptr(zone, Ptr::Kind::Null, n, off));
    }
    rebuild_children(n);
    return n;
}

Node *clone(Zone &zone, Node *node)
{
    std::unordered_map<const Node *, Node *> memo;
    return clone(zone, node, memo);
}

struct Token
{
    enum Kind
    {
        Op,
        Int,
        Float,
        Str
    } kind;
    int32_t i = 0;
    double f = 0;
    std::string s;
    bool operator==(const Token &o) const
    {
        return kind == o.kind && i == o.i && f == o.f && s == o.s;
    }
};

// reads and edits the items of a console menuDef_t
class MenuEditor
{
  public:
    MenuEditor(const Platform &p, Zone &zone, Node *menu) : p(p), zone(zone), menu(menu)
    {
        window = offset_of(p, "itemDef_s", "window");
        Ptr *ptr = menu->relocs.get(offset_of(p, "menuDef_t", "items"));
        if (!ptr || ptr->kind != Ptr::Kind::Follow)
            throw MenuError(std::string("menu ") + OPTIONS_TEMPLATE + " has no items");
        array = ptr->node;
        items = array->children;
        string_template = first_string(menu);
        menu->walk([&](Node *n) {
            if (!entries_template && (n->origin == Origin::Member || n->origin == Origin::PtrArray) && n->origin_record &&
                *n->origin_record == "statement_s" && n->origin_field && *n->origin_field == "entries")
                entries_template = n;
        });
        if (!string_template || !entries_template || entries_template->children.empty())
            throw MenuError("no string or expression to copy");
        entry_template = entries_template->children[0];
    }

    uint32_t offset(const std::string &field) const
    {
        if (py::starts_with(field, "window."))
            return window + offset_of(p, "windowDef_t", field.substr(7));
        return offset_of(p, "itemDef_s", field);
    }

    std::optional<std::string> string(Node *item, const std::string &field) const
    {
        return text_of(item, offset(field));
    }

    void set_string(Node *item, const std::string &field, const std::optional<std::string> &text)
    {
        set_on(item, offset(field), text);
        rebuild_children(item);
    }

    void set_menu_string(const std::string &field, const std::optional<std::string> &text)
    {
        uint32_t off = py::starts_with(field, "window.") ? offset_of(p, "menuDef_t", "window") + offset_of(p, "windowDef_t", field.substr(7))
                                                         : offset_of(p, "menuDef_t", field);
        set_on(menu, off, text);
        rebuild_children(menu);
    }

    void move(Node *item, double dy, double dx = 0.0)
    {
        for (const char *field : {"window.rect", "window.rectClient"})
        {
            uint32_t off = offset(field);
            put_bef32(item, off, static_cast<float>(bef32(item, off) + dx));
            put_bef32(item, off + 4, static_cast<float>(bef32(item, off + 4) + dy));
        }
    }

    std::vector<Token> expression(Node *item, const std::string &field) const
    {
        uint32_t off = offset(field);
        int32_t count = static_cast<int32_t>(be32(item, off));
        Ptr *ptr = item->relocs.get(off + 4);
        Node *arr = ptr && ptr->kind == Ptr::Kind::Follow ? ptr->node : nullptr;
        std::vector<Token> tokens;
        for (int32_t i = 0; i < (arr ? count : 0); ++i)
        {
            Node *entry = arr->relocs.get(static_cast<uint32_t>(4 * i))->node;
            int32_t kind = static_cast<int32_t>(be32(entry, 0)), a = static_cast<int32_t>(be32(entry, 4)), b = static_cast<int32_t>(be32(entry, 8));
            Token t{Token::Op};
            if (kind == 0)
                t.i = a;
            else if (a == 0)
            {
                t.kind = Token::Int;
                t.i = b;
            }
            else if (a == 1)
            {
                t.kind = Token::Float;
                t.f = bef32(entry, 8);
            }
            else
            {
                t.kind = Token::Str;
                t.s = stripped_text(entry->relocs.get(8)->target());
            }
            tokens.push_back(t);
        }
        return tokens;
    }

    void set_expression(Node *item, const std::string &field, const std::vector<Token> &tokens)
    {
        uint32_t off = offset(field);
        put_be32(item, off, static_cast<uint32_t>(tokens.size()));
        if (tokens.empty())
        {
            item->relocs.set(off + 4, new_ptr(zone, Ptr::Kind::Null, item, off + 4));
            put_be32(item, off + 4, 0);
            rebuild_children(item);
            return;
        }
        Node *arr = zone.new_node();
        arr->type = entries_template->type;
        arr->count = static_cast<uint32_t>(tokens.size());
        arr->block = entries_template->block;
        arr->data.assign(std::vector<uint8_t>(4 * tokens.size(), 0xFF));
        arr->segments = {Segment{arr->type, arr->count, static_cast<uint32_t>(4 * tokens.size()), false}};
        arr->align = entries_template->align;
        arr->origin = entries_template->origin;
        arr->origin_record = entries_template->origin_record;
        arr->origin_field = entries_template->origin_field;
        for (size_t i = 0; i < tokens.size(); ++i)
        {
            const Token &t = tokens[i];
            Node *entry = zone.new_node();
            entry->type = entry_template->type;
            entry->count = 1;
            entry->block = entry_template->block;
            entry->segments = entry_template->segments;
            copy_extra(entry, entry_template);
            entry->data.assign(std::vector<uint8_t>(12));
            if (t.kind == Token::Op)
            {
                put_be32(entry, 0, 0);
                put_be32(entry, 4, static_cast<uint32_t>(t.i));
            }
            else if (t.kind == Token::Int)
            {
                put_be32(entry, 0, 1);
                put_be32(entry, 8, static_cast<uint32_t>(t.i));
            }
            else if (t.kind == Token::Float)
            {
                put_be32(entry, 0, 1);
                put_be32(entry, 4, 1);
                put_bef32(entry, 8, static_cast<float>(t.f));
            }
            else
            {
                put_be32(entry, 0, 1);
                put_be32(entry, 4, 2);
                put_be32(entry, 8, FOLLOW);
                Node *text = string_like(zone, t.s, string_template);
                Ptr *tp = new_ptr(zone, Ptr::Kind::Follow, entry, 8, text);
                entry->relocs.set(8, tp);
                text->ptr = tp;
            }
            rebuild_children(entry);
            Ptr *ep = new_ptr(zone, Ptr::Kind::Follow, arr, static_cast<uint32_t>(4 * i), entry);
            arr->relocs.set(static_cast<uint32_t>(4 * i), ep);
            entry->ptr = ep;
        }
        rebuild_children(arr);
        Ptr *ap = new_ptr(zone, Ptr::Kind::Follow, item, off + 4, arr);
        item->relocs.set(off + 4, ap);
        arr->ptr = ap;
        put_be32(item, off + 4, FOLLOW);
        rebuild_children(item);
    }

    void set_items(const std::vector<Node *> &now)
    {
        array->count = static_cast<uint32_t>(now.size());
        array->data.assign(std::vector<uint8_t>(4 * now.size(), 0xFF));
        array->segments = {Segment{array->segments[0].type, array->count, static_cast<uint32_t>(4 * now.size()), false}};
        array->relocs.clear();
        for (size_t i = 0; i < now.size(); ++i)
        {
            Ptr *ip = new_ptr(zone, Ptr::Kind::Follow, array, static_cast<uint32_t>(4 * i), now[i]);
            array->relocs.set(static_cast<uint32_t>(4 * i), ip);
            now[i]->ptr = ip;
        }
        rebuild_children(array);
        put_be32(menu, offset_of(p, "menuDef_t", "itemCount"), static_cast<uint32_t>(now.size()));
        items = now;
    }

    const Platform &p;
    Zone &zone;
    Node *menu;
    Node *array = nullptr;
    std::vector<Node *> items;
    const Node *string_template = nullptr;
    Node *entries_template = nullptr;
    Node *entry_template = nullptr;
    uint32_t window = 0;

  private:
    void set_on(Node *owner, uint32_t off, const std::optional<std::string> &text)
    {
        if (!text)
        {
            owner->relocs.set(off, new_ptr(zone, Ptr::Kind::Null, owner, off));
            put_be32(owner, off, 0);
            return;
        }
        Node *node = string_like(zone, *text, string_template);
        Ptr *ptr = new_ptr(zone, Ptr::Kind::Follow, owner, off, node);
        owner->relocs.set(off, ptr);
        node->ptr = ptr;
        put_be32(owner, off, FOLLOW);
    }
};

std::vector<Token> replace_int(std::vector<Token> tokens, int32_t old, int32_t now)
{
    for (Token &t : tokens)
        if (t.kind == Token::Int && t.i == old)
            t.i = now;
    return tokens;
}

// the asset type of a record, as the Python's {record: type} of ASSET_RECORDS (the last type of a record)
std::string record_asset_type(const std::string &rec)
{
    if (rec == "clipMap_t")
        return "clipmap_pvs";
    const char *t = asset_type_of_record(rec);
    return t ? t : "";
}

// a copy of the game's difficulty list for zone, its nested assets name references
Node *options_template_copy(const Platform &p, Zone &zone, ConsoleLibrary *library)
{
    std::optional<LibraryEntry> found = library ? library->find_in_game_zones("menuDef_t", OPTIONS_TEMPLATE) : std::nullopt;
    if (!found)
        return nullptr;
    Cloner cloner(p, zone, zone.script_strings, [&](const std::string &rec, const std::string &name, Node *node) -> Node * {
        std::string type = record_asset_type(rec);
        if (type.empty())
            return nullptr;
        return build_reference(zone, p, type, *node, "," + lstrip_commas(name));
    });
    return cloner.copy_asset(*found->first, found->second);
}

std::vector<Node *> menu_lists_with_inline_menus(const Platform &p, Zone &zone)
{
    uint32_t menus_off = offset_of(p, "MenuList", "menus");
    std::vector<Node *> lists;
    for (const ZoneAsset &asset : zone.assets)
    {
        if (asset.type != "menulist" || !asset.ptr || !loads(asset.ptr))
            continue;
        Node *node = asset.ptr->node;
        Ptr *ptr = node->relocs.get(menus_off);
        Node *array = ptr && ptr->kind == Ptr::Kind::Follow ? ptr->node : nullptr;
        if (!array)
            continue;
        for (const auto &[off, r] : array->relocs.list())
            if (r->kind == Ptr::Kind::Follow)
            {
                lists.push_back(node);
                break;
            }
    }
    return lists;
}

// pointers of menu to asset slots load their own copy of the asset instead
void dealias(Zone &zone, Node *menu)
{
    std::vector<Node *> nodes;
    menu->walk([&](Node *n) { nodes.push_back(n); });
    for (Node *node : nodes)
    {
        bool changed = false;
        std::vector<std::pair<uint32_t, Ptr *>> relocs(node->relocs.list().begin(), node->relocs.list().end());
        for (const auto &[off, ptr] : relocs)
        {
            if (ptr->kind == Ptr::Kind::Alias && ptr->slot && ptr->slot->node)
            {
                Node *copy = clone(zone, ptr->slot->node);
                Ptr *fp = new_ptr(zone, Ptr::Kind::Follow, node, off, copy);
                node->relocs.set(off, fp);
                copy->ptr = fp;
                put_be32(node, off, FOLLOW);
                changed = true;
            }
        }
        if (changed)
            rebuild_children(node);
    }
}

void append_menu(const Platform &p, Zone &zone, Node *menu_list, Node *menu)
{
    Node *array = menu_list->relocs.get(offset_of(p, "MenuList", "menus"))->node;
    uint32_t offset = static_cast<uint32_t>(array->data.size());
    std::vector<uint8_t> &data = array->data.owned();
    data.insert(data.end(), 4, 0xFF);
    array->count += 1;
    array->segments = {Segment{array->segments[0].type, array->count, static_cast<uint32_t>(array->data.size()), false}};
    array->relocs.set(offset, new_ptr(zone, Ptr::Kind::Follow, array, offset, menu));
    rebuild_children(array);
    put_be32(menu_list, offset_of(p, "MenuList", "menuCount"), array->count);
}
} // namespace

std::vector<std::string> add_options_menus(const Platform &p, Zone &zone, ConsoleLibrary *library, const std::vector<MapOption> &options,
                                           const Log &log)
{
    static const Regex highlight_re(R"re(("setLocalVarInt"\s+"ui_highlight"\s+)\d+)re");
    std::vector<Node *> lists = menu_lists_with_inline_menus(p, zone);
    if (options.empty() || lists.empty())
        return {};
    std::vector<std::string> names;
    for (size_t index = 0; index < options.size(); ++index)
    {
        const MapOption &option = options[index];
        Node *menu = options_template_copy(p, zone, library);
        if (!menu)
            return {};
        MenuEditor editor(p, zone, menu);
        std::vector<Node *> items = editor.items;
        std::optional<size_t> first_button;
        for (size_t i = 0; i < items.size() && !first_button; ++i)
        {
            auto action = editor.string(items[i], "action");
            if (action && !action->empty())
                first_button = i;
        }
        if (!first_button || *first_button < 3)
            throw MenuError(std::string("unexpected layout of ") + OPTIONS_TEMPLATE);
        std::vector<Node *> row(items.begin() + static_cast<std::ptrdiff_t>(*first_button - 3), items.begin() + static_cast<std::ptrdiff_t>(*first_button + 1));
        Node *highlight = row[1], *hint = row[2], *button = row[3];
        // the frame and title; the descriptions and pictures of the game's difficulties go
        std::vector<Node *> frame;
        for (size_t i = 0; i < *first_button - 3; ++i)
        {
            bool has_str = false;
            for (const char *field : {"visibleExp", "materialExp"})
                for (const Token &t : editor.expression(items[i], field))
                    has_str = has_str || t.kind == Token::Str;
            if (!has_str)
                frame.push_back(items[i]);
        }
        for (Node *item : frame)
        {
            std::vector<Token> tokens = editor.expression(item, "textExp");
            if (tokens.empty())
                continue;
            for (Token &t : tokens)
                if (t.kind == Token::Str)
                    t.s = py::upper(option.label);
            editor.set_expression(item, "textExp", tokens);
        }
        std::vector<Node *> new_items = frame;
        size_t default_choice = 0;
        for (size_t k = 0; k < option.choices.size(); ++k)
        {
            const auto &[value, name] = option.choices[k];
            std::vector<Node *> copies;
            for (Node *item : row)
                copies.push_back(clone(zone, item));
            for (Node *copy : copies)
                editor.move(copy, OPTIONS_ROW_STEP * static_cast<double>(k));
            for (auto [copy, source] : {std::pair<Node *, Node *>{copies[1], highlight}, {copies[2], hint}})
                editor.set_expression(copy, "visibleExp", replace_int(editor.expression(source, "visibleExp"), 1, static_cast<int32_t>(k + 1)));
            Node *b = copies[3];
            editor.set_string(b, "window.name", "t4ff_choice" + std::to_string(k));
            std::vector<Token> text = editor.expression(button, "textExp");
            for (Token &t : text)
                if (t.kind == Token::Str)
                    t.s = name;
            editor.set_expression(b, "textExp", text);
            editor.set_string(b, "action", "\"play\" \"mouse_click\" ; \"scriptMenuResponse\" \"" + value + "\" ; ");
            std::string focus = highlight_re.sub(editor.string(button, "onFocus").value_or(""), "\\g<1>" + std::to_string(k + 1));
            editor.set_string(b, "onFocus", focus);
            editor.set_string(b, "dvarTest", option.dvar);
            editor.set_string(b, "enableDvar", value + " ");
            if (value == option.default_value)
                default_choice = k;
            new_items.insert(new_items.end(), copies.begin(), copies.end());
        }
        editor.set_items(new_items);
        std::string name = "t4ff_option" + std::to_string(index);
        editor.set_menu_string("window.name", name);
        editor.set_menu_string("onOpen", "\"setLocalVarBool\" \"ui_centerPopup\" 1 ; \"setfocus\" \"t4ff_choice" + std::to_string(default_choice) +
                                             "\" ; \"setfocusbydvar\" \"" + option.dvar + "\" ; ");
        // B keeps the choice (the level script closes the menu, and the map's menus wait for the answer)
        editor.set_menu_string("onESC", std::string("\"scriptMenuResponse\" \"keep\" ; "));
        dealias(zone, menu);
        append_menu(p, zone, lists[0], menu);
        names.push_back(name);
    }
    // the restart: the frame alone, which runs fast_restart when it opens
    Node *menu = options_template_copy(p, zone, library);
    MenuEditor editor(p, zone, menu);
    editor.set_items(std::vector<Node *>(editor.items.begin(), editor.items.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(3, editor.items.size()))));
    editor.set_menu_string("window.name", std::string(OPTIONS_RESTART_MENU));
    editor.set_menu_string("onOpen", std::string("\"exec\" \"fast_restart\" ; "));
    editor.set_menu_string("onClose", std::nullopt);
    editor.set_menu_string("onESC", std::nullopt);
    dealias(zone, menu);
    append_menu(p, zone, lists[0], menu);
    names.push_back(OPTIONS_RESTART_MENU);
    if (log)
    {
        std::vector<std::string> labels;
        for (const MapOption &o : options)
            labels.push_back(o.label);
        log("options: asked in game too (" + join(labels) + "), at the start: the level restarts when a choice changes");
    }
    return names;
}

std::vector<std::pair<std::string, std::string>> localized_strings(const Platform &p, Zone &zone)
{
    uint32_t value_off = offset_of(p, "LocalizeEntry", "value"), name_off = offset_of(p, "LocalizeEntry", "name");
    std::vector<std::pair<std::string, std::string>> out;
    std::unordered_map<std::string, size_t> index;
    zone.walk([&](Node *node) {
        if (!is_type(node, "LocalizeEntry"))
            return;
        std::string texts[2];
        uint32_t offs[2] = {value_off, name_off};
        for (int k = 0; k < 2; ++k)
        {
            Ptr *ptr = node->relocs.get(offs[k]);
            Node *target = ptr && ptr->kind != Ptr::Kind::Null ? ptr->target() : nullptr;
            texts[k] = target ? stripped_text(target) : "";
        }
        if (texts[1].empty())
            return;
        auto it = index.find(texts[1]);
        if (it != index.end())
            out[it->second].second = texts[0];
        else
        {
            index[texts[1]] = out.size();
            out.emplace_back(texts[1], texts[0]);
        }
    });
    return out;
}
} // namespace t4ff
