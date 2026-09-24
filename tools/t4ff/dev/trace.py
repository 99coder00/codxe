"""Development helper: find where a console layout is wrong.

Reads a fastfile and, when reading fails, prints the last allocations made by
the loader (stream position, type, count, block) followed by a hex dump of the
stream around the failure. Comparing that with the PC version of the same asset
(``python -m t4ff info --list``) is how the console layouts were recovered.

    python dev/trace.py path/to/fastfile.ff [allocations to show]
"""

import collections
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from t4ff.fastfile import read_fastfile  # noqa: E402
from t4ff.platforms import for_endian  # noqa: E402
from t4ff.zone import BLOCK_NAMES, Reader  # noqa: E402


def main(path, count=30):
    endian, _, data = read_fastfile(path)
    reader = Reader(for_endian(endian), data)
    log = collections.deque(maxlen=count)
    reader.trace = lambda t, n, block, pos: log.append((pos, repr(t), n, BLOCK_NAMES[block]))
    try:
        zone = reader.load()
    except Exception as e:  # noqa: BLE001 - report whatever went wrong
        for pos, t, n, block in log:
            print(f"{pos:#010x}  {t:40s} x{n:<8} {block}")
        print(f"\nerror: {e} (stream position {reader.pos:#x})\n")
        start = max(0, reader.pos - 0x40)
        for i in range(start, min(len(data), start + 0xC0), 16):
            chunk = data[i : i + 16]
            text = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
            print(f"{i:#010x}  {chunk.hex(' '):48s}  {text}")
        return 1
    print(f"{path}: read {len(zone.assets)} assets without errors")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 30))
