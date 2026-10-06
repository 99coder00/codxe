#pragma once

#include <string_view>

namespace t4ff
{
// Data compiled into the executable (resources.rc): ids of the RCDATA resources.
enum class Resource
{
    LayoutPc = 101,
    LayoutX360 = 102,
    CommandsPc = 103,
    CommandsX360 = 104,
    VerifiedX360 = 105, // console record layouts verified against real fastfiles
};

// The bytes of a resource of the running executable (they live as long as the process).
std::string_view resource(Resource id);
} // namespace t4ff
