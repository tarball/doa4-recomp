// Synchronization exports: events, waits, critical sections, spinlocks/IRQL, TLS and handles.
#include "guest_abi.h"
#include "kernel/objects.h"
#include "kernel/status.h"
#include "kernel/threads.h"
#include "log.h"

#include <atomic>
#include <bitset>
#include <unordered_map>
#include <vector>

#include <immintrin.h>
#include <windows.h>

namespace
{
    using namespace kernel;

    // ---------------------------------------------------------------------------------------------
    // Atomics on big-endian guest words, compatible with the guest's own lwarx/stwcx. (compare-and-swap).

    std::atomic_ref<uint32_t> AtomicWord(void* word)
    {
        guest::BeforeHostWriteTo(word, 4); // the store map's (guest_memory.h)
        return std::atomic_ref<uint32_t>(*static_cast<uint32_t*>(word));
    }

    // Adds `delta` and returns the new value.
    int32_t AtomicAdd(be<int32_t>* value, int32_t delta)
    {
        auto word = AtomicWord(value);
        uint32_t expected = word.load();
        for (;;)
        {
            const int32_t current = int32_t(__builtin_bswap32(expected));
            const uint32_t desired = __builtin_bswap32(uint32_t(current + delta));
            if (word.compare_exchange_weak(expected, desired))
                return current + delta;
        }
    }

    bool CompareExchange(void* word, uint32_t expected, uint32_t desired)
    {
        uint32_t raw = __builtin_bswap32(expected);
        return AtomicWord(word).compare_exchange_strong(raw, __builtin_bswap32(desired));
    }

    // ---------------------------------------------------------------------------------------------
    // Events

    class Event : public GuestBackedObject
    {
    public:
        Event(DispatcherType type, bool signaled) : GuestBackedObject(sizeof(DispatcherHeader))
        {
            Header()->type = uint8_t(type);
            Header()->size = sizeof(DispatcherHeader) / 4;
            Header()->signalState = signaled ? 1 : 0;
        }
        const char* TypeName() const override { return "Event"; }
    };

    uint32_t NtCreateEvent(be<uint32_t>* handle, uint32_t /*objectAttributes*/, uint32_t eventType, uint32_t initialState)
    {
        if (!handle)
            return status::kInvalidParameter;
        auto event = std::make_shared<Event>(eventType == 0 ? DispatcherType::NotificationEvent : DispatcherType::SynchronizationEvent, initialState != 0);
        RegisterGuestObject(event);
        *handle = CreateHandle(event);
        return status::kSuccess;
    }

    // Resolves a handle to its waitable guest header.
    uint32_t WaitableHeader(uint32_t handle)
    {
        const auto object = LookupHandle(handle);
        return object && object->IsWaitable() ? object->GuestObject() : 0;
    }

    template <int32_t (*Operation)(uint32_t)>
    uint32_t EventOperation(uint32_t handle, be<int32_t>* previousState)
    {
        const uint32_t header = WaitableHeader(handle);
        if (!header)
            return status::kInvalidHandle;
        const int32_t previous = Operation(header);
        if (previousState)
            *previousState = previous;
        return status::kSuccess;
    }

    uint32_t NtSetEvent(uint32_t handle, be<int32_t>* previousState) { return EventOperation<kernel::SetEvent>(handle, previousState); }
    uint32_t NtPulseEvent(uint32_t handle, be<int32_t>* previousState) { return EventOperation<kernel::PulseEvent>(handle, previousState); }
    uint32_t NtClearEvent(uint32_t handle) { return EventOperation<kernel::ResetEvent>(handle, nullptr); }

    int32_t KeSetEvent(uint32_t event, uint32_t /*increment*/, uint32_t /*wait*/)
    {
        return event ? kernel::SetEvent(event) : 0;
    }

    int32_t KeResetEvent(uint32_t event)
    {
        return event ? kernel::ResetEvent(event) : 0;
    }

    // ---------------------------------------------------------------------------------------------
    // Waits

    uint32_t KeWaitForSingleObject(uint32_t object, uint32_t /*waitReason*/, uint32_t /*processorMode*/, uint32_t /*alertable*/, const be<int64_t>* timeout)
    {
        if (!object)
            return status::kInvalidParameter;
        const uint32_t headers[] = { object };
        return WaitForHeaders(headers, false, timeout);
    }

    uint32_t NtWaitForSingleObjectEx(uint32_t handle, uint32_t /*waitMode*/, uint32_t /*alertable*/, const be<int64_t>* timeout)
    {
        const uint32_t header = WaitableHeader(handle);
        if (!header)
            return status::kInvalidHandle;
        const uint32_t headers[] = { header };
        return WaitForHeaders(headers, false, timeout);
    }

    uint32_t NtWaitForMultipleObjectsEx(uint32_t count, const be<uint32_t>* handles, uint32_t waitType, uint32_t /*waitMode*/, uint32_t /*alertable*/, const be<int64_t>* timeout)
    {
        if (!handles || count == 0 || count > 64)
            return status::kInvalidParameter;
        std::vector<uint32_t> headers(count);
        for (uint32_t i = 0; i < count; i++)
        {
            headers[i] = WaitableHeader(handles[i]);
            if (!headers[i])
                return status::kInvalidHandle;
        }
        // WaitType: 0 = WaitAll, 1 = WaitAny.
        return WaitForHeaders(headers, waitType == 0, timeout);
    }

    uint32_t NtSignalAndWaitForSingleObjectEx(uint32_t signalHandle, uint32_t waitHandle, uint32_t /*waitMode*/, uint32_t /*alertable*/, const be<int64_t>* timeout)
    {
        const uint32_t signalHeader = WaitableHeader(signalHandle);
        const uint32_t waitHeader = WaitableHeader(waitHandle);
        if (!signalHeader || !waitHeader)
            return status::kInvalidHandle;
        kernel::SetEvent(signalHeader);
        const uint32_t headers[] = { waitHeader };
        return WaitForHeaders(headers, false, timeout);
    }

    // Owner identity for locks: the caller's KTHREAD, or a unique nonzero value for host-only threads
    // (which have no KTHREAD) so recursion checks still work when the runtime itself takes a guest lock.
    uint32_t OwnerId()
    {
        if (const uint32_t kthread = CurrentKThread())
            return kthread;
        return 0x80000000u | (GetCurrentThreadId() & 0x7FFFFFFFu);
    }

    // ---------------------------------------------------------------------------------------------
    // Critical sections (X_RTL_CRITICAL_SECTION), waiting on their embedded synchronization event.

    struct CriticalSection
    {
        DispatcherHeader header;
        be<int32_t> lockCount;      // -1 when free
        be<int32_t> recursionCount;
        be<uint32_t> owningThread;  // KTHREAD of the owner
    };
    static_assert(sizeof(CriticalSection) == 0x1C);

    void RtlInitializeCriticalSection(CriticalSection* section)
    {
        if (!section)
            return;
        section->header.type = uint8_t(DispatcherType::SynchronizationEvent);
        section->header.absolute = 0; // spin count / 256
        section->header.size = sizeof(DispatcherHeader) / 4;
        section->header.inserted = 0;
        section->header.signalState = 0;
        section->header.waitListFlink = guest::ToGuest(&section->header.waitListFlink);
        section->header.waitListBlink = guest::ToGuest(&section->header.waitListFlink);
        section->lockCount = -1;
        section->recursionCount = 0;
        section->owningThread = 0;
    }

    void RtlEnterCriticalSection(CriticalSection* section)
    {
        const uint32_t self = OwnerId();
        if (section->owningThread == self)
        {
            AtomicAdd(&section->lockCount, 1);
            section->recursionCount = section->recursionCount + 1;
            return;
        }

        for (uint32_t spin = uint32_t(section->header.absolute) * 256; spin > 0; spin--)
        {
            if (CompareExchange(&section->lockCount, uint32_t(-1), 0))
            {
                section->owningThread = self;
                section->recursionCount = 1;
                return;
            }
            _mm_pause();
        }

        if (AtomicAdd(&section->lockCount, 1) != 0)
        {
            // Contended: wait for the owner's release to signal the embedded event.
            const uint32_t headers[] = { guest::ToGuest(&section->header) };
            WaitForHeaders(headers, false, nullptr);
        }
        section->owningThread = self;
        section->recursionCount = 1;
    }

    uint32_t RtlTryEnterCriticalSection(CriticalSection* section)
    {
        const uint32_t self = OwnerId();
        if (CompareExchange(&section->lockCount, uint32_t(-1), 0))
        {
            section->owningThread = self;
            section->recursionCount = 1;
            return 1;
        }
        if (section->owningThread == self)
        {
            AtomicAdd(&section->lockCount, 1);
            section->recursionCount = section->recursionCount + 1;
            return 1;
        }
        return 0;
    }

    void RtlLeaveCriticalSection(CriticalSection* section)
    {
        const int32_t recursion = section->recursionCount - 1;
        section->recursionCount = recursion;
        if (recursion != 0)
        {
            AtomicAdd(&section->lockCount, -1);
            return;
        }

        section->owningThread = 0;
        if (AtomicAdd(&section->lockCount, -1) != -1)
            kernel::SetEvent(guest::ToGuest(&section->header)); // wake one waiter (synchronization event)
    }

    // ---------------------------------------------------------------------------------------------
    // Spinlocks and IRQL. A held spinlock contains the owner's KTHREAD (never 0).

    constexpr uint8_t kDispatchLevel = 2;

    uint8_t& CurrentIrql()
    {
        static thread_local uint8_t hostIrql = 0;
        GuestThread* thread = CurrentThread();
        return thread ? thread->irql : hostIrql;
    }

    void AcquireSpinLock(be<uint32_t>* lock)
    {
        const uint32_t owner = OwnerId();
        while (!CompareExchange(lock, 0, owner))
        {
            for (int i = 0; i < 64 && uint32_t(*lock) != 0; i++)
                _mm_pause();
            if (uint32_t(*lock) != 0)
                SwitchToThread();
        }
    }

    void ReleaseSpinLock(be<uint32_t>* lock)
    {
        AtomicWord(lock).store(0);
    }

    uint32_t KfAcquireSpinLock(be<uint32_t>* lock)
    {
        uint8_t& irql = CurrentIrql();
        const uint8_t previous = irql;
        irql = kDispatchLevel;
        AcquireSpinLock(lock);
        return previous;
    }

    void KfReleaseSpinLock(be<uint32_t>* lock, uint32_t previousIrql)
    {
        ReleaseSpinLock(lock);
        CurrentIrql() = uint8_t(previousIrql);
    }

    void KeAcquireSpinLockAtRaisedIrql(be<uint32_t>* lock) { AcquireSpinLock(lock); }
    void KeReleaseSpinLockFromRaisedIrql(be<uint32_t>* lock) { ReleaseSpinLock(lock); }

    uint32_t KeTryToAcquireSpinLockAtRaisedIrql(be<uint32_t>* lock)
    {
        return CompareExchange(lock, 0, OwnerId()) ? 1 : 0;
    }

    uint32_t KeRaiseIrqlToDpcLevel()
    {
        uint8_t& irql = CurrentIrql();
        const uint8_t previous = irql;
        irql = kDispatchLevel;
        return previous;
    }

    void KfLowerIrql(uint32_t newIrql)
    {
        CurrentIrql() = uint8_t(newIrql);
    }

    // Critical regions only disable APCs, which aren't delivered here.
    void KeEnterCriticalRegion() {}
    void KeLeaveCriticalRegion() {}

    // L2 cache locking has no host equivalent.
    uint32_t KeLockL2(uint32_t /*mode*/) { return 0; }
    void KeUnlockL2(uint32_t /*mode*/) {}

    // ---------------------------------------------------------------------------------------------
    // TLS. Slot allocation is process-wide; values are per guest thread.

    std::mutex g_tlsLock;
    std::bitset<kTlsSlotCount> g_tlsSlots;

    std::array<uint32_t, kTlsSlotCount>& CurrentTls()
    {
        static thread_local std::array<uint32_t, kTlsSlotCount> hostTls{};
        GuestThread* thread = CurrentThread();
        return thread ? thread->tls : hostTls;
    }

    uint32_t KeTlsAlloc()
    {
        std::lock_guard lock(g_tlsLock);
        for (uint32_t i = 0; i < kTlsSlotCount; i++)
        {
            if (!g_tlsSlots[i])
            {
                g_tlsSlots[i] = true;
                return i;
            }
        }
        return 0xFFFFFFFF; // TLS_OUT_OF_INDEXES
    }

    uint32_t KeTlsFree(uint32_t index)
    {
        std::lock_guard lock(g_tlsLock);
        if (index >= kTlsSlotCount || !g_tlsSlots[index])
            return 0;
        g_tlsSlots[index] = false;
        return 1;
    }

    uint32_t KeTlsGetValue(uint32_t index)
    {
        return index < kTlsSlotCount ? CurrentTls()[index] : 0;
    }

    uint32_t KeTlsSetValue(uint32_t index, uint32_t value)
    {
        if (index >= kTlsSlotCount)
            return 0;
        CurrentTls()[index] = value;
        return 1;
    }

    // ---------------------------------------------------------------------------------------------
    // Handles and object references.

    uint32_t NtClose(uint32_t handle)
    {
        if (handle == kCurrentThreadHandle || handle == kCurrentProcessHandle)
            return status::kSuccess;
        return kernel::CloseHandle(handle) ? status::kSuccess : status::kInvalidHandle;
    }

    uint32_t NtDuplicateObject(uint32_t sourceHandle, be<uint32_t>* targetHandle, uint32_t options)
    {
        auto object = LookupHandle(sourceHandle);
        if (!object)
            return status::kInvalidHandle;
        if (targetHandle)
            *targetHandle = CreateHandle(object);
        constexpr uint32_t kDuplicateCloseSource = 0x1;
        if (options & kDuplicateCloseSource)
            kernel::CloseHandle(sourceHandle);
        return status::kSuccess;
    }

    // References handed out by ObReferenceObjectByHandle, released by ObDereferenceObject.
    std::mutex g_referenceLock;
    std::unordered_multimap<uint32_t, std::shared_ptr<KernelObject>> g_references;

    uint32_t ObReferenceObjectByHandle(uint32_t handle, uint32_t /*objectType*/, be<uint32_t>* object)
    {
        auto target = LookupHandle(handle);
        if (!target || !target->GuestObject())
            return status::kInvalidHandle;
        {
            std::lock_guard lock(g_referenceLock);
            g_references.emplace(target->GuestObject(), target);
        }
        if (object)
            *object = target->GuestObject();
        return status::kSuccess;
    }

    void ObDereferenceObject(uint32_t object)
    {
        std::shared_ptr<KernelObject> released;
        std::lock_guard lock(g_referenceLock);
        const auto it = g_references.find(object);
        if (it != g_references.end())
        {
            released = std::move(it->second);
            g_references.erase(it);
        }
    }
}

GUEST_EXPORT(NtCreateEvent, NtCreateEvent)
GUEST_EXPORT(NtSetEvent, NtSetEvent)
GUEST_EXPORT(NtPulseEvent, NtPulseEvent)
GUEST_EXPORT(NtClearEvent, NtClearEvent)
GUEST_EXPORT(KeSetEvent, KeSetEvent)
GUEST_EXPORT(KeResetEvent, KeResetEvent)
GUEST_EXPORT(KeWaitForSingleObject, KeWaitForSingleObject)
GUEST_EXPORT(NtWaitForSingleObjectEx, NtWaitForSingleObjectEx)
GUEST_EXPORT(NtWaitForMultipleObjectsEx, NtWaitForMultipleObjectsEx)
GUEST_EXPORT(NtSignalAndWaitForSingleObjectEx, NtSignalAndWaitForSingleObjectEx)
GUEST_EXPORT(RtlInitializeCriticalSection, RtlInitializeCriticalSection)
GUEST_EXPORT(RtlEnterCriticalSection, RtlEnterCriticalSection)
GUEST_EXPORT(RtlTryEnterCriticalSection, RtlTryEnterCriticalSection)
GUEST_EXPORT(RtlLeaveCriticalSection, RtlLeaveCriticalSection)
GUEST_EXPORT(KfAcquireSpinLock, KfAcquireSpinLock)
GUEST_EXPORT(KfReleaseSpinLock, KfReleaseSpinLock)
GUEST_EXPORT(KeAcquireSpinLockAtRaisedIrql, KeAcquireSpinLockAtRaisedIrql)
GUEST_EXPORT(KeReleaseSpinLockFromRaisedIrql, KeReleaseSpinLockFromRaisedIrql)
GUEST_EXPORT(KeTryToAcquireSpinLockAtRaisedIrql, KeTryToAcquireSpinLockAtRaisedIrql)
GUEST_EXPORT(KeRaiseIrqlToDpcLevel, KeRaiseIrqlToDpcLevel)
GUEST_EXPORT(KfLowerIrql, KfLowerIrql)
GUEST_EXPORT(KeEnterCriticalRegion, KeEnterCriticalRegion)
GUEST_EXPORT(KeLeaveCriticalRegion, KeLeaveCriticalRegion)
GUEST_EXPORT(KeLockL2, KeLockL2)
GUEST_EXPORT(KeUnlockL2, KeUnlockL2)
GUEST_EXPORT(KeTlsAlloc, KeTlsAlloc)
GUEST_EXPORT(KeTlsFree, KeTlsFree)
GUEST_EXPORT(KeTlsGetValue, KeTlsGetValue)
GUEST_EXPORT(KeTlsSetValue, KeTlsSetValue)
GUEST_EXPORT(NtClose, NtClose)
GUEST_EXPORT(NtDuplicateObject, NtDuplicateObject)
GUEST_EXPORT(ObReferenceObjectByHandle, ObReferenceObjectByHandle)
GUEST_EXPORT(ObDereferenceObject, ObDereferenceObject)
