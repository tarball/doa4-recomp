// Host fibers for XAPI fibers (DOA4-specific addresses).
//
// XAPI's SwitchToFiber saves the caller's registers into its fiber record, loads the target's, and
// tail-calls KeSetCurrentStackPointers to move r1 and "return" into the target's saved LR. Recompiled
// code can't do that: a guest blr is a host return. Instead, each guest fiber runs on a host stack of
// its own with its own PPCContext, and the switch switches host stacks.
//
// XAPI fiber record (allocated by CreateFiber / ConvertThreadToFiber):
//   [0] parameter  [4] stack allocation base  [8] stack base (top)  [12] stack limit
//   [28] saved LR (0x82793F70, the startup trampoline, for a fiber that hasn't run)
//   [48] saved r1 (64-bit)  [288] saved r31 (64-bit; the start address for a new fiber)
//
// The runtime switches the host stacks itself, as Windows' fibers would, rather than with Windows' fibers, so that a
// savestate can hold what a suspended fiber keeps on the host (game/fibers.h): SwitchHostStacks pushes the nonvolatile
// registers (rbx, rbp, rsi, rdi, r12-r15, xmm6-xmm15, MXCSR and the x87 control word) on the fiber's own stack and keeps
// the stack pointer, so a suspended fiber is its stack pointer and the bytes of its stack above it, which a load copies
// back to the same addresses. That's what lets savestates load across a fight's transitions, where the fight's main
// task yields from inside its phase functions with their counters in host registers (ANALYSIS.md, Phase 6). Each switch
// also sets the stack's bounds in the thread's TIB, which exception dispatch, stack walks and __chkstk read. A
// fiber's stack is committed whole (16 MB, as the Windows fibers' were), with an inaccessible page below it; its top
// holds a return address of 0, so stack walks end there. Not for images built with /CETCOMPAT: the hardware shadow
// stack would refuse the switch's return into another stack.
//
// For netplay's savestates (game/fibers.h), a suspended fiber's state is its PPCContext, plus what its recompiled code
// holds on the host while suspended: its host stack and nonvolatile registers, which hold only constants during play
// (ANALYSIS.md, Phase 6). A sync test hashes each fiber's host state at its switches, to compare its two runs.
#include "game/fibers.h"
#include "game/frame_times.h"
#include "game/journal.h"
#include "gpu/command_processor.h"
#include "gpu/renderer.h"
#include "guest_abi.h"
#include "guest_memory.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/sync_test.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

extern "C" PPC_FUNC(__imp__sub_82792588); // XAPI DeleteFiber

namespace
{
    constexpr uint32_t kPcrCurrentThread = 0x100;
    constexpr uint32_t kStartupTrampoline = 0x82793F70; // a new fiber's saved LR
    constexpr uint32_t kStartFiber = 0x827925F8;        // what the trampoline calls with the start routine: runs it, then exits
    constexpr uint32_t kPcrStackBase = 0x70;
    constexpr uint32_t kPcrStackLimit = 0x74;
    constexpr uint32_t kKThreadStackBase = 0x5C;
    constexpr uint32_t kKThreadStackLimit = 0x60;
    constexpr uint32_t kKThreadFiber = 0x164;
    constexpr size_t kHostFiberStack = 16 * 1024 * 1024;
    constexpr size_t kGuardSize = 4096;

    // What SwitchHostStacks pushes, from the suspended stack pointer up: xmm6-xmm15, MXCSR and the x87 control word, the
    // eight general registers (r15 first, rbp last), then the return address.
    constexpr size_t kSavedMxcsr = 160, kSavedControlWord = 164, kSavedReturn = 232, kSavedSize = 240;

    struct HostFiber
    {
        uint32_t record = 0;
        std::unique_ptr<PPCContext> context; // only for fibers started by the runtime; a thread's own has none
        uint64_t serial = 0;                 // tells fibers apart that reuse a record (DEBUG_FIBERS, savestates)
        uint64_t hostState = 0;              // a hash of the host state it held at its last switch (when tracked)
        uint32_t yieldSite = 0;              // the guest return address of its last switch away (DEBUG_FRAME_TIMES)
        bool running = false;                // some thread runs on it
        // The frame the game deleted it in, while the deletion is deferred (netplay's journal, sub_82792588): the game's
        // no longer, so savestates from then on leave it out, though it stays until the deletion is made.
        std::optional<uint64_t> deletedAt;
        uintptr_t rsp = 0;                   // while suspended: its stack pointer, below the registers it saved
        // Its stack, [stackLimit, stackBase): a fiber the runtime started has one of its own (`allocation`, with the guard
        // page below), a thread's own fiber its thread's, as the TIB says when it switches away.
        void* stackBase = nullptr;
        void* stackLimit = nullptr;
        void* allocation = nullptr;
    };

    std::mutex g_lock;
    std::unordered_map<uint32_t, std::unique_ptr<HostFiber>> g_fibers; // guest record -> host fiber
    std::vector<std::unique_ptr<HostFiber>> g_retired; // threads' own fibers whose records were deleted (a thread may still run on one)
    uint64_t g_nextSerial = 1;

    // Per host thread: the fiber it runs on (none before its first switch), its own (its thread's stack), and an exit to
    // rethrow there.
    thread_local HostFiber* t_current = nullptr;
    thread_local HostFiber* t_threadFiber = nullptr;
    thread_local std::exception_ptr t_pendingExit;

    uint64_t Load64(uint32_t address)
    {
        return *guest::Translate<be<uint64_t>>(address);
    }

    // Pushes the caller's nonvolatile registers on its stack, stores its stack pointer at `from`, and resumes the fiber
    // whose stack pointer is `to`, which was suspended the same way (or set up so: NewStack), by popping its registers.
    extern "C" __attribute__((naked)) void SwitchHostStacks(uintptr_t* /*from: rcx*/, uintptr_t /*to: rdx*/)
    {
        __asm__ volatile(
            "pushq %rbp\n\t"
            "pushq %rbx\n\t"
            "pushq %rsi\n\t"
            "pushq %rdi\n\t"
            "pushq %r12\n\t"
            "pushq %r13\n\t"
            "pushq %r14\n\t"
            "pushq %r15\n\t"
            "subq $168, %rsp\n\t"
            "movdqu %xmm6, 0(%rsp)\n\t"
            "movdqu %xmm7, 16(%rsp)\n\t"
            "movdqu %xmm8, 32(%rsp)\n\t"
            "movdqu %xmm9, 48(%rsp)\n\t"
            "movdqu %xmm10, 64(%rsp)\n\t"
            "movdqu %xmm11, 80(%rsp)\n\t"
            "movdqu %xmm12, 96(%rsp)\n\t"
            "movdqu %xmm13, 112(%rsp)\n\t"
            "movdqu %xmm14, 128(%rsp)\n\t"
            "movdqu %xmm15, 144(%rsp)\n\t"
            "stmxcsr 160(%rsp)\n\t"
            "fnstcw 164(%rsp)\n\t"
            "movq %rsp, (%rcx)\n\t"
            "movq %rdx, %rsp\n\t"
            "ldmxcsr 160(%rsp)\n\t"
            "fldcw 164(%rsp)\n\t"
            "movdqu 0(%rsp), %xmm6\n\t"
            "movdqu 16(%rsp), %xmm7\n\t"
            "movdqu 32(%rsp), %xmm8\n\t"
            "movdqu 48(%rsp), %xmm9\n\t"
            "movdqu 64(%rsp), %xmm10\n\t"
            "movdqu 80(%rsp), %xmm11\n\t"
            "movdqu 96(%rsp), %xmm12\n\t"
            "movdqu 112(%rsp), %xmm13\n\t"
            "movdqu 128(%rsp), %xmm14\n\t"
            "movdqu 144(%rsp), %xmm15\n\t"
            "addq $168, %rsp\n\t"
            "popq %r15\n\t"
            "popq %r14\n\t"
            "popq %r13\n\t"
            "popq %r12\n\t"
            "popq %rdi\n\t"
            "popq %rsi\n\t"
            "popq %rbx\n\t"
            "popq %rbp\n\t"
            "retq\n\t");
    }

    // Switches the calling thread from `from`, the fiber it runs on, to `to`; returns when a switch comes back to `from`.
    void Switch(HostFiber* from, HostFiber* to)
    {
        // A fiber of the runtime's resumes from its own stack, returning into a switch or FiberStart.
        if (to->allocation)
        {
            uint64_t resume = 0;
            const bool inside = to->rsp >= uintptr_t(to->stackLimit) && to->rsp + kSavedSize < uintptr_t(to->stackBase);
            if (inside)
                memcpy(&resume, reinterpret_cast<const void*>(to->rsp + kSavedReturn), sizeof(resume));
            if (!inside || !resume)
            {
                LOG_ERROR("fibers: fiber %08X (serial %llu) can't be resumed: its stack pointer %llX, its stack %p-%p, returning to %llX",
                    to->record, (unsigned long long)to->serial, (unsigned long long)to->rsp, to->stackLimit, to->stackBase,
                    (unsigned long long)resume);
                std::abort();
            }
        }
        auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
        from->stackBase = tib->StackBase;
        from->stackLimit = tib->StackLimit;
        from->running = false;
        to->running = true;
        tib->StackBase = to->stackBase;
        tib->StackLimit = to->stackLimit;
        t_current = to;
        SwitchHostStacks(&from->rsp, to->rsp);
    }

    // A fiber's first run, switched to through the stack NewStack set up, as if called with a return address of 0.
    [[noreturn]] void FiberStart()
    {
        HostFiber* fiber = t_current;
        try
        {
            // First run: the startup trampoline (the record's saved LR) passes the start routine, which XAPI keeps in the saved
            // r31, to kStartFiber. That's called directly, on the record's stack: recompiled code may keep r31 in a local
            // rather than the context.
            PPCContext& ctx = *fiber->context;
            ctx.r1.u64 = uint32_t(Load64(fiber->record + 48));
            if (const uint32_t start = guest::LoadU32(fiber->record + 28); start != kStartupTrampoline)
                LOG_ERROR("Fiber 0x%08X starts at %08X, not the startup trampoline", fiber->record, start);
            kernel::CallGuest(ctx, kStartFiber, { Load64(fiber->record + 288) });
            LOG_ERROR("Fiber 0x%08X returned from its start routine", fiber->record);
        }
        catch (...)
        {
            // The thread is exiting from this fiber: finish it on the thread's own fiber.
            t_pendingExit = std::current_exception();
        }
        Switch(fiber, t_threadFiber);
        LOG_ERROR("Fiber 0x%08X was resumed after it finished", fiber->record);
        std::abort();
    }

    // A new fiber's host stack, set up for SwitchHostStacks to resume it in FiberStart: the registers it pops (MXCSR as
    // the fiber's context caches it, since the generated code changes MXCSR only when its cache says the mode changes,
    // and the creator's x87 control word), FiberStart as the return address, and above it a return address of 0.
    void NewStack(HostFiber& fiber)
    {
        fiber.allocation = VirtualAlloc(nullptr, kHostFiberStack + kGuardSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        DWORD previous = 0;
        if (!fiber.allocation || !VirtualProtect(fiber.allocation, kGuardSize, PAGE_NOACCESS, &previous))
        {
            LOG_ERROR("Fiber 0x%08X: no host stack (%lu)", fiber.record, GetLastError());
            std::abort();
        }
        uint8_t* const limit = static_cast<uint8_t*>(fiber.allocation) + kGuardSize;
        uint8_t* const top = limit + kHostFiberStack;
        fiber.stackLimit = limit;
        fiber.stackBase = top;
        uint8_t* const saved = top - 8 - kSavedSize;
        memset(saved, 0, kSavedSize + 8);
        const uint32_t mxcsr = fiber.context->fpscr.csr;
        uint16_t controlWord = 0;
        __asm__ volatile("fnstcw %0" : "=m"(controlWord));
        memcpy(saved + kSavedMxcsr, &mxcsr, sizeof(mxcsr));
        memcpy(saved + kSavedControlWord, &controlWord, sizeof(controlWord));
        const uint64_t start = reinterpret_cast<uint64_t>(&FiberStart);
        memcpy(saved + kSavedReturn, &start, sizeof(start));
        fiber.rsp = reinterpret_cast<uintptr_t>(saved);
    }

    HostFiber* FiberFor(uint32_t record, const PPCContext& creator)
    {
        std::lock_guard lock(g_lock);
        auto& entry = g_fibers[record];
        if (!entry)
        {
            entry = std::make_unique<HostFiber>();
            entry->record = record;
            entry->context = std::make_unique<PPCContext>(creator);
            entry->serial = g_nextSerial++;
            NewStack(*entry);
        }
        return entry.get();
    }

    // The fiber the calling thread runs on: on its first switch, its own (its thread's stack), which the guest knows as
    // `record` (the thread's converted fiber).
    HostFiber* CurrentFiber(uint32_t record)
    {
        if (t_current)
        {
            if (t_current->record != record)
            {
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    LOG_ERROR("fibers: the guest switches from fiber %08X, but the host runs %08X (logged once)", record, t_current->record);
                }
            }
            return t_current;
        }
        std::lock_guard lock(g_lock);
        auto& entry = g_fibers[record];
        if (entry && entry->context)
        {
            LOG_ERROR("fibers: a thread's first switch is from fiber %08X, which the runtime started", record);
            std::abort();
        }
        if (!entry)
        {
            entry = std::make_unique<HostFiber>();
            entry->record = record;
            entry->serial = g_nextSerial++;
        }
        entry->running = true;
        t_current = entry.get();
        if (!t_threadFiber)
            t_threadFiber = entry.get();
        return entry.get();
    }

    int DebugFibers()
    {
        static const int level = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DEBUG_FIBERS");
            return setting ? std::atoi(setting) : 0;
        }();
        return level;
    }

    // Whether each task fiber's host state is hashed at its switches (in a sync test, which compares its runs').
    bool TrackHostState()
    {
        static const bool track = netplay::synctest::Enabled();
        return track;
    }

    // DEBUG_FIBERS and TrackHostState: the switching guest code's registers.
    thread_local CONTEXT t_switchRegisters;

    uint64_t Fnv(uint64_t hash, const void* data, size_t bytes) // in 8-byte words
    {
        for (size_t i = 0; i < bytes; i += 8)
            hash = (hash ^ *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(data) + i)) * 0x100000001B3ull;
        return hash;
    }

    // The host state the guest code that switched holds while suspended: its stack, from its call's return address to
    // the stack's base, and its nonvolatile registers.
    struct SuspendedState
    {
        size_t stackBytes = 0;
        uint64_t stackHash = 0;
        // rbx, rbp, rsi, rdi, r12-r15, MXCSR, the x87 control word, then xmm6-xmm15 (two words each). MXCSR without its
        // exception flags, which record what happened since it was last set and which nothing reads (a guest mffs reads
        // only the rounding mode).
        std::array<uint64_t, 30> values{};
        uint64_t Hash() const { return Fnv(stackHash, values.data(), sizeof(values)); }
    };

    SuspendedState CaptureSuspendedState(const CONTEXT& registers)
    {
        constexpr uint64_t kFnvBasis = 0xCBF29CE484222325ull;
        SuspendedState state;
        const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        const auto* stackTop = reinterpret_cast<const uint8_t*>(registers.Rsp - 8);
        state.stackBytes = size_t(static_cast<const uint8_t*>(tib->StackBase) - stackTop);
        state.stackHash = Fnv(kFnvBasis, stackTop, state.stackBytes);
        state.values = { registers.Rbx, registers.Rbp, registers.Rsi, registers.Rdi, registers.R12, registers.R13, registers.R14, registers.R15,
            registers.MxCsr & ~0x3Fu, registers.FltSave.ControlWord };
        memcpy(&state.values[10], &registers.Xmm6, 10 * sizeof(M128A));
        return state;
    }

    // DEBUG_FIBERS=1 logs:
    // - each switch from a fiber at a call chain not seen before (the guest return addresses from the switch up), with
    //   the frame and the chain's number;
    // - a fiber's start routine when it first runs;
    // - each game frame in which a task fiber (one the runtime started) switched from another chain than at its last
    //   switch ("F #a>#b"), or from the same chain holding another host state ("F #b stack", "F #b rbx/xmm6"): the
    //   switching code's stack, from its call to the switch up to the stack's base, whose size is logged, and its
    //   nonvolatile registers, which the switch saves outside the stack ("F new #b" for a fiber's first switch);
    // - every 600 frames, the switches from each fiber since the last report, with the stack it holds at the switch and
    //   how often its host state changed since the fiber's last switch from the same chain.
    // DEBUG_FIBERS=2 also dumps the frame shown after a game frame with changes, as DUMP_FRAMES would (at most one every
    // 30 frames, 300 in all). For finding what a frame's fibers hold across their switches, and when that changes.
    void DebugSwitch(const PPCContext& ctx, uint32_t current, uint32_t target, const SuspendedState& suspended)
    {
        const int level = DebugFibers();
        static std::mutex lock;
        static std::map<std::pair<uint32_t, std::string>, uint64_t> chains;
        static std::unordered_map<std::string, uint32_t> chainNumbers;
        static std::map<uint32_t, uint64_t> switches;
        static uint64_t reportedFrame = 0;
        // The switch's call site, then the callers' return addresses.
        char text[16];
        snprintf(text, sizeof(text), "%08X", uint32_t(ctx.lr));
        std::string readable = text;
        for (uint32_t returnAddress : guest::CallChain(ctx.r1.u32, 12))
        {
            snprintf(text, sizeof(text), " %08X", returnAddress);
            readable += text;
        }
        const uint64_t frameNumber = gpu::GetStatistics().swaps.load();
        constexpr uint64_t kFnvBasis = 0xCBF29CE484222325ull;
        const size_t stackBytes = suspended.stackBytes;
        const uint64_t stackHash = suspended.stackHash;
        const std::array<uint64_t, 30>& values = suspended.values;
        const uint64_t registerHash = Fnv(kFnvBasis, values.data(), sizeof(values));
        uint64_t serial = 0; // a task fiber's
        {
            std::lock_guard fibers(g_lock);
            if (const auto it = g_fibers.find(current); it != g_fibers.end() && it->second->context)
                serial = it->second->serial;
        }
        struct StackStats
        {
            size_t bytes = 0;
            uint64_t same = 0, changed = 0;
        };
        static std::map<std::pair<uint32_t, std::string>, uint64_t> lastHashes; // (fiber, chain) -> host state hash
        static std::map<uint32_t, StackStats> stackStats;
        std::lock_guard guard(lock);
        const uint32_t chain = chainNumbers.emplace(readable, uint32_t(chainNumbers.size() + 1)).first->second;
        {
            StackStats& stats = stackStats[current];
            stats.bytes = stackBytes;
            const uint64_t stateHash = Fnv(stackHash, &registerHash, sizeof(registerHash));
            auto [it, inserted] = lastHashes.emplace(std::make_pair(current, readable), stateHash);
            if (!inserted)
            {
                (it->second == stateHash ? stats.same : stats.changed)++;
                it->second = stateHash;
            }
        }
        switches[current]++;
        if (chains.emplace(std::make_pair(current, readable), frameNumber).second)
            LOG_INFO("fibers: frame %llu: %08X -> %08X (r1 %08X) at #%u: %s", (unsigned long long)frameNumber, current, target, ctx.r1.u32, chain,
                readable.c_str());
        // Changes since each task fiber's last switch, one line per game frame.
        struct LastSwitch
        {
            uint64_t serial = 0;
            uint32_t chain = 0;
            uint64_t stackHash = 0;
            std::array<uint64_t, 30> values{};
        };
        static std::map<uint32_t, LastSwitch> lastSwitches;
        static std::string changes;
        static uint64_t changeFrame = 0, changeSwap = 0, dumpSwap = 0, dumps = 0;
        if (const uint64_t gameFrame = netplay::Frame(); gameFrame != changeFrame)
        {
            if (!changes.empty())
                LOG_INFO("fibers: game frame %llu (frame %llu):%s", (unsigned long long)changeFrame, (unsigned long long)changeSwap, changes.c_str());
            changes.clear();
            changeFrame = gameFrame;
        }
        if (serial)
        {
            LastSwitch& last = lastSwitches[current];
            char part[48];
            std::string entry;
            if (last.serial != serial)
            {
                snprintf(part, sizeof(part), " %08X new #%u", current, chain);
                entry = part;
            }
            else if (last.chain != chain)
            {
                snprintf(part, sizeof(part), " %08X #%u>#%u", current, last.chain, chain);
                entry = part;
            }
            else if (last.stackHash != stackHash || last.values != values)
            {
                snprintf(part, sizeof(part), " %08X #%u%s", current, chain, last.stackHash != stackHash ? " stack" : "");
                entry = part;
                // The registers that changed (an xmm register once).
                static const char* const kNames[] = { "rbx", "rbp", "rsi", "rdi", "r12", "r13", "r14", "r15", "mxcsr", "fpcw" };
                const char* separator = " ";
                for (size_t i = 0; i < values.size(); i++)
                {
                    if (values[i] == last.values[i] || (i >= 10 && (i & 1) && values[i - 1] != last.values[i - 1]))
                        continue;
                    if (i == 8 || i == 9) // MXCSR and the x87 control word: modes, with their values
                        snprintf(part, sizeof(part), "%s%s %llX>%llX", separator, kNames[i], (unsigned long long)last.values[i],
                            (unsigned long long)values[i]);
                    else if (i < 10)
                        snprintf(part, sizeof(part), "%s%s", separator, kNames[i]);
                    else
                        snprintf(part, sizeof(part), "%sxmm%zu", separator, 6 + (i - 10) / 2);
                    entry += part;
                    separator = "/";
                }
            }
            last = { serial, chain, stackHash, values };
            if (!entry.empty())
            {
                snprintf(part, sizeof(part), " %zu", stackBytes);
                entry += part;
                if (changes.empty())
                {
                    changeSwap = frameNumber;
                    if (level >= 2 && dumps < 300 && (dumps == 0 || frameNumber >= dumpSwap + 30))
                    {
                        gpu::RequestFrameDump();
                        dumpSwap = frameNumber;
                        dumps++;
                    }
                }
                else
                    changes += ',';
                changes += entry;
            }
        }
        // A fiber switched to for the first time (records are reused, with new stacks): its start routine.
        static std::map<uint32_t, uint32_t> stacks;
        if (const uint32_t stack = guest::LoadU32(target + 8); stacks[target] != stack)
        {
            stacks[target] = stack;
            LOG_INFO("fibers: frame %llu (game frame %llu): %08X starts at %08X (stack %08X-%08X)", (unsigned long long)frameNumber,
                (unsigned long long)netplay::Frame(), target, uint32_t(Load64(target + 288)), guest::LoadU32(target + 12), stack);
        }
        if (frameNumber >= reportedFrame + 600)
        {
            std::string counts;
            for (const auto& [fiber, count] : switches)
            {
                const StackStats& stats = stackStats[fiber];
                char entry[96];
                snprintf(entry, sizeof(entry), " %08X:%llu (stack %zu bytes, same %llu, changed %llu)", fiber, (unsigned long long)count, stats.bytes,
                    (unsigned long long)stats.same, (unsigned long long)stats.changed);
                counts += entry;
            }
            LOG_INFO("fibers: frames %llu-%llu: switches from%s", (unsigned long long)reportedFrame, (unsigned long long)frameNumber, counts.c_str());
            switches.clear();
            stackStats.clear();
            reportedFrame = frameNumber;
        }
    }
}

// XAPI SwitchToFiber's core: r3 = target fiber record.
PPC_FUNC(sub_82793F80)
{
    const uint32_t target = ctx.r3.u32;
    const uint32_t pcr = ctx.r13.u32;
    const uint32_t kthread = guest::LoadU32(pcr + kPcrCurrentThread);
    const uint32_t current = guest::LoadU32(kthread + kKThreadFiber);

    // The bookkeeping XAPI does around the switch: current fiber and stack bounds.
    guest::StoreU32(kthread + kKThreadFiber, target);
    guest::StoreU32(kthread + kKThreadStackBase, guest::LoadU32(target + 8));
    guest::StoreU32(kthread + kKThreadStackLimit, guest::LoadU32(target + 12));
    guest::StoreU32(pcr + kPcrStackBase, guest::LoadU32(target + 8));
    guest::StoreU32(pcr + kPcrStackLimit, guest::LoadU32(target + 12));

    HostFiber* self = CurrentFiber(current);
    HostFiber* next = FiberFor(target, ctx);
    const bool debug = DebugFibers() > 0;
    if (debug || TrackHostState())
    {
        // What the guest code that switched holds while suspended: its nonvolatile registers as at its call here (unwound
        // past this function, whose own frame has dead slots), and its stack (from the call's return address).
        RtlCaptureContext(&t_switchRegisters);
        DWORD64 imageBase = 0;
        if (RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(t_switchRegisters.Rip, &imageBase, nullptr))
        {
            void* handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, t_switchRegisters.Rip, function, &t_switchRegisters, &handlerData, &establisherFrame,
                nullptr);
        }
        const SuspendedState suspended = CaptureSuspendedState(t_switchRegisters);
        if (TrackHostState() && self->context)
            self->hostState = suspended.Hash();
        if (debug)
            DebugSwitch(ctx, current, target, suspended);
    }
    if (next != self)
    {
        // DEBUG_FRAME_TIMES: the scheduler's switch to a task fiber, until it yields back.
        const bool timed = self == t_threadFiber && next->context && game::frametimes::Recording() && game::frametimes::OnMainThread();
        const int64_t start = timed ? game::frametimes::Now() : 0;
        self->yieldSite = uint32_t(ctx.lr);
        Switch(self, next);
        if (timed)
            game::frametimes::AddTask(next->record, next->yieldSite, next->context->r1.u32, game::frametimes::Now() - start);
    }

    // Resumed by a switch back to this fiber.
    if (t_pendingExit && t_current == t_threadFiber)
    {
        std::exception_ptr exit = std::exchange(t_pendingExit, nullptr);
        std::rethrow_exception(exit);
    }
    (void)base;
}

namespace
{
    // XAPI DeleteFiber made: frees the host fiber, then XAPI frees the stack and record.
    void DeleteFiberNow(PPCContext& ctx, uint8_t* base)
    {
        const uint32_t record = ctx.r3.u32;
        if (DebugFibers())
            LOG_INFO("fibers: game frame %llu: %08X deleted", (unsigned long long)netplay::Frame(), record);
        {
            std::lock_guard lock(g_lock);
            const auto it = g_fibers.find(record);
            if (it != g_fibers.end() && !it->second->running)
            {
                if (it->second->allocation) // started by the runtime (not a converted thread)
                    VirtualFree(it->second->allocation, 0, MEM_RELEASE);
                else
                    g_retired.push_back(std::move(it->second));
                g_fibers.erase(it);
            }
        }
        __imp__sub_82792588(ctx, base);
    }
}

// XAPI DeleteFiber: r3 = fiber record. While netplay's journal runs (savestates may go back to before the deletion: a
// rollback, a sync test), deferred as a release (netplay::journal::Defer) until no savestate from before it can be loaded,
// so a load across it finds the fiber as it was (story mode makes and deletes fibers inside a fight: the pre-fight
// dialogue's task, deleted as the intro ends, or as a press skips it); a replay that doesn't delete it again drops the
// deletion. Else made. Deferred, the fiber is the game's no longer from this frame on (deletedAt): savestates saved
// after it leave it out, and their loads leave it be, as its deletion will be made (a load of a later savestate that
// still had it failed once the deletion was made, though no savestate from before it was kept).
PPC_FUNC(sub_82792588)
{
    const uint32_t record = ctx.r3.u32;
    game::DeferCall(0x82792588, DeleteFiberNow, ctx, base);
    std::lock_guard lock(g_lock);
    if (const auto it = g_fibers.find(record); it != g_fibers.end())
        it->second->deletedAt = netplay::Frame();
}

namespace
{
    struct FiberSnapshot
    {
        uint32_t record = 0;
        uint64_t serial = 0;
        uint32_t r1 = 0;
        std::vector<uint32_t> chain; // guest return addresses from r1 up
        uint64_t hostState = 0;
        PPCContext context;
        // Its host stack from its stack pointer, below the registers it saved, to its base.
        uintptr_t rsp = 0;
        std::vector<uint8_t> stack;
    };

    std::string FiberText(uint32_t record, uint32_t r1, const std::vector<uint32_t>& chain)
    {
        char text[48];
        snprintf(text, sizeof(text), "fiber %08X (r1 %08X:", record, r1);
        std::string result = text;
        for (uint32_t address : chain)
        {
            snprintf(text, sizeof(text), " %08X", address);
            result += text;
        }
        return result + ")";
    }

    // Each must be the same fiber, suspended; then each gets its context, and its host stack and stack pointer as they
    // were, wherever it's suspended now. Fibers started since the save, which the game didn't have then, are deleted
    // (XAPI's DeleteFiber, on the frame boundary's guest context): the frames replayed from the save start their own.
    // Deletions are deferred while savestates may go back to before them (sub_82792588), so a fiber the game deleted
    // since the save is still there (and the game's again: a replay deletes it anew or not); one it deleted before the
    // save (`frame`'s boundary), which the save left out, stays as it is until its deletion is made.
    std::string LoadFibers(const std::vector<FiberSnapshot>& snapshots, uint64_t frame, std::string* note)
    {
        std::vector<uint32_t> started; // records of fibers started since the save, deleted once loaded
        std::unique_lock lock(g_lock);
        std::map<uint32_t, HostFiber*> fibers; // the runtime's fibers now
        for (const auto& [record, fiber] : g_fibers)
        {
            if (fiber->context && !(fiber->deletedAt && *fiber->deletedAt < frame))
                fibers[record] = fiber.get();
        }
        for (const FiberSnapshot& snapshot : snapshots)
        {
            const auto it = fibers.find(snapshot.record);
            if (it == fibers.end() || it->second->serial != snapshot.serial)
                return FiberText(snapshot.record, snapshot.r1, snapshot.chain) + " was deleted since the save";
            if (it->second->running)
                return FiberText(snapshot.record, snapshot.r1, snapshot.chain) + " is running";
            fibers.erase(it);
        }
        PPCContext* boundary = netplay::BoundaryContext();
        for (const auto& [record, fiber] : fibers)
        {
            if (fiber->running || !boundary)
                return FiberText(fiber->record, fiber->context->r1.u32, guest::CallChain(fiber->context->r1.u32, 12)) + " started since the save" +
                    (fiber->running ? " is running" : ", and the load isn't at a frame boundary");
            started.push_back(record);
        }
        std::string moved;
        size_t elsewhere = 0, otherState = 0;
        for (const FiberSnapshot& snapshot : snapshots)
        {
            HostFiber& fiber = *g_fibers.find(snapshot.record)->second;
            const uint32_t r1 = fiber.context->r1.u32;
            const std::vector<uint32_t> chain = guest::CallChain(r1, 12);
            if (r1 != snapshot.r1 || chain != snapshot.chain)
            {
                elsewhere++;
                moved += "; " + FiberText(snapshot.record, r1, chain) + " back to " + FiberText(snapshot.record, snapshot.r1, snapshot.chain);
            }
            else if (fiber.hostState != snapshot.hostState)
                otherState++;
            if (DebugFibers())
                LOG_INFO("fibers: load: %08X (serial %llu) back to stack pointer %llX (%zu bytes of its stack, %p-%p) from %llX",
                    snapshot.record, (unsigned long long)snapshot.serial, (unsigned long long)snapshot.rsp, snapshot.stack.size(), fiber.stackLimit,
                    fiber.stackBase, (unsigned long long)fiber.rsp);
            *fiber.context = snapshot.context;
            memcpy(reinterpret_cast<void*>(snapshot.rsp), snapshot.stack.data(), snapshot.stack.size());
            fiber.rsp = snapshot.rsp;
            fiber.hostState = snapshot.hostState;
            fiber.deletedAt.reset();
        }
        lock.unlock();
        for (uint32_t record : started)
        {
            const PPCContext saved = *boundary;
            boundary->r3.u64 = record;
            DeleteFiberNow(*boundary, guest::g_base);
            *boundary = saved;
        }
        if (note)
        {
            *note = std::to_string(snapshots.size()) + " fibers loaded, " + std::to_string(elsewhere) + " of them suspended elsewhere than at the save and " +
                std::to_string(otherState) + " at the same place holding other host state" + moved;
            if (!started.empty())
                *note += "; " + std::to_string(started.size()) + " started since the save deleted";
        }
        return {};
    }
}

namespace game
{
    netplay::HostState SaveFibers(std::string& error)
    {
        netplay::HostState state;
        auto snapshots = std::make_shared<std::vector<FiberSnapshot>>();
        {
            std::lock_guard lock(g_lock);
            for (const auto& [record, fiber] : g_fibers)
            {
                if (!fiber->context) // a thread's own fiber (the main thread's runs this)
                    continue;
                if (fiber->deletedAt) // deleted by the game, the deletion deferred
                    continue;
                if (fiber->running)
                {
                    error = FiberText(record, fiber->context->r1.u32, guest::CallChain(fiber->context->r1.u32, 12)) + " is running";
                    return {};
                }
                FiberSnapshot& snapshot = snapshots->emplace_back();
                snapshot.record = record;
                snapshot.serial = fiber->serial;
                snapshot.r1 = fiber->context->r1.u32;
                snapshot.chain = guest::CallChain(snapshot.r1, 12);
                snapshot.hostState = fiber->hostState;
                snapshot.context = *fiber->context;
                snapshot.rsp = fiber->rsp;
                const auto* stack = reinterpret_cast<const uint8_t*>(fiber->rsp);
                snapshot.stack.assign(stack, static_cast<const uint8_t*>(fiber->stackBase));
                const uint32_t limit = guest::LoadU32(record + 12), stackBase = guest::LoadU32(record + 8);
                if (limit < stackBase)
                    state.stacks.emplace_back(limit, stackBase);
                if (limit < snapshot.r1)
                    state.freeStack.emplace_back(limit, snapshot.r1);
            }
        }
        std::sort(snapshots->begin(), snapshots->end(), [](const FiberSnapshot& a, const FiberSnapshot& b) { return a.record < b.record; });
        state.load = [snapshots, frame = netplay::Frame()](std::string* note) { return LoadFibers(*snapshots, frame, note); };
        return state;
    }

    std::vector<std::pair<uint32_t, uint64_t>> FiberHostStates()
    {
        std::vector<std::pair<uint32_t, uint64_t>> states;
        std::lock_guard lock(g_lock);
        for (const auto& [record, fiber] : g_fibers)
        {
            if (fiber->context && !fiber->deletedAt)
                states.emplace_back(record, fiber->hostState);
        }
        std::sort(states.begin(), states.end());
        return states;
    }

    std::vector<std::pair<uint32_t, uint32_t>> FiberStacks()
    {
        std::vector<std::pair<uint32_t, uint32_t>> stacks;
        std::lock_guard lock(g_lock);
        for (const auto& [record, fiber] : g_fibers)
        {
            if (!fiber->context)
                continue;
            const uint32_t limit = guest::LoadU32(record + 12), stackBase = guest::LoadU32(record + 8);
            if (limit < stackBase)
                stacks.emplace_back(limit, stackBase);
        }
        return stacks;
    }
}
