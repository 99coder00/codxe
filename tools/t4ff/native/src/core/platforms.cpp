#include "core/platforms.h"

#include "core/resources.h"

namespace t4ff
{
namespace
{
std::vector<std::string> asset_types(bool x360)
{
    size_t count;
    const char *const *names = pc_asset_types(&count);
    std::vector<std::string> types(names, names + count);
    if (x360)
        types.insert(types.begin() + 7, "pixelshader");
    return types;
}

std::unique_ptr<Platform> build(bool x360)
{
    auto layout = Layout::from_json(resource(x360 ? Resource::LayoutX360 : Resource::LayoutPc), x360 ? "x360" : "pc");
    auto cmds = std::make_unique<Commands>();
    CommandParser parser(*layout, *cmds, x360);
    parser.parse_text(resource(x360 ? Resource::CommandsX360 : Resource::CommandsPc));
    auto p = std::make_unique<Platform>(x360 ? "x360" : "pc", x360, std::move(layout), std::move(cmds), asset_types(x360),
                                        std::vector<int>{BLOCK_TEMP, BLOCK_VIRTUAL, BLOCK_LARGE, BLOCK_PHYSICAL});
    p->prepare();
    return p;
}
} // namespace

const Platform &pc()
{
    static const std::unique_ptr<Platform> p = build(false);
    return *p;
}

const Platform &x360()
{
    static const std::unique_ptr<Platform> p = build(true);
    return *p;
}
} // namespace t4ff
