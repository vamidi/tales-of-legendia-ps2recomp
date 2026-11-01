# TOL_LINK_GENERATED links the translated game corpus and PS2 runtime.

add_executable(tol
    main.cpp
)

# The ELF, the disc image, saves and settings are opened by narrow paths.
include("${CMAKE_SOURCE_DIR}/cmake/TolUtf8CodePage.cmake")
tol_use_utf8_code_page(tol)
include("${CMAKE_SOURCE_DIR}/cmake/TolIcon.cmake")
tol_add_icon(tol)

target_compile_definitions(tol PRIVATE TOL_VERSION="${TOL_VERSION}")

# Overlays require separately translated corpora, so linking is opt-in.
option(TOL_LINK_OVERLAYS
    "Link the six translated MWo3 overlays; requires TOL_LINK_GENERATED=ON"
    OFF)

if(TOL_LINK_GENERATED)
    target_sources(tol PRIVATE screen_size.cpp vu_bounds.cpp)
    if(NOT TARGET ps2_runtime)
        message(FATAL_ERROR
            "TOL_LINK_GENERATED=ON but the ps2_runtime target does not exist. "
            "Check that thirdparty/PS2Recomp/ps2xRuntime configured successfully "
            "and its platform dependencies are installed.")
    endif()

    file(GLOB TOL_GENERATED_SOURCES CONFIGURE_DEPENDS
        "${TOL_GENERATED_DIR}/*.cpp")

    # This corpus has a few translation units far larger than TOL's (the recompiler emitted single functions of up
    # to 9 MB / 226k lines, plus register_functions.cpp at 20 MB; TOL caps at 0.8 MB). cl.exe exhausts its heap on
    # them at /O2 even with one job, so they are built without the PCH and without optimisation. They are slow to
    # compile and to run, but they compile; everything else keeps the fast path.
    set(TOL_HEAVY_SOURCES)
    foreach(TOL_SRC ${TOL_GENERATED_SOURCES})
        file(SIZE "${TOL_SRC}" TOL_SRC_SIZE)
        if(TOL_SRC_SIZE GREATER 2097152)
            list(APPEND TOL_HEAVY_SOURCES "${TOL_SRC}")
        endif()
    endforeach()
    if(TOL_HEAVY_SOURCES AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        list(LENGTH TOL_HEAVY_SOURCES TOL_HEAVY_COUNT)
        message(STATUS "tol: ${TOL_HEAVY_COUNT} oversized translation units build without PCH at /Od")
        set_source_files_properties(${TOL_HEAVY_SOURCES} PROPERTIES
            SKIP_PRECOMPILE_HEADERS ON
            COMPILE_OPTIONS "/Od")
    endif()

    list(LENGTH TOL_GENERATED_SOURCES TOL_GENERATED_SOURCE_COUNT)
    if(TOL_GENERATED_SOURCE_COUNT EQUAL 0)
        message(FATAL_ERROR
            "TOL_LINK_GENERATED=ON but no .cpp files were found under "
            "${TOL_GENERATED_DIR}. Regenerate with: "
            "./build/ps2recomp-standalone/ps2xRecomp/ps2_recomp "
            "config/SLUS_212.07/tol.toml")
    endif()
    message(STATUS
        "tol: linking ${TOL_GENERATED_SOURCE_COUNT} generated translation "
        "units from ${TOL_GENERATED_DIR}")

    # Isolate corpus optimization flags and rebuilds from the launcher. One
    # archive, or parts of 1,500 files whose ar command needs no response file
    # (top-level CMakeLists.txt); the first part owns the precompiled header.
    separate_arguments(TOL_GENERATED_OPTFLAGS_LIST UNIX_COMMAND "${TOL_GENERATED_OPTFLAGS}")
    set(TOL_GENERATED_PART_SIZE ${TOL_GENERATED_SOURCE_COUNT})
    if(TOL_SPLIT_GENERATED_ARCHIVE)
        set(TOL_GENERATED_PART_SIZE 1500)
    endif()
    set(TOL_GENERATED_TARGETS)
    set(TOL_GENERATED_PART_BEGIN 0)
    while(TOL_GENERATED_PART_BEGIN LESS TOL_GENERATED_SOURCE_COUNT)
        list(LENGTH TOL_GENERATED_TARGETS TOL_GENERATED_PART)
        set(TOL_GENERATED_TARGET tol_generated)
        if(TOL_GENERATED_PART GREATER 0)
            set(TOL_GENERATED_TARGET tol_generated_${TOL_GENERATED_PART})
        endif()
        list(SUBLIST TOL_GENERATED_SOURCES ${TOL_GENERATED_PART_BEGIN} ${TOL_GENERATED_PART_SIZE}
             TOL_GENERATED_PART_SOURCES)
        add_library(${TOL_GENERATED_TARGET} STATIC ${TOL_GENERATED_PART_SOURCES})
        target_link_libraries(${TOL_GENERATED_TARGET} PRIVATE tol_host_simd)
        target_include_directories(${TOL_GENERATED_TARGET} PRIVATE
            "${TOL_GENERATED_DIR}"
            "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/include"
            "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/include/runtime"
            "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xIOP/include"
            "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/src/lib/Kernel"
        )
        target_compile_options(${TOL_GENERATED_TARGET} PRIVATE ${TOL_GENERATED_OPTFLAGS_LIST})
        if(TOL_GENERATED_PART EQUAL 0)
            # Every function includes the same runtime and large generated declarations.
            target_precompile_headers(tol_generated PRIVATE
                <ps2_runtime_macros.h> <ps2_runtime.h>
                <ps2_recompiled_functions.h> <ps2_recompiled_stubs.h>
                <ps2_syscalls.h> <ps2_stubs.h>)
        else()
            target_precompile_headers(${TOL_GENERATED_TARGET} REUSE_FROM tol_generated)
        endif()
        list(APPEND TOL_GENERATED_TARGETS ${TOL_GENERATED_TARGET})
        math(EXPR TOL_GENERATED_PART_BEGIN "${TOL_GENERATED_PART_BEGIN} + ${TOL_GENERATED_PART_SIZE}")
    endwhile()

    # Prefer lld for the large generated corpus.
    find_program(TOL_LLD_PROGRAM ld.lld)
    if(TOL_LLD_PROGRAM AND NOT APPLE)
        message(STATUS "tol: using lld (${TOL_LLD_PROGRAM}) to link the generated corpus")
        target_link_options(tol PRIVATE -fuse-ld=lld)
    endif()

    target_link_libraries(tol PRIVATE ${TOL_GENERATED_TARGETS} ps2_runtime)
    target_compile_definitions(tol PRIVATE TOL_LINK_GENERATED=1)
    # On Windows the runtime's FFmpeg is DLLs; the launcher starts tol.exe from
    # this build folder, so they go beside it. Elsewhere this does nothing.
    ps2x_stage_ffmpeg_runtime_dlls(tol)

    # The hardware GS backend, when SDL3 was found. Selected at runtime with
    # --gs=sdlgpu; without it tol still runs on the software rasteriser.
    if(TARGET tol_gfx_sdlgpu)
        # The in-game menu (F1) draws over the same window.
        target_link_libraries(tol PRIVATE tol_gfx_sdlgpu tol_ui)
        target_compile_definitions(tol PRIVATE TOL_HAS_SDLGPU=1)
        message(STATUS "tol: SDL GPU backend available (--gs=sdlgpu)")
    else()
        message(STATUS "tol: SDL GPU backend not built; --gs=sdlgpu unavailable")
    endif()

    if(TOL_LINK_OVERLAYS)
        add_subdirectory(overlay)
        target_link_libraries(tol PRIVATE tol_overlay)
        # main.cpp guards its tol::overlay::install(runtime) call on this.
        target_compile_definitions(tol PRIVATE TOL_LINK_OVERLAYS=1)
    endif()
    # main.cpp includes ps2_recompiled_functions.h so it can name individual
    # recompiled functions -- --trace-printf redirects the stubbed debug-log
    # wrappers onto one of them.
    target_include_directories(tol PRIVATE "${TOL_GENERATED_DIR}")
else()
    if(TOL_LINK_OVERLAYS)
        message(FATAL_ERROR
            "TOL_LINK_OVERLAYS=ON needs TOL_LINK_GENERATED=ON: the overlay manager "
            "hooks a function (0x00100320) that lives in the main recompiled corpus.")
    endif()
    message(STATUS "tol: TOL_LINK_GENERATED=OFF -- building stub executable only")
endif()

if(APPLE AND TOL_LINK_GENERATED)
    set(TOL_MACOS_ELF "${CMAKE_SOURCE_DIR}/../Extracted_Usa/SLUS_212.07"
        CACHE FILEPATH "User-supplied ELF for the local macOS launcher")
    set(TOL_MACOS_ISO "${CMAKE_SOURCE_DIR}/../TOL_USA.iso"
        CACHE FILEPATH "User-supplied ISO for the local macOS launcher")
    set(TOL_MACOS_BINARY "$<TARGET_FILE:tol>")
    set(TOL_MACOS_APP "${CMAKE_BINARY_DIR}/TOLRecomp.app")
    configure_file("${CMAKE_SOURCE_DIR}/tools/macos/Info.plist"
        "${TOL_MACOS_APP}/Contents/Info.plist" COPYONLY)
    configure_file("${CMAKE_SOURCE_DIR}/res/icon/tol.icns"
        "${TOL_MACOS_APP}/Contents/Resources/TOLRecomp.icns" COPYONLY)
    configure_file("${CMAKE_SOURCE_DIR}/tools/macos/launch.sh.in"
        "${CMAKE_CURRENT_BINARY_DIR}/macos-launch.sh" @ONLY)
    file(GENERATE OUTPUT "${TOL_MACOS_APP}/Contents/MacOS/TOLRecomp"
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/macos-launch.sh"
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                         GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()
