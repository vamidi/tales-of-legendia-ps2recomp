# TOLRecomp — native PC port of Dragon Quest 8 (PS2) via static recompilation.
# Licensed under GPL-3.0 (see LICENSE). No game code or assets are built into
# this tree; recompiled output is generated locally by setup.py into
# build/generated/ (gitignored) and linked against the runtime here.

cmake_minimum_required(VERSION 3.24)

# Respect explicit compiler choices; otherwise prefer clang for generated code.
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    find_program(TOL_DEFAULT_CLANG clang)
    if(TOL_DEFAULT_CLANG)
        set(CMAKE_C_COMPILER "${TOL_DEFAULT_CLANG}" CACHE FILEPATH "C compiler")
    endif()
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(TOL_DEFAULT_CLANGXX clang++)
    if(TOL_DEFAULT_CLANGXX)
        set(CMAKE_CXX_COMPILER "${TOL_DEFAULT_CLANGXX}" CACHE FILEPATH "C++ compiler")
    endif()
endif()

# Regeneration rewrites the corpus; ccache reuses unchanged function bodies.
find_program(TOL_CCACHE ccache)
if(TOL_CCACHE)
    message(STATUS "TOLRecomp: using ccache at ${TOL_CCACHE}")
    set(CMAKE_C_COMPILER_LAUNCHER "${TOL_CCACHE}" CACHE STRING "C compiler launcher")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${TOL_CCACHE}" CACHE STRING "C++ compiler launcher")
endif()

# Version: `git describe` when available, otherwise a dev placeholder.
set(TOL_VERSION "0.0.0-dev")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                describe --tags --always --dirty
        OUTPUT_VARIABLE TOL_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE TOL_GIT_DESCRIBE_RESULT)
    if(TOL_GIT_DESCRIBE_RESULT EQUAL 0 AND NOT TOL_GIT_DESCRIBE STREQUAL "")
        set(TOL_VERSION "${TOL_GIT_DESCRIBE}")
    endif()
endif()

project(TOLRecomp
    VERSION 0.0.0
    DESCRIPTION "Dragon Quest 8 static recompilation"
    LANGUAGES C CXX)

# Apple's ar does not accept Ninja's @response files, which the 12k-file corpus
# needs as one archive. Without llvm-ar it is split into archives small enough
# to need none (src/runtime/CMakeLists.txt).
set(TOL_SPLIT_GENERATED_ARCHIVE OFF)
if(APPLE AND TOL_LINK_GENERATED)
    find_program(TOL_LLVM_AR NAMES llvm-ar
        HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
    if(TOL_LLVM_AR)
        set(CMAKE_AR "${TOL_LLVM_AR}")
    else()
        set(TOL_SPLIT_GENERATED_ARCHIVE ON)
        message(STATUS "TOLRecomp: no llvm-ar; the generated corpus is archived in parts")
    endif()
endif()

message(STATUS "TOLRecomp version: ${TOL_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(TOL_BUILD_PS2RECOMP
    "Build PS2Recomp tools/runtime; configure may fetch dependencies."
    OFF)

option(TOL_LINK_GENERATED
    "Link the locally generated game corpus; enables TOL_BUILD_PS2RECOMP."
    OFF)

set(TOL_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/src/runner"
    CACHE PATH "Directory of recompiled .cpp files produced by ps2_recomp.")
set(TOL_VU_PROGRAMS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/generated/SLUS_212.07-vu1"
    CACHE PATH "VU1 programs setup.py rebuilt from the game's ELF, compiled into the runtime.")

set(TOL_GENERATED_SIMD_FLAGS "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$")
    set(TOL_GENERATED_SIMD_FLAGS "-msse4.1")
endif()
# Generated memory accesses alias across widths and guest arithmetic wraps.
# Its x86 intrinsics also require SSE4.1, independently of runtime target flags.
set(TOL_GENERATED_OPTFLAGS "-O2 -g0 ${TOL_GENERATED_SIMD_FLAGS} -fno-strict-aliasing -fwrapv" CACHE STRING
    "Space-separated corpus flags; retain aliasing, wraparound, and required SIMD flags.")
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # cl knows none of those flags but -O2, and warned about the others for
    # every file. Without /Zc:inline it also keeps, in each of the 12k
    # objects, every inline function and variable of the runtime and STL
    # headers they include, used or not, which lib.exe then has to archive.
    set(TOL_GENERATED_OPTFLAGS "/O2 /Zc:inline")
endif()

if(TOL_LINK_GENERATED AND NOT TOL_BUILD_PS2RECOMP)
    message(STATUS "TOL_LINK_GENERATED=ON: forcing TOL_BUILD_PS2RECOMP=ON")
    set(TOL_BUILD_PS2RECOMP ON CACHE BOOL
        "Build the PS2Recomp submodule (forced on by TOL_LINK_GENERATED)." FORCE)
endif()

# SIMDe supplies the generated x86 intrinsic operations on ARM64 hosts.
add_library(tol_simde INTERFACE)
include(cmake/TolHostSimd.cmake)
target_include_directories(tol_simde SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/simde")

if(TOL_BUILD_PS2RECOMP)
    message(STATUS
        "TOL_BUILD_PS2RECOMP=ON: see https://github.com/Sinan-Karakaya/TOL-Recompiled/wiki/Building for host dependencies.")
    # The game does not need the standalone studio GUI or upstream test runner.
    set(PS2X_BUILD_STUDIO OFF CACHE BOOL "TOLRecomp: studio GUI not required" FORCE)
    set(PS2X_BUILD_TEST OFF CACHE BOOL "TOLRecomp: upstream tests not required" FORCE)
    if(TOL_LINK_GENERATED)
        # setup.py runs the recompiler separately before this runtime build.
        set(PS2X_BUILD_RECOMP OFF CACHE BOOL
            "TOLRecomp: use the standalone ps2_recomp binary" FORCE)
        set(PS2X_BUILD_ANALYZER OFF CACHE BOOL
            "TOLRecomp: use the standalone ps2_analyzer binary" FORCE)
        set(PS2X_BUILD_RUNTIME ON CACHE BOOL
            "TOLRecomp: need the ps2_runtime target to link tol" FORCE)
        # The game decrypts MVI streams; FFmpeg decodes them. Without these
        # optional libraries the game still builds, and shows garbage for movies.
        find_package(PkgConfig QUIET)
        set(TOL_HAVE_FFMPEG OFF)
        if(WIN32)
            # The runtime downloads its own prebuilt FFmpeg on Windows and the
            # launcher's payload carries none, so pkg-config never finds one
            # there. Probing turned movie decode off for every Windows build.
            set(TOL_HAVE_FFMPEG ON)
        elseif(PKG_CONFIG_FOUND)
            pkg_check_modules(TOL_FFMPEG_PROBE QUIET
                libavcodec libavformat libavutil libswresample libswscale)
            if(TOL_FFMPEG_PROBE_FOUND)
                set(TOL_HAVE_FFMPEG ON)
            endif()
        endif()
        if(WIN32)
            message(STATUS "TOLRecomp: FFmpeg comes from the runtime's prebuilt download; MPEG video decode enabled")
        elseif(TOL_HAVE_FFMPEG)
            message(STATUS "TOLRecomp: FFmpeg found (libavcodec ${TOL_FFMPEG_PROBE_libavcodec_VERSION}); MPEG video decode enabled")
        else()
            message(STATUS "TOLRecomp: FFmpeg not found; movies will show garbage. "
                           "Install ffmpeg-devel (or ffmpeg-free-devel) to play them.")
        endif()
        set(PS2X_ENABLE_FFMPEG ${TOL_HAVE_FFMPEG} CACHE BOOL
            "TOLRecomp: FFmpeg-backed MPEG decode, enabled when the libs are present" FORCE)
        set(PS2X_ENABLE_DEBUG_UI OFF CACHE BOOL
            "TOLRecomp: skip imgui/rlImGui, not needed to link ps2_runtime" FORCE)
        # Declared here first so the runtime's own declaration of it, under an
        # older CMake policy, keeps the directory appended below.
        set(PS2X_VU_PROGRAM_PROFILES "" CACHE STRING
            "Local VU1 program profile directories to compile; never distributed")
        if(EXISTS "${TOL_VU_PROGRAMS_DIR}/entries.txt")
            list(APPEND PS2X_VU_PROGRAM_PROFILES "${TOL_VU_PROGRAMS_DIR}")
        else()
            message(STATUS "TOLRecomp: no VU1 programs in ${TOL_VU_PROGRAMS_DIR}; "
                           "run setup.py recompile, then configure again, to compile them.")
        endif()
    endif()
    add_subdirectory(thirdparty/PS2Recomp)

    if(TOL_LINK_GENERATED AND TARGET ps2_runtime)
        # Keep --trace-printf available even when the compiler omits _DEBUG.
        target_compile_definitions(ps2_runtime PRIVATE RUNTIME_DECI2CALL)
    endif()
    if(MSVC AND TARGET ps2_runtime)
        # The runtime asks for whole-program optimization: /GL objects hold no
        # machine code, and link.exe compiles all of them at once, the VU1
        # programs' 350 MB among them. Linking the game took hours and 16 GB
        # that way, for code the translated game, most of the executable,
        # never got optimized with anyway.
        set_property(TARGET ps2_runtime PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
        get_target_property(TOL_RUNTIME_OPTIONS ps2_runtime COMPILE_OPTIONS)
        list(REMOVE_ITEM TOL_RUNTIME_OPTIONS /GL)
        set_property(TARGET ps2_runtime PROPERTY COMPILE_OPTIONS "${TOL_RUNTIME_OPTIONS}")
    endif()
endif()

add_subdirectory(src)
# tools/runtime is Dragon Quest 8's diagnostic harness (render cadence and a GS fan probe, both pinned to
# TOL guest addresses). This port has no copy, so it is not added.

