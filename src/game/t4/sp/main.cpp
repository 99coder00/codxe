#include "pch.h"
#include "components/clipmap.h"
#include "components/console.h"
#include "components/fastfiles.h"
#include "components/gsc_fields.h"
#include "components/gsc.h"
#include "components/scr_parser.h"
#include "components/streaming.h"
#include "components/thread_watch.h"
#include "components/ui.h"
#include "components/usermaps.h"
#include "main.h"

namespace t4
{
namespace sp
{

Detour R_GetTextureFromCode_Detour;

uint32_t R_GetTextureFromCode_Hook(uint32_t source, unsigned int codeTexture, uint8_t *sampler)
{
    // TODO: track down the techset issue in the assets and remove this bandaid
    const auto image =
        R_GetTextureFromCode_Detour.GetOriginal<decltype(R_GetTextureFromCode)>()(source, codeTexture, sampler);
    if (image || codeTexture != 18)
        return image;

    // TU7 normally binds $white for dynamic shadows when shadow cookies are off. Some draws reach this
    // lookup before that binding, so supply the same fallback only when the slot is actually empty.
    const auto *scEnable = *reinterpret_cast<const uint8_t *const *>(0x84F1EFC4);
    if (!scEnable || scEnable[16] || !source)
        return image;

    const auto whiteImage = *reinterpret_cast<const uint32_t *>(0x84B58308);
    if (!whiteImage)
        return image;

    *reinterpret_cast<uint32_t *>(source + 4056) = whiteImage;
    static bool loggedFallback = false;
    if (!loggedFallback)
    {
        loggedFallback = true;
        DbgPrint("[codxe][T4 SP][Renderer] Bound $white to missing dynamic-shadow code image\n");
    }
    return whiteImage;
}

// D3D's vertex shader binding (TU7 8237D8B0: r3 the shader, r5 the vertex declaration, r7 its bound variant)
// patches the shader's vertex fetches with the declaration's elements. The game sets no declaration when a
// pass has a vertex shader of the vertex type it draws: Treyarch compiled those for that type, with
// nothing left to bind. Shaders compiled by others for converted maps (CoD Xenon's mc_ambient_t0c0, on
// Kino Rezurrection's dry grass) still have fetches to bind, and D3D then reads the element count at
// address 0x18 and loops over memory forever: the game froze at its first frame. An empty declaration
// binds each fetch to D3D's own default for an input a declaration lacks; the draw is wrong, not fatal.
typedef void (*D3D_BindVertexShader_t)(uint32_t shader, uint32_t r4, uint32_t declaration, uint32_t r6, uint32_t variant,
                                       uint32_t r8, uint32_t r9, uint32_t r10);
Detour D3D_BindVertexShader_Detour;
__declspec(align(16)) uint8_t emptyVertexDeclaration[128] = {};

void D3D_BindVertexShader_Hook(uint32_t shader, uint32_t r4, uint32_t declaration, uint32_t r6, uint32_t variant,
                               uint32_t r8, uint32_t r9, uint32_t r10)
{
    if (!declaration)
    {
        // once a shader (the first 16: a map draws them every frame)
        static uint32_t loggedShaders[16] = {};
        for (int i = 0; i < 16; ++i)
        {
            if (loggedShaders[i] == shader)
                break;
            if (!loggedShaders[i])
            {
                loggedShaders[i] = shader;
                DbgPrint("[codxe][T4 SP][Renderer] Vertex shader %08X bound without a vertex declaration: an empty one used\n",
                         shader);
                break;
            }
        }
        declaration = reinterpret_cast<uint32_t>(emptyVertexDeclaration);
    }
    D3D_BindVertexShader_Detour.GetOriginal<D3D_BindVertexShader_t>()(shader, r4, declaration, r6, variant, r8, r9, r10);
}

T4_SP_Plugin::T4_SP_Plugin()
{
    D3D_BindVertexShader_Detour = Detour(reinterpret_cast<void *>(0x8237D8B0), D3D_BindVertexShader_Hook);
    D3D_BindVertexShader_Detour.Install();

    R_GetTextureFromCode_Detour = Detour(R_GetTextureFromCode, R_GetTextureFromCode_Hook);
    R_GetTextureFromCode_Detour.Install();

    // Default loc_warnings off to prevent console spam
    *(volatile uint8_t *)0x8225FA17 = 0x00;

    // Default ui_autoContinue on so level loads do not wait for input in solo or split-screen.
    *(volatile uint8_t *)0x82279B07 = 0x01;

    // Send every view model notetrack to client scripts (level notify "notetrack"), not only those starting
    // with "tesla_": mods' weapons wait for their own (the Thundergun's "thundergun_fire_start" plays its
    // effects). T4M does the same on PC, and maps made for it rely on it. This is the branch taken when
    // strnicmp(note, "tesla_", 6) differs.
    *(volatile uint32_t *)0x82141FB8 = 0x60000000;

    RegisterModule(new Config(Config::GAME_T4));
    // First: its hooks must be in place before the game starts its threads.
    RegisterModule(new thread_watch());
    RegisterModule(new FastFiles());
    RegisterModule(new clipmap());
    RegisterModule(new console());
    RegisterModule(new GSC());
    RegisterModule(new GSCFields());
    RegisterModule(new scr_parser());
    RegisterModule(new Streaming());
    RegisterModule(new ui());
    RegisterModule(new UsermapList());
}

} // namespace sp
} // namespace t4
