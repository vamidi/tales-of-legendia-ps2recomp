#include "gfx/backends/sdlgpu/sdlgpu_window.h"

#if !defined(_WIN32) && !defined(__APPLE__)
// res/icon/tol-window.bmp, embedded by tol_embed_window_icon (cmake/TolIcon.cmake).
extern const unsigned char kWindowIconBmp[];
extern const unsigned int kWindowIconBmp_size;
#endif

namespace tol::gfx {

void setWindowIcon(SDL_Window *window) {
#if !defined(_WIN32) && !defined(__APPLE__)
    SDL_Surface *icon = SDL_LoadBMP_IO(SDL_IOFromConstMem(kWindowIconBmp, kWindowIconBmp_size), true);
    if (!icon)
        return;
    SDL_SetWindowIcon(window, icon);
    SDL_DestroySurface(icon);
#else
    (void)window;
#endif
}

} // namespace tol::gfx
