#pragma once

#include "runtime/gs/gs_types.h"
#include "runtime/native/native_gs_hook.h"

#include <cstdint>
#include <vector>

struct GSDebugSnapshot;

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;
 
    virtual bool UsesRawCommands() const { return false; }
    virtual void ProcessGIF(uint32_t, const uint8_t *, uint32_t) {}
    virtual void WriteRegisterRaw(uint8_t, uint64_t) {}
    virtual void ReadRegisterState(GSDebugSnapshot &) const {} 
    virtual uint64_t GetReadbackCount() const { return 0; }

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;
    // From boubou666/PS2Recomp's native renderer. Defaults keep every existing
    // backend unchanged: no native draw points, and presentation on the caller.
    virtual bool PresentsOnDrawThread() const { return false; }

    // The drawing registers a native draw marker hands on with it, in GsDrawTarget's raw GS layouts.
    // Written against GSDrawState, not GSContext: this tree's GSContext is a context's four registers
    // (frame/scissor/tex0/xyoffset), while the fork's also carries the whole drawing state, so the
    // registers the hook reads (TEXA, ALPHA, TEST, CLAMP, FOGCOL, the Z write mask) live on the state.
    static void fillNativeRegisters(ps2x::native::GsDrawTarget &target, const GSDrawState &state)
    {
        const GSTex0Reg &t = state.context.tex0;
        target.tex0 = static_cast<uint64_t>(t.tbp0 & 0x3FFFu) | (static_cast<uint64_t>(t.tbw & 0x3Fu) << 14) |
                      (static_cast<uint64_t>(t.psm & 0x3Fu) << 20) | (static_cast<uint64_t>(t.tw & 0xFu) << 26) |
                      (static_cast<uint64_t>(t.th & 0xFu) << 30) | (static_cast<uint64_t>(t.tcc & 1u) << 34) |
                      (static_cast<uint64_t>(t.tfx & 3u) << 35) | (static_cast<uint64_t>(t.cbp & 0x3FFFu) << 37) |
                      (static_cast<uint64_t>(t.cpsm & 0xFu) << 51) | (static_cast<uint64_t>(t.csm & 1u) << 55) |
                      (static_cast<uint64_t>(t.csa & 0x1Fu) << 56) | (static_cast<uint64_t>(t.cld & 7u) << 61);
        target.texa = state.texaRaw;
        target.alpha = state.context.alpha;
        target.test = state.context.test;
        target.clamp = state.context.clamp;
        target.fogcol = static_cast<uint32_t>(state.fogR) | (static_cast<uint32_t>(state.fogG) << 8u) |
                        (static_cast<uint32_t>(state.fogB) << 16u);
        target.zmask = state.context.zbuf.zmask;
    }

    // A native renderer's draws at this point of the stream (GS::queueNativeDraw): the hook with the draw
    // state at the marker, and no render target (a renderer that owns one overrides this and adds it).
    virtual void NativeDraw(uint64_t group, const GSDrawState &state)
    {
        if (const ps2x::native::GsDrawHook hook = ps2x::native::gsDrawHook())
        {
            ps2x::native::GsDrawTarget target;
            fillNativeRegisters(target, state);
            target.xOffset = state.context.xyoffset.ofx;
            target.yOffset = state.context.xyoffset.ofy;
            target.scissorX0 = state.context.scissor.x0;
            target.scissorY0 = state.context.scissor.y0;
            target.scissorX1 = state.context.scissor.x1;
            target.scissorY1 = state.context.scissor.y1;
            hook(group, target);
        }
    }

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;
};
