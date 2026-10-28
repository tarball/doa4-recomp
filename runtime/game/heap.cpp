// DOA4's heap functions (XAPI's): RtlCreateHeap sub_82795D28(flags, base, reserve, commit, lock, parameters),
// RtlAllocateHeap sub_827962D0(heap, flags, size), RtlFreeHeap sub_82796BC0(heap, flags, block), RtlReAllocateHeap
// sub_82796EA8(heap, flags, block, size), which allocates and frees through the other two, and RtlSizeHeap
// sub_82795570(heap, flags, block), for the C runtime's _msize; nothing else calls into the heap's code. One heap is
// created, XAPI's process heap at startup (sub_82793260), and everyone allocates from it: the C runtime's malloc and
// operator new, LocalAlloc and LocalFree (sub_827944C0, sub_82794508), libraries calling RtlAllocateHeap, and XMemAlloc
// sub_827930E0(size, attributes) (XAPI's default; DOA4 doesn't replace it) with XMemFree sub_82793178, which take virtual
// memory through LocalAlloc and physical memory otherwise. XMemAlloc's attributes' allocator ID (bits 16-23) says whose an
// allocation is.
//
// For netplay (runtime/netplay/heaps.h): every call is reported, and with the game heap, a second heap created right
// after the process heap with the same arguments takes the game's allocations; frees, reallocations and sizes follow the
// block to its heap.
#include "game/journal.h"
#include "guest_abi.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/heaps.h"

#include <array>
#include <cstdio>
#include <mutex>
#include <string>

extern "C" PPC_FUNC(__imp__sub_82795D28);
extern "C" PPC_FUNC(__imp__sub_827962D0);
extern "C" PPC_FUNC(__imp__sub_82796BC0);
extern "C" PPC_FUNC(__imp__sub_82796EA8);
extern "C" PPC_FUNC(__imp__sub_82795570);
extern "C" PPC_FUNC(__imp__sub_827930E0);
extern "C" PPC_FUNC(__imp__sub_82791B50);
extern "C" PPC_FUNC(__imp__sub_82791BE8);

namespace heaps = netplay::heaps;

namespace
{
    thread_local bool t_reallocating = false;                // RtlReAllocateHeap's own allocations and frees are its
    thread_local int t_allocatorId = heaps::kNoAllocatorId; // inside XMemAlloc: its caller's allocator ID

    // The heap that holds a block passed with the process heap: the game heap, if it lies in its memory.
    uint32_t HeapOf(uint32_t heap, uint32_t block)
    {
        const uint32_t gameHeap = heaps::GameHeap();
        if (gameHeap && block && heap == heaps::ProcessHeap() && heaps::InGameHeap(block))
            return gameHeap;
        return heap;
    }

    // DEBUG_GAME_HEAP_CHECK: the game heap's last operations, and a check of its free lists. XAPI's heap keeps 128 list
    // heads (Flink, Blink) at heap+0x180, each linking free blocks by the list entry at their header+8 (the header is the
    // 16 bytes before a block: its size and the previous block's in 16-byte units at +0 and +2, its flags at +5).
    struct Operation
    {
        uint64_t frame = 0;
        char kind = 0; // 'a'llocate, 'f'ree, 'r'eallocate
        uint32_t block = 0, size = 0, caller = 0;
    };
    std::mutex g_operationsLock;
    std::array<Operation, 64> g_operations;
    size_t g_operationCount = 0;

    void NoteOperation(char kind, uint32_t block, uint32_t size, uint32_t caller)
    {
        if (!heaps::GameHeapCheckEnabled())
            return;
        std::lock_guard lock(g_operationsLock);
        g_operations[g_operationCount++ % g_operations.size()] = Operation{ netplay::Frame(), kind, block, size, caller };
    }

    std::string RecentOperations()
    {
        std::lock_guard lock(g_operationsLock);
        std::string text;
        const size_t count = std::min(g_operationCount, g_operations.size());
        for (size_t i = g_operationCount - count; i < g_operationCount; i++)
        {
            const Operation& operation = g_operations[i % g_operations.size()];
            char line[96];
            snprintf(line, sizeof(line), "  frame %llu: %c %08X (%X bytes), called from %08X\n", (unsigned long long)operation.frame, operation.kind,
                operation.block, operation.size, operation.caller);
            text += line;
        }
        return text;
    }

    std::string CheckFreeLists()
    {
        const uint32_t heap = heaps::GameHeap();
        if (!heap)
            return {};
        char problem[160];
        for (uint32_t list = 0; list < 128; list++)
        {
            const uint32_t head = heap + 0x180 + list * 8;
            uint32_t previous = head;
            uint32_t entry = guest::LoadU32(head);
            for (uint32_t steps = 0; entry != head; steps++)
            {
                if (steps == 1 << 20 || !heaps::InGameHeap(entry) || (entry & 7))
                {
                    snprintf(problem, sizeof(problem), "free list %u: entry %08X after %08X lies outside the heap", list, entry, previous);
                    return problem;
                }
                if (guest::LoadU32(entry + 4) != previous)
                {
                    snprintf(problem, sizeof(problem), "free list %u: entry %08X links back to %08X, not %08X (block %08X)", list, entry,
                        guest::LoadU32(entry + 4), previous, entry + 8);
                    return problem;
                }
                previous = entry;
                entry = guest::LoadU32(entry);
            }
            if (guest::LoadU32(head + 4) != previous)
            {
                snprintf(problem, sizeof(problem), "free list %u: the head links back to %08X, not to its last entry %08X", list,
                    guest::LoadU32(head + 4), previous);
                return problem;
            }
        }
        return {};
    }

    [[maybe_unused]] const bool g_checkRegistered = [] {
        if (heaps::GameHeapCheckEnabled())
            heaps::SetGameHeapCheck(CheckFreeLists, RecentOperations);
        return true;
    }();
}

PPC_FUNC(sub_827930E0)
{
    const int previous = t_allocatorId;
    t_allocatorId = int((ctx.r4.u32 >> 16) & 0xFF);
    __imp__sub_827930E0(ctx, base);
    t_allocatorId = previous;
}

PPC_FUNC(sub_82795D28)
{
    const uint32_t flags = ctx.r3.u32, address = ctx.r4.u32, reserve = ctx.r5.u32, commit = ctx.r6.u32, lock = ctx.r7.u32,
                   parameters = ctx.r8.u32, lr = uint32_t(ctx.lr), r1 = ctx.r1.u32;
    heaps::BeginHeapCall();
    __imp__sub_82795D28(ctx, base);
    const uint32_t heap = ctx.r3.u32;
    heaps::EndHeapCall(heap);
    heaps::Created(heap, flags, reserve, lr, r1);
    if (!heap || heaps::ProcessHeap())
        return;
    // The first heap is the process heap; the game heap is another like it.
    heaps::SetProcessHeap(heap);
    if (!heaps::GameHeapEnabled() || address)
        return;
    ctx.r3.u64 = flags;
    ctx.r4.u64 = address;
    ctx.r5.u64 = reserve;
    ctx.r6.u64 = commit;
    ctx.r7.u64 = lock;
    ctx.r8.u64 = parameters;
    ctx.lr = lr;
    heaps::BeginHeapCall();
    __imp__sub_82795D28(ctx, base);
    const uint32_t gameHeap = ctx.r3.u32;
    heaps::EndHeapCall(gameHeap);
    if (gameHeap)
    {
        heaps::Created(gameHeap, flags, reserve, lr, r1);
        heaps::SetGameHeap(gameHeap);
        LOG_INFO("Game heap at %08X, beside the process heap at %08X", gameHeap, heap);
    }
    else
        LOG_ERROR("Game heap: RtlCreateHeap failed");
    ctx.r3.u64 = heap;
}

PPC_FUNC(sub_827962D0)
{
    const uint32_t size = ctx.r5.u32, lr = uint32_t(ctx.lr), r1 = ctx.r1.u32;
    if (const uint32_t gameHeap = heaps::GameHeap();
        gameHeap && !t_reallocating && ctx.r3.u32 == heaps::ProcessHeap() && heaps::IsGameAllocation(lr, r1, t_allocatorId))
        ctx.r3.u64 = gameHeap;
    const uint32_t heap = ctx.r3.u32;
    heaps::BeginHeapCall();
    __imp__sub_827962D0(ctx, base);
    heaps::EndHeapCall(heap);
    if (!t_reallocating)
        heaps::Allocated(heap, ctx.r3.u32, size, lr, r1, t_allocatorId);
    if (heap == heaps::GameHeap())
        NoteOperation('a', ctx.r3.u32, size, lr);
}

PPC_FUNC(sub_82796BC0)
{
    ctx.r3.u64 = HeapOf(ctx.r3.u32, ctx.r5.u32);
    const uint32_t heap = ctx.r3.u32;
    // Noted before the free, while the block's address can't be handed out again.
    if (!t_reallocating)
        heaps::Freed(heap, ctx.r5.u32, uint32_t(ctx.lr), ctx.r1.u32);
    if (heap == heaps::GameHeap())
        NoteOperation('f', ctx.r5.u32, 0, uint32_t(ctx.lr));
    heaps::BeginHeapCall();
    __imp__sub_82796BC0(ctx, base);
    heaps::EndHeapCall(heap);
}

PPC_FUNC(sub_82796EA8)
{
    const uint32_t block = ctx.r5.u32, size = ctx.r6.u32, lr = uint32_t(ctx.lr), r1 = ctx.r1.u32;
    if (const uint32_t gameHeap = heaps::GameHeap(); gameHeap && ctx.r3.u32 == heaps::ProcessHeap())
    {
        if (block ? heaps::InGameHeap(block) : heaps::IsGameAllocation(lr, r1, heaps::kNoAllocatorId))
            ctx.r3.u64 = gameHeap;
    }
    const uint32_t heap = ctx.r3.u32;
    t_reallocating = true;
    heaps::BeginHeapCall();
    __imp__sub_82796EA8(ctx, base);
    heaps::EndHeapCall(heap);
    t_reallocating = false;
    heaps::Reallocated(heap, block, ctx.r3.u32, size, lr, r1);
    if (heap == heaps::GameHeap())
        NoteOperation('r', ctx.r3.u32, size, lr);
}

PPC_FUNC(sub_82795570)
{
    ctx.r3.u64 = HeapOf(ctx.r3.u32, ctx.r5.u32);
    __imp__sub_82795570(ctx, base);
}

namespace
{
    void PhysicalAlloc(PPCContext& ctx, uint8_t* base)
    {
        const uint32_t size = ctx.r3.u32, lr = uint32_t(ctx.lr), r1 = ctx.r1.u32;
        __imp__sub_82791B50(ctx, base);
        heaps::PhysicalAllocated(ctx.r3.u32, size, lr, r1, t_allocatorId);
    }

    void PhysicalFree(PPCContext& ctx, uint8_t* base)
    {
        heaps::PhysicalFreed(ctx.r3.u32);
        __imp__sub_82791BE8(ctx, base);
    }
}

// XAPI's XPhysicalAlloc sub_82791B50(size, the highest physical address, alignment, protection) and XPhysicalFree
// sub_82791BE8(block), through which XMemAlloc and the game's code take physical memory. The game's: the arenas the
// mode's start makes (sub_82404E20: about 230 MB in nine blocks, among them the 0xF30000 bytes at [0x83035E74] where
// the fight records its frames for the KO's replay, a ring of 428-byte frames that sub_823F8C00 moves the replay's two
// samples through), and a block for each model's file (sub_823FC310, which reads it there; sub_823FC8B0 frees it, as
// the story mode's intro ends). Reported with their owners (netplay/heaps.h), from which the game describes its state in
// physical memory (netplay_state.cpp). From the game's frames, journaled (journal.cpp): a replay gets the blocks its
// first run did, and the frees wait until those frames are past rolling back.
PPC_FUNC(sub_82791B50)
{
    game::JournalCall(0x82791B50, PhysicalAlloc, ctx, base);
}

PPC_FUNC(sub_82791BE8)
{
    game::DeferCall(0x82791BE8, PhysicalFree, ctx, base);
}
