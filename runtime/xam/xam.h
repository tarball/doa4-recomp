#pragma once

#include "guest_abi.h"

#include <cstdint>

namespace xam
{
    inline constexpr uint32_t kErrorSuccess = 0;
    inline constexpr uint32_t kErrorFunctionFailed = 1627;
    inline constexpr uint32_t kErrorInvalidParameter = 87;
    inline constexpr uint32_t kErrorIoPending = 997;
    inline constexpr uint32_t kErrorNoMoreFiles = 18;
    inline constexpr uint32_t kErrorDeviceNotConnected = 1167;
    inline constexpr uint32_t kErrorEmpty = 4306;
    inline constexpr uint32_t kErrorNoSuchUser = 1317;
    inline constexpr uint32_t kErrorNotLoggedOn = 1245;
    inline constexpr uint32_t kErrorCancelled = 1223;

    inline constexpr uint32_t kTitleId = 0x544307D1; // Dead or Alive 4
    inline constexpr uint64_t kLocalXuid = 0xB13EBABEBABEBABEull; // offline profile XUID (as Xenia uses)
    inline constexpr const char* kLocalGamertag = "Player";
    inline constexpr uint32_t kMaxUsers = 4;

    // XOVERLAPPED, used by asynchronous XAM calls.
    struct Overlapped
    {
        be<uint32_t> internalLow;     // result
        be<uint32_t> internalHigh;    // length
        be<uint32_t> internalContext;
        be<uint32_t> event;           // handle signalled on completion
        be<uint32_t> completionRoutine;
        be<uint32_t> completionContext;
        be<uint32_t> extendedError;
    };
    static_assert(sizeof(Overlapped) == 0x1C);

    // Completes an asynchronous request immediately. With an XOVERLAPPED it stores the results,
    // signals its event and returns ERROR_IO_PENDING; without one it returns `result`.
    uint32_t Complete(Overlapped* overlapped, uint32_t result, uint32_t length = 0, uint32_t extendedError = 0);

    bool IsUserSignedIn(uint32_t userIndex);
}
