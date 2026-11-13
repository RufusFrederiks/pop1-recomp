cmake_minimum_required(VERSION 3.16)
project(pop1_recomp C)
set(CMAKE_C_STANDARD 11)

# Run tools/prepare.py first: it fills src/gen/ (lifted C) and work/ from your
# own disk images. Neither is in git.
#
# POP1_SOURCES_ONLY is for the automatic checks on GitHub, which never have
# the game: it compiles everything written by hand (src/, macrecomp's runtime)
# but not the lifted code, and so makes no program. It shows that the code
# still builds on Windows, Linux and macOS.
option(POP1_SOURCES_ONLY "Compile the hand-written code only (no game files needed, no program)" OFF)
file(GLOB POP1_GEN ${CMAKE_SOURCE_DIR}/src/gen/code_*.c)
if(POP1_SOURCES_ONLY)
    set(POP1_GEN "")
elseif(NOT POP1_GEN)
    message(FATAL_ERROR "src/gen/ is empty: run `python tools/prepare.py` first (see README)")
endif()

set(MR ${CMAKE_SOURCE_DIR}/ext/macrecomp)

# macrecomp's runtime, compiled here rather than through add_subdirectory so
# that two of its symbols can be renamed: the game's own layer (src/traps.c)
# then sits in front of the Toolbox dispatcher and the loop time slice without
# a single change inside the submodule.
# One build-time edit to a copy of m68k.c (the submodule itself is untouched):
# macrecomp advances the low-memory Ticks global on every lifted call, a
# pseudo-clock that lets HyperCard's busy-waits finish. PoP's music driver
# times its mixing off Ticks, so a clock that runs as fast as the code does
# makes it mix several times too much. src/traps.c keeps Ticks on the real
# 60 Hz clock instead. If macrecomp changes this line, the build stops here.
file(READ ${MR}/runtime/m68k.c MR_M68K_SRC)
set(MR_TICK_CALL "    bump_ticks();\n    if (g_shadow_sp < 512) g_shadow[g_shadow_sp] = addr;")
string(FIND "${MR_M68K_SRC}" "${MR_TICK_CALL}" MR_TICK_AT)
if(MR_TICK_AT EQUAL -1)
    message(FATAL_ERROR "ext/macrecomp/runtime/m68k.c changed: the per-call Ticks bump "
                        "that CMakeLists.txt removes was not found. See DEV-LOG.md.")
endif()
string(REPLACE "${MR_TICK_CALL}"
    "    /* pop1: per-call Ticks bump removed (see CMakeLists.txt) */\n    if (g_shadow_sp < 512) g_shadow[g_shadow_sp] = addr;"
    MR_M68K_SRC "${MR_M68K_SRC}")
file(WRITE ${CMAKE_BINARY_DIR}/mr/m68k.c "${MR_M68K_SRC}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MR}/runtime/m68k.c)

set(MR_RUNTIME
    ${CMAKE_BINARY_DIR}/mr/m68k.c
    ${MR}/runtime/textedit.c
    # These four are compiled through src/mr/*_state.c, which #include them
    # unchanged and add a function that saves and restores their file-local
    # state (for savestates, src/state.c).
    src/mr/quickdraw_state.c
    src/mr/dialog_state.c
    src/mr/files_state.c
    src/mr/toolbox_state.c
)
set_source_files_properties(src/mr/toolbox_state.c PROPERTIES
    COMPILE_DEFINITIONS "m68k_trap=mr_toolbox_trap")
set_source_files_properties(${CMAKE_BINARY_DIR}/mr/m68k.c PROPERTIES
    COMPILE_DEFINITIONS "m68k_time_slice=mr_core_time_slice")

set(POP1_SOURCES
    src/main.c
    src/traps.c
    src/sound.c
    src/input.c
    src/pad.c
    src/qd.c
    src/cqd.c
    src/state.c
    src/heap.c
    src/platform.c
    src/filters.c
    src/crt.c
    src/light.c
    src/gfx.c
    ${MR_RUNTIME}
)
if(POP1_SOURCES_ONLY)
    if(POP1_STANDALONE)
        message(FATAL_ERROR "POP1_SOURCES_ONLY and POP1_STANDALONE do not go together")
    endif()
    add_library(pop1 OBJECT ${POP1_SOURCES})
    # main.c once more as the single-file build has it: that is where each
    # system's folder for saves and settings is chosen.
    add_library(pop1_standalone_main OBJECT src/main.c)
    target_include_directories(pop1_standalone_main PRIVATE ${MR}/include src)
    target_compile_definitions(pop1_standalone_main PRIVATE
        MACRECOMP_NO_TRAP_STUB MACRECOMP_SDL POP_STANDALONE)
else()
    add_executable(pop1 ${POP1_SOURCES} ${POP1_GEN})
endif()
target_include_directories(pop1 PRIVATE ${MR}/include src)
target_compile_definitions(pop1 PRIVATE MACRECOMP_NO_TRAP_STUB MACRECOMP_SDL)

# The lifted code is ~3 MB of C with one label per instruction: optimising it
# hard costs minutes and buys little. Quiet its warnings; they are the
# lifter's, not ours.
if(POP1_GEN)
    set_source_files_properties(${POP1_GEN} PROPERTIES COMPILE_OPTIONS "-O1;-w")
endif()

# The single-file build for Linux and macOS: your game files packed inside
# (tools/embed.py), unpacked on first start into the per-user folder, where
# saves and settings go too (see main.c). Lands in game/PrinceOfPersia.
# Made from your own disks: for your own use, not for sharing.
#   cmake -S . -B build-game -DPOP1_STANDALONE=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-game
# On Windows the one-file exe comes from tools/cross-windows.sh --standalone.
option(POP1_STANDALONE "Build game/PrinceOfPersia with the game files inside" OFF)
if(POP1_STANDALONE)
    if(WIN32)
        message(FATAL_ERROR "On Windows, build the standalone exe with tools/cross-windows.sh --standalone")
    endif()
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/work/files/files.txt)
        message(FATAL_ERROR "work/files/ is empty: run `python3 tools/prepare.py` first (see README)")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(POP1_EMBEDDED ${CMAKE_SOURCE_DIR}/src/gen/embedded.c)
    add_custom_command(OUTPUT ${POP1_EMBEDDED}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed.py
        DEPENDS ${CMAKE_SOURCE_DIR}/tools/embed.py ${CMAKE_SOURCE_DIR}/work/files/files.txt
        COMMENT "Packing work/files into src/gen/embedded.c")
    target_sources(pop1 PRIVATE ${POP1_EMBEDDED})
    # megabytes of byte literals: nothing to optimise
    set_source_files_properties(${POP1_EMBEDDED} PROPERTIES COMPILE_OPTIONS "-O0")
    target_compile_definitions(pop1 PRIVATE POP_STANDALONE)
    set_target_properties(pop1 PROPERTIES
        OUTPUT_NAME PrinceOfPersia
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/game)
endif()

option(POP1_LOOPGUARD "Count backward branches (MRMAXLOOPS) to find spins" OFF)
if(POP1_LOOPGUARD)
    target_compile_definitions(pop1 PRIVATE MACRECOMP_LOOPGUARD)
endif()

find_package(SDL2 QUIET)
if(SDL2_FOUND)
    target_link_libraries(pop1 PRIVATE SDL2::SDL2)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    target_include_directories(pop1 PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_directories(pop1 PRIVATE ${SDL2_LIBRARY_DIRS})
    target_link_libraries(pop1 PRIVATE ${SDL2_LIBRARIES})
endif()
if(NOT MSVC)
    target_link_libraries(pop1 PRIVATE m)
endif()
