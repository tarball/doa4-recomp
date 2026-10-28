#pragma once

// Builds the game's code module, <executable>_ppc.dll, from the player's disc image with the release's toolchain, as
// tools\buildmodule.ps1 does by hand (ANALYSIS.md, Phase 5, Packaging, The launcher):
//  1. default.xex, as it is on the disc, goes to recomp\private\, where the config names it;
//  2. XenonRecomp (toolchain\) recompiles it into recomp\ppc\ (the config's paths are relative to recomp\config\);
//  3. clang compiles the generated code and the module's own source (recomp\src\module.cpp) into recomp\obj\, the
//     largest files first, as many at once as the logical processors and the free memory allow;
//  4. lld-link makes the executable's import library from recomp\<executable>.def, then links the module, its objects
//     in name order (ordinal), as the development build does;
//  5. the module's SHA-256 must be the release's (recomp\<executable>_ppc.sha256): only then is it moved beside the
//     executable;
//  6. the intermediate files (recomp\private, ppc and obj, about 240 MB, all from the game) are deleted, whatever
//     happened.
// The tools run in recomp\ with relative paths, without console windows, in a job object that ends them with the
// launcher; their output goes to <data>\build.log with each step's time.
#include "launcher/launcher.h"

#include <windows.h>

namespace launcher
{
    class Build
    {
    public:
        Build();
        ~Build();
        Build(const Build&) = delete;
        Build& operator=(const Build&) = delete;

        // Builds the module: kReady once it's built and is the release's, else kFailed (`message` says why, for the
        // player) or kCancelled.
        Readiness Run(const Folder& folder, const Disc& disc, const Reporter& report, std::wstring& message);
        // Stops a build in progress, from any thread: its tools are ended and its files deleted. Until the next Run.
        void Cancel();
        bool Cancelled() const;
        // Clears a cancel, for the next Run (from the thread that runs it).
        void Reset();

    private:
        HANDLE m_cancel = nullptr; // a manual-reset event, set by Cancel
    };

    // Only one launcher builds in a release folder at a time: it holds recomp\build.lock open, without sharing, while it
    // checks and builds (Windows deletes the file when the launcher ends, however it ends).
    class BuildLock
    {
    public:
        BuildLock() = default;
        ~BuildLock();
        BuildLock(const BuildLock&) = delete;
        BuildLock& operator=(const BuildLock&) = delete;

        // False if another launcher holds it (or the folder can't be written: `error` is then set).
        bool TryAcquire(const Folder& folder, unsigned long& error);
        void Release();

    private:
        HANDLE m_file = INVALID_HANDLE_VALUE;
    };

    // Deletes what a build left when it was ended before it could: the intermediate files and a module linked but not
    // checked yet. Call it with the lock held.
    void RemoveLeftovers(const Folder& folder);
}
