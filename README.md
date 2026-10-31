# Dead or Alive 4 for Windows

An unofficial Windows port of the Xbox 360 version of Dead or Alive 4, made by static recompilation with a
[fork](https://github.com/tarball/XenonRecomp) of [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

**This repository doesn't include any of the game's resources. You need your own Dead or Alive 4 (Rev 1) (USA, Europe)
disc image to play.**

## Requirements

- 64-bit Windows 10 or 11
- A processor with AVX (most from 2011 on)
- A graphics card with Direct3D 12
- Your Dead or Alive 4 (Rev 1) (USA, Europe) disc image (.iso)

## How to install

1. Download the latest release's zip from [Releases](https://github.com/tarball/doa4-recomp/releases).
2. Extract it to a folder of its own that you can write to, such as one in Documents (not Program Files).
3. Run `DOA4 Launcher.exe`. If Windows says it doesn't recognize the app, choose More info, then Run anyway.
4. Select your Dead or Alive 4 (Rev 1) (USA, Europe) disc image, choose Play or Netplay, then press Launch.

To update, extract a new release over the old one. Your saves and settings are kept.

## Playing

- **Controls:** an Xbox controller or any XInput controller (PlayStation controllers need a tool such as DS4Windows).
  Keyboard: arrows or WASD move, Enter is Start, Backspace is Back, Z/J/Space is A, X/K is B, C/U is X, V/I is Y, Q
  and E are the shoulder buttons, 1 and 3 the triggers.
- **Graphics options:** SETTINGS > VIDEO has Fullscreen, Resolution (up to 4 times 1280x720), Anti-Aliasing, Alpha to
  Coverage and Texture Filtering. Alt+Enter toggles fullscreen.
- **First run:** the game compiles its shaders in the background, so some scenes can appear a moment late.
- Saves, settings and the log (`doa4.log`) are in the `data` folder.

## Netplay

Netplay plays Versus with another PC, over the internet or a local network. One player hosts on a UDP port (to play
over the internet, forward that port to the host's PC on its router); the other joins with the host's address and
port. Both choose the same for fights: Rollback, the default, which plays better over the internet, or Lockstep.

## Building

See [BUILD.md](BUILD.md).

## License

GNU General Public License, version 3: see [LICENSE](LICENSE). Releases include the licenses of the libraries they
ship. This project isn't affiliated with or endorsed by Koei Tecmo, Team NINJA or Microsoft.
