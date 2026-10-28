// Builds the game's code module from the player's disc image (build.h).
#include "launcher/build.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

#ifndef GAME_GENERATED_SOURCES
#define GAME_GENERATED_SOURCES 1 // the .cpp files XenonRecomp generates for this release (CMakeLists.txt)
#endif

namespace launcher
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // The free disk space a build needs: its files at their largest (DOA4's: the generated code 169 MB, the objects
        // 52 MB, default.xex 16 MB and the module, 38 MB, which stays), with a margin.
        constexpr uint64_t kBuildSpace = 350ull << 20;
        // The memory a compile takes, about (session 11 measured 250 MB a compile at 16 at once and 350 at 4; the largest
        // files take the most), and what's left for the rest of the PC.
        constexpr uint64_t kCompileMemory = 400ull << 20;
        constexpr uint64_t kReservedMemory = 1ull << 30;
        constexpr unsigned kMaxJobs = 32;
        // The progress bar's shares: XenonRecomp is about a tenth of the build, linking and checking the module its end.
        constexpr double kRecompileShare = 0.10, kCompileShare = 0.87;
        // The rate a compile goes at before one has finished to measure it by, in bytes of source a second: here (16 at
        // once on 16 logical processors) 351 KB/s, the largest files about 400, after XenonRecomp's 5 s, and a PC's in
        // proportion to its XenonRecomp's time.
        constexpr double kGuessedRate = 350e3, kGuessedRecompileSeconds = 5.0;

        const wchar_t kModuleName[] = L"" GAME_EXECUTABLE "_ppc.dll";

        // The build's log, in UTF-8, each line with the seconds since the build started.
        class Log
        {
        public:
            ~Log()
            {
                if (m_file)
                    fclose(m_file);
            }

            bool Open(const std::wstring& path)
            {
                m_start = Clock::now();
                m_file = _wfopen(path.c_str(), L"wb");
                return m_file != nullptr;
            }

            double Seconds() const
            {
                return std::chrono::duration<double>(Clock::now() - m_start).count();
            }

            void Line(const std::wstring& text)
            {
                if (!m_file)
                    return;
                fprintf(m_file, "[%6.1f s] %s\r\n", Seconds(), Utf8(text).c_str());
                fflush(m_file);
            }

            // A tool's output, as it wrote it, indented.
            void Output(const std::string& text)
            {
                if (!m_file || text.empty())
                    return;
                size_t start = 0;
                while (start < text.size())
                {
                    size_t end = text.find('\n', start);
                    if (end == std::string::npos)
                        end = text.size();
                    std::string line = text.substr(start, end - start);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    fprintf(m_file, "           %s\r\n", line.c_str());
                    start = end + 1;
                }
                fflush(m_file);
            }

        private:
            FILE* m_file = nullptr;
            Clock::time_point m_start;
        };

        // Deletes a folder and everything in it (a link inside is removed, not followed). True if it's all gone.
        bool RemoveTreeOnce(const std::wstring& path)
        {
            bool ok = true;
            WIN32_FIND_DATAW entry;
            const HANDLE search = FindFirstFileW(Join(path, L"*").c_str(), &entry);
            if (search != INVALID_HANDLE_VALUE)
            {
                do
                {
                    const std::wstring name = entry.cFileName;
                    if (name == L"." || name == L"..")
                        continue;
                    const std::wstring child = Join(path, name);
                    if (entry.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                        SetFileAttributesW(child.c_str(), entry.dwFileAttributes & ~DWORD(FILE_ATTRIBUTE_READONLY));
                    if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        ok = DeleteFileW(child.c_str()) && ok;
                    else if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                        ok = RemoveDirectoryW(child.c_str()) && ok;
                    else
                        ok = RemoveTreeOnce(child) && ok;
                } while (FindNextFileW(search, &entry));
                FindClose(search);
            }
            return RemoveDirectoryW(path.c_str()) && ok;
        }

        // Tries for a few seconds: a tool just ended, or a virus scanner, can hold a file a moment longer.
        bool RemoveTree(const std::wstring& path)
        {
            for (int attempt = 0; attempt < 50 && IsDirectory(path); attempt++)
            {
                if (RemoveTreeOnce(path))
                    return true;
                Sleep(100);
            }
            return !IsDirectory(path);
        }

        uint64_t TreeSize(const std::wstring& path)
        {
            uint64_t size = 0;
            WIN32_FIND_DATAW entry;
            const HANDLE search = FindFirstFileW(Join(path, L"*").c_str(), &entry);
            if (search == INVALID_HANDLE_VALUE)
                return 0;
            do
            {
                const std::wstring name = entry.cFileName;
                if (name == L"." || name == L"..")
                    continue;
                if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    size += (uint64_t(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
                else if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    size += TreeSize(Join(path, name));
            } while (FindNextFileW(search, &entry));
            FindClose(search);
            return size;
        }

        // The tools' environment: Windows' own variables and PATH its folders, nothing else, so none of a Visual Studio
        // environment's (CL, LINK, INCLUDE, ...) or clang's (CCC_OVERRIDE_OPTIONS) change what they build.
        std::wstring ToolEnvironment()
        {
            static const wchar_t* const kKept[] = { L"SystemRoot", L"SystemDrive", L"windir", L"TEMP", L"TMP", L"USERPROFILE", L"LOCALAPPDATA",
                L"APPDATA", L"ProgramData", L"COMPUTERNAME", L"USERNAME", L"NUMBER_OF_PROCESSORS", L"PROCESSOR_ARCHITECTURE", L"OS", L"PATHEXT",
                L"ComSpec" };
            std::vector<std::wstring> entries;
            for (const std::wstring& entry : EnvironmentEntries())
            {
                const size_t equals = entry.find(L'=', 1);
                if (equals == std::wstring::npos)
                    continue;
                const std::wstring name = entry.substr(0, equals);
                for (const wchar_t* kept : kKept)
                {
                    if (_wcsicmp(name.c_str(), kept) == 0)
                        entries.push_back(entry);
                }
            }
            wchar_t windows[MAX_PATH] = L"C:\\Windows";
            GetSystemWindowsDirectoryW(windows, MAX_PATH);
            entries.push_back(std::wstring(L"PATH=") + windows + L"\\System32;" + windows);
            return EnvironmentBlock(entries);
        }

        // How many files to compile at once: as many as the logical processors, as long as the free memory lasts.
        // DEBUG_LAUNCHER_JOBS=<n> sets it (for measuring).
        unsigned Jobs(uint64_t& available)
        {
            const unsigned processors = std::max(1u, unsigned(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
            MEMORYSTATUSEX memory{};
            memory.dwLength = sizeof(memory);
            available = GlobalMemoryStatusEx(&memory) ? memory.ullAvailPhys : 0;
            if (const char* jobs = std::getenv(GAME_PREFIX "_DEBUG_LAUNCHER_JOBS"); jobs && std::atoi(jobs) > 0)
                return std::min(unsigned(std::atoi(jobs)), kMaxJobs);
            const uint64_t byMemory = available > kReservedMemory + kCompileMemory ? (available - kReservedMemory) / kCompileMemory : 1;
            return unsigned(std::clamp<uint64_t>(byMemory, 1, std::min(processors, kMaxJobs)));
        }

        std::wstring Seconds(double seconds)
        {
            wchar_t text[32];
            swprintf(text, 32, L"%.1f s", seconds);
            return text;
        }

        // One build: its steps in order, each false when the build can't go on (m_message says why, unless cancelled).
        class Builder
        {
        public:
            Builder(const Folder& folder, const Disc& disc, const Reporter& report, HANDLE cancel, Log& log)
                : m_folder(folder), m_disc(disc), m_report(report), m_cancel(cancel), m_log(log), m_cancelAfter(CancelAfterSeconds())
            {
                m_recomp = folder.Recomp();
                m_private = Join(m_recomp, L"private");
                m_ppc = Join(m_recomp, L"ppc");
                m_objects = Join(m_recomp, L"obj");
                m_recompiler = Join(folder.Toolchain(), L"XenonRecomp.exe");
                m_clang = Join(folder.Toolchain(), L"bin\\clang.exe");
                m_lld = Join(folder.Toolchain(), L"bin\\lld-link.exe");
                m_environment = ToolEnvironment();
            }

            ~Builder()
            {
                if (m_job)
                    CloseHandle(m_job);
            }

            Readiness Run(std::wstring& message)
            {
                const bool built = Begin() && Recompile() && Compile() && Link() && Install();
                Clean();
                if (built)
                {
                    m_log.Line(L"Built in " + Seconds(m_log.Seconds()) + L": XenonRecomp " + Seconds(m_recompileSeconds) + L", compiling " +
                        Seconds(m_compileSeconds) + L" (" + std::to_wstring(m_jobs) + L" at once), linking " + Seconds(m_linkSeconds));
                    return Readiness::kReady;
                }
                if (m_cancelled)
                {
                    m_log.Line(L"Cancelled");
                    return Readiness::kCancelled;
                }
                m_log.Line(L"Failed: " + m_message);
                message = m_message;
                return Readiness::kFailed;
            }

        private:
            bool Fail(const std::wstring& message)
            {
                if (m_message.empty())
                    m_message = message;
                return false;
            }

            std::wstring CannotWrite(const std::wstring& path, DWORD error)
            {
                return L"Can't write " + path + L": " + SystemMessage(error) +
                    L"\n\nPut the game in a folder you can change, such as one in Documents (not Program Files).";
            }

            std::wstring CannotStart(const wchar_t* tool, const std::wstring& program, DWORD error)
            {
                return std::wstring(L"Can't start ") + tool + L": " + SystemMessage(error, FileNameOf(program)) +
                    L"\n\nIf antivirus software blocked or removed it, allow this folder, or extract the release again.";
            }

            // Cancel was pressed (or DEBUG_LAUNCHER_CANCEL's time came): the tools are ended at once.
            bool CheckCancelled()
            {
                if (!m_cancelled && m_cancelAfter > 0 && m_log.Seconds() >= m_cancelAfter)
                    SetEvent(m_cancel);
                if (!m_cancelled && WaitForSingleObject(m_cancel, 0) == WAIT_OBJECT_0)
                {
                    m_cancelled = true;
                    if (m_job)
                        TerminateJobObject(m_job, 1);
                }
                return m_cancelled;
            }

            void Show(double fraction, const std::wstring& text)
            {
                if (m_report)
                    m_report({ launcher::Report::Phase::kBuilding, std::clamp(fraction, 0.0, 1.0), text });
            }

            // Starts a tool in the build's job: no console window, below normal priority, its output (stdout and stderr) to
            // a file, and only those handles inherited. 0, or the error.
            DWORD Start(const std::wstring& program, const std::wstring& arguments, const std::wstring& output, HANDLE& process)
            {
                process = nullptr;
                SECURITY_ATTRIBUTES inheritable{ sizeof(inheritable), nullptr, TRUE };
                const HANDLE out = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inheritable,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (out == INVALID_HANDLE_VALUE)
                    return GetLastError();
                const HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
                if (in == INVALID_HANDLE_VALUE)
                {
                    const DWORD error = GetLastError();
                    CloseHandle(out);
                    return error;
                }
                SIZE_T size = 0;
                InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
                std::vector<uint8_t> storage(size);
                auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
                HANDLE inherited[2] = { out, in };
                DWORD error = 0;
                const bool initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &size);
                if (!initialized ||
                    !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
                    error = GetLastError();
                STARTUPINFOEXW startup{};
                startup.StartupInfo.cb = sizeof(startup);
                startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                startup.StartupInfo.hStdInput = in;
                startup.StartupInfo.hStdOutput = out;
                startup.StartupInfo.hStdError = out;
                startup.lpAttributeList = attributes;
                std::wstring commandLine = Quote(program) + L" " + arguments;
                PROCESS_INFORMATION info{};
                if (!error &&
                    !CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        m_environment.data(), m_recomp.c_str(), &startup.StartupInfo, &info))
                    error = GetLastError();
                if (initialized)
                    DeleteProcThreadAttributeList(attributes);
                CloseHandle(out);
                CloseHandle(in);
                if (error)
                    return error;
                if (!AssignProcessToJobObject(m_job, info.hProcess))
                {
                    error = GetLastError();
                    TerminateProcess(info.hProcess, 1);
                    CloseHandle(info.hThread);
                    CloseHandle(info.hProcess);
                    return error;
                }
                ResumeThread(info.hThread);
                CloseHandle(info.hThread);
                process = info.hProcess;
                return 0;
            }

            // A tool's output file into the log, then deleted.
            void Collect(const std::wstring& output)
            {
                std::vector<uint8_t> bytes;
                if (ReadFileBytes(output, bytes))
                    m_log.Output(std::string(bytes.begin(), bytes.end()));
                DeleteFileW(output.c_str());
            }

            // Runs a tool to its end, calling `tick` every 100 ms. False if it failed (or the build was cancelled).
            bool RunTool(const wchar_t* what, const std::wstring& program, const std::wstring& arguments, const std::function<void()>& tick = {})
            {
                const std::wstring output = Join(m_objects, std::wstring(what) + L".txt");
                m_log.Line(std::wstring(what) + L": " + FileNameOf(program) + L" " + arguments);
                HANDLE process = nullptr;
                if (const DWORD error = Start(program, arguments, output, process))
                {
                    m_log.Line(L"  can't start it: " + SystemMessage(error, FileNameOf(program)));
                    return Fail(CannotStart(what, program, error));
                }
                const HANDLE waits[2] = { process, m_cancel };
                for (;;)
                {
                    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 100);
                    if (CheckCancelled())
                        break;
                    if (wait == WAIT_OBJECT_0)
                        break;
                    if (tick)
                        tick();
                }
                if (m_cancelled)
                {
                    WaitForSingleObject(process, 5000);
                    CloseHandle(process);
                    return false;
                }
                DWORD code = 1;
                GetExitCodeProcess(process, &code);
                CloseHandle(process);
                Collect(output);
                m_log.Line(std::wstring(what) + L": exit code " + std::to_wstring(code));
                if (code != 0)
                    return Fail(std::wstring(what) + L" stopped with an error (exit code " + std::to_wstring(code) + L"), so the game's code wasn't built.\n\n"
                        L"The build's log has its messages: " + m_folder.BuildLog());
                return true;
            }

            // The checks, the folders, default.xex where the config names it, the job.
            bool Begin()
            {
                ULARGE_INTEGER free{};
                if (GetDiskFreeSpaceExW(m_folder.root.c_str(), &free, nullptr, nullptr))
                {
                    m_log.Line(L"Free disk space: " + Megabytes(free.QuadPart));
                    if (free.QuadPart < kBuildSpace)
                        return Fail(L"There isn't enough free disk space to build the game's code: it needs about " + Megabytes(kBuildSpace) +
                            L" while it builds (" + Megabytes(38ull << 20) + L" afterwards), and the drive has " + Megabytes(free.QuadPart) +
                            L" free.\n\nFree some space, then try again.");
                }
                // The old module first: a game running from this folder holds it.
                const std::wstring module = m_folder.Module();
                if (IsFile(module) && !DeleteFileW(module.c_str()))
                {
                    const DWORD error = GetLastError();
                    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)
                        return Fail(L"Can't replace the game's code, " + module + L": " + SystemMessage(error) +
                            L"\n\nIf the game is running from this folder, close it, then try again.");
                    return Fail(CannotWrite(module, error));
                }
                for (const std::wstring& directory : { m_private, m_ppc, m_objects })
                {
                    if (!MakeDirectories(directory))
                        return Fail(CannotWrite(directory, GetLastError()));
                }
                // The module is linked into obj\, then moved beside the executable: that folder must take a new file too.
                const std::wstring probe = module + L".new";
                const HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
                if (file == INVALID_HANDLE_VALUE)
                    return Fail(CannotWrite(m_folder.root, GetLastError()));
                CloseHandle(file);
                const std::wstring xex = Join(m_private, L"default.xex");
                if (const DWORD error = WriteFileAtomically(xex, m_disc.xex.data(), m_disc.xex.size()))
                    return Fail(CannotWrite(xex, error));
                m_log.Line(L"default.xex from the disc image: " + std::to_wstring(m_disc.xex.size()) + L" bytes, to recomp\\private");

                // The tools end with the job: when the build is cancelled, and when the launcher ends, however it ends.
                m_job = CreateJobObjectW(nullptr, nullptr);
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
                if (!m_job || !SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
                    return Fail(L"Can't set up the build: " + SystemMessage(GetLastError()));
                return !CheckCancelled();
            }

            // XenonRecomp, which translates the game's PowerPC code into C++ (the player reads "Translating": the next
            // step compiles). Its output comes only at its end (the C runtime buffers it), so its progress is the
            // generated files it has written, of the release's GAME_GENERATED_SOURCES.
            bool Recompile()
            {
                const Clock::time_point start = Clock::now();
                Show(0, L"Translating the game's code...");
                const auto tick = [this] {
                    unsigned count = 0;
                    WIN32_FIND_DATAW entry;
                    const HANDLE search = FindFirstFileW(Join(m_ppc, L"*.cpp").c_str(), &entry);
                    if (search != INVALID_HANDLE_VALUE)
                    {
                        do
                            count++;
                        while (FindNextFileW(search, &entry));
                        FindClose(search);
                    }
                    const double done = std::min(1.0, double(count) / double(std::max(GAME_GENERATED_SOURCES, 1)));
                    Show(kRecompileShare * done, L"Translating the game's code: " + std::to_wstring(int(done * 100)) + L"%");
                };
                const bool ok = RunTool(L"XenonRecomp", m_recompiler, L"config\\" GAME_PREFIX ".toml include\\ppc_context.h", tick);
                m_recompileSeconds = std::chrono::duration<double>(Clock::now() - start).count();
                if (ok)
                    m_log.Line(L"XenonRecomp: " + Seconds(m_recompileSeconds) + L", generated code " + Megabytes(TreeSize(m_ppc)));
                return ok;
            }

            struct Source
            {
                std::wstring path;   // relative to recomp\.
                std::wstring name;   // the file's name
                uint64_t size = 0;
            };

            struct Running
            {
                HANDLE process = nullptr;
                const Source* source = nullptr;
                std::wstring output;
                Clock::time_point start;
            };

            // The generated code and the module's own source, the largest first, as many at once as Jobs gives.
            bool Compile()
            {
                std::vector<Source> sources;
                WIN32_FIND_DATAW entry;
                const HANDLE search = FindFirstFileW(Join(m_ppc, L"*.cpp").c_str(), &entry);
                if (search != INVALID_HANDLE_VALUE)
                {
                    do
                        sources.push_back({ L"ppc\\" + std::wstring(entry.cFileName), entry.cFileName,
                            (uint64_t(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow });
                    while (FindNextFileW(search, &entry));
                    FindClose(search);
                }
                if (sources.empty())
                    return Fail(L"XenonRecomp made no code from the disc image's default.xex, so the game's code wasn't built.\n\nThe build's log: " +
                        m_folder.BuildLog());
                sources.push_back({ L"src\\module.cpp", L"module.cpp", 4096 });
                std::stable_sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) { return a.size > b.size; });
                uint64_t total = 0;
                for (const Source& source : sources)
                    total += source.size;

                uint64_t available = 0;
                m_jobs = Jobs(available);
                m_log.Line(L"Compiling " + std::to_wstring(sources.size()) + L" files, " + std::to_wstring(m_jobs) + L" at once (" +
                    std::to_wstring(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) + L" logical processors, " + Megabytes(available) +
                    L" of memory free)");
                // compile.rsp holds the flags the development build compiles the module with; crash reports (which would copy
                // the generated code to the temporary folder) are off.
                const std::wstring flags = L"--driver-mode=cl @compile.rsp /clang:-fno-crash-diagnostics /FIinclude\\ppc_overrides.h "
                                           L"-Iinclude\\module -Ippc -Iinclude\\simde -c ";
                const Clock::time_point start = Clock::now();
                std::vector<Running> running;
                size_t next = 0, done = 0;
                uint64_t doneSize = 0;
                double doneSeconds = 0;
                const double guessedRate = kGuessedRate * std::clamp(kGuessedRecompileSeconds / std::max(m_recompileSeconds, 0.1), 0.2, 2.0);
                double shown = 0; // the part of the compile the progress bar and its text show
                const auto finish = [&](bool ok) {
                    if (!ok)
                        TerminateJobObject(m_job, 1);
                    for (Running& job : running)
                    {
                        WaitForSingleObject(job.process, 5000);
                        CloseHandle(job.process);
                        DeleteFileW(Join(m_recomp, job.output).c_str());
                    }
                    running.clear();
                    m_compileSeconds = std::chrono::duration<double>(Clock::now() - start).count();
                    return ok;
                };
                while (next < sources.size() || !running.empty())
                {
                    while (running.size() < m_jobs && next < sources.size())
                    {
                        const Source& source = sources[next++];
                        Running job;
                        job.source = &source;
                        job.output = L"obj\\" + source.name + L".txt";
                        job.start = Clock::now();
                        const std::wstring arguments = flags + source.path + L" /Foobj\\" + source.name + L".obj";
                        if (const DWORD error = Start(m_clang, arguments, Join(m_recomp, job.output), job.process))
                        {
                            m_log.Line(L"clang: can't start it: " + SystemMessage(error, L"clang.exe"));
                            return finish(Fail(CannotStart(L"clang, the compiler", m_clang, error)));
                        }
                        running.push_back(job);
                    }
                    std::vector<HANDLE> waits = { m_cancel };
                    for (const Running& job : running)
                        waits.push_back(job.process);
                    const DWORD wait = WaitForMultipleObjects(DWORD(waits.size()), waits.data(), FALSE, 100);
                    if (CheckCancelled())
                        return finish(false);
                    if (wait > WAIT_OBJECT_0 && wait < WAIT_OBJECT_0 + waits.size())
                    {
                        Running job = running[wait - WAIT_OBJECT_0 - 1];
                        running.erase(running.begin() + (wait - WAIT_OBJECT_0 - 1));
                        DWORD code = 1;
                        GetExitCodeProcess(job.process, &code);
                        CloseHandle(job.process);
                        const double seconds = std::chrono::duration<double>(Clock::now() - job.start).count();
                        m_log.Line(L"  " + job.source->name + L": " + Seconds(seconds) + (code ? L", exit code " + std::to_wstring(code) : L""));
                        Collect(Join(m_recomp, job.output));
                        if (code != 0)
                            return finish(Fail(L"Compiling the game's code failed (" + job.source->name + L", exit code " + std::to_wstring(code) +
                                L").\n\nThe build's log has the compiler's messages: " + m_folder.BuildLog()));
                        done++;
                        doneSize += job.source->size;
                        doneSeconds += seconds;
                    }
                    // The files being compiled count for the part their time so far would compile at the rate measured.
                    // That estimate drops when a file takes longer than the rate says, so what's shown never goes back: it
                    // closes on the estimate when that's ahead, a part of the way each time, and waits when it's behind.
                    const double rate = doneSeconds > 0 ? double(doneSize) / doneSeconds : guessedRate;
                    double estimate = double(doneSize);
                    for (const Running& job : running)
                    {
                        const double seconds = std::chrono::duration<double>(Clock::now() - job.start).count();
                        estimate += std::min(0.95 * double(job.source->size), seconds * rate);
                    }
                    estimate /= double(std::max<uint64_t>(total, 1));
                    if (estimate > shown)
                        shown += (estimate - shown) * 0.3;
                    Show(kRecompileShare + kCompileShare * shown, L"Compiling the game's code: " + std::to_wstring(int(shown * 100)) + L"%");
                }
                finish(true);
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                QueryInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr);
                QueryInformationJobObject(m_job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr);
                const double cpu = double(accounting.TotalUserTime.QuadPart + accounting.TotalKernelTime.QuadPart) / 1e7;
                m_log.Line(L"Compiled " + std::to_wstring(done) + L" files in " + Seconds(m_compileSeconds) + L"; the build's tools so far: " +
                    Seconds(cpu) + L" of CPU, at most " + Megabytes(limits.PeakJobMemoryUsed) + L" of memory at once (the most for one: " +
                    Megabytes(limits.PeakProcessMemoryUsed) + L"); objects " + Megabytes(TreeSize(m_objects)));
                return true;
            }

            // The executable's import library, then the module, its objects in name order (ordinal), as the development
            // build lists them, so both link the same file.
            bool Link()
            {
                const Clock::time_point start = Clock::now();
                Show(kRecompileShare + kCompileShare, L"Linking the game's code...");
                std::vector<std::wstring> names;
                WIN32_FIND_DATAW entry;
                const HANDLE search = FindFirstFileW(Join(m_objects, L"*.obj").c_str(), &entry);
                if (search != INVALID_HANDLE_VALUE)
                {
                    do
                        names.push_back(entry.cFileName);
                    while (FindNextFileW(search, &entry));
                    FindClose(search);
                }
                std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return wcscmp(a.c_str(), b.c_str()) < 0; });
                std::string list;
                for (const std::wstring& name : names)
                    list += "obj\\" + Utf8(name) + "\r\n";
                const std::wstring objects = Join(m_objects, L"objects.rsp");
                if (const DWORD error = WriteFileAtomically(objects, list.data(), list.size()))
                    return Fail(CannotWrite(objects, error));
                const bool ok = RunTool(L"Import library", m_lld, L"/lib /nologo /def:" GAME_EXECUTABLE ".def /machine:x64 /out:obj\\" GAME_EXECUTABLE ".lib") &&
                    RunTool(L"Link", m_lld, L"@link.rsp /DEBUG:NONE @obj\\objects.rsp /wholearchive:obj\\" GAME_EXECUTABLE ".lib /def:" GAME_EXECUTABLE
                                            "_ppc.def /out:obj\\" GAME_EXECUTABLE "_ppc.dll");
                m_linkSeconds = std::chrono::duration<double>(Clock::now() - start).count();
                return ok;
            }

            // The module, if it's the release's, beside the executable.
            bool Install()
            {
                Show(0.99, L"Checking the game's code...");
                const std::wstring built = Join(m_objects, kModuleName);
                const std::string hash = FileSha256(built), expected = ExpectedModuleHash(m_folder);
                WIN32_FILE_ATTRIBUTE_DATA attributes{};
                GetFileAttributesExW(built.c_str(), GetFileExInfoStandard, &attributes);
                m_log.Line(std::wstring(kModuleName) + L": " + std::to_wstring((uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow) +
                    L" bytes, SHA-256 " + FromUtf8(hash) + (hash == expected ? L" (the release's)" : L" (NOT the release's, " + FromUtf8(expected) + L")"));
                if (hash.empty() || hash != expected)
                    return Fail(L"The game's code was built, but it isn't this release's: its SHA-256 is " + FromUtf8(hash) + L", the release's " +
                        FromUtf8(expected) + L". The game won't start with it.\n\nPlease report this with the build's log: " + m_folder.BuildLog());
                if (!MoveFileExW(built.c_str(), m_folder.Module().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    return Fail(CannotWrite(m_folder.Module(), GetLastError()));
                Show(1, L"The game's code is built.");
                return true;
            }

            // The tools ended and the intermediate files deleted, whatever happened.
            void Clean()
            {
                if (m_job)
                {
                    TerminateJobObject(m_job, 1);
                    for (int i = 0; i < 100; i++)
                    {
                        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                        if (!QueryInformationJobObject(m_job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) ||
                            accounting.ActiveProcesses == 0)
                            break;
                        Sleep(50);
                    }
                }
                Show(m_cancelled ? 0.0 : 1.0, L"Deleting the build's files...");
                uint64_t size = 0;
                bool removed = true;
                for (const std::wstring& directory : { m_private, m_ppc, m_objects })
                {
                    size += TreeSize(directory);
                    removed = RemoveTree(directory) && removed;
                }
                m_log.Line(std::wstring(removed ? L"Deleted" : L"Couldn't delete all of") + L" the intermediate files (" + Megabytes(size) +
                    L": recomp\\private, ppc and obj)");
            }

            const Folder& m_folder;
            const Disc& m_disc;
            const Reporter& m_report;
            HANDLE m_cancel;
            Log& m_log;
            const double m_cancelAfter;
            std::wstring m_recomp, m_private, m_ppc, m_objects, m_recompiler, m_clang, m_lld, m_environment, m_message;
            HANDLE m_job = nullptr;
            bool m_cancelled = false;
            unsigned m_jobs = 0;
            double m_recompileSeconds = 0, m_compileSeconds = 0, m_linkSeconds = 0;
        };
    }

    Build::Build()
    {
        m_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    Build::~Build()
    {
        if (m_cancel)
            CloseHandle(m_cancel);
    }

    void Build::Cancel()
    {
        SetEvent(m_cancel);
    }

    bool Build::Cancelled() const
    {
        return WaitForSingleObject(m_cancel, 0) == WAIT_OBJECT_0;
    }

    void Build::Reset()
    {
        ResetEvent(m_cancel);
    }

    Readiness Build::Run(const Folder& folder, const Disc& disc, const Reporter& report, std::wstring& message)
    {
        if (!MakeDirectories(folder.data))
        {
            message = L"Can't write " + folder.data + L": " + SystemMessage(GetLastError()) +
                L"\n\nPut the game in a folder you can change, such as one in Documents (not Program Files).";
            return Readiness::kFailed;
        }
        Log log;
        if (!log.Open(folder.BuildLog()))
        {
            message = L"Can't write " + folder.BuildLog() + L": " + SystemMessage(GetLastError()) +
                L"\n\nPut the game in a folder you can change, such as one in Documents (not Program Files).";
            return Readiness::kFailed;
        }
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t date[64];
        swprintf(date, 64, L"%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        log.Line(L"" GAME_PREFIX " Launcher: building the game's code, " GAME_EXECUTABLE "_ppc.dll, " + std::wstring(date));
        log.Line(L"Release folder: " + folder.root);
        log.Line(L"Disc image: " + disc.path);
        log.Line(L"The release's module: SHA-256 " + FromUtf8(ExpectedModuleHash(folder)));
        Builder builder(folder, disc, report, m_cancel, log);
        return builder.Run(message);
    }

    BuildLock::~BuildLock()
    {
        Release();
    }

    bool BuildLock::TryAcquire(const Folder& folder, unsigned long& error)
    {
        error = 0;
        if (m_file != INVALID_HANDLE_VALUE)
            return true;
        const std::wstring path = Join(folder.Recomp(), L"build.lock");
        for (int attempt = 0;; attempt++)
        {
            m_file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                nullptr);
            if (m_file != INVALID_HANDLE_VALUE)
                return true;
            const DWORD last = GetLastError();
            if (last == ERROR_SHARING_VIOLATION)
                return false;
            // Denied a moment while another launcher's lock is being deleted; for longer, the folder can't be written.
            if (attempt >= 4)
            {
                error = last;
                return false;
            }
            Sleep(50);
        }
    }

    void BuildLock::Release()
    {
        if (m_file != INVALID_HANDLE_VALUE)
            CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }

    void RemoveLeftovers(const Folder& folder)
    {
        for (const wchar_t* name : { L"private", L"ppc", L"obj" })
            RemoveTree(Join(folder.Recomp(), name));
        DeleteFileW((folder.Module() + L".new").c_str());
    }
}
