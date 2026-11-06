# pop1-recomp

A learning project: static recompilation of the Macintosh version of
*Prince of Persia* (Brøderbund, 1992, Motorola 68k) into native code,
in the style of XenonRecomp / N64Recomp.

Built on [macrecomp](https://github.com/sp00nznet/macrecomp) (MIT), included
as a git submodule in `ext/macrecomp`: 68k → C lifting plus a Mac Toolbox
runtime on SDL2.

**This repository contains no game data and no code generated from it.**
You need your own copy of the game. Put it in `original/` (gitignored).

## Layout

```
original/        your copy of the game: the three DiskCopy 4.2 images (gitignored)
work/            extracted resources, files, disassembly (gitignored)
src/gen/         lifted C generated from the game (gitignored)
src/             hand-written glue for this game
  main.c         loader: address space, A5 world, jump table, THINK C startup
  traps.c        PoP's Toolbox layer in front of macrecomp (VBL, resources, ...)
  sound.c        Sound Manager (bufferCmd) on SDL2 audio
  input.c        keyboard: GetKeys, key events, MenuKey
  qd.c           regions, update clipping, CopyBits transfer modes
  platform.c     window, display themes, the recomp menu (replaces macrecomp's platform_sdl.c)
tools/
  prepare.py     images -> work/ + src/gen/ (also decodes the music driver)
  lift.py        macrecomp's lift68k.py plus a few 68000 forms it lacks
  cross-windows.sh  build pop1.exe / PrinceOfPersia.exe for Windows from Linux
  embed.py       pack work/files into the standalone exe (src/gen/embedded.c)
  make_icon.py   the game's Finder icon -> a Windows .ico
ext/macrecomp/   the toolkit (submodule, unmodified)
DEV-LOG.md       what was done, step by step
```

## Building

Needs Python 3 (`pip install -r ext/macrecomp/tools/requirements.txt`), CMake,
a C compiler and SDL2. On Windows the easy route is MSYS2's MinGW64 shell:
`pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,SDL2,python,python-pip}`.

```sh
git clone --recursive https://github.com/RufusFrederiks/pop1-recomp
cd pop1-recomp
# put "Prince of Persia (Disk 1 of 3).dc42" (and disks 2, 3) in original/
python tools/prepare.py
cmake -S . -B build -G Ninja
cmake --build build
./build/pop1            # run from the repo root: it reads work/
```

## Playing

On Windows, double-click **`PrinceOfPersia.exe`**: one file, no install. It
carries your game files inside (built from your own disks with
`tools/cross-windows.sh --standalone`; keep it for yourself, don't share it).
On first start it unpacks them into `%APPDATA%\pop1-recomp\PrinceOfPersia\`,
where its settings and a log (`pop1.log`) also live. `Play.cmd` runs the
developer build instead (`build\pop1.exe` with a console, reading `work\files\`).

The game opens with the **recomp menu**: press Enter on *Start Game*. **Esc**
or **F1** brings the menu back at any time and pauses the game: resume,
restart the level, sound effects and music on/off, display theme
(Macintosh, Amber, Phosphor, Parchment, Midnight), window size, fullscreen,
scanlines, the controls, quit. **F11** or **Alt+Enter** toggles fullscreen.
Settings are kept in `pop1.cfg`.

In the game: arrow keys or the numeric keypad move, **Shift** acts (grab,
climb, careful step, draw the sword). **Ctrl+N** is a new game (Ctrl stands
in for the Mac's Command key, Alt for Option). The picture is the game's
black-and-white mode (512x342).

To build the Windows executables on Linux: `tools/cross-windows.sh
[--standalone] <SDL2 MinGW folder>` (see the script).

Debugging knobs (environment variables): `POPTRACE=n` (first n traps with
their stack effect), `POPEXIT=s` (quit after s seconds), `POPSHOTS=5,10`
(+ `POPSHOTDIR`, screenshots), `POPKEYS="^n@8,right@12+2"` (scripted keys),
`POPWAV=out.wav` (record audio), `POPSOUND=1`, `POPNOAUDIO=1`, and
macrecomp's own `MRTRACE`, `MRTRAPS`, `MRSHOT`, `MRWATCHADDR`.
For headless runs: `SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software`.

## Status

- [x] Recon: extract resources, trap census
- [x] Lift 68k → C (CODE 1–4 at 98–100%, music driver at 99.8%)
- [x] Boots
- [x] Intro and title screen (B&W)
- [x] Title music (with audible seams still)
- [x] Level 1 visible
- [x] Prince moves
- [x] Recomp menu (start, pause, sound, music, themes, fullscreen)
- [ ] Level 1 finishable
- [ ] Sound effects checked, music seams fixed
- [ ] Colour mode (Persia(COLOR), Colour QuickDraw)
- [ ] Copy-protection screen checked
