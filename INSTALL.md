# Playing Dead or Alive 4 on Windows

This project recompiles the Xbox 360 version of Dead or Alive 4 into a Windows program. None of the game's files come
with it, not even its recompiled code: you need your own disc image of **Dead or Alive 4 (Rev 1) (USA, Europe)**, which the game's
code is built from, on your PC, the first time you start it, and which the game runs from.

## What you need

- Windows 10 or 11, 64-bit, with a processor that has AVX (from 2011 on) and a graphics card that supports Direct3D 12.
- An Xbox controller, or any XInput controller (a keyboard works too). PlayStation controllers need a tool that presents
  them as an Xbox controller, such as DS4Windows.
- Your disc image of Dead or Alive 4 (Rev 1) (USA, Europe), as an `.iso` file. The launcher checks that it's this
  version.
- About 350 MB of free disk space the first time, while the game's code is built (38 MB of it stays).

## Installing

1. Download the zip of the latest release from https://github.com/tarball/doa4-recomp-releases/releases and extract it
   to a folder of its own that you can change, such as one in Documents (not Program Files).
2. Start `DOA4 Launcher.exe` in that folder. Windows may say it doesn't recognize the app, since it isn't signed: choose
   More info, then Run anyway.
3. Press Choose disc image... and choose your disc image. The launcher checks it and remembers where it is (in
   `data\settings.ini`): the disc image stays where it is, and is only read, never changed. If an `.iso` is already in
   the folder, it's found by itself.
4. The first time, the launcher builds the game's code from the disc image, which can take a few minutes depending on
   your PC (it says how far it's got, and Cancel stops it). It happens once: later starts go straight to playing.
5. Choose how to play, Play (on this PC) or Netplay, and press Launch. The launcher starts the game and closes.

If something goes wrong, the launcher says what, and its log of the build, `data\build.log`, has the details (Show log
opens it). Some antivirus programs block or slow the compiler the build runs (`toolchain\bin\clang.exe`): if the build
can't start a tool, allow the folder in the antivirus program. Windows Security's controlled folder access can keep
the launcher and the game from writing to Documents: allow `DOA4 Launcher.exe` and `doa4.exe` there, or use another
folder.

## Playing

- **Settings:** SETTINGS > VIDEO in the game has the PC's options above the game's own: Fullscreen, Resolution (up to
  4 times 1280x720), Anti-Aliasing, Alpha to Coverage and Texture Filtering. Alt+Enter also switches fullscreen.
- **Keyboard:** arrows or WASD move, Enter is Start, Backspace is Back, Z/J/Space is A, X/K is B, C/U is X, V/I is Y,
  Q and E are the shoulder buttons, 1 and 3 the triggers.
- **Saves and settings** are in the `data` folder next to the launcher, with the logs, `doa4.log` and `build.log`.
- **The first run** compiles the game's shaders in the background, which takes a minute or two: until then parts of a
  scene can appear a moment late, and the first story intro can stutter. Later runs use the cache in `data`.

## Netplay

Netplay plays Versus with another PC: choose Netplay in the launcher.

- **One player hosts:** choose Host and a UDP port (7000 unless you change it). To play over the internet, that port
  must be reachable from it: forward it to the host's PC on the router.
- **The other joins:** choose Join, and type the host's address (an IPv4 address such as 203.0.113.7, or a name that
  has one) and port.
- **Both players choose the same for fights:** Rollback, the default, which plays better over the internet, or
  Lockstep, which waits for both players' moves every frame. Games that don't match don't connect.
- Both start the game. It goes straight into Versus once both have booted; either player can stop by closing the game.
- **Only play with people you trust, on a network you trust:** netplay hasn't had a security review yet.

The launcher remembers these choices (in `data\launcher.ini`).

## Updating

Extract the new release over the old one, then start the launcher: it builds the game's code again for the new
release, once. Your saves and settings are in `data`, which releases don't include, and the disc image stays where it
is.

## Building from source

To build the game yourself rather than download it:

1. Install these free tools:
   - [Visual Studio 2026 Build Tools](https://visualstudio.microsoft.com/downloads/) (or any edition). In the
     installer, choose "Desktop development with C++", and under its optional components also "C++ Clang tools for
     Windows" and "C++ CMake tools for Windows".
   - [Python 3](https://www.python.org/downloads/).
   - [Git](https://git-scm.com/downloads).

   They need about 10 GB, and the build a few GB more.

2. Get this project and its recompiler side by side, in the same folder:

   ```
   git clone https://github.com/tarball/doa4-recomp-releases.git doa4-recomp
   git clone --recursive --branch doa4 https://github.com/tarball/XenonRecomp.git XenonRecomp
   ```

   That's the latest release. Each release is a tag (its date and commit, such as `v2026.10.27-e3a1ced`) whose notes
   name the XenonRecomp commit it was built with: to build an earlier one, `git checkout <tag>` in `doa4-recomp`, and
   that commit in `XenonRecomp` (`git checkout <commit>`, then `git submodule update --recursive`).

3. Run the setup from the `doa4-recomp` folder. It asks for your disc image (or pass `-Iso`):

   ```
   powershell -ExecutionPolicy Bypass -File setup.ps1
   ```

   It checks the tools, extracts and checks the game's executable, recompiles it, offers to download FFmpeg (the
   game's audio decoder, about 90 MB), builds the game and puts it in the `game` folder, with a shortcut. It takes a few
   minutes. `-Destination <folder>` puts the game elsewhere.

4. Play with `game\Dead or Alive 4.lnk`.

The disc image stays where it is: the game reads it from there (its path is in `game\data\settings.ini`). If you move
it next to `doa4.exe`, the game finds it there too. To update, get the new version (`git pull` in both folders) and
run the setup again: it updates the game in place, keeping your saves and settings.
