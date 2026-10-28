# Dead or Alive 4 for Windows

A recompilation of the Xbox 360 game Dead or Alive 4 into a Windows program. The game's PowerPC code is translated to
C++ by [XenonRecomp](https://github.com/tarball/XenonRecomp) (a fork of it, branch `doa4`), and runs on a runtime of its
own: the console's kernel and system calls, a Direct3D 12 renderer for its graphics, its XMA audio and movies, and
netplay for Versus.

**None of the game's files are here or in the releases, not even its recompiled code.** You need your own disc image of
Dead or Alive 4 (Rev 1) (USA, Europe), an `.iso` file. The first time a release starts, its launcher builds the game's
code from your disc image, on your PC; the game then runs from the disc image, which is only read, never changed.

## Playing

Download the zip of the latest release from [the releases](https://github.com/tarball/doa4-recomp-releases/releases),
extract it to a folder of its own that you can change (such as one in Documents), and start `DOA4 Launcher.exe`.
[INSTALL.md](INSTALL.md) has what it needs (64-bit Windows 10 or 11, a processor with AVX, a graphics card with
Direct3D 12), the controls and settings, netplay and updating.

Netplay plays Versus with another PC, over the internet or a local network. Only play with people you trust, on a
network you trust: netplay hasn't had a security review yet.

## Building from source

[INSTALL.md's Building from source](INSTALL.md#building-from-source): with Visual Studio's C++ and Clang tools, Python
and Git, `setup.ps1` checks your disc image, recompiles the game's code and builds the game.

## This repository

Each commit is a release, tagged by its date (`20261027`; a second release on the same day is `20261027.2`), and the
release of the same name has its zip. Its notes name the XenonRecomp commit it was built with.

## Licenses

A release's `licenses` folder has the licenses of what it ships and what's compiled into it: LLVM's clang and lld-link
(with libxml2's notice), XenonRecomp (with GNU binutils' PowerPC disassembler, GPL-2.0 or later, and libmspack,
LGPL-2.1), FFmpeg (LGPL), simde, xxHash, fmt, toml++, tiny-AES-c and TinySHA1. This project's own license is still to
be chosen.

This project isn't affiliated with or endorsed by Koei Tecmo, Team NINJA or Microsoft.
