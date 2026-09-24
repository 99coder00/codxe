"""Parser for OpenAssetTools zone code command files (``*.gen`` / ``XAssets/*.txt``).

The commands describe how each structure is streamed from a fastfile: which
pointers are strings, how many elements an array pointer has, which block data
is loaded into, which union member is active and so on. See
``OpenAssetTools/src/ZoneCode/Game/T4``.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from .layout import Layout, OAT_ROOT, Record

OAT_T4_COMMANDS = os.path.join(OAT_ROOT, "src", "ZoneCode", "Game", "T4", "T4_Commands.txt")


# ---------------------------------------------------------------------------
# Expressions


class Expr:
    def eval(self, ctx) -> int:
        raise NotImplementedError


@dataclass
class Num(Expr):
    value: int

    def eval(self, ctx):
        return self.value


@dataclass
class Var(Expr):
    path: List[str]  # ['GfxWorld', 'dpvsPlanes', 'cellCount'] or ['numBones']
    indices: List[Expr] = field(default_factory=list)

    def eval(self, ctx):
        return ctx.lookup(self.path, [i.eval(ctx) for i in self.indices])


@dataclass
class Unary(Expr):
    op: str
    operand: Expr

    def eval(self, ctx):
        v = self.operand.eval(ctx)
        if self.op == "-":
            return -v
        if self.op == "!":
            return int(not v)
        if self.op == "~":
            return ~v
        raise ValueError(self.op)


@dataclass
class Binary(Expr):
    op: str
    lhs: Expr
    rhs: Expr

    def eval(self, ctx):
        op = self.op
        if op == "&&":
            return int(bool(self.lhs.eval(ctx)) and bool(self.rhs.eval(ctx)))
        if op == "||":
            return int(bool(self.lhs.eval(ctx)) or bool(self.rhs.eval(ctx)))
        a = self.lhs.eval(ctx)
        b = self.rhs.eval(ctx)
        if op == "+":
            return a + b
        if op == "-":
            return a - b
        if op == "*":
            return a * b
        if op == "/":
            return int(a / b) if b else 0
        if op == "%":
            return a % b
        if op == "<<":
            return a << b
        if op == ">>":
            return a >> b
        if op == "&":
            return a & b
        if op == "|":
            return a | b
        if op == "^":
            return a ^ b
        if op == "==":
            return int(a == b)
        if op == "!=":
            return int(a != b)
        if op == "<":
            return int(a < b)
        if op == "<=":
            return int(a <= b)
        if op == ">":
            return int(a > b)
        if op == ">=":
            return int(a >= b)
        raise ValueError(op)


@dataclass
class Ternary(Expr):
    cond: Expr
    a: Expr
    b: Expr

    def eval(self, ctx):
        return self.a.eval(ctx) if self.cond.eval(ctx) else self.b.eval(ctx)


class Never(Expr):
    def eval(self, ctx):
        return 0

    def __repr__(self):
        return "never"


NEVER = Never()

_TOKEN = re.compile(r"\s*(0[xX][0-9a-fA-F]+|\d+|[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*|&&|\|\||==|!=|<=|>=|<<|>>|[-+*/%()<>!~&|^?:\[\]])")

_PRECEDENCE = [
    ["||"],
    ["&&"],
    ["|"],
    ["^"],
    ["&"],
    ["==", "!="],
    ["<", "<=", ">", ">="],
    ["<<", ">>"],
    ["+", "-"],
    ["*", "/", "%"],
]


class _ExprParser:
    def __init__(self, text: str, enums: Dict[str, int]):
        self.tokens = []
        pos = 0
        text = text.strip()
        while pos < len(text):
            m = _TOKEN.match(text, pos)
            if not m:
                raise SyntaxError(f"bad expression near {text[pos:]!r}")
            self.tokens.append(m.group(1))
            pos = m.end()
        self.pos = 0
        self.enums = enums

    def peek(self):
        return self.tokens[self.pos] if self.pos < len(self.tokens) else None

    def take(self, expected=None):
        tok = self.peek()
        if expected is not None and tok != expected:
            raise SyntaxError(f"expected {expected!r} got {tok!r}")
        self.pos += 1
        return tok

    def parse(self) -> Expr:
        e = self.ternary()
        if self.peek() is not None:
            raise SyntaxError(f"trailing tokens {self.tokens[self.pos:]}")
        return e

    def ternary(self) -> Expr:
        cond = self.binary(0)
        if self.peek() == "?":
            self.take()
            a = self.ternary()
            self.take(":")
            b = self.ternary()
            return Ternary(cond, a, b)
        return cond

    def binary(self, level: int) -> Expr:
        if level == len(_PRECEDENCE):
            return self.unary()
        lhs = self.binary(level + 1)
        while self.peek() in _PRECEDENCE[level]:
            op = self.take()
            rhs = self.binary(level + 1)
            lhs = Binary(op, lhs, rhs)
        return lhs

    def unary(self) -> Expr:
        tok = self.peek()
        if tok in ("-", "!", "~"):
            self.take()
            return Unary(tok, self.unary())
        if tok == "+":
            self.take()
            return self.unary()
        return self.primary()

    def primary(self) -> Expr:
        tok = self.take()
        if tok == "(":
            e = self.ternary()
            self.take(")")
            return e
        if tok is None:
            raise SyntaxError("unexpected end of expression")
        if tok[0].isdigit():
            return Num(int(tok, 0))
        if tok in self.enums:
            return Num(self.enums[tok])
        if tok in ("true", "false"):
            return Num(int(tok == "true"))
        var = Var(tok.split("::"))
        while self.peek() == "[":
            self.take()
            var.indices.append(self.ternary())
            self.take("]")
        return var


def parse_expr(text: str, enums: Dict[str, int]) -> Expr:
    if text.strip() == "never":
        return NEVER
    return _ExprParser(text, enums).parse()


# ---------------------------------------------------------------------------
# Commands


@dataclass
class MemberInfo:
    context: str  # record the expressions are evaluated against
    string: bool = False
    scriptstring: bool = False
    reusable: bool = False
    count: Optional[Expr] = None
    index_counts: Dict[Tuple[int, ...], Expr] = field(default_factory=dict)
    arraysize: Optional[Expr] = None
    condition: Optional[Expr] = None
    block: Optional[str] = None
    allocalign: Optional[Expr] = None
    assetref: Optional[str] = None
    delayed: Optional[tuple] = None  # (block, alignment, condition or None)


@dataclass
class TypeInfo:
    block: Optional[str] = None
    allocalign: Optional[Expr] = None
    reorder: Optional[List[str]] = None


@dataclass
class Commands:
    assets: Dict[str, str] = field(default_factory=dict)  # record name -> asset enum name
    blocks: List[Tuple[str, str, bool]] = field(default_factory=list)  # (kind, name, default)
    types: Dict[str, TypeInfo] = field(default_factory=dict)
    # (owner record, member name) -> {context record: MemberInfo}
    members: Dict[Tuple[str, str], Dict[str, MemberInfo]] = field(default_factory=dict)

    def type_info(self, name: str) -> TypeInfo:
        return self.types.setdefault(name, TypeInfo())

    def member_infos(self, owner: str, member: str) -> Dict[str, MemberInfo]:
        return self.members.get((owner, member), {})


class CommandParser:
    def __init__(self, layout: Layout, lenient: bool = False):
        self.layout = layout
        self.cmds = Commands()
        self.use: Optional[str] = None
        # When lenient, directives for members that do not exist in the layout are skipped. This is
        # used to apply the PC commands to console layouts where some members were removed.
        self.lenient = lenient
        self.skipped: List[str] = []

    # -- file handling ------------------------------------------------------

    def parse_file(self, path: str):
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()

        base = os.path.dirname(path)
        statements = []
        for line in text.splitlines():
            stripped = line.strip()
            m = re.match(r'#include\s+"([^"]+)"', stripped)
            if m:
                self._flush(statements)
                statements = []
                self.parse_file(os.path.join(base, m.group(1)))
                continue
            line = re.sub(r"//.*", "", line)
            statements.append(line)
        self._flush(statements)

    def _flush(self, lines: List[str]):
        text = "\n".join(lines)
        for statement in text.split(";"):
            statement = " ".join(statement.split())
            if statement:
                self.statement(statement)

    # -- statements -----------------------------------------------------------

    def statement(self, s: str):
        try:
            self._statement(s)
        except KeyError:
            if not self.lenient:
                raise
            self.skipped.append(s)

    def _statement(self, s: str):
        if s.startswith("game ") or s.startswith("wordsize ") or s.startswith("architecture "):
            return
        if s.startswith("asset "):
            _, rec, enum = s.split()
            self.cmds.assets[rec] = enum
            return
        if s.startswith("block "):
            parts = s.split()
            self.cmds.blocks.append((parts[1], parts[2], len(parts) > 3 and parts[3] == "default"))
            return
        if s.startswith("use "):
            self.use = s.split()[1]
            if self.use not in self.layout.records:
                raise KeyError(f"use of unknown type {self.use}")
            return
        if s.startswith("reorder"):
            m = re.match(r"reorder\s*(\w+)?\s*:\s*(.*)", s)
            target = m.group(1) or self.use
            self.cmds.type_info(target).reorder = m.group(2).split()
            return
        if s.startswith("set "):
            self.set_statement(s[4:])
            return
        raise SyntaxError(f"unknown command {s!r}")

    def set_statement(self, s: str):
        kind, rest = s.split(" ", 1)
        if kind == "action":
            return

        if kind in ("string", "scriptstring", "reusable"):
            owner, member, _, ctx = self.resolve_member(rest.strip())
            setattr(self.member(owner, member, ctx), kind, True)
            return

        path, _, arg = rest.partition(" ")
        arg = arg.strip()

        if kind == "block":
            if not arg:
                # 'set block BLOCK' applies to the current type
                self.cmds.type_info(self.use).block = path
                return
            owner, member, _, ctx = self.resolve_member(path)
            self.member(owner, member, ctx).block = arg
            return

        if kind == "allocalign":
            if path in self.layout.records and self._resolve_is_type(path):
                self.cmds.type_info(path).allocalign = parse_expr(arg, self.layout.enums)
                return
            owner, member, _, ctx = self.resolve_member(path)
            self.member(owner, member, ctx).allocalign = parse_expr(arg, self.layout.enums)
            return

        if kind == "delayed":
            # Console extension: the pointed data is streamed after all assets (e.g. image pixels).
            # set delayed <member> <block> <alignment> [condition]
            owner, member, _, ctx = self.resolve_member(path)
            parts = arg.split(None, 2)
            info = self.member(owner, member, ctx)
            condition = parse_expr(parts[2], self.layout.enums) if len(parts) > 2 else None
            info.delayed = (parts[0], int(parts[1], 0) if len(parts) > 1 else 1, condition)
            return

        if kind == "assetref":
            owner, member, _, ctx = self.resolve_member(path)
            self.member(owner, member, ctx).assetref = arg
            return

        owner, member, indices, ctx = self.resolve_member(path)
        info = self.member(owner, member, ctx)
        expr = parse_expr(arg, self.layout.enums)
        if kind == "count":
            if indices:
                info.index_counts[tuple(indices)] = expr
            else:
                info.count = expr
        elif kind == "arraysize":
            info.arraysize = expr
        elif kind == "condition":
            info.condition = expr
        else:
            raise SyntaxError(f"unknown set command {kind}")

    def _resolve_is_type(self, name: str) -> bool:
        # A bare type name, as opposed to a member of the current type with the same spelling.
        return self.use is None or self.layout.records[self.use].field(name) is None

    def member(self, owner: str, member: str, ctx: str) -> MemberInfo:
        infos = self.cmds.members.setdefault((owner, member), {})
        if ctx not in infos:
            infos[ctx] = MemberInfo(context=ctx)
        return infos[ctx]

    def resolve_member(self, path: str):
        """Resolve ``a::b::c[1]`` to (owner record, member name, indices, context record)."""

        indices = [int(x, 0) for x in re.findall(r"\[(\w+)\]", path)]
        path = re.sub(r"\[\w+\]", "", path)
        parts = path.split("::")

        records = self.layout.records
        if len(parts) > 1 and parts[0] in records and (self.use is None or find_field(records[self.use], parts[0]) is None):
            ctx = parts[0]
            parts = parts[1:]
        else:
            ctx = self.use

        owner = records[ctx]
        for part in parts[:-1]:
            # (paths only walk through embedded records here)
            f = find_field(owner, part)
            if f is None:
                raise KeyError(f"{owner.name} has no member {part} ({path})")
            t = f.type
            while t.kind == "array":
                t = t.elem
            if t.kind != "record":
                raise KeyError(f"{owner.name}::{part} is not a record ({path})")
            owner = records[t.name]

        if find_field(owner, parts[-1]) is None:
            raise KeyError(f"{owner.name} has no member {parts[-1]} ({path})")
        return owner.name, parts[-1], indices, ctx


def find_field(record: Record, name: str):
    for f in record.fields:
        if f.name == name:
            return f
    return None


def load_commands(layout: Layout, path: str = OAT_T4_COMMANDS) -> Commands:
    parser = CommandParser(layout)
    parser.parse_file(path)
    return parser.cmds
