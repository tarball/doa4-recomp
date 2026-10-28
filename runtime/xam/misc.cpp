// XAM odds and ends: notifications, background tasks, the title loader, voice and execution info.
#include "guest_abi.h"
#include "kernel/module.h"
#include "kernel/objects.h"
#include "kernel/threads.h"
#include "log.h"
#include "xam/xam.h"

#include <cstdlib>
#include <string>

namespace
{
    using namespace kernel;

    // A notification listener. Nothing posts notifications yet, so its queue is always empty.
    class NotificationListener : public GuestBackedObject
    {
    public:
        explicit NotificationListener(uint64_t mask) : GuestBackedObject(sizeof(DispatcherHeader)), mask(mask)
        {
            Header()->type = uint8_t(DispatcherType::NotificationEvent);
            Header()->size = sizeof(DispatcherHeader) / 4;
        }
        const char* TypeName() const override { return "NotificationListener"; }
        uint64_t mask;
    };

    uint32_t XamNotifyCreateListener(uint64_t mask, uint32_t /*maxVersion*/)
    {
        auto listener = std::make_shared<NotificationListener>(mask);
        RegisterGuestObject(listener);
        return CreateHandle(listener);
    }

    // Returns TRUE and fills id/param if a notification was dequeued.
    uint32_t XNotifyGetNext(uint32_t handle, uint32_t /*matchId*/, be<uint32_t>* id, be<uint32_t>* param)
    {
        if (!LookupHandleAs<NotificationListener>(handle))
            return 0;
        if (id)
            *id = 0;
        if (param)
            *param = 0;
        return 0;
    }

    // Background tasks run their callback on a new guest thread: XamTaskSchedule(callback, context, unknown, &handle).
    uint32_t XamTaskSchedule(uint32_t callback, uint32_t context, uint32_t /*unknown*/, be<uint32_t>* handle)
    {
        GuestThread::Params params;
        params.startAddress = callback;
        params.startContext = context;
        auto thread = GuestThread::Create(params);
        if (!thread)
            return xam::kErrorFunctionFailed;
        if (handle)
            *handle = CreateHandle(thread);
        return xam::kErrorSuccess;
    }

    uint32_t XamTaskShouldExit(uint32_t /*handle*/) { return 0; }

    uint32_t XamTaskCloseHandle(uint32_t handle)
    {
        return CloseHandle(handle) ? xam::kErrorSuccess : xam::kErrorInvalidParameter;
    }

    [[noreturn]] void XamLoaderTerminateTitle()
    {
        LOG_INFO("XamLoaderTerminateTitle: title exited");
        std::exit(0);
    }

    [[noreturn]] void XamLoaderLaunchTitle(const char* name, uint32_t flags)
    {
        LOG_INFO("XamLoaderLaunchTitle(\"%s\", 0x%X): launching another title isn't supported; exiting", name ? name : "", flags);
        std::exit(0);
    }

    // XamGetExecutionId(&info): points at the XEX's execution info header.
    uint32_t XamGetExecutionId(be<uint32_t>* info)
    {
        const uint32_t executionInfo = kernel::GetXexOptionalHeader(kernel::ExecutableXexHeader(), 0x00040006);
        if (info)
            *info = executionInfo;
        return executionInfo ? 0 : 0xC0000001;
    }

    // No headset is ever connected.
    uint32_t XamVoiceCreate(uint32_t /*userIndex*/, uint32_t /*flags*/, be<uint32_t>* voice)
    {
        if (voice)
            *voice = 0;
        return xam::kErrorDeviceNotConnected;
    }
    uint32_t XamVoiceHeadsetPresent(uint32_t /*voice*/) { return 0; }
    uint32_t XamVoiceClose(uint32_t /*voice*/) { return 0; }
    uint32_t XamVoiceSubmitPacket(uint32_t, uint32_t, uint32_t) { return xam::kErrorDeviceNotConnected; }

    // A big-endian UTF-16 guest string as UTF-8 (non-ASCII characters become '?').
    std::string GuestWideString(const be<uint16_t>* text)
    {
        std::string result;
        for (size_t i = 0; text && text[i] != 0 && i < 4096; i++)
        {
            const uint16_t c = text[i];
            result += c == 0x0A || (c >= 0x20 && c < 0x7F) ? char(c) : '?';
        }
        return result;
    }

    // System message boxes are answered with the focused button (logged for the record), or for a player who isn't
    // signed in with the last, declining: only the one profile can sign in (XamShowSigninUI), so a player asked to sign
    // in (player 2 joining a versus fight: [Sign In], [Don't Sign In]) would otherwise be asked again forever.
    uint32_t XamShowMessageBoxUI(uint32_t userIndex, const be<uint16_t>* title, const be<uint16_t>* text, uint32_t buttonCount,
        const be<uint32_t>* buttons, uint32_t focusButton, uint32_t flags, be<uint32_t>* result, xam::Overlapped* overlapped)
    {
        uint32_t answer = focusButton < buttonCount ? focusButton : 0;
        if (userIndex < 4 && !xam::IsUserSignedIn(userIndex) && buttonCount > 1)
            answer = buttonCount - 1;
        std::string buttonText;
        for (uint32_t i = 0; buttons && i < buttonCount && i < 3; i++)
        {
            const uint32_t address = buttons[i];
            buttonText += std::string(i ? ", " : "") + "[" + (address ? GuestWideString(reinterpret_cast<const be<uint16_t>*>(guest::ToHost(address))) : "") + "]";
        }
        LOG_WARN("XamShowMessageBoxUI (user %u, flags 0x%X): \"%s\": \"%s\" buttons %s; answering %u", userIndex, flags, GuestWideString(title).c_str(),
            GuestWideString(text).c_str(), buttonText.c_str(), answer);
        if (result)
            *result = answer;
        return xam::Complete(overlapped, xam::kErrorSuccess);
    }
}

GUEST_EXPORT(XamNotifyCreateListener, XamNotifyCreateListener)
GUEST_EXPORT(XNotifyGetNext, XNotifyGetNext)
GUEST_EXPORT(XamTaskSchedule, XamTaskSchedule)
GUEST_EXPORT(XamTaskShouldExit, XamTaskShouldExit)
GUEST_EXPORT(XamTaskCloseHandle, XamTaskCloseHandle)
GUEST_EXPORT(XamLoaderTerminateTitle, XamLoaderTerminateTitle)
GUEST_EXPORT(XamLoaderLaunchTitle, XamLoaderLaunchTitle)
GUEST_EXPORT(XamGetExecutionId, XamGetExecutionId)
GUEST_EXPORT(XamVoiceCreate, XamVoiceCreate)
GUEST_EXPORT(XamVoiceHeadsetPresent, XamVoiceHeadsetPresent)
GUEST_EXPORT(XamVoiceClose, XamVoiceClose)
GUEST_EXPORT(XamVoiceSubmitPacket, XamVoiceSubmitPacket)
GUEST_EXPORT(XamShowMessageBoxUI, XamShowMessageBoxUI)
