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
game/            PrinceOfPersia.exe, the playable build made from your disks (gitignored)
work/            extracted resources, files, disassembly (gitignored)
src/gen/         lifted C generated from the game (gitignored)
src/             hand-written glue for this game
  main.c         loader: address space, A5 world, jump table, THINK C startup
  traps.c        PoP's Toolbox layer in front of macrecomp (VBL, resources, ...)
  sound.c        Sound Manager (bufferCmd) on SDL2 audio
  input.c        keyboard: GetKeys, key events, MenuKey
  qd.c           regions, update clipping, CopyBits transfer modes
  cqd.c          colour: 8-bit screen, GDevice, colour ports, CopyBits with PixMaps, Palette Manager
  state.c        savestates
  heap.c         NewHandle/NewPtr/Dispose with reuse of freed memory
  mr/            macrecomp's toolbox/quickdraw/files/dialog .c files, included unchanged,
                 plus the functions that save their internal state
  platform.c     window, display looks and themes, the recomp menu (replaces macrecomp's platform_sdl.c)
  filters.c      AdvMAME and xBR (3x and 4x) for the Remastered looks
  crt.c          the CRT Monitor and CRT TV looks (GPU blend layers)
tools/
  prepare.py     images -> work/ + src/gen/ (also decodes the music driver)
  lift.py        macrecomp's lift68k.py plus a few forms it lacks (incl. 68020 scaled index)
  cross-windows.sh  build pop1.exe / PrinceOfPersia.exe for Windows from Linux
  embed.py       pack work/files into the standalone exe (src/gen/embedded.c)
  make_icon.py   the game's Finder icon -> a Windows .ico
ext/macrecomp/   the toolkit (submodule, unmodified)
DEV-LOG.md       what was done, step by step
```

## Supported version

Built and tested against one release only: the Macintosh *Prince of Persia*
(Brøderbund, 1992) on three 800K floppies, as DiskCopy 4.2 images. Other
releases or dumps may have different code and are not expected to work.
Check your copy against these SHA-256 hashes:

| File | SHA-256 |
|---|---|
| `Prince of Persia (Disk 1 of 3).dc42` | `40ec0ba5ec7578e4b551e38d83b9869e9353f2b85f932a24b20e2b973c9b24c4` |
| `Prince of Persia (Disk 2 of 3).dc42` | `85831460c77859a85bda16ebe46af636fa1924f34d8bc1cdaf79011eb18bda02` |
| `Prince of Persia (Disk 3 of 3).dc42` | `1e2401f94020dafbc38717de41773d162114ddefd1dfbb4ce6fdc52d7a191539` |

On Windows: `certutil -hashfile "Prince of Persia (Disk 1 of 3).dc42" SHA256`;
on Linux/macOS: `sha256sum *.dc42` (or `shasum -a 256`).

This project does not say where to get the game; use your own copy.

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

On Windows, double-click **`game\PrinceOfPersia.exe`** (or `Play.cmd`): one
file, no install. It carries your game files inside (built from your own
disks with `tools/cross-windows.sh --standalone`; keep it for yourself, don't
share it). The `game` folder is **portable**: everything the game writes
stays in it, so it can be copied or moved as a whole:

```
game\
  PrinceOfPersia.exe
  data\        the game files, unpacked on first start
  saves\       savestates
  pop1.cfg     settings
  pop1.log     log
```

(Only if that folder is read-only does it use
`%APPDATA%\pop1-recomp\PrinceOfPersia\` instead.)
The developer build (`tools/cross-windows.sh` without `--standalone`) is
`build\pop1.exe` with a console, reading `work\files\`.

The game opens with the **recomp menu**: press Enter on *Start Game*. **Esc**
or **F1** brings the menu back at any time and pauses the game: resume,
restart the level, save/load state, sound effects and music on/off,
graphics, look, display theme (black-and-white only: Macintosh, Amber,
Phosphor, Parchment, Midnight), size, crop borders (colour only),
scanlines, the controls, quit.
**F11** or **Alt+Enter** toggles fullscreen, **F2** (Shift+F2) switches
to the next (previous) look while you play. Settings are kept in
`pop1.cfg`.

**Look**: *Remastered* (the default) scales the picture 4× and then
smoothly to the largest size that fits, so it fills the screen and stays
sharp. *AdvMAME* smooths the diagonals (still pixel art, no new
colours); *xBR* gives smooth curves and edges. These two run at 3× or 4×,
whichever is closest to the size on your screen, so their work is not
shrunk away. *CRT Monitor* looks like the Apple RGB monitors of the time
(fine red, green and blue stripes, light scanlines, a little glow); *CRT
TV* has stronger scanlines, a shadow mask, a softer picture and more glow.
*Original*
uses whole-number scaling only (1×, 2×, 3×...), the Mac's pixels exactly,
with black borders where the screen has room left.

**Crop Borders** (colour modes, on by default): the colour game only
draws in a band of its screen (640×400 of 640×480; 512×320 in LC) and
leaves the rest black. With this on, only that band is shown, so the
rooms come out about 20% bigger.

**Size**: *Fullscreen*, *Window Auto* (the default: the largest window
that fits your screen) or a fixed window size. Only sizes that fit your
screen are offered. The window can also be resized by dragging.

**Graphics** (left/right, then Enter: the game restarts in that mode):

- **Colour** (default): the Mac II version, 256 colours, 640x480,
  `Persia(COLOR)` from disk 2.
- **LC Colour**: the Macintosh LC version, 256 colours, 512x384,
  `Persia(LC)` from disk 3.
- **B&W**: the original black-and-white version, 512x342, `Persia(BW)`.

**Savestates**: **F5** saves, **F9** loads, in the slot chosen in the menu
(*Save State* / *Load State*, left/right picks slot 1-9; the menu shows when a
slot was saved). A state is taken at the start of the game's next frame and
belongs to the graphics mode it was saved in. Files go to `saves/` next to
`pop1.cfg`.

In the game: arrow keys or the numeric keypad move, **Shift** acts (grab,
climb, careful step, draw the sword). **Ctrl+N** is a new game (Ctrl stands
in for the Mac's Command key, Alt for Option). The picture is the game's
colour mode unless you choose otherwise.

To build the Windows executables on Linux: `tools/cross-windows.sh
[--standalone] <SDL2 MinGW folder>` (see the script).

Debugging knobs (environment variables): `POPMODE=bw|colour|lc` (graphics
mode, overrides `pop1.cfg`), `POPNOMENU=1` (no start menu), `POPTRACE=n`
(first n traps with their stack effect), `POPCQD=n` (colour/palette traps),
`POPEXIT=s` (quit after s seconds), `POPSHOTS=5,10` (+ `POPSHOTDIR`,
screenshots: `.pgm` in B&W, `.ppm` in colour), `POPUISHOT=file.ppm` +
`POPUISHOTAT=s` (what the player sees, menu and dialogs included),
`POPKEYS="^n@8,right@12+2,save1@15,load1@20,menu1000.1@25"` (scripted keys,
savestates and menu picks), `POPPOKE="a5-0x459e=1@2.5"` (write guest memory
at a time; this one starts at the copy-protection level), `POPUPD=1`,
`POPHEAPSTAT=1`, `POPWINSHOT=out.ppm` + `POPWINSHOTAT=s` (the final
window picture at a time), `POPWAV=out.wav` (record audio), `POPSOUND=1`,
`POPNOAUDIO=1`, and macrecomp's own `MRTRACE`, `MRTRAPS`, `MRSHOT`,
`MRWATCHADDR`.
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
- [x] Colour mode (Persia(COLOR), 640x480) and LC colour (Persia(LC), 512x384)
- [x] Savestates (F5 / F9, nine slots)
- [x] Level 1 finishable (played through on Windows)
- [ ] Levels 2 and up tested
- [ ] Sound effects checked, music seams fixed
- [ ] Copy-protection screen checked
