// Getting ready to play and starting the game (launcher.h).
#include "launcher/build.h"
#include "launcher/launcher.h"

#include <cstdlib>
#include <cwctype>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <intrin.h>

namespace launcher
{
    namespace
    {
        // The game is built for AVX (/arch:AVX): the processor must have it, and Windows must keep its registers.
        __attribute__((target("xsave"))) bool HasAvx()
        {
            int info[4];
            __cpuid(info, 1);
            const bool osxsave = (info[2] & (1 << 27)) != 0, avx = (info[2] & (1 << 28)) != 0;
            return osxsave && avx && (_xgetbv(0) & 6) == 6;
        }

        std::wstring TrimmedW(const std::wstring& text)
        {
            const size_t first = text.find_first_not_of(L" \t\r\n"), last = text.find_last_not_of(L" \t\r\n");
            return first == std::wstring::npos ? std::wstring() : text.substr(first, last - first + 1);
        }

        bool SamePath(const std::wstring& a, const std::wstring& b)
        {
            return !a.empty() && CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
        }

        // `path` as the game's data directory argument: relative to the release folder (the game's working folder) when
        // it's inside it, so the game's narrow paths never hold the folder's own name; else as the game's paths take it.
        std::string DataArgument(const Folder& folder, std::wstring& problem)
        {
            const std::wstring root = folder.root + L"\\";
            if (folder.data.size() > root.size() && CompareStringOrdinal(folder.data.c_str(), int(root.size()), root.c_str(), int(root.size()), TRUE) == CSTR_EQUAL)
            {
                const std::string relative = NarrowPath(folder.data.substr(root.size()));
                if (!relative.empty())
                    return relative;
            }
            const std::string narrow = NarrowPath(folder.data);
            if (narrow.empty())
                problem = L"The game can't use the data folder " + folder.data +
                    L": its path has characters that this PC's code page for older programs lacks.";
            return narrow;
        }
    }

    double CancelAfterSeconds()
    {
        const char* setting = std::getenv(GAME_PREFIX "_DEBUG_LAUNCHER_CANCEL");
        return setting ? std::atof(setting) : 0.0;
    }

    Readiness Prepare(const Folder& folder, const std::wstring& chosen, Build& build, const Reporter& report, std::wstring& message, Disc& disc)
    {
        message.clear();
        disc = {};
        const auto show = [&](Report::Phase phase, const std::wstring& text) {
            if (report)
                report({ phase, 0, text });
        };
        show(Report::Phase::kChecking, L"Checking the game's files...");
        if (const std::wstring missing = MissingFile(folder); !missing.empty())
        {
            message = L"This release isn't complete: " + missing + L" is missing from " + folder.root +
                L".\n\nExtract the whole zip again, to a folder of its own.";
            return Readiness::kFailed;
        }
        if (!HasAvx())
        {
            message = L"This PC's processor doesn't have AVX, which the game needs: processors from 2011 on have it.";
            return Readiness::kFailed;
        }

        // The disc image: the one chosen, else the one remembered, else one next to the launcher or in the data folder.
        show(Report::Phase::kChecking, L"Checking your disc image...");
        std::vector<std::wstring> candidates;
        const std::wstring remembered = RememberedDisc(folder);
        if (!chosen.empty())
            candidates.push_back(chosen);
        else
        {
            if (!remembered.empty())
                candidates.push_back(remembered);
            for (const std::wstring& nearby : DiscsNearby(folder))
            {
                if (!SamePath(FullPath(nearby), FullPath(remembered)))
                    candidates.push_back(nearby);
            }
        }
        if (candidates.empty())
            return Readiness::kNeedDisc;
        std::wstring first;
        for (const std::wstring& candidate : candidates)
        {
            const std::wstring problem = CheckDisc(candidate, disc);
            if (problem.empty())
            {
                first.clear();
                break;
            }
            if (first.empty())
                first = problem;
        }
        if (!first.empty())
        {
            message = first;
            return Readiness::kNeedDisc;
        }
        if (!SamePath(disc.path, FullPath(remembered)))
        {
            if (const std::wstring problem = RememberDisc(folder, disc); !problem.empty())
            {
                message = problem;
                return Readiness::kFailed;
            }
        }

        // The game's code: the release's, or built now.
        if (folder.development)
        {
            if (IsFile(folder.Module()))
                return Readiness::kReady;
            message = L"There's no " GAME_EXECUTABLE "_ppc.dll beside " GAME_EXECUTABLE ".exe in " + folder.root +
                L", and this isn't a release's folder (it has no recomp folder), so it can't be built here: build it with build.cmd.";
            return Readiness::kFailed;
        }
        const std::string expected = ExpectedModuleHash(folder);
        if (expected.empty())
        {
            message = L"This release isn't complete: " + folder.ExpectedHashFile() + L" isn't right.\n\nExtract the whole zip again, to a folder of its own.";
            return Readiness::kFailed;
        }
        // Another launcher in this folder holds the lock while it checks the module (a moment) or builds it.
        BuildLock lock;
        for (int waits = 0;; waits++)
        {
            unsigned long error = 0;
            if (lock.TryAcquire(folder, error))
                break;
            if (error)
            {
                message = L"Can't write " + folder.Recomp() + L": " + SystemMessage(error) +
                    L"\n\nPut the game in a folder you can change, such as one in Documents (not Program Files).";
                return Readiness::kFailed;
            }
            if (waits == 4)
                show(Report::Phase::kWaiting, L"Another launcher is building the game's code in this folder: this one waits for it.");
            if (build.Cancelled())
                return Readiness::kCancelled;
            Sleep(250);
        }
        RemoveLeftovers(folder);
        show(Report::Phase::kChecking, L"Checking the game's code...");
        if (FileSha256(folder.Module()) == expected)
            return Readiness::kReady;
        return build.Run(folder, disc, report, message);
    }

    std::string NetplaySetting(const Choices& choices, std::wstring& problem)
    {
        problem.clear();
        if (!choices.netplay)
            return {};
        if (choices.host)
        {
            if (!choices.hostPort)
            {
                problem = L"Type the port to host on, from 1 to 65535.";
                return {};
            }
            return "host:" + std::to_string(choices.hostPort);
        }
        const std::wstring address = TrimmedW(choices.address);
        if (address.empty())
        {
            problem = L"Type the host's address, such as 203.0.113.7.";
            return {};
        }
        if (address.find(L':') != std::wstring::npos)
        {
            problem = L"Type the host's address without its port, which has a box of its own: netplay needs an IPv4 address, such as "
                L"203.0.113.7, or a name that has one.";
            return {};
        }
        if (!choices.joinPort)
        {
            problem = L"Type the host's port, from 1 to 65535.";
            return {};
        }
        // An IPv4 address as it is, or a name looked up (the game takes IPv4 addresses only).
        static const bool started = [] {
            WSADATA data;
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        in_addr ipv4{};
        if (InetPtonW(AF_INET, address.c_str(), &ipv4) != 1)
        {
            addrinfoW hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfoW* found = nullptr;
            const int error = started ? GetAddrInfoW(address.c_str(), nullptr, &hints, &found) : WSANOTINITIALISED;
            if (error != 0 || !found)
            {
                problem = L"Can't find the host " + address + L": " + SystemMessage(DWORD(error)) +
                    L"\n\nCheck its address: netplay needs an IPv4 address, such as 203.0.113.7, or a name that has one.";
                return {};
            }
            ipv4 = reinterpret_cast<const sockaddr_in*>(found->ai_addr)->sin_addr;
            FreeAddrInfoW(found);
        }
        char text[INET_ADDRSTRLEN] = "";
        inet_ntop(AF_INET, &ipv4, text, sizeof(text));
        return "join:" + std::string(text) + ":" + std::to_string(choices.joinPort);
    }

    std::wstring StartGame(const Folder& folder, const Choices& choices, bool wait, unsigned long* exitCode)
    {
        std::wstring problem;
        const std::string netplay = NetplaySetting(choices, problem);
        if (!problem.empty())
            return problem;
        const std::string data = DataArgument(folder, problem);
        if (!problem.empty())
            return problem;

        // The launcher's environment, but for the session's variables, which the choices set.
        const std::wstring netplayName = L"" GAME_PREFIX "_NETPLAY", rollbackName = L"" GAME_PREFIX "_NETPLAY_ROLLBACK";
        std::vector<std::wstring> entries;
        for (const std::wstring& entry : EnvironmentEntries())
        {
            const std::wstring name = entry.substr(0, entry.find(L'=', 1));
            if (_wcsicmp(name.c_str(), netplayName.c_str()) != 0 && _wcsicmp(name.c_str(), rollbackName.c_str()) != 0)
                entries.push_back(entry);
        }
        if (!netplay.empty())
        {
            // Lockstep's 0 too: rollback is the game's default.
            entries.push_back(netplayName + L"=" + FromUtf8(netplay));
            entries.push_back(rollbackName + (choices.rollback ? L"=1" : L"=0"));
        }
        std::wstring environment = EnvironmentBlock(entries);

        const std::wstring executable = folder.Executable();
        std::wstring commandLine = Quote(executable) + L" " + Quote(FromNarrow(data));
        // No standard handles: the game logs to <data>\<executable>.log. It gets Windows' default error mode, not the
        // launcher's (which keeps the tools' crashes from showing dialogs).
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        PROCESS_INFORMATION info{};
        const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_DEFAULT_ERROR_MODE | (wait ? CREATE_SUSPENDED : 0);
        if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags, environment.data(), folder.root.c_str(), &startup, &info))
            return L"Can't start " + executable + L": " + SystemMessage(GetLastError(), FileNameOf(executable)) +
                L"\n\nIf antivirus software blocked or removed it, allow this folder, or extract the release again.";
        if (!wait)
        {
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
            return {};
        }
        // Waiting (for tests): the game ends with the launcher, however the launcher ends.
        const HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job)
        {
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            AssignProcessToJobObject(job, info.hProcess);
        }
        ResumeThread(info.hThread);
        CloseHandle(info.hThread);
        WaitForSingleObject(info.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hProcess);
        if (job)
            CloseHandle(job);
        if (exitCode)
            *exitCode = code;
        return {};
    }
}
