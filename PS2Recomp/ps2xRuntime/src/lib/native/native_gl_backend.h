#pragma once

// GS raster backend implemented on the host GPU through OpenGL, replacing
// parallel-gs for the picture while keeping gs_frontend.cpp's GIF decoding.
//
// Opt-in only: the runtime installs it when PS2X_GS_NATIVE=1. The normal path
// (parallel-gs, or the software GSCpuBackend) is untouched, so a failure here
// can never break a working desktop run.

#include <cstdint>
#include <memory>

class GSRasterBackend;

namespace ps2x::native
{
    // False when no GL context is current or the GL entry points are missing;
    // the caller then keeps its existing backend.
    bool glAvailable();

    // The GS-facing backend. Feeds ps2x::native::Backend with the primitives
    // gs_frontend.cpp decodes. `vram`/`vramSize` are the 4 MiB GS local memory
    // handed to every raster backend.
    std::unique_ptr<GSRasterBackend> createGlGsBackend(uint8_t *vram, uint32_t vramSize);
}
