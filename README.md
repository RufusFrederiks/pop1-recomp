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
original/        your copy of the game (gitignored)
work/            extracted resources, disassembly, reports (gitignored)
src/gen/         lifted C generated from the game (gitignored)
src/             hand-written glue for this game
ext/macrecomp/   the toolkit (submodule)
DEV-LOG.md       what was done, step by step
```

## Getting started

```sh
git clone --recursive https://github.com/<you>/pop1-recomp
cd pop1-recomp
pip install -r ext/macrecomp/tools/requirements.txt
# put the DiskCopy 4.2 image in original/, then:
python ext/macrecomp/tools/extract_resources.py original/<image>.dc42 -o work/
```

## Status

- [ ] Recon: extract resources, trap census
- [ ] Lift 68k → C
- [ ] Boots
- [ ] Title screen
- [ ] Level 1 visible
- [ ] Prince moves
- [ ] Level 1 finishable
- [ ] Sound
