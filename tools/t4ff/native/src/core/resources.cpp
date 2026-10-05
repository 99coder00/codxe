#include "core/resources.h"

#include <stdexcept>
#include <string>

#include <windows.h>

namespace t4ff
{
std::string_view resource(Resource id)
{
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(static_cast<int>(id)), MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (!found)
        throw std::runtime_error("resource " + std::to_string(static_cast<int>(id)) + " is missing from the executable");
    HGLOBAL loaded = LoadResource(module, found);
    const void *data = loaded ? LockResource(loaded) : nullptr;
    if (!data)
        throw std::runtime_error("resource " + std::to_string(static_cast<int>(id)) + " cannot be loaded");
    return std::string_view(static_cast<const char *>(data), SizeofResource(module, found));
}
} // namespace t4ff
