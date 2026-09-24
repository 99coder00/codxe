"""Struct layout extraction.

The T4 asset structures are described by OpenAssetTools' ``T4_Assets.h``. Rather
than re-implementing a C parser, libclang is used to compute the exact MSVC x86
layout (sizes, alignments, field offsets and bitfields) of every record.

The 360 build of the game uses the same 32-bit MSVC data model (4-byte pointers,
8-byte aligned 64-bit integers), so the 360 layouts are computed the same way
from a copy of the header where the structures that differ on console are
replaced by the definitions in ``x360_structs.h``.

Layouts are cached as JSON next to the tool so libclang is only needed when the
headers change.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
from dataclasses import dataclass, field
from typing import Dict, List, Optional

TOOL_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(TOOL_DIR, "..", "..", ".."))
OAT_ROOT = os.path.join(REPO_ROOT, "OpenAssetTools")
OAT_T4_HEADER = os.path.join(OAT_ROOT, "src", "Common", "Game", "T4", "T4_Assets.h")
OAT_INCLUDE = os.path.join(OAT_ROOT, "src", "Common")
X360_OVERRIDES = os.path.join(TOOL_DIR, "defs", "x360_structs.h")
CACHE_DIR = os.path.join(TOOL_DIR, "cache")

_STDINT = """
typedef signed char int8_t; typedef unsigned char uint8_t;
typedef short int16_t; typedef unsigned short uint16_t;
typedef int int32_t; typedef unsigned int uint32_t;
typedef long long int64_t; typedef unsigned long long uint64_t;
"""

# Scalar kinds understood by the zone code. "code" is a struct module format
# character (without endianness).
SCALARS = {
    "char": ("b", 1),
    "schar": ("b", 1),
    "uchar": ("B", 1),
    "bool": ("B", 1),
    "short": ("h", 2),
    "ushort": ("H", 2),
    "int": ("i", 4),
    "uint": ("I", 4),
    "long": ("i", 4),
    "ulong": ("I", 4),
    "longlong": ("q", 8),
    "ulonglong": ("Q", 8),
    "float": ("f", 4),
    "double": ("d", 8),
}


@dataclass
class TypeRef:
    """Reference to a type as used by a field.

    kind: 'scalar' | 'enum' | 'pointer' | 'array' | 'record' | 'void'
    """

    kind: str
    name: str = ""  # scalar name / record name / enum name
    size: int = 0
    align: int = 1
    to: Optional["TypeRef"] = None  # pointer target
    elem: Optional["TypeRef"] = None  # array element
    count: int = 0  # array element count

    def to_json(self):
        d = {"kind": self.kind, "name": self.name, "size": self.size, "align": self.align}
        if self.to is not None:
            d["to"] = self.to.to_json()
        if self.elem is not None:
            d["elem"] = self.elem.to_json()
            d["count"] = self.count
        return d

    @staticmethod
    def from_json(d) -> "TypeRef":
        return TypeRef(
            kind=d["kind"],
            name=d.get("name", ""),
            size=d.get("size", 0),
            align=d.get("align", 1),
            to=TypeRef.from_json(d["to"]) if "to" in d else None,
            elem=TypeRef.from_json(d["elem"]) if "elem" in d else None,
            count=d.get("count", 0),
        )

    def __repr__(self):
        if self.kind == "pointer":
            return f"{self.to!r}*"
        if self.kind == "array":
            return f"{self.elem!r}[{self.count}]"
        return self.name or self.kind


@dataclass
class Field:
    name: str  # empty for anonymous members
    offset: int  # byte offset
    type: TypeRef
    bit_offset: int = -1  # bit offset inside the storage unit for bitfields, -1 otherwise
    bit_width: int = 0

    def to_json(self):
        d = {"name": self.name, "offset": self.offset, "type": self.type.to_json()}
        if self.bit_width:
            d["bit_offset"] = self.bit_offset
            d["bit_width"] = self.bit_width
        return d

    @staticmethod
    def from_json(d) -> "Field":
        return Field(d["name"], d["offset"], TypeRef.from_json(d["type"]), d.get("bit_offset", -1), d.get("bit_width", 0))


@dataclass
class Record:
    name: str
    is_union: bool
    size: int
    align: int
    fields: List[Field] = field(default_factory=list)

    def to_json(self):
        return {
            "name": self.name,
            "union": self.is_union,
            "size": self.size,
            "align": self.align,
            "fields": [f.to_json() for f in self.fields],
        }

    @staticmethod
    def from_json(d) -> "Record":
        return Record(d["name"], d["union"], d["size"], d["align"], [Field.from_json(f) for f in d["fields"]])

    def field(self, name: str) -> Optional[Field]:
        for f in self.fields:
            if f.name == name:
                return f
        return None


@dataclass
class Layout:
    """All records and enum constants of one platform."""

    platform: str
    records: Dict[str, Record]
    enums: Dict[str, int]
    enum_sizes: Dict[str, int]

    def record(self, name: str) -> Record:
        return self.records[name]

    def to_json(self):
        return {
            "platform": self.platform,
            "records": {k: v.to_json() for k, v in self.records.items()},
            "enums": self.enums,
            "enum_sizes": self.enum_sizes,
        }

    @staticmethod
    def from_json(d) -> "Layout":
        return Layout(
            d["platform"],
            {k: Record.from_json(v) for k, v in d["records"].items()},
            d["enums"],
            d["enum_sizes"],
        )


def _replace_struct_definitions(text: str, overrides: str) -> str:
    """Replace every struct/union of ``text`` that is also defined in ``overrides``."""

    definition = re.compile(r"^(\s*)(struct|union)\s+(?:type_align\(\d+\)\s+|gcc_align32\(\d+\)\s+)?(\w+)\s*\n\s*\{", re.M)

    def find_definition(src: str, name: str):
        for m in definition.finditer(src):
            if m.group(3) != name:
                continue
            depth = 0
            i = src.index("{", m.start())
            while True:
                c = src[i]
                if c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                    if depth == 0:
                        end = src.index(";", i) + 1
                        return m.start(), end
                i += 1
        return None

    for m in definition.finditer(overrides):
        name = m.group(3)
        span = find_definition(overrides, name)
        if span is None:
            continue
        body = overrides[span[0] : span[1]]
        target = find_definition(text, name)
        if target is None:
            raise ValueError(f"x360 override for unknown record {name}")
        text = text[: target[0]] + "\n" + body + text[target[1] :]
    return text


class _ClangExtractor:
    def __init__(self):
        import clang.cindex as ci  # imported lazily: only needed to refresh the cache

        self.ci = ci
        self.records: Dict[str, Record] = {}
        self.enums: Dict[str, int] = {}
        self.enum_sizes: Dict[str, int] = {}
        self._anon_names: Dict[str, str] = {}

    def parse(self, source: str) -> Layout:
        ci = self.ci
        index = ci.Index.create()
        tu = index.parse(
            "t4_layout.h",
            args=[
                "-x",
                "c++",
                "-std=c++20",
                "-target",
                "i386-pc-windows-msvc",
                "-fms-extensions",
                "-D_MSVC_LANG=202002",
                "-DARCH_x86",
                "-I" + OAT_INCLUDE,
            ],
            unsaved_files=[("t4_layout.h", source)],
        )
        errors = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
        if errors:
            raise RuntimeError("libclang failed to parse the T4 headers:\n" + "\n".join(str(e) for e in errors[:20]))

        for cursor in tu.cursor.walk_preorder():
            if cursor.kind == ci.CursorKind.ENUM_CONSTANT_DECL:
                self.enums[cursor.spelling] = cursor.enum_value
            elif cursor.kind == ci.CursorKind.ENUM_DECL and cursor.is_definition() and cursor.spelling:
                self.enum_sizes[cursor.spelling] = cursor.type.get_size()

        for cursor in tu.cursor.walk_preorder():
            if cursor.kind in (ci.CursorKind.STRUCT_DECL, ci.CursorKind.UNION_DECL) and cursor.is_definition():
                if cursor.is_anonymous() or not cursor.spelling or "(" in cursor.spelling:
                    continue
                self._record(cursor, cursor.spelling)

        return Layout("", self.records, self.enums, self.enum_sizes)

    def _record(self, cursor, name: str) -> Record:
        ci = self.ci
        if name in self.records:
            return self.records[name]

        rec = Record(name, cursor.kind == ci.CursorKind.UNION_DECL, cursor.type.get_size(), cursor.type.get_align())
        self.records[name] = rec

        anon_index = 0
        for child in cursor.get_children():
            if child.kind != ci.CursorKind.FIELD_DECL:
                continue

            field_name = child.spelling
            bit_offset = child.get_field_offsetof()
            decl = child.type.get_canonical().get_declaration()
            is_anonymous_record = decl is not None and decl.kind in (ci.CursorKind.STRUCT_DECL, ci.CursorKind.UNION_DECL) and decl.is_anonymous()

            if is_anonymous_record and not field_name:
                # C11 style anonymous member, keep it as an unnamed field so paths can be flattened.
                field_name = ""

            type_ref = self._type(child.type, f"{name}::{field_name or f'__anon{anon_index}'}")
            if not field_name:
                anon_index += 1

            if child.is_bitfield():
                width = child.get_bitfield_width()
                storage = type_ref.size
                storage_offset = (bit_offset // (storage * 8)) * storage
                rec.fields.append(Field(field_name, storage_offset, type_ref, bit_offset - storage_offset * 8, width))
            else:
                rec.fields.append(Field(field_name, bit_offset // 8, type_ref))

        return rec

    def _type(self, t, context: str) -> TypeRef:
        ref = self._canonical_type(t, context)

        # Typedefs such as raw_byte16 or XSurfaceTri16 carry their own alignment which the
        # canonical type loses. The zone loader aligns allocations using the declared type.
        declared_align = t.get_align()
        if declared_align > ref.align:
            ref.align = declared_align
        return ref

    def _canonical_type(self, t, context: str) -> TypeRef:
        ci = self.ci
        K = ci.TypeKind
        canonical = t.get_canonical()
        kind = canonical.kind

        if kind == K.POINTER:
            # Keep the declared pointee (not the canonical one) so typedef alignment survives.
            declared = t
            while declared.kind == K.ELABORATED:
                declared = declared.get_named_type()
            pointee = declared.get_pointee() if declared.kind == K.POINTER else canonical.get_pointee()
            if pointee.get_canonical().kind == K.FUNCTIONPROTO:
                return TypeRef("pointer", size=4, align=4, to=TypeRef("void"))
            return TypeRef("pointer", size=4, align=4, to=self._type(pointee, context + "*"))

        if kind == K.CONSTANTARRAY:
            declared = t
            while declared.kind == K.ELABORATED:
                declared = declared.get_named_type()
            element_type = declared.element_type if declared.kind == K.CONSTANTARRAY else canonical.element_type
            elem = self._type(element_type, context)
            return TypeRef("array", size=canonical.get_size(), align=canonical.get_align(), elem=elem, count=canonical.element_count)

        if kind == K.RECORD:
            decl = canonical.get_declaration()
            if decl.is_anonymous() or not decl.spelling or "(" in decl.spelling:
                rec_name = context
            else:
                rec_name = decl.spelling
            if decl.is_definition() or decl.get_definition() is not None:
                definition = decl.get_definition() or decl
                self._record(definition, rec_name)
            return TypeRef("record", name=rec_name, size=canonical.get_size(), align=canonical.get_align())

        if kind == K.ENUM:
            decl = canonical.get_declaration()
            size = canonical.get_size()
            return TypeRef("enum", name=decl.spelling, size=size, align=canonical.get_align())

        if kind == K.VOID:
            return TypeRef("void")

        scalar_names = {
            K.CHAR_S: "char",
            K.SCHAR: "schar",
            K.UCHAR: "uchar",
            K.CHAR_U: "uchar",
            K.BOOL: "bool",
            K.SHORT: "short",
            K.USHORT: "ushort",
            K.INT: "int",
            K.UINT: "uint",
            K.LONG: "long",
            K.ULONG: "ulong",
            K.LONGLONG: "longlong",
            K.ULONGLONG: "ulonglong",
            K.FLOAT: "float",
            K.DOUBLE: "double",
            K.WCHAR: "ushort",
        }
        if kind in scalar_names:
            name = scalar_names[kind]
            return TypeRef("scalar", name=name, size=canonical.get_size(), align=canonical.get_align())

        raise TypeError(f"unsupported type {t.spelling} ({kind}) in {context}")


def _source_for(platform: str) -> str:
    with open(OAT_T4_HEADER, "r", encoding="utf-8") as f:
        header = f.read()

    if platform == "x360":
        with open(X360_OVERRIDES, "r", encoding="utf-8") as f:
            header = _replace_struct_definitions(header, f.read())
    elif platform != "pc":
        raise ValueError(platform)

    # The header includes TypeAlignment.h relatively; keep it resolvable from the unsaved buffer.
    header = header.replace('#include "../../Utils/TypeAlignment.h"', '#include "Utils/TypeAlignment.h"')
    return _STDINT + header


def load_layout(platform: str) -> Layout:
    """Return the layout for ``platform`` ('pc' or 'x360'), using the JSON cache when valid."""

    source = _source_for(platform)
    digest = hashlib.sha1(source.encode("utf-8")).hexdigest()[:16]
    os.makedirs(CACHE_DIR, exist_ok=True)
    cache_path = os.path.join(CACHE_DIR, f"layout_{platform}_{digest}.json")

    if os.path.exists(cache_path):
        with open(cache_path, "r", encoding="utf-8") as f:
            layout = Layout.from_json(json.load(f))
            layout.platform = platform
            return layout

    layout = _ClangExtractor().parse(source)
    layout.platform = platform
    with open(cache_path, "w", encoding="utf-8") as f:
        json.dump(layout.to_json(), f)
    return layout
