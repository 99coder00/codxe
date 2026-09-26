"""A dynamic, scrolling map list in the Nazi Zombies menu.

CoD Xenon's ``patch_ui.ff`` lists their maps in the menu ``levels_unlock``: the four stock maps,
then one hand made row per converted map (a backing, a highlight, the A button hint and the button,
which runs ``devmap <map>``) and its preview (picture, name and description) on the right. It
holds 13 custom maps, the screen holds no more.

:func:`make_dynamic` keeps the stock rows and replaces the custom ones by ``ROWS`` rows whose text
and command are dvars, which CoD Xe fills from the usermaps folder (``src/game/t4/sp/components/
usermaps.cpp``):

- ``ui_codxe_map<k>`` / ``ui_codxe_mapcmd<k>``: the name and the command (``devmap <map>``) of the
  row ``k`` (empty: the row is hidden);
- ``ui_codxe_mapoffset``, ``ui_codxe_mapmore``: the list's position (the scroll catchers above and
  below the rows are there only when there is more to see), ``ui_codxe_maprange`` the counter;
- ``ui_codxe_maptitle``, ``ui_codxe_mapdesc``, ``ui_codxe_mapimage``: the preview of the focused map;
- the menu sets ``ui_codxe_focus`` (the focused row) and ``ui_codxe_scroll`` (rows to scroll: the
  catchers +-1, LB / RB a page), which CoD Xe reads back.
"""

from __future__ import annotations

import re
import struct
from typing import Dict, List, Optional, Tuple

from .commands import find_field
from .zone import Node, Platform, Ptr, Zone

LIST_MENU = "levels_unlock"
ROWS = 13
STOCK_MAPS = ("nazi_zombie_prototype", "nazi_zombie_asylum", "nazi_zombie_sumpf", "nazi_zombie_factory")
FIRST_HIGHLIGHT = 100  # ui_highlight of the dynamic rows (the stock ones use 2 to 5)
PREVIEW = "image_codxe_map"
COUNTER_DX = 280.0  # the counter, right of the rows (250 wide)

# The picture of the focused map when the menu zone has none of its own: CoD Xe copies the map's
# preview.bin (a 512x288 DXT1 texture, tiled for the console) into this image, which the menu shows.
PREVIEW_SLOT = "codxe_map_preview"
PREVIEW_SIZE = (512, 288)
PREVIEW_MAGIC = b"CXPV"
PREVIEW_FILE = "preview.bin"
KEY_LSHLDR, KEY_RSHLDR = 5, 6

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

    def __init__(self, p: Platform, zone: Zone, menu_name: str = LIST_MENU):
        self.p = p
        self.zone = zone
        self.irec = p.record("itemDef_s")
        self.wrec = p.record("windowDef_t")
        self.mrec = p.record("menuDef_t")
        self.window = find_field(self.irec, "window").offset
        menu = next((a for a in zone.assets if a.type == "menu" and a.name == menu_name), None)
        if menu is None:
            raise MenuError(f"no menu {menu_name} in this zone")
        self.menu = menu.ptr.target() if menu.ptr.kind == "alias" else menu.ptr.node
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
    """Replace the custom map rows of ``levels_unlock`` by ``rows`` dynamic ones (see the module).
    Returns CoD Xenon's rows that were there (map, localized name and description keys, preview
    picture), for their description files."""
    editor = MenuEditor(p, zone)
    found = custom_rows(editor)
    if not found:
        raise MenuError(f"{LIST_MENU} has no custom map rows (already dynamic?)")
    items = editor.items
    template = found[0]
    button = items[template["button"]]
    row_items = [items[i] for i in template["row"]]
    if len(row_items) != 4 or row_items[-1] is not button:
        raise MenuError("unexpected layout of the custom map rows")
    backing, highlight, hint, _ = row_items
    old = template["highlight"]
    preview_items = [items[i] for i in template["preview"]]
    removed = {i for r in found for i in r["row"] + r["preview"]}
    first = min(removed)
    y0 = min(r["y"] for r in found)
    step = 20.0

    new_items: List[Node] = []

    def row_copy(item: Node, row: int) -> Node:
        copy = clone(item)
        editor.move(copy, y0 + step * row - template["y"])
        return copy

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

    # the position in the list ("14-26 / 40"), right of the last row (under it is the Back button)
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
        new_items.append(copy)

    kept = [item for i, item in enumerate(items) if i not in removed]
    position = sum(1 for i in range(first) if i not in removed)
    editor.set_items(kept[:position] + new_items + kept[position:])
    check_references(zone, [items[i] for i in removed])

    # LB / RB: a page up / down
    key_owner = next((item for item in editor.items if item.relocs.get(editor.offset("onKey")) is not None and item.relocs[editor.offset("onKey")].kind == "follow"), editor.items[0])
    editor.add_key_handler(key_owner, KEY_LSHLDR, f'"setdvar" "ui_codxe_scroll" "-{rows}" ; ')
    editor.add_key_handler(key_owner, KEY_RSHLDR, f'"setdvar" "ui_codxe_scroll" "{rows}" ; ')

    add_preview_slot(p, zone, template["image"])
    return found


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
