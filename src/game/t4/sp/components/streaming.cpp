#include "pch.h"
#include "streaming.h"
#include "fastfiles.h"

// Texture streaming (Title Update 7 addresses; the disc executable's in comments).
//
// When the streamer has read an image's top level into its slot (R_StreamLoadHighMip), or drops it,
// it queues (image, slot address or 0); the render thread applies the queue (R_StreamApplyQueue): it
// edits the fetch constant of the image's texture to use the slot as its base level and the
// fastfile's copy as its mips, doubling its size, or reverts that (R_StreamRevertHighMip).
//
// Deep images (t4ff's --deep-stream) keep a texture of a quarter of their size in the fastfile (an
// eighth with IMAGES_PAK_EIGHTH, for maps that need the memory); their entry in the map's images.pak
// is the whole texture, level 0 then from mipOffset on its mips (a texture of half its size, whose
// own mips start level2Offset on). They come in steps, as Black Ops'
// streamed images do: the half size texture (a quarter of the memory) once they are near, the base
// level once the view is inside their box; out of reach they go back a step, then to the fastfile's
// copy. Entries without level2Offset come whole, in one step.
//
// The streamer's slots are managed here too (Stream slots, below), over more memory than the game's
// 64 MB buffer: once a map of images.pak streams, the main memory it leaves becomes an extra pool
// (as Black Ops' extraRStreamBuffer), given back before the game's next PMem allocation or free.
//
// Locks, always taken in this order: slotLock (slots, deep images' blocks), the game's swap queue
// lock, deepImagesLock (deep images' fetch constants). The render thread never takes slotLock.

namespace t4
{
namespace sp
{
namespace
{
struct StreamSwapRequest
{
    unsigned char *image;
    unsigned int address;
};

const unsigned int STREAM_SWAP_QUEUE_SIZE = 64;
RTL_CRITICAL_SECTION *const streamSwapLock = reinterpret_cast<RTL_CRITICAL_SECTION *>(0x84F6B748); // disc 84E97948
volatile unsigned int *const streamSwapCount = reinterpret_cast<volatile unsigned int *>(0x84F6B764); // disc 84E97964
StreamSwapRequest *const streamSwapQueue = reinterpret_cast<StreamSwapRequest *>(0x84F6B768);       // disc 84E97968

typedef void (*R_StreamLoadHighMip_t)(unsigned char *image, double distance);
R_StreamLoadHighMip_t R_StreamLoadHighMip = reinterpret_cast<R_StreamLoadHighMip_t>(0x824457D0); // disc 82428C28

typedef void (*R_StreamApplyQueue_t)();
R_StreamApplyQueue_t R_StreamApplyQueue = reinterpret_cast<R_StreamApplyQueue_t>(0x824536A8); // disc 82436668

typedef void (*R_StreamRevertHighMip_t)(unsigned char *image);
R_StreamRevertHighMip_t R_StreamRevertHighMip = reinterpret_cast<R_StreamRevertHighMip_t>(0x824535F0); // disc 824365B0

typedef bool (*R_StreamQueueSwap_t)(unsigned char *image, unsigned int address);
R_StreamQueueSwap_t R_StreamQueueSwap = reinterpret_cast<R_StreamQueueSwap_t>(0x82453508); // disc 824364C8

Detour R_StreamLoadHighMip_Detour;
Detour R_StreamApplyQueue_Detour;
Detour R_StreamRevertHighMip_Detour;

// PMem, the game's physical memory: one "main" pool (480 MB in Title Update 7) whose two sides grow
// toward each other as stacks of named allocations; maps load on side 0, side 1 holds what the game
// allocates at boot (the 64 MB streaming buffer...) and a map's load zone. A free must be the side's
// newest allocation ("free does not match allocation" is fatal), found by its name's pointer.
const int PMEM_SIDE_HIGH = 1;
const int PMEM_PAGE_READWRITE = 4;
typedef void (*PMem_BeginAlloc_t)(const char *name, int side);
PMem_BeginAlloc_t PMem_BeginAlloc = reinterpret_cast<PMem_BeginAlloc_t>(0x822916A0); // disc 8227B658
typedef void (*PMem_EndAlloc_t)(const char *name, int side);
PMem_EndAlloc_t PMem_EndAlloc = reinterpret_cast<PMem_EndAlloc_t>(0x822916F8); // disc 8227B6B0
typedef void (*PMem_Free_t)(const char *name, int side);
PMem_Free_t PMem_Free = reinterpret_cast<PMem_Free_t>(0x82291718); // disc 8227B6D0
typedef unsigned char *(*PMem_Alloc_t)(unsigned int size, unsigned int alignment, int protect, int side);
PMem_Alloc_t PMem_Alloc = reinterpret_cast<PMem_Alloc_t>(0x82291840); // disc 8227B7F8
typedef unsigned int (*PMem_GetFree_t)();
PMem_GetFree_t PMem_GetFree = reinterpret_cast<PMem_GetFree_t>(0x82291BD8); // disc 8227BB90

// GfxImage: texture (D3DBaseTexture *) at 4, baseSize at 28 (bytes of the fastfile copy's base
// level: the streamer reads 4 times that), streamSlot at 32 (bit 15: not loaded), streaming at 34,
// name at 36. D3DBaseTexture: its fetch constant at 28.
const char *ImageName(const unsigned char *image)
{
    return *reinterpret_cast<const char *const *>(image + 36);
}

unsigned int *ImageBaseSize(unsigned char *image)
{
    return reinterpret_cast<unsigned int *>(image + 28);
}

unsigned short *ImageStreamSlot(unsigned char *image)
{
    return reinterpret_cast<unsigned short *>(image + 32);
}

unsigned short *ImageStreaming(unsigned char *image)
{
    return reinterpret_cast<unsigned short *>(image + 34);
}

unsigned int *TextureFetch(unsigned char *image)
{
    return reinterpret_cast<unsigned int *>(*reinterpret_cast<unsigned char **>(image + 4) + 28);
}

#ifndef STREAM_LOG_FULL
#define STREAM_LOG_FULL 0 // 1: what holds the memory when a load finds no room, and what gives way (tests)
#endif
#ifndef STREAM_LOG_SWAPS
#define STREAM_LOG_SWAPS 0 // 1: every deep image's swap in the log (tests)
#endif

// Deep images' fetch constants (deepImagesLock). A request on the queue names the level by its
// address: halfAddress the half size texture, fullAddress the base level (its mips at halfAddress);
// for an image that comes whole, its texture (its mips at mipOffset).
struct DeepImage
{
    std::string name;
    unsigned int mipOffset;
    unsigned int level2Offset;
    unsigned int extraSteps; // 1: the fastfile's copy is an eighth (IMAGES_PAK_EIGHTH)
    unsigned int halfAddress;
    unsigned int fullAddress;
    bool applied;
    unsigned int original[6]; // the fetch constant of the fastfile's copy, while applied
};

CRITICAL_SECTION deepImagesLock;
std::map<unsigned char *, DeepImage> deepImages;

// The deep image the streamer loaded at this address, if it is one (images move between maps).
DeepImage *FindDeepImage(unsigned char *image)
{
    std::map<unsigned char *, DeepImage>::iterator found = deepImages.find(image);
    if (found == deepImages.end() || found->second.name != ImageName(image))
        return nullptr;
    return &found->second;
}

// The pitch of a tiled texture `width` texels wide (fetch constant units: 32 texels), as t4ff lays
// them out: rows of whole 32 block tiles. Its format (bits 0-5 of word 1) says the block: 4x4 texels
// for DXT1-5, DXN, DXT3A, DXT5A and CTX1.
unsigned int TexturePitch(unsigned int width, unsigned int format)
{
    switch (format)
    {
    case 18: case 19: case 20: case 49: case 58: case 59: case 60: case 61:
        return ((width + 3) / 4 + 31) / 32 * 4;
    default:
        return (width + 31) / 32;
    }
}

// Fetch constant fields: pitch / 32 in bits 22-30 of word 0, the base address in word 1, width - 1
// and height - 1 in bits 0-12 and 13-25 of word 2, the largest mip level in bits 6-9 of word 4, the
// mip address in word 5. The fastfile's copy, `steps` levels larger: its base and mips elsewhere (the
// pitch from the width: an eighth's rows are padded, its pitch not an eighth of the whole texture's).
void ApplyDeepImage(unsigned char *image, DeepImage &deep, unsigned int base, unsigned int mips, unsigned int steps)
{
    unsigned int *fetch = TextureFetch(image);
    if (!deep.applied)
    {
        memcpy(deep.original, fetch, sizeof(deep.original));
        deep.applied = true;
    }

    const unsigned int *original = deep.original;
    const unsigned int width = (original[2] & 0x1FFF) + 1;
    const unsigned int height = ((original[2] >> 13) & 0x1FFF) + 1;
    const unsigned int maxMip = (original[4] >> 6) & 0xF;
    fetch[0] = (original[0] & ~0x7FC00000u) | ((TexturePitch(width << steps, original[1] & 0x3F) & 0x1FF) << 22);
    fetch[1] = (original[1] & 0xFFF) | (base & 0xFFFFF000);
    fetch[2] = (original[2] & ~0x3FFFFFFu) | (((width << steps) - 1) & 0x1FFF) | ((((height << steps) - 1) & 0x1FFF) << 13);
    fetch[4] = (original[4] & ~0x3C0u) | (((maxMip + steps) & 0xF) << 6);
    fetch[5] = (original[5] & 0xFFF) | (mips & 0xFFFFF000);

#if STREAM_LOG_SWAPS
    // (DbgPrint takes few arguments: more come out wrong)
    DbgPrint("[codxe][T4 SP][Streaming] deep %s %s at %08X (%ux%u)\n", steps == 2 + deep.extraSteps ? "full" : "half",
             deep.name.c_str(),
             base, width << steps, height << steps);
#endif
}

void RevertDeepImage(unsigned char *image, DeepImage &deep)
{
    if (!deep.applied)
        return;

    memcpy(TextureFetch(image), deep.original, sizeof(deep.original));
    deep.applied = false;
#if STREAM_LOG_SWAPS
    DbgPrint("[codxe][T4 SP][Streaming] deep drop %s\n", deep.name.c_str());
#endif
}

// The levels are counted from the fastfile's copy: the whole texture two above a quarter, three above an
// eighth.
void ApplyDeepRequest(unsigned char *image, DeepImage &deep, unsigned int address)
{
    const unsigned int full = 2 + deep.extraSteps;
    if (!address)
        RevertDeepImage(image, deep);
    else if (!deep.level2Offset)
        ApplyDeepImage(image, deep, address, address + deep.mipOffset, full); // whole
    else if (deep.fullAddress && address == deep.fullAddress)
        ApplyDeepImage(image, deep, address, deep.halfAddress, full);
    else if (address == deep.halfAddress)
        ApplyDeepImage(image, deep, address, address + deep.level2Offset, full - 1);
    // else stale: a block that has gone
}

// The queue's deep images are applied (or dropped) here, the others by the game.
void R_StreamApplyQueue_Hook()
{
    RtlEnterCriticalSection(streamSwapLock);
    EnterCriticalSection(&deepImagesLock);
    unsigned int count = *streamSwapCount;
    if (count > STREAM_SWAP_QUEUE_SIZE)
        count = STREAM_SWAP_QUEUE_SIZE;

    unsigned int kept = 0;
    for (unsigned int i = 0; i < count; ++i)
    {
        const StreamSwapRequest request = streamSwapQueue[i];
        DeepImage *deep = FindDeepImage(request.image);
        if (deep)
            ApplyDeepRequest(request.image, *deep, request.address);
        else
            streamSwapQueue[kept++] = request;
    }
    *streamSwapCount = kept;
    LeaveCriticalSection(&deepImagesLock);
    RtlLeaveCriticalSection(streamSwapLock);

    R_StreamApplyQueue_Detour.GetOriginal<R_StreamApplyQueue_t>()();
}

// The streamer also drops images itself when it takes their slot.
void R_StreamRevertHighMip_Hook(unsigned char *image)
{
    EnterCriticalSection(&deepImagesLock);
    DeepImage *deep = FindDeepImage(image);
    if (deep)
        RevertDeepImage(image, *deep);
    LeaveCriticalSection(&deepImagesLock);

    if (!deep)
        R_StreamRevertHighMip_Detour.GetOriginal<R_StreamRevertHighMip_t>()(image);
}

// The levels the next requests for a deep image name (taken briefly: never with the queue's lock).
void SetDeepTargets(unsigned char *image, const ImagesPakImage &entry, unsigned int halfAddress, unsigned int fullAddress)
{
    EnterCriticalSection(&deepImagesLock);
    DeepImage *deep = FindDeepImage(image);
    if (!deep)
    {
        DeepImage added;
        added.name = ImageName(image);
        added.applied = false;
        deep = &(deepImages[image] = added);
    }
    deep->mipOffset = entry.mipOffset;
    deep->level2Offset = entry.level2Offset;
    deep->extraSteps = (entry.flags & IMAGES_PAK_EIGHTH) ? 1 : 0;
    deep->halfAddress = halfAddress;
    deep->fullAddress = fullAddress;
    LeaveCriticalSection(&deepImagesLock);
}

// Stream slots: the streamer's memory and its bookkeeping, replacing the game's.
//
// The game hands out its 64 MB streaming buffer in slots of 128 KiB (a slot's address: the buffer
// + slot << 17) through a buddy allocator of 16 regions of 4 MB, and keeps per slot the frames it
// was last asked for (R_StreamRecordUse, by distance band) and last changed. Each block has the
// image it holds and a priority: 5 applied and needed, 4 dropped but kept, 1-3 loaded ahead (by
// distance), 0 free. Allocating takes the cheapest place: free first, else the lowest priority the
// request may evict (below its own, never 4 or 5: R_StreamAlloc), the least recently changed. Each
// frame a quarter of the blocks are rated again (R_StreamUpdateSlots, the state machine below: it
// queues applies and drops). Here the same rules hold over ranges of memory of any number, each its
// own slot numbers: an image's slot (GfxImage.streamSlot, 15 bits) says which range holds it. A deep
// image coming in steps has two blocks: its half size texture (its slot) and its base level.
const unsigned int SLOT_SIZE = 128 * 1024;
const unsigned int SLOTS_PER_REGION = 32; // a 4 MB block, the largest
const unsigned int MAX_SLOTS = 4096;      // 512 MB
const unsigned int MAX_RANGES = 4;
const unsigned short NO_BLOCK = 0xFFFF;
const unsigned short IMAGE_EVICTED = 0xFFFE; // GfxImage.streamSlot: not loaded (bit 15)
const int PRIORITY_APPLIED = 5;
const int PRIORITY_DROPPED = 4;
const unsigned char BLOCK_IMAGE = 0;      // an image's texture (its slot)
const unsigned char BLOCK_BASE_LEVEL = 1; // a deep image's base level

struct StreamRange
{
    unsigned int address;
    unsigned int firstSlot;
    unsigned int slotCount;
};

struct StreamBlock // at its first slot
{
    unsigned short units; // slots, a power of 2; 0: no block starts here
    unsigned char priority;
    unsigned char kind;
    unsigned char *image; // nullptr while it is read
    unsigned int changed; // allocation order, for the least recently changed
};

struct SlotUse // frames, as the game's (stream frame counter)
{
    unsigned int changed;
    unsigned int inside;  // asked from inside its box
    unsigned int middle;  // within 100 units
    unsigned int distant; // within 300 units
};

// A deep image that comes in steps (slotLock): its blocks and the level last queued (0 the
// fastfile's copy, 1 the half size texture, 2 the whole).
struct DeepSteps
{
    std::string name;
    ImagesPakImage entry;
    unsigned short halfSlot;
    unsigned short fullSlot;
    int level;
    unsigned int demotedAt; // the frame its base level gave way to others' half sizes (0: never)
};

// When the memory is full of base levels the view is inside the boxes of (applied: never evicted),
// a load finding no room would be proposed again every frame, the closest, and no other image would
// load (a map whose large textures have boxes thousands of units wide: its wall buys' chalk, 128 KiB,
// never came in). So:
// - a load that finds no room waits FAILED_FRAMES, the others going first;
// - images near showing their fastfile copy go before base levels (Propose's keys),
//   and while memory is short an image's first load does not bring its base level along;
// - a half size texture within 100 units of the view that finds no room has the base levels of one
//   place of its size (the one with the fewest) go back to their half size for DEMOTED_FRAMES (dropped as any,
//   then evictable): every image near gets its half size before others keep their base level.
const unsigned int FAILED_FRAMES = 30;
const unsigned int DEMOTED_FRAMES = 900; // ~15-30 seconds: fewer base levels read again
const unsigned int SHORT_FRAMES = 60; // memory is short this long after a load found no room
std::map<unsigned char *, unsigned int> failedLoads; // image -> the frame its load found no room (slotLock)
unsigned int lastNoRoom = 0;                          // the frame a load last found no room (0: never)

bool MemoryShort(unsigned int frame)
{
    return lastNoRoom && frame - lastNoRoom < SHORT_FRAMES;
}

bool LoadFailedRecently(unsigned char *image, unsigned int frame)
{
    std::map<unsigned char *, unsigned int>::iterator found = failedLoads.find(image);
    return found != failedLoads.end() && frame - found->second < FAILED_FRAMES;
}

bool Demoted(const DeepSteps &steps, unsigned int frame)
{
    return steps.demotedAt && frame - steps.demotedAt < DEMOTED_FRAMES;
}

CRITICAL_SECTION slotLock;
StreamRange ranges[MAX_RANGES];
unsigned int rangeCount = 0;
unsigned short slotBlock[MAX_SLOTS]; // the first slot of the block covering each, or NO_BLOCK
StreamBlock blocks[MAX_SLOTS];
SlotUse slotUse[MAX_SLOTS];
unsigned int blockOrder = 0;
bool allocBaseRangeOnly = false; // the game's own loader computes addresses from the buffer alone
std::map<unsigned char *, DeepSteps> deepSteps;

volatile unsigned int *const streamFrame = reinterpret_cast<volatile unsigned int *>(0x84F3B9A8);        // disc 84E67BA8
unsigned int *const streamBufferAddress = reinterpret_cast<unsigned int *>(0x84F3B9AC);                // disc 84E67BAC
unsigned char **const streamCandidate = reinterpret_cast<unsigned char **>(0x84F3B9D0);                // disc 84E67BD0
float *const streamCandidateDistance = reinterpret_cast<float *>(0x84F3B9D4);                          // disc 84E67BD4
volatile unsigned int *const streamLoadingImage = reinterpret_cast<volatile unsigned int *>(0x84F3B9E0); // disc 84E67BE0
const unsigned int STREAM_BUFFER_SIZE = 64 * 1024 * 1024;

DeepSteps *FindDeepSteps(unsigned char *image)
{
    std::map<unsigned char *, DeepSteps>::iterator found = deepSteps.find(image);
    if (found == deepSteps.end() || found->second.name != ImageName(image))
        return nullptr;
    return &found->second;
}

unsigned int SlotAddress(unsigned int slot)
{
    for (unsigned int i = 0; i < rangeCount; ++i)
    {
        if (slot >= ranges[i].firstSlot && slot < ranges[i].firstSlot + ranges[i].slotCount)
            return ranges[i].address + (slot - ranges[i].firstSlot) * SLOT_SIZE;
    }
    return 0;
}

void RemoveBlock(unsigned int first)
{
    const unsigned int units = blocks[first].units;
    for (unsigned int s = first; s < first + units; ++s)
        slotBlock[s] = NO_BLOCK;
    blocks[first].units = 0;
    blocks[first].image = nullptr;
}

// An image's requests still on the queue (it is evicted: they would name a block that has gone).
void PurgeQueuedSwaps(unsigned char *image)
{
    RtlEnterCriticalSection(streamSwapLock);
    const unsigned int count = min(static_cast<unsigned int>(*streamSwapCount), STREAM_SWAP_QUEUE_SIZE);
    unsigned int kept = 0;
    for (unsigned int i = 0; i < count; ++i)
    {
        if (streamSwapQueue[i].image != image)
            streamSwapQueue[kept++] = streamSwapQueue[i];
    }
    *streamSwapCount = kept;
    RtlLeaveCriticalSection(streamSwapLock);
}

// A deep image back to the fastfile's copy (applied: reverted), its blocks gone.
void UnloadDeepSteps(unsigned char *image, DeepSteps &steps)
{
    PurgeQueuedSwaps(image);
    if (steps.level)
        R_StreamRevertHighMip(image);
    steps.level = 0;
    if (steps.fullSlot != NO_BLOCK && blocks[steps.fullSlot].kind == BLOCK_BASE_LEVEL && blocks[steps.fullSlot].image == image)
        RemoveBlock(steps.fullSlot);
    steps.fullSlot = NO_BLOCK;
    steps.halfSlot = NO_BLOCK;
}

// The streamer took the slot (never applied: priority under 4). A deep image's base level goes
// alone (the image stays at its half size); its half size texture takes the base level along.
void EvictBlock(unsigned int first)
{
    unsigned char *image = blocks[first].image;
    const unsigned char kind = blocks[first].kind;
    RemoveBlock(first);
    if (!image)
        return;

    DeepSteps *steps = FindDeepSteps(image);
    if (kind == BLOCK_BASE_LEVEL)
    {
        if (steps && steps->fullSlot == first)
            steps->fullSlot = NO_BLOCK;
        return;
    }
    if (steps && steps->halfSlot == first)
        UnloadDeepSteps(image, *steps);
    *ImageStreamSlot(image) = IMAGE_EVICTED;
}

void ResetSlots()
{
    for (unsigned int s = 0; s < MAX_SLOTS; ++s)
    {
        slotBlock[s] = NO_BLOCK;
        blocks[s].units = 0;
        blocks[s].image = nullptr;
    }
    memset(slotUse, 0, sizeof(slotUse));
    deepSteps.clear();
    failedLoads.clear();
    lastNoRoom = 0;
}

typedef void (*R_StreamInitSlots_t)();
R_StreamInitSlots_t R_StreamInitSlots = reinterpret_cast<R_StreamInitSlots_t>(0x824655B0); // disc 82446328
Detour R_StreamInitSlots_Detour;

// After the game allocates its buffer (render init): the first range.
void R_StreamInitSlots_Hook()
{
    R_StreamInitSlots_Detour.GetOriginal<R_StreamInitSlots_t>()();
    EnterCriticalSection(&slotLock);
    ResetSlots();
    ranges[0].address = *streamBufferAddress;
    ranges[0].firstSlot = 0;
    ranges[0].slotCount = STREAM_BUFFER_SIZE / SLOT_SIZE;
    rangeCount = 1;
    LeaveCriticalSection(&slotLock);
    DbgPrint("[codxe][T4 SP][Streaming] slots: the game's buffer at %08X, %u slots\n", ranges[0].address,
             ranges[0].slotCount);
}

typedef bool (*R_StreamAlloc_t)(unsigned int size, int priority, unsigned int *block, unsigned int *slot);
R_StreamAlloc_t R_StreamAlloc = reinterpret_cast<R_StreamAlloc_t>(0x824656A8); // disc 82446420
Detour R_StreamAlloc_Detour;

bool R_StreamAlloc_Hook(unsigned int size, int priority, unsigned int *block, unsigned int *slot)
{
    unsigned int units = 1;
    while (units * SLOT_SIZE < size)
        units <<= 1;
    if (units > SLOTS_PER_REGION)
        return false;
    const int evictable = priority == PRIORITY_APPLIED ? PRIORITY_DROPPED : priority; // below this

    EnterCriticalSection(&slotLock);
    int bestCost = 0x7FFFFFFF;
    unsigned int bestChanged = 0;
    unsigned int best = MAX_SLOTS;
    const unsigned int usable = allocBaseRangeOnly ? 1 : rangeCount;
    for (unsigned int r = 0; r < usable; ++r) // the game's buffer first: the extra pool when it is full
    {
        const StreamRange &range = ranges[r];
        for (unsigned int start = range.firstSlot; start + units <= range.firstSlot + range.slotCount; start += units)
        {
            int cost = 0;
            unsigned int changed = 0;
            bool possible = true;
            for (unsigned int s = start; s < start + units && possible; ++s)
            {
                if (slotBlock[s] == NO_BLOCK)
                    continue;
                const StreamBlock &held = blocks[slotBlock[s]];
                if (!held.image || held.priority >= evictable)
                    possible = false;
                else
                {
                    cost = max(cost, static_cast<int>(held.priority));
                    changed = max(changed, held.changed);
                }
            }
            if (possible && (cost < bestCost || (cost == bestCost && changed < bestChanged)))
            {
                bestCost = cost;
                bestChanged = changed;
                best = start;
                if (!cost)
                    break; // free: the first will do
            }
        }
        if (best != MAX_SLOTS && !bestCost)
            break;
    }

    if (best == MAX_SLOTS)
    {
#if STREAM_LOG_FULL
        // what holds the memory when a load finds no room (every 2 seconds at most)
        static DWORD lastLog = 0;
        if (GetTickCount() - lastLog > 2000)
        {
            lastLog = GetTickCount();
            unsigned int held[6] = {};
            unsigned int freeSlots = 0;
            for (unsigned int r = 0; r < usable; ++r)
                for (unsigned int s = ranges[r].firstSlot; s < ranges[r].firstSlot + ranges[r].slotCount; ++s)
                {
                    if (slotBlock[s] == NO_BLOCK)
                        ++freeSlots;
                    else
                        ++held[min(static_cast<unsigned int>(blocks[slotBlock[s]].priority), 5u)];
                }
            DbgPrint("[codxe][T4 SP][Streaming] full: %u KiB at priority %d; slots free %u, by priority 0-5: %u %u %u %u %u %u\n",
                     size / 1024, priority, freeSlots, held[0], held[1], held[2], held[3], held[4], held[5]);
        }
#endif
        LeaveCriticalSection(&slotLock);
        return false;
    }

    for (unsigned int s = best; s < best + units; ++s)
    {
        if (slotBlock[s] != NO_BLOCK)
            EvictBlock(slotBlock[s]);
    }
    for (unsigned int s = best; s < best + units; ++s)
        slotBlock[s] = static_cast<unsigned short>(best);
    blocks[best].units = static_cast<unsigned short>(units);
    blocks[best].priority = static_cast<unsigned char>(priority);
    blocks[best].kind = BLOCK_IMAGE;
    blocks[best].image = nullptr;
    blocks[best].changed = ++blockOrder;
    *block = best;
    *slot = best;
    LeaveCriticalSection(&slotLock);
    return true;
}

typedef void (*R_StreamFreeSlot_t)(unsigned int slot);
R_StreamFreeSlot_t R_StreamFreeSlot = reinterpret_cast<R_StreamFreeSlot_t>(0x824658B0); // disc 82446628
Detour R_StreamFreeSlot_Detour;

void R_StreamFreeSlot_Hook(unsigned int slot)
{
    EnterCriticalSection(&slotLock);
    if (slot < MAX_SLOTS && slotBlock[slot] == slot)
        RemoveBlock(slot);
    LeaveCriticalSection(&slotLock);
}

typedef void (*R_StreamAssignSlot_t)(unsigned int block, unsigned int slot, unsigned char *image);
R_StreamAssignSlot_t R_StreamAssignSlot = reinterpret_cast<R_StreamAssignSlot_t>(0x82465988); // disc 82446700
Detour R_StreamAssignSlot_Detour;

void R_StreamAssignSlot_Hook(unsigned int block, unsigned int slot, unsigned char *image)
{
    EnterCriticalSection(&slotLock);
    if (block < MAX_SLOTS && blocks[block].units)
    {
        blocks[block].image = image;
        *ImageStreamSlot(image) = static_cast<unsigned short>(slot);
    }
    LeaveCriticalSection(&slotLock);
}

void AllocateExtraPool();

typedef void (*R_StreamLockSlots_t)();
R_StreamLockSlots_t R_StreamLockSlots = reinterpret_cast<R_StreamLockSlots_t>(0x82465B08);   // disc 82446880
R_StreamLockSlots_t R_StreamUnlockSlots = reinterpret_cast<R_StreamLockSlots_t>(0x82465B38); // disc 824468B0
Detour R_StreamLockSlots_Detour;
Detour R_StreamUnlockSlots_Detour;

void R_StreamLockSlots_Hook()
{
    AllocateExtraPool(); // the game's frame (R_StreamUpdate): once a map of images.pak streams
    EnterCriticalSection(&slotLock);
}

void R_StreamUnlockSlots_Hook()
{
    LeaveCriticalSection(&slotLock);
}

typedef void (*R_StreamRecordUse_t)(unsigned char *image, unsigned int slot, unsigned int frame, double distance);
R_StreamRecordUse_t R_StreamRecordUse = reinterpret_cast<R_StreamRecordUse_t>(0x82445568); // disc 82428BB0
Detour R_StreamRecordUse_Detour;

// Distances are squared: inside the box, within 100 units, within 300.
void R_StreamRecordUse_Hook(unsigned char *image, unsigned int slot, unsigned int frame, double distance)
{
    if (slot >= MAX_SLOTS)
        return;
    if (distance <= 0.0)
        slotUse[slot].inside = frame;
    else if (distance <= 10000.0)
        slotUse[slot].middle = frame;
    else if (distance <= 90000.0)
        slotUse[slot].distant = frame;
}

typedef void (*R_StreamConsiderMaterial_t)(unsigned char *material, double distance, unsigned int frame,
                                           unsigned char *loading);
R_StreamConsiderMaterial_t R_StreamConsiderMaterial = reinterpret_cast<R_StreamConsiderMaterial_t>(0x82445C00); // disc 82429018
Detour R_StreamConsiderMaterial_Detour;

#ifndef STREAM_TEST_WHOLE
#define STREAM_TEST_WHOLE 0 // a test build: deep images come whole, as before steps
#endif
#ifndef STREAM_TEST_HALF_ONLY
#define STREAM_TEST_HALF_ONLY 0 // a test build: deep images stop at their half size
#endif

// The streamer loads the candidate of the lowest key; the loader is given the candidate's distance.
// Keys: an image within 300 units still showing its fastfile copy -2 to -1 (inside its box -2, then
// by distance), a base level 0 (inside its box), others their distance. So what shows its fastfile
// copy near the view gets its half size before any base level comes in: a wall buy's chalk 61 units
// away, its box ending 58 units from it, waited behind the base levels of every large texture the
// view was inside the box of, and showed its eighth.
unsigned char *candidateImage = nullptr;
double candidateDistance = 0.0;

void Propose(unsigned char *image, double key, double distance)
{
    if (!*streamCandidate || key < *streamCandidateDistance)
    {
        *streamCandidate = image;
        *streamCandidateDistance = static_cast<float>(key);
        candidateImage = image;
        candidateDistance = distance;
    }
}

double FirstLoadKey(double distance)
{
    if (distance > 90000.0)
        return distance;
    return -2.0 + (distance > 0.0 ? distance : 0.0) / 90000.0;
}

// A material drawn at this distance (the game's, under the slots' lock): its streamed images' use,
// the closest of those not loaded proposed for loading, and the deep images whose base level is
// wanted (inside their box) and missing.
void R_StreamConsiderMaterial_Hook(unsigned char *material, double distance, unsigned int frame, unsigned char *loading)
{
    const unsigned int count = material[75];
    unsigned char *table = *reinterpret_cast<unsigned char **>(material + 84);
    for (unsigned int i = 0; i < count; ++i)
    {
        unsigned char *def = table + i * 16;
        if (def[7] == 11) // a water map
            continue;
        unsigned char *image = *reinterpret_cast<unsigned char **>(def + 12);
        if (!*ImageStreaming(image))
            continue;
        const unsigned short slot = *ImageStreamSlot(image);
        if (slot & 0x8000)
        {
            if (image != loading && !LoadFailedRecently(image, frame))
                Propose(image, FirstLoadKey(distance), distance);
            continue;
        }
        R_StreamRecordUse_Hook(image, slot, frame, distance);
#if !STREAM_TEST_HALF_ONLY
        if (distance <= 0.0 && image != loading && !LoadFailedRecently(image, frame))
        {
            DeepSteps *steps = FindDeepSteps(image);
            if (steps && steps->fullSlot == NO_BLOCK && !Demoted(*steps, frame))
                Propose(image, 0.0, distance);
        }
#endif
    }
}

int WantedPriority(const SlotUse &use, unsigned int frame)
{
    if (frame - use.inside < 10)
        return PRIORITY_APPLIED;
    if (frame - use.middle < 10)
        return 3;
    if (frame - use.distant < 10)
        return 2;
    return 1;
}

// The game's state machine (disc 82428ED8): the priority a block has now, queueing what changes.
int RateBlock(unsigned char *image, int priority, unsigned int slot)
{
    SlotUse &use = slotUse[slot];
    const unsigned int frame = *streamFrame;
    int wanted = WantedPriority(use, frame);

    if (priority == PRIORITY_APPLIED)
    {
        if (wanted != PRIORITY_APPLIED)
        {
            if (!R_StreamQueueSwap(image, 0))
                return PRIORITY_APPLIED;
            wanted = PRIORITY_DROPPED;
        }
    }
    else if (priority == PRIORITY_DROPPED)
    {
        if (wanted == PRIORITY_APPLIED)
        {
            if (!R_StreamQueueSwap(image, SlotAddress(slot)))
                return PRIORITY_DROPPED;
        }
        else if (frame - use.changed < 5)
            return PRIORITY_DROPPED;
    }
    else if (wanted == PRIORITY_APPLIED && !R_StreamQueueSwap(image, SlotAddress(slot)))
        return priority;

    use.changed = frame;
    return wanted;
}

// A deep image coming in steps: inside its box its base level (once loaded, else its half size),
// near its half size, out of reach the fastfile's copy. Its blocks' priorities follow: applied 5,
// just dropped 4 (5 frames), else 1 (the first to go).
void RateDeepSteps(unsigned char *image, DeepSteps &steps)
{
    SlotUse &use = slotUse[steps.halfSlot];
    const unsigned int frame = *streamFrame;
    const int wanted = WantedPriority(use, frame);
    int level = wanted == PRIORITY_APPLIED ? 2 : wanted >= 2 ? 1 : 0;
    if (level == 2 && (steps.fullSlot == NO_BLOCK || Demoted(steps, frame)))
        level = 1; // until the base level is read (R_StreamConsiderMaterial proposes it), or given way

    if (level != steps.level)
    {
        const unsigned int half = SlotAddress(steps.halfSlot);
        const unsigned int full = steps.fullSlot != NO_BLOCK ? SlotAddress(steps.fullSlot) : 0;
        SetDeepTargets(image, steps.entry, half, full);
        if (R_StreamQueueSwap(image, level == 2 ? full : level == 1 ? half : 0))
        {
            steps.level = level;
            use.changed = frame;
        }
    }

    const bool justChanged = frame - use.changed < 5;
    StreamBlock &halfBlock = blocks[steps.halfSlot];
    halfBlock.priority = static_cast<unsigned char>(steps.level ? PRIORITY_APPLIED : justChanged ? PRIORITY_DROPPED : 1);
    if (steps.fullSlot != NO_BLOCK)
        blocks[steps.fullSlot].priority =
            static_cast<unsigned char>(steps.level == 2 ? PRIORITY_APPLIED : justChanged ? PRIORITY_DROPPED : 1);
}

typedef void (*R_StreamUpdateSlots_t)(int frame);
R_StreamUpdateSlots_t R_StreamUpdateSlots = reinterpret_cast<R_StreamUpdateSlots_t>(0x82465BE0); // disc 82446958
Detour R_StreamUpdateSlots_Detour;

// A quarter of the blocks each frame, as the game's (every fourth region).
void R_StreamUpdateSlots_Hook(int frame)
{
    EnterCriticalSection(&slotLock);
    const unsigned int quarter = static_cast<unsigned int>(frame) % 4;
    for (unsigned int first = 0; first < MAX_SLOTS; ++first)
    {
        StreamBlock &block = blocks[first];
        if (!block.units || !block.image || block.kind != BLOCK_IMAGE || (first / SLOTS_PER_REGION) % 4 != quarter)
            continue;
        if (*ImageStreamSlot(block.image) & 0x8000)
        {
            unsigned char *image = block.image;
            DeepSteps *steps = FindDeepSteps(image);
            RemoveBlock(first); // the image went (its zone unloaded, or it stopped streaming)
            if (steps && steps->halfSlot == first)
                UnloadDeepSteps(image, *steps);
            continue;
        }
        DeepSteps *steps = FindDeepSteps(block.image);
        if (steps && steps->halfSlot == first)
        {
            RateDeepSteps(block.image, *steps);
            continue;
        }
        const int rated = RateBlock(block.image, block.priority, first);
        if (rated != block.priority)
        {
            block.priority = static_cast<unsigned char>(rated);
            block.changed = ++blockOrder;
        }
    }
    LeaveCriticalSection(&slotLock);
}

typedef void (*R_StreamEvictAll_t)();
R_StreamEvictAll_t R_StreamEvictAll = reinterpret_cast<R_StreamEvictAll_t>(0x82465C78); // disc 824469F0
Detour R_StreamEvictAll_Detour;

// r_streamClear, r_stream changes, a cinematic borrowing the buffer: everything goes, the applied
// textures reverted first.
void R_StreamEvictAll_Hook()
{
    EnterCriticalSection(&slotLock);
    unsigned int evicted = 0;
    for (unsigned int first = 0; first < MAX_SLOTS; ++first)
    {
        StreamBlock &block = blocks[first];
        if (!block.units)
            continue;
        if (block.kind == BLOCK_IMAGE && block.image && !(*ImageStreamSlot(block.image) & 0x8000))
        {
            PurgeQueuedSwaps(block.image);
            DeepSteps *steps = FindDeepSteps(block.image);
            if (steps && steps->halfSlot == first)
            {
                if (steps->level)
                    R_StreamRevertHighMip(block.image);
                steps->level = 0;
                steps->halfSlot = steps->fullSlot = NO_BLOCK;
            }
            else if (block.priority == PRIORITY_APPLIED)
                R_StreamRevertHighMip(block.image);
            *ImageStreamSlot(block.image) = IMAGE_EVICTED;
            ++evicted;
        }
        RemoveBlock(first);
    }
    LeaveCriticalSection(&slotLock);
    DbgPrint("[codxe][T4 SP][Streaming] slots: evicted %u images\n", evicted);
}

// The extra pool (Black Ops' extraRStreamBuffer): once a map of images.pak streams, the main memory
// it leaves (less a margin) becomes a second range of slots; it is given back before the game
// allocates or frees anything else in PMem (a map unloading, the next loading), its images evicted
// first (applied ones reverted). PMem frees must come newest first.
const char *const EXTRA_POOL_NAME = "codxe_stream_pool"; // PMem matches this pointer
// left to the game: it allocates nothing from PMem while a map plays (logged over a tour of Kino
// Rezurrection: every allocation came while the map loaded) and the pool goes back before anything
// else in PMem, so a console's map, which leaves about 10 MB, still gets 8 MB of it
const unsigned int EXTRA_POOL_MARGIN = 1 * 1024 * 1024; // (2 MB: Kino Rezurrection's 10 MB gave 4 MB)
const unsigned int EXTRA_POOL_UNIT = SLOTS_PER_REGION * SLOT_SIZE;
unsigned char *extraPool = nullptr;
unsigned int extraPoolSize = 0;
volatile bool extraPoolWanted = false; // a map of images.pak streams
bool extraPoolTried = false;           // once per map: what is left may not change
RTL_CRITICAL_SECTION *const streamLock = reinterpret_cast<RTL_CRITICAL_SECTION *>(0x84F3B9B4); // disc 84E67BB4

Detour PMem_BeginAlloc_Detour;
Detour PMem_Free_Detour;

void AllocateExtraPool()
{
    if (extraPool || !extraPoolWanted || extraPoolTried || !rangeCount)
        return;
    extraPoolTried = true;

    const unsigned int freeBytes = PMem_GetFree();
    if (freeBytes < EXTRA_POOL_MARGIN + EXTRA_POOL_UNIT)
    {
        DbgPrint("[codxe][T4 SP][Streaming] extra pool: %u KiB free, none\n", freeBytes / 1024);
        return;
    }
    unsigned int size = (freeBytes - EXTRA_POOL_MARGIN) / EXTRA_POOL_UNIT * EXTRA_POOL_UNIT;
    const unsigned int firstSlot = ranges[0].firstSlot + ranges[0].slotCount;
    size = min(size, (MAX_SLOTS - firstSlot) * SLOT_SIZE);

    PMem_BeginAlloc_Detour.GetOriginal<PMem_BeginAlloc_t>()(EXTRA_POOL_NAME, PMEM_SIDE_HIGH);
    unsigned char *pool = PMem_Alloc(size, 0x10000, PMEM_PAGE_READWRITE, PMEM_SIDE_HIGH);
    PMem_EndAlloc(EXTRA_POOL_NAME, PMEM_SIDE_HIGH);
    if (!pool)
        return;

    EnterCriticalSection(&slotLock);
    ranges[rangeCount].address = reinterpret_cast<unsigned int>(pool);
    ranges[rangeCount].firstSlot = firstSlot;
    ranges[rangeCount].slotCount = size / SLOT_SIZE;
    ++rangeCount;
    extraPool = pool;
    extraPoolSize = size;
    LeaveCriticalSection(&slotLock);
    DbgPrint("[codxe][T4 SP][Streaming] extra pool: %u KiB at %08X (slots %u-%u), %u KiB left\n", size / 1024,
             reinterpret_cast<unsigned int>(pool), firstSlot, firstSlot + size / SLOT_SIZE - 1, PMem_GetFree() / 1024);
}

void FreeExtraPool(const char *why)
{
    if (!extraPool)
        return;

    // no read may be going into it: the streamer loads with its lock left, marking the image
    RtlEnterCriticalSection(streamLock);
    while (*streamLoadingImage)
    {
        RtlLeaveCriticalSection(streamLock);
        Sleep(1);
        RtlEnterCriticalSection(streamLock);
    }

    EnterCriticalSection(&slotLock);
    unsigned int evicted = 0;
    const StreamRange extra = ranges[rangeCount - 1];
    for (unsigned int first = extra.firstSlot; first < extra.firstSlot + extra.slotCount; ++first)
    {
        StreamBlock &block = blocks[first];
        if (!block.units)
            continue;
        unsigned char *image = block.image;
        if (!image || (*ImageStreamSlot(image) & 0x8000))
        {
            RemoveBlock(first);
            continue;
        }
        PurgeQueuedSwaps(image);
        DeepSteps *steps = FindDeepSteps(image);
        if (block.kind == BLOCK_BASE_LEVEL)
        {
            // the image goes back to the fastfile's copy, its half size texture kept
            if (steps && steps->level)
            {
                R_StreamRevertHighMip(image);
                steps->level = 0;
            }
            if (steps)
                steps->fullSlot = NO_BLOCK;
            RemoveBlock(first);
            continue;
        }
        if (steps && steps->halfSlot == first)
            UnloadDeepSteps(image, *steps);
        else if (block.priority == PRIORITY_APPLIED)
            R_StreamRevertHighMip(image);
        *ImageStreamSlot(image) = IMAGE_EVICTED;
        ++evicted;
        RemoveBlock(first);
    }
    --rangeCount;
    extraPool = nullptr;
    LeaveCriticalSection(&slotLock);
    RtlLeaveCriticalSection(streamLock);

    PMem_Free_Detour.GetOriginal<PMem_Free_t>()(EXTRA_POOL_NAME, PMEM_SIDE_HIGH);
    DbgPrint("[codxe][T4 SP][Streaming] extra pool: given back before %s (%u images evicted), %u KiB free\n", why,
             evicted, PMem_GetFree() / 1024);
}

void PMem_BeginAlloc_Hook(const char *name, int side)
{
    if (name != EXTRA_POOL_NAME)
    {
        FreeExtraPool(name ? name : "an allocation");
        extraPoolWanted = false;
        extraPoolTried = false;
    }
    PMem_BeginAlloc_Detour.GetOriginal<PMem_BeginAlloc_t>()(name, side);
}

void PMem_Free_Hook(const char *name, int side)
{
    if (name != EXTRA_POOL_NAME)
    {
        FreeExtraPool(name ? name : "a free");
        extraPoolWanted = false;
        extraPoolTried = false;
    }
    PMem_Free_Detour.GetOriginal<PMem_Free_t>()(name, side);
}

typedef int (*Sys_OpenFile_t)(const char *filename, int desiredAccess, int shareMode, int securityAttributes,
                              int creationDisposition, int flagsAndAttributes);
Sys_OpenFile_t Sys_OpenFile = reinterpret_cast<Sys_OpenFile_t>(0x823972F0); // the CRT's CreateFileA

// Part of an image's entry into a slot (unbuffered, as the game's reads: 4 KiB aligned).
bool ReadPakPart(const ImagesPakImage &entry, unsigned int offset, unsigned int size, unsigned int address)
{
    const std::string pak = GetImagesPakPath();
    if (pak.empty())
        return false;
    const int handle = Sys_OpenFile(pak.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
                                    FILE_FLAG_NO_BUFFERING | FILE_ATTRIBUTE_NORMAL);
    if (handle == -1)
        return false;
    DWORD bytesRead = 0;
    const bool read =
        SetFilePointer(reinterpret_cast<HANDLE>(handle), static_cast<LONG>(entry.offset + offset), nullptr, FILE_BEGIN) !=
            INVALID_SET_FILE_POINTER &&
        ReadFile(reinterpret_cast<HANDLE>(handle), reinterpret_cast<void *>(address), size, &bytesRead, nullptr) &&
        bytesRead == size;
    CloseHandle(reinterpret_cast<HANDLE>(handle));
    DbgPrint("[codxe][T4 SP][Streaming] read %u bytes at %u of images.pak\n", size, entry.offset + offset);
    return read;
}

int DistancePriority(double distance)
{
    if (distance <= 0.0)
        return PRIORITY_APPLIED;
    return distance > 10000.0 ? (distance > 90000.0 ? 1 : 2) : 3;
}

void FailedRead(unsigned char *image, unsigned int slot, unsigned int size)
{
    *streamLoadingImage = 0;
    __lwsync();
    *ImageStreaming(image) = 0;
    DbgPrint("[codxe][T4 SP][Streaming] could not stream %s (%u bytes)\n", ImageName(image), size);
    R_StreamFreeSlot_Hook(slot);
}

// A load found no room (slotLock): it waits; one of a half size texture near the view has the oldest
// applied base levels, as many slots as it needs, go back to their half size (RateDeepSteps drops
// them as any, then they can be evicted).
// The deep image whose applied base level is the block at `first`, if it may give way.
DeepSteps *DemotableBase(unsigned int first, unsigned char *requester, unsigned int frame)
{
    const StreamBlock &block = blocks[first];
    if (block.kind != BLOCK_BASE_LEVEL || !block.image || block.image == requester)
        return nullptr;
    DeepSteps *steps = FindDeepSteps(block.image);
    return steps && steps->fullSlot == first && !Demoted(*steps, frame) ? steps : nullptr;
}

void NoRoom(unsigned char *image, unsigned int size, bool half, int priority)
{
    const unsigned int frame = *streamFrame;
    failedLoads[image] = frame;
    lastNoRoom = frame ? frame : 1;
    if (!half || priority < 3)
        return;
    unsigned int units = 1;
    while (units * SLOT_SIZE < size)
        units <<= 1;
    if (units > SLOTS_PER_REGION)
        return;
    // the place of its size (as R_StreamAlloc lays blocks) whose every block is evictable or an applied
    // base level that may give way, the fewest of those
    unsigned int best = MAX_SLOTS;
    unsigned int bestCount = 0xFFFFFFFF;
    for (unsigned int r = 0; r < rangeCount; ++r)
    {
        for (unsigned int start = ranges[r].firstSlot; start + units <= ranges[r].firstSlot + ranges[r].slotCount; start += units)
        {
            unsigned int count = 0;
            bool possible = true;
            for (unsigned int s = start; s < start + units && possible; ++s)
            {
                if (slotBlock[s] == NO_BLOCK)
                    continue;
                const unsigned int first = slotBlock[s];
                const StreamBlock &held = blocks[first];
                if (!held.image || held.priority >= PRIORITY_DROPPED)
                {
                    if (held.priority == PRIORITY_APPLIED && DemotableBase(first, image, frame))
                    {
                        if (s == start || slotBlock[s - 1] != first)
                            ++count;
                    }
                    else
                        possible = false;
                }
            }
            if (possible && count && count < bestCount)
            {
                best = start;
                bestCount = count;
            }
        }
    }
    if (best == MAX_SLOTS)
        return;
    for (unsigned int s = best; s < best + units; ++s)
    {
        if (slotBlock[s] == NO_BLOCK || (s != best && slotBlock[s - 1] == slotBlock[s]))
            continue;
        DeepSteps *steps = blocks[slotBlock[s]].priority == PRIORITY_APPLIED ? DemotableBase(slotBlock[s], image, frame) : nullptr;
        if (steps)
            steps->demotedAt = frame ? frame : 1;
    }
#if STREAM_LOG_FULL
    DbgPrint("[codxe][T4 SP][Streaming] %s: %u base levels give way\n", ImageName(image), bestCount);
#endif
}

// A deep image coming in steps: its half size texture first, then (asked for again inside its box)
// its base level.
void LoadDeepSteps(unsigned char *image, const ImagesPakImage &entry, double distance)
{
    const int priority = DistancePriority(distance);
    const bool loaded = !(*ImageStreamSlot(image) & 0x8000);
    EnterCriticalSection(&slotLock);
    DeepSteps *steps = FindDeepSteps(image);
    const bool wantsBase = loaded && steps && steps->halfSlot != NO_BLOCK && steps->fullSlot == NO_BLOCK &&
                           !Demoted(*steps, *streamFrame);
    if (wantsBase)
        blocks[steps->halfSlot].priority = PRIORITY_APPLIED; // kept while its base level is read
    LeaveCriticalSection(&slotLock);
    if (loaded && !wantsBase)
        return;

    const unsigned int offset = wantsBase ? 0 : entry.mipOffset;
    const unsigned int size = wantsBase ? entry.mipOffset : entry.size - entry.mipOffset;
    unsigned int block = 0;
    unsigned int slot = 0;
    if (!R_StreamAlloc_Hook(size, wantsBase ? PRIORITY_APPLIED : priority, &block, &slot))
    {
        EnterCriticalSection(&slotLock);
        NoRoom(image, size, !wantsBase, priority);
        LeaveCriticalSection(&slotLock);
        return;
    }
    const unsigned int address = SlotAddress(slot);
    if (!ReadPakPart(entry, offset, size, address))
    {
        if (!wantsBase)
            FailedRead(image, slot, size);
        else
            R_StreamFreeSlot_Hook(slot);
        return;
    }

    EnterCriticalSection(&slotLock);
    steps = FindDeepSteps(image);
    if (wantsBase)
    {
        // the half size texture may have gone while this was read
        if (!steps || steps->halfSlot == NO_BLOCK || (*ImageStreamSlot(image) & 0x8000))
        {
            RemoveBlock(slot);
            LeaveCriticalSection(&slotLock);
            return;
        }
        blocks[slot].kind = BLOCK_BASE_LEVEL;
        blocks[slot].image = image;
        steps->fullSlot = static_cast<unsigned short>(slot);
        RateDeepSteps(image, *steps); // the base level at once
    }
    else
    {
        if (!steps)
        {
            DeepSteps added;
            added.name = ImageName(image);
            steps = &(deepSteps[image] = added);
        }
        steps->entry = entry;
        steps->halfSlot = static_cast<unsigned short>(slot);
        steps->fullSlot = NO_BLOCK;
        steps->level = 0;
        steps->demotedAt = 0;
        blocks[slot].image = image;
        *ImageStreamSlot(image) = static_cast<unsigned short>(slot);
        R_StreamRecordUse_Hook(image, slot, *streamFrame, distance);
        RateDeepSteps(image, *steps); // its half size at once when it is near
    }
    LeaveCriticalSection(&slotLock);

    // inside its box already: its base level now too, not a load later (the view arrived there),
    // unless memory is short (the others' half sizes first)
    if (!wantsBase && priority == PRIORITY_APPLIED && !MemoryShort(*streamFrame))
        LoadDeepSteps(image, entry, distance);
}

// The game's loader for an image of the map's images.pak (disc 82428C28): its priority from the
// distance, a block, the read, applied at once when it is needed now. Deep images that come whole
// are read whole.
void LoadPakImage(unsigned char *image, const ImagesPakImage &entry, double distance)
{
    if (!*ImageStreaming(image) || !(*ImageStreamSlot(image) & 0x8000))
        return;

    const int priority = DistancePriority(distance);
    const bool whole = (entry.flags & IMAGES_PAK_DEEP) != 0;
    const unsigned int size = whole ? entry.size : 4 * *ImageBaseSize(image);
    unsigned int block = 0;
    unsigned int slot = 0;
    if (!R_StreamAlloc_Hook(size, priority, &block, &slot))
    {
        EnterCriticalSection(&slotLock);
        NoRoom(image, size, false, priority);
        LeaveCriticalSection(&slotLock);
        return;
    }
    const unsigned int address = SlotAddress(slot);
    if (!ReadPakPart(entry, 0, size, address))
    {
        FailedRead(image, slot, size);
        return;
    }

    if (whole)
    {
        ImagesPakImage target = entry;
        target.level2Offset = 0; // read whole: applied whole
        SetDeepTargets(image, target, 0, 0);
    }
    if (priority == PRIORITY_APPLIED)
    {
        while (!R_StreamQueueSwap(image, address))
            Sleep(1);
    }
    R_StreamRecordUse_Hook(image, slot, *streamFrame, distance);
    R_StreamAssignSlot_Hook(block, slot, image);
}

// The images of the map's images.pak are loaded here; the others (the disc's own maps') by the game,
// in its buffer.
void R_StreamLoadHighMip_Hook(unsigned char *image, double distance)
{
    if (image == candidateImage)
        distance = candidateDistance; // not its key (Propose)
    ImagesPakImage entry;
    if (!GetImagesPakImage(ImageName(image), &entry))
    {
        allocBaseRangeOnly = true;
        R_StreamLoadHighMip_Detour.GetOriginal<R_StreamLoadHighMip_t>()(image, distance);
        allocBaseRangeOnly = false;
        return;
    }

    extraPoolWanted = true;
#if STREAM_TEST_WHOLE
    LoadPakImage(image, entry, distance);
#else
    if ((entry.flags & IMAGES_PAK_DEEP) && entry.level2Offset && entry.mipOffset < entry.size)
        LoadDeepSteps(image, entry, distance);
    else
        LoadPakImage(image, entry, distance);
#endif
}
} // namespace

Streaming::Streaming()
{
    InitializeCriticalSection(&deepImagesLock);
    deepImages.clear();
    InitializeCriticalSection(&slotLock);
    ResetSlots();
    rangeCount = 0;
    allocBaseRangeOnly = false;
    if (*streamBufferAddress) // loaded after the game's render init
    {
        ranges[0].address = *streamBufferAddress;
        ranges[0].firstSlot = 0;
        ranges[0].slotCount = STREAM_BUFFER_SIZE / SLOT_SIZE;
        rangeCount = 1;
    }
    extraPool = nullptr;
    extraPoolWanted = false;
    extraPoolTried = false;

#define INSTALL(name)                                                                                                  \
    name##_Detour = Detour(name, name##_Hook);                                                                         \
    name##_Detour.Install()
    INSTALL(R_StreamLoadHighMip);
    INSTALL(R_StreamApplyQueue);
    INSTALL(R_StreamRevertHighMip);
    INSTALL(R_StreamInitSlots);
    INSTALL(R_StreamAlloc);
    INSTALL(R_StreamFreeSlot);
    INSTALL(R_StreamAssignSlot);
    INSTALL(R_StreamLockSlots);
    INSTALL(R_StreamUnlockSlots);
    INSTALL(R_StreamRecordUse);
    INSTALL(R_StreamConsiderMaterial);
    INSTALL(R_StreamUpdateSlots);
    INSTALL(R_StreamEvictAll);
    INSTALL(PMem_BeginAlloc);
    INSTALL(PMem_Free);
#undef INSTALL
}

Streaming::~Streaming()
{
    PMem_Free_Detour.Remove();
    PMem_BeginAlloc_Detour.Remove();
    R_StreamEvictAll_Detour.Remove();
    R_StreamUpdateSlots_Detour.Remove();
    R_StreamConsiderMaterial_Detour.Remove();
    R_StreamRecordUse_Detour.Remove();
    R_StreamUnlockSlots_Detour.Remove();
    R_StreamLockSlots_Detour.Remove();
    R_StreamAssignSlot_Detour.Remove();
    R_StreamFreeSlot_Detour.Remove();
    R_StreamAlloc_Detour.Remove();
    R_StreamInitSlots_Detour.Remove();
    R_StreamRevertHighMip_Detour.Remove();
    R_StreamApplyQueue_Detour.Remove();
    R_StreamLoadHighMip_Detour.Remove();
    DeleteCriticalSection(&slotLock);
    DeleteCriticalSection(&deepImagesLock);
}
} // namespace sp
} // namespace t4
