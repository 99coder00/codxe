#include "pch.h"
#include "common/gsc_loader.h"
#include "scr_parser.h"

namespace t4
{
namespace sp
{
Detour Scr_AddSourceBuffer_Detour;

// A usermap's own versions of the game's scripts (usermaps/<map>/scripts/maps/_load.gsc, ...), which
// t4ff writes there: on PC a mod's loose scripts win over the game's zones, on the console the game's
// zones load first and theirs would win. The active mod's scripts still come first.
char *TryLoadUsermapScript(const char *extFilename)
{
    const char *mapName = Dvar_GetVariantString("mapname");
    if (!extFilename || !mapName || !*mapName || std::strpbrk(mapName, "/\\:.") != nullptr)
        return nullptr;

    const std::string usermaps = Config::ResolveDataDirectory("usermaps");
    if (usermaps.empty())
        return nullptr;

    const std::string scripts =
        filesystem::JoinPath(filesystem::JoinPath(usermaps.c_str(), mapName).c_str(), "scripts");
    const std::string path = filesystem::JoinPath(scripts.c_str(), extFilename);
    return gsc_loader::TryLoadOverride(extFilename, path.c_str(), Hunk_AllocateTempMemoryHighInternal);
}

char *Scr_AddSourceBuffer_Hook(scriptInstance_t inst, const char *filename, const char *extFilename,
                               const char *codePos, bool archive)
{
    auto callOriginal = [&]()
    {
        return Scr_AddSourceBuffer_Detour.GetOriginal<decltype(Scr_AddSourceBuffer)>()(inst, filename, extFilename,
                                                                                       codePos, archive);
    };

    if (Config::dump_rawfile)
    {
        char *contents = callOriginal();
        gsc_loader::DumpSource(extFilename, contents);
        return contents;
    }

    char *contents = gsc_loader::TryLoadOverride(extFilename, Hunk_AllocateTempMemoryHighInternal);
    if (!contents)
        contents = TryLoadUsermapScript(extFilename);
    return contents ? contents : callOriginal();
}

scr_parser::scr_parser()
{
    Scr_AddSourceBuffer_Detour = Detour(Scr_AddSourceBuffer, Scr_AddSourceBuffer_Hook);
    Scr_AddSourceBuffer_Detour.Install();
}

scr_parser::~scr_parser()
{
    Scr_AddSourceBuffer_Detour.Remove();
}
} // namespace sp
} // namespace t4
