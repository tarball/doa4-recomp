// User/profile exports: one local (offline) profile signed in on controller slot 0.
#include "guest_abi.h"
#include "log.h"
#include "xam/xam.h"

#include <cstring>

namespace xam
{
    bool IsUserSignedIn(uint32_t userIndex)
    {
        return userIndex == 0;
    }
}

namespace
{
    constexpr uint32_t kSignedInLocally = 1;

    uint32_t XamUserGetSigninState(uint32_t userIndex)
    {
        return xam::IsUserSignedIn(userIndex) ? kSignedInLocally : 0;
    }

    uint32_t XamUserGetXUID(uint32_t userIndex, uint32_t /*typeMask*/, be<uint64_t>* xuid)
    {
        if (!xam::IsUserSignedIn(userIndex))
            return xam::kErrorNoSuchUser;
        if (xuid)
            *xuid = xam::kLocalXuid;
        return xam::kErrorSuccess;
    }

    uint32_t XamUserGetName(uint32_t userIndex, char* buffer, uint32_t bufferSize)
    {
        if (!xam::IsUserSignedIn(userIndex))
            return xam::kErrorNoSuchUser;
        if (buffer && bufferSize)
        {
            strncpy(buffer, xam::kLocalGamertag, bufferSize - 1);
            buffer[bufferSize - 1] = 0;
        }
        return xam::kErrorSuccess;
    }

    // Privileges (multiplayer, content, communications...) are granted, as Xenia does.
    uint32_t XamUserCheckPrivilege(uint32_t userIndex, uint32_t /*privilege*/, be<uint32_t>* result)
    {
        if (!xam::IsUserSignedIn(userIndex))
            return xam::kErrorNoSuchUser;
        if (result)
            *result = 1;
        return xam::kErrorSuccess;
    }

    // The sign-in UI, for another player's profile: there's only the one, so it isn't shown, as when it can't open, and
    // nobody else signs in (a game asking player 2 to sign in to save goes on without).
    uint32_t XamShowSigninUI(uint32_t paneCount, uint32_t flags)
    {
        LOG_INFO("XamShowSigninUI(%u panes, flags 0x%X): only one profile, not shown", paneCount, flags);
        return xam::kErrorFunctionFailed;
    }

    uint32_t XamUserAreUsersFriends(uint32_t /*userIndex*/, uint32_t /*xuids*/, uint32_t count, be<uint32_t>* results, xam::Overlapped* overlapped)
    {
        for (uint32_t i = 0; results && i < count; i++)
            results[i] = 0;
        return xam::Complete(overlapped, xam::kErrorSuccess);
    }
}

namespace xam
{
    uint32_t Complete(Overlapped* overlapped, uint32_t result, uint32_t length, uint32_t extendedError)
    {
        if (!overlapped)
            return result;
        overlapped->internalLow = result;
        overlapped->internalHigh = length;
        overlapped->extendedError = extendedError ? extendedError : (result == kErrorSuccess ? 0 : 0x80070000 | result);
        if (overlapped->completionRoutine != 0)
            LOG_WARN("XOVERLAPPED completion routine 0x%08X not called", uint32_t(overlapped->completionRoutine));
        if (overlapped->event != 0)
        {
            // Signal the event through the handle-based export so it wakes waiters.
            PPCContext ctx{};
            ctx.r3.u64 = overlapped->event;
            ctx.r4.u64 = 0;
            __imp__NtSetEvent(ctx, guest::g_base);
        }
        return kErrorIoPending;
    }
}

GUEST_EXPORT(XamUserGetSigninState, XamUserGetSigninState)
GUEST_EXPORT(XamUserGetXUID, XamUserGetXUID)
GUEST_EXPORT(XamUserGetName, XamUserGetName)
GUEST_EXPORT(XamUserCheckPrivilege, XamUserCheckPrivilege)
GUEST_EXPORT(XamShowSigninUI, XamShowSigninUI)
GUEST_EXPORT(XamUserAreUsersFriends, XamUserAreUsersFriends)
