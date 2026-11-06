# Dev log

Newest entry at the bottom. Each session: what was done, what was learned,
what's next.

## 2026-11-05 — Repo setup

- Created the repo with macrecomp as a submodule (`ext/macrecomp`).
- `.gitignore` keeps game data (`original/`), extracted files (`work/`)
  and generated code (`src/gen/`) out of git.
- Source to use: `prince_of_persia_moof_dc42.zip` from Macintosh
  Repository (DiskCopy 4.2 image; macrecomp reads it directly).
  Fallback: `PoP-NoCopyProtect-Speedfix.sit`.
- Reference projects:
  - macrecomp (MIT) — toolkit, free to use.
  - mac-recomp/pop2 — PoP 2 Mac recomp. **No license**: read for ideas,
    don't copy code.
  - SDLPoP — DOS disassembly; reference for game logic.

**Next:** put the image in `original/`, run extraction and
`scan_traps.py --coverage`, and record the numbers here.

## 2026-11-05 — Step 1: repo hygiene

- `.gitignore` now also covers `*.moof`, `*.woz`, `*.2mg`, `*.image`. The
  three `.moof` images sat in the repo root and would have been committed.
- Disk images belong in `original/`. The images in the repo root are still
  covered by `.gitignore`, so nothing leaks, but `original/` is the
  documented place.
- Submodule `ext/macrecomp` is pinned at `708adac` ("Show a card, not an
  error dialog"). After cloning: `git submodule update --init`.

## 2026-11-05 — Step 2: recon (extraction)

`extract_resources.py` on all three DiskCopy 4.2 images (800K each):

| Disk | Files |
|---|---|
| 1 | `Prince of Persia` (APPL, creator `PoƒP`, rsrc 491,230 B, no data fork), `Persia(BW)` (`PPgf`, data 294,251 B), `Read Me` |
| 2 | `Persia(COLOR)` (`PPgf`, data 470,375 B) |
| 3 | `Persia(LC)` (`PPgf`, data 384,160 B) |

So the program is on disk 1 and the three `PPgf` files are the graphics
sets: black-and-white, 256-colour (Mac II 13"), and LC 12". The Read Me says
the game falls back to **B&W when there is no Color QuickDraw**. That is the
plan for the first boot: macrecomp's QuickDraw is 1-bit, so B&W first,
colour after.

The application (246 resources):

- **CODE 0–4**: 73,116 B. Jump table: 171 entries (seg 1: 10, 2: 43,
  3: 67, 4: 51). Above A5 1,400 B, below A5 (globals) 27,112 B.
- **DATA 0 / ZERO 0 / DREL 0**: THINK C globals initialiser + relocations.
- **No code encryption**: the segments disassemble as ordinary 68k straight
  away (unlike Shufflepuck's HLS protection).
- There is a `DLOG 1001 "Copy protect"`: a manual-lookup style check, not
  encrypted code. To deal with when the game reaches it.
- Sound: `snd ` ×31, `csnd` ×20, `MIDI` ×24, `SONG` ×46, `INST` ×28, and
  **`MDRV` 11 / 22 "11k/22k Note and MIDI driver"** (~10.9 KB each). MDRV is
  68k code the game loads and calls itself, so music means lifting it too.
- `LEVL` ×16 (2,304 B each: the levels), `clut` ×11, `FONT`/`FOND`,
  `HIGH` (high scores), `OPTS` (options), `MENU` 1000–1003, 12 `DITL`.

## 2026-11-05 — Step 3: trap census

`scan_traps.py work/disk1/code --coverage ext/macrecomp/runtime/toolbox.c`:

| | 68k | CODE segs | Distinct traps | Call sites | Sites covered | Traps covered |
|---|---|---|---|---|---|---|
| Prince of Persia (1992) | 73 KB | 4 + CODE 0 | 171 | 505 | **87%** | 75% |

Per segment: CODE 2 holds 424 of the 505 trap calls (the Mac shell: menus,
dialogs, resources, palettes). CODE 3 has 56 and CODE 4 only 17 in 8,000
instructions: that is the game engine, and it barely calls the Toolbox. The
blitter writes to memory itself.

Missing, ranked by call sites:

- **Palette Manager** (colour only): `SetEntryUsage`, `AnimatePalette`,
  `ActivatePalette`, `SetPalette`, `NewPalette`, `PmForeColor`, `GetSubTable`
- **VBL**: `VInstall`/`VRemove` — the game's timer. Needs a periodic task.
- **Colour QuickDraw / GDevice**: `OpenCPort`, `NewCWindow`, `SetGDevice`,
  `GetGDevice`, `ForeColor`, `BackColor`, `SetOrigin`
- **Sound**: `SndDoImmediate`, `SndDoCommand`, `SndDisposeChannel`, `KillIO`
- Small: `WriteResource`, `ChangedResource` (high scores), `UprString`,
  `HiWord`/`LoWord`, `NewHandleClear`, `ResrvMem`, `PurgeMem`,
  `WaitNextEvent`, `GetToolTrapAddress`, `SystemClick`, `OpenDeskAcc`.

## 2026-11-05 — Step 4: first lift

`lift68k.py` on CODE 1–4, no `--entry` hints yet:

| Seg | Functions | Insns | Untranslated | Coverage |
|---|---|---|---|---|
| 1 | 13 | 211 | 4 | 98.1% |
| 2 | 186 | 8,116 | 9 | 99.9% |
| 3 | 227 | 7,040 | 0 | 100% |
| 4 | 216 | 8,024 | 0 | 100% |

The untranslated instructions:

- CODE 1 `0x24 jsr (a5,d1.w)`: THINK C startup calling `main` through
  the jump table (`d1` = `0x1c2`, entry 52). Avoidable by calling main from
  the glue, or by teaching the lifter this form.
- CODE 1 `0xa2/0xb6/0xd2`: THINK C's `switch` helpers (computed `jmp`).
  Shufflepuck notes these are lowered at each call site and never reached.
- CODE 2 `0x381a`, `0x4e86`, `0x4eac`: data misread as code (a table and
  the credits text, "...Jim Nichols... Licensed to Presage Software").
- CODE 2 `0x3a34`–`0x3a54`, `0x3dd4`–`0x3df4`: **real 68020 code**,
  scaled index `(a0,d1.w*4)` into two 8-entry handle tables at
  `A5-0x5a86`/`A5-0x5a66`. macrecomp's lifter is 68000-only. Probably a
  colour path (Colour QuickDraw implies a 68020+), to confirm at runtime.

## 2026-11-05 — Step 5: build setup

Project pieces (all original code, none copied from pop2 or Shufflepuck):

- `tools/prepare.py`: one cross-platform script. Finds the three images in
  `original/`, extracts the application into `work/app/`, every file on every
  disk into `work/files/` (plus a `files.txt` manifest, written in Mac Roman
  so the creator `PoƒP` stays 4 bytes), disassembles into `work/disasm/`,
  and lifts CODE 1–4 into `src/gen/`.
- `src/main.c`: the loader. Flat 32 MB address space: stack top `0x180000`,
  A5 `0x200000` (27,112 bytes of globals below, jump table above), CODE n at
  `0x300000 + (n-1)*0x10000`, macrecomp's heap from 8 MB. Wires all 171
  jump-table entries, registers resources and files, then runs THINK C's
  own globals initialiser (CODE 1 `0x30`) and calls main through jump-table
  slot `A5+0x1c2`, as CODE 1's startup would.
- `src/traps.c`: this game's layer in front of macrecomp.
  `CMakeLists.txt` compiles macrecomp's `toolbox.c` with
  `-Dm68k_trap=mr_toolbox_trap` and `m68k.c` with
  `-Dm68k_time_slice=mr_core_time_slice`, so `traps.c` sees every trap and
  every loop time slice first and forwards what it does not handle. **The
  submodule is not modified.**
- Builds in ~10 s with CMake + SDL2 (lifted code at `-O1`).

## 2026-11-05 — Step 6: first boot → the intro story screen

Headless (`SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software`, `MRSHOT`).
What stood in the way, in order:

1. **main returned straight after init.** `POPTRACE` (new: logs each trap's
   stack effect) showed `FlushEvents` moving SP by +4. It is a register trap
   (`move.l (a7)+,d0; _FlushEvents`), but macrecomp `708adac` pops a long as
   well. PoP's init routine has no stack frame, so its `rts` then found
   main's caller's return marker and unwound past main. HyperCard and
   Shufflepuck call it from functions with `link`/`unlk`, which hides the
   bug. Fixed in `traps.c`. **Worth reporting upstream.**
2. **`WaitNextEvent` unimplemented.** An unimplemented Pascal trap doesn't
   pop its arguments, so each event-loop pass leaked 12 bytes of stack.
   Now forwarded to `GetNextEvent` after dropping `sleep` and `mouseRgn`.
3. **Small traps added:** `ResrvMem` (no-op), `ForeColor`/`BackColor` (1-bit:
   no-op), `SetOrigin` (real: moves portRect, portBits.bounds and visRgn,
   then re-syncs via SetPort). PoP centres its play area with
   `SetOrigin(0,-4)` and then blits to `portBits.baseAddr` itself.
4. **Game idled in the event loop.** The flag at `A5-0x69e5` ("running") is
   set only by the window's activate handler. PoP creates its window
   visible with `NewWindow` (procID 2, plainDBox), which on a real Mac makes
   it the active front window; macrecomp raises `activateEvt` only on
   `ShowWindow`/`SelectWindow`. `traps.c` now does a `SelectWindow` after a
   visible `NewWindow`. Also an upstream candidate.

Result: **the intro story screen** ("In the Sultan's absence the Grand
Vizier JAFFAR rules…") in the border art, drawn in B&W by the game's own
code. The graphics come from `Persia(BW)` via the File Manager.

Also added: `POPEXIT=<seconds>` (clean exit, so `MRTRAPS` histograms print)
and `VInstall`/`VRemove` (VBL tasks run each 1/60 s tick, at the next trap
or loop time slice, with all registers saved).

Next: the music driver. The game calls into the heap at `0x882784`: that is
an `MDRV` resource (68k code) loaded by `GetResource`. Plus `KillIO`
($A006), i.e. the Sound Driver.

## 2026-11-05 — Step 7: the music driver

The game calls into the heap: that is `MDRV` 11 or 22, "Note and MIDI
driver" (© 1991–92 James Nitchals), 68k code shipped **encrypted and
compressed**. How the game loads it (CODE 2):

- `0x45f0`: `GetResource('MDRV', id)`, `DetachResource`.
- `0x4532` → `0x44fe`: decrypts bytes 7..end in place with the eexec-style
  stream cipher: `plain = c ^ (r>>8); r = (c + r) * 52845 + 22719`, seed
  `0xDCE5`.
- `0x4dda` → `0x4c98`: the first long is the unpacked size (22,174 / 22,590);
  the rest is **LZSS** (4 KB window; flag byte LSB-first, 1 = literal, 0 =
  big-endian word: 12-bit position, 4-bit length, copy length+3 from
  `out − 4096 + position`).

`tools/prepare.py` now does the same offline (exact sizes, all input used,
no references before the buffer) and lifts both drivers as pseudo-segments
11 and 22. `traps.c` wraps `0x4dda`: it runs the game's own unpacker, then
registers the lifted driver at the heap address it unpacked to.

The driver's code needed four 68000 forms macrecomp's lifter lacks.
**`tools/lift.py`** imports `lift68k.py` as a module, adds them, and runs it
unchanged (submodule untouched): `ori/andi/eori #imm,ccr` (it returns errors
in the carry flag), the same to `sr` (the interrupt mask goes to `pop_ipl`, so
no VBL task runs inside its critical sections), `move sr`/`move to sr/ccr`, and
`movep`. Driver coverage 99.2% → 99.8%; the rest is data.

Then, in order:

1. **Second load decrypted twice.** macrecomp keeps one handle per resource
   and ignores `DetachResource`, so the next `GetResource` returned the bytes
   already decrypted in place. `traps.c` now remembers detached handles and
   hands back a fresh copy of the original bytes. (Upstream candidate.)
2. **"Could not locate the vbl just installed in the sys queue!"** (the
   driver's own `DebugStr`). It walks the real VBL queue at `$160` for its
   task's *predecessor*, to move itself to the front. So VBL tasks now live in
   the guest's queue, and `pop_init` seeds it with one inert system task,
   because on a real Mac it is never empty.
3. **Sound Manager** (`src/sound.c`, new). The driver mixes in software and
   sends one `sampledSynth` channel `bufferCmd` (81), `flushCmd` (4) and
   `quietCmd` (3). Buffers are 44,400 samples at 22,254.5 Hz. It queues the
   same buffer twice and keeps mixing into it ahead of playback (a ring),
   and it paces itself by watching the channel's `qHead` (offset 32). So
   samples are read from guest memory *when played*, on the wall clock, and
   `qHead` advances when a buffer finishes. SDL2 audio, 22,050 Hz mono.
   `POPWAV=<file>` records the output; `POPSOUND=1` traces commands.
4. **Music ran 4× too fast.** macrecomp bumps the low-memory `Ticks` on
   every lifted call; the driver times its mixing on `Ticks`. CMake now
   copies `m68k.c` into the build tree with that one line removed (and fails
   if it ever doesn't match), and `traps.c` keeps `Ticks` on the real 60 Hz
   clock. Audio length now tracks wall time.

Result: **title screen** ("Prince of Persia © Copyright 1992 Jordan Mechner")
and **the title music**, recorded headless. The spectrogram shows clean note
harmonics; there is a noise floor and a seam every few seconds where my
play position and the driver's estimate disagree. To refine.

Also added: `PurgeMem` (no-op), `DebugStr` (printed, then carry on).

## 2026-11-05 — Step 8: keyboard → Level 1, the prince moves

How PoP reads the keyboard:

- In play, the engine (CODE 3 `0x0a3a`) calls **`GetKeys`** and tests bit
  groups of the KeyMap (key code k = byte k/8, bit k%8). Left: ← arrow,
  keypad 4/1/7, `s`/`w`, `j`/`u`. Right: → arrow, keypad 6/3/9, `f`/`r`,
  `l`/`o`. So a keypad-style layout on both hands, plus arrows; ⌘ held
  disables movement. macrecomp's `GetKeys` reports nothing held.
- Otherwise keyDown events: Esc and ⌘-keys, which go to `MenuKey`. Menus:
  File (New Game ⌘N, End Game, Open ⌘O, Save ⌘S, Save As, Quit ⌘Q), Edit,
  Game (Pause, Sounds, Music, Current Level, Time Remaining, Restart Level).
  Init sets `SysEvtMask` to `$FFDF`: keyUp on, autoKey off.

`src/input.c` (new) takes the keyboard over: SDL key events are pulled out of
SDL's queue before macrecomp's pump and become Mac events
(`keycode<<8 | char`, proper modifiers; Ctrl = ⌘, Alt = Option); `GetKeys`
reads SDL's keyboard state; `MenuKey` searches the `MENU` resources for the
⌘-key. `POPKEYS` scripts keys for headless runs.

Fixed on the way:

- Key events that an `EventAvail` mask excluded were dropped; a Mac leaves
  them queued (the main loop polls `EventAvail(updateMask)` every pass).
- `HiWord`/`LoWord` were unimplemented, so the menu choice got lost.
- Added `UprString`, `GetPenState`.
- `POPSHOTS` / `POPSHOTDIR`: screenshots at given seconds.

Result, scripted `^n@8, right@14+2`: "Brøderbund Software presents", then
**Level 1**, the prince crouching at the start, running right, and dropping to
the lower floor. A 20 s run hits no unimplemented trap.

### Known issues / next

- `m68k_call: no function at <heap address>`: a call to the address just
  past the first sound channel record, now and then. Harmless so far;
  find out what the driver thinks lives there.
- Music seams every few seconds: my play position and the driver's
  `Ticks`-based estimate drift apart; also check sound effects (`snd `/`csnd`).
- The 68020 scaled-index code in CODE 2 (`0x3a34`–`0x3df4`) is not lifted;
  probably colour-only. Colour mode needs the Palette Manager and colour
  QuickDraw (study mac-recomp/pop2 for ideas only: no license).
- The copy-protection dialog (`DLOG 1001`) has not appeared yet; it may come
  after a level or two.
- Upstream candidates for macrecomp: `FlushEvents` popping 4 bytes,
  activate event for a visible `NewWindow`, `DetachResource`, `GetKeys`,
  `HiWord`/`LoWord`, `MenuKey`, the per-call `Ticks` bump, and the lifter
  forms in `tools/lift.py`.

## 2026-11-05 — Step 9: a Windows build to test with

- The loader now reads the **application's resource fork itself**
  (`work/files/disk1/1.rsrc`, found through `files.txt`): CODE 0–4, the jump
  table and all 241 other resources. The runtime needs only `work/files/`
  (the manifest and the forks from the three disks); `work/app/` is for the
  build. It also picks up `PoƒP 0`, which the old one-file-per-resource
  layout could not name.
- `pop1` looks for `work/` in the current folder, next to the executable and
  one level up, so double-clicking `build\pop1.exe` works.
- Cross-compiled for Windows (MinGW-w64 + SDL 2.30.9) and tested under Wine:
  boots, ⌘N (Ctrl+N), Level 1. `Play.cmd` starts it from the repo root.

## 2026-11-05 — Step 10: first test on real Windows

Rufus ran `build\pop1.exe` on his own PC (started with `POPKEYS=^n@5`):
it plays, and he got far into Level 1. A couple of visual glitches, to be
described and fixed next.

Found on the way: pressing Ctrl on its own opens the About box. The Ctrl
key-down goes out as a ⌘-key event with character 0, `MenuKey(0)` then
matches the first menu item with no key equivalent (About, in the Apple
menu). Fix: don't send events for modifier keys alone. Plain keys don't
start a game: Tab lets the intro run on, Space and Esc freeze it (Esc is
Pause).

## 2026-11-05 — Step 11: visual glitches fixed, Ctrl, the recomp menu

Rufus's screenshot: a room drawn black except around the prince. Scripted
random play (four 2-minute runs, a frame every 0.5 s, scanning for
near-black frames) found three related faults. How PoP draws: the engine
composes each frame in an offscreen bitmap and `CopyBits` it to the
window, the whole play area after a room change (CODE 2 `0x1c2a`) and
otherwise a list of dirty rectangles (`0x1c74`).

1. **Inverted rooms that stay inverted.** The hurt flash (effect 0,
   counter `A5-0x45e6`) is `InverRect` → copy the frame with
   `notSrcCopy` → `InverRect`. In QuickDraw `notSrcCopy` is **4**; macrecomp
   tests `(mode & 0x24) == 0x24` ($24 is the colour mode `transparent`),
   so the copy came out normal and the second `InverRect` left the room
   inverted.
2. **Black rooms.** The engine `InvalRect`s small areas (status bar, play
   area). The update handler (`0xfde`) does `BeginUpdate`,
   `PaintRgn(border)`, the full copy, `EndUpdate`, relying on
   `BeginUpdate` to clip all of it to what was invalidated. macrecomp
   (a) turns every `InvalRect` into "whole window", (b) does not clip
   during an update, and (c) keeps regions as bounding boxes, so the
   border, `DiffRgn(window, play area)`, painted the whole window black.
3. **A black frame at every room change.** The engine blanks the play area
   (CODE 3 `0x13d4`) and the new room arrives with the update one game
   frame later; macrecomp presented the window at every `EventAvail`, i.e.
   mid-frame.

`src/qd.c` (new): regions as rectangle lists (`NewRgn`, `RectRgn`,
`DiffRgn`, `UnionRgn`, `SectRgn`, `CopyRgn`, `PaintRgn`, `EraseRgn`,
`InvertRgn`, with the bounding box kept in guest memory), `InvalRect` that
accumulates the real rectangle, `BeginUpdate`/`EndUpdate` that narrow
QuickDraw's clip to it, and `CopyBits` with all eight 1-bit transfer modes,
clipped to the port and the mask region. The front-end presents only at the
top of the game's loop and holds the last frame (at most 3) while an update
is owed. Result: 0 black frames and no stuck inversion over the same runs.
(Also seen: on the screen macrecomp ignores a port's origin, so the picture
sits 4 rows high; consistent everywhere, left as is.)

**Ctrl opened the About box**: a Ctrl key-down went out as ⌘ + character 0
and `MenuKey(0)` matched the first item without a key equivalent. Modifier
keys alone no longer make events (as on a Mac), and `MenuKey` never matches
"no key". Ctrl+N starts a game.

**The recomp menu.** `src/platform.c` replaces macrecomp's `platform_sdl.c`
(same `plat_*` interface): letterboxed integer scaling, resizable window,
F11/Alt+Enter fullscreen, five themes for the 1-bit picture, scanlines,
settings in `pop1.cfg`, and the menu, drawn in the game's own "Persian"
display font (FONT 25616, capitals only; digits fall back to the 5×7 font at
2×). It opens at start-up (Start Game), Esc/F1 open it in play and pause the
game. Commands go to the game as its own menu picks (`pop_inject_menu` → a
queued ⌘-key that `MenuKey` answers): File › New Game, Game › Sounds, Music,
Restart Level. Added `ChangedResource`/`WriteResource` (no-ops: the game saves
its options there).

Checked headless and under Wine (Windows build): menu → Start Game → play,
Esc → menu → Sound/Music/Restart, sound effects during play.
`tools/cross-windows.sh` builds `build/pop1.exe` from Linux.

## 2026-11-05 — Step 12: a single-file PrinceOfPersia.exe

`tools/cross-windows.sh --standalone` builds `build/PrinceOfPersia.exe`
(7.5 MB): no console, SDL 2.30.9 linked in statically (only Windows' own
DLLs needed), the game's files inside, and the game's own icon.

- `tools/embed.py` packs `work/files/` (the manifest and the forks of the
  three disks, 1.66 MB) into `src/gen/embedded.c`, gitignored like the
  lifted code: it comes from your own disks.
- With `POP_STANDALONE`, `main.c` unpacks them once into SDL's per-user
  folder (`%APPDATA%\pop1-recomp\PrinceOfPersia\work\`), because the File
  Manager HAL reads files from disk; `pop1.cfg` and `pop1.log` (stderr, as
  there is no console) go there too.
- `tools/make_icon.py` turns `icl8`/`ICN#` 1000 and `ics8`/`ics#` 1000 into
  a .ico (16, 32, 48, 256 px) with the standard Macintosh 8-bit palette;
  `windres` adds it and version info.

Tested under Wine with nothing but the .exe in an empty folder: unpacks,
start menu, Start Game, play.
