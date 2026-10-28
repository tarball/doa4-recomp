# Builds a release for players: build\package\doa4-recomp\ and a zip of it (ANALYSIS.md, Phase 5, Packaging, The
# launcher). Neither the game's files nor its recompiled code are in it: the launcher builds the code on the player's PC
# from their own disc image, with the toolchain and the recompiler's inputs here, the first time it starts.
#   DOA4 Launcher.exe                      what players start (runtime/launcher/)
#   doa4.exe and FFmpeg's DLLs             the game's executable (Release), without the game's code, and its audio decoder
#   toolchain\bin\clang.exe, lld-link.exe  LLVM's own build (third_party\llvm, tools\getllvm.ps1)
#   toolchain\lib\clang\<v>\include        clang's own headers (its resource directory)
#   toolchain\XenonRecomp.exe              from XENONRECOMP_ROOT's build
#   recomp\config\                         DOA4.toml and the two files it names (switch tables, registers kept)
#   recomp\include\ppc_context.h           XenonRecomp's, which it copies into the generated code
#   recomp\include\ppc_overrides.h, module\ (the shims), simde\ (its headers the generated code includes)
#   recomp\src\module.cpp, module.h        the module's own source
#   recomp\compile.rsp, link.rsp           the flags
#   recomp\doa4.def, doa4_ppc.def          what doa4.exe and the module import from each other (the Release build's)
#   recomp\doa4_ppc.sha256                 the Release build's module's SHA-256, which every PC's build must give
#   README.txt, licenses\
# It stops if anything else is in it. tools\buildmodule.ps1 -Root build\package\doa4-recomp is the player's build by
# hand, for a dry run.
# Usage: powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-Version <name>] [-NoBuild] [-NoZip]
#   -Version names the release (default: git describe): a release's is its tag in the release repository, its date and
#   commit (v2026.10.27-e3a1ced), which the README's source link names. -NoBuild packages the Release build as it is.
param(
    [string]$Version = '',
    [switch]$NoBuild,
    [switch]$NoZip
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# The game: CMakeLists.txt's GAME_TITLE, GAME_VERSION, GAME_REGION, GAME_PREFIX, the executable and the launcher
# (GAME_LAUNCHER).
$title = 'Dead or Alive 4'
$gameVersion = 'Rev 1'
$gameRegion = 'USA, Europe'
$prefix = 'DOA4'
$executable = 'doa4'
$launcher = "$prefix Launcher.exe"
$folderName = 'doa4-recomp'
# The release repository, a commit and a tag a release, and the XenonRecomp fork.
$sourceRepository = 'https://github.com/tarball/doa4-recomp-releases'
$xenonRepository = 'https://github.com/tarball/XenonRecomp'

# XenonRecomp: XENONRECOMP_ROOT, else beside this checkout, else (in a worktree) beside the main checkout, as env.cmd.
$xenonRecomp = $env:XENONRECOMP_ROOT
if (-not $xenonRecomp) {
    $xenonRecomp = Join-Path (Split-Path $root -Parent) 'XenonRecomp'
    if (-not (Test-Path (Join-Path $xenonRecomp 'thirdparty\simde'))) {
        $common = (& git -C $root rev-parse --path-format=absolute --git-common-dir).Trim()
        $xenonRecomp = Join-Path (Split-Path (Split-Path $common -Parent) -Parent) 'XenonRecomp'
    }
}
$recompiler = Join-Path $xenonRecomp 'out\build\x64-Clang-Release\XenonRecomp\XenonRecomp.exe'
if (-not (Test-Path $recompiler)) { throw "No XenonRecomp.exe in $xenonRecomp (set XENONRECOMP_ROOT)" }
$ffmpeg = Join-Path $root 'third_party\ffmpeg'
if (-not (Test-Path (Join-Path $ffmpeg 'LICENSE.txt'))) { throw "FFmpeg isn't in $ffmpeg (run tools\getffmpeg.ps1): without it the game has no XMA audio." }
# LLVM's own build: Visual Studio's license doesn't let a release ship its clang and lld-link.
$llvm = Join-Path $root 'third_party\llvm'
if (-not (Test-Path (Join-Path $llvm 'bin\clang.exe')) -or -not (Test-Path (Join-Path $llvm 'bin\lld-link.exe'))) {
    throw "LLVM isn't in $llvm (run tools\getllvm.ps1): a release ships LLVM's own clang and lld-link, not Visual Studio's."
}
$resource = Get-ChildItem (Join-Path $llvm 'lib\clang') -Directory | Select-Object -First 1

if (-not $NoBuild) {
    $env:XENONRECOMP_ROOT = $xenonRecomp
    & cmd.exe /c call (Join-Path $root 'build.cmd') Release
    if ($LASTEXITCODE) { throw 'The build failed.' }
}
$build = Join-Path $root 'build\Release'
foreach ($file in "$executable.exe", $launcher, "$($executable)_ppc.dll", "module\$executable.def", "module\$($executable)_ppc.def") {
    if (-not (Test-Path (Join-Path $build $file))) { throw "No $file in ${build}: build.cmd Release first" }
}
# The commit (or a version tag), not the release/<name> tags that record a release's commit, whose slash a file name
# can't have.
if (-not $Version) { $Version = (& git -C $root describe --always --dirty --tags --exclude 'release/*').Trim() }
$xenonCommit = (& git -C $xenonRecomp rev-parse HEAD).Trim()
# A release's source is its tag in the release repository.
$source = if ($Version -match '^v\d{4}\.\d{2}\.\d{2}-[0-9a-f]{7,}$') { "$sourceRepository/tree/$Version" } else { $sourceRepository }

$packages = Join-Path $root 'build\package'
$package = Join-Path $packages $folderName
if (Test-Path $package) { Remove-Item -Recurse -Force $package }
$recomp = Join-Path $package 'recomp'
$licenses = Join-Path $package 'licenses'
foreach ($dir in 'toolchain\bin', "toolchain\lib\clang\$($resource.Name)", 'recomp\config', 'recomp\include\module', 'recomp\include\simde\x86',
    'recomp\src', 'licenses') {
    New-Item -ItemType Directory -Force (Join-Path $package $dir) | Out-Null
}

# The launcher, the game's executable and FFmpeg's DLLs (the Release build's, but its module).
$dlls = @(Get-ChildItem (Join-Path $build '*.dll') | Where-Object { $_.Name -ne "$($executable)_ppc.dll" })
Copy-Item (Join-Path $build $launcher), (Join-Path $build "$executable.exe") $package
$dlls | Copy-Item -Destination $package
# The toolchain.
Copy-Item (Join-Path $llvm 'bin\clang.exe'), (Join-Path $llvm 'bin\lld-link.exe') (Join-Path $package 'toolchain\bin')
Copy-Item -Recurse (Join-Path $resource.FullName 'include') (Join-Path $package "toolchain\lib\clang\$($resource.Name)")
Copy-Item $recompiler (Join-Path $package 'toolchain')
# The recompiler's inputs and the module's own source, flags and imports.
Copy-Item (Join-Path $root "config\$prefix.toml"), (Join-Path $root "config\$($prefix)_switch_tables.toml"),
    (Join-Path $root "config\$($prefix)_keep_registers.toml") (Join-Path $recomp 'config')
Copy-Item (Join-Path $xenonRecomp 'XenonUtils\ppc_context.h') (Join-Path $recomp 'include')
Copy-Item (Join-Path $root 'runtime\ppc_overrides.h') (Join-Path $recomp 'include')
Copy-Item (Join-Path $root 'runtime\module\include\*') (Join-Path $recomp 'include\module')
Copy-Item (Join-Path $xenonRecomp 'thirdparty\simde\*.h') (Join-Path $recomp 'include\simde')
Copy-Item (Join-Path $xenonRecomp 'thirdparty\simde\x86\*.h') (Join-Path $recomp 'include\simde\x86')
Copy-Item (Join-Path $root 'runtime\module\module.cpp'), (Join-Path $root 'runtime\module\module.h') (Join-Path $recomp 'src')
Copy-Item (Join-Path $root 'runtime\module\compile.rsp'), (Join-Path $root 'runtime\module\link.rsp') $recomp
Copy-Item (Join-Path $build "module\$executable.def"), (Join-Path $build "module\$($executable)_ppc.def") $recomp
$moduleHash = (Get-FileHash (Join-Path $build "$($executable)_ppc.dll") -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content (Join-Path $recomp "$($executable)_ppc.sha256") $moduleHash -Encoding ASCII

# Licenses: this project's, the GPL-3.0 (LICENSE), and those of what's shipped or compiled in. In doa4.exe: xxHash
# (BSD-2), and parts that follow Xenia's (BSD-3-Clause); in the module the player builds: XenonRecomp's
# ppc_context.h (MIT) and simde (MIT); beside it: FFmpeg (LGPL), LLVM's clang and lld-link (Apache 2.0 with the LLVM
# exception), and XenonRecomp.exe (MIT), with what's compiled into it: its PowerPC disassembler, binutils' (GPL-2.0 or
# later), libmspack's LZX decoder (LGPL-2.1), Xenia's XEX patcher and export tables (BSD-3-Clause:
# tools\licenses\xenia.txt), fmt and toml++ (MIT), tiny-AES-c (public domain), TinySHA1 and xxHash. And lld-link.exe
# holds libxml2 2.9.12 (MIT, for merging manifests): tools\licenses\libxml2.txt.
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $licenses "$folderName (GPL-3.0).txt")
$third = Join-Path $xenonRecomp 'thirdparty'
Copy-Item (Join-Path $xenonRecomp 'LICENSE.md') (Join-Path $licenses 'XenonRecomp.txt')
Copy-Item (Join-Path $third 'simde\COPYING') (Join-Path $licenses 'simde.txt')
Copy-Item (Join-Path $third 'xxHash\LICENSE') (Join-Path $licenses 'xxHash.txt')
Copy-Item (Join-Path $third 'fmt\LICENSE') (Join-Path $licenses 'fmt.txt')
Copy-Item (Join-Path $third 'tomlplusplus\LICENSE') (Join-Path $licenses 'toml++.txt')
Copy-Item (Join-Path $third 'tiny-AES-c\unlicense.txt') (Join-Path $licenses 'tiny-AES-c.txt')
Copy-Item (Join-Path $third 'libmspack\libmspack\COPYING.LIB') (Join-Path $licenses 'libmspack.txt')
$sha1 = Get-Content (Join-Path $third 'TinySHA1\TinySHA1.hpp') -TotalCount 24
Set-Content (Join-Path $licenses 'TinySHA1.txt') ($sha1 -join "`r`n") -Encoding ASCII
$binutils = (Get-Content (Join-Path $third 'disasm\ppc-dis.c') -TotalCount 20) -join "`r`n"
Set-Content (Join-Path $licenses 'binutils (XenonRecomp''s PowerPC disassembler).txt') -Encoding ASCII ("XenonRecomp.exe's PowerPC " +
    "disassembler is from GNU binutils, under the GNU General Public License, version 2 or later (GPL-2.0.txt). Its source, " +
    "and XenonRecomp's, is $xenonRepository at commit $xenonCommit (thirdparty\disasm). Its notice:`r`n`r`n$binutils")
Copy-Item (Join-Path $root 'tools\licenses\GPL-2.0.txt') (Join-Path $licenses 'GPL-2.0.txt')
Copy-Item (Join-Path $root 'tools\licenses\libxml2.txt') (Join-Path $licenses 'libxml2.txt')
Copy-Item (Join-Path $root 'tools\licenses\xenia.txt') (Join-Path $licenses 'Xenia.txt')
Get-ChildItem (Join-Path $llvm 'LICENSE-*.TXT') | ForEach-Object { Copy-Item $_.FullName (Join-Path $licenses "LLVM $($_.Name)") }
$ffmpegBuild = [regex]::Match((Get-Content -Raw (Join-Path $root 'tools\getffmpeg.ps1')), "(?m)^\`$name = '([^']+)'").Groups[1].Value
$ffmpegLicense = (Get-Content -Raw (Join-Path $ffmpeg 'LICENSE.txt')).TrimEnd()
Set-Content (Join-Path $licenses 'FFmpeg.txt') -Encoding UTF8 ("The FFmpeg DLLs are BtbN's LGPL build $ffmpegBuild " +
    "(https://github.com/BtbN/FFmpeg-Builds, which has its build scripts), from FFmpeg's source at " +
    "https://git.ffmpeg.org/ffmpeg.git. Their license:`r`n`r`n$ffmpegLicense")

$readme = @"
$title for Windows ($Version)

A recompilation of the Xbox 360 game for Windows. None of the game's files come with it: it needs your own disc image
of $title ($gameVersion) ($gameRegion), which it builds the game's code from and runs from, and never changes.

To play
1. Keep this folder somewhere you can change, such as in Documents (not Program Files).
2. Start "$launcher". Windows may say it doesn't recognize the app, since it isn't signed: choose More info, then
   Run anyway.
3. Choose your disc image of $title ($gameVersion) ($gameRegion), an .iso file. The first time, the launcher
   builds the game's code from it, which can take a few minutes depending on your PC, and about 350 MB of free disk
   space while it runs (38 MB stay). Later starts go straight to playing.
4. Choose how to play, on this PC or netplay, and press Launch.

Netplay plays Versus with another PC. One player hosts on a UDP port (to play over the internet, that port must be
forwarded to the host's PC on its router); the other joins with the host's address and port. Both choose the same
for fights: rollback, the default, which plays better over the internet, or lockstep. Only play with people you
trust, on a network you trust: netplay hasn't had a security review.

It needs 64-bit Windows 10 or 11, a processor with AVX (from 2011 on) and a graphics card with Direct3D 12.

Controls: an Xbox controller, or another controller that works as one (XInput). PlayStation controllers need a tool
that presents them as an Xbox controller, such as DS4Windows. The keyboard works too: arrows or WASD move, Enter is
Start, Backspace is Back, Z, J or Space is A, X or K is B, C or U is X, V or I is Y, Q and E are the shoulder buttons,
and 1 and 3 the triggers.

SETTINGS > VIDEO in the game has the PC's options above the game's own: Fullscreen, Resolution (up to 4 times
1280x720), Anti-Aliasing, Alpha to Coverage and Texture Filtering. Alt+Enter also switches fullscreen.

The data folder the game makes here holds its settings, saves, shader cache and logs ($executable.log, and build.log
from building the game's code). On the first run the game compiles its shaders in the background, which takes a
minute or two: until then parts of a scene can appear a moment late, and the first story intro can stutter.

To update, extract the new release over this one: the launcher builds the game's code again for it, and the data
folder, with your saves and settings, stays.

Source: $source
Its recompiler: $xenonRepository, branch doa4, at commit $xenonCommit
This project is free software under the GNU General Public License, version 3 (licenses\$folderName (GPL-3.0).txt).
The licenses of the rest of what's in this folder are in the licenses folder too.
"@
Set-Content (Join-Path $package 'README.txt') -Encoding ASCII ($readme -replace "`r?`n", "`r`n")

# Only these files: nothing from the game, and not its code, which the player's PC builds.
$expected = @($launcher, "$executable.exe", 'README.txt', 'licenses', 'toolchain', 'recomp') + @($dlls | ForEach-Object Name)
$unexpected = @(Get-ChildItem $package | Where-Object { $expected -notcontains $_.Name })
if ($unexpected.Count) { throw "Unexpected files in the package: $($unexpected.Name -join ', ')" }
$gameDerived = @(Get-ChildItem -Recurse $package | Where-Object {
    $_.Name -like '*_ppc.dll' -or $_.Extension -in '.xex', '.pe', '.iso', '.obj', '.lib', '.pdb' -or
    ($_.PSIsContainer -and $_.Name -in 'ppc', 'obj', 'private', 'data', 'dumps', 'cache', 'content') })
if ($gameDerived.Count) { throw "The package holds what it mustn't: $($gameDerived.FullName -join ', ')" }

$files = @(Get-ChildItem -Recurse -File $package)
'{0}: {1} files, {2:N1} MB' -f $package, $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB)
$files | Group-Object { ($_.FullName.Substring($package.Length + 1) -split '\\')[0] } | ForEach-Object {
    '  {0,-24} {1,5} files {2,8:N1} MB' -f $_.Name, $_.Count, (($_.Group | Measure-Object Length -Sum).Sum / 1MB)
}
"The module every PC builds: SHA-256 $moduleHash"
if ($NoZip) { return }
$zip = Join-Path $packages "$folderName-$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# (ZipFile rather than Compress-Archive, whose entries in Windows PowerShell 5.1 can have backslashes in their paths.)
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($package, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
"Packaged $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
