# tje3-recomp

An unofficial native Windows version of **ToeJam & Earl III: Mission to Earth** (original Xbox, USA), made by static
recompilation. It runs on Vulkan, not in an emulator.

**This project contains none of the game's code or data.** You need your own copy of the game. The setup step
translates and compiles the game's program on your PC, so the result only ever exists on your computer. Please don't
share the resulting `game` folder.

This project isn't affiliated with or endorsed by the game's developers, publishers or rights holders, or by
Microsoft. It was written with AI assistance (Claude).

## What you need

- Windows 10 or 11, 64-bit, with a graphics driver that supports Vulkan
- Your own copy of the USA Xbox release, as an uncompressed `.iso` or a folder of its extracted files
  (`default.xbe` SHA-1 `eb00994059d1b7d357810a1d80c8878e8f14d10b`)
- About 4 GB of free disk space

## Install

1. Download the zip from the [Releases](../../releases/latest) page and unpack the whole thing into a folder of its own.
2. Run `TJE3.exe`. Windows may warn about an unrecognised app: choose **More info**, then **Run anyway**.
3. Choose your disc image or extracted folder. Setup tells you straight away whether it's the right copy.
4. Optional: add folders holding the game's original Xbox Live downloadable content that you already have. Setup
   tells you how many packs it found.
5. Optional: press **Browse** to choose where the game is installed.
6. Press **Install** and wait a few minutes, then press **Play**. Setup can add a desktop shortcut, with an icon taken
   from your disc. After that, `TJE3.exe` or the shortcut starts the game straight away.

Nothing needs installing in Windows, and no admin rights are needed.

## Android (optional)

Setup can also build an app for your own arm64 Android device. Tick the Android box: this downloads Google's Android
NDK (about 750 MB) straight from Google, under Google's own terms, and writes `tje3.apk` and instructions to the
`Android` folder in your install folder. That one file holds the game and your downloadable content, and unpacks itself
on first launch. It's built from your disc, so please don't share it.

## Playing

- Xbox-style controllers work. Keyboard: arrows = d-pad, Enter = Start, Esc = Back, Z/Space = A, X = B, A = X, S = Y,
  Q/W = triggers, C/V = white/black.
- Alt+Enter switches between fullscreen and a window. **Exit Game** is at the bottom of the main menu.
- In the install folder, options are in `tje3.ini` and saved games are in `userdata`.
- To add downloadable content later, copy each pack's folder into `DLC` in the install folder.

## Building from source

The release zip contains everything setup needs. To work on the code, you need LLVM-MinGW, CMake, Ninja, Python with
`capstone`, and the Vulkan SDK:

```
python tools/recomp/build_game.py <path to default.xbe>
```

Run `build/game/tje3.exe --data <folder with your game files>`.

Third-party licences are in the `licenses` folder of the release.
