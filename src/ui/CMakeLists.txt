# tol_ui -- the in-game menu, drawn with Dear ImGui over the SDL GPU backend's
# window. Built wherever that backend is: the game links it, and the dump
# harness builds it for its tests and the menu preview.

if(NOT TARGET tol_gfx_sdlgpu)
    return()
endif()

get_filename_component(TOL_UI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(TOL_IMGUI_DIR "${TOL_UI_ROOT}/thirdparty/imgui")
if(NOT EXISTS "${TOL_IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "Dear ImGui not found under ${TOL_IMGUI_DIR}. "
        "Run: git submodule update --init thirdparty/imgui")
endif()

# Imported targets are local to the directory that found them. SDL 3.2 is
# enough: the sources guard what only SDL 3.4 has.
if(TOL_GFX_SDL_TARGET STREQUAL "SDL3::SDL3" AND NOT TARGET SDL3::SDL3)
    find_package(SDL3 3.2 CONFIG REQUIRED)
elseif(TOL_GFX_SDL_TARGET STREQUAL "PkgConfig::TOL_GFX_SDL3" AND NOT TARGET PkgConfig::TOL_GFX_SDL3)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TOL_GFX_SDL3 REQUIRED IMPORTED_TARGET sdl3>=3.2)
endif()

add_library(tol_imgui STATIC
    "${TOL_IMGUI_DIR}/imgui.cpp"
    "${TOL_IMGUI_DIR}/imgui_draw.cpp"
    "${TOL_IMGUI_DIR}/imgui_tables.cpp"
    "${TOL_IMGUI_DIR}/imgui_widgets.cpp"
    "${TOL_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${TOL_IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp"
)
target_include_directories(tol_imgui SYSTEM PUBLIC "${TOL_IMGUI_DIR}" "${TOL_IMGUI_DIR}/backends")
target_compile_definitions(tol_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(tol_imgui PUBLIC ${TOL_GFX_SDL_TARGET})

# Roboto Medium from Dear ImGui's own fonts (Apache 2.0), compiled in so the
# menu needs no files beside the executable.
set(TOL_UI_FONT "${TOL_IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf")
set(TOL_UI_FONT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/ui_font_data.cpp")
add_custom_command(
    OUTPUT "${TOL_UI_FONT_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${TOL_UI_FONT}" "-DOUTPUT=${TOL_UI_FONT_SOURCE}"
            -DSYMBOL=kUiFontData -P "${TOL_UI_ROOT}/cmake/TolEmbedBinary.cmake"
    DEPENDS "${TOL_UI_FONT}" "${TOL_UI_ROOT}/cmake/TolEmbedBinary.cmake"
    COMMENT "Embedding the menu font"
    VERBATIM)

add_library(tol_ui STATIC
    ui_glyphs.cpp
    ui_overlay.cpp
    ui_pages.cpp
    ui_script.cpp
    ui_settings.cpp
    ui_style.cpp
    ui_widgets.cpp
    "${TOL_UI_FONT_SOURCE}"
)
target_include_directories(tol_ui PUBLIC "${TOL_UI_ROOT}/src")
target_link_libraries(tol_ui PUBLIC tol_gfx_sdlgpu tol_imgui)

message(STATUS "tol ui: in-game menu enabled (Dear ImGui)")
