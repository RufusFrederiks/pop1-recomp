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
