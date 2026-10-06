#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/pystr.h"
#include "core/zone.h"

// Reading and editing the menus of a console zone (the Python t4ff's MenuEditor and clone of menu.py),
// for the conversion's menu passes and the menu command.
namespace t4ff::menu_edit
{
uint32_t offset_of(const Platform &p, const std::string &rec, const std::string &field);
std::string node_text(const Node *n);
std::string stripped_text(const Node *n);
std::string lstrip_commas(const std::string &s);
void set_text(Node *s, const std::string &text_with_nul);
// the string a pointer of node at off loads (not a null pointer), nullopt when none
std::optional<std::string> text_of(Node *node, uint32_t off);
bool is_type(const Node *n, std::string_view name);
bool loads(const Ptr *ptr);
Ptr *new_ptr(Zone &zone, Ptr::Kind kind, Node *owner, uint32_t offset, Node *node = nullptr, uint32_t index = 0, uint32_t inner = 0,
             Ptr *slot = nullptr);
// a string like template's (its type and block)
Node *string_like(Zone &zone, const std::string &text, const Node *tmpl);
void put_be32(Node *node, uint32_t off, uint32_t v);
uint32_t be32(const Node *node, uint32_t off);
float bef32(const Node *node, uint32_t off);
void put_bef32(Node *node, uint32_t off, float f);
// children: the nodes the follow / insert pointers load, in the order of the pointers / of the fields
void rebuild_children(Node *node);
void sorted_children(Node *node);
void copy_extra(Node *to, const Node *from);
const Node *first_string(Node *root);
std::vector<Node *> menus_of(Zone &zone);
// a copy of node and of the nodes it loads; pointers to anything else keep pointing there
Node *clone(Zone &zone, Node *node, std::unordered_map<const Node *, Node *> &memo);
Node *clone(Zone &zone, Node *node);
// pointers of menu to asset slots load their own copy of the asset instead
void dealias(Zone &zone, Node *menu);

struct MenuError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

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
    MenuEditor(const Platform &p, Zone &zone, Node *menu, const std::string &menu_name = "popmenu_difficulty") : p(p), zone(zone), menu(menu)
    {
        window = offset_of(p, "itemDef_s", "window");
        Ptr *ptr = menu->relocs.get(offset_of(p, "menuDef_t", "items"));
        if (!ptr || ptr->kind != Ptr::Kind::Follow)
            throw MenuError("menu " + menu_name + " has no items");
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

    // the menu asset menu_name of the zone (its node)
    static Node *find_menu(Zone &zone, const std::string &menu_name)
    {
        for (const ZoneAsset &a : zone.assets)
            if (a.type == "menu" && a.name == menu_name)
                return a.ptr->kind == Ptr::Kind::Alias ? a.ptr->target() : a.ptr->node;
        throw MenuError("no menu " + menu_name + " in this zone");
    }

    // x, y, w, h of an item's window.rect
    std::array<float, 4> rect(const Node *item) const
    {
        uint32_t off = offset("window.rect");
        return {bef32(item, off), bef32(item, off + 4), bef32(item, off + 8), bef32(item, off + 12)};
    }

    // the key handlers (execKey) of an item, in order
    std::vector<Node *> key_handlers(Node *item) const
    {
        std::vector<Node *> handlers;
        Ptr *ptr = item->relocs.get(offset("onKey"));
        while (ptr && ptr->kind != Ptr::Kind::Null && ptr->target())
        {
            handlers.push_back(ptr->target());
            ptr = handlers.back()->relocs.get(8);
        }
        return handlers;
    }

    // appends a key handler to an item's chain
    void add_key_handler(Node *item, int key, const std::string &action)
    {
        uint32_t off = offset("onKey");
        Node *tmpl = nullptr;
        zone.walk([&](Node *n) {
            if (!tmpl && is_type(n, "ItemKeyHandler"))
                tmpl = n;
        });
        if (!tmpl)
            throw MenuError("no key handler to copy");
        Node *handler = zone.new_node();
        handler->type = tmpl->type;
        handler->count = 1;
        handler->block = tmpl->block;
        handler->data.assign(std::vector<uint8_t>(12));
        put_be32(handler, 0, static_cast<uint32_t>(key));
        put_be32(handler, 4, FOLLOW);
        handler->segments = {Segment{tmpl->type, 1, 12, false}};
        handler->align = tmpl->align;
        handler->origin = tmpl->origin;
        handler->origin_record = tmpl->origin_record;
        handler->origin_field = tmpl->origin_field;
        Node *text = string_like(zone, action, string_template);
        Ptr *tp = new_ptr(zone, Ptr::Kind::Follow, handler, 4, text);
        handler->relocs.set(4, tp);
        text->ptr = tp;
        handler->relocs.set(8, new_ptr(zone, Ptr::Kind::Null, handler, 8));
        rebuild_children(handler);
        // the end of the chain
        Node *owner = item;
        uint32_t owner_off = off;
        while (owner->relocs.get(owner_off) && owner->relocs.get(owner_off)->kind == Ptr::Kind::Follow)
        {
            owner = owner->relocs.get(owner_off)->node;
            owner_off = 8;
        }
        Ptr *fp = new_ptr(zone, Ptr::Kind::Follow, owner, owner_off, handler);
        owner->relocs.set(owner_off, fp);
        handler->ptr = fp;
        put_be32(owner, owner_off, FOLLOW);
        rebuild_children(owner);
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

std::vector<Token> replace_int(std::vector<Token> tokens, int32_t old, int32_t now);
} // namespace t4ff::menu_edit
