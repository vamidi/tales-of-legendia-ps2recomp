# The project's icon (res/icon, drawn by make_icon.py) on an executable:
# Windows shows the one in its resources, macOS the one in its app bundle.
# Linux executables carry none, so there the window sets its own.

include_guard(GLOBAL)

function(tol_add_icon target)
    set(icons "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../res/icon")
    # The TOL icon assets are not part of this port; skip cleanly so a tree without res/icon configures.
    if(WIN32 AND EXISTS "${icons}/tol.rc")
        # rc.exe and windres find tol.ico beside the .rc file.
        target_sources(${target} PRIVATE "${icons}/tol.rc")
    elseif(APPLE)
        get_target_property(bundle ${target} MACOSX_BUNDLE)
        if(bundle AND EXISTS "${icons}/tol.icns")
            target_sources(${target} PRIVATE "${icons}/tol.icns")
            set_source_files_properties("${icons}/tol.icns" TARGET_DIRECTORY ${target}
                PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
            set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE tol.icns)
        endif()
    endif()
endfunction()

# The window icon, as the kWindowIconBmp array sdlgpu_window_icon.cpp hands to
# SDL_SetWindowIcon. Only Linux needs it: SDL takes Windows' from the
# executable, and macOS shows the bundle's.
function(tol_embed_window_icon target)
    if(WIN32 OR APPLE)
        return()
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}_window_icon.cpp")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../res/icon/tol-window.bmp"
                "-DOUTPUT=${output}" -DSYMBOL=kWindowIconBmp
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/TolEmbedBinary.cmake"
        DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../res/icon/tol-window.bmp"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/TolEmbedBinary.cmake"
        COMMENT "Embedding the window icon"
        VERBATIM)
    target_sources(${target} PRIVATE "${output}")
endfunction()
