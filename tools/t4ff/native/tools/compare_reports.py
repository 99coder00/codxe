"""Compares two texture battery reports (t4ff-cli textures, py_reference.py textures).

    python native/tools/compare_reports.py <cpp.txt> <py.txt>

Prints how many lines match, and for the ones that differ, which stage and which fields.
"""

import collections
import sys


def fields(line):
    parts = line.split(" ")
    return parts[0], parts[1], {p.split("=", 1)[0]: p for p in parts[2:]}


def main(a_path, b_path):
    a = open(a_path, encoding="latin-1").read().splitlines()
    b = open(b_path, encoding="latin-1").read().splitlines()
    if len(a) != len(b):
        print(f"line counts differ: {len(a)} vs {len(b)}")
    same = sum(x == y for x, y in zip(a, b))
    print(f"{same} of {max(len(a), len(b))} lines identical")
    by_field = collections.Counter()
    examples = collections.defaultdict(list)
    for x, y in zip(a, b):
        if x == y:
            continue
        name_a, stage_a, fa = fields(x)
        name_b, stage_b, fb = fields(y)
        if (name_a, stage_a) != (name_b, stage_b):
            by_field["(misaligned)"] += 1
            examples["(misaligned)"].append((x, y))
            continue
        for key in sorted(set(fa) | set(fb)):
            if fa.get(key) != fb.get(key):
                tag = f"{stage_a}:{key}"
                by_field[tag] += 1
                if len(examples[tag]) < 3:
                    examples[tag].append((name_a, fa.get(key), fb.get(key)))
    for tag, count in by_field.most_common():
        print(f"  {tag}: {count}")
        for ex in examples[tag]:
            print(f"      {ex}")
    return 0 if same == len(a) == len(b) else 1


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:3]))
