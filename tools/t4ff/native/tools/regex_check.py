"""Checks native/src/core/pyre (the C++ regex engine of the script passes) against Python's re.

    python regex_check.py corpus OUT_DIR FASTFILE...   the raw files and strings of PC fastfiles, one file each
    python regex_check.py run PATTERNS FILE...         a line per pattern and file: matches and their CRC

t4ff-cli regex PATTERNS FILE... prints the same lines; compare them. PATTERNS: a line per pattern,
"<flags>\t<pattern>" (flags: b bytes, i, m, s; "-" none).
"""

import os
import re
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def corpus(out_dir, *paths):
    from t4ff.fastfile import read_fastfile
    from t4ff.platforms import pc
    from t4ff.zone import Reader

    os.makedirs(out_dir, exist_ok=True)
    n = 0
    for path in paths:
        _, _, data = read_fastfile(path)
        zone = Reader(pc(), data).load()
        strings = []
        for node in zone.extra_root.walk():
            if node.string:
                strings.append(bytes(node.data).rstrip(b"\0"))
            origin = node.extra.get("origin") or ("", "", "")
            if origin[1:] == ("RawFile", "buffer"):
                with open(os.path.join(out_dir, f"{n:05d}.txt"), "wb") as f:
                    f.write(bytes(node.data).rstrip(b"\0"))
                n += 1
        with open(os.path.join(out_dir, f"{n:05d}.txt"), "wb") as f:
            f.write(b"\n".join(strings))
        n += 1
    print(f"{n} files")


def patterns(path):
    out = []
    with open(path, "rb") as f:
        for line in f.read().split(b"\n"):
            line = line.rstrip(b"\r")
            if not line:
                continue
            flags, pattern = line.split(b"\t", 1)
            flags = flags.decode()
            f_ = (re.I if "i" in flags else 0) | (re.M if "m" in flags else 0) | (re.S if "s" in flags else 0)
            out.append(re.compile(pattern, f_) if "b" in flags else re.compile(pattern.decode("latin-1"), f_))
    return out


def write_patterns(out_path):
    """Every pattern of the script and menu passes (those of the modules, and those made in their
    functions), as a PATTERNS file."""
    from t4ff import loadscreen, menu, merge, named, scripts

    found = []
    for module in (scripts, named, menu, merge, loadscreen):
        for value in vars(module).values():
            if isinstance(value, re.Pattern) and value not in found:
                found.append(value)
    inline = [
        re.compile(rb"#include\s+[\w\\/]+\s*;"),
        re.compile(rb"(?m)^[ \t]*" + re.escape(b"main") + rb"[ \t]*\([^)]*\)\s*\{", re.I),
        re.compile(rb"(?m)^[ \t]*" + re.escape(b"init_animscripts") + rb"[ \t]*\([^)]*\)\s*\{", re.I),
        re.compile(r'"classname"\s+"(actor_[^"]*)"'),
        re.compile(r'"([^"]+)"\s+"([^"]*)"'),
        re.compile(r"\{[^{}]*\}"),
        scripts._script_path("maps/_zombiemode_zone_manager.gsc"),
        scripts._script_path("maps/_laststand.gsc"),
        re.compile(rf"^\s*{'scriptevent'}\s*{{", re.M | re.I),
        re.compile(r"[\w.\-]{1,63}"),
        re.compile(r'("setLocalVarInt"\s+"ui_highlight"\s+)\d+'),
        re.compile(r'(?i)(?:openmenu|precachemenu|closemenu)\s*\(\s*"([^"]+)"'),
        re.compile(r"\{([^}]*)\}"),
        re.compile(r'\bmap\s+"([^"]+)"', re.I),
        re.compile(r'longname\s+"([^"]+)"', re.I),
        re.compile(r"@?[A-Z0-9_]+"),
        re.compile(r"\^."),
        re.compile(r"[A-Z0-9_]+"),
        # the engine's features on their own
        re.compile(r"(\w+?)(\d*)\s*(?:=|==)\s*(.{0,3}?)(?=;)"),
        re.compile(rb"(a|ab)(c|bcd)(d*)"),
        re.compile(r"(?s)\{.*?\}|\[[^\]]*\]"),
        re.compile(r"\B\w{2,4}\b|$", re.M),
        re.compile(r"(?<=\()\s*\w+(?:\s*,\s*\w+)*\s*(?=\))"),
        re.compile(rb"(?:(\w)\W){2,3}?x?"),
        re.compile(r"\x41|[\x61-\x63]+|\t\n"),
        re.compile("[À-ÿ]+é", re.I),
    ]
    with open(out_path, "wb") as f:
        for p in found + inline:
            flags = ("b" if isinstance(p.pattern, bytes) else "") + ("i" if p.flags & re.I else "") + ("m" if p.flags & re.M else "") + ("s" if p.flags & re.S else "")
            pattern = p.pattern if isinstance(p.pattern, bytes) else p.pattern.encode("latin-1")
            assert b"\n" not in pattern and b"\t" not in pattern, pattern
            f.write((flags or "-").encode() + b"\t" + pattern + b"\n")
    print(f"{len(found) + len(inline)} patterns")


def signature(m):
    return " ".join(f"{m.start(g)},{m.end(g)}" for g in range(m.re.groups + 1))


def run(pattern_file, *files):
    pats = patterns(pattern_file)
    for j, path in enumerate(files):
        with open(path, "rb") as f:
            data = f.read()
        for i, p in enumerate(pats):
            subject = data if isinstance(p.pattern, bytes) else data.decode("latin-1")
            parts = [signature(m) for m in p.finditer(subject)]
            count = len(parts)
            # match / fullmatch on every line, and on every line from its second character
            for line in (subject.split(b"\n") if isinstance(subject, bytes) else subject.split("\n")):
                for m in (p.match(line), p.fullmatch(line), p.match(line, 1), p.search(line, 1, max(len(line) - 1, 0))):
                    parts.append(signature(m) if m else "-")
            crc = zlib.crc32("\n".join(parts).encode())
            print(f"{i} {j} {count} {crc:08x}")


if __name__ == "__main__":
    {"corpus": corpus, "run": run, "patterns": write_patterns}[sys.argv[1]](*sys.argv[2:])
