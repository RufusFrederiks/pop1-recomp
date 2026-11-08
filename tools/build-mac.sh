#!/bin/bash
# Build Prince of Persia for macOS from your own disks, as a normal Mac app.
#
#   tools/build-mac.sh [folder with the three .dc42 disk images]
#       (the folder defaults to original/)
#   -> game/Prince of Persia.app   (double-click it, or drag it to Applications)
#
# Needs only Apple's Command Line Tools (the script asks macOS to install them
# if they are missing). Everything else goes into build-mac/ in this repo:
# a private Python with cmake and ninja, and SDL2, built from its source.
# Nothing is installed elsewhere on the Mac; delete build-mac/ to start over.
#
# The app carries the game files inside: made from your own disks, for your
# own use, not for sharing. Saves and settings go to
# ~/Library/Application Support/pop1-recomp/PrinceOfPersia/.
#
#   tools/build-mac.sh --check
#       everything except the game (no disks needed): used by the build check
#       on GitHub, to test this script on a Mac.
set -euo pipefail

SDL_VERSION=2.32.10
SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
APP_NAME="Prince of Persia"

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
WORK="$ROOT/build-mac"
CHECK=0
DISKS="$ROOT/original"
case "${1:-}" in
    --check) CHECK=1 ;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    "") ;;
    *) [ -d "$1" ] || { echo "Stopped: no folder $1" >&2; exit 1; }
       DISKS="$(cd "$1" && pwd)" ;;
esac

step(){ printf '\n==> %s\n' "$*"; }
fail(){ printf '\nStopped: %s\n' "$*" >&2; exit 1; }

[ "$(uname)" = Darwin ] || fail "this script is for macOS (on Windows and Linux, see the README)"

# ---- 1. Apple's Command Line Tools: the C compiler, git, python3 ----
step "Checking Apple's Command Line Tools"
if ! xcode-select -p >/dev/null 2>&1; then
    xcode-select --install 2>/dev/null || true
    fail "the Command Line Tools are not installed yet. A window has opened to
install them: click Install, wait until it is done, then run this script again."
fi
command -v cc >/dev/null || fail "no C compiler found (cc)"
python3 -c 'import sys; sys.exit(sys.version_info < (3, 9))' \
    || fail "python3 is older than 3.9"
echo "ok: $(cc --version | head -1), $(python3 --version)"

# ---- 2. the game's disks (checked first, so a missing disk stops us early) ----
if [ $CHECK = 0 ]; then
    step "Looking for the disks in $DISKS"
    found=0
    for f in "$DISKS"/*; do
        case "$(basename "$f" | tr 'A-Z' 'a-z')" in
            *disk*1*.dc42|*disk*1*.image|*disk*1*.img|*disk*1*.dsk) found=1 ;;
        esac
    done
    [ $found = 1 ] || fail "no 'Disk 1' image in $DISKS. Put the three
'Prince of Persia (Disk N of 3).dc42' files there, or give their folder:
    tools/build-mac.sh /path/to/folder"
    for f in "$DISKS"/*.dc42; do [ -f "$f" ] && echo "  $(basename "$f")"; done
fi

# ---- 3. macrecomp (the git submodule) ----
if [ ! -f ext/macrecomp/runtime/m68k.c ]; then
    step "Fetching macrecomp"
    git submodule update --init --recursive
fi

# ---- 4. a private Python with the tools ----
step "Setting up Python tools in build-mac/venv"
mkdir -p "$WORK"
[ -x "$WORK/venv/bin/python" ] || python3 -m venv "$WORK/venv"
export PATH="$WORK/venv/bin:$PATH"
python -m pip install -q --upgrade pip
python -m pip install -q cmake ninja pillow -r ext/macrecomp/tools/requirements.txt
echo "ok: $(cmake --version | head -1), ninja $(ninja --version)"

# ---- 5. SDL2, built once from its source as a static library ----
SDL_PREFIX="$WORK/sdl"
if [ ! -f "$SDL_PREFIX/lib/libSDL2.a" ]; then
    step "Building SDL2 $SDL_VERSION (once; takes a minute or two)"
    TGZ="$WORK/SDL2-$SDL_VERSION.tar.gz"
    [ -f "$TGZ" ] || curl -fL --progress-bar -o "$TGZ" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"
    echo "$SDL_SHA256  $TGZ" | shasum -a 256 -c - >/dev/null \
        || { rm -f "$TGZ"; fail "the SDL2 download is damaged (checksum differs). Run the script again."; }
    rm -rf "$WORK/sdl-src" "$WORK/sdl-build"
    mkdir -p "$WORK/sdl-src"
    tar xzf "$TGZ" -C "$WORK/sdl-src" --strip-components 1
    cmake -S "$WORK/sdl-src" -B "$WORK/sdl-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
        -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" >"$WORK/sdl-configure.log" \
        || fail "SDL2 configure failed, see build-mac/sdl-configure.log"
    cmake --build "$WORK/sdl-build" >"$WORK/sdl-build.log" \
        || fail "SDL2 build failed, see build-mac/sdl-build.log"
    cmake --install "$WORK/sdl-build" >/dev/null
fi
echo "ok: SDL2 $SDL_VERSION in build-mac/sdl"

# ---- the .app: a folder with the program, an Info.plist and the icon ----
make_app(){   # make_app <program> <app folder> [icon.icns]
    local bin="$1" app="$2" icns="${3:-}"
    rm -rf "$app"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
    cp "$bin" "$app/Contents/MacOS/PrinceOfPersia"
    local iconline=""
    if [ -n "$icns" ] && [ -f "$icns" ]; then
        cp "$icns" "$app/Contents/Resources/pop1.icns"
        iconline="<key>CFBundleIconFile</key><string>pop1</string>"
    fi
    cat >"$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$APP_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_NAME</string>
  <key>CFBundleExecutable</key><string>PrinceOfPersia</string>
  <key>CFBundleIdentifier</key><string>io.github.rufusfrederiks.pop1-recomp</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  $iconline
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.arcade-games</string>
  <key>NSHumanReadableCopyright</key><string>Unofficial fan project. Game (c) 1992 Jordan Mechner / Broderbund.</string>
</dict>
</plist>
PLIST
    plutil -lint "$app/Contents/Info.plist" >/dev/null || fail "Info.plist is not valid"
    # Apple Silicon only runs signed programs; an ad-hoc signature (no Apple
    # account needed) is enough for an app built on this Mac.
    codesign --force --sign - "$app" >/dev/null 2>&1 || fail "could not sign the app"
    codesign --verify "$app" || fail "the app's signature does not verify"
}

if [ $CHECK = 1 ]; then
    step "Check: compiling the hand-written code against this SDL2"
    cmake -S . -B "$WORK/check" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DPOP1_SOURCES_ONLY=ON -DCMAKE_PREFIX_PATH="$SDL_PREFIX" >/dev/null
    cmake --build "$WORK/check"
    step "Check: making and signing a test app"
    make_app /usr/bin/true "$WORK/check/Test.app"
    "$WORK/check/Test.app/Contents/MacOS/PrinceOfPersia" && echo "ok: test app made, signed and runs"
    echo
    echo "Check passed."
    exit 0
fi

# ---- 6. the game: lift the code from the disks, then build ----
step "Reading the disks and translating the game's code (tools/prepare.py)"
python tools/prepare.py "$DISKS"

step "Compiling the game (the first time takes a few minutes)"
cmake -S . -B "$WORK/game" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPOP1_STANDALONE=ON -DCMAKE_PREFIX_PATH="$SDL_PREFIX" >/dev/null
cmake --build "$WORK/game"

# ---- 7. wrap it as a Mac app ----
step "Making $APP_NAME.app"
python tools/make_icon.py work/app/rsrc "$WORK/pop1.icns" >/dev/null || echo "(no icon: carrying on without)"
APP="$ROOT/game/$APP_NAME.app"
make_app "$ROOT/game/PrinceOfPersia" "$APP" "$WORK/pop1.icns"
rm -f "$ROOT/game/PrinceOfPersia"     # it now lives inside the app

cat <<DONE

Done: game/$APP_NAME.app

  Play:  double-click it in Finder, or run  open "game/$APP_NAME.app"
         (drag it to Applications to keep it there)
  Keys:  Esc = menu, F5/F9 = save/load state, F11 = fullscreen
         (on a MacBook, hold fn for the F keys)
  Saves and settings: ~/Library/Application Support/pop1-recomp/PrinceOfPersia/
DONE
