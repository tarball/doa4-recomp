// DOA4's frame timing diagnostic (frame_times.h).
#include "game/frame_times.h"

#include "config.h"
#include "gpu/command_processor.h"
#include "gpu/memory_watch.h"
#include "guest_abi.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/game_state.h"
#include "netplay/replay.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <intrin.h>
#include <windows.h>

namespace game::frametimes
{
    namespace
    {
        // The calls the update (sub_824F4878) makes, in its order, each timed when called from it (the hooks below), with
        // what's known of them. Not timed: the register save and restore helpers, SetEvent and ResetEvent around the
        // worker pools' jobs, QueryPerformanceCounter, and the wait for the pools (sub_827922E0, timed apart).
        struct UpdateCall
        {
            uint32_t function;
            const char* what;
        };
        constexpr UpdateCall kUpdateCalls[] = {
            { 0x824F31E8, "the scheduler: the task fibers" },
            { 0x82401CD8, "the file loader" },
            { 0x823FBE18, "the fight's per-player setup" },
            { 0x824F4CE8, "" },
            { 0x82566430, "the stage manager's frame" },
            { 0x82354DB0, "the effect system" },
            { 0x82542BF8, "sound" },
            { 0x825413D0, "sound: fades" },
            { 0x82541538, "sound: voices playing" },
            { 0x8231DCD8, "" },
            { 0x8245E580, "" },
            { 0x82566818, "the stage manager" },
            { 0x823EACB8, "the spectators' snapshot" },
            { 0x824B2F78, "" },
            { 0x824F4E90, "" },
            { 0x82566B80, "the stage manager" },
            { 0x824B2EE0, "" },
            { 0x824F0F00, "" },
            { 0x82566920, "the stage manager" },
            { 0x82354EB8, "the effect system" },
            { 0x82354F30, "the effect system" },
            { 0x8231D750, "" },
            { 0x824B2DC8, "" },
            { 0x823B43D0, "" },
            { 0x8231D248, "" },
            { 0x82449EA8, "the network" },
            { 0x8244B690, "the network" },
            { 0x8247F388, "" },
            { 0x82453430, "" },
            { 0x82308A10, "" },
            { 0x824F10D8, "" },
            { 0x8242B778, "" },
            { 0x8242A5D8, "movies" },
            { 0x82732890, "" },
            { 0x823F8A10, "" },
            { 0x824519A0, "" },
            { 0x827655C0, "D3D" },
            { 0x8274FDD8, "D3D" },
            { 0x8277A248, "the GPU memory pool" },
            { 0x8277A308, "the GPU memory pool" },
            { 0x82758A48, "D3D" },
            { 0x82758DB0, "D3D" },
            { 0x82472ED8, "" },
            { 0x82451F20, "" },
            { 0x824524A8, "" },
            { 0x8245D718, "" },
            { 0x82354678, "the effect system" },
            { 0x823B4BB8, "" },
            { 0x82475648, "" },
            { 0x824F3E68, "" },
            { 0x823886D8, "the UI tasks" },
            { 0x82732950, "" },
            { 0x82355B68, "" },
        };
        constexpr size_t kUpdateCallCount = std::size(kUpdateCalls);
        constexpr uint32_t kUpdateStart = 0x824F4878, kUpdateEnd = 0x824F4CE4; // sub_824F4878's code

        struct Settings
        {
            bool set = false;
            bool fight = false; // `first` counts fight frames
            uint64_t first = 0, count = 600;
            Settings()
            {
                const char* setting = std::getenv(GAME_PREFIX "_DEBUG_FRAME_TIMES");
                if (!setting)
                    return;
                set = true;
                if (*setting == 'F' || *setting == 'f')
                {
                    fight = true;
                    setting++;
                }
                char* end = nullptr;
                first = strtoull(setting, &end, 10);
                if (end && *end == ',')
                    count = std::max<uint64_t>(strtoull(end + 1, nullptr, 10), 1);
            }
        };

        const Settings& GetSettings()
        {
            static const Settings settings;
            return settings;
        }

        int64_t Frequency()
        {
            static const int64_t frequency = [] {
                LARGE_INTEGER value;
                QueryPerformanceFrequency(&value);
                return int64_t(value.QuadPart);
            }();
            return frequency;
        }

        // A recorded frame, in QueryPerformanceCounter ticks (CPU time in TSC cycles).
        struct Record
        {
            uint64_t frame = 0;
            int64_t fightFrame = -1;
            bool replayed = false;
            int64_t wall = 0; // from this frame's top to the next frame's
            int64_t parts[kPartCount]{};
            int64_t tasks = 0, workerWait = 0, gpuWaitUpdate = 0, gpuWaitEnd = 0;
            uint32_t taskRuns = 0;
            uint64_t mainCycles = 0, workerCycles = 0; // the main thread's, from top to end; the workers', top to top
            uint64_t faults = 0;                       // the main thread's write faults on memory the GPU holds
            int64_t faultTicks = 0;                    // and the time in their handler
            uint64_t d3dCycles = 0;                    // in D3D's entry points called from the game's code (TSC cycles)
            int64_t d3dTicks = 0;                      // and in those among the update's calls (QueryPerformanceCounter)
            uint32_t d3dCalls = 0;
            std::array<int64_t, kUpdateCallCount> calls{};
        };

        struct Task
        {
            uint64_t ticks = 0, replayTicks = 0;
            uint32_t frames = 0, replayFrames = 0;
            uint32_t yieldSite = 0;
            std::vector<uint32_t> chain; // the guest call chain where it first yielded
        };

        bool g_recording = false; // the current frame (main thread)
        bool g_done = false;
        thread_local bool t_main = false;
        int64_t g_top = 0, g_partStart = 0;
        Part g_part = kBoundary;
        uint64_t g_topCycles = 0;
        gpu::watch::FaultStatistics g_topFaults;
        uint64_t g_workerCycles = 0; // the workers' total at the last top
        int64_t g_firstQpc = 0;
        uint64_t g_firstTsc = 0;
        gpu::DiscardStatistics g_firstGpu; // the command processor's frames and their thread's time, at the first
        Record g_current;
        std::vector<Record> g_records;
        std::map<uint32_t, Task> g_tasks;
        std::map<uint32_t, int64_t> g_frameTasks; // this frame's, by fiber

        uint64_t ThreadCycles(HANDLE thread)
        {
            ULONG64 cycles = 0;
            QueryThreadCycleTime(thread, &cycles);
            return cycles;
        }

        // The CPU time the game's worker pools have used (every game thread but this one).
        uint64_t WorkerCycles()
        {
            const netplay::GameState* game = netplay::GetGameState();
            if (!game || !game->isGameThread)
                return 0;
            const kernel::GuestThread* self = kernel::CurrentThread();
            uint64_t total = 0;
            kernel::ForEachThread([&](kernel::GuestThread& thread) {
                if (&thread != self && game->isGameThread(thread.StartAddress()) && thread.NativeHandle())
                    total += ThreadCycles(thread.NativeHandle());
            });
            return total;
        }

        bool InWindow()
        {
            const Settings& settings = GetSettings();
            uint64_t now = netplay::Frame();
            if (settings.fight)
            {
                const std::optional<uint64_t> fightFrame = netplay::FightFrame();
                if (!fightFrame)
                    return false;
                now = *fightFrame;
            }
            return now >= settings.first;
        }

        double Microseconds(int64_t ticks)
        {
            return double(ticks) * 1e6 / double(Frequency());
        }

        double Percentile(std::vector<double> values, double fraction)
        {
            if (values.empty())
                return 0.0;
            const size_t index = std::min(values.size() - 1, size_t(fraction * double(values.size() - 1) + 0.5));
            std::nth_element(values.begin(), values.begin() + index, values.end());
            return values[index];
        }

        std::string ChainText(const std::vector<uint32_t>& chain)
        {
            std::string text;
            char part[16];
            for (uint32_t address : chain)
            {
                snprintf(part, sizeof(part), "%s%08X", text.empty() ? "" : " ", address);
                text += part;
            }
            return text;
        }

        // A frame's values, microseconds: the columns of the report.
        struct ColumnSpec
        {
            const char* header; // in the per-frame lines
            const char* name;   // in the statistics
        };
        constexpr ColumnSpec kColumns[] = {
            { "wall", "wall (top to top)" },
            { "bound", "boundary (netplay's)" },
            { "timing", "timing sub_824F4498" },
            { "render", "render begin sub_824F0F78" },
            { "0B20", "sub_824F0B20" },
            { "pads", "pads sub_824F34D8" },
            { "update", "update sub_824F4878" },
            { "after", "after the update (netplay's)" },
            { "end", "end sub_824F4398" },
            { "tasks", "  update: task fibers" },
            { "workers", "  update: worker pools' wait" },
            { "gpuUpd", "  update: GPU waits" },
            { "rest", "  update: the rest" },
            { "gpuEnd", "  end: GPU waits" },
            { "work", "game work (all but waits)" },
            { "cpuMain", "main thread CPU" },
            { "cpuWork", "worker pools' CPU" },
            { "faults", "main thread's memory watch faults" },
            { "faultUs", "  their handler's time" },
            { "d3d", "D3D's calls (all but GPU waits)" },
            { "d3dN", "  calls" },
        };
        constexpr size_t kColumnCount = std::size(kColumns);
        enum Column : size_t
        {
            kWall,
            kUpdateColumn = 6,
            kEndColumn = 8,
            kTasksColumn,
            kWorkersColumn,
            kGpuUpdateColumn,
            kRestColumn,
            kGpuEndColumn,
            kWorkColumn,
            kCpuMainColumn,
            kCpuWorkersColumn,
            kFaultsColumn,
            kFaultTimeColumn,
            kD3DColumn,
            kD3DCallsColumn,
        };
        static_assert(kD3DCallsColumn + 1 == kColumnCount);

        // The cost of timing a D3D call in cycles, inside the interval timed (the hook's extra call and the counter's
        // reads), measured on an empty call.
        void EmptyCall(PPCContext&, uint8_t*) {}
        double TimingCycles()
        {
            static const double cycles = [] {
                void (*volatile function)(PPCContext&, uint8_t*) = EmptyCall;
                PPCContext ctx{};
                uint64_t total = 0;
                constexpr int kRuns = 200000;
                for (int i = 0; i < kRuns; i++)
                {
                    const uint64_t start = __rdtsc();
                    t_main = !t_main;
                    function(ctx, nullptr);
                    t_main = !t_main;
                    total += __rdtsc() - start;
                }
                return double(total) / kRuns;
            }();
            return cycles;
        }

#ifdef FRAME_TIMES_D3D
        constexpr bool kTimeD3D = true;
#else
        constexpr bool kTimeD3D = false; // only the update's four D3D calls are timed: no D3D column
#endif

        std::array<double, kColumnCount> Values(const Record& record, double cyclesPerUs)
        {
            // D3D's GPU waits happen inside its calls.
            const double d3d = kTimeD3D ? (double(record.d3dCycles) - TimingCycles() * record.d3dCalls) / cyclesPerUs +
                    Microseconds(record.d3dTicks) - Microseconds(record.gpuWaitUpdate + record.gpuWaitEnd)
                                        : 0.0;
            int64_t game = 0;
            for (uint32_t part = kTiming; part < kPartCount; part++)
            {
                if (part != kAfterUpdate)
                    game += record.parts[part];
            }
            const int64_t waits = record.workerWait + record.gpuWaitUpdate + record.gpuWaitEnd;
            return { Microseconds(record.wall), Microseconds(record.parts[kBoundary]), Microseconds(record.parts[kTiming]),
                Microseconds(record.parts[kRenderBegin]), Microseconds(record.parts[kFrame0B20]), Microseconds(record.parts[kPads]),
                Microseconds(record.parts[kUpdate]), Microseconds(record.parts[kAfterUpdate]), Microseconds(record.parts[kEnd]),
                Microseconds(record.tasks), Microseconds(record.workerWait), Microseconds(record.gpuWaitUpdate),
                Microseconds(record.parts[kUpdate] - record.tasks - record.workerWait - record.gpuWaitUpdate), Microseconds(record.gpuWaitEnd),
                Microseconds(game - waits), double(record.mainCycles) / cyclesPerUs, double(record.workerCycles) / cyclesPerUs,
                double(record.faults), Microseconds(record.faultTicks), d3d, double(record.d3dCalls) };
        }

        struct Statistics
        {
            double mean = 0, p50 = 0, p95 = 0, max = 0;
        };

        Statistics Summarize(const std::vector<double>& values)
        {
            Statistics statistics;
            if (values.empty())
                return statistics;
            double sum = 0;
            for (double value : values)
            {
                sum += value;
                statistics.max = std::max(statistics.max, value);
            }
            statistics.mean = sum / double(values.size());
            statistics.p50 = Percentile(values, 0.5);
            statistics.p95 = Percentile(values, 0.95);
            return statistics;
        }

        void Report()
        {
            const std::string path = config::DumpPath("frame_times.txt");
            FILE* file = fopen(path.c_str(), "w");
            if (!file)
            {
                LOG_WARN("Frame times: can't write %s", path.c_str());
                return;
            }
            // TSC cycles per microsecond, over the window (QueryThreadCycleTime counts the TSC's).
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double elapsedUs = Microseconds(int64_t(now.QuadPart) - g_firstQpc);
            const double cyclesPerUs = elapsedUs > 0 ? double(__rdtsc() - g_firstTsc) / elapsedUs : 1.0;
            fprintf(file, "Frame times, in microseconds: %zu frames, the TSC at %.0f MHz. Per frame: its number, its fight frame, r if replayed, "
                          "then the columns the statistics below name, and the task fibers' runs.\n",
                g_records.size(), cyclesPerUs);
            fprintf(file, "%8s %6s %1s", "frame", "fight", "r");
            for (const ColumnSpec& column : kColumns)
                fprintf(file, " %7s", column.header);
            fprintf(file, " %5s\n", "runs");
            std::array<std::vector<double>, kColumnCount> first, replayed;
            std::array<std::vector<double>, kUpdateCallCount> firstCalls, replayedCalls;
            for (const Record& record : g_records)
            {
                const std::array<double, kColumnCount> values = Values(record, cyclesPerUs);
                fprintf(file, "%8llu %6lld %1s", (unsigned long long)record.frame, (long long)record.fightFrame, record.replayed ? "r" : "");
                for (size_t i = 0; i < kColumnCount; i++)
                {
                    fprintf(file, " %7.0f", values[i]);
                    (record.replayed ? replayed : first)[i].push_back(values[i]);
                }
                fprintf(file, " %5u\n", record.taskRuns);
                for (size_t i = 0; i < kUpdateCallCount; i++)
                    (record.replayed ? replayedCalls : firstCalls)[i].push_back(Microseconds(record.calls[i]));
            }
            std::string summary;
            const auto statistics = [&](const char* title, const std::array<std::vector<double>, kColumnCount>& set,
                                        const std::array<std::vector<double>, kUpdateCallCount>& calls) {
                if (set[0].empty())
                    return;
                fprintf(file, "\n%s, over %zu frames: %10s %10s %10s %10s\n", title, set[0].size(), "mean", "p50", "p95", "max");
                std::array<double, kColumnCount> means{};
                for (size_t i = 0; i < kColumnCount; i++)
                {
                    const Statistics column = Summarize(set[i]);
                    means[i] = column.mean;
                    fprintf(file, "  %-32s %10.1f %10.1f %10.1f %10.1f\n", kColumns[i].name, column.mean, column.p50, column.p95, column.max);
                }
                fprintf(file, "  The update's calls, by mean:\n");
                std::vector<std::pair<Statistics, size_t>> rows;
                for (size_t i = 0; i < kUpdateCallCount; i++)
                    rows.emplace_back(Summarize(calls[i]), i);
                std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first.mean > b.first.mean; });
                for (const auto& [call, i] : rows)
                {
                    if (call.max == 0)
                        continue;
                    char name[64];
                    snprintf(name, sizeof(name), "sub_%08X %s", kUpdateCalls[i].function, kUpdateCalls[i].what);
                    fprintf(file, "    %-42s %8.1f %10.1f %10.1f %10.1f\n", name, call.mean, call.p50, call.p95, call.max);
                }
                char d3d[64] = "";
                if (kTimeD3D)
                    snprintf(d3d, sizeof(d3d), "D3D %.0f in %.0f calls, ", means[kD3DColumn], means[kD3DCallsColumn]);
                char line[512];
                snprintf(line, sizeof(line),
                    "%s%s (%zu): wall %.0f, update %.0f (tasks %.0f, workers' wait %.0f, GPU waits %.0f), end %.0f (GPU waits %.0f), game work %.0f "
                    "(%s%.0f memory watch faults %.0f), main thread CPU %.0f, workers' CPU %.0f us",
                    summary.empty() ? "" : "; ", title, set[0].size(), means[kWall], means[kUpdateColumn], means[kTasksColumn],
                    means[kWorkersColumn], means[kGpuUpdateColumn], means[kEndColumn], means[kGpuEndColumn], means[kWorkColumn], d3d,
                    means[kFaultsColumn], means[kFaultTimeColumn], means[kCpuMainColumn], means[kCpuWorkersColumn]);
                summary += line;
            };
            statistics("Frames run the first time", first, firstCalls);
            statistics("Replayed frames", replayed, replayedCalls);
            // The command processor's thread over the window (it runs a frame or two behind): its CPU time a frame drawn,
            // and a frame discarded (replayed).
            const gpu::DiscardStatistics gpu = gpu::GetDiscardStatistics();
            const uint64_t drawn = gpu.drawnFrames - g_firstGpu.drawnFrames, discarded = gpu.frames - g_firstGpu.frames;
            char cp[160];
            snprintf(cp, sizeof(cp), "%.0f us a frame drawn (%llu), %.0f us a frame discarded (%llu)",
                drawn ? (gpu.drawnMicroseconds - g_firstGpu.drawnMicroseconds) / double(drawn) : 0.0, (unsigned long long)drawn,
                discarded ? (gpu.discardedMicroseconds - g_firstGpu.discardedMicroseconds) / double(discarded) : 0.0, (unsigned long long)discarded);
            fprintf(file, "\nThe command processor's thread, CPU time: %s\n", cp);
            summary += std::string("; the command processor's thread ") + cp;
            fprintf(file, "\nTasks (the task fibers by record): microseconds a frame, over the frames they ran in, and where they first yielded:\n");
            std::vector<std::pair<uint32_t, const Task*>> tasks;
            for (const auto& [fiber, task] : g_tasks)
                tasks.emplace_back(fiber, &task);
            std::sort(tasks.begin(), tasks.end(), [](const auto& a, const auto& b) { return a.second->ticks > b.second->ticks; });
            for (const auto& [fiber, task] : tasks)
            {
                fprintf(file, "  %08X: %8.1f us in %u frames", fiber, task->frames ? Microseconds(int64_t(task->ticks)) / task->frames : 0.0,
                    task->frames);
                if (task->replayFrames)
                    fprintf(file, ", replayed %8.1f us in %u", Microseconds(int64_t(task->replayTicks)) / task->replayFrames, task->replayFrames);
                fprintf(file, "; yields at %08X; %s\n", task->yieldSite, ChainText(task->chain).c_str());
            }
            fclose(file);
            LOG_INFO("Frame times: %s (means); details in %s", summary.c_str(), path.c_str());
        }

        // A call from the update, timed.
        void AddCall(size_t index, int64_t ticks)
        {
            g_current.calls[index] += ticks;
        }
    }

    bool Configured()
    {
        return GetSettings().set;
    }

    bool Recording()
    {
        return g_recording;
    }

    int64_t Now()
    {
        LARGE_INTEGER value;
        QueryPerformanceCounter(&value);
        return int64_t(value.QuadPart);
    }

    bool OnMainThread()
    {
        return t_main;
    }

    void BeginFrame()
    {
        if (!Configured() || g_done)
            return;
        t_main = true;
        const int64_t now = Now();
        if (g_recording)
        {
            const uint64_t workers = WorkerCycles();
            g_current.wall = now - g_top;
            g_current.workerCycles = workers - g_workerCycles;
            g_workerCycles = workers;
            g_records.push_back(g_current);
            if (g_records.size() >= GetSettings().count)
            {
                g_recording = false;
                g_done = true;
                Report();
                return;
            }
        }
        g_top = now;
        g_topCycles = ThreadCycles(GetCurrentThread());
        g_topFaults = gpu::watch::ThreadFaults();
        g_part = kBoundary;
        g_partStart = now;
    }

    void Enter(Part part)
    {
        if (!Configured() || g_done)
            return;
        const int64_t now = Now();
        if (part == kTiming)
        {
            // After the frame boundary, which counts the frame: whether this one is recorded. The first's worker pools'
            // CPU time counts from here.
            const bool wasRecording = g_recording;
            g_recording = InWindow();
            if (g_recording)
            {
                if (!wasRecording)
                    g_workerCycles = WorkerCycles();
                if (!g_firstQpc)
                {
                    g_firstQpc = now;
                    g_firstTsc = __rdtsc();
                    g_firstGpu = gpu::GetDiscardStatistics();
                }
                g_current = Record{};
                g_current.frame = netplay::Frame();
                const std::optional<uint64_t> fightFrame = netplay::FightFrame();
                g_current.fightFrame = fightFrame ? int64_t(*fightFrame) : -1;
                g_current.replayed = netplay::replay::Active();
                g_current.parts[kBoundary] = now - g_top;
                g_frameTasks.clear();
            }
        }
        else if (g_recording)
            g_current.parts[g_part] = now - g_partStart;
        g_part = part;
        g_partStart = now;
    }

    void EndFrame()
    {
        if (!g_recording)
            return;
        g_current.parts[g_part] = Now() - g_partStart;
        g_current.mainCycles = ThreadCycles(GetCurrentThread()) - g_topCycles;
        const gpu::watch::FaultStatistics faults = gpu::watch::ThreadFaults();
        g_current.faults = faults.faults - g_topFaults.faults;
        g_current.faultTicks = faults.handlerTicks - g_topFaults.handlerTicks;
        for (const auto& [fiber, ticks] : g_frameTasks)
        {
            Task& task = g_tasks[fiber];
            (g_current.replayed ? task.replayTicks : task.ticks) += uint64_t(ticks);
            (g_current.replayed ? task.replayFrames : task.frames)++;
        }
    }

    void AddTask(uint32_t fiber, uint32_t yieldSite, uint32_t r1, int64_t ticks)
    {
        if (!g_recording)
            return;
        g_current.tasks += ticks;
        g_current.taskRuns++;
        g_frameTasks[fiber] += ticks;
        Task& task = g_tasks[fiber];
        if (task.chain.empty())
        {
            task.yieldSite = yieldSite;
            task.chain = guest::CallChain(r1, 4);
        }
    }

    void AddWorkerWait(int64_t ticks)
    {
        if (g_recording)
            g_current.workerWait += ticks;
    }

    void AddGpuWait(int64_t ticks)
    {
        if (!g_recording)
            return;
        (g_part == kEnd ? g_current.gpuWaitEnd : g_current.gpuWaitUpdate) += ticks;
    }

    namespace detail
    {
        constexpr uint32_t kD3DStart = 0x8274C280, kD3DEnd = 0x8276B5F0; // D3D's code
        thread_local int t_d3dDepth = 0;                                // inside a timed D3D call

        constexpr bool InD3D(uint32_t address)
        {
            return address >= kD3DStart && address < kD3DEnd;
        }

        // Index of a function in kUpdateCalls (compile time).
        constexpr size_t UpdateCallIndex(uint32_t function)
        {
            for (size_t i = 0; i < kUpdateCallCount; i++)
            {
                if (kUpdateCalls[i].function == function)
                    return i;
            }
            return kUpdateCallCount;
        }

        // A call of `function` (at guest address `address`): timed as one of the update's calls if it's one and called
        // from the update, and as D3D's CPU work if it's D3D's and called from outside D3D's code. D3D's are many (about
        // 19,000 a frame), so they're timed in TSC cycles, with the timing's own cost subtracted in the report.
        template <uint32_t Address, size_t Index>
        void Time(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*))
        {
            const uint32_t lr = uint32_t(ctx.lr);
            const bool update = Index < kUpdateCallCount && g_recording && lr > kUpdateStart && lr <= kUpdateEnd && t_main;
            const bool d3d = InD3D(Address) && g_recording && !InD3D(lr) && !t_d3dDepth && t_main;
            if (!update && !d3d)
            {
                function(ctx, base);
                return;
            }
            if (!update)
            {
                const uint64_t start = __rdtsc();
                t_d3dDepth++;
                function(ctx, base);
                t_d3dDepth--;
                g_current.d3dCycles += __rdtsc() - start;
                g_current.d3dCalls++;
                return;
            }
            const int64_t start = Now();
            t_d3dDepth += d3d;
            function(ctx, base);
            t_d3dDepth -= d3d;
            const int64_t ticks = Now() - start;
            if constexpr (Index < kUpdateCallCount)
                AddCall(Index, ticks);
            if (d3d)
            {
                g_current.d3dTicks += ticks;
                g_current.d3dCalls++;
            }
        }
    }
}

// The update's calls, timed when called from it, and D3D's entry points, timed when called from outside D3D (with the
// D3D calls among the update's both ways).
#define TIMED_CALL(address)                                                                                                     \
    extern "C" PPC_FUNC(__imp__sub_##address);                                                                                  \
    PPC_FUNC(sub_##address)                                                                                                     \
    {                                                                                                                           \
        game::frametimes::detail::Time<0x##address, game::frametimes::detail::UpdateCallIndex(0x##address)>(ctx, base,        \
            __imp__sub_##address);                                                                                              \
    }
#define TIMED_UPDATE_CALL(address) TIMED_CALL(address)
#define TIMED_D3D(address) TIMED_CALL(address)

TIMED_UPDATE_CALL(824F31E8)
TIMED_UPDATE_CALL(82401CD8)
TIMED_UPDATE_CALL(823FBE18)
TIMED_UPDATE_CALL(824F4CE8)
TIMED_UPDATE_CALL(82566430)
TIMED_UPDATE_CALL(82354DB0)
TIMED_UPDATE_CALL(82542BF8)
TIMED_UPDATE_CALL(825413D0)
TIMED_UPDATE_CALL(82541538)
TIMED_UPDATE_CALL(8231DCD8)
TIMED_UPDATE_CALL(8245E580)
TIMED_UPDATE_CALL(82566818)
TIMED_UPDATE_CALL(823EACB8)
TIMED_UPDATE_CALL(824B2F78)
TIMED_UPDATE_CALL(824F4E90)
TIMED_UPDATE_CALL(82566B80)
TIMED_UPDATE_CALL(824B2EE0)
TIMED_UPDATE_CALL(824F0F00)
TIMED_UPDATE_CALL(82566920)
TIMED_UPDATE_CALL(82354EB8)
TIMED_UPDATE_CALL(82354F30)
TIMED_UPDATE_CALL(8231D750)
TIMED_UPDATE_CALL(824B2DC8)
TIMED_UPDATE_CALL(823B43D0)
TIMED_UPDATE_CALL(8231D248)
TIMED_UPDATE_CALL(82449EA8)
TIMED_UPDATE_CALL(8244B690)
TIMED_UPDATE_CALL(8247F388)
TIMED_UPDATE_CALL(82453430)
TIMED_UPDATE_CALL(82308A10)
TIMED_UPDATE_CALL(824F10D8)
TIMED_UPDATE_CALL(8242B778)
TIMED_UPDATE_CALL(8242A5D8)
TIMED_UPDATE_CALL(82732890)
TIMED_UPDATE_CALL(823F8A10)
TIMED_UPDATE_CALL(824519A0)
TIMED_UPDATE_CALL(827655C0)
TIMED_UPDATE_CALL(8274FDD8)
TIMED_UPDATE_CALL(8277A248)
TIMED_UPDATE_CALL(8277A308)
TIMED_UPDATE_CALL(82758A48)
TIMED_UPDATE_CALL(82758DB0)
TIMED_UPDATE_CALL(82472ED8)
TIMED_UPDATE_CALL(82451F20)
TIMED_UPDATE_CALL(824524A8)
TIMED_UPDATE_CALL(8245D718)
TIMED_UPDATE_CALL(82354678)
TIMED_UPDATE_CALL(823B4BB8)
TIMED_UPDATE_CALL(82475648)
TIMED_UPDATE_CALL(824F3E68)
TIMED_UPDATE_CALL(823886D8)
TIMED_UPDATE_CALL(82732950)
TIMED_UPDATE_CALL(82355B68)

// D3D's entry points the game's code calls (but those among the update's calls above, and those hooked elsewhere: the frame
// time's sub_82765FE8 and sub_82765EF8, netplay_state.cpp's sub_82751E10), about 19,000 calls a fight frame. Timing them costs about 10 ns a call while
// recording, and their hooks a nanosecond or two otherwise, so they're built only with FRAME_TIMES_D3D defined (cmake
// -DCMAKE_CXX_FLAGS=-DFRAME_TIMES_D3D, in a build directory of its own).
#ifdef FRAME_TIMES_D3D
TIMED_D3D(8274C280) TIMED_D3D(8274C558) TIMED_D3D(8274C840) TIMED_D3D(8274C930) TIMED_D3D(8274C998) TIMED_D3D(8274CA18)
TIMED_D3D(8274CA80) TIMED_D3D(8274CB30) TIMED_D3D(8274CBC8) TIMED_D3D(8274CC50) TIMED_D3D(8274D1B8) TIMED_D3D(8274D250)
TIMED_D3D(8274D768) TIMED_D3D(8274DC60) TIMED_D3D(8274DE88) TIMED_D3D(8274E1D0) TIMED_D3D(8274E1E0) TIMED_D3D(8274E1E8)
TIMED_D3D(8274E290) TIMED_D3D(8274E2A8) TIMED_D3D(8274E2C0) TIMED_D3D(8274E368) TIMED_D3D(8274E378) TIMED_D3D(8274E380)
TIMED_D3D(8274E410) TIMED_D3D(8274E588) TIMED_D3D(8274E590) TIMED_D3D(8274E6B0) TIMED_D3D(8274E9D8) TIMED_D3D(8274EA30)
TIMED_D3D(8274EA58) TIMED_D3D(8274EAE8) TIMED_D3D(8274EB08) TIMED_D3D(8274EB20) TIMED_D3D(8274F0C0) TIMED_D3D(8274F7C8)
TIMED_D3D(82750AC8) TIMED_D3D(82750D68) TIMED_D3D(82750FA8) TIMED_D3D(82751248) TIMED_D3D(82751390) TIMED_D3D(827514D8)
TIMED_D3D(82751530) TIMED_D3D(82751588) TIMED_D3D(827515D8) TIMED_D3D(82751628) TIMED_D3D(827516E0) TIMED_D3D(82751798)
TIMED_D3D(827517E8) TIMED_D3D(827518D8) TIMED_D3D(82751AF0) TIMED_D3D(82751BC8) TIMED_D3D(82751E90)
TIMED_D3D(82751F40) TIMED_D3D(82751F48) TIMED_D3D(82752058) TIMED_D3D(827520B0) TIMED_D3D(827520B8) TIMED_D3D(827520C8)
TIMED_D3D(82752130) TIMED_D3D(827521A0) TIMED_D3D(82752240) TIMED_D3D(82752300) TIMED_D3D(82752380) TIMED_D3D(827523C8)
TIMED_D3D(82752420) TIMED_D3D(82752478) TIMED_D3D(82752578) TIMED_D3D(82752580) TIMED_D3D(82752668) TIMED_D3D(82752670)
TIMED_D3D(82752678) TIMED_D3D(82752708) TIMED_D3D(82752710) TIMED_D3D(82752748) TIMED_D3D(82752770) TIMED_D3D(82752858)
TIMED_D3D(82752860) TIMED_D3D(82752868) TIMED_D3D(827556E0) TIMED_D3D(82755C88) TIMED_D3D(82756020) TIMED_D3D(827577D0)
TIMED_D3D(82758738) TIMED_D3D(82758800) TIMED_D3D(82758F80) TIMED_D3D(827591A8) TIMED_D3D(82759FC0) TIMED_D3D(8275A1A0)
TIMED_D3D(8275A9B8) TIMED_D3D(8275AA10) TIMED_D3D(8275AAA8) TIMED_D3D(8275BE70) TIMED_D3D(8275BFB0) TIMED_D3D(8275C6A0)
TIMED_D3D(8275F188) TIMED_D3D(82765658) TIMED_D3D(82765790) TIMED_D3D(82765AE0) TIMED_D3D(827660D8) TIMED_D3D(82766148)
TIMED_D3D(82766248) TIMED_D3D(827662C0) TIMED_D3D(82766328) TIMED_D3D(82766390) TIMED_D3D(827674E8) TIMED_D3D(827677F8)
TIMED_D3D(827678C0) TIMED_D3D(82767938) TIMED_D3D(82767AA0) TIMED_D3D(82767CD8) TIMED_D3D(82767E48) TIMED_D3D(82767F20)
TIMED_D3D(82767F80) TIMED_D3D(82768238) TIMED_D3D(82768378) TIMED_D3D(827685D0) TIMED_D3D(827686A0) TIMED_D3D(82768738)
TIMED_D3D(827687A8) TIMED_D3D(82768800) TIMED_D3D(82768878) TIMED_D3D(82768940) TIMED_D3D(82768A08) TIMED_D3D(82768CC0)
TIMED_D3D(82768E88) TIMED_D3D(82769020) TIMED_D3D(827694F8) TIMED_D3D(82769578) TIMED_D3D(827696D8) TIMED_D3D(82769768)
TIMED_D3D(82769C88) TIMED_D3D(82769D90) TIMED_D3D(82769E08) TIMED_D3D(82769EA8) TIMED_D3D(82769F58) TIMED_D3D(8276A060)
TIMED_D3D(8276A108) TIMED_D3D(8276A190) TIMED_D3D(8276A248) TIMED_D3D(8276A438) TIMED_D3D(8276A508) TIMED_D3D(8276A648)
TIMED_D3D(8276A6F0) 
#endif
