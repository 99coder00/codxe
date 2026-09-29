"""A dynamic, scrolling list of the usermaps, opened from the Nazi Zombies menu.

CoD Xenon's ``patch_ui.ff`` lists their maps in the menu ``levels_unlock``: the four stock maps,
then one hand made row per converted map (a backing, a highlight, the A button hint and the button,
which runs ``devmap <map>``) and its preview (picture, name and description) on the right. It
holds 13 custom maps, the screen holds no more.

:func:`make_dynamic` keeps the stock rows and replaces the custom ones by one "Custom Maps" row,
which opens a menu of its own (made from a copy of ``levels_unlock``: its background and Back
button, a title; named ``levels_dev``, a menu of the game it replaces, see ``USERMAPS_MENU``): ``ROWS`` rows whose text and command are dvars, which CoD Xe
fills from the usermaps folder (``src/game/t4/sp/components/usermaps.cpp``), and the preview of the
focused map on the right. The dvars:

- ``ui_codxe_map<k>`` / ``ui_codxe_mapcmd<k>``: the name and the command (``devmap <map>``) of the
  row ``k`` (empty: the row is hidden);
- ``ui_codxe_mapoffset``, ``ui_codxe_mapmore``: the list's position (the scroll catchers above and
  below the rows are there only when there is more to see), ``ui_codxe_maprange`` the counter;
- ``ui_codxe_maptitle``, ``ui_codxe_mapdesc``, ``ui_codxe_mapimage``: the preview of the focused map;
- the menu sets ``ui_codxe_focus`` (the focused row) and ``ui_codxe_scroll`` (rows to scroll: the
  catchers +-1, LB / RB a page), which CoD Xe reads back.

A catcher gives the focus back to its row with ``setfocus``, which does not move the menu's cursor
(the D-pad moves on from the cursor): CoD Xe puts the cursor back on the focused item, and past
the ends of the list (the menu wrapping to the catcher at its other end) goes to the other end.
"""

from __future__ import annotations

import re
import struct
from typing import Dict, List, Optional, Set, Tuple

from .commands import find_field
from .layout import TypeRef
from .zone import Node, Platform, Ptr, Zone

LIST_MENU = "levels_unlock"
ROWS = 13
STOCK_MAPS = ("nazi_zombie_prototype", "nazi_zombie_asylum", "nazi_zombie_sumpf", "nazi_zombie_factory")
FIRST_HIGHLIGHT = 100  # ui_highlight of the dynamic rows (the stock ones use 2 to 5)
PREVIEW = "image_codxe_map"
COUNTER_DX = 280.0  # the counter, right of the rows (250 wide)
OPTIONS_DY = 120.0  # the focused map's options, under its description

# The picture of the focused map when the menu zone has none of its own: CoD Xe copies the map's
# preview.bin (a 512x288 DXT1 texture, tiled for the console) into this image, which the menu shows.
PREVIEW_SLOT = "codxe_map_preview"
PREVIEW_SIZE = (512, 288)
PREVIEW_MAGIC = b"CXPV"
PREVIEW_FILE = "preview.bin"
KEY_LSHLDR, KEY_RSHLDR = 5, 6
ROW_HEIGHT = 19.0  # the items of a map row
# The Custom Maps menu takes the place of a menu of the game nothing opens (its developers' level
# list): the game only has the menus of its ui/menus.txt, a patch replaces some of them (those of
# ui/patch_menus.txt, as CoD Xenon's levels_unlock) but cannot add new ones.
USERMAPS_MENU = "levels_dev"
MENU_LIST = "ui/patch_menus.txt"
USERMAPS_TITLE = "Custom Maps"
TITLE_RECT = (22.0, 32.0, 100.0, 100.0)
TITLE_ALIGN, TITLE_SCALE, TITLE_STYLE = 4, 0.5476, 6

# menu expression operators (operationEnum)
OP_RIGHTPAREN, OP_GREATERTHAN, OP_EQUALS, OP_NOTEQUAL, OP_AND, OP_LEFTPAREN = 1, 10, 12, 13, 14, 16
OP_DVARINT, OP_DVARSTRING = 28, 31

_DEVMAP = re.compile(r'"exec"\s+"devmap\s+([^"\s]+)"')


class MenuError(Exception):
    pass


def _ptr(kind: str, owner: Node, offset: int, node: Optional[Node] = None, index: int = 0, inner: int = 0, slot: Optional[Ptr] = None) -> Ptr:
    ptr = Ptr(kind, node, index, inner, slot)
    ptr.owner = owner
    ptr.offset = offset
    return ptr


def _rebuild_children(node: Node):
    node.children = [p.node for p in node.relocs.values() if p.kind in ("follow", "insert") and p.node is not None]


def _string_node(text: str, template: Node) -> Node:
    node = Node(template.type, 0, template.block)
    node.string = True
    node.data = bytearray(text.encode("latin-1") + b"\0")
    node.count = len(node.data)
    node.segments = [(node.type, node.count, node.count, False)]
    node.extra["align"] = 1
    return node


def clone(node: Node, memo: Optional[Dict[int, Node]] = None) -> Node:
    """A copy of ``node`` and of the nodes it loads; pointers to anything else (aliases to asset
    slots) keep pointing there."""
    memo = {} if memo is None else memo
    new = Node(node.type, node.count, node.block)
    new.data = bytearray(node.data)
    new.push_before, new.push_after = node.push_before, node.push_after
    new.string, new.asset, new.runtime_size = node.string, node.asset, node.runtime_size
    new.segments = list(node.segments)
    new.extra = {k: v for k, v in node.extra.items() if k in ("align", "origin", "delayed")}
    memo[id(node)] = new
    for off, ptr in node.relocs.items():
        if ptr.kind in ("follow", "insert"):
            if ptr.kind == "insert":
                # a nested asset: the copy uses the one the original loads
                new.relocs[off] = _ptr("alias", new, off, slot=ptr, index=1)
                continue
            child = clone(ptr.node, memo)
            new.relocs[off] = _ptr("follow", new, off, child)
            child.extra["ptr"] = new.relocs[off]
        elif ptr.kind == "ref":
            target = memo.get(id(ptr.node), ptr.node)
            new.relocs[off] = _ptr("ref", new, off, target, ptr.index, ptr.inner)
        elif ptr.kind == "alias":
            new.relocs[off] = _ptr("alias", new, off, slot=ptr.slot, index=ptr.index)
        else:
            new.relocs[off] = _ptr("null", new, off)
    _rebuild_children(new)
    return new


class MenuEditor:
    """Reads and edits the items of a console menuDef_t."""

    def __init__(self, p: Platform, zone: Zone, menu_name: str = LIST_MENU, menu: Optional[Node] = None):
        self.p = p
        self.zone = zone
        self.irec = p.record("itemDef_s")
        self.wrec = p.record("windowDef_t")
        self.mrec = p.record("menuDef_t")
        self.window = find_field(self.irec, "window").offset
        if menu is None:
            asset = next((a for a in zone.assets if a.type == "menu" and a.name == menu_name), None)
            if asset is None:
                raise MenuError(f"no menu {menu_name} in this zone")
            menu = asset.ptr.target() if asset.ptr.kind == "alias" else asset.ptr.node
        self.menu = menu
        ptr = self.menu.relocs.get(find_field(self.mrec, "items").offset)
        if ptr is None or ptr.kind != "follow":
            raise MenuError(f"menu {menu_name} has no items")
        self.array = ptr.node
        self.items: List[Node] = list(self.array.children)
        # templates for new nodes
        self.string_template = next(n for n in self.menu.walk() if n.string)
        entries = next(n for n in self.menu.walk() if (n.extra.get("origin") or ("", "", ""))[1:] == ("statement_s", "entries"))
        self.entries_template = entries
        self.entry_template = entries.children[0]

    # -- fields ------------------------------------------------------------

    def offset(self, field: str) -> int:
        if field.startswith("window."):
            return self.window + find_field(self.wrec, field[7:]).offset
        return find_field(self.irec, field).offset

    def string(self, item: Node, field: str) -> Optional[str]:
        ptr = item.relocs.get(self.offset(field))
        target = ptr.target() if ptr is not None and ptr.kind != "null" else None
        return bytes(target.data).rstrip(b"\0").decode("latin-1") if target is not None and target.string else None

    def set_string(self, item: Node, field: str, text: Optional[str]):
        off = self.offset(field)
        if text is None:
            item.relocs[off] = _ptr("null", item, off)
            struct.pack_into(">I", item.data, off, 0)
        else:
            node = _string_node(text, self.string_template)
            item.relocs[off] = _ptr("follow", item, off, node)
            node.extra["ptr"] = item.relocs[off]
            struct.pack_into(">I", item.data, off, 0xFFFFFFFF)
        _rebuild_children(item)

    def set_menu_string(self, field: str, text: Optional[str]):
        """A string of the menu itself (``window.name``, ``onOpen``...)."""
        rec = self.wrec if field.startswith("window.") else self.mrec
        off = find_field(rec, field[7:] if field.startswith("window.") else field).offset
        if field.startswith("window."):
            off += find_field(self.mrec, "window").offset
        if text is None:
            self.menu.relocs[off] = _ptr("null", self.menu, off)
            struct.pack_into(">I", self.menu.data, off, 0)
        else:
            node = _string_node(text, self.string_template)
            self.menu.relocs[off] = _ptr("follow", self.menu, off, node)
            node.extra["ptr"] = self.menu.relocs[off]
            struct.pack_into(">I", self.menu.data, off, 0xFFFFFFFF)
        _rebuild_children(self.menu)

    def key_handlers(self, item: Node) -> List[Node]:
        """The key handlers (execKey) of an item, in order."""
        handlers = []
        ptr = item.relocs.get(self.offset("onKey"))
        while ptr is not None and ptr.kind != "null" and ptr.target() is not None:
            handlers.append(ptr.target())
            ptr = handlers[-1].relocs.get(8)
        return handlers

    def rect(self, item: Node) -> Tuple[float, float, float, float]:
        return struct.unpack_from(">4f", item.data, self.offset("window.rect"))

    def move(self, item: Node, dy: float, dx: float = 0.0):
        for field in ("window.rect", "window.rectClient"):
            for off, delta in ((self.offset(field), dx), (self.offset(field) + 4, dy)):
                struct.pack_into(">f", item.data, off, struct.unpack_from(">f", item.data, off)[0] + delta)

    def expression(self, item: Node, field: str) -> List[Tuple[str, object]]:
        """The tokens of an item's expression: ("op", n), ("int", n), ("float", x), ("str", s)."""
        off = self.offset(field)
        count = struct.unpack_from(">i", item.data, off)[0]
        ptr = item.relocs.get(off + 4)
        array = ptr.node if ptr is not None and ptr.kind == "follow" else None
        tokens = []
        for i in range(count if array is not None else 0):
            entry = array.relocs[4 * i].node
            kind, a, b = struct.unpack_from(">iii", entry.data, 0)
            if kind == 0:
                tokens.append(("op", a))
            elif a == 0:
                tokens.append(("int", b))
            elif a == 1:
                tokens.append(("float", struct.unpack(">f", struct.pack(">i", b))[0]))
            else:
                target = entry.relocs[8].target()
                tokens.append(("str", bytes(target.data).rstrip(b"\0").decode("latin-1")))
        return tokens

    def set_expression(self, item: Node, field: str, tokens: List[Tuple[str, object]]):
        off = self.offset(field)
        struct.pack_into(">i", item.data, off, len(tokens))
        if not tokens:
            item.relocs[off + 4] = _ptr("null", item, off + 4)
            struct.pack_into(">I", item.data, off + 4, 0)
            _rebuild_children(item)
            return
        array = Node(self.entries_template.type, len(tokens), self.entries_template.block)
        array.data = bytearray(b"\xff\xff\xff\xff" * len(tokens))
        array.segments = [(array.type, len(tokens), len(array.data), False)]
        array.extra = {k: v for k, v in self.entries_template.extra.items() if k in ("align", "origin")}
        for i, (kind, value) in enumerate(tokens):
            entry = Node(self.entry_template.type, 1, self.entry_template.block)
            entry.segments = list(self.entry_template.segments)
            entry.extra = {k: v for k, v in self.entry_template.extra.items() if k in ("align", "origin")}
            if kind == "op":
                entry.data = bytearray(struct.pack(">iii", 0, value, 0))
            elif kind == "int":
                entry.data = bytearray(struct.pack(">iii", 1, 0, value))
            elif kind == "float":
                entry.data = bytearray(struct.pack(">iif", 1, 1, value))
            else:
                entry.data = bytearray(struct.pack(">iiI", 1, 2, 0xFFFFFFFF))
                text = _string_node(value, self.string_template)
                entry.relocs[8] = _ptr("follow", entry, 8, text)
                text.extra["ptr"] = entry.relocs[8]
            _rebuild_children(entry)
            array.relocs[4 * i] = _ptr("follow", array, 4 * i, entry)
            entry.extra["ptr"] = array.relocs[4 * i]
        _rebuild_children(array)
        item.relocs[off + 4] = _ptr("follow", item, off + 4, array)
        array.extra["ptr"] = item.relocs[off + 4]
        struct.pack_into(">I", item.data, off + 4, 0xFFFFFFFF)
        _rebuild_children(item)

    def set_items(self, items: List[Node]):
        array = self.array
        array.count = len(items)
        array.data = bytearray(b"\xff\xff\xff\xff" * len(items))
        array.segments = [(array.segments[0][0], len(items), len(array.data), False)]
        array.relocs = {}
        for i, item in enumerate(items):
            array.relocs[4 * i] = _ptr("follow", array, 4 * i, item)
            item.extra["ptr"] = array.relocs[4 * i]
        _rebuild_children(array)
        struct.pack_into(">i", self.menu.data, find_field(self.mrec, "itemCount").offset, len(items))
        self.items = list(items)

    def add_key_handler(self, item: Node, key: int, action: str):
        """Append a key handler (execKey) to an item's chain."""
        off = self.offset("onKey")
        existing = [n for n in self.zone.extra_root.walk() if n.type.name == "ItemKeyHandler"]
        if not existing:
            raise MenuError("no key handler to copy")
        template = existing[0]
        handler = Node(template.type, 1, template.block)
        handler.data = bytearray(struct.pack(">iII", key, 0xFFFFFFFF, 0))
        handler.segments = [(template.type, 1, len(handler.data), False)]
        handler.extra = {k: v for k, v in template.extra.items() if k in ("align", "origin")}
        text = _string_node(action, self.string_template)
        handler.relocs[4] = _ptr("follow", handler, 4, text)
        text.extra["ptr"] = handler.relocs[4]
        handler.relocs[8] = _ptr("null", handler, 8)
        _rebuild_children(handler)
        # the end of the chain
        owner, owner_off = item, off
        while owner.relocs.get(owner_off) is not None and owner.relocs[owner_off].kind == "follow":
            owner, owner_off = owner.relocs[owner_off].node, 8
        owner.relocs[owner_off] = _ptr("follow", owner, owner_off, handler)
        handler.extra["ptr"] = owner.relocs[owner_off]
        struct.pack_into(">I", owner.data, owner_off, 0xFFFFFFFF)
        _rebuild_children(owner)


def _dvar_string(name: str) -> List[Tuple[str, object]]:
    return [("op", OP_DVARSTRING), ("str", name), ("op", OP_RIGHTPAREN)]


def _not_empty(name: str) -> List[Tuple[str, object]]:
    return _dvar_string(name) + [("op", OP_NOTEQUAL), ("str", "")]


def _and(a, b) -> List[Tuple[str, object]]:
    return [("op", OP_LEFTPAREN)] + a + [("op", OP_RIGHTPAREN), ("op", OP_AND), ("op", OP_LEFTPAREN)] + b + [("op", OP_RIGHTPAREN)]


def _replace_int(tokens, old: int, new: int):
    return [("int", new) if t == ("int", old) else t for t in tokens]


def custom_rows(editor: MenuEditor) -> List[dict]:
    """CoD Xenon's rows of converted maps: their items (the row and its preview) and what they show."""
    rows = []
    items = editor.items
    for index, item in enumerate(items):
        action = editor.string(item, "action") or ""
        match = _DEVMAP.search(action)
        if not match or match.group(1).lower() in STOCK_MAPS:
            continue
        name = match.group(1)
        y = editor.rect(item)[1]
        row = [i for i in range(max(0, index - 4), index + 1) if abs(editor.rect(items[i])[1] - y) <= 1.5]
        preview = [i for i, other in enumerate(items) if editor.string(other, "window.name") == f"image_{name}"]
        text = editor.expression(item, "textExp")
        focus = editor.string(item, "onFocus") or ""
        highlight = re.search(r'"setLocalVarInt"\s+"ui_highlight"\s+"(\d+)"', focus)
        image = None
        for i in preview:
            tokens = editor.expression(items[i], "materialExp")
            if len(tokens) == 1 and tokens[0][0] == "str":
                image = tokens[0][1]
        title_key = text[0][1] if len(text) == 1 and text[0][0] == "str" else None
        desc_key = None
        for i in preview:
            tokens = editor.expression(items[i], "textExp")
            if len(tokens) == 1 and tokens[0][0] == "str" and tokens[0][1] != title_key:
                desc_key = tokens[0][1]
        rows.append(
            {
                "map": name,
                "row": row,
                "preview": preview,
                "button": index,
                "y": y,
                "highlight": int(highlight.group(1)) if highlight else None,
                "title": title_key,
                "description": desc_key,
                "image": image,
            }
        )
    return rows


def make_dynamic(p: Platform, zone: Zone, rows: int = ROWS) -> List[dict]:
    """The Nazi Zombies menu keeps the stock maps; CoD Xenon's rows of converted maps become one
    "Custom Maps" row, which opens the Custom Maps menu (``USERMAPS_MENU``): ``rows`` dynamic rows showing the
    maps of the usermaps folder, and the preview of the focused one (see the module). Returns CoD
    Xenon's rows that were there (map, localized name and description keys, preview picture), for
    their description files."""
    editor = MenuEditor(p, zone)
    found = custom_rows(editor)
    if not found:
        raise MenuError(f"{LIST_MENU} has no custom map rows (already made dynamic?)")
    usermaps = MenuEditor(p, zone, menu=clone(editor.menu))
    dropped = usermaps_menu(usermaps, rows)
    custom_maps_row(editor, found)
    add_menu(p, zone, usermaps.menu, USERMAPS_MENU, LIST_MENU, MENU_LIST)
    check_references(zone, dropped)
    add_preview_slot(p, zone, found[0]["image"])
    return found


def not_cod_xenon_menu(p: Platform, zone: Zone) -> Optional[str]:
    """Why ``zone`` is not CoD Xenon's own patch_ui.ff (the menu :func:`make_dynamic` starts from),
    or None when it is."""
    made = [a.name for a in zone.assets if a.type == "menu" and a.name in (USERMAPS_MENU, "codxe_usermaps")]
    if made:
        return f"it already has a Custom Maps menu ({made[0]}): it is a CoD Xe menu"
    try:
        editor = MenuEditor(p, zone)
    except MenuError as e:
        return str(e)
    if custom_rows(editor):
        return None
    if any((editor.string(item, "window.name") or "").startswith("codxe_map") for item in editor.items):
        return "its map list was already made dynamic (by an older t4ff menu)"
    return f"its {LIST_MENU} has no rows of CoD Xenon's maps"


def usermaps_menu(editor: MenuEditor, rows: int = ROWS) -> List[Node]:
    """Make ``editor``'s menu (a copy of ``levels_unlock``) the Custom Maps menu: its frame (background,
    Back), a title, and ``rows`` dynamic rows where the map rows were, with the preview of the
    focused map. Returns the items of the copy left out."""
    items = editor.items
    found = custom_rows(editor)
    template = found[0]
    button = items[template["button"]]
    row_items = [items[i] for i in template["row"]]
    if len(row_items) != 4 or row_items[-1] is not button:
        raise MenuError("unexpected layout of the custom map rows")
    backing, highlight, hint, _ = row_items
    old = template["highlight"]
    preview_items = [items[i] for i in template["preview"]]
    # the frame: the items before the first map row (row items are ROW_HEIGHT high)
    first = next(i for i, item in enumerate(items) if abs(editor.rect(item)[3] - ROW_HEIGHT) < 1.5)
    frame = items[:first]
    y0 = editor.rect(items[first])[1]
    step = 20.0

    def row_copy(item: Node, row: int) -> Node:
        copy = clone(item)
        editor.move(copy, y0 + step * row - template["y"])
        return copy

    new_items: List[Node] = list(frame)

    # the title, in the style of the frame's texts
    text_item = next((item for item in frame if editor.string(item, "text")), None)
    if text_item is not None:
        title = clone(text_item)
        editor.set_string(title, "text", USERMAPS_TITLE)
        editor.set_expression(title, "textExp", [])
        editor.set_expression(title, "visibleExp", [])
        struct.pack_into(">4f", title.data, editor.offset("window.rect"), *TITLE_RECT)
        struct.pack_into(">4f", title.data, editor.offset("window.rectClient"), *TITLE_RECT)
        struct.pack_into(">ii", title.data, editor.offset("window.rect") + 16, 1, 1)
        struct.pack_into(">ii", title.data, editor.offset("window.rectClient") + 16, 1, 1)
        struct.pack_into(">i", title.data, editor.offset("textAlignMode"), TITLE_ALIGN)
        struct.pack_into(">f", title.data, editor.offset("textscale"), TITLE_SCALE)
        struct.pack_into(">i", title.data, editor.offset("textStyle"), TITLE_STYLE)
        new_items.append(title)

    visible_language = editor.expression(backing, "visibleExp")

    # the catcher above the rows: scrolls up when the first row is left upwards
    up = row_copy(button, 0)
    editor.set_string(up, "window.name", "codxe_map_up")
    editor.set_string(up, "action", None)
    editor.set_string(up, "leaveFocus", None)
    editor.set_string(up, "onFocus", '"setdvar" "ui_codxe_scroll" "-1" ; "setfocus" "codxe_map0" ; ')
    editor.set_expression(up, "textExp", [])
    editor.set_expression(up, "visibleExp", [("op", OP_DVARINT), ("str", "ui_codxe_mapoffset"), ("op", OP_RIGHTPAREN), ("op", OP_GREATERTHAN), ("int", 0)])
    new_items.append(up)

    for row in range(rows):
        dvar = f"ui_codxe_map{row}"
        shown = _and(visible_language, _not_empty(dvar)) if visible_language else _not_empty(dvar)
        b = row_copy(backing, row)
        editor.set_expression(b, "visibleExp", shown)
        h = row_copy(highlight, row)
        editor.set_expression(h, "visibleExp", _replace_int(editor.expression(highlight, "visibleExp"), old, FIRST_HIGHLIGHT + row))
        a = row_copy(hint, row)
        editor.set_expression(a, "visibleExp", _replace_int(editor.expression(hint, "visibleExp"), old, FIRST_HIGHLIGHT + row))
        btn = row_copy(button, row)
        editor.set_string(btn, "window.name", f"codxe_map{row}")
        action = _DEVMAP.sub(f'"exec" "vstr ui_codxe_mapcmd{row}"', editor.string(button, "action"))
        editor.set_string(btn, "action", action)
        focus = editor.string(button, "onFocus")
        focus = re.sub(r'("setLocalVarInt"\s+"ui_highlight"\s+)"\d+"', rf'\1"{FIRST_HIGHLIGHT + row}"', focus)
        focus = re.sub(r'"show"\s+"image_[^"]*"\s*;', f'"show" "{PREVIEW}" ; "setdvar" "ui_codxe_focus" "{row}" ;', focus)
        editor.set_string(btn, "onFocus", focus)
        editor.set_expression(btn, "visibleExp", shown)
        editor.set_expression(btn, "textExp", _dvar_string(dvar))
        new_items += [b, h, a, btn]

    # the catcher below the rows: scrolls down when the last row is left downwards
    down = row_copy(button, rows - 1)
    editor.set_string(down, "window.name", "codxe_map_down")
    editor.set_string(down, "action", None)
    editor.set_string(down, "leaveFocus", None)
    editor.set_string(down, "onFocus", f'"setdvar" "ui_codxe_scroll" "1" ; "setfocus" "codxe_map{rows - 1}" ; ')
    editor.set_expression(down, "textExp", [])
    editor.set_expression(down, "visibleExp", [("op", OP_DVARINT), ("str", "ui_codxe_mapmore"), ("op", OP_RIGHTPAREN), ("op", OP_EQUALS), ("int", 1)])
    new_items.append(down)

    # the position in the list ("14-26 / 40"), right of the last row
    counter = row_copy(hint, rows - 1)
    editor.move(counter, 0.0, COUNTER_DX)
    editor.set_string(counter, "text", None)
    editor.set_expression(counter, "textExp", _dvar_string("ui_codxe_maprange"))
    editor.set_expression(counter, "visibleExp", _not_empty("ui_codxe_maprange"))
    new_items.append(counter)

    # the preview of the focused map: its picture, name and description
    for item in preview_items:
        copy = clone(item)
        editor.set_string(copy, "window.name", PREVIEW)
        if editor.expression(item, "materialExp"):
            editor.set_expression(copy, "materialExp", _dvar_string("ui_codxe_mapimage"))
            editor.set_expression(copy, "visibleExp", _not_empty("ui_codxe_mapimage"))
        else:
            tokens = editor.expression(item, "textExp")
            is_title = len(tokens) == 1 and tokens[0] == ("str", template["title"])
            editor.set_expression(copy, "textExp", _dvar_string("ui_codxe_maptitle" if is_title else "ui_codxe_mapdesc"))
            if not is_title:
                # the focused map's options (its options.txt: "Difficulty: Default (X)"), under its description
                options = clone(copy)
                editor.move(options, OPTIONS_DY)
                editor.set_expression(options, "textExp", _dvar_string("ui_codxe_mapoptions"))
                editor.set_expression(options, "visibleExp", _not_empty("ui_codxe_mapoptions"))
                new_items.append(copy)
                copy = options
        new_items.append(copy)

    dropped = [item for item in items if item not in new_items]
    editor.set_items(new_items)

    # the menu: its own name, the first row focused; Back (B) closes it, LB / RB: a page up / down
    editor.set_menu_string("window.name", USERMAPS_MENU)
    editor.set_menu_string("onOpen", '"setfocus" "codxe_map0" ; ')
    editor.set_menu_string("onClose", None)
    for item in frame:
        for handler in editor.key_handlers(item):
            action = _text_of(handler, 4)
            if action and LIST_MENU in action:
                text = _string_node(action.replace(f'"{LIST_MENU}"', f'"{USERMAPS_MENU}"'), editor.string_template)
                handler.relocs[4] = _ptr("follow", handler, 4, text)
                text.extra["ptr"] = handler.relocs[4]
                _rebuild_children(handler)
    key_owner = next((item for item in frame if editor.key_handlers(item)), new_items[0])
    editor.add_key_handler(key_owner, KEY_LSHLDR, f'"setdvar" "ui_codxe_scroll" "-{rows}" ; ')
    editor.add_key_handler(key_owner, KEY_RSHLDR, f'"setdvar" "ui_codxe_scroll" "{rows}" ; ')
    # X / Y: the focused map's next choice of its first / second option (CoD Xe changes it)
    editor.add_key_handler(key_owner, KEY_BUTTON_X, '"setdvar" "ui_codxe_option" "1" ; ')
    editor.add_key_handler(key_owner, KEY_BUTTON_Y, '"setdvar" "ui_codxe_option" "2" ; ')
    return dropped


def custom_maps_row(editor: MenuEditor, found: List[dict]):
    """Replace CoD Xenon's rows of converted maps (``found``) and their previews in the Nazi Zombies
    menu by one "Custom Maps" row opening the Custom Maps menu."""
    items = editor.items
    template = found[0]
    button = items[template["button"]]
    row = [clone(items[i]) for i in template["row"]]
    new_button = row[-1]
    editor.set_string(new_button, "action", f'"play" "mouse_click" ; "open" "{USERMAPS_MENU}" ; ')
    editor.set_expression(new_button, "textExp", [])
    editor.set_string(new_button, "text", USERMAPS_TITLE)
    # no map picture while it is focused
    focus = re.sub(r'"show"\s+"image_[^"]*"\s*;\s*', "", editor.string(button, "onFocus") or "")
    editor.set_string(new_button, "onFocus", focus)
    removed = {i for r in found for i in r["row"] + r["preview"]}
    first = min(removed)
    kept = [item for i, item in enumerate(items) if i not in removed]
    position = sum(1 for i in range(first) if i not in removed)
    editor.set_items(kept[:position] + row + kept[position:])
    check_references(editor.zone, [items[i] for i in removed])


def add_menu(p: Platform, zone: Zone, menu: Node, name: str, after: str, menu_list: str):
    """Add ``menu`` to the zone as the asset right after the menu ``after``, and to the menu list
    ``menu_list`` (the game opens the menus of the lists it loads)."""
    from .zone import ZoneAsset

    assets = zone.assets_node
    index = next((i for i, a in enumerate(zone.assets) if a.type == "menu" and a.name == after), None)
    lists = [i for i, a in enumerate(zone.assets) if a.type == "menulist" and a.name == menu_list]
    if index is None or not lists or lists[0] <= index:
        raise MenuError(f"no menu {after} before the menu list {menu_list} in this zone")
    ptr = _ptr("insert", assets, 0, menu)
    menu.insert = True
    menu.extra["ptr"] = ptr
    entries = [(bytes(assets.data[8 * i : 8 * i + 8]), assets.relocs.get(8 * i + 4)) for i in range(len(zone.assets))]
    entries.insert(index + 1, (struct.pack(">iI", p.asset_type_index["menu"], 0xFFFFFFFE), ptr))
    assets.data = bytearray()
    assets.relocs = {}
    for i, (entry, slot) in enumerate(entries):
        assets.data += entry
        if slot is not None:
            slot.offset = 8 * i + 4
            assets.relocs[8 * i + 4] = slot
    assets.count = 2 * len(entries)
    assets.segments = [(assets.segments[0][0], assets.count, len(assets.data), False)]
    _rebuild_children(assets)
    zone.assets.insert(index + 1, ZoneAsset("menu", ptr, name))

    # the menu list: one more menu
    rec = p.record("MenuList")
    listed = assets.relocs[8 * (lists[0] + 1) + 4].node
    array = listed.relocs[find_field(rec, "menus").offset].node
    offset = len(array.data)
    array.data += b"\0\0\0\0"
    array.count += 1
    array.segments = [(array.segments[0][0], array.count, len(array.data), False)]
    array.relocs[offset] = _ptr("alias", array, offset, slot=ptr, index=1)
    struct.pack_into(">i", listed.data, find_field(rec, "menuCount").offset, array.count)


def preview_texture(rgba):
    """A picture as the preview slot's texture: 512x288 DXT1, one level, tiled."""
    import numpy as np

    from . import images as img
    from .loadscreen import resize

    width, height = PREVIEW_SIZE
    picture = np.array(resize(rgba, width, height), dtype=np.uint8, copy=True)
    picture[:, :, 3] = 255
    bgra = picture[:, :, [2, 1, 0, 3]].tobytes()
    return img.build_console_texture(img.ImageData(PREVIEW_SLOT, "A8R8G8B8", width, height, [bgra]), keep_mips=False)


def preview_file(rgba) -> bytes:
    """preview.bin: "CXPV", version, width, height, 0, GPU texture format, size (big endian), then the
    texture as the console holds it, which CoD Xe copies as is into the preview slot."""
    tex = preview_texture(rgba)
    return PREVIEW_MAGIC + struct.pack(">HHHHII", 1, tex.width, tex.height, 0, tex.format.gpu, len(tex.pixels)) + tex.pixels


def add_preview_slot(p: Platform, zone: Zone, template_material: Optional[str]):
    """Add the material and image ``codxe_map_preview`` (a copy of a preview material of CoD
    Xenon's, with a black 512x288 image of its own) at the end of the zone."""
    import types

    import numpy as np

    from .assets import build_console_image
    from .zone import ZoneAsset

    material = next((a for a in zone.assets if a.type == "material" and a.name == template_material), None)
    if material is None:
        raise MenuError(f"no preview material {template_material!r} to copy")
    source = material.ptr.target() if material.ptr.kind == "alias" else material.ptr.node
    copy = clone(source)
    rec = p.record("Material")
    name_off = find_field(rec, "info").offset + find_field(p.record("MaterialInfo"), "name").offset
    name = _string_node(PREVIEW_SLOT, next(n for n in source.walk() if n.string))
    copy.relocs[name_off] = _ptr("follow", copy, name_off, name)
    name.extra["ptr"] = copy.relocs[name_off]
    _rebuild_children(copy)

    tex = preview_texture(np.zeros((PREVIEW_SIZE[1], PREVIEW_SIZE[0], 4), dtype=np.uint8))
    image = build_console_image(types.SimpleNamespace(dst=p), PREVIEW_SLOT, tex, 0, 3, 0)
    table = copy.relocs[find_field(rec, "textureTable").offset].node
    image_off = find_field(p.record("MaterialTextureDef"), "u").offset
    image.insert = True
    table.relocs[image_off] = _ptr("insert", table, image_off, image)
    image.extra["ptr"] = table.relocs[image_off]
    struct.pack_into(">I", table.data, image_off, 0xFFFFFFFE)
    _rebuild_children(table)

    # the zone's asset list: one more entry, loading the material (and its image)
    assets = zone.assets_node
    offset = len(assets.data)
    assets.data += struct.pack(">iI", p.asset_type_index["material"], 0xFFFFFFFE)
    assets.count += 2
    assets.segments = [(assets.segments[0][0], assets.count, len(assets.data), False)]
    ptr = _ptr("insert", assets, offset + 4, copy)
    copy.insert = True
    copy.extra["ptr"] = ptr
    assets.relocs[offset + 4] = ptr
    _rebuild_children(assets)
    zone.assets.append(ZoneAsset("material", ptr, PREVIEW_SLOT))


def localized_strings(p: Platform, zone: Zone) -> Dict[str, str]:
    """The localized strings of a zone (key without "@" -> text)."""
    rec = p.record("LocalizeEntry")
    value_off, name_off = find_field(rec, "value").offset, find_field(rec, "name").offset
    out = {}
    for node in zone.extra_root.walk():
        if node.type.name != "LocalizeEntry":
            continue
        texts = []
        for off in (value_off, name_off):
            ptr = node.relocs.get(off)
            target = ptr.target() if ptr is not None and ptr.kind != "null" else None
            texts.append(bytes(target.data).rstrip(b"\0").decode("latin-1") if target is not None else "")
        if texts[1]:
            out[texts[1]] = texts[0]
    return out


def description_text(title: str, description: str = "") -> str:
    """The content of a map's description.txt: its name, then its description."""
    return title.strip() + "\n" + (description.strip() + "\n" if description.strip() else "")


def check_references(zone: Zone, removed: List[Node]):
    """Nothing left in the zone refers to the removed items."""
    gone = {id(n) for item in removed for n in item.walk()}
    for node in zone.extra_root.walk():
        if id(node) in gone:
            raise MenuError(f"a removed item is still in the zone: {node!r}")
        for ptr in node.relocs.values():
            if (ptr.kind == "ref" and id(ptr.node) in gone) or (ptr.kind == "alias" and ptr.slot is not None and id(ptr.slot.owner) in gone):
                raise MenuError(f"{node!r} refers to a removed item")


# ---------------------------------------------------------------------------
# PC script menus on a controller

# The console's gamepad keys (keyNum_t), which its own menus bind in key handlers (patch_ui.ff:
# B 2, X 3, Y 4, LB 5, RB 6, the D-pad left and right 22 and 23).
KEY_BUTTON_A, KEY_BUTTON_B, KEY_BUTTON_X, KEY_BUTTON_Y = 1, 2, 3, 4
KEY_DPAD_UP, KEY_DPAD_DOWN, KEY_DPAD_LEFT, KEY_DPAD_RIGHT = 20, 21, 22, 23
# the button a number key of a PC script menu becomes, and its name in the menu's labels
GAMEPAD_FOR_DIGIT = {
    "1": (KEY_BUTTON_A, "A"),
    "2": (KEY_BUTTON_X, "X"),
    "3": (KEY_BUTTON_Y, "Y"),
    "4": (KEY_LSHLDR, "LB"),
    "5": (KEY_RSHLDR, "RB"),
    "6": (KEY_DPAD_UP, "Up"),
    "7": (KEY_DPAD_DOWN, "Down"),
    "8": (KEY_DPAD_LEFT, "Left"),
    "9": (KEY_DPAD_RIGHT, "Right"),
}
# the number of an entry, color codes around it: "^11: ^4Beauty Of Annihilation"
_DIGIT_LABEL = re.compile(r"^((?:\^\d)?\s*)([1-9])(?=\s*(?:\^\d)?\s*[:.)\-])")
_ESC_LABEL = re.compile(r"\b(?:ESC|Esc|ESCAPE|Escape)\b")


def _text_of(node: Node, off: int) -> Optional[str]:
    ptr = node.relocs.get(off)
    target = ptr.target() if ptr is not None and ptr.kind != "null" else None
    return bytes(target.data).rstrip(b"\0").decode("latin-1") if target is not None and target.string else None


def gamepad_script_menus(p: Platform, zone: Zone, log=print) -> List[str]:
    """Give PC script menus (a music box...) controller buttons.

    They answer the number keys (``execKey "1"``) and Escape, which a controller does not have. The
    action of each number key also goes to a button, as the console's own menus bind theirs (key
    handlers of the menu): 1 A, 2 X, 3 Y, 4 LB, 5 RB, 6 to 9 the D-pad up, down, left and right, and
    the menu's Escape action to B. The labels naming the keys ("^11: ...", "Press ESC to close")
    name the buttons. Returns the names of the menus changed.
    """
    mrec, irec, krec = p.record("menuDef_t"), p.record("itemDef_s"), p.record("ItemKeyHandler")
    on_key, on_esc = find_field(mrec, "onKey").offset, find_field(mrec, "onESC").offset
    items_off, count_off = find_field(mrec, "items").offset, find_field(mrec, "itemCount").offset
    name_off = find_field(mrec, "window").offset + find_field(p.record("windowDef_t"), "name").offset
    text_off = find_field(irec, "text").offset
    key_off, action_off, next_off = (find_field(krec, f).offset for f in ("key", "action", "next"))

    def append_handler(chain: List[Node], key: int, action: str, string_template: Node):
        template = chain[-1]
        handler = Node(template.type, 1, template.block)
        handler.data = bytearray(len(template.data))
        struct.pack_into(">i", handler.data, key_off, key)
        struct.pack_into(">I", handler.data, action_off, 0xFFFFFFFF)
        handler.segments = list(template.segments)
        handler.extra = {k: v for k, v in template.extra.items() if k in ("align", "origin")}
        text = _string_node(action, string_template)
        handler.relocs[action_off] = _ptr("follow", handler, action_off, text)
        text.extra["ptr"] = handler.relocs[action_off]
        handler.relocs[next_off] = _ptr("null", handler, next_off)
        _rebuild_children(handler)
        template.relocs[next_off] = _ptr("follow", template, next_off, handler)
        handler.extra["ptr"] = template.relocs[next_off]
        struct.pack_into(">I", template.data, next_off, 0xFFFFFFFF)
        _rebuild_children(template)
        chain.append(handler)

    changed = []
    labels: List[Tuple[Node, str]] = []  # (string node, new text)
    for asset in zone.assets:
        if asset.type != "menulist" or asset.ptr is None:
            continue
        root = asset.ptr.target()
        if root is None:
            continue
        for menu in root.walk():
            if menu.type.name != "menuDef_t":
                continue
            chain: List[Node] = []
            ptr = menu.relocs.get(on_key)
            while ptr is not None and ptr.kind != "null" and ptr.target() is not None:
                chain.append(ptr.target())
                ptr = chain[-1].relocs.get(next_off)
            keys = {struct.unpack_from(">i", h.data, key_off)[0]: h for h in chain}
            digits = {chr(k): h for k, h in keys.items() if ord("1") <= k <= ord("9")}
            if not digits:
                continue
            string_template = next(n for n in menu.walk() if n.string)
            names: Dict[str, str] = {}  # digit -> button now doing its action
            for digit, handler in sorted(digits.items()):
                key, button = GAMEPAD_FOR_DIGIT[digit]
                action = _text_of(handler, action_off)
                if key not in keys and action is not None:
                    append_handler(chain, key, action, string_template)
                    names[digit] = button
            escape = _text_of(menu, on_esc)
            back = escape is not None and KEY_BUTTON_B not in keys
            if back:
                append_handler(chain, KEY_BUTTON_B, escape, string_template)
            elif not names:
                continue
            ptr = menu.relocs.get(items_off)
            array = ptr.target() if ptr is not None and ptr.kind != "null" else None
            count = struct.unpack_from(">i", menu.data, count_off)[0]
            for i in range(count if array is not None else 0):
                item_ptr = array.relocs.get(4 * i)
                item = item_ptr.target() if item_ptr is not None and item_ptr.kind != "null" else None
                text_ptr = item.relocs.get(text_off) if item is not None else None
                text = _text_of(item, text_off) if item is not None else None
                if text is None or text_ptr.kind != "follow":
                    continue
                new = _DIGIT_LABEL.sub(lambda m: m.group(1) + names[m.group(2)] if m.group(2) in names else m.group(0), text)
                if back:
                    new = _ESC_LABEL.sub("B", new)
                if new != text:
                    labels.append((text_ptr.node, new))
            name = _text_of(menu, name_off) or asset.name
            buttons = [f"{names[d]} ({d})" for d in sorted(names)] + (["B (Escape)"] if back else [])
            log(f"menu {name}: controller buttons for its keys: {', '.join(buttons)}")
            changed.append(name)
    _set_label_texts(zone, labels, log)
    return changed


def _set_label_texts(zone: Zone, labels: List[Tuple[Node, str]], log=print):
    """Change the text of label strings in place. A string another pointer refers into (the PC linker
    stores a string ending another one only once) keeps its text."""
    if not labels:
        return
    targets = {id(node) for node, _ in labels}
    shared = set()
    for node in zone.extra_root.walk():
        for ptr in node.relocs.values():
            if ptr.kind == "ref" and id(ptr.node) in targets and (ptr.index or ptr.inner):
                shared.add(id(ptr.node))
    for node, text in labels:
        if id(node) in shared:
            log(f'menu label "{bytes(node.data).rstrip(bytes(1)).decode("latin-1")}" left as it is (shared with another string)')
            continue
        node.data = bytearray(text.encode("latin-1") + b"\0")
        node.count = len(node.data)
        node.segments = [(node.type, node.count, node.count, False)]


# A controller moves the focus from item to item (the D-pad), and the console focuses any item that
# is not a decoration (staticFlags WINDOW_DECORATION), then acts on the focused one (A). PC menus made
# for the mouse can leave their backgrounds without "decoration": the click still reaches the button
# over them. Mini-Labor's weapon choice puts a background under each button, its ACCEPT's at the
# same place and before it, so on the console the focus stopped on backgrounds and ACCEPT could not
# be pressed: the game waited for it forever. Items that do nothing (no action, no handler, no dvar)
# are decorations, as the menus of the game mark theirs; they are drawn as before.
WINDOW_DECORATION = 0x00100000
_INERT_TYPES = (0, 1)  # ITEM_TYPE_TEXT, ITEM_TYPE_BUTTON
_ITEM_HANDLERS = ("mouseEnterText", "mouseExitText", "mouseEnter", "mouseExit", "action", "onAccept", "onFocus",
                  "leaveFocus", "dvar", "dvarTest", "onListboxSelectionChange", "onKey", "enableDvar")  # fmt: skip


def decorate_inert_items(p: Platform, zone: Zone, log=print, script_menus: Optional[Set[str]] = None) -> Dict[str, int]:
    """Items of the zone's menus that do nothing become decorations (see above); with
    ``script_menus`` (lowercase names the map's scripts use), only those menus. Returns {menu name:
    items changed}."""
    mrec, irec, wrec = p.record("menuDef_t"), p.record("itemDef_s"), p.record("windowDef_t")
    name_off = find_field(mrec, "window").offset + find_field(wrec, "name").offset
    items_off, count_off = find_field(mrec, "items").offset, find_field(mrec, "itemCount").offset
    flags_off = find_field(irec, "window").offset + find_field(wrec, "staticFlags").offset
    type_off = find_field(irec, "type").offset
    handlers = [find_field(irec, f).offset for f in _ITEM_HANDLERS if find_field(irec, f) is not None]
    changed: Dict[str, int] = {}
    for menu in zone.extra_root.walk():
        if menu.type.name != "menuDef_t":
            continue
        ptr = menu.relocs.get(items_off)
        array = ptr.target() if ptr is not None and ptr.kind != "null" else None
        count = p.u32.unpack_from(menu.data, count_off)[0] if array is not None else 0
        focusable, inert = 0, []
        for i in range(count):
            item_ptr = array.relocs.get(4 * i)
            item = item_ptr.target() if item_ptr is not None and item_ptr.kind != "null" else None
            if item is None or len(item.data) < flags_off + 4:
                continue
            flags = p.u32.unpack_from(item.data, flags_off)[0]
            if flags & WINDOW_DECORATION:
                continue
            if any(off in item.relocs and item.relocs[off].kind != "null" for off in handlers):
                focusable += 1
            elif p.u32.unpack_from(item.data, type_off)[0] in _INERT_TYPES:
                inert.append((item, flags))
        if not inert or not focusable:
            continue  # nothing to reach, nothing in the way
        if script_menus is not None and (_text_of(menu, name_off) or "").lower() not in script_menus:
            continue
        for item, flags in inert:
            p.u32.pack_into(item.data, flags_off, flags | WINDOW_DECORATION)
        name = _text_of(menu, name_off) or "?"
        changed[name] = len(inert)
    if changed:
        log(f"menus: items that do nothing are decorations, which the controller's focus skips "
            f"({', '.join(f'{name} {n}' for name, n in sorted(changed.items()))})")
    return changed


# The D-pad moves the focus through a menu's items in their order: up and left to the previous one
# (left only when it is on the same row), down and right to the next (the disc's menu key handler,
# see HANDOFF.md). In a grid of buttons (Mini-Labor's weapon choice, 3 by 3 under an ACCEPT) down
# only moves at the end of a row and up reaches ACCEPT from the first button only. A focused item's
# own key handlers come first (then the default moves): every button of the map's menus gets one per
# direction, D-pad and stick, giving the focus to the nearest button that way ("setfocus"), or
# keeping it. Buttons without a name get one ("t4ff_focus_<n>") for "setfocus" to find them.
KEY_APAD_UP, KEY_APAD_DOWN, KEY_APAD_LEFT, KEY_APAD_RIGHT = 28, 29, 30, 31
_DIRECTIONS = (
    ((0.0, -1.0), (KEY_DPAD_UP, KEY_APAD_UP)),
    ((0.0, 1.0), (KEY_DPAD_DOWN, KEY_APAD_DOWN)),
    ((-1.0, 0.0), (KEY_DPAD_LEFT, KEY_APAD_LEFT)),
    ((1.0, 0.0), (KEY_DPAD_RIGHT, KEY_APAD_RIGHT)),
)
# A menu whose buttons choose a value it shows (they set a local variable: the weapon a frame is
# around) and send it to the scripts, with one other button confirming (ACCEPT): the focus shows
# nothing, so the choice follows the focus (the choice's action becomes its onFocus), and A on a
# choice confirms it (its action becomes the confirming button's). Not both on A: a script waiting
# for menu responses in a loop gets one per frame, the second is lost.
_NUMBERS = re.compile(r"\d+")


def _sorted_children(node: Node):
    """A structure's pointed data loads in the order of its fields."""
    node.children = [ptr.node for _, ptr in sorted(node.relocs.items()) if ptr.kind in ("follow", "insert") and ptr.node is not None]


def _set_string(owner: Node, off: int, text: str, template: Node):
    string = _string_node(text, template)
    owner.relocs[off] = _ptr("follow", owner, off, string)
    string.extra["ptr"] = owner.relocs[off]
    struct.pack_into(">I", owner.data, off, 0xFFFFFFFF)
    _sorted_children(owner)


def _screen_rect(item: Node, rect_off: int) -> Tuple[float, float, float, float]:
    x, y, w, h = struct.unpack_from(">4f", item.data, rect_off)
    horz, vert = struct.unpack_from(">2i", item.data, rect_off + 16)
    # the alignments place the rectangle on a 640 by 480 screen (center 2, right / bottom 3)
    x += {2: 320.0, 3: 640.0}.get(horz, 0.0)
    y += {2: 240.0, 3: 480.0}.get(vert, 0.0)
    return x, y, w, h


def _nearest(centers: List[Tuple[float, float]], i: int, direction: Tuple[float, float]) -> int:
    """The item nearest to ``i`` that way: within 45 degrees of it first, then anywhere that side."""
    dx, dy = direction
    best, best_score = i, None
    for j, (x, y) in enumerate(centers):
        vx, vy = x - centers[i][0], y - centers[i][1]
        along = vx * dx + vy * dy
        if j == i or along <= 0.5:
            continue
        across = abs(vx * dy - vy * dx)
        score = along + 2 * across + (0 if across <= along else 100000)
        if best_score is None or score < best_score:
            best, best_score = j, score
    return best


def controller_navigation(p: Platform, zone: Zone, log=print, script_menus: Optional[Set[str]] = None) -> Dict[str, int]:
    """Buttons of the zone's menus move the focus to the nearest button in the direction pressed,
    and choices confirmed by a button follow the focus (see above); with ``script_menus``
    (lowercase names the map's scripts use), only those menus. Returns {menu name: buttons}."""
    mrec, irec, wrec, krec = p.record("menuDef_t"), p.record("itemDef_s"), p.record("windowDef_t"), p.record("ItemKeyHandler")
    menu_name = find_field(mrec, "window").offset + find_field(wrec, "name").offset
    items_off, count_off = find_field(mrec, "items").offset, find_field(mrec, "itemCount").offset
    window = find_field(irec, "window").offset
    name_off, flags_off = window + find_field(wrec, "name").offset, window + find_field(wrec, "staticFlags").offset
    rect_off = window + find_field(wrec, "rect").offset
    action_off, focus_off, on_key = (find_field(irec, f).offset for f in ("action", "onFocus", "onKey"))
    key_off, key_action, key_next = (find_field(krec, f).offset for f in ("key", "action", "next"))
    # strings other pointers refer into (the PC linker shares them) stay where they are
    shared = {id(ptr.node) for node in zone.extra_root.walk() for ptr in node.relocs.values() if ptr.kind in ("ref", "alias") and ptr.node is not None}
    changed: Dict[str, int] = {}
    for menu in list(zone.extra_root.walk()):
        if menu.type.name != "menuDef_t":
            continue
        if script_menus is not None and (_text_of(menu, menu_name) or "").lower() not in script_menus:
            continue
        ptr = menu.relocs.get(items_off)
        array = ptr.target() if ptr is not None and ptr.kind != "null" else None
        count = p.u32.unpack_from(menu.data, count_off)[0] if array is not None else 0
        buttons = []
        for i in range(count):
            item_ptr = array.relocs.get(4 * i)
            item = item_ptr.target() if item_ptr is not None and item_ptr.kind != "null" else None
            if item is None or len(item.data) < on_key + 4 or p.u32.unpack_from(item.data, flags_off)[0] & WINDOW_DECORATION:
                continue
            if _text_of(item, action_off) is not None:
                buttons.append(item)
        names = [_text_of(item, name_off) for item in buttons]
        named = [n for n in names if n]
        if len(buttons) < 2 or len(set(named)) != len(named):
            continue  # nothing to move between, or names "setfocus" cannot tell apart
        template = next(n for n in menu.walk() if n.string)
        taken = set(named)
        for i, item in enumerate(buttons):
            if not names[i]:
                n = i
                while f"t4ff_focus_{n}" in taken:
                    n += 1
                names[i] = f"t4ff_focus_{n}"
                taken.add(names[i])
                _set_string(item, name_off, names[i], template)
        centers = []
        for item in buttons:
            x, y, w, h = _screen_rect(item, rect_off)
            centers.append((x + w / 2, y + h / 2))
        for i, item in enumerate(buttons):
            # the item's key handlers: those it has keep their keys
            chain: List[Node] = []
            ptr = item.relocs.get(on_key)
            while ptr is not None and ptr.kind != "null" and ptr.target() is not None:
                chain.append(ptr.target())
                ptr = chain[-1].relocs.get(key_next)
            bound = {struct.unpack_from(">i", h.data, key_off)[0] for h in chain}
            for direction, keys in _DIRECTIONS:
                target = names[_nearest(centers, i, direction)]
                for key in keys:
                    if key in bound:
                        continue
                    handler = Node(TypeRef("record", "ItemKeyHandler", krec.size), 1, item.block)
                    handler.data = bytearray(krec.size)
                    handler.extra["align"] = 4
                    struct.pack_into(">i", handler.data, key_off, key)
                    _set_string(handler, key_action, f'"setfocus" "{target}" ; ', template)
                    handler.relocs[key_next] = _ptr("null", handler, key_next)
                    owner, off = (chain[-1], key_next) if chain else (item, on_key)
                    owner.relocs[off] = _ptr("follow", owner, off, handler)
                    handler.extra["ptr"] = owner.relocs[off]
                    struct.pack_into(">I", owner.data, off, 0xFFFFFFFF)
                    _sorted_children(owner)
                    chain.append(handler)
        # choices confirmed by a button: the choice follows the focus, A confirms it too
        actions = [_text_of(item, action_off) for item in buttons]
        groups: Dict[str, List[int]] = {}
        for i, action in enumerate(actions):
            if "scriptmenuresponse" in action.lower() and "setlocalvar" in action.lower():
                groups.setdefault(_NUMBERS.sub("#", action), []).append(i)
        choices = max(groups.values(), key=len) if groups else []
        confirms = [i for i, action in enumerate(actions) if i not in choices and "scriptmenuresponse" in action.lower()]
        if any(id(buttons[i].relocs[action_off].node) in shared or _text_of(buttons[i], focus_off) is not None for i in choices):
            choices = []  # a shared string, or choices that already do something on focus
        if len(choices) >= 2 and len(confirms) == 1:
            for i in choices:
                _set_string(buttons[i], focus_off, actions[i], template)
                _set_string(buttons[i], action_off, actions[confirms[0]], template)
        changed[_text_of(menu, menu_name) or "?"] = len(buttons)
    if changed:
        log(f"menus: the D-pad moves between buttons as they are laid out ({', '.join(f'{n} {c}' for n, c in sorted(changed.items()))})")
    return changed


# Options a mod chooses in its own front end menus: a multiple choice item bound to a dvar the map's
# scripts read (PhilMod's difficulty: philmod_gamemode, Easy 0 to Overkill 4, main menu default 2).
# The console shows its own menus, so they go to the map's options.txt, which CoD Xe's Custom Maps
# menu shows for the focused map (X / Y change them) and sets before loading the map:
#   option <dvar> <default value> <label>
#   choice <value> <name>
ITEM_TYPE_TEXT, ITEM_TYPE_MULTI = 0, 12
OPTIONS_FILE = "options.txt"


def _format_value(value: float) -> str:
    return str(int(value)) if value == int(value) else f"{value:g}"


def menu_options(p: Platform, zones: List[Zone], read_dvars: Set[str], menu_values: Dict[str, Set[str]]) -> List[dict]:
    """The options of the PC ``zones``' menus (see above) whose dvars the map's scripts read
    (``read_dvars``, lowercase): [{dvar, label, default, choices: [(value, name)]}]."""
    mrec, irec, wrec, mdef = p.record("menuDef_t"), p.record("itemDef_s"), p.record("windowDef_t"), p.record("multiDef_s")
    items_off, count_off = find_field(mrec, "items").offset, find_field(mrec, "itemCount").offset
    window = find_field(irec, "window").offset
    rect_off = window + find_field(wrec, "rect").offset
    type_off, text_off, dvar_off, data_off = (find_field(irec, f).offset for f in ("type", "text", "dvar", "typeData"))
    names_off, strs_off, values_off = (find_field(mdef, f).offset for f in ("dvarList", "dvarStr", "dvarValue"))
    count_off_m, strdef_off = find_field(mdef, "count").offset, find_field(mdef, "strDef").offset
    options: Dict[str, dict] = {}
    labels: Dict[str, str] = {}
    for zone in zones:
        for menu in zone.extra_root.walk():
            if menu.type.name != "menuDef_t":
                continue
            ptr = menu.relocs.get(items_off)
            array = ptr.target() if ptr is not None and ptr.kind != "null" else None
            count = p.u32.unpack_from(menu.data, count_off)[0] if array is not None else 0
            items = [array.relocs[4 * i].target() for i in range(count) if array.relocs.get(4 * i) is not None and array.relocs[4 * i].kind != "null"]
            texts = [(item, _text_of(item, text_off)) for item in items if p.u32.unpack_from(item.data, type_off)[0] == ITEM_TYPE_TEXT]
            for item in items:
                if p.u32.unpack_from(item.data, type_off)[0] != ITEM_TYPE_MULTI:
                    continue
                dvar = (_text_of(item, dvar_off) or "").lower()
                ptr = item.relocs.get(data_off)
                multi = ptr.target() if ptr is not None and ptr.kind != "null" else None
                if dvar not in read_dvars or multi is None:
                    continue
                n = p.u32.unpack_from(multi.data, count_off_m)[0]
                strings = p.u32.unpack_from(multi.data, strdef_off)[0] != 0
                choices = []
                for k in range(min(n, 32)):
                    name = _text_of(multi, names_off + 4 * k)
                    if strings:
                        value = _text_of(multi, strs_off + 4 * k)
                    else:
                        value = _format_value(struct.unpack_from(p.endian + "f", multi.data, values_off + 4 * k)[0])
                    if name and value is not None and re.fullmatch(r"[\w.\-]{1,63}", value):
                        choices.append((value, name))
                if dvar not in options and len(choices) >= 2:
                    options[dvar] = {"dvar": dvar, "choices": choices}
                # its label: a text of the same row ending with ":" ("Difficulty:")
                y, h = struct.unpack_from(p.endian + "f", item.data, rect_off + 4)[0], struct.unpack_from(p.endian + "f", item.data, rect_off + 12)[0]
                for other, text in texts:
                    oy = struct.unpack_from(p.endian + "f", other.data, rect_off + 4)[0]
                    if text and text.rstrip().endswith(":") and not text.startswith("@") and abs(oy - y) <= max(h, 1.0):
                        labels.setdefault(dvar, text.rstrip()[:-1].strip())
    result = []
    for dvar, option in sorted(options.items()):
        values = [v for v, _ in option["choices"]]
        set_by_menus = [v for v in menu_values.get(dvar, ()) if v in values]
        option["default"] = set_by_menus[0] if len(set_by_menus) == 1 else values[0]
        option["label"] = labels.get(dvar) or dvar.replace("_", " ").title()
        result.append(option)
    return result


def write_options(options: List[dict], out_dir: str, log=print) -> Optional[str]:
    """``out_dir``/options.txt for CoD Xe's Custom Maps menu (see above); an earlier one goes when
    there are none."""
    import os

    path = os.path.join(out_dir, OPTIONS_FILE)
    if not options:
        if os.path.exists(path):
            os.remove(path)
        return None
    lines = []
    for option in options:
        lines.append(f"option {option['dvar']} {option['default']} {option['label']}")
        lines += [f"choice {value} {name}" for value, name in option["choices"]]
    with open(path, "w", encoding="latin-1", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    for option in options:
        names = ", ".join(name for _, name in option["choices"])
        default = next(name for value, name in option["choices"] if value == option["default"])
        log(f"options: {option['label']} ({option['dvar']}: {names}; {default} by default) in the Custom Maps menu, X to change")
    return path


# The same options in game, for the maps' players without that menu: a menu of the map (t4ff_option<n>)
# asks for each at the start, over the map's own start menus, and when a choice changes, the level
# restarts with it (t4ff_options_restart, whose opening runs fast_restart as the pause menu's
# Restart Level does: the level script reads the options once, as it starts, and the game's scripts
# have no map_restart). The menus are the game's own difficulty list (common.ff, always loaded), its
# rows one per choice.
OPTIONS_TEMPLATE = "popmenu_difficulty"
OPTIONS_MENU = "t4ff_option{}"
OPTIONS_RESTART_MENU = "t4ff_options_restart"
OPTIONS_ROW_STEP = 24.0  # the rows of the game's difficulty list


def _options_template_copy(p: Platform, zone: Zone, library):
    """A copy of the game's difficulty list for ``zone``, its nested assets name references."""
    import types

    from .assets import build_reference
    from .library import Cloner
    from .zone import ASSET_RECORDS

    found = library.find_in_game_zones("menuDef_t", OPTIONS_TEMPLATE) if library is not None else None
    if found is None:
        return None
    records = {rec: asset_type for asset_type, rec in ASSET_RECORDS.items()}
    conv = types.SimpleNamespace(dst=p, src=p)

    def reference(rec_name: str, name: str, node: Node):
        asset_type = records.get(rec_name)
        return build_reference(conv, asset_type, node, "," + name.lstrip(",")) if asset_type else None

    return Cloner(p, zone.script_strings, reference).copy_asset(found[0], found[1])


def _menu_lists_with_inline_menus(p: Platform, zone: Zone) -> List[Node]:
    rec = p.record("MenuList")
    lists = []
    for asset in zone.assets:
        if asset.type != "menulist" or asset.ptr is None or asset.ptr.kind not in ("follow", "insert"):
            continue
        node = asset.ptr.node
        ptr = node.relocs.get(find_field(rec, "menus").offset)
        array = ptr.node if ptr is not None and ptr.kind == "follow" else None
        if array is not None and any(r.kind == "follow" for r in array.relocs.values()):
            lists.append(node)
    return lists


def _dealias(menu: Node):
    """Pointers of ``menu`` to asset slots (the rows copied from one row alias its materials) load
    their own copy of the asset instead: the items holding the slots may be gone. The copies are the
    game's assets by name only."""
    for node in list(menu.walk()):
        changed = False
        for off, ptr in list(node.relocs.items()):
            if ptr.kind == "alias" and ptr.slot is not None and ptr.slot.node is not None:
                copy = clone(ptr.slot.node)
                node.relocs[off] = _ptr("follow", node, off, copy)
                copy.extra["ptr"] = node.relocs[off]
                struct.pack_into(">I", node.data, off, 0xFFFFFFFF)
                changed = True
        if changed:
            _rebuild_children(node)


def _append_menu(p: Platform, menu_list: Node, menu: Node):
    """Load ``menu`` inline in ``menu_list``, after its menus."""
    rec = p.record("MenuList")
    array = menu_list.relocs[find_field(rec, "menus").offset].node
    offset = len(array.data)
    array.data += b"\xff\xff\xff\xff"
    array.count += 1
    array.segments = [(array.segments[0][0], array.count, len(array.data), False)]
    array.relocs[offset] = _ptr("follow", array, offset, menu)
    _rebuild_children(array)
    struct.pack_into(">i", menu_list.data, find_field(rec, "menuCount").offset, array.count)


def add_options_menus(p: Platform, zone: Zone, library, options: List[dict], log=print) -> List[str]:
    """The in game menus of the map's options (see above), in a menu list of the zone. Returns the
    names of the menus, the restart menu last; none when the zone has no menu list to hold them or
    the game's difficulty list is not among the console fastfiles."""
    lists = _menu_lists_with_inline_menus(p, zone)
    if not options or not lists:
        return []
    names = []
    for index, option in enumerate(options):
        menu = _options_template_copy(p, zone, library)
        if menu is None:
            return []
        editor = MenuEditor(p, zone, OPTIONS_TEMPLATE, menu=menu)
        items = editor.items
        first_button = next((i for i, item in enumerate(items) if editor.string(item, "action")), None)
        if first_button is None or first_button < 3:
            raise MenuError(f"unexpected layout of {OPTIONS_TEMPLATE}")
        row = items[first_button - 3 : first_button + 1]  # backing, highlight, select button hint, button
        highlight, hint, button = row[1], row[2], row[3]
        # the frame and title; the descriptions and pictures of the game's difficulties go
        frame = [item for item in items[: first_button - 3]
                 if not any(t[0] == "str" for t in editor.expression(item, "visibleExp") + editor.expression(item, "materialExp"))]  # fmt: skip
        for item in frame:
            tokens = editor.expression(item, "textExp")
            if tokens:
                editor.set_expression(item, "textExp", [(kind, option["label"].upper() if kind == "str" else value) for kind, value in tokens])
        new_items = list(frame)
        default = 0
        for k, (value, name) in enumerate(option["choices"]):
            copies = [clone(item) for item in row]
            for copy in copies:
                editor.move(copy, OPTIONS_ROW_STEP * k)
            for copy, source in ((copies[1], highlight), (copies[2], hint)):
                editor.set_expression(copy, "visibleExp", _replace_int(editor.expression(source, "visibleExp"), 1, k + 1))
            b = copies[3]
            editor.set_string(b, "window.name", f"t4ff_choice{k}")
            editor.set_expression(b, "textExp", [(kind, name if kind == "str" else v) for kind, v in editor.expression(button, "textExp")])
            editor.set_string(b, "action", f'"play" "mouse_click" ; "scriptMenuResponse" "{value}" ; ')
            focus = re.sub(r'("setLocalVarInt"\s+"ui_highlight"\s+)\d+', rf"\g<1>{k + 1}", editor.string(button, "onFocus") or "")
            editor.set_string(b, "onFocus", focus)
            editor.set_string(b, "dvarTest", option["dvar"])
            editor.set_string(b, "enableDvar", f"{value} ")
            if value == option["default"]:
                default = k
            new_items += copies
        editor.set_items(new_items)
        name = OPTIONS_MENU.format(index)
        editor.set_menu_string("window.name", name)
        editor.set_menu_string("onOpen", f'"setLocalVarBool" "ui_centerPopup" 1 ; "setfocus" "t4ff_choice{default}" ; "setfocusbydvar" "{option["dvar"]}" ; ')
        # B keeps the choice (the level script closes the menu, and the map's menus wait for the answer)
        editor.set_menu_string("onESC", '"scriptMenuResponse" "keep" ; ')
        _dealias(menu)
        _append_menu(p, lists[0], menu)
        names.append(name)
    # the restart: the frame alone, which runs fast_restart when it opens
    menu = _options_template_copy(p, zone, library)
    editor = MenuEditor(p, zone, OPTIONS_TEMPLATE, menu=menu)
    editor.set_items([item for item in editor.items[:3]])
    editor.set_menu_string("window.name", OPTIONS_RESTART_MENU)
    editor.set_menu_string("onOpen", '"exec" "fast_restart" ; ')
    editor.set_menu_string("onClose", None)
    editor.set_menu_string("onESC", None)
    _dealias(menu)
    _append_menu(p, lists[0], menu)
    names.append(OPTIONS_RESTART_MENU)
    log(f"options: asked in game too ({', '.join(o['label'] for o in options)}), at the start: the level restarts when a choice changes")
    return names
