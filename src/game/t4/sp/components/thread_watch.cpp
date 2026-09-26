#include "pch.h"
#include "thread_watch.h"

namespace t4
{
namespace sp
{
namespace
{
// The game starts its threads in Sys_ThreadMain(threadContext), the start address of their ExCreateThread
// calls in xenia.log. The Server thread, which runs the level's frames (scripts, AI, entities), has context 4.
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

static const int MAX_STALL_REPORTS = 3;
static const int MAX_STACK_CALLS = 1024; // return addresses read from a stack
static const int MAX_CHAIN = 96;         // calls printed per thread
static const int MAX_OTHER_CHANGED = 16; // and changed return addresses that are not among them
static const int MAX_FUNCTIONS = 48;
static const int MAX_THREADS = 16;
static const UINT32 STACK_BLOCK = 256;    // bytes of a stack compared at once
static const int MAX_STACK_BLOCKS = 4096; // 1 MB, the most FindStack finds

struct WatchedThread
{
    const char *name;
    const char *progress; // what it does when it makes progress
    DWORD stallMs;        // this long without progress is a stall
    LONG minBeats;        // progress made before stalls count
    volatile LONG id;
    UINT32 stackLow;
    UINT32 stackHigh;
    volatile DWORD lastBeat;
    volatile LONG beats;
};

// The Server thread links the players every frame (20 a second). Short, because a runaway loop there takes
// Xenia down within a second.
WatchedThread g_server = {"Server", "linked an entity", 150, 5, 0, 0, 0, 0, 0};
// The main thread loads the level (it spawns its entities: the first thread besides the Server thread to link
// one) and then runs the client frames. Loading also waits a while for the fastfile and stream threads.
WatchedThread g_main = {"main", "printed or run a client frame", 3000, 1, 0, 0, 0, 0, 0};
volatile LONG g_mainFound = 0;

// Every game thread's stack, written out with a stall's report: a thread that waits usually waits for another.
struct ThreadStack
{
    const char *name;
    UINT32 context;
    volatile LONG id;
    UINT32 stackLow;
    UINT32 stackHigh;
};

ThreadStack g_threads[MAX_THREADS];
volatile LONG g_threadSlots = 0;

volatile LONG g_longjmps = 0;

Detour Sys_ThreadMain_Detour;
Detour SV_LinkEntity_Detour;
Detour longjmp_Detour;

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

// The function a call went to, 0 when it went through a pointer (bctrl, blrl).
UINT32 Callee(UINT32 returnAddress)
{
    const UINT32 call = Read(returnAddress - 4);
    return IsBranchAndLink(call) ? BranchTarget(returnAddress - 4, call) : 0;
}

// The code at address is in function when no other function starts (mflr r12) in between. Not function
// itself: CoD Xe's hooks overwrite the start of the functions they hook.
bool IsWithin(UINT32 address, UINT32 function)
{
    if (!InCode(function) || address <= function || address - function > 0x10000)
        return false;
    for (UINT32 at = address - 4; at > function; at -= 4)
    {
        if (Read(at) == PPC_MFLR_R12)
            return false;
    }
    return true;
}

// A call to callee runs the code at address: in callee, or in a function callee ends by jumping to (a tail
// call: b instead of bl, to before callee or past its end).
bool RunsIn(UINT32 address, UINT32 callee, int depth)
{
    if (IsWithin(address, callee))
        return true;
    if (depth == 0 || !InCode(callee))
        return false;

    UINT32 end = callee + 4;
    while (end < callee + 0x2000 && end < CODE_END && Read(end) != PPC_MFLR_R12)
        end += 4;
    for (UINT32 at = callee; at < end; at += 4)
    {
        const UINT32 instruction = Read(at);
        if (!IsBranch(instruction))
            continue;
        const UINT32 target = BranchTarget(at, instruction);
        if ((target < callee || target >= end) && RunsIn(address, target, depth - 1))
            return true;
    }
    return false;
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

// The pages around the stack pointer that can be read: Xenia puts no-access pages on both sides of a thread's
// stack.
void FindStack(UINT32 stackPointer, UINT32 &low, UINT32 &high)
{
    high = (stackPointer + 0x1000) & ~0xFFFu;
    while (high - stackPointer < 0x100000 && IsReadable(high))
        high += 0x1000;
    low = stackPointer & ~0xFFFu;
    while (high - low < 0x100000 && IsReadable(low - 0x1000))
        low -= 0x1000;
}

// On the thread itself, once.
void StartWatching(WatchedThread &thread)
{
    volatile UINT32 marker = 0;
    FindStack(reinterpret_cast<UINT32>(&marker), thread.stackLow, thread.stackHigh);
    thread.lastBeat = GetTickCount();
    InterlockedExchange(&thread.id, static_cast<LONG>(GetCurrentThreadId()));
    DbgPrint("[codxe][T4 SP] thread watch: %s thread %08X, stack %08X-%08X\n", thread.name, thread.id, thread.stackLow,
             thread.stackHigh);
}

const char *ContextName(UINT32 context)
{
    static const char *const names[] = {"main",      "Backend",   "Worker0",  "Worker1", "Server",
                                        "occlusion", "Cinematic", "Database", "Stream"};
    return context < sizeof(names) / sizeof(names[0]) ? names[context] : "game";
}

// On the thread itself, once.
void RegisterThread(UINT32 context, UINT32 stackLow, UINT32 stackHigh)
{
    const LONG slot = InterlockedIncrement(&g_threadSlots) - 1;
    if (slot >= MAX_THREADS)
        return;
    ThreadStack &thread = g_threads[slot];
    thread.name = ContextName(context);
    thread.context = context;
    thread.stackLow = stackLow;
    thread.stackHigh = stackHigh;
    InterlockedExchange(&thread.id, static_cast<LONG>(GetCurrentThreadId()));
}

void Beat(WatchedThread &thread)
{
    thread.lastBeat = GetTickCount();
    InterlockedIncrement(&thread.beats);
}

// A return address on a stack, and where.
struct StackCall
{
    UINT32 at;
    UINT32 value;
};

// Used by the watch thread only.
StackCall g_before[MAX_STACK_CALLS];
StackCall g_after[MAX_STACK_CALLS];
bool g_changed[MAX_STACK_CALLS];
bool g_inChain[MAX_STACK_CALLS];
int g_chain[MAX_CHAIN];
bool g_chainGuessed[MAX_CHAIN];
UINT32 g_blocksBefore[MAX_STACK_BLOCKS];

// Read from a thread's live stack, outermost first.
int ReadCalls(UINT32 stackLow, UINT32 stackHigh, StackCall *calls)
{
    int count = 0;
    for (UINT32 at = stackHigh; at > stackLow && count < MAX_STACK_CALLS;)
    {
        at -= 4;
        const UINT32 value = Read(at);
        if (IsReturnAddress(value))
        {
            calls[count].at = at;
            calls[count].value = value;
            ++count;
        }
    }
    return count;
}

// A function saves the return address into its caller 8 bytes below the caller's stack pointer, and its own
// frame starts with the caller's stack pointer (the back chain). So the return address at lower was saved by a
// function called from the one whose return address is at upper when the back chain from lower + 8 leads to
// upper + 8: the stack links followed (more than one: CoD Xe's hooks have frames with no return address into the
// game), 0 when it does not.
int LinkHops(UINT32 lower, UINT32 upper, UINT32 stackLow, UINT32 stackHigh)
{
    UINT32 stackPointer = lower + 8;
    for (int hop = 1; hop <= 4; ++hop)
    {
        if (stackPointer < stackLow || stackPointer >= stackHigh || (stackPointer & 3) != 0)
            return 0;
        const UINT32 backChain = Read(stackPointer);
        if (backChain == upper + 8)
            return hop;
        if (backChain <= stackPointer)
            return 0;
        stackPointer = backChain;
    }
    return 0;
}

int CheckedDepth(int index, int count, UINT32 stackLow, UINT32 stackHigh);

// The next call below the one at index upper, -1 when there is none. It links back to it and returns into the
// function that call went to (or one that function tail-calls): an earlier call can leave a return address that
// links back too in the space a function has not used yet, but it returns into another function. When the
// function a direct call went to has no call of its own on the stack, the thread is in it. guessed, when the call
// went through a pointer: of those that link back, the one linked most directly, then the one with the most
// checked calls below it.
int NextCall(int upper, int count, UINT32 stackLow, UINT32 stackHigh, bool guess, bool &guessed)
{
    const UINT32 callee = Callee(g_after[upper].value);
    int best = -1;
    int bestHops = 5;
    int bestChecked = -1;
    for (int next = upper + 1; next < count; ++next)
    {
        const int hops = LinkHops(g_after[next].at, g_after[upper].at, stackLow, stackHigh);
        if (hops == 0)
            continue;
        if (callee)
        {
            if (RunsIn(g_after[next].value, callee, 2))
            {
                guessed = false;
                return next;
            }
            continue;
        }
        if (!guess || hops > bestHops)
            continue;
        const int checked = CheckedDepth(next, count, stackLow, stackHigh);
        if (hops < bestHops || checked > bestChecked)
        {
            best = next;
            bestHops = hops;
            bestChecked = checked;
        }
    }
    guessed = best >= 0;
    return best;
}

// How many calls below the one at index each return into the function the one above them called.
int CheckedDepth(int index, int count, UINT32 stackLow, UINT32 stackHigh)
{
    int depth = 0;
    bool guessed = false;
    while (depth < MAX_CHAIN)
    {
        index = NextCall(index, count, stackLow, stackHigh, false, guessed);
        if (index < 0)
            break;
        ++depth;
    }
    return depth;
}

// The calls the thread is in, outermost first, from the return addresses on its stack (g_after), starting at the
// outermost (its thread's start).
int WalkCalls(int count, UINT32 stackLow, UINT32 stackHigh)
{
    if (count == 0)
        return 0;
    int length = 0;
    g_chain[length] = 0;
    g_chainGuessed[length++] = false;
    while (length < MAX_CHAIN)
    {
        bool guessed = false;
        const int next = NextCall(g_chain[length - 1], count, stackLow, stackHigh, true, guessed);
        if (next < 0)
            break;
        g_chain[length] = next;
        g_chainGuessed[length++] = guessed;
    }
    return length;
}

void AddFunction(UINT32 function, UINT32 *functions, int &functionCount)
{
    bool seen = function == 0;
    for (int f = 0; f < functionCount && !seen; ++f)
        seen = functions[f] == function;
    if (!seen && functionCount < MAX_FUNCTIONS)
        functions[functionCount++] = function;
}

UINT32 BlockSum(UINT32 at)
{
    UINT32 sum = 0;
    for (UINT32 offset = 0; offset < STACK_BLOCK; offset += 4)
        sum = ((sum << 5) | (sum >> 27)) ^ Read(at + offset);
    return sum;
}

int StackBlocks(UINT32 stackLow, UINT32 stackHigh)
{
    const UINT32 blocks = (stackHigh - stackLow) / STACK_BLOCK;
    return blocks < MAX_STACK_BLOCKS ? static_cast<int>(blocks) : MAX_STACK_BLOCKS;
}

// Xenia's DbgPrint reads 7 values at most (the registers): a report line has no more.
void LogWords(const char *what, UINT32 from, UINT32 to)
{
    DbgPrint("[codxe][T4 SP]   %s, %08X-%08X:\n", what, from, to);
    for (UINT32 at = from; at < to; at += 24)
    {
        UINT32 words[6] = {};
        for (UINT32 i = 0; i < 6 && at + i * 4 < to; ++i)
            words[i] = Read(at + i * 4);
        DbgPrint("[codxe][T4 SP]     %08X: %08X %08X %08X %08X %08X %08X\n", at, words[0], words[1], words[2], words[3],
                 words[4], words[5]);
    }
}

// The code of a function up to the next one (mflr r12), and of the functions it ends by jumping to, to be
// disassembled.
void LogCode(UINT32 function, int depth)
{
    if (!InCode(function))
        return;
    UINT32 end = function + 4;
    while (end < function + 0x800 && end < CODE_END && Read(end) != PPC_MFLR_R12)
        end += 4;
    LogWords("code", function, end);
    for (UINT32 at = function; depth > 0 && at < end; at += 4)
    {
        const UINT32 instruction = Read(at);
        const UINT32 target = BranchTarget(at, instruction);
        if (IsBranch(instruction) && (target < function || target >= end))
            LogCode(target, depth - 1);
    }
}

// The code of each function the code of function calls (bl), and of those they end by jumping to.
void LogCallees(UINT32 function)
{
    UINT32 end = function + 4;
    while (end < function + 0x800 && end < CODE_END && Read(end) != PPC_MFLR_R12)
        end += 4;
    UINT32 logged[8];
    int loggedCount = 0;
    for (UINT32 at = function; at < end && loggedCount < 8; at += 4)
    {
        const UINT32 instruction = Read(at);
        if (!IsBranchAndLink(instruction))
            continue;
        const UINT32 target = BranchTarget(at, instruction);
        bool seen = !InCode(target);
        for (int i = 0; i < loggedCount && !seen; ++i)
            seen = logged[i] == target;
        if (seen)
            continue;
        logged[loggedCount++] = target;
        DbgPrint("[codxe][T4 SP]   a function it calls, %08X:\n", target);
        LogCode(target, 2);
    }
}

void PrintCall(int index, bool guessed)
{
    const UINT32 value = g_after[index].value;
    DbgPrint("[codxe][T4 SP]   %s%s %08X: returns to %08X in %08X, calls %08X\n", g_changed[index] ? "*" : " ",
             guessed ? "?" : " ", g_after[index].at, value, FunctionStart(value), Callee(value));
}

// everything: also the code of the function it is in, the stack around the call to it and every return address on
// the stack with the stack link above it, to work out by hand.
void ReportThread(const char *name, UINT32 context, LONG id, UINT32 stackLow, UINT32 stackHigh, bool everything,
                  UINT32 *functions, int &functionCount)
{
    // Twice: what changed in between is where the thread runs. Nothing changed: it waits (or loops without
    // calling anything or using its stack).
    const int blocks = StackBlocks(stackLow, stackHigh);
    for (int b = 0; b < blocks; ++b)
        g_blocksBefore[b] = BlockSum(stackLow + b * STACK_BLOCK);
    const int beforeCount = ReadCalls(stackLow, stackHigh, g_before);
    Sleep(100);
    const int afterCount = ReadCalls(stackLow, stackHigh, g_after);
    int changedBlocks = 0;
    UINT32 lowestChange = 0;
    for (int b = 0; b < blocks; ++b)
    {
        if (BlockSum(stackLow + b * STACK_BLOCK) == g_blocksBefore[b])
            continue;
        if (changedBlocks++ == 0)
            lowestChange = stackLow + b * STACK_BLOCK;
    }

    int changedCount = 0;
    for (int a = 0, b = 0; a < afterCount; ++a)
    {
        while (b < beforeCount && g_before[b].at > g_after[a].at)
            ++b;
        g_changed[a] = !(b < beforeCount && g_before[b].at == g_after[a].at && g_before[b].value == g_after[a].value);
        g_inChain[a] = false;
        changedCount += g_changed[a] ? 1 : 0;
    }

    const int chainLength = WalkCalls(afterCount, stackLow, stackHigh);
    DbgPrint("[codxe][T4 SP] thread watch: %s thread (context %u, %08X, stack %08X-%08X): %s\n", name, context, id,
             stackLow, stackHigh, changedBlocks ? "runs" : "waits");
    DbgPrint("[codxe][T4 SP]   within 100 ms, %d of its %d return addresses and %d of its %d 256-byte stack blocks "
             "changed, the lowest at %08X. The calls it is in, outermost first, the last one to the function it is "
             "in (* = changed, ? = guessed: a call through a pointer):\n",
             changedCount, afterCount, changedBlocks, blocks, lowestChange);
    for (int i = 0; i < chainLength; ++i)
    {
        g_inChain[g_chain[i]] = true;
        PrintCall(g_chain[i], g_chainGuessed[i]);
        AddFunction(FunctionStart(g_after[g_chain[i]].value), functions, functionCount);
    }

    int othersPrinted = 0;
    for (int a = 0; a < afterCount && othersPrinted < MAX_OTHER_CHANGED; ++a)
    {
        if (!g_changed[a] || g_inChain[a])
            continue;
        if (othersPrinted++ == 0)
            DbgPrint("[codxe][T4 SP]   changed, not among those:\n");
        PrintCall(a, false);
    }

    if (!everything || chainLength == 0)
        return;

    // The innermost call: the function it went to (the one the thread is in) and those it calls, the call and
    // the stack around it.
    const StackCall &innermost = g_after[g_chain[chainLength - 1]];
    const UINT32 function = Callee(innermost.value);
    DbgPrint("[codxe][T4 SP]   the innermost call, from %08X to %08X (0: through a pointer):\n", innermost.value - 4,
             function);
    LogCode(function, 2);
    LogCallees(function);
    LogWords("the code around the call", innermost.value - 0x60, innermost.value + 0x18);
    const UINT32 stackPointer = innermost.at + 8; // of the function called
    LogWords("the stack below and above the stack pointer it was called with",
             stackPointer - 0x180 > stackLow ? stackPointer - 0x180 : stackLow,
             stackPointer + 0x60 < stackHigh ? stackPointer + 0x60 : stackHigh);

    DbgPrint("[codxe][T4 SP]   every return address on its stack, outermost first (where, return address, in "
             "function, calls, the stack link at where + 8):\n");
    for (int a = 0; a < afterCount; ++a)
    {
        const UINT32 at = g_after[a].at;
        const UINT32 value = g_after[a].value;
        DbgPrint("[codxe][T4 SP]     %08X %08X %08X %08X %08X\n", at, value, FunctionStart(value), Callee(value),
                 at + 8 < stackHigh ? Read(at + 8) : 0);
    }
}

void ReportStall(const WatchedThread &thread, DWORD idle)
{
    DbgPrint("[codxe][T4 SP] thread watch: the %s thread has not %s for %u ms. Where the game's threads are:\n",
             thread.name, thread.progress, idle);

    UINT32 functions[MAX_FUNCTIONS];
    int functionCount = 0;
    ReportThread(thread.name, &thread == &g_server ? THREAD_CONTEXT_SERVER : 0, thread.id, thread.stackLow,
                 thread.stackHigh, true, functions, functionCount);
    for (int t = 0; t < MAX_THREADS; ++t)
    {
        const ThreadStack &other = g_threads[t];
        if (other.id && other.id != thread.id)
            ReportThread(other.name, other.context, other.id, other.stackLow, other.stackHigh, false, functions,
                         functionCount);
    }

    DbgPrint("[codxe][T4 SP] thread watch: text those functions use:\n");
    for (int f = 0; f < functionCount; ++f)
    {
        DbgPrint("[codxe][T4 SP]     %08X\n", functions[f]);
        LogFunctionStrings(functions[f]);
    }
    DbgPrint("[codxe][T4 SP] thread watch: end of report\n");
}

DWORD WINAPI WatchThread(void *)
{
    const int threadCount = 2;
    WatchedThread *const threads[threadCount] = {&g_server, &g_main};
    LONG reportedAt[threadCount] = {-1, -1};
    int reports[threadCount] = {0, 0};
    for (int done = 0; done < threadCount;)
    {
        Sleep(10);
        done = 0;
        for (int i = 0; i < threadCount; ++i)
        {
            WatchedThread &thread = *threads[i];
            if (reports[i] == MAX_STALL_REPORTS)
            {
                ++done;
                continue;
            }
            const LONG beats = thread.beats;
            if (!thread.id || beats < thread.minBeats || beats == reportedAt[i])
                continue;                           // not started yet, or this stall was reported
            const DWORD lastBeat = thread.lastBeat; // before the time: a later beat must not make it negative
            const DWORD idle = GetTickCount() - lastBeat;
            if (idle < thread.stallMs)
                continue;
            reportedAt[i] = beats;
            ++reports[i];
            ReportStall(thread, idle);
        }
    }
    return 0;
}

DWORD Sys_ThreadMain_Hook(UINT32 threadContext)
{
    volatile UINT32 marker = 0;
    UINT32 stackLow = 0;
    UINT32 stackHigh = 0;
    FindStack(reinterpret_cast<UINT32>(&marker), stackLow, stackHigh);
    RegisterThread(threadContext, stackLow, stackHigh);

    if (threadContext == THREAD_CONTEXT_SERVER && !g_server.id)
    {
        StartWatching(g_server);

        HANDLE thread = nullptr;
        if (NT_SUCCESS(ExCreateThread(&thread, 0, nullptr, nullptr, WatchThread, nullptr, EX_CREATE_FLAG_TITLE_EXEC)))
            CloseHandle(thread);
        else
            DbgPrint("[codxe][T4 SP] thread watch: could not start the watch thread\n");
    }
    return Sys_ThreadMain_Detour.GetOriginal<Sys_ThreadMain_t>()(threadContext);
}

void SV_LinkEntity_Hook(gentity_s *gEnt)
{
    const LONG thread = static_cast<LONG>(GetCurrentThreadId());
    if (thread == g_server.id)
        Beat(g_server);
    else if (thread == g_main.id)
        Beat(g_main);
    else if (g_server.id && InterlockedCompareExchange(&g_mainFound, 1, 0) == 0)
    {
        StartWatching(g_main);
        RegisterThread(0, g_main.stackLow, g_main.stackHigh);
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
        if (IsReadable(reinterpret_cast<UINT32>(env)) && IsReadable(reinterpret_cast<UINT32>(env + 63)))
        {
            for (int i = 0; i < 64 && !to; ++i)
            {
                if (IsReturnAddress(env[i]))
                    to = env[i];
            }
        }
        DbgPrint("[codxe][T4 SP] longjmp %d on thread %08X: from %08X (in %08X) to %08X (in %08X), jump buffer "
                 "%08X\n",
                 count, GetCurrentThreadId(), from, FunctionStart(from), to, FunctionStart(to), env);
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

thread_watch::thread_watch()
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
        DbgPrint("[codxe][T4 SP] log_console: Server and main threads watched (Sys_ThreadMain at %08X, "
                 "SV_LinkEntity at %08X)\n",
                 threadMain, linkEntity);
    }
    else
    {
        DbgPrint("[codxe][T4 SP] log_console: %08X or %08X does not look like the start of Sys_ThreadMain or "
                 "SV_LinkEntity, the threads are not watched\n",
                 threadMain, linkEntity);
    }

    UINT32 visited[96];
    int visitedCount = 0;
    const UINT32 longjmpAddress = FindLongjmp(reinterpret_cast<UINT32>(Scr_Error), 4, visited, visitedCount);
    if (longjmpAddress && LooksLikeFunctionStart(longjmpAddress))
    {
        // Its code, to check that it is longjmp.
        const UINT32 *code = reinterpret_cast<const UINT32 *>(longjmpAddress);
        DbgPrint("[codxe][T4 SP] log_console: longjmps logged (longjmp at %08X: %08X %08X %08X %08X %08X %08X)\n",
                 longjmpAddress, code[0], code[1], code[2], code[3], code[4], code[5]);
        longjmp_Detour = Detour(reinterpret_cast<void *>(longjmpAddress), longjmp_Hook);
        longjmp_Detour.Install();
    }
    else
    {
        DbgPrint("[codxe][T4 SP] log_console: longjmp not found from Scr_Error (%08X, %d functions looked at)\n",
                 longjmpAddress, visitedCount);
    }
}

thread_watch::~thread_watch()
{
    longjmp_Detour.Remove();
    SV_LinkEntity_Detour.Remove();
    Sys_ThreadMain_Detour.Remove();
}

void thread_watch::OnProgress()
{
    if (static_cast<LONG>(GetCurrentThreadId()) == g_main.id)
        Beat(g_main);
}
} // namespace sp
} // namespace t4
