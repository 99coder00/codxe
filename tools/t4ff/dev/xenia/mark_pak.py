"""Color-mark entries of a map's images.pak to see in game which level of a streamed texture shows.

A deep entry gets level 0 green, level 1 red and the rest blue (level 1 alone when the entry has no
level 2 offset: red to the end); a top level entry gets pink. The colors are DXT1 blocks whose two
bytes are equal, so the console's 16 bit swap leaves them as they are. The original bytes are saved
next to the pack (<pack>.marks/<name>.bin) and put back with --restore.

usage: python mark_pak.py <images.pak> <name or substring>... [--eighth] [--restore]

--eighth: every entry whose fastfile copy is an eighth of its size (PAK_EIGHTH) too.
"""
import os
import struct
import sys

GREEN, RED, BLUE, PINK = 0x0707, 0xE0E0, 0x1F1F, 0xF8F8


def block(color):
    return struct.pack(">HH", color, color) + bytes(4)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    pak, patterns = args[0], [a.lower() for a in args[1:]]
    restore = "--restore" in sys.argv
    eighths = "--eighth" in sys.argv
    backups = pak + ".marks"
    os.makedirs(backups, exist_ok=True)
    with open(pak, "r+b") as f:
        magic, version, count, index_offset, index_size = struct.unpack_from(">8sIIII", f.read(32))
        if magic != b"T4FFPAK1" or version not in (2, 3):
            sys.exit(f"{pak}: not a t4ff images.pak (version 2 or 3)")
        f.seek(index_offset)
        index = f.read(index_size)
        for i in range(count):
            name_offset, name_length, flags, offset, size, mip_offset, level2 = struct.unpack_from(">IHHIIII", index, 24 * i)
            name = index[name_offset:name_offset + name_length].decode("latin1")
            if not any(p == name or p in name for p in patterns) and not (eighths and flags & 2):
                continue
            backup = os.path.join(backups, "".join(c if c.isalnum() or c in "_-" else "_" for c in name) + ".bin")
            f.seek(offset)
            if restore:
                if os.path.exists(backup):
                    f.write(open(backup, "rb").read())
                    os.remove(backup)
                    print("restored", name)
                continue
            if not os.path.exists(backup):
                open(backup, "wb").write(f.read(size))
                f.seek(offset)
            if flags & 1:
                level1 = level2 if level2 else size - mip_offset
                data = block(GREEN) * (mip_offset // 8) + block(RED) * (level1 // 8) + block(BLUE) * ((size - mip_offset - level1) // 8)
            else:
                data = block(PINK) * (size // 8)
            f.write(data)
            print("marked", name, "deep" if flags & 1 else "top level")


if __name__ == "__main__":
    main()
