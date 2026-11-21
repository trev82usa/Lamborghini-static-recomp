cmake_minimum_required(VERSION 3.20)

# ============================================================================
# YOUR_GAME_NAME - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(Lamborghini C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Recompiled code reads guest memory through casted pointers (MEM32 of a byte
# buffer, floats through uint32 views) and relies on two's-complement wrap.
# Without these the optimiser may assume both away; clang -O3 does so more
# readily than GCC. Set before add_subdirectory so the runtime gets them too.
add_compile_options(-fno-strict-aliasing -fwrapv)

# Portable Linux binary: runs on glibc 2.34+ rather than only the build
# machine's (src/glibc_compat.h). Before add_subdirectory: the runtime too.
option(LAMBO_PORTABLE_GLIBC "Bind newer glibc symbol versions to old ones" ON)
if(LAMBO_PORTABLE_GLIBC AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_options(-include "${CMAKE_CURRENT_SOURCE_DIR}/src/glibc_compat.h")
endif()

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

if(WIN32)
    set(GAME_EXE_KIND WIN32)
endif()
add_executable(${PROJECT_NAME} ${GAME_EXE_KIND}
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

if(WIN32)
    # Windows SDK libraries needed at link time
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        dxguid
        $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
        winmm
        # Lets the fault handler resolve the host RIP back to the generated
        # function's symbol, i.e. to the guest function that faulted.
        dbghelp
    )
    if(XBOXRECOMP_SDL_HOST)
        # SDL2 host layer as on Linux (see xboxrecomp's XBOXRECOMP_SDL_HOST):
        # also the audio shim, which only needs SDL2.
        find_package(SDL2 REQUIRED)
        target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2)
        target_sources(${PROJECT_NAME} PRIVATE src/audio_shim.c)
        target_compile_definitions(${PROJECT_NAME} PRIVATE LAMBO_AUDIO_SHIM=1 XBOX_SDL_HOST=1)
        # audio_shim.c includes <SDL2/SDL.h>: the package's include root.
        target_include_directories(${PROJECT_NAME} PRIVATE "${SDL2_DIR}/../../../include")
        set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME Lamborghini)
    endif()
else()
    # Native Linux: SDL2 + OpenGL come in through the runtime libraries.
    # -rdynamic exports the generated functions (sub_XXXXXXXX) so dladdr can
    # name the guest function at a fault, as dbghelp does on Windows.
    find_package(Threads REQUIRED)
    find_package(SDL2 REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m SDL2::SDL2)
    # Audio (audio_shim.c): SDL2 output for the title's DirectSound use. Linux
    # only -- the Windows build has no SDL2 -- so main.c calls it under
    # LAMBO_AUDIO_SHIM.
    target_sources(${PROJECT_NAME} PRIVATE src/audio_shim.c)
    if(LAMBO_PORTABLE_GLIBC)
        target_sources(${PROJECT_NAME} PRIVATE src/glibc_compat.c)
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE LAMBO_AUDIO_SHIM=1)
    target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
    # No .exe suffix here, and "Lamborghini" is the data folder beside it.
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME Lamborghini.x86_64)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
