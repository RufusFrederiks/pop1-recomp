cmake_minimum_required(VERSION 3.16)
project(pop1_recomp C)
set(CMAKE_C_STANDARD 11)

# Run tools/prepare.py first: it fills src/gen/ (lifted C) and work/ from your
# own disk images. Neither is in git.
file(GLOB POP1_GEN ${CMAKE_SOURCE_DIR}/src/gen/code_*.c)
if(NOT POP1_GEN)
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
    ${MR}/runtime/quickdraw.c
    ${MR}/runtime/dialog.c
    ${MR}/runtime/textedit.c
    ${MR}/runtime/files.c
    ${MR}/runtime/toolbox.c
)
set_source_files_properties(${MR}/runtime/toolbox.c PROPERTIES
    COMPILE_DEFINITIONS "m68k_trap=mr_toolbox_trap")
set_source_files_properties(${CMAKE_BINARY_DIR}/mr/m68k.c PROPERTIES
    COMPILE_DEFINITIONS "m68k_time_slice=mr_core_time_slice")

add_executable(pop1
    src/main.c
    src/traps.c
    src/sound.c
    src/input.c
    src/qd.c
    src/platform.c
    ${POP1_GEN}
    ${MR_RUNTIME}
)
target_include_directories(pop1 PRIVATE ${MR}/include src)
target_compile_definitions(pop1 PRIVATE MACRECOMP_NO_TRAP_STUB MACRECOMP_SDL)

# The lifted code is ~3 MB of C with one label per instruction: optimising it
# hard costs minutes and buys little. Quiet its warnings; they are the
# lifter's, not ours.
set_source_files_properties(${POP1_GEN} PROPERTIES COMPILE_OPTIONS "-O1;-w")

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
