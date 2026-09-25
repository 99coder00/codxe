"""Progress of the long steps of a conversion.

The window runs the converter as a separate process (``--progress-lines``) and reads the
``@progress <done> <total> <step>`` lines it prints (``total`` 0: a step without a count). In a
terminal a counted step prints how far it got every 25%.
"""

from __future__ import annotations

import time

PREFIX = "@progress "

_lines = False
_state = {"step": None, "time": 0.0, "quarter": 0}


def use_lines(enabled: bool = True):
    global _lines
    _lines = enabled
    _state.update(step=None, time=0.0, quarter=0)


def step(label: str, done: int = 0, total: int = 0):
    """``done`` of the ``total`` items of step ``label`` are finished (``total`` 0: no count)."""
    now = time.monotonic()
    new = label != _state["step"]
    if new:
        _state.update(step=label, quarter=0)
    if _lines:
        # at most 5 updates a second, besides the first and last of a step
        if not new and 0 < done < total and now - _state["time"] < 0.2:
            return
        _state["time"] = now
        print(f"{PREFIX}{done} {total} {label}", flush=True)
        return
    if total > 0:
        quarter = done * 4 // total
        if 0 < quarter < 4 and quarter > _state["quarter"]:
            _state["quarter"] = quarter
            print(f"  {label}: {done * 100 // total}% ({done}/{total})", flush=True)


def parse(line: str):
    """(done, total, step) of an ``@progress`` line, None for other lines."""
    if not line.startswith(PREFIX):
        return None
    parts = line[len(PREFIX) :].rstrip("\r\n").split(" ", 2)
    try:
        return int(parts[0]), int(parts[1]), parts[2] if len(parts) > 2 else ""
    except (ValueError, IndexError):
        return None
