# Shared by the standalone GS dump harness and the game, so paths are
# relative to this file rather than either caller.

get_filename_component(TOL_GFX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(TOL_GFX_PS2X_RUNTIME "${TOL_GFX_ROOT}/thirdparty/PS2Recomp/ps2xRuntime")
include("${TOL_GFX_ROOT}/cmake/TolHostSimd.cmake")

if(NOT EXISTS "${TOL_GFX_PS2X_RUNTIME}/src/lib/gs/gs_frontend.cpp")
    message(FATAL_ERROR
        "ps2xRuntime GS sources not found under ${TOL_GFX_PS2X_RUNTIME}. "
        "Run: git submodule update --init --recursive")
endif()

# ---------------------------------------------------------------------------
# tol_gfx_gs -- backend-neutral GS support: local memory, swizzled pixel
# access, page tracking and the transfer engine. No graphics API here, so it
# can be unit-tested against GSCpuBackend with no device present.
# ---------------------------------------------------------------------------
set(TOL_GFX_GS_SOURCES
    gs/gs_vram.cpp
    gs/gs_transfer.cpp
    gs/gs_state.cpp
)
# GsVram's pixel accessors are backed by ps2xRuntime's swizzle tables. In the
# standalone dump-harness build nothing else compiles them, so they come in
# here; in the main build ps2_runtime already provides them and compiling them
# again would duplicate every GSMem symbol at the final link.
if(NOT TARGET ps2_runtime)
    list(APPEND TOL_GFX_GS_SOURCES
        "${TOL_GFX_PS2X_RUNTIME}/src/lib/gs/ps2_gs_memory.cpp")
endif()

add_library(tol_gfx_gs STATIC ${TOL_GFX_GS_SOURCES})
target_link_libraries(tol_gfx_gs PUBLIC tol_host_simd)

target_include_directories(tol_gfx_gs PUBLIC "${TOL_GFX_ROOT}/src")
target_include_directories(tol_gfx_gs SYSTEM PUBLIC
    "${TOL_GFX_PS2X_RUNTIME}/include"
    "${TOL_GFX_PS2X_RUNTIME}/include/runtime"
)
if(NOT TARGET ps2_runtime)
    set_source_files_properties(
        "${TOL_GFX_PS2X_RUNTIME}/src/lib/gs/ps2_gs_memory.cpp"
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# ---------------------------------------------------------------------------
# tol_gfx_sdlgpu -- the SDL3 GPU backend. Optional: without SDL3 the rest of
# the graphics tree, including the dump harness and the software backend,
# still builds.
# ---------------------------------------------------------------------------
option(TOL_GFX_ENABLE_SDLGPU "Build the SDL3 GPU hardware GS backend" ON)

set(TOL_GFX_SDL_TARGET "")
if(TOL_GFX_ENABLE_SDLGPU)
    find_package(SDL3 3.2 CONFIG QUIET)
    if(TARGET SDL3::SDL3)
        set(TOL_GFX_SDL_TARGET SDL3::SDL3)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(TOL_GFX_SDL3 QUIET IMPORTED_TARGET sdl3>=3.2)
            if(TARGET PkgConfig::TOL_GFX_SDL3)
                set(TOL_GFX_SDL_TARGET PkgConfig::TOL_GFX_SDL3)
            endif()
        endif()
    endif()
endif()

if(TOL_GFX_SDL_TARGET)
    include("${TOL_GFX_ROOT}/cmake/TolShaders.cmake")

    add_library(tol_gfx_sdlgpu STATIC
        backends/sdlgpu/sdlgpu_backend.cpp
        backends/sdlgpu/sdlgpu_device.cpp
        backends/sdlgpu/sdlgpu_display.cpp
        backends/sdlgpu/sdlgpu_input.cpp
        backends/sdlgpu/sdlgpu_targets.cpp
        backends/sdlgpu/sdlgpu_textures.cpp
        backends/sdlgpu/sdlgpu_window_icon.cpp
    )
    target_link_libraries(tol_gfx_sdlgpu PUBLIC tol_gfx_gs ${TOL_GFX_SDL_TARGET})
    include("${TOL_GFX_ROOT}/cmake/TolIcon.cmake")
    tol_embed_window_icon(tol_gfx_sdlgpu)

    tol_add_shader(tol_gfx_sdlgpu
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_draw.vert"
        STAGE vert SYMBOL kGsDrawVertSpirv)
    tol_add_shader(tol_gfx_sdlgpu
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_draw.frag"
        STAGE frag SYMBOL kGsDrawFragSpirv)
    tol_add_shader(tol_gfx_sdlgpu
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_reinterpret.vert"
        STAGE vert SYMBOL kGsReinterpretVertSpirv)
    tol_add_shader(tol_gfx_sdlgpu
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_reinterpret.frag"
        STAGE frag SYMBOL kGsReinterpretFragSpirv)
    tol_add_shader(tol_gfx_sdlgpu MSL_DECORATION_BINDING
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_display.frag"
        STAGE frag SYMBOL kGsDisplayFragSpirv)
    tol_add_shader(tol_gfx_sdlgpu MSL_DECORATION_BINDING
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_index8.frag"
        STAGE frag SYMBOL kGsIndex8FragSpirv)
    tol_add_shader(tol_gfx_sdlgpu MSL_DECORATION_BINDING
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_local_copy.frag"
        STAGE frag SYMBOL kGsLocalCopyFragSpirv)
    if(APPLE)
        tol_add_shader(tol_gfx_sdlgpu FRAMEBUFFER_FETCH
            SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_draw.frag"
            STAGE frag SYMBOL kGsDrawFetchSpirv)
    endif()

    message(STATUS "tol gfx: SDL3 GPU backend enabled")
elseif(TOL_GFX_ENABLE_SDLGPU)
    message(STATUS "tol gfx: SDL3 >= 3.2 not found -- hardware backend disabled")
endif()
# The menu (src/ui) links the same SDL.
set(TOL_GFX_SDL_TARGET "${TOL_GFX_SDL_TARGET}" PARENT_SCOPE)
