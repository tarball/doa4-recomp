// Small kernel exports: process type, debugging, bug checks, status conversion, memory fill/compare.
#include "guest_abi.h"
#include "kernel/status.h"
#include "kernel/video_mode.h"
#include "log.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    constexpr uint32_t kProcessTypeTitle = 1;

    uint32_t KeGetCurrentProcessType()
    {
        return kProcessTypeTitle;
    }

    // Only the format string is printed; formatting guest varargs comes with the sprintf family.
    uint32_t DbgPrint(const char* format)
    {
        if (format)
        {
            std::string text(format);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            LOG_INFO("DbgPrint: %s", text.c_str());
        }
        return 0;
    }

    void DbgBreakPoint()
    {
        LOG_WARN("DbgBreakPoint");
    }

    [[noreturn]] void KeBugCheckEx(uint32_t code, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4)
    {
        LOG_ERROR("KeBugCheckEx(0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X)", code, p1, p2, p3, p4);
        std::abort();
    }

    [[noreturn]] void KeBugCheck(uint32_t code)
    {
        KeBugCheckEx(code, 0, 0, 0, 0);
    }

    [[noreturn]] void HalReturnToFirmware(uint32_t routine)
    {
        LOG_INFO("HalReturnToFirmware(%u): title exited", routine);
        std::exit(0);
    }

    uint32_t RtlNtStatusToDosError(uint32_t ntStatus)
    {
        switch (ntStatus)
        {
        case status::kSuccess: return 0;
        case status::kTimeout: return 1460;           // ERROR_TIMEOUT
        case status::kPending: return 997;            // ERROR_IO_PENDING
        case status::kBufferOverflow: return 234;     // ERROR_MORE_DATA
        case status::kUnsuccessful: return 31;        // ERROR_GEN_FAILURE
        case status::kNotImplemented: return 1;       // ERROR_INVALID_FUNCTION
        case status::kInvalidHandle: return 6;        // ERROR_INVALID_HANDLE
        case status::kInvalidParameter: return 87;    // ERROR_INVALID_PARAMETER
        case status::kNoSuchFile: return 2;           // ERROR_FILE_NOT_FOUND
        case status::kEndOfFile: return 38;           // ERROR_HANDLE_EOF
        case status::kNoMemory: return 8;             // ERROR_NOT_ENOUGH_MEMORY
        case status::kAccessDenied: return 5;         // ERROR_ACCESS_DENIED
        case status::kObjectNameNotFound: return 2;   // ERROR_FILE_NOT_FOUND
        case 0xC0000035: return 183;                  // STATUS_OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS
        case status::kObjectPathNotFound: return 3;   // ERROR_PATH_NOT_FOUND
        case status::kMemoryNotAllocated: return 487; // ERROR_INVALID_ADDRESS
        default:
            // HRESULT_FROM_WIN32 values carry the Win32 code directly.
            if ((ntStatus & 0xFFFF0000) == 0x80070000)
                return ntStatus & 0xFFFF;
            LOG_WARN("RtlNtStatusToDosError: unmapped status 0x%08X", ntStatus);
            return 317; // ERROR_MR_MID_NOT_FOUND, what NT returns for unknown codes
        }
    }

    void RtlFillMemoryUlong(be<uint32_t>* destination, uint32_t length, uint32_t pattern)
    {
        for (uint32_t i = 0; i < length / 4; i++)
            destination[i] = pattern;
    }

    uint32_t RtlCompareMemoryUlong(const be<uint32_t>* source, uint32_t length, uint32_t pattern)
    {
        uint32_t matched = 0;
        for (uint32_t i = 0; i < length / 4 && source[i] == pattern; i++)
            matched += 4;
        return matched;
    }

    // XAM system queries.

    void XGetVideoMode(kernel::VideoMode* mode)
    {
        if (mode)
            kernel::FillVideoMode(mode);
    }

    uint32_t XGetLanguage()
    {
        return 1; // English
    }

    uint32_t XGetGameRegion()
    {
        return 0x00FF; // North America, all
    }

    uint32_t XamGetSystemVersion()
    {
        return 0x20449700; // 2.0.17559.0, the final dashboard
    }
}

GUEST_EXPORT(KeGetCurrentProcessType, KeGetCurrentProcessType)
GUEST_EXPORT(DbgPrint, DbgPrint)
GUEST_EXPORT(DbgBreakPoint, DbgBreakPoint)
GUEST_EXPORT(KeBugCheckEx, KeBugCheckEx)
GUEST_EXPORT(KeBugCheck, KeBugCheck)
GUEST_EXPORT(HalReturnToFirmware, HalReturnToFirmware)
GUEST_EXPORT(RtlNtStatusToDosError, RtlNtStatusToDosError)
GUEST_EXPORT(RtlFillMemoryUlong, RtlFillMemoryUlong)
GUEST_EXPORT(RtlCompareMemoryUlong, RtlCompareMemoryUlong)
GUEST_EXPORT(XGetVideoMode, XGetVideoMode)
GUEST_EXPORT(XGetLanguage, XGetLanguage)
GUEST_EXPORT(XGetGameRegion, XGetGameRegion)
GUEST_EXPORT(XamGetSystemVersion, XamGetSystemVersion)
