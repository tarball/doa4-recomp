// Runtime unit tests: page heap bookkeeping and the memory exports called the way recompiled code calls them.
#include "audio/xma.h"
#include "config.h"
#include "disc_image.h"
#include "gpu/shader/hlsl.h"
#include "gpu/shader/shader.h"
#include "guest_abi.h"
#include "guest_memory.h"
#include "imports.h"
#include "kernel/module.h"
#include "kernel/objects.h"
#include "kernel/status.h"
#include "kernel/threads.h"
#include "netplay/frame.h"
#include "netplay/replay.h"
#include "page_heap.h"
#include "settings.h"
#include "util/image.h"
#include "xam/achievements.h"
#include "xam/spa.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#include <windows.h>
#include <d3dcompiler.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                    \
    do                                                      \
    {                                                       \
        g_checks++;                                         \
        if (!(cond))                                        \
        {                                                   \
            g_failures++;                                   \
            printf("FAIL %s:%d: %s", __func__, __LINE__, #cond); \
            printf(" | " __VA_ARGS__);                      \
            printf("\n");                                   \
        }                                                   \
    } while (0)

// ---------------------------------------------------------------------------------------------
// PageHeap

static void test_heap_alloc_alignment_and_order()
{
    PageHeap heap(0x40000000, 0x01000000, 0x10000); // 16 MB of 64 KB pages
    const uint32_t a = heap.Alloc(0x10000, 0, true, 4, false);
    const uint32_t b = heap.Alloc(0x18000, 0, true, 4, false); // rounds up to 2 pages
    CHECK(a == 0x40000000, "a=%08X", a);
    CHECK(b == 0x40010000, "b=%08X", b);
    const uint32_t c = heap.Alloc(0x10000, 0x100000, true, 4, false); // 1 MB alignment
    CHECK(c == 0x40100000, "c=%08X", c);
    const uint32_t top = heap.Alloc(0x20000, 0, true, 4, true);
    CHECK(top == 0x40FE0000, "top=%08X", top);
    const uint32_t top2 = heap.Alloc(0x10000, 0x100000, true, 4, true);
    CHECK(top2 == 0x40F00000, "top2=%08X", top2);
}

static void test_heap_range_and_exhaustion()
{
    PageHeap heap(0x70000000, 0x00100000, 0x10000); // 16 pages
    const uint32_t a = heap.AllocRange(0x70080000, 0x700A0000, 0x20000, 0, true, 4, true);
    CHECK(a == 0x70080000, "a=%08X", a);
    CHECK(heap.AllocRange(0x70080000, 0x700A0000, 0x10000, 0, true, 4, false) == 0, "range is full");
    CHECK(heap.Alloc(0x00100000, 0, true, 4, false) == 0, "whole heap no longer fits");
    CHECK(heap.Alloc(0x80000, 0, true, 4, false) == 0x70000000, "bottom half fits");
    CHECK(heap.Alloc(0x60000, 0, true, 4, true) == 0x700A0000, "top 6 pages fit");
    CHECK(heap.Alloc(0x10000, 0, true, 4, false) == 0, "heap exhausted");
}

static void test_heap_fixed_commit_release()
{
    PageHeap heap(0x00000000, 0x00100000, 0x1000);
    CHECK(!heap.AllocFixed(0x10000, 0x3000, false, true, 4), "commit without reserve on free pages fails");
    CHECK(heap.AllocFixed(0x10000, 0x3000, true, false, 4), "reserve");
    CHECK(heap.QueryProtect(0x10000) == 0, "reserved pages have no protection");
    CHECK(!heap.AllocFixed(0x10000, 0x1000, true, false, 4), "can't reserve twice");
    CHECK(heap.AllocFixed(0x11000, 0x1000, false, true, 2), "commit inside reservation");
    CHECK(heap.QueryProtect(0x11000) == 2, "committed page protect");
    CHECK(!heap.AllocFixed(0x12000, 0x2000, false, true, 4), "commit spilling past reservation fails");

    PageHeap::RegionInfo info{};
    CHECK(heap.QueryRegion(0x10000, info), "query");
    CHECK(info.baseAddress == 0x10000 && info.regionSize == 0x1000 && info.state == PageHeap::State::Reserved, "first page: %08X %X", info.baseAddress, info.regionSize);
    CHECK(heap.QueryRegion(0x11800, info) && info.baseAddress == 0x11000 && info.state == PageHeap::State::Committed && info.allocationBase == 0x10000,
        "committed page region base=%08X alloc=%08X", info.baseAddress, info.allocationBase);
    CHECK(heap.QueryRegion(0x13000, info) && info.state == PageHeap::State::Free && info.allocationBase == 0 && info.regionSize == 0xED000,
        "free run size=%X", info.regionSize);

    CHECK(!heap.Release(0x11000), "release needs the region base");
    uint32_t released = 0;
    CHECK(heap.Release(0x10000, &released) && released == 0x3000, "released=%X", released);
    CHECK(heap.QueryRegion(0x10000, info) && info.state == PageHeap::State::Free, "free after release");
}

static void test_heap_decommit_and_protect()
{
    PageHeap heap(0x00000000, 0x00010000, 0x1000);
    const uint32_t a = heap.Alloc(0x4000, 0, true, 4, false);
    CHECK(heap.Decommit(a + 0x1000, 0x1000), "decommit one page");
    CHECK(heap.QueryProtect(a + 0x1000) == 0 && heap.QueryProtect(a) == 4, "only that page decommitted");
    CHECK(!heap.Protect(a, 0x4000, 2), "can't protect a reserved page");
    CHECK(heap.Protect(a + 0x2000, 0x2000, 2) && heap.QueryProtect(a + 0x3000) == 2, "protect committed pages");
}

static void test_heap_top_of_address_space()
{
    // Heaps near the top of the address space; make sure 64-bit math handles them.
    PageHeap heap(0xF0000000, 0x0FD00000, 0x1000);
    const uint32_t a = heap.Alloc(0x1000, 0, true, 4, true);
    CHECK(a == 0xFFCFF000, "a=%08X", a);
    PageHeap full(0xFFFF0000, 0x10000, 0x1000);
    CHECK(full.Alloc(0x10000, 0, true, 4, false) == 0xFFFF0000, "heap ending exactly at 4 GB");
}

// ---------------------------------------------------------------------------------------------
// Exports, called through the generated-code ABI.

struct Call
{
    std::unique_ptr<PPCContext> ctx = [] {
        auto context = std::make_unique<PPCContext>();
        context->fpscr.loadFromHost();
        return context;
    }();

    template <typename... Args>
    uint32_t operator()(PPCFunc* function, Args... args)
    {
        const uint64_t values[] = { uint64_t(args)... };
        PPCRegister* regs[] = { &ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10 };
        for (size_t i = 0; i < sizeof...(Args); i++)
            regs[i]->u64 = values[i];
        function(*ctx, guest::g_base);
        return ctx->r3.u32;
    }
};

// Scratch guest memory for in/out arguments.
static uint32_t Scratch()
{
    static uint32_t scratch = guest::SystemAlloc(0x1000);
    return scratch;
}

static void test_nt_allocate_and_free()
{
    Call call;
    const uint32_t basePtr = Scratch(), sizePtr = Scratch() + 4;

    guest::StoreU32(basePtr, 0);
    guest::StoreU32(sizePtr, 0x1801);
    uint32_t result = call(__imp__NtAllocateVirtualMemory, basePtr, sizePtr, 0x3000 /* reserve|commit */, 4, 0);
    const uint32_t address = guest::LoadU32(basePtr);
    CHECK(result == status::kSuccess, "status=%08X", result);
    CHECK(address >= 0x10000 && address < 0x40000000 && address % 0x1000 == 0, "4 KB heap address %08X", address);
    // From the top of the heap: DOA4 relocates files loaded there, which only works above their offsets.
    CHECK(address >= 0x30000000, "4 KB pages are allocated top-down: %08X", address);
    CHECK(guest::LoadU32(sizePtr) == 0x2000, "size rounded to pages: %X", guest::LoadU32(sizePtr));
    CHECK(guest::ToHost(address)[0x1FFF] == 0, "zeroed");

    // Query
    const uint32_t infoPtr = Scratch() + 0x100;
    result = call(__imp__NtQueryVirtualMemory, address + 0x1000, infoPtr, 0);
    CHECK(result == status::kSuccess, "query status=%08X", result);
    CHECK(guest::LoadU32(infoPtr + 0) == address + 0x1000, "BaseAddress %08X", guest::LoadU32(infoPtr));
    CHECK(guest::LoadU32(infoPtr + 4) == address, "AllocationBase");
    CHECK(guest::LoadU32(infoPtr + 12) == 0x1000, "RegionSize %X", guest::LoadU32(infoPtr + 12));
    CHECK(guest::LoadU32(infoPtr + 16) == 0x1000, "State MEM_COMMIT");
    CHECK(guest::LoadU32(infoPtr + 20) == 4, "Protect");

    // Release
    guest::StoreU32(sizePtr, 0);
    result = call(__imp__NtFreeVirtualMemory, basePtr, sizePtr, 0x8000 /* release */, 0);
    CHECK(result == status::kSuccess && guest::LoadU32(sizePtr) == 0x2000, "release status=%08X size=%X", result, guest::LoadU32(sizePtr));
    CHECK(call(__imp__NtFreeVirtualMemory, basePtr, sizePtr, 0x8000, 0) == status::kMemoryNotAllocated, "double free fails");
}

static void test_nt_allocate_large_pages_and_fixed()
{
    Call call;
    const uint32_t basePtr = Scratch(), sizePtr = Scratch() + 4;

    guest::StoreU32(basePtr, 0);
    guest::StoreU32(sizePtr, 0x10000);
    uint32_t result = call(__imp__NtAllocateVirtualMemory, basePtr, sizePtr, 0x20002000 /* large pages, reserve */, 4, 0);
    const uint32_t address = guest::LoadU32(basePtr);
    CHECK(result == status::kSuccess && address >= 0x40000000 && address < 0x7F000000 && address % 0x10000 == 0, "large-page reserve at %08X", address);
    CHECK(call(__imp__MmQueryAddressProtect, address) == 0, "reserved, not committed");

    // Commit the reservation in place.
    guest::StoreU32(basePtr, address + 0x10);
    guest::StoreU32(sizePtr, 0x100);
    result = call(__imp__NtAllocateVirtualMemory, basePtr, sizePtr, 0x20001000 /* large pages, commit */, 4, 0);
    CHECK(result == status::kSuccess && guest::LoadU32(basePtr) == address && guest::LoadU32(sizePtr) == 0x10000,
        "commit in place: %08X %X", guest::LoadU32(basePtr), guest::LoadU32(sizePtr));
    CHECK(call(__imp__MmQueryAddressProtect, address + 0x8000) == 4, "now committed");

    // Committing outside any reservation fails.
    guest::StoreU32(basePtr, 0x3F000000);
    guest::StoreU32(sizePtr, 0x1000);
    CHECK(call(__imp__NtAllocateVirtualMemory, basePtr, sizePtr, 0x1000, 4, 0) == status::kNoMemory, "commit of free pages without reserve");

    CHECK(call(__imp__NtAllocateVirtualMemory, basePtr, sizePtr, 0x4000 /* decommit isn't an allocation type */, 4, 0) == status::kInvalidParameter, "bad type");

    // A 4 KB request at an address in the 64 KB heap is refused.
    guest::StoreU32(basePtr, 0x50000000);
    CHECK(call(__imp__NtAllocateVirtualMemory, basePtr, sizePtr, 0x3000, 4, 0) == status::kAccessDenied, "page size mismatch");
}

static void test_physical_memory()
{
    Call call;
    // 64 KB pages (A0000000 view), 1 MB, 64 KB aligned.
    const uint32_t a = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x100000, 0x20000004, 0, 0xFFFFFFFF, 0);
    CHECK(a >= 0xA0000000 && a < 0xC0000000 && a % 0x10000 == 0, "A0 view: %08X", a);
    CHECK(call(__imp__MmGetPhysicalAddress, a) == a - 0xA0000000, "physical of A0 address");

    // 4 KB pages come from the A0000000 view too (on hardware, the E0000000 view) and never overlap
    // its other physical ranges.
    const uint32_t e = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x2000, 4, 0, 0xFFFFFFFF, 0);
    const uint32_t ePhys = call(__imp__MmGetPhysicalAddress, e);
    CHECK(e >= 0xA0000000 && e < 0xC0000000 && ePhys == e - 0xA0000000, "4 KB pages: %08X phys %08X", e, ePhys);
    const uint32_t aPhys = a - 0xA0000000;
    CHECK(ePhys + 0x2000 <= aPhys || ePhys >= aPhys + 0x100000, "physical ranges overlap: A %08X 4 KB %08X", aPhys, ePhys);

    // They're 4 KB granular: sixteen fit in a 64 KB physical window.
    uint32_t pages[16];
    bool allFit = true;
    for (uint32_t& page : pages)
    {
        page = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x1000, 4, 0x03000000, 0x0300FFFF, 0);
        allFit = allFit && page != 0;
    }
    CHECK(allFit, "sixteen 4 KB pages in 64 KB");
    for (const uint32_t page : pages)
        call(__imp__MmFreePhysicalMemory, 0, page);

    // Every view of a physical page is the same memory: D3D reads and patches vertex shaders through
    // C0000000 addresses of 4 KB-page allocations.
    guest::StoreU32(guest::kSharedViewC + ePhys + 0x10, 0x12345678);
    CHECK(guest::LoadU32(e + 0x10) == 0x12345678, "C0 alias of a 4 KB page: %08X", guest::LoadU32(e + 0x10));
    MEMORY_BASIC_INFORMATION memory{};
    CHECK(VirtualQuery(guest::ToHost(guest::kViewE), &memory, sizeof(memory)) && memory.State == MEM_RESERVE, "E0000000 isn't mapped: %lX",
        memory.State);

    // Constrained physical range.
    const uint32_t low = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x1000, 4, 0x01000000, 0x01FFFFFF, 0x10000);
    const uint32_t lowPhys = call(__imp__MmGetPhysicalAddress, low);
    CHECK(low != 0 && lowPhys >= 0x01000000 && lowPhys + 0x1000 <= 0x02000000 && lowPhys % 0x10000 == 0, "constrained: phys %08X", lowPhys);

    // 16 MB pages.
    const uint32_t c = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x100, 0x80000004, 0, 0xFFFFFFFF, 0);
    CHECK(c >= 0xC0000000 && c < 0xE0000000 && c % 0x1000000 == 0, "C0 view: %08X", c);

    CHECK(call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x1000, 0x01 /* no access */, 0, 0xFFFFFFFF, 0) == 0, "needs a readable protection");

    call(__imp__MmFreePhysicalMemory, 0, a);
    const uint32_t again = call(__imp__MmAllocatePhysicalMemoryEx, 0, 0x100000, 0x20000004, 0, 0xFFFFFFFF, 0);
    CHECK(again == a, "freed physical range is reused (top-down): %08X vs %08X", again, a);

    // The GPU sees physical memory through the A0000000 view.
    CHECK(guest::PhysicalToGuest(ePhys) == e, "4 KB page from the GPU: %08X", guest::PhysicalToGuest(ePhys));
    CHECK(guest::PhysicalToGuest(aPhys) == a, "A0 page from the GPU: %08X", guest::PhysicalToGuest(aPhys));

    // NtFreeVirtualMemory can't release physical memory (DOA4 tries, then keeps using it).
    const uint32_t basePtr = Scratch(), sizePtr = Scratch() + 4;
    guest::StoreU32(basePtr, e);
    guest::StoreU32(sizePtr, 0);
    CHECK(call(__imp__NtFreeVirtualMemory, basePtr, sizePtr, 0x8000, 0) == status::kMemoryNotAllocated, "NtFreeVirtualMemory of physical memory");
    CHECK(call(__imp__MmQueryAddressProtect, e) == 4, "still allocated after NtFreeVirtualMemory");
    call(__imp__MmFreePhysicalMemory, 0, e);
    CHECK(call(__imp__MmQueryAddressProtect, e) == 0, "freed by MmFreePhysicalMemory");
}

static void test_xam_alloc()
{
    Call call;
    const uint32_t out = Scratch() + 8;
    CHECK(call(__imp__XamAlloc, 0, 0x30, out) == xerror::kSuccess && guest::LoadU32(out) != 0, "XamAlloc");
    const uint32_t p = guest::LoadU32(out);
    CHECK(call(__imp__XamFree, p) == xerror::kSuccess, "XamFree");
    CHECK(call(__imp__XamAlloc, 0x00100000, 0x30, out) == xerror::kSuccess && guest::LoadU32(out) >= 0xA0000000 && guest::LoadU32(out) < 0xC0000000,
        "physical XamAlloc: %08X", guest::LoadU32(out));
}

// ---------------------------------------------------------------------------------------------
// Kernel basics

static uint32_t StoreAnsi(uint32_t address, const char* text)
{
    memcpy(guest::ToHost(address), text, strlen(text) + 1);
    return address;
}

static void test_strings()
{
    Call call;
    const uint32_t text = StoreAnsi(Scratch() + 0x200, "hello");
    const uint32_t ansi = Scratch() + 0x300;
    call(__imp__RtlInitAnsiString, ansi, text);
    CHECK(guest::LoadU32(ansi) == 0x00050006 && guest::LoadU32(ansi + 4) == text, "ANSI_STRING %08X %08X", guest::LoadU32(ansi), guest::LoadU32(ansi + 4));

    // Unicode "Hi!" (big-endian UTF-16) to ANSI with allocation.
    const uint32_t wide = Scratch() + 0x400;
    const uint16_t chars[] = { 'H', 'i', '!', 0 };
    for (int i = 0; i < 4; i++)
        *guest::Translate<be<uint16_t>>(wide + i * 2) = chars[i];
    const uint32_t unicode = Scratch() + 0x500;
    call(__imp__RtlInitUnicodeString, unicode, wide);
    CHECK(guest::LoadU32(unicode) == 0x00060008, "UNICODE_STRING lengths %08X", guest::LoadU32(unicode));
    CHECK(call(__imp__RtlUnicodeStringToAnsiString, ansi, unicode, 1) == status::kSuccess, "convert");
    const char* converted = guest::Translate<char>(guest::LoadU32(ansi + 4));
    CHECK(converted && strcmp(converted, "Hi!") == 0 && guest::LoadU32(ansi) >> 16 == 3, "converted '%s'", converted ? converted : "(null)");
    call(__imp__RtlFreeAnsiString, ansi);
    CHECK(guest::LoadU32(ansi + 4) == 0, "freed buffer cleared");
}

static void test_time()
{
    Call call;
    const uint32_t time = Scratch() + 0x600, fields = Scratch() + 0x610, back = Scratch() + 0x620;
    *guest::Translate<be<uint64_t>>(time) = 0x01C60C7E21ECFDB0ull; // 2005-12-29 13:45:30.123 (Thursday)
    call(__imp__RtlTimeToTimeFields, time, fields);
    const auto* f = guest::Translate<be<uint16_t>>(fields);
    CHECK(f[0] == 2005 && f[1] == 12 && f[2] == 29 && f[3] == 13 && f[4] == 45 && f[5] == 30 && f[6] == 123 && f[7] == 4,
        "%u-%u-%u %u:%u:%u.%u wd%u", uint16_t(f[0]), uint16_t(f[1]), uint16_t(f[2]), uint16_t(f[3]), uint16_t(f[4]), uint16_t(f[5]), uint16_t(f[6]), uint16_t(f[7]));
    CHECK(call(__imp__RtlTimeFieldsToTime, fields, back) == 1 && *guest::Translate<be<uint64_t>>(back) == 0x01C60C7E21ECFDB0ull, "round trip");

    // KeQueryPerformanceFrequency returns a 64-bit value in r3.
    call(__imp__KeQueryPerformanceFrequency);
    CHECK(call.ctx->r3.u64 == 50000000, "frequency %llu", (unsigned long long)call.ctx->r3.u64);

    const uint64_t start = DOA4QueryTimeBase();
    Sleep(100);
    const uint64_t ticks = DOA4QueryTimeBase() - start;
    CHECK(ticks > 4500000 && ticks < 6500000, "100 ms at 50 MHz = %llu ticks", (unsigned long long)ticks);
}

static void test_memory_ulong()
{
    Call call;
    const uint32_t buffer = Scratch() + 0x700;
    call(__imp__RtlFillMemoryUlong, buffer, 16, 0xDEADBEEF);
    CHECK(guest::ToHost(buffer)[0] == 0xDE && guest::LoadU32(buffer + 12) == 0xDEADBEEF, "fill is big-endian");
    guest::StoreU32(buffer + 8, 0);
    CHECK(call(__imp__RtlCompareMemoryUlong, buffer, 16, 0xDEADBEEF) == 8, "matches until the first difference");
}

static void test_module()
{
    Call call;
    imports::ResolveSlots();
    if (!kernel::InitializeExecutableModule("private/default.xex", uint32_t(PPC_IMAGE_BASE), uint32_t(PPC_IMAGE_SIZE)))
    {
        printf("skipping module tests: private/default.xex not found (run from the repo root)\n");
        return;
    }

    const uint32_t handleVariable = imports::VariableAddress("XexExecutableModuleHandle");
    const uint32_t entry = guest::LoadU32(handleVariable);
    const uint32_t header = guest::LoadU32(entry + 0x58);
    CHECK(header == kernel::ExecutableXexHeader() && guest::LoadU32(header) == 0x58455832, "handle -> entry -> XEX2 header");
    CHECK(guest::LoadU32(entry + 0x3C) == 0x827929B0, "entry point in loader entry");

    CHECK(call(__imp__RtlImageXexHeaderField, header, 0x00010100) == 0x827929B0, "inline value (entry point)");
    const uint32_t execution = call(__imp__RtlImageXexHeaderField, header, 0x00040006);
    CHECK(execution && guest::LoadU32(execution + 12) == 0x544307D1, "execution info -> title id %08X", execution ? guest::LoadU32(execution + 12) : 0);
    CHECK(call(__imp__RtlImageXexHeaderField, header, 0x00020401) == 0, "absent header (default heap size)");

    CHECK(call(__imp__XexCheckExecutablePrivilege, 9) == 1, "system flags 0x200 -> privilege 9");
    CHECK(call(__imp__XexCheckExecutablePrivilege, 6) == 0, "privilege 6 not set");
}

// ---------------------------------------------------------------------------------------------
// Synchronization and threads. Host functions registered in the lookup table act as guest code.

static constexpr uint32_t kFakeCode = uint32_t(PPC_CODE_BASE); // inside PPC_CODE range, so the lookup table covers it

static uint32_t RegisterGuestFunction(uint32_t index, PPCFunc* function)
{
    const uint32_t address = kFakeCode + index * 4;
    PPC_LOOKUP_FUNC(guest::g_base, address) = function;
    return address;
}

static constexpr int64_t kNoWait = 0;

static uint32_t TimeoutPtr(int64_t value)
{
    const uint32_t address = Scratch() + 0x800;
    *guest::Translate<be<int64_t>>(address) = value;
    return address;
}

static void test_events()
{
    Call call;
    const uint32_t out = Scratch() + 0x810;
    CHECK(call(__imp__NtCreateEvent, out, 0, 1 /* synchronization */, 0) == status::kSuccess, "create sync event");
    const uint32_t sync = guest::LoadU32(out);
    CHECK(call(__imp__NtCreateEvent, out, 0, 0 /* notification */, 1) == status::kSuccess, "create notification event");
    const uint32_t manual = guest::LoadU32(out);

    CHECK(call(__imp__NtWaitForSingleObjectEx, sync, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "unsignaled poll times out");
    call(__imp__NtSetEvent, sync, 0);
    CHECK(call(__imp__NtWaitForSingleObjectEx, sync, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess, "signaled");
    CHECK(call(__imp__NtWaitForSingleObjectEx, sync, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "auto-reset after one wait");

    CHECK(call(__imp__NtWaitForSingleObjectEx, manual, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess &&
          call(__imp__NtWaitForSingleObjectEx, manual, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess, "manual reset stays signaled");

    // Wait any returns the index of the first signaled object; wait all needs every one.
    const uint32_t handles = Scratch() + 0x820;
    guest::StoreU32(handles, sync);
    guest::StoreU32(handles + 4, manual);
    CHECK(call(__imp__NtWaitForMultipleObjectsEx, 2, handles, 1, 0, 0, TimeoutPtr(kNoWait)) == status::kSuccess + 1, "wait any -> index 1");
    CHECK(call(__imp__NtWaitForMultipleObjectsEx, 2, handles, 0, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "wait all needs both");

    const auto start = GetTickCount64();
    CHECK(call(__imp__NtWaitForSingleObjectEx, sync, 0, 0, TimeoutPtr(-500000 /* 50 ms */)) == status::kTimeout, "relative timeout");
    const auto elapsed = GetTickCount64() - start;
    CHECK(elapsed >= 40 && elapsed < 500, "waited %llu ms", (unsigned long long)elapsed);

    CHECK(call(__imp__NtClose, sync) == status::kSuccess && call(__imp__NtClose, sync) == status::kInvalidHandle, "close once");
    call(__imp__NtClose, manual);
}

// Guest thread bodies.
static std::atomic<int> g_threadRuns{ 0 };

static void ThreadReturnsContextPlusOne(PPCContext& ctx, uint8_t*)
{
    g_threadRuns++;
    // r13 points at this thread's PCR, whose current-thread field is its KTHREAD.
    const uint32_t kthread = guest::LoadU32(ctx.r13.u32 + 0x100);
    guest::StoreU32(Scratch() + 0x900, kthread == kernel::CurrentKThread() ? 1 : 0);
    guest::StoreU32(Scratch() + 0x904, guest::LoadU32(kthread + 0x14C)); // thread id
    ctx.r3.u64 = ctx.r3.u64 + 1;
}

static void XapiStartupCallsStart(PPCContext& ctx, uint8_t* base)
{
    // xapiStartup(start, context): call start(context), then exit with its result.
    const uint32_t start = ctx.r3.u32;
    ctx.r3.u64 = ctx.r4.u64;
    (PPC_LOOKUP_FUNC(base, start))(ctx, base);
}

static void ThreadTerminatesEarly(PPCContext& ctx, uint8_t* base)
{
    ctx.r3.u64 = 0x1234;
    __imp__ExTerminateThread(ctx, base);
    ctx.r3.u64 = 0xBAD; // unreachable
}

static uint32_t g_section = 0, g_counter = 0;

static void ThreadIncrementsUnderLock(PPCContext& ctx, uint8_t* base)
{
    for (int i = 0; i < 50000; i++)
    {
        ctx.r3.u64 = g_section;
        __imp__RtlEnterCriticalSection(ctx, base);
        guest::StoreU32(g_counter, guest::LoadU32(g_counter) + 1); // non-atomic on purpose
        ctx.r3.u64 = g_section;
        __imp__RtlLeaveCriticalSection(ctx, base);
    }
    ctx.r3.u64 = 0;
}

static void ThreadUsesTls(PPCContext& ctx, uint8_t* base)
{
    const uint32_t slot = ctx.r3.u32;
    ctx.r3.u64 = slot;
    ctx.r4.u64 = 0x7777;
    __imp__KeTlsSetValue(ctx, base);
    ctx.r3.u64 = slot;
    __imp__KeTlsGetValue(ctx, base);
}

static uint32_t WaitForThread(Call& call, uint32_t handle)
{
    return call(__imp__NtWaitForSingleObjectEx, handle, 0, 0, TimeoutPtr(-50000000 /* 5 s */));
}

static uint32_t ThreadExitCode(uint32_t handle)
{
    auto thread = kernel::LookupHandleAs<kernel::GuestThread>(handle);
    return thread ? thread->ExitCode() : 0xFFFFFFFF;
}

static void test_threads()
{
    Call call;
    const uint32_t handlePtr = Scratch() + 0x830, idPtr = Scratch() + 0x834;
    const uint32_t body = RegisterGuestFunction(0, ThreadReturnsContextPlusOne);
    const uint32_t startup = RegisterGuestFunction(1, XapiStartupCallsStart);

    // Plain thread: start(context).
    CHECK(call(__imp__ExCreateThread, handlePtr, 0, idPtr, 0, body, 41, 0) == status::kSuccess, "create");
    uint32_t handle = guest::LoadU32(handlePtr);
    CHECK(WaitForThread(call, handle) == status::kSuccess, "thread handle signaled on exit");
    CHECK(ThreadExitCode(handle) == 42, "exit code %u", ThreadExitCode(handle));
    CHECK(guest::LoadU32(Scratch() + 0x900) == 1, "PCR current thread is the thread's KTHREAD");
    CHECK(guest::LoadU32(Scratch() + 0x904) == guest::LoadU32(idPtr), "KTHREAD thread id matches ExCreateThread's");
    call(__imp__NtClose, handle);

    // Through the XAPI startup routine, with an affinity byte in the flags.
    CHECK(call(__imp__ExCreateThread, handlePtr, 0x10000, idPtr, startup, body, 99, 0x04000000) == status::kSuccess, "create via startup");
    handle = guest::LoadU32(handlePtr);
    CHECK(WaitForThread(call, handle) == status::kSuccess && ThreadExitCode(handle) == 100, "startup path exit %u", ThreadExitCode(handle));
    call(__imp__NtClose, handle);

    // Created suspended: doesn't run until resumed.
    const int runsBefore = g_threadRuns;
    CHECK(call(__imp__ExCreateThread, handlePtr, 0, idPtr, 0, body, 1, kernel::kCreateSuspended) == status::kSuccess, "create suspended");
    handle = guest::LoadU32(handlePtr);
    Sleep(50);
    CHECK(g_threadRuns == runsBefore, "suspended thread didn't run");
    CHECK(call(__imp__NtWaitForSingleObjectEx, handle, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "not finished");
    const uint32_t countPtr = Scratch() + 0x838;
    CHECK(call(__imp__NtResumeThread, handle, countPtr) == status::kSuccess && guest::LoadU32(countPtr) == 1, "resume (previous count 1)");
    CHECK(WaitForThread(call, handle) == status::kSuccess && g_threadRuns == runsBefore + 1, "ran after resume");
    call(__imp__NtClose, handle);

    // ExTerminateThread unwinds out of the thread with its exit code.
    const uint32_t terminator = RegisterGuestFunction(2, ThreadTerminatesEarly);
    call(__imp__ExCreateThread, handlePtr, 0, idPtr, 0, terminator, 0, 0);
    handle = guest::LoadU32(handlePtr);
    CHECK(WaitForThread(call, handle) == status::kSuccess && ThreadExitCode(handle) == 0x1234, "ExTerminateThread exit 0x%X", ThreadExitCode(handle));
    call(__imp__NtClose, handle);

    // TLS values are per thread.
    const uint32_t slot = call(__imp__KeTlsAlloc);
    CHECK(slot < kernel::kTlsSlotCount, "TLS slot %u", slot);
    call(__imp__KeTlsSetValue, slot, 0x1111);
    const uint32_t tlsBody = RegisterGuestFunction(3, ThreadUsesTls);
    call(__imp__ExCreateThread, handlePtr, 0, idPtr, 0, tlsBody, slot, 0);
    handle = guest::LoadU32(handlePtr);
    CHECK(WaitForThread(call, handle) == status::kSuccess && ThreadExitCode(handle) == 0x7777, "thread sees its own TLS value");
    CHECK(call(__imp__KeTlsGetValue, slot) == 0x1111, "caller's TLS value unchanged");
    call(__imp__KeTlsFree, slot);
    call(__imp__NtClose, handle);
}

static void test_critical_section_contention()
{
    Call call;
    g_section = guest::KernelAlloc(0x1C);
    g_counter = guest::KernelAlloc(4);
    call(__imp__RtlInitializeCriticalSection, g_section);
    CHECK(int32_t(guest::LoadU32(g_section + 0x10)) == -1, "lock count starts at -1");

    const uint32_t body = RegisterGuestFunction(4, ThreadIncrementsUnderLock);
    uint32_t handles[4];
    for (uint32_t& handle : handles)
    {
        call(__imp__ExCreateThread, Scratch() + 0x840, 0, 0, 0, body, 0, 0);
        handle = guest::LoadU32(Scratch() + 0x840);
    }
    for (uint32_t handle : handles)
    {
        CHECK(WaitForThread(call, handle) == status::kSuccess, "worker finished");
        call(__imp__NtClose, handle);
    }
    CHECK(guest::LoadU32(g_counter) == 4 * 50000, "counter %u (lost updates mean the lock failed)", guest::LoadU32(g_counter));
    CHECK(int32_t(guest::LoadU32(g_section + 0x10)) == -1 && guest::LoadU32(g_section + 0x18) == 0, "released: lock count -1, no owner");

    // Recursion on one thread.
    call(__imp__RtlEnterCriticalSection, g_section);
    call(__imp__RtlEnterCriticalSection, g_section);
    CHECK(int32_t(guest::LoadU32(g_section + 0x14)) == 2, "recursion count 2");
    call(__imp__RtlLeaveCriticalSection, g_section);
    call(__imp__RtlLeaveCriticalSection, g_section);
    CHECK(int32_t(guest::LoadU32(g_section + 0x10)) == -1, "fully released");
    CHECK(call(__imp__RtlTryEnterCriticalSection, g_section) == 1, "try-enter when free");
    call(__imp__RtlLeaveCriticalSection, g_section);
}

static uint32_t g_pulseEvent = 0;
static std::atomic<int> g_pulseWoken{ 0 };

static void ThreadWaitsForPulse(PPCContext& ctx, uint8_t* base)
{
    ctx.r3.u64 = g_pulseEvent;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 0;
    ctx.r6.u64 = TimeoutPtr(-20000000); // 2 s
    __imp__NtWaitForSingleObjectEx(ctx, base);
    if (ctx.r3.u32 == status::kSuccess)
        g_pulseWoken++;
}

// The runtime implements it, but games that don't import it (DOAX2) don't have it in the generated declarations.
PPC_EXTERN_FUNC(__imp__KeTryToAcquireSpinLockAtRaisedIrql);

static void test_pulse_and_spinlock()
{
    Call call;
    call(__imp__NtCreateEvent, Scratch() + 0x850, 0, 0 /* notification */, 0);
    g_pulseEvent = guest::LoadU32(Scratch() + 0x850);
    const uint32_t body = RegisterGuestFunction(5, ThreadWaitsForPulse);
    uint32_t handles[2];
    for (uint32_t& handle : handles)
    {
        call(__imp__ExCreateThread, Scratch() + 0x854, 0, 0, 0, body, 0, 0);
        handle = guest::LoadU32(Scratch() + 0x854);
    }
    Sleep(100); // let both start waiting
    call(__imp__NtPulseEvent, g_pulseEvent, 0);
    for (uint32_t handle : handles)
    {
        WaitForThread(call, handle);
        call(__imp__NtClose, handle);
    }
    CHECK(g_pulseWoken == 2, "pulse woke %d of 2 waiters", g_pulseWoken.load());
    CHECK(call(__imp__NtWaitForSingleObjectEx, g_pulseEvent, 0, 0, TimeoutPtr(kNoWait)) == status::kTimeout, "event non-signaled after pulse");
    call(__imp__NtClose, g_pulseEvent);

    const uint32_t lock = guest::KernelAlloc(4);
    const uint32_t irql = call(__imp__KfAcquireSpinLock, lock);
    CHECK(guest::LoadU32(lock) != 0, "spinlock held");
    CHECK(call(__imp__KeTryToAcquireSpinLockAtRaisedIrql, lock) == 0, "try-acquire fails while held");
    call(__imp__KfReleaseSpinLock, lock, irql);
    CHECK(guest::LoadU32(lock) == 0 && call(__imp__KeTryToAcquireSpinLockAtRaisedIrql, lock) == 1, "released and re-acquirable");
}

// ---------------------------------------------------------------------------------------------
// Shaders

// The shader binaries precompilation finds in the executable: with and without constant tables, all parsed and
// translated, with nothing the translation can't express but the format 0 fetches of D3D's vertex shader templates
// (bound copies get their formats). Pixel shaders' centroid variants declare their centroid interpolators so, and the
// smallest compiles, as do the smallest vertex and pixel shaders with products translated with Xenos products and
// DEBUG_NAN's counts. Uses the game's executable, when private/default.pe is there.
static void test_shader_binaries()
{
    std::ifstream file("private/default.pe", std::ios::binary);
    if (!file)
    {
        printf("  skipped: no private/default.pe\n");
        return;
    }
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    uint32_t vertex = 0, pixel = 0, translated = 0, centroids = 0;
    std::string smallestCentroid, smallestProducts[2];
    for (const gpu::shader::ShaderBinary& binary : gpu::shader::FindShaderBinaries(image.data(), image.size()))
    {
        const bool isVertex = binary.stage == gpu::ShaderStage::kVertex;
        (isVertex ? vertex : pixel)++;
        const gpu::shader::ParsedShader parsed = gpu::shader::Parse(binary.stage, binary.ucode.data(), uint32_t(binary.ucode.size()));
        CHECK(parsed.valid, "%s at 0x%zX: %s", isVertex ? "vs" : "ps", binary.offset, parsed.error.c_str());
        if (!parsed.valid)
            continue;
        const gpu::shader::HlslResult result = gpu::shader::TranslateToHlsl(parsed);
        translated += !result.source.empty();
        for (const std::string& what : result.unsupported)
            CHECK(isVertex && what == "unsupported vertex format 0", "%s at 0x%zX: %s", isVertex ? "vs" : "ps", binary.offset, what.c_str());
        if (!isVertex && parsed.tempCount >= 8)
        {
            gpu::shader::TranslationOptions options;
            options.centroidInterpolators = 0xC0;
            const std::string source = gpu::shader::TranslateToHlsl(parsed, options).source;
            const bool declared = source.find("centroid float4 interpolator6 : TEXCOORD6;") != std::string::npos &&
                source.find("centroid float4 interpolator7 : TEXCOORD7;") != std::string::npos &&
                source.find("    float4 interpolator5 : TEXCOORD5;") != std::string::npos;
            CHECK(declared, "ps at 0x%zX: centroid interpolators 6 and 7 not declared so", binary.offset);
            centroids += declared;
            if (smallestCentroid.empty() || source.size() < smallestCentroid.size())
                smallestCentroid = source;
        }
        gpu::shader::TranslationOptions products;
        products.xenosProducts = true;
        products.debugNan = 2;
        const std::string source = gpu::shader::TranslateToHlsl(parsed, products).source;
        std::string& smallest = smallestProducts[isVertex ? 0 : 1];
        if (source.find("XeMul(") != std::string::npos && source.find("XeDot(") != std::string::npos &&
            (smallest.empty() || source.size() < smallest.size()))
            smallest = source;
    }
    // DOA4's: 341 vertex and 185 pixel shaders, 241 of them without a constant table.
    CHECK(vertex == 341 && pixel == 185 && translated == vertex + pixel, "%u vertex, %u pixel, %u translated", vertex, pixel, translated);
    CHECK(centroids > 0, "no pixel shader with 8 interpolators");
    CHECK(!smallestProducts[0].empty() && !smallestProducts[1].empty(), "no vertex or pixel shader with products");
    auto compile = [](const std::string& source, const char* target, const char* what) {
        ID3DBlob* bytecode = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT compiled = D3DCompile(source.data(), source.size(), what, nullptr, nullptr, "main", target,
            D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0, &bytecode, &errors);
        CHECK(SUCCEEDED(compiled), "the smallest %s doesn't compile: %s", what, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        if (bytecode)
            bytecode->Release();
        if (errors)
            errors->Release();
    };
    compile(smallestCentroid, "ps_5_1", "centroid variant");
    compile(smallestProducts[0], "vs_5_1", "vertex shader with Xenos products and DEBUG_NAN");
    compile(smallestProducts[1], "ps_5_1", "pixel shader with Xenos products and DEBUG_NAN");
    printf("  %u pixel shaders' centroid variants declared\n", centroids);
}

// ---------------------------------------------------------------------------------------------
// Audio

// XMA streams in private/dumps (DUMP_XMA=1 records them), decoded the way contexts decode them (frames read
// and repacked) and from their packets as they are, give the same samples: every frame's 512 per channel, less
// what FFmpeg's decoder trims at the start (64) and at the end (as the last frame says, under 2048).
static void test_xma_decoding()
{
#if HAVE_FFMPEG
    struct Dump
    {
        std::filesystem::path path;
        uint32_t packets, channels, rate;
    };
    std::vector<Dump> dumps;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("private/dumps", error))
    {
        Dump dump{ entry.path() };
        uint32_t physical = 0;
        if (sscanf(entry.path().filename().string().c_str(), "xma_%8X_%u_%uch_%u.bin", &physical, &dump.packets, &dump.channels, &dump.rate) == 4)
            dumps.push_back(dump);
    }
    if (dumps.empty())
    {
        printf("  skipped: no XMA streams in private/dumps\n");
        return;
    }
    uint32_t frames = 0;
    for (const Dump& dump : dumps)
    {
        std::ifstream file(dump.path, std::ios::binary);
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const std::string name = dump.path.filename().string();
        if (data.size() != size_t(dump.packets) * 2048 || (dump.channels != 1 && dump.channels != 2))
        {
            CHECK(false, "%s: %zu bytes", name.c_str(), data.size());
            continue;
        }
        const uint32_t buffer = guest::AllocPhysical(uint32_t(data.size()), 0x1000, guest::kPageReadWrite, 0, 0x1FFFFFFF, 0x1000);
        memcpy(guest::ToHost(buffer), data.data(), data.size());
        const uint32_t physical = guest::PhysicalAddress(buffer);
        const audio::XmaTestDecode direct = audio::DecodeXmaForTest(physical, dump.packets, dump.channels, dump.rate, true);
        const audio::XmaTestDecode repacked = audio::DecodeXmaForTest(physical, dump.packets, dump.channels, dump.rate, false);
        guest::FreePhysical(buffer);
        CHECK(direct.ok && repacked.ok, "%s: FFmpeg can't decode it (direct %d, repacked %d)", name.c_str(), direct.ok, repacked.ok);
        size_t difference = 0;
        while (difference < direct.samples.size() && difference < repacked.samples.size() && direct.samples[difference] == repacked.samples[difference])
            difference++;
        CHECK(repacked.samples == direct.samples, "%s: %zu samples repacked, %zu direct, first difference at %zu", name.c_str(), repacked.samples.size(),
            direct.samples.size(), difference);
        const size_t samples = repacked.samples.size() / dump.channels, whole = size_t(repacked.frames) * 512;
        CHECK(repacked.frames && samples + 64 <= whole && samples + 64 + 2048 > whole, "%s: %u frames, %zu samples per channel", name.c_str(),
            repacked.frames, samples);
        frames += repacked.frames;
    }
    printf("  %zu streams, %u frames\n", dumps.size(), frames);
#else
    printf("  skipped: built without FFmpeg\n");
#endif
}

// ---------------------------------------------------------------------------------------------
// Settings file

static void test_settings_file()
{
    const std::string previous = config::DataDirectory();
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "doa4_settings_test";
    std::filesystem::create_directories(directory);
    config::SetDataDirectory(directory.string());
    const std::filesystem::path file = directory / "settings.ini";
    for (const char* variable : { GAME_PREFIX "_FULLSCREEN", GAME_PREFIX "_RESOLUTION_SCALE", GAME_PREFIX "_MSAA", GAME_PREFIX "_ALPHA_TO_COVERAGE", GAME_PREFIX "_ANISOTROPY" })
        _putenv_s(variable, "");

    // Values out of range are brought into it; unknown lines are ignored.
    std::ofstream(file) << "; comment\n[game]\ndisc_image = D:\\Games\\Dead or Alive 4 (Rev 1).iso  \r\n[graphics]\nfullscreen = 1\n"
                           "resolution_scale = 9\nmsaa = 6\nalpha_to_coverage = 1\nanisotropy = 12\nunknown = 3\n";
    settings::Load();
    CHECK(settings::DiscImage() == "D:\\Games\\Dead or Alive 4 (Rev 1).iso", "disc image \"%s\"", settings::DiscImage().c_str());
    settings::Graphics graphics = settings::GetGraphics();
    CHECK(graphics.fullscreen && graphics.resolutionScale == 4 && graphics.msaaSamples == 4 && graphics.alphaToCoverage &&
            graphics.anisotropy == 8,
        "scale %u, MSAA %u, anisotropy %u", graphics.resolutionScale, graphics.msaaSamples, graphics.anisotropy);

    // The environment overrides the file, and isn't saved unless the settings change.
    _putenv_s(GAME_PREFIX "_ANISOTROPY", "2");
    settings::Load();
    _putenv_s(GAME_PREFIX "_ANISOTROPY", "");
    CHECK(settings::GetGraphics().anisotropy == 2, "anisotropy %u", settings::GetGraphics().anisotropy);
    std::filesystem::remove(file);
    settings::Save();
    CHECK(!std::filesystem::exists(file), "saved without a change");

    // A change counts as a new version, and is what Save writes.
    const uint32_t version = settings::GraphicsVersion();
    graphics = settings::GetGraphics();
    graphics.fullscreen = false;
    graphics.resolutionScale = 2;
    graphics.alphaToCoverage = false;
    settings::SetGraphics(graphics);
    CHECK(settings::GraphicsVersion() == version + 1, "version %u after %u", settings::GraphicsVersion(), version);
    settings::SetGraphics(graphics);
    CHECK(settings::GraphicsVersion() == version + 1, "setting the same values isn't a change");
    settings::Save();
    settings::Load();
    graphics = settings::GetGraphics();
    CHECK(!graphics.fullscreen && graphics.resolutionScale == 2 && graphics.msaaSamples == 4 && !graphics.alphaToCoverage &&
            graphics.anisotropy == 2,
        "read back: scale %u, MSAA %u, anisotropy %u", graphics.resolutionScale, graphics.msaaSamples, graphics.anisotropy);
    CHECK(settings::DiscImage() == "D:\\Games\\Dead or Alive 4 (Rev 1).iso", "disc image kept: \"%s\"", settings::DiscImage().c_str());

    std::filesystem::remove_all(directory);
    config::SetDataDirectory(previous);
    settings::SetGraphics(settings::Graphics{});
}

// ---------------------------------------------------------------------------------------------
// The game's executable from its disc image

static void test_disc_image()
{
    // Something that isn't a disc image.
    const std::filesystem::path fake = std::filesystem::temp_directory_path() / "doa4_not_a_disc.iso";
    {
        std::ofstream out(fake, std::ios::binary);
        const std::vector<char> zeros(256 * 1024, 0);
        out.write(zeros.data(), std::streamsize(zeros.size()));
    }
    std::vector<uint8_t> xex;
    CHECK(disc::ReadExecutable(fake.string(), xex) == disc::Check::kUnreadable, "a file of zeros");
    std::filesystem::remove(fake);

    // The real executable (development checkouts have it in private/): checked, and unpacked into default.pe.
    std::ifstream file("private/default.xex", std::ios::binary | std::ios::ate);
    if (!file)
    {
        printf("  skipped the rest: no private/default.xex\n");
        return;
    }
    xex.resize(size_t(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(xex.data()), std::streamsize(xex.size()));
    CHECK(disc::IsSupported(xex), "private/default.xex isn't the supported version's");
    std::vector<uint8_t> image;
    CHECK(disc::Unpack(xex, image), "unpacking");
    CHECK(image.size() == PPC_IMAGE_SIZE, "image size 0x%zX", image.size());
    const std::string imageHash = disc::Sha256(image.data(), image.size());
    CHECK(imageHash == "e618f3b02857f4da630407a3c11103bb044432216372a36a3fad6d6f893184ec", "image SHA-256 %s", imageHash.c_str());
    std::vector<uint8_t> changed = xex;
    changed[changed.size() / 2] ^= 1;
    CHECK(!disc::IsSupported(changed), "a changed byte");

    // The disc image itself, if it's in the working directory.
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(".", error))
    {
        if (entry.path().extension() != ".iso")
            continue;
        std::vector<uint8_t> fromDisc;
        CHECK(disc::ReadExecutable(entry.path().string(), fromDisc) == disc::Check::kOk, "%s", entry.path().string().c_str());
        CHECK(fromDisc == xex, "default.xex from the disc image differs from private/default.xex");
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// The title's SPA data (its achievements, strings and images) from the executable's resources

static bool ReadFile(const char* path, std::vector<uint8_t>& data)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    data.resize(size_t(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    return bool(file);
}

// A string's FNV-1a hash: the tests check the game's own text by it rather than spell it out.
static uint32_t Fnv1a(const std::string& text)
{
    uint32_t hash = 2166136261u;
    for (unsigned char c : text)
        hash = (hash ^ c) * 16777619u;
    return hash;
}

static void test_spa()
{
    std::vector<uint8_t> xex, image;
    if (!ReadFile("private/default.xex", xex) || !ReadFile("private/default.pe", image))
    {
        printf("  skipped: no private/default.xex and default.pe\n");
        return;
    }
    CHECK(spa::Load(xex, image), "the SPA data");
    CHECK(spa::TitleName(1) == "DEAD OR ALIVE 4", "title \"%s\"", spa::TitleName(1).c_str());
    CHECK(spa::DefaultLanguage() == 1, "default language %u", spa::DefaultLanguage());
    uint32_t gamerscore = 0;
    for (const spa::Achievement& achievement : spa::Achievements())
        gamerscore += achievement.gamerscore;
    CHECK(spa::Achievements().size() == 45 && gamerscore == 1000, "%zu achievements, %u gamerscore", spa::Achievements().size(), gamerscore);
    const spa::Achievement* survival = spa::FindAchievement(15);
    // Its English name, a medal in Survival (by its hash).
    CHECK(survival && Fnv1a(spa::String(survival->nameId, 1)) == 0xB4F175BD && survival->gamerscore == 10 && survival->imageId == 15,
        "achievement 15");
    CHECK(survival && spa::String(survival->nameId, 2) != spa::String(survival->nameId, 1) && !spa::String(survival->nameId, 2).empty(),
        "achievement 15's name in Japanese");
    CHECK(!spa::FindAchievement(24), "no achievement 24");
    // The title's image, the window's icon: a 64x64 PNG.
    const std::vector<uint8_t> title = spa::TitleImage();
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;
    CHECK(util::DecodeImage(title.data(), title.size(), width, height, false, bgra) && width == 64 && height == 64, "title image %ux%u",
        width, height);
    width = height = 20;
    CHECK(util::DecodeImage(title.data(), title.size(), width, height, true, bgra) && bgra.size() == 20 * 20 * 4, "scaled to 20x20");
    const std::vector<uint8_t> picture = survival ? spa::Image(survival->imageId) : std::vector<uint8_t>();
    width = height = 0;
    CHECK(util::DecodeImage(picture.data(), picture.size(), width, height, true, bgra) && width == 64 && height == 64,
        "achievement 15's picture %ux%u", width, height);
}

// The game writing achievements (XUserWriteAchievements through XMsg), recorded in the profile's file.
static void test_achievements()
{
    if (!spa::Loaded())
    {
        printf("  skipped: no SPA data\n");
        return;
    }
    Call call;
    const std::string previous = config::DataDirectory();
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "doa4_achievements_test";
    std::filesystem::remove_all(directory);
    config::SetDataDirectory(directory.string());
    achievements::Reset();
    // As the game writes them: XMsgStartIORequest(0xFB, 0xB0008, overlapped, { count, list }, 8), 8 bytes an achievement
    // (the game user, its ID). User 1 has no profile.
    const uint32_t request = Scratch() + 0x200, list = Scratch() + 0x210, overlapped = Scratch() + 0x240;
    const uint32_t written[3][2] = { { 0, 15 }, { 1, 16 }, { 0, 19 } };
    guest::StoreU32(request, 3);
    guest::StoreU32(request + 4, list);
    for (uint32_t i = 0; i < 3; i++)
    {
        guest::StoreU32(list + i * 8, written[i][0]);
        guest::StoreU32(list + i * 8 + 4, written[i][1]);
    }
    memset(guest::ToHost(overlapped), 0, 0x1C);
    CHECK(call(__imp__XMsgStartIORequest, 0xFB, 0xB0008, overlapped, request, 8) == 997 && guest::LoadU32(overlapped) == 0, "result %08X",
        guest::LoadU32(overlapped));
    CHECK(achievements::Earned(15) && achievements::Earned(19) && !achievements::Earned(16), "earned 15 and 19, not 16");
    CHECK(!achievements::Earn(0, 15), "15 again isn't new");
    achievements::Reset();
    CHECK(achievements::Earned(15) && achievements::Earned(19) && !achievements::Earned(16), "read back from the file");
    std::ifstream file(achievements::FilePath());
    std::string line, lines;
    int count = 0;
    while (std::getline(file, line))
    {
        if (!line.empty() && line[0] != ';')
            count++, lines += line + "|";
    }
    // Each line has the achievement's gamerscore and English name, as the SPA data has them.
    const spa::Achievement* fifteen = spa::FindAchievement(15);
    CHECK(count == 2 && lines.find("15 ") == 0 && fifteen && lines.find("10 " + spa::String(fifteen->nameId, 1) + "|") != std::string::npos &&
            lines.find("|19 ") != std::string::npos,
        "the file's lines: %s", lines.c_str());
    // Bounded: a count the game never writes is refused.
    guest::StoreU32(request, 100000);
    CHECK(call(__imp__XMsgStartIORequest, 0xFB, 0xB0008, overlapped, request, 8) == 997 && guest::LoadU32(overlapped) == 0x80070057,
        "too many: %08X", guest::LoadU32(overlapped));
    file.close();
    achievements::Reset();
    std::filesystem::remove_all(directory);
    config::SetDataDirectory(previous);
}

// ---------------------------------------------------------------------------------------------
// netplay::journal: a replay with other calls than the first run's (a rollback's), answered where they're the same

static int g_journalPlays = 0, g_journalQueries = 0, g_journalReleases = 0;
static uint32_t g_journalNextId = 100;

static void JournalPlay(PPCContext& ctx, uint8_t*)
{
    g_journalPlays++;
    ctx.r3.u64 = g_journalNextId++;
}

static void JournalQuery(PPCContext& ctx, uint8_t*)
{
    g_journalQueries++;
    guest::StoreU32(ctx.r5.u32, 0x12345678); // an out argument on the caller's stack
    ctx.r3.u64 = 7;
}

static void JournalRelease(PPCContext&, uint8_t*)
{
    g_journalReleases++;
}

static void test_journal_divergence()
{
    namespace journal = netplay::journal;
    // A stack, and a processor control region giving its bounds, for the spans the journal watches.
    const uint32_t stack = guest::SystemAlloc(0x10000), pcr = guest::SystemAlloc(0x1000);
    guest::StoreU32(pcr + 0x70, stack + 0x10000);
    guest::StoreU32(pcr + 0x74, stack);
    PPCContext ctx{};
    ctx.r1.u64 = stack + 0x8000;
    ctx.r13.u64 = pcr;
    const uint32_t block = ctx.r1.u32 + 96, out = ctx.r1.u32 + 64;
    constexpr uint32_t kPlay = 0x1000, kQuery = 0x2000, kRelease = 0x3000, kObject = 0x5000;
    journal::FunctionInfo play, query, release;
    play.address = kPlay;
    play.arguments = journal::kArgumentR3;
    play.blockRegister = 4;
    play.blockFields = { { 0, 4 } }; // the sound's number
    play.nearFrames = 2;
    query.address = kQuery;
    query.arguments = journal::kArgumentR3 | 0x1; // r3, r4
    release.address = kRelease;
    journal::Describe({ play, query, release });
    const auto Play = [&](uint32_t sound) {
        guest::StoreU32(block, sound);
        ctx.r3.u64 = kObject;
        ctx.r4.u64 = block;
        ctx.r5.u64 = 0xDEAD; // a leftover the function doesn't read
        ctx.lr = 0x82000100;
        journal::Call(kPlay, JournalPlay, ctx, guest::g_base);
        return ctx.r3.u32;
    };
    const auto Query = [&](uint32_t which) {
        guest::StoreU32(out, 0);
        ctx.r3.u64 = kObject;
        ctx.r4.u64 = which;
        ctx.r5.u64 = out;
        ctx.lr = 0x82000200;
        journal::Call(kQuery, JournalQuery, ctx, guest::g_base);
        return ctx.r3.u32;
    };
    const auto Release = [&](uint32_t object) {
        ctx.r3.u64 = object;
        ctx.lr = 0x82000300;
        journal::Defer(kRelease, JournalRelease, ctx, guest::g_base);
    };

    // The first run: frame 10 plays sound 5 and queries, frame 11 plays sound 6 and releases an object.
    netplay::SetFrame(10);
    journal::Record();
    CHECK(Play(5) == 100 && g_journalPlays == 1, "first run's sound 5: %u plays", g_journalPlays);
    CHECK(Query(1) == 7 && g_journalQueries == 1 && guest::LoadU32(out) == 0x12345678, "first run's query");
    netplay::SetFrame(11);
    CHECK(Play(6) == 101, "first run's sound 6");
    Release(0x7000);
    CHECK(g_journalReleases == 0, "a release is deferred");

    // Rolled back to frame 10 and replayed with other input: the same query (answered, its out argument written),
    // another sound (made), no sound 5; at frame 11 sound 6 again (answered) and no release.
    netplay::SetFrame(10);
    journal::ReplayFrom(10);
    CHECK(Query(1) == 7 && g_journalQueries == 1 && guest::LoadU32(out) == 0x12345678, "the same query answered from the record");
    CHECK(!journal::MadeInReplay(kQuery), "an answered call isn't one the replay made");
    CHECK(Query(2) == 7 && g_journalQueries == 2, "a query with another argument made");
    CHECK(journal::MadeInReplay(kQuery), "the replay made the query");
    CHECK(Play(9) == 102 && g_journalPlays == 3, "a sound the record doesn't hold made");
    CHECK(journal::MadeInReplay(kPlay), "the replay made the sound");
    netplay::SetFrame(11);
    CHECK(Play(6) == 101 && g_journalPlays == 3, "the same sound at the same frame answered");
    CHECK(!journal::MadeInReplay(kPlay), "the latest sound answered");
    netplay::SetFrame(12);
    journal::FinishReplay();
    CHECK(!journal::MadeInReplay(kQuery), "no replay under way");
    journal::Statistics statistics = journal::GetStatistics();
    CHECK(statistics.replayed == 2 && statistics.mismatched == 2, "answered %llu, made %llu", (unsigned long long)statistics.replayed,
        (unsigned long long)statistics.mismatched);
    // Sound 5 (frame 10) may still match a sound up to 2 frames away, until frame 13; the release is dropped at once.
    CHECK(statistics.unreplayed == 1 && statistics.releasesDropped == 1, "abandoned %llu, releases dropped %llu",
        (unsigned long long)statistics.unreplayed, (unsigned long long)statistics.releasesDropped);
    journal::OnFrame(13, 0);
    statistics = journal::GetStatistics();
    CHECK(statistics.unreplayed == 2, "sound 5 abandoned once passed: %llu", (unsigned long long)statistics.unreplayed);

    // A sound replayed a frame later than recorded is the same sound.
    netplay::SetFrame(20);
    CHECK(Play(8) == 103, "first run's sound 8");
    netplay::SetFrame(20);
    journal::ReplayFrom(20);
    netplay::SetFrame(21);
    CHECK(Play(8) == 103 && g_journalPlays == 4, "sound 8 a frame later answered");
    netplay::SetFrame(22);
    journal::FinishReplay();
    statistics = journal::GetStatistics();
    CHECK(statistics.replayedNear == 1, "answered from another frame's record: %llu", (unsigned long long)statistics.replayedNear);

    // A replay of the same frames, as a sync test's: everything answered, nothing made or abandoned.
    netplay::SetFrame(30);
    Play(3);
    Query(1);
    netplay::SetFrame(30);
    journal::ReplayFrom(30);
    const journal::Statistics before = journal::GetStatistics();
    Play(3);
    Query(1);
    netplay::SetFrame(31);
    journal::FinishReplay();
    statistics = journal::GetStatistics();
    CHECK(statistics.replayed == before.replayed + 2 && statistics.mismatched == before.mismatched && statistics.unreplayed == before.unreplayed,
        "the same calls all answered");
    journal::Stop();
    CHECK(g_journalReleases == 0, "the dropped release never made");
    journal::Describe({});
}

// The store map's miss handler (ppc_overrides.h), which the generated code calls with every register live
// (preserve_all): it must give them all back whole, the YMM registers' upper halves too, where the generated code keeps
// VMX's floating point in double precision. Clang ended it with a vzeroupper, which cleared them after restoring them:
// whenever the page log watched pages (savestates, rollback), the cloth and hair simulations' vectors lost two of their
// lanes, and Kasumi's sash and hair stretched across the screen (ANALYSIS.md, Phase 6).
static void test_store_miss_preserves_registers()
{
    // The 16 YMM registers, 4 words each, then rax, rcx, rdx, rsi, rdi, r8, r9 and r10 (preserve_all keeps every
    // general purpose register but r11; rcx and rdx are set to the arguments after their own check).
    constexpr size_t kWords = 16 * 4 + 8;
    uint64_t in[kWords], out[kWords] = {};
    for (size_t i = 0; i < kWords; i++)
        in[i] = 0x0123456789ABCDEFull * uint64_t(i + 1) ^ (uint64_t(i) << 56);
    const uint32_t address = guest::SystemAlloc(0x1000);
    for (int armed = 0; armed < 2; armed++)
    {
        // A miss on a page the page log armed (it isn't logging here, so the miss just quiets the page), and one on a quiet page.
        guest::StoreMap()[address >> guest::kStorePageShift] = uint8_t(armed);
        in[64 + 1] = address; // rcx, rdx: the arguments, as the generated code passes them
        in[64 + 2] = 4;
        __asm__ volatile(
            "vmovdqu 0x000(%[in]), %%ymm0\n\t"
            "vmovdqu 0x020(%[in]), %%ymm1\n\t"
            "vmovdqu 0x040(%[in]), %%ymm2\n\t"
            "vmovdqu 0x060(%[in]), %%ymm3\n\t"
            "vmovdqu 0x080(%[in]), %%ymm4\n\t"
            "vmovdqu 0x0A0(%[in]), %%ymm5\n\t"
            "vmovdqu 0x0C0(%[in]), %%ymm6\n\t"
            "vmovdqu 0x0E0(%[in]), %%ymm7\n\t"
            "vmovdqu 0x100(%[in]), %%ymm8\n\t"
            "vmovdqu 0x120(%[in]), %%ymm9\n\t"
            "vmovdqu 0x140(%[in]), %%ymm10\n\t"
            "vmovdqu 0x160(%[in]), %%ymm11\n\t"
            "vmovdqu 0x180(%[in]), %%ymm12\n\t"
            "vmovdqu 0x1A0(%[in]), %%ymm13\n\t"
            "vmovdqu 0x1C0(%[in]), %%ymm14\n\t"
            "vmovdqu 0x1E0(%[in]), %%ymm15\n\t"
            "movq 0x200(%[in]), %%rax\n\t"
            "movq 0x208(%[in]), %%rcx\n\t"
            "movq 0x210(%[in]), %%rdx\n\t"
            "movq 0x218(%[in]), %%rsi\n\t"
            "movq 0x220(%[in]), %%rdi\n\t"
            "movq 0x228(%[in]), %%r8\n\t"
            "movq 0x230(%[in]), %%r9\n\t"
            "movq 0x238(%[in]), %%r10\n\t"
            // A call as the generated code makes it: the stack aligned, with the shadow space Windows' convention has.
            "movq %%rsp, %%rbx\n\t"
            "andq $-16, %%rsp\n\t"
            "subq $32, %%rsp\n\t"
            "callq PPCStoreMiss\n\t"
            "movq %%rbx, %%rsp\n\t"
            "vmovdqu %%ymm0, 0x000(%[out])\n\t"
            "vmovdqu %%ymm1, 0x020(%[out])\n\t"
            "vmovdqu %%ymm2, 0x040(%[out])\n\t"
            "vmovdqu %%ymm3, 0x060(%[out])\n\t"
            "vmovdqu %%ymm4, 0x080(%[out])\n\t"
            "vmovdqu %%ymm5, 0x0A0(%[out])\n\t"
            "vmovdqu %%ymm6, 0x0C0(%[out])\n\t"
            "vmovdqu %%ymm7, 0x0E0(%[out])\n\t"
            "vmovdqu %%ymm8, 0x100(%[out])\n\t"
            "vmovdqu %%ymm9, 0x120(%[out])\n\t"
            "vmovdqu %%ymm10, 0x140(%[out])\n\t"
            "vmovdqu %%ymm11, 0x160(%[out])\n\t"
            "vmovdqu %%ymm12, 0x180(%[out])\n\t"
            "vmovdqu %%ymm13, 0x1A0(%[out])\n\t"
            "vmovdqu %%ymm14, 0x1C0(%[out])\n\t"
            "vmovdqu %%ymm15, 0x1E0(%[out])\n\t"
            "movq %%rax, 0x200(%[out])\n\t"
            "movq %%rcx, 0x208(%[out])\n\t"
            "movq %%rdx, 0x210(%[out])\n\t"
            "movq %%rsi, 0x218(%[out])\n\t"
            "movq %%rdi, 0x220(%[out])\n\t"
            "movq %%r8, 0x228(%[out])\n\t"
            "movq %%r9, 0x230(%[out])\n\t"
            "movq %%r10, 0x238(%[out])\n\t"
            "vzeroupper\n\t"
            :
            : [in] "r"(in), [out] "r"(out)
            : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
            "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
        static const char* const kGprs[] = { "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10" };
        for (size_t i = 0; i < kWords; i++)
        {
            if (i < 64)
                CHECK(out[i] == in[i], "%s miss: ymm%zu's word %zu %016llX, was %016llX", armed ? "an armed page's" : "a quiet page's", i / 4, i % 4,
                    (unsigned long long)out[i], (unsigned long long)in[i]);
            else
                CHECK(out[i] == in[i], "%s miss: %s %016llX, was %016llX", armed ? "an armed page's" : "a quiet page's", kGprs[i - 64],
                    (unsigned long long)out[i], (unsigned long long)in[i]);
        }
        CHECK(guest::StoreMap()[address >> guest::kStorePageShift] == 0, "the page quiet after its miss");
    }
}

#define RUN(test) (printf("- %s\n", #test), test())

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    RUN(test_heap_alloc_alignment_and_order);
    RUN(test_heap_range_and_exhaustion);
    RUN(test_heap_fixed_commit_release);
    RUN(test_heap_decommit_and_protect);
    RUN(test_heap_top_of_address_space);
    RUN(test_settings_file);
    RUN(test_disc_image);
    RUN(test_spa);

    if (!guest::Initialize())
    {
        printf("can't reserve guest memory\n");
        return 1;
    }
    RUN(test_nt_allocate_and_free);
    RUN(test_nt_allocate_large_pages_and_fixed);
    RUN(test_physical_memory);
    RUN(test_xam_alloc);
    RUN(test_strings);
    RUN(test_time);
    RUN(test_memory_ulong);
    RUN(test_module);
    RUN(test_achievements);
    RUN(test_events);
    RUN(test_threads);
    RUN(test_critical_section_contention);
    RUN(test_pulse_and_spinlock);
    RUN(test_shader_binaries);
    RUN(test_xma_decoding);
    RUN(test_journal_divergence);
    RUN(test_store_miss_preserves_registers);

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures != 0;
}
