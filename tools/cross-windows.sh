#!/usr/bin/env bash
# Cross-compile the game for Windows from Linux (MinGW-w64 + SDL2).
#
#   tools/cross-windows.sh /path/to/SDL2-2.30.x/x86_64-w64-mingw32
#       -> build/pop1.exe + build/SDL2.dll (console; reads work/files/)
#   tools/cross-windows.sh --standalone /path/to/SDL2-2.30.x/x86_64-w64-mingw32
#       -> build/PrinceOfPersia.exe: one file, no console, SDL linked in,
#          your game files inside (tools/embed.py), the game's own icon.
#          Made from your own disks: for your own use, not for sharing.
#
# Needs: x86_64-w64-mingw32-gcc, python3, and the SDL2 MinGW development
# package (SDL2-devel-<ver>-mingw.tar.gz from github.com/libsdl-org/SDL).
# Run tools/prepare.py first. Mirrors CMakeLists.txt: the same two renames
# and the same edited copy of m68k.c.
set -euo pipefail
STANDALONE=0
if [ "${1:-}" = "--standalone" ]; then STANDALONE=1; shift; fi
SDLW="${1:?path to SDL2 x86_64-w64-mingw32 folder}"
cd "$(dirname "$0")/.."
MR=ext/macrecomp
OUT=build/win
mkdir -p "$OUT" build
python3 - "$MR/runtime/m68k.c" "$OUT/m68k.c" <<'EOF'
import sys
src = open(sys.argv[1]).read()
a = "    bump_ticks();\n    if (g_shadow_sp < 512) g_shadow[g_shadow_sp] = addr;"
if a not in src: sys.exit("m68k.c changed: the per-call Ticks bump was not found")
open(sys.argv[2], "w").write(src.replace(a, "    /* pop1: per-call Ticks bump removed */\n    if (g_shadow_sp < 512) g_shadow[g_shadow_sp] = addr;"))
EOF
CC="x86_64-w64-mingw32-gcc -std=gnu11 -I$MR/include -Isrc -I$SDLW/include/SDL2 -DMACRECOMP_NO_TRAP_STUB -DMACRECOMP_SDL"
if [ $STANDALONE = 1 ]; then
  OUT=build/win-standalone; mkdir -p "$OUT"
  cp build/win/m68k.c "$OUT/m68k.c" 2>/dev/null || true
  CC="$CC -DPOP_STANDALONE"
fi
[ -f "$OUT/m68k.c" ] || cp build/win/m68k.c "$OUT/m68k.c"
for f in src/gen/code_*.c; do $CC -O1 -w -c "$f" -o "$OUT/$(basename "$f" .c).o" & done
for f in src/*.c; do $CC -O2 -c "$f" -o "$OUT/$(basename "$f" .c).o" & done
$CC -O2 -w -Dm68k_time_slice=mr_core_time_slice -c "$OUT/m68k.c" -o "$OUT/m68k.o" &
$CC -O2 -w -Dm68k_trap=mr_toolbox_trap -c "$MR/runtime/toolbox.c" -o "$OUT/toolbox.o" &
for f in quickdraw dialog textedit files; do $CC -O2 -w -c "$MR/runtime/$f.c" -o "$OUT/$f.o" & done
wait
if [ $STANDALONE = 0 ]; then
  x86_64-w64-mingw32-gcc -mconsole -o build/pop1.exe "$OUT"/*.o -L"$SDLW/lib" -lSDL2 -lm -static-libgcc
  cp "$SDLW/bin/SDL2.dll" build/
  echo "built build/pop1.exe (+ build/SDL2.dll)"
  exit 0
fi
python3 tools/embed.py
$CC -O0 -c src/gen/embedded.c -o "$OUT/embedded.o"
python3 tools/make_icon.py work/app/rsrc "$OUT/pop1.ico"
cat > "$OUT/pop1.rc" <<RC
1 ICON "pop1.ico"
1 VERSIONINFO
FILEVERSION 0,1,0,0
PRODUCTVERSION 0,1,0,0
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "FileDescription", "Prince of Persia (Macintosh, 1992) - static recompilation"
      VALUE "ProductName", "pop1-recomp"
      VALUE "FileVersion", "0.1"
      VALUE "ProductVersion", "0.1"
      VALUE "Comments", "Unofficial fan project. Game (c) 1992 Jordan Mechner / Broderbund."
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
RC
(cd "$OUT" && x86_64-w64-mingw32-windres pop1.rc -O coff -o pop1res.o)
x86_64-w64-mingw32-gcc -mwindows -static -o build/PrinceOfPersia.exe "$OUT"/*.o \
  "$SDLW/lib/libSDL2.a" -lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 \
  -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8
echo "built build/PrinceOfPersia.exe"
