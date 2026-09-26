#include "pch.h"
#include "server_watch.h"

namespace t4
{
namespace sp
{
namespace
{
// The game starts its threads in Sys_ThreadMain(threadContext), the start address of their ExCreateThread
// calls in xenia.log. The Server thread, which runs the level (scripts, AI, entities), has context 4.
typedef DWORD (*Sys_ThreadMain_t)(UINT32 threadContext);
static const Sys_ThreadMain_t Sys_ThreadMain = reinterpret_cast<Sys_ThreadMain_t>(0x82255CD8);
static const UINT32 THREAD_CONTEXT_SERVER = 4;

typedef void (*longjmp_t)(UINT32 *env, int value);

// The game's code, and its image with the strings, in memory.
static const UINT32 CODE_BEGIN = 0x82000000;
static const UINT32 CODE_END = 0x82500000;
static const UINT32 IMAGE_END = 0x82A00000;

static const UINT32 PPC_MFLR_R12 = 0x7D8802A6; // the first instruction of a function that calls others
static const UINT32 PPC_BLR = 0x4E800020;
static const UINT32 PPC_BCTRL = 0x4E800421;
static const UINT32 PPC_BLRL = 0x4E800021;

// The Server thread links the players every frame (20 a second): this long without is a stall. Short, because a
// runaway loop there takes Xenia down within a second.
static const DWORD STALL_MS = 150;
static const int MAX_STALL_REPORTS = 3;
static const int MAX_RETURN_ADDRESSES = 80;
static const int MAX_FUNCTIONS = 24;

Detour Sys_ThreadMain_Detour;
Detour SV_LinkEntity_Detour;
Detour longjmp_Detour;

volatile DWORD g_serverThreadId = 0;
volatile DWORD g_lastBeat = 0;
volatile LONG g_beats = 0;
volatile LONG g_longjmps = 0;
UINT32 g_stackLow = 0;
UINT32 g_stackHigh = 0;
UINT32 *g_snapshot[2] = {nullptr, nullptr};

UINT32 Read(UINT32 address)
{
    return *reinterpret_cast<const volatile UINT32 *>(address);
}

bool IsBranch(UINT32 instruction)
{
    return (instruction & 0xFC000003) == 0x48000000;
}

bool IsBranchAndLink(UINT32 instruction)
{
    return (instruction & 0xFC000003) == 0x48000001;
}

UINT32 BranchTarget(UINT32 address, UINT32 instruction)
{
    INT32 offset = static_cast<INT32>(instruction & 0x03FFFFFC);
    if (offset & 0x02000000)
        offset -= 0x04000000;
    return address + offset;
}

bool InCode(UINT32 address)
{
    return address >= CODE_BEGIN + 4 && address < CODE_END && (address & 3) == 0;
}

// A function starts right after the end of the one before it: a blr, a tail branch or padding.
bool LooksLikeFunctionStart(UINT32 address)
{
    if (!InCode(address))
        return false;
    const UINT32 before = Read(address - 4);
    return before == PPC_BLR || before == 0 || IsBranch(before);
}

// An address a call returns to: the instruction before it is a call.
bool IsReturnAddress(UINT32 value)
{
    if (!InCode(value))
        return false;
    const UINT32 call = Read(value - 4);
    return IsBranchAndLink(call) || call == PPC_BCTRL || call == PPC_BLRL;
}

UINT32 FunctionStart(UINT32 address)
{
    if (!InCode(address))
        return 0;
    for (UINT32 at = address - 4; at >= CODE_BEGIN && address - at < 0x10000; at -= 4)
    {
        if (Read(at) == PPC_MFLR_R12)
            return at;
    }
    return 0;
}

bool CopyString(UINT32 address, char *out, size_t size)
{
    if (address < CODE_BEGIN || address >= IMAGE_END)
        return false;

    const char *text = reinterpret_cast<const char *>(address);
    size_t length = 0;
    bool letter = false;
    for (; length < 200 && text[length]; ++length)
    {
        const char c = text[length];
        if (c == '\n' || c == '\t')
            continue;
        if (c < 0x20 || c > 0x7E)
            return false;
        letter = letter || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }
    if (length < 4 || length == 200 || !letter)
        return false;

    const size_t copied = length < size - 1 ? length : size - 1;
    for (size_t i = 0; i < copied; ++i)
        out[i] = text[i] == '\n' || text[i] == '\t' ? ' ' : text[i];
    out[copied] = '\0';
    return true;
}

// The strings a function uses (lis rX, hi + addi rY, rX, lo): error messages and dvar names tell which one it is.
void LogFunctionStrings(UINT32 function)
{
    UINT32 high[32] = {};
    bool known[32] = {};
    UINT32 shown[3] = {};
    int count = 0;
    for (UINT32 at = function; at < function + 0x1000 && at < CODE_END && count < 3; at += 4)
    {
        const UINT32 instruction = Read(at);
        if (at != function && instruction == PPC_MFLR_R12)
            break; // the next function
        const UINT32 opcode = instruction >> 26;
        const UINT32 rd = (instruction >> 21) & 31;
        const UINT32 ra = (instruction >> 16) & 31;
        if (opcode == 15 && ra == 0)
        {
            high[rd] = instruction << 16;
            known[rd] = true;
        }
        else if (opcode == 14 && ra != 0 && known[ra])
        {
            const UINT32 target = high[ra] + static_cast<INT32>(static_cast<INT16>(instruction & 0xFFFF));
            char text[72];
            if (target != shown[0] && target != shown[1] && CopyString(target, text, sizeof(text)))
            {
                DbgPrint("[codxe][T4 SP]       \"%s\"\n", text);
                shown[count++] = target;
            }
        }
    }
}

bool IsReadable(UINT32 address)
{
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof(info)) != sizeof(info))
        return false;
    return info.State == MEM_COMMIT && info.Protect != 0 && !(info.Protect & PAGE_NOACCESS);
}

// Its top from the stack pointer at its start; its bottom where the pages stop being readable (Xenia puts
// no-access pages on both sides of a thread's stack).
void FindStack(UINT32 stackPointer)
{
    g_stackHigh = (stackPointer + 0xFFF) & ~0xFFFu;
    UINT32 low = g_stackHigh;
    while (g_stackHigh - low < 0x100000 && IsReadable(low - 0x1000))
        low -= 0x1000;
    g_stackLow = low < stackPointer ? low : stackPointer & ~0xFFFu;
}

void ReportStall(DWORD idle)
{
    const UINT32 words = (g_stackHigh - g_stackLow) / 4;
    if (!g_snapshot[0] || !g_snapshot[1])
        return;

    // Twice: what changed in between is where the thread is busy.
    memcpy(g_snapshot[0], reinterpret_cast<const void *>(g_stackLow), words * 4);
    Sleep(10);
    memcpy(g_snapshot[1], reinterpret_cast<const void *>(g_stackLow), words * 4);

    DbgPrint(
        "[codxe][T4 SP] server watch: the Server thread has not linked an entity for %u ms. The calls on its stack "
        "(%08X-%08X), outermost first, * = changed within 10 ms:\n",
        idle, g_stackLow, g_stackHigh);

    UINT32 functions[MAX_FUNCTIONS];
    int functionCount = 0;
    int printed = 0;
    for (UINT32 i = words; i-- > 0 && printed < MAX_RETURN_ADDRESSES;)
    {
        const UINT32 value = g_snapshot[1][i];
        if (!IsReturnAddress(value))
            continue;
        const UINT32 call = Read(value - 4);
        const UINT32 function = FunctionStart(value);
        const UINT32 callee = IsBranchAndLink(call) ? BranchTarget(value - 4, call) : 0;
        DbgPrint("[codxe][T4 SP]   %s %08X: returns to %08X in %08X, calls %08X\n",
                 g_snapshot[0][i] != value ? "*" : " ", g_stackLow + i * 4, value, function, callee);
        ++printed;

        bool seen = function == 0;
        for (int f = 0; f < functionCount && !seen; ++f)
            seen = functions[f] == function;
        if (!seen && functionCount < MAX_FUNCTIONS)
            functions[functionCount++] = function;
    }

    DbgPrint("[codxe][T4 SP] server watch: text those functions use:\n");
    for (int f = 0; f < functionCount; ++f)
    {
        DbgPrint("[codxe][T4 SP]     %08X\n", functions[f]);
        LogFunctionStrings(functions[f]);
    }
    DbgPrint("[codxe][T4 SP] server watch: end of report\n");
}

DWORD WINAPI WatchThread(void *)
{
    LONG reportedAt = -1;
    for (int reports = 0; reports < MAX_STALL_REPORTS;)
    {
        Sleep(10);
        const LONG beats = g_beats;
        if (beats < 5 || beats == reportedAt)
            continue; // the level is not running yet, or this stall was reported
        const DWORD idle = GetTickCount() - g_lastBeat;
        if (idle < STALL_MS)
            continue;
        reportedAt = beats;
        ++reports;
        ReportStall(idle);
    }
    return 0;
}

void WatchServerThread()
{
    volatile UINT32 marker = 0;
    FindStack(reinterpret_cast<UINT32>(&marker));
    const UINT32 bytes = g_stackHigh - g_stackLow;
    g_snapshot[0] = static_cast<UINT32 *>(malloc(bytes));
    g_snapshot[1] = static_cast<UINT32 *>(malloc(bytes));
    g_serverThreadId = GetCurrentThreadId();
    DbgPrint("[codxe][T4 SP] server watch: Server thread %08X, stack %08X-%08X\n", g_serverThreadId, g_stackLow,
             g_stackHigh);

    HANDLE thread = nullptr;
    if (NT_SUCCESS(ExCreateThread(&thread, 0, nullptr, nullptr, WatchThread, nullptr, EX_CREATE_FLAG_TITLE_EXEC)))
        CloseHandle(thread);
    else
        DbgPrint("[codxe][T4 SP] server watch: could not start the watch thread\n");
}

DWORD Sys_ThreadMain_Hook(UINT32 threadContext)
{
    if (threadContext == THREAD_CONTEXT_SERVER && !g_serverThreadId)
        WatchServerThread();
    return Sys_ThreadMain_Detour.GetOriginal<Sys_ThreadMain_t>()(threadContext);
}

void SV_LinkEntity_Hook(gentity_s *gEnt)
{
    if (GetCurrentThreadId() == g_serverThreadId)
    {
        g_lastBeat = GetTickCount();
        InterlockedIncrement(&g_beats);
    }
    SV_LinkEntity_Detour.GetOriginal<decltype(SV_LinkEntity)>()(gEnt);
}

// Where it came from and where it goes: the jump buffer holds the return address of its setjmp.
void longjmp_Hook(UINT32 *env, int value)
{
    const LONG count = InterlockedIncrement(&g_longjmps);
    if (count <= 32 || (count & 255) == 0)
    {
        const UINT32 from = reinterpret_cast<UINT32>(_ReturnAddress());
        UINT32 to = 0;
        for (int i = 0; i < 64 && !to; ++i)
        {
            if (IsReturnAddress(env[i]))
                to = env[i];
        }
        DbgPrint("[codxe][T4 SP] longjmp %d on thread %08X: from %08X (in %08X) to %08X (in %08X)\n", count,
                 GetCurrentThreadId(), from, FunctionStart(from), to, FunctionStart(to));
    }
    longjmp_Detour.GetOriginal<longjmp_t>()(env, value);
}

// longjmp loads the stack pointer from the jump buffer: lwz/ld r1, d(rA). A function's epilogue only reloads r1
// from r1.
bool LoadsStackPointer(UINT32 instruction)
{
    const UINT32 opcode = instruction >> 26;
    const UINT32 rd = (instruction >> 21) & 31;
    const UINT32 ra = (instruction >> 16) & 31;
    return rd == 1 && ra > 1 && (opcode == 32 || (opcode == 58 && (instruction & 3) == 0));
}

// Scr_Error ends in a longjmp back into the script VM: follow its calls to the function that loads r1.
UINT32 FindLongjmp(UINT32 function, int depth, UINT32 *visited, int &visitedCount)
{
    if (!InCode(function) || visitedCount == 96)
        return 0;
    for (int i = 0; i < visitedCount; ++i)
    {
        if (visited[i] == function)
            return 0;
    }
    visited[visitedCount++] = function;

    UINT32 callees[16];
    int calleeCount = 0;
    for (UINT32 at = function; at < function + 0x800 && at < CODE_END; at += 4)
    {
        const UINT32 instruction = Read(at);
        if (at != function && instruction == PPC_MFLR_R12)
            break; // the next function
        if (LoadsStackPointer(instruction))
            return function;
        if ((IsBranchAndLink(instruction) || IsBranch(instruction)) && calleeCount < 16)
            callees[calleeCount++] = BranchTarget(at, instruction);
    }
    for (int i = 0; depth > 0 && i < calleeCount; ++i)
    {
        const UINT32 found = FindLongjmp(callees[i], depth - 1, visited, visitedCount);
        if (found)
            return found;
    }
    return 0;
}
} // namespace

server_watch::server_watch()
{
    if (!Config::log_console)
        return;

    const UINT32 threadMain = reinterpret_cast<UINT32>(Sys_ThreadMain);
    const UINT32 linkEntity = reinterpret_cast<UINT32>(SV_LinkEntity);
    if (LooksLikeFunctionStart(threadMain) && LooksLikeFunctionStart(linkEntity))
    {
        Sys_ThreadMain_Detour = Detour(Sys_ThreadMain, Sys_ThreadMain_Hook);
        Sys_ThreadMain_Detour.Install();
        SV_LinkEntity_Detour = Detour(SV_LinkEntity, SV_LinkEntity_Hook);
        SV_LinkEntity_Detour.Install();
        DbgPrint("[codxe][T4 SP] log_console: Server thread watched (Sys_ThreadMain at %08X, SV_LinkEntity at %08X)\n",
                 threadMain, linkEntity);
    }
    else
    {
        DbgPrint("[codxe][T4 SP] log_console: %08X or %08X does not look like the start of Sys_ThreadMain or "
                 "SV_LinkEntity, the Server thread is not watched\n",
                 threadMain, linkEntity);
    }

    UINT32 visited[96];
    int visitedCount = 0;
    const UINT32 longjmpAddress = FindLongjmp(reinterpret_cast<UINT32>(Scr_Error), 4, visited, visitedCount);
    if (longjmpAddress && LooksLikeFunctionStart(longjmpAddress))
    {
        longjmp_Detour = Detour(reinterpret_cast<void *>(longjmpAddress), longjmp_Hook);
        longjmp_Detour.Install();
        DbgPrint("[codxe][T4 SP] log_console: longjmps logged (longjmp at %08X)\n", longjmpAddress);
    }
    else
    {
        DbgPrint("[codxe][T4 SP] log_console: longjmp not found from Scr_Error (%08X, %d functions looked at)\n",
                 longjmpAddress, visitedCount);
    }
}

server_watch::~server_watch()
{
    longjmp_Detour.Remove();
    SV_LinkEntity_Detour.Remove();
    Sys_ThreadMain_Detour.Remove();
}
} // namespace sp
} // namespace t4
