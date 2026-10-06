#include "convert/usermaps_menu.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_set>

#include "convert/assets.h"
#include "convert/menu_editor.h"
#include "core/pyre.h"
#include "core/pystr.h"

namespace t4ff
{
namespace fs = std::filesystem;
using namespace menu_edit;
using pyre::Regex;

namespace
{
constexpr const char *LIST_MENU = "levels_unlock";
constexpr const char *STOCK_MAPS[] = {"nazi_zombie_prototype", "nazi_zombie_asylum", "nazi_zombie_sumpf", "nazi_zombie_factory"};
constexpr int FIRST_HIGHLIGHT = 100; // ui_highlight of the dynamic rows (the stock ones use 2 to 5)
constexpr const char *PREVIEW = "image_codxe_map";
constexpr double COUNTER_DX = 280.0; // the counter, right of the rows
constexpr double OPTIONS_DY = 120.0; // the focused map's options, under its description
constexpr const char *PREVIEW_SLOT = "codxe_map_preview";
constexpr uint32_t PREVIEW_WIDTH = 512, PREVIEW_HEIGHT = 288;
constexpr int KEY_BUTTON_X = 3, KEY_BUTTON_Y = 4, KEY_LSHLDR = 5, KEY_RSHLDR = 6;
constexpr double ROW_HEIGHT = 19.0; // the items of a map row
constexpr const char *USERMAPS_MENU = "levels_dev";
constexpr const char *MENU_LIST = "ui/patch_menus.txt";
constexpr const char *USERMAPS_TITLE = "Custom Maps";
constexpr float TITLE_RECT[4] = {22.0f, 32.0f, 100.0f, 100.0f};
constexpr int TITLE_ALIGN = 4, TITLE_STYLE = 6;
constexpr float TITLE_SCALE = 0.5476f;
constexpr const char *OWN_USERMAPS_MENU = "codxe_usermaps";
// menu expression operators (operationEnum)
constexpr int OP_RIGHTPAREN = 1, OP_GREATERTHAN = 10, OP_EQUALS = 12, OP_NOTEQUAL = 13, OP_AND = 14, OP_LEFTPAREN = 16;
constexpr int OP_DVARINT = 28, OP_DVARSTRING = 31;

const Regex &devmap_re()
{
    static const Regex re(R"re("exec"\s+"devmap\s+([^"\s]+)")re");
    return re;
}

Token op(int v)
{
    Token t{Token::Op};
    t.i = v;
    return t;
}
Token str(const std::string &s)
{
    Token t{Token::Str};
    t.s = s;
    return t;
}
Token integer(int v)
{
    Token t{Token::Int};
    t.i = v;
    return t;
}

std::vector<Token> dvar_string(const std::string &name)
{
    return {op(OP_DVARSTRING), str(name), op(OP_RIGHTPAREN)};
}

std::vector<Token> not_empty(const std::string &name)
{
    std::vector<Token> t = dvar_string(name);
    t.push_back(op(OP_NOTEQUAL));
    t.push_back(str(""));
    return t;
}

std::vector<Token> and_of(const std::vector<Token> &a, const std::vector<Token> &b)
{
    std::vector<Token> t{op(OP_LEFTPAREN)};
    t.insert(t.end(), a.begin(), a.end());
    t.push_back(op(OP_RIGHTPAREN));
    t.push_back(op(OP_AND));
    t.push_back(op(OP_LEFTPAREN));
    t.insert(t.end(), b.begin(), b.end());
    t.push_back(op(OP_RIGHTPAREN));
    return t;
}

struct Row
{
    std::string map;
    std::vector<size_t> row, preview;
    size_t button;
    double y;
    std::optional<int> highlight;
    std::optional<std::string> title, description, image;
};

// CoD Xenon's rows of converted maps: their items (the row and its preview) and what they show
std::vector<Row> custom_rows(MenuEditor &editor)
{
    static const Regex highlight_re(R"re("setLocalVarInt"\s+"ui_highlight"\s+"(\d+)")re");
    std::vector<Row> rows;
    const std::vector<Node *> &items = editor.items;
    for (size_t index = 0; index < items.size(); ++index)
    {
        Node *item = items[index];
        std::string action = editor.string(item, "action").value_or("");
        auto match = devmap_re().search(action);
        if (!match)
            continue;
        std::string name = match->str(1);
        bool stock = false;
        for (const char *s : STOCK_MAPS)
            stock = stock || py::lower(name) == s;
        if (stock)
            continue;
        Row r;
        r.map = name;
        r.button = index;
        r.y = editor.rect(item)[1];
        for (size_t i = index >= 4 ? index - 4 : 0; i <= index; ++i)
            if (std::abs(editor.rect(items[i])[1] - r.y) <= 1.5)
                r.row.push_back(i);
        for (size_t i = 0; i < items.size(); ++i)
            if (editor.string(items[i], "window.name") == std::optional<std::string>("image_" + name))
                r.preview.push_back(i);
        std::vector<Token> text = editor.expression(item, "textExp");
        std::string focus = editor.string(item, "onFocus").value_or("");
        if (auto h = highlight_re.search(focus))
            r.highlight = std::stoi(h->str(1));
        for (size_t i : r.preview)
        {
            std::vector<Token> tokens = editor.expression(items[i], "materialExp");
            if (tokens.size() == 1 && tokens[0].kind == Token::Str)
                r.image = tokens[0].s;
        }
        if (text.size() == 1 && text[0].kind == Token::Str)
            r.title = text[0].s;
        for (size_t i : r.preview)
        {
            std::vector<Token> tokens = editor.expression(items[i], "textExp");
            if (tokens.size() == 1 && tokens[0].kind == Token::Str && std::optional<std::string>(tokens[0].s) != r.title)
                r.description = tokens[0].s;
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

void put_be_int(Node *node, uint32_t off, int32_t v)
{
    put_be32(node, off, static_cast<uint32_t>(v));
}

// Make editor's menu (a copy of levels_unlock) the Custom Maps menu; returns the items of the copy left out.
std::vector<Node *> usermaps_menu(MenuEditor &editor, int rows)
{
    static const Regex highlight_sub(R"re(("setLocalVarInt"\s+"ui_highlight"\s+)"\d+")re");
    static const Regex show_sub(R"re("show"\s+"image_[^"]*"\s*;)re");
    Zone &zone = editor.zone;
    std::vector<Node *> items = editor.items;
    std::vector<Row> found = custom_rows(editor);
    const Row &tmpl = found.at(0);
    Node *button = items[tmpl.button];
    std::vector<Node *> row_items;
    for (size_t i : tmpl.row)
        row_items.push_back(items[i]);
    if (row_items.size() != 4 || row_items.back() != button)
        throw MenuError("unexpected layout of the custom map rows");
    Node *backing = row_items[0], *highlight = row_items[1], *hint = row_items[2];
    std::optional<int> old = tmpl.highlight;
    std::vector<Node *> preview_items;
    for (size_t i : tmpl.preview)
        preview_items.push_back(items[i]);
    // the frame: the items before the first map row (row items are ROW_HEIGHT high)
    size_t first = items.size();
    for (size_t i = 0; i < items.size(); ++i)
        if (std::abs(editor.rect(items[i])[3] - ROW_HEIGHT) < 1.5)
        {
            first = i;
            break;
        }
    if (first == items.size())
        throw MenuError("no map row in the menu");
    std::vector<Node *> frame(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(first));
    double y0 = editor.rect(items[first])[1];
    const double step = 20.0;
    auto row_copy = [&](Node *item, int row) {
        Node *copy = clone(zone, item);
        editor.move(copy, y0 + step * row - tmpl.y);
        return copy;
    };
    auto replace_old = [&](std::vector<Token> tokens, int now) { return old ? replace_int(std::move(tokens), *old, now) : tokens; };

    std::vector<Node *> new_items = frame;

    // the title, in the style of the frame's texts
    Node *text_item = nullptr;
    for (Node *item : frame)
    {
        auto t = editor.string(item, "text");
        if (t && !t->empty())
        {
            text_item = item;
            break;
        }
    }
    if (text_item)
    {
        Node *title = clone(zone, text_item);
        editor.set_string(title, "text", std::string(USERMAPS_TITLE));
        editor.set_expression(title, "textExp", {});
        editor.set_expression(title, "visibleExp", {});
        for (const char *field : {"window.rect", "window.rectClient"})
        {
            uint32_t off = editor.offset(field);
            for (int k = 0; k < 4; ++k)
                put_bef32(title, off + 4 * k, TITLE_RECT[k]);
            put_be_int(title, off + 16, 1);
            put_be_int(title, off + 20, 1);
        }
        put_be_int(title, editor.offset("textAlignMode"), TITLE_ALIGN);
        put_bef32(title, editor.offset("textscale"), TITLE_SCALE);
        put_be_int(title, editor.offset("textStyle"), TITLE_STYLE);
        new_items.push_back(title);
    }

    std::vector<Token> visible_language = editor.expression(backing, "visibleExp");

    // the catcher above the rows: scrolls up when the first row is left upwards
    Node *up = row_copy(button, 0);
    editor.set_string(up, "window.name", std::string("codxe_map_up"));
    editor.set_string(up, "action", std::nullopt);
    editor.set_string(up, "leaveFocus", std::nullopt);
    editor.set_string(up, "onFocus", std::string("\"setdvar\" \"ui_codxe_scroll\" \"-1\" ; \"setfocus\" \"codxe_map0\" ; "));
    editor.set_expression(up, "textExp", {});
    editor.set_expression(up, "visibleExp", {op(OP_DVARINT), str("ui_codxe_mapoffset"), op(OP_RIGHTPAREN), op(OP_GREATERTHAN), integer(0)});
    new_items.push_back(up);

    for (int row = 0; row < rows; ++row)
    {
        std::string dvar = "ui_codxe_map" + std::to_string(row);
        std::vector<Token> shown = visible_language.empty() ? not_empty(dvar) : and_of(visible_language, not_empty(dvar));
        Node *b = row_copy(backing, row);
        editor.set_expression(b, "visibleExp", shown);
        Node *h = row_copy(highlight, row);
        editor.set_expression(h, "visibleExp", replace_old(editor.expression(highlight, "visibleExp"), FIRST_HIGHLIGHT + row));
        Node *a = row_copy(hint, row);
        editor.set_expression(a, "visibleExp", replace_old(editor.expression(hint, "visibleExp"), FIRST_HIGHLIGHT + row));
        Node *btn = row_copy(button, row);
        editor.set_string(btn, "window.name", "codxe_map" + std::to_string(row));
        std::string action = devmap_re().sub(editor.string(button, "action").value_or(""),
                                             [&](const pyre::Match &) { return "\"exec\" \"vstr ui_codxe_mapcmd" + std::to_string(row) + "\""; });
        editor.set_string(btn, "action", action);
        std::string focus = editor.string(button, "onFocus").value_or("");
        focus = highlight_sub.sub(focus, [&](const pyre::Match &m) { return m.str(1) + "\"" + std::to_string(FIRST_HIGHLIGHT + row) + "\""; });
        focus = show_sub.sub(focus, [&](const pyre::Match &) {
            return std::string("\"show\" \"") + PREVIEW + "\" ; \"setdvar\" \"ui_codxe_focus\" \"" + std::to_string(row) + "\" ;";
        });
        editor.set_string(btn, "onFocus", focus);
        editor.set_expression(btn, "visibleExp", shown);
        editor.set_expression(btn, "textExp", dvar_string(dvar));
        new_items.insert(new_items.end(), {b, h, a, btn});
    }

    // the catcher below the rows: scrolls down when the last row is left downwards
    Node *down = row_copy(button, rows - 1);
    editor.set_string(down, "window.name", std::string("codxe_map_down"));
    editor.set_string(down, "action", std::nullopt);
    editor.set_string(down, "leaveFocus", std::nullopt);
    editor.set_string(down, "onFocus",
                      "\"setdvar\" \"ui_codxe_scroll\" \"1\" ; \"setfocus\" \"codxe_map" + std::to_string(rows - 1) + "\" ; ");
    editor.set_expression(down, "textExp", {});
    editor.set_expression(down, "visibleExp", {op(OP_DVARINT), str("ui_codxe_mapmore"), op(OP_RIGHTPAREN), op(OP_EQUALS), integer(1)});
    new_items.push_back(down);

    // the position in the list ("14-26 / 40"), right of the last row
    Node *counter = row_copy(hint, rows - 1);
    editor.move(counter, 0.0, COUNTER_DX);
    editor.set_string(counter, "text", std::nullopt);
    editor.set_expression(counter, "textExp", dvar_string("ui_codxe_maprange"));
    editor.set_expression(counter, "visibleExp", not_empty("ui_codxe_maprange"));
    new_items.push_back(counter);

    // the preview of the focused map: its picture, name and description
    for (Node *item : preview_items)
    {
        Node *copy = clone(zone, item);
        editor.set_string(copy, "window.name", std::string(PREVIEW));
        if (!editor.expression(item, "materialExp").empty())
        {
            editor.set_expression(copy, "materialExp", dvar_string("ui_codxe_mapimage"));
            editor.set_expression(copy, "visibleExp", not_empty("ui_codxe_mapimage"));
        }
        else
        {
            std::vector<Token> tokens = editor.expression(item, "textExp");
            bool is_title = tokens.size() == 1 && tokens[0].kind == Token::Str && tmpl.title && tokens[0].s == *tmpl.title;
            editor.set_expression(copy, "textExp", dvar_string(is_title ? "ui_codxe_maptitle" : "ui_codxe_mapdesc"));
            if (!is_title)
            {
                // the focused map's options (its options.txt), under its description
                Node *options = clone(zone, copy);
                editor.move(options, OPTIONS_DY);
                editor.set_expression(options, "textExp", dvar_string("ui_codxe_mapoptions"));
                editor.set_expression(options, "visibleExp", not_empty("ui_codxe_mapoptions"));
                new_items.push_back(copy);
                copy = options;
            }
        }
        new_items.push_back(copy);
    }

    std::unordered_set<const Node *> kept(new_items.begin(), new_items.end());
    std::vector<Node *> dropped;
    for (Node *item : items)
        if (!kept.count(item))
            dropped.push_back(item);
    editor.set_items(new_items);

    // the menu: its own name, the first row focused; Back (B) closes it, LB / RB: a page up / down
    editor.set_menu_string("window.name", std::string(USERMAPS_MENU));
    editor.set_menu_string("onOpen", std::string("\"setfocus\" \"codxe_map0\" ; "));
    editor.set_menu_string("onClose", std::nullopt);
    for (Node *item : frame)
    {
        for (Node *handler : editor.key_handlers(item))
        {
            std::optional<std::string> action = text_of(handler, 4);
            if (action && !action->empty() && py::contains(*action, LIST_MENU))
            {
                Node *text = string_like(zone, py::replace(*action, std::string("\"") + LIST_MENU + "\"", std::string("\"") + USERMAPS_MENU + "\""),
                                         editor.string_template);
                Ptr *tp = new_ptr(zone, Ptr::Kind::Follow, handler, 4, text);
                handler->relocs.set(4, tp);
                text->ptr = tp;
                rebuild_children(handler);
            }
        }
    }
    Node *key_owner = new_items[0];
    for (Node *item : frame)
        if (!editor.key_handlers(item).empty())
        {
            key_owner = item;
            break;
        }
    editor.add_key_handler(key_owner, KEY_LSHLDR, "\"setdvar\" \"ui_codxe_scroll\" \"-" + std::to_string(rows) + "\" ; ");
    editor.add_key_handler(key_owner, KEY_RSHLDR, "\"setdvar\" \"ui_codxe_scroll\" \"" + std::to_string(rows) + "\" ; ");
    // X / Y: the focused map's next choice of its first / second option (CoD Xe changes it)
    editor.add_key_handler(key_owner, KEY_BUTTON_X, "\"setdvar\" \"ui_codxe_option\" \"1\" ; ");
    editor.add_key_handler(key_owner, KEY_BUTTON_Y, "\"setdvar\" \"ui_codxe_option\" \"2\" ; ");
    return dropped;
}

// nothing left in the zone refers to the removed items
void check_references(Zone &zone, const std::vector<Node *> &removed)
{
    std::unordered_set<const Node *> gone;
    for (Node *item : removed)
        item->walk([&](Node *n) { gone.insert(n); });
    zone.walk([&](Node *node) {
        if (gone.count(node))
            throw MenuError("a removed item is still in the zone: " + node->repr());
        for (const auto &[off, ptr] : node->relocs.list())
            if ((ptr->kind == Ptr::Kind::Ref && gone.count(ptr->node)) ||
                (ptr->kind == Ptr::Kind::Alias && ptr->slot && gone.count(ptr->slot->owner)))
                throw MenuError(node->repr() + " refers to a removed item");
    });
}

// Replace CoD Xenon's rows of converted maps and their previews by one "Custom Maps" row
void custom_maps_row(MenuEditor &editor, const std::vector<Row> &found)
{
    static const Regex show_re(R"re("show"\s+"image_[^"]*"\s*;\s*)re");
    std::vector<Node *> items = editor.items;
    const Row &tmpl = found[0];
    Node *button = items[tmpl.button];
    std::vector<Node *> row;
    for (size_t i : tmpl.row)
        row.push_back(clone(editor.zone, items[i]));
    Node *new_button = row.back();
    editor.set_string(new_button, "action", std::string("\"play\" \"mouse_click\" ; \"open\" \"") + USERMAPS_MENU + "\" ; ");
    editor.set_expression(new_button, "textExp", {});
    editor.set_string(new_button, "text", std::string(USERMAPS_TITLE));
    // no map picture while it is focused
    std::string focus = show_re.sub(editor.string(button, "onFocus").value_or(""), std::string_view(""));
    editor.set_string(new_button, "onFocus", focus);
    std::set<size_t> removed;
    for (const Row &r : found)
    {
        removed.insert(r.row.begin(), r.row.end());
        removed.insert(r.preview.begin(), r.preview.end());
    }
    size_t first = *removed.begin();
    std::vector<Node *> kept;
    for (size_t i = 0; i < items.size(); ++i)
        if (!removed.count(i))
            kept.push_back(items[i]);
    size_t position = 0;
    for (size_t i = 0; i < first; ++i)
        position += !removed.count(i);
    std::vector<Node *> now(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(position));
    now.insert(now.end(), row.begin(), row.end());
    now.insert(now.end(), kept.begin() + static_cast<std::ptrdiff_t>(position), kept.end());
    editor.set_items(now);
    std::vector<Node *> gone;
    for (size_t i : removed)
        gone.push_back(items[i]);
    check_references(editor.zone, gone);
}

// Add menu to the zone as the asset right after the menu after, and to the menu list menu_list
void add_menu(const Platform &p, Zone &zone, Node *menu, const std::string &name, const std::string &after, const std::string &menu_list)
{
    Node *assets = zone.assets_node;
    std::optional<size_t> index;
    std::vector<size_t> lists;
    for (size_t i = 0; i < zone.assets.size(); ++i)
    {
        if (!index && zone.assets[i].type == "menu" && zone.assets[i].name == after)
            index = i;
        if (zone.assets[i].type == "menulist" && zone.assets[i].name == menu_list)
            lists.push_back(i);
    }
    if (!index || lists.empty() || lists[0] <= *index)
        throw MenuError("no menu " + after + " before the menu list " + menu_list + " in this zone");
    Ptr *ptr = new_ptr(zone, Ptr::Kind::Insert, assets, 0, menu);
    menu->insert = true;
    menu->ptr = ptr;
    struct Entry
    {
        std::vector<uint8_t> bytes;
        Ptr *slot;
    };
    std::vector<Entry> entries;
    for (size_t i = 0; i < zone.assets.size(); ++i)
        entries.push_back(Entry{std::vector<uint8_t>(assets->data.begin() + 8 * i, assets->data.begin() + 8 * i + 8),
                                assets->relocs.get(static_cast<uint32_t>(8 * i + 4))});
    auto type_index = std::find(p.asset_types.begin(), p.asset_types.end(), "menu") - p.asset_types.begin();
    std::vector<uint8_t> entry(8);
    p.put_u32(entry.data(), static_cast<uint32_t>(type_index));
    p.put_u32(entry.data() + 4, INSERT);
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(*index + 1), Entry{entry, ptr});
    std::vector<uint8_t> data;
    assets->relocs.clear();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        data.insert(data.end(), entries[i].bytes.begin(), entries[i].bytes.end());
        if (entries[i].slot)
        {
            entries[i].slot->offset = static_cast<uint32_t>(8 * i + 4);
            assets->relocs.set(static_cast<uint32_t>(8 * i + 4), entries[i].slot);
        }
    }
    assets->count = static_cast<uint32_t>(2 * entries.size());
    assets->segments = {Segment{assets->segments[0].type, assets->count, static_cast<uint32_t>(data.size()), false}};
    assets->data.assign(std::move(data));
    rebuild_children(assets);
    ZoneAsset asset;
    asset.type = "menu";
    asset.ptr = ptr;
    asset.name = name;
    zone.assets.insert(zone.assets.begin() + static_cast<std::ptrdiff_t>(*index + 1), asset);

    // the menu list: one more menu
    Node *listed = assets->relocs.get(static_cast<uint32_t>(8 * (lists[0] + 1) + 4))->node;
    Node *array = listed->relocs.get(offset_of(p, "MenuList", "menus"))->node;
    uint32_t offset = static_cast<uint32_t>(array->data.size());
    std::vector<uint8_t> &arr = array->data.owned();
    arr.insert(arr.end(), 4, 0);
    array->count += 1;
    array->segments = {Segment{array->segments[0].type, array->count, static_cast<uint32_t>(array->data.size()), false}};
    array->relocs.set(offset, new_ptr(zone, Ptr::Kind::Alias, array, offset, nullptr, 1, 0, ptr));
    put_be32(listed, offset_of(p, "MenuList", "menuCount"), array->count);
}

// Add the material and image codxe_map_preview (a copy of a preview material of CoD Xenon's, with a
// black 512x288 image of its own) at the end of the zone.
void add_preview_slot(const Platform &p, Zone &zone, const std::optional<std::string> &template_material)
{
    const ZoneAsset *material = nullptr;
    for (const ZoneAsset &a : zone.assets)
        if (a.type == "material" && template_material && a.name == *template_material)
        {
            material = &a;
            break;
        }
    if (!material)
        throw MenuError("no preview material " + (template_material ? "'" + *template_material + "'" : std::string("None")) + " to copy");
    Node *source = material->ptr->kind == Ptr::Kind::Alias ? material->ptr->target() : material->ptr->node;
    Node *copy = clone(zone, source);
    uint32_t name_off = offset_of(p, "Material", "info") + offset_of(p, "MaterialInfo", "name");
    Node *name = string_like(zone, PREVIEW_SLOT, first_string(source));
    Ptr *np = new_ptr(zone, Ptr::Kind::Follow, copy, name_off, name);
    copy->relocs.set(name_off, np);
    name->ptr = np;
    rebuild_children(copy);

    // a black picture, as the slot's texture: 512x288 DXT1, one level
    ImageData black;
    black.name = PREVIEW_SLOT;
    black.format = Fmt::A8R8G8B8;
    black.width = PREVIEW_WIDTH;
    black.height = PREVIEW_HEIGHT;
    std::vector<uint8_t> bgra(static_cast<size_t>(PREVIEW_WIDTH) * PREVIEW_HEIGHT * 4, 0);
    for (size_t i = 3; i < bgra.size(); i += 4)
        bgra[i] = 255;
    black.levels = {bgra};
    TextureOptions o;
    o.keep_mips = false;
    ConsoleTexture tex = build_console_texture(black, o);
    Node *image = build_console_image(zone, p, PREVIEW_SLOT, tex, 0, 3, 0);
    Node *table = copy->relocs.get(offset_of(p, "Material", "textureTable"))->node;
    uint32_t image_off = offset_of(p, "MaterialTextureDef", "u");
    image->insert = true;
    Ptr *ip = new_ptr(zone, Ptr::Kind::Insert, table, image_off, image);
    table->relocs.set(image_off, ip);
    image->ptr = ip;
    put_be32(table, image_off, INSERT);
    rebuild_children(table);

    // the zone's asset list: one more entry, loading the material (and its image)
    Node *assets = zone.assets_node;
    uint32_t offset = static_cast<uint32_t>(assets->data.size());
    auto type_index = std::find(p.asset_types.begin(), p.asset_types.end(), "material") - p.asset_types.begin();
    std::vector<uint8_t> &data = assets->data.owned();
    data.resize(data.size() + 8);
    p.put_u32(data.data() + offset, static_cast<uint32_t>(type_index));
    p.put_u32(data.data() + offset + 4, INSERT);
    assets->count += 2;
    assets->segments = {Segment{assets->segments[0].type, assets->count, static_cast<uint32_t>(assets->data.size()), false}};
    Ptr *ptr = new_ptr(zone, Ptr::Kind::Insert, assets, offset + 4, copy);
    copy->insert = true;
    copy->ptr = ptr;
    assets->relocs.set(offset + 4, ptr);
    rebuild_children(assets);
    ZoneAsset asset;
    asset.type = "material";
    asset.ptr = ptr;
    asset.name = PREVIEW_SLOT;
    zone.assets.push_back(asset);
}
} // namespace

std::vector<CustomRow> make_dynamic(const Platform &p, Zone &zone, int rows)
{
    MenuEditor editor(p, zone, MenuEditor::find_menu(zone, LIST_MENU), LIST_MENU);
    std::vector<Row> found = custom_rows(editor);
    if (found.empty())
        throw MenuError(std::string(LIST_MENU) + " has no custom map rows (already made dynamic?)");
    MenuEditor usermaps(p, zone, clone(zone, editor.menu), LIST_MENU);
    std::vector<Node *> dropped = usermaps_menu(usermaps, rows);
    custom_maps_row(editor, found);
    add_menu(p, zone, usermaps.menu, USERMAPS_MENU, LIST_MENU, MENU_LIST);
    check_references(zone, dropped);
    add_preview_slot(p, zone, found[0].image);
    std::vector<CustomRow> out;
    for (const Row &r : found)
        out.push_back(CustomRow{r.map, r.title, r.description, r.image});
    return out;
}

bool has_own_usermaps_list(const Zone &zone)
{
    for (const ZoneAsset &a : zone.assets)
        if (a.type == "menu" && a.name == OWN_USERMAPS_MENU)
            return true;
    return false;
}

std::optional<std::string> not_cod_xenon_menu(const Platform &p, Zone &zone)
{
    for (const ZoneAsset &a : zone.assets)
        if (a.type == "menu" && (a.name == USERMAPS_MENU || a.name == OWN_USERMAPS_MENU))
            return "it already has a Custom Maps menu (" + a.name + "): it is a CoD Xe menu";
    std::unique_ptr<MenuEditor> editor;
    try
    {
        editor = std::make_unique<MenuEditor>(p, zone, MenuEditor::find_menu(zone, LIST_MENU), LIST_MENU);
    }
    catch (const MenuError &e)
    {
        return std::string(e.what());
    }
    if (!custom_rows(*editor).empty())
        return std::nullopt;
    for (Node *item : editor->items)
        if (py::starts_with(editor->string(item, "window.name").value_or(""), "codxe_map"))
            return std::string("its map list was already made dynamic (by an older t4ff menu)");
    return "its " + std::string(LIST_MENU) + " has no rows of CoD Xenon's maps";
}

std::vector<fs::path> menu_zone_candidates(const std::vector<fs::path> &paths)
{
    std::vector<fs::path> found;
    std::error_code ec;
    for (const fs::path &path : paths)
    {
        if (fs::is_regular_file(path, ec))
        {
            found.push_back(path);
            continue;
        }
        for (const fs::path &sub : {fs::path(), fs::path("zone"), fs::path("_codxe") / "t4" / "zone", fs::path("t4") / "zone"})
        {
            fs::path candidate = sub.empty() ? path / "patch_ui.ff" : path / sub / "patch_ui.ff";
            if (fs::is_regular_file(candidate, ec) && std::find(found.begin(), found.end(), candidate) == found.end())
                found.push_back(candidate);
        }
    }
    return found;
}

std::string description_text(const std::string &title, const std::string &description)
{
    std::string d(py::strip(description));
    return std::string(py::strip(title)) + "\n" + (d.empty() ? "" : d + "\n");
}
} // namespace t4ff
