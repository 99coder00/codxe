#pragma once

#include <string>

#include "core/zone.h"

// A canonical text form of a zone's node tree: every node, its block, offset, segments, origin and
// pointers (the Python reader's dump has the same lines, native/tools/py_reference.py dump).
namespace t4ff
{
std::string dump_text(Zone *zone, size_t *node_count = nullptr);
} // namespace t4ff
