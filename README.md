# pop1-recomp

[![Build check](https://github.com/RufusFrederiks/pop1-recomp/actions/workflows/build.yml/badge.svg)](https://github.com/RufusFrederiks/pop1-recomp/actions/workflows/build.yml)
(Windows, Linux and macOS: the code compiles. GitHub never has the game, so
this is not a playable build; that you make from your own disks, below.)

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
  pad.c          game controllers (SDL GameController): buttons into the KeyMap, menu, rumble
  qd.c           regions, update clipping, CopyBits transfer modes
  cqd.c          colour: 8-bit screen, GDevice, colour ports, CopyBits with PixMaps, Palette Manager
  state.c        savestates
  heap.c         NewHandle/NewPtr/Dispose with reuse of freed memory
  mr/            macrecomp's toolbox/quickdraw/files/dialog .c files, included unchanged,
                 plus the functions that save their internal state
  platform.c     window, display looks and themes, the recomp menu (replaces macrecomp's platform_sdl.c)
  filters.c      AdvMAME and xBR (3x and 4x) and MMPX (4x) for the Remastered looks
  crt.c          the CRT Monitor and CRT TV looks (GPU blend layers)
  light.c        torch lighting, Classic (reads the room from the game's memory, GPU blend layers)
  gfx.c          torch lighting, Enhanced (OpenGL shaders: shadows, glow, relief, sparks and dust)
tools/
  prepare.py     images -> work/ + src/gen/ (also decodes the music driver)
  lift.py        macrecomp's lift68k.py plus a few forms it lacks (incl. 68020 scaled index)
  cross-windows.sh  build pop1.exe / PrinceOfPersia.exe for Windows from Linux
  build-mac.sh   build "Prince of Persia.app" on a Mac from your disks
  embed.py       pack work/files into the standalone exe (src/gen/embedded.c)
  make_icon.py   the game's Finder icon -> a Windows .ico or a Mac .icns
  extract_sprites.py  every colour sprite and tile -> PNG (+ mask, indexed, sprites.json)
                 in work/sprites/, for upscaling; standard library only
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
On Debian/Ubuntu (also inside WSL on Windows):
`sudo apt install build-essential cmake ninja-build libsdl2-dev python3-pip`.
On Fedora: `sudo dnf install gcc cmake ninja-build SDL2-devel python3-pip`.

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

On **Linux**, build the one-file game after `tools/prepare.py`:

```sh
cmake -S . -B build-game -G Ninja -DPOP1_STANDALONE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-game
./game/PrinceOfPersia
```

It keeps the unpacked game files, saves, `pop1.cfg` and `pop1.log` in
`~/.local/share/pop1-recomp/PrinceOfPersia/` (or under `$XDG_DATA_HOME`),
the usual place on Linux, so the program itself can live anywhere, even
somewhere read-only. Started from a terminal, it prints that folder once.
The developer build (`./build/pop1`) keeps reading `work/` and saving next
to it in the repo, as on Windows.
Linux: tested by Claude (build, save/load); not yet tested by me.

On a **Mac** (Apple Silicon or Intel), one script makes a normal Mac app.
It needs only Apple's Command Line Tools (it asks macOS to install them if
they are missing); everything else it fetches into `build-mac/`, nothing
is installed elsewhere and no Homebrew is needed:

```sh
git clone --recursive https://github.com/RufusFrederiks/pop1-recomp
cd pop1-recomp
tools/build-mac.sh /path/to/folder/with/the/disks    # or put them in original/
open "game/Prince of Persia.app"
```

The first run takes a few minutes (it builds SDL2 once). The app has the
game's own icon and can be dragged to Applications. Saves and settings go to
`~/Library/Application Support/pop1-recomp/PrinceOfPersia/`. On a MacBook,
hold **fn** for the F keys (F5/F9 save/load, F11 fullscreen).
macOS: the script is tested on GitHub's Mac (everything but the game); the
game itself not yet on a real Mac.

The game opens with the **recomp menu**: press Enter on *Start Game*. **Esc**
or **F1** brings the menu back at any time and pauses the game: resume,
restart the level, save/load state, sound effects and music on/off,
graphics, look, lighting (colour only), display theme (black-and-white only:
Macintosh, Amber, Phosphor, Parchment, Midnight), size, crop borders (colour only),
scanlines, the controls, quit.
**F11** or **Alt+Enter** toggles fullscreen, **F2** (Shift+F2) switches
to the next (previous) look while you play, **F3** steps through the
lighting (Off, Classic, Enhanced). Settings are kept in
`pop1.cfg`.

**Look**: *Remastered* (the default) scales the picture 4× and then
smoothly to the largest size that fits, so it fills the screen and stays
sharp. *AdvMAME* smooths the diagonals (still pixel art, no new
colours); *xBR* gives smooth curves and edges. These two run at 3× or 4×,
whichever is closest to the size on your screen, so their work is not
shrunk away. *MMPX* (Morgan McGuire and Mara Gagiu's style-preserving
magnification, 2021) keeps the art as it was drawn: slopes become clean
lines, while single pixels, thin lines and the tips of shapes stay, and
every colour is one the game itself drew. It always works at 4×.
*CRT Monitor* looks like the Apple RGB monitors of the time
(fine red, green and blue stripes, light scanlines, a little glow); *CRT
TV* has stronger scanlines, a shadow mask, a softer picture and more glow.
*Original*
uses whole-number scaling only (1×, 2×, 3×...), the Mac's pixels exactly,
with black borders where the screen has room left.

**Lighting** (colour modes, off by default; F3): an effect of this recomp,
not of the original game. *Classic*: the room is dimmed a little and each
torch casts a warm, gently flickering pool of light, with a small glow on
its flame; a potion glows faintly in its own colour. *Enhanced* goes much
further, with shaders on the graphics card:

- the torches really light the room: the light falls off with distance,
  is stopped by walls, floors and pillars, and the Prince and the guards
  throw shadows;
- relief: the light rakes across the stones, from the picture's own shading
  (no art is redrawn);
- the flames glow (HDR bloom), the black background shows as a faintly lit
  wall near a torch, and corners and the space under ledges are a little
  darker;
- heat haze above the flames, sparks rising from them, dust drifting in the
  light, dust where the Prince lands or a loose floor drops, sparks and a
  flash where swords meet;
- a potion lights its surroundings in its own colour.

Enhanced needs OpenGL 2.1 (any graphics card since about 2008). The game
is drawn through the system's usual renderer until Enhanced is first
chosen, and switches to OpenGL only then; if that fails the choice is
simply not offered. Its parts can be switched off one by one in `pop1.cfg`
(`bloom`, `shadows`, `relief`, `haze`, `particles`, `vignette`), and
`gpu = 0` keeps the game off OpenGL altogether. Lighting works in every look except
*Original*, which always stays the Mac's picture, and under the CRT looks.
The recomp reads where the torches are from the level in the game's memory
and only lights a torch once its flame is really on the screen, so the
title, the story and the princess scenes are never lit.

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
`pop1.cfg` (in the `game` folder on Windows, in the per-user folder on
Linux, see above).

In the game: arrow keys or the numeric keypad move, **Shift** acts (grab,
climb, careful step, draw the sword). **Ctrl+N** is a new game (Ctrl stands
in for the Mac's Command key, Alt for Option). The picture is the game's
colour mode unless you choose otherwise.

**Controllers**: Xbox, PlayStation (DualShock 4, DualSense), Switch Pro and
most others SDL knows, over USB or Bluetooth, plugged in at any time; the
keyboard keeps working alongside. Buttons go by position, so "A" is the
bottom button on every controller (Cross on PlayStation, B on Switch).

| Controller (Xbox names) | During play | In the menu (game paused) |
|---|---|---|
| D-pad / left stick | run, turn, jump up (up), crouch (down) | move |
| A | Shift: grab, hold, careful step, sword | select |
| B | jump / climb (Up) | back |
| X | Shift, like A (or RT) | **save state** |
| Y | crouch / step down (Down) | **load state** |
| LB / RB | nothing | previous / next look |
| Menu (Start) | open the menu | close it |
| View (Back) | next look | next look |

During play nothing but Start and View does anything outside the game, so
nothing can go off by mistake; save and load are in the menu (the slot it
shows; the menu closes after).

A running jump: run, then press B (or push up). The Controls page in the
menu shows the buttons with the names printed on the controller you have
connected. To change them, edit `controller.cfg` (written next to `pop1.cfg`
on first start; it explains itself): stick on/off, dead zone, rumble, and
what each button does during play (a quick save on a button is possible
there; the menu's buttons stay as above).
The controller rumbles when the Prince gets hurt, lands hard, hits spikes or
a slicer, and lightly for landings, crashing floors, slamming doors and sword
blows. A `gamecontrollerdb.txt` next to it adds controllers SDL doesn't know.

To build the Windows executables on Linux: `tools/cross-windows.sh
[--standalone] <SDL2 MinGW folder>` (see the script).

Debugging knobs (environment variables): `POPMODE=bw|colour|lc` (graphics
mode, overrides `pop1.cfg`), `POPNOMENU=1` (no start menu), `POPTRACE=n`
(first n traps with their stack effect), `POPCQD=n` (colour/palette traps),
`POPEXIT=s` (quit after s seconds), `POPSHOTS=5,10` (+ `POPSHOTDIR`,
screenshots: `.pgm` in B&W, `.ppm` in colour), `POPUISHOT=file.ppm` +
`POPUISHOTAT=s` (what the player sees, menu and dialogs included),
`POPKEYS="^n@8,right@12+2,save1@15,load1@20,menu1000.1@25"` (scripted keys,
savestates and menu picks), `POPPAD="a@3,dpright@7+2,lx=-1@10+1"` (a scripted
virtual controller, button names as in `controller.cfg`), `POPPADSND=1`
(log each sound effect the game plays), `POPPOKE="a5-0x459e=1@2.5"` (write guest memory
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
- [x] Display looks: Original, Remastered, AdvMAME, xBR, MMPX, CRT Monitor, CRT TV (F2); Crop Borders; DPI-aware fullscreen
- [x] Torch lighting (F3), a recomp-only effect: Classic, and Enhanced (shader lighting with shadows, glow, relief, sparks and dust; tested by Claude on Linux and under Wine, not yet on a real Windows PC)
- [x] Controllers: Xbox, PlayStation, Switch Pro; menu, rumble, remappable (tested by Claude with a virtual controller)
- [x] Sprite extractor (colour sets to PNG, for the HD work)
- [x] Level 1 finishable (played through on Windows)
- [ ] Levels 2 and up tested (1 to 4 played through on Windows, no bugs; rest in progress)
- [ ] Sound effects checked, music seams fixed
- [ ] Copy-protection screen checked
