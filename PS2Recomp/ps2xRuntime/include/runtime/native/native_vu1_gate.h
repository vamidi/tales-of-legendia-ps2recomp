#ifndef PS2X_NATIVE_VU1_GATE_H
#define PS2X_NATIVE_VU1_GATE_H

// Which VU1 microprograms a native port draws itself.
//
// VU1 is where a PS2 game batches its 3D: a microprogram transforms vertices and kicks PATH1 packets for a model. A
// native model renderer takes such a program over - it reads the same VU1 code and data and draws the model on the
// host - so the emulated VU1 must NOT run it, and the GS stream must get a marker where its PATH1 output would have
// been (GS::queueNativeDraw), so the native draws sort with the GS draws around them.
//
// Two pieces, both plain pointers/atomics so any translation unit can set them and none has to know the renderer:
//
//   gate         - is this program drawn natively? Called with VU1 code memory and the MSCAL entry pc, so a renderer
//                  can identify the program by its code (Tales of Legendia uploads every program at pc 0, so the
//                  entry pc alone does not identify one).
//   group count  - how many native draw groups the port produces, numbered 1, 2, ... in stream order, as the port
//                  numbers them. A skipped program takes the next number; the marker is emitted only if the port
//                  draws that group. This is the "when a model group is available" condition: with no renderer (or
//                  a group the port does not draw) the marker is not emitted, and nothing is skipped either.
//
// Default state is inert: no gate, no groups, so the emulated VU1 runs everything exactly as before.

#include <atomic>
#include <cstdint>

namespace ps2x::native
{
    // Returns true when the microprogram at startPc is one the native port draws itself.
    using Vu1Gate = bool (*)(const uint8_t *vu1Code, uint32_t startPc);

    inline std::atomic<Vu1Gate> g_vu1Gate{nullptr};
    inline void setVu1Gate(Vu1Gate gate) { g_vu1Gate.store(gate, std::memory_order_release); }
    inline Vu1Gate vu1Gate() { return g_vu1Gate.load(std::memory_order_acquire); }
    inline bool vu1Skipped(const uint8_t *vu1Code, uint32_t startPc)
    {
        const Vu1Gate gate = vu1Gate();
        return gate != nullptr && gate(vu1Code, startPc);
    }

    // Groups 1..n are the ones the port draws (monotonic over the run, in stream order).
    inline std::atomic<uint64_t> g_nativeGroupCount{0};
    inline void setNativeGroupCount(uint64_t count) { g_nativeGroupCount.store(count, std::memory_order_release); }
    inline uint64_t nativeGroupCount() { return g_nativeGroupCount.load(std::memory_order_acquire); }
    inline bool nativeGroupAvailable(uint64_t group)
    {
        return group != 0u && group <= nativeGroupCount();
    }

    // Whether a continued program (MSCNT) is a new group of its own. A renderer that draws one model pass per
    // MSCAL leaves this off; one whose second pass is a separate group turns it on.
    inline std::atomic<bool> g_mscntGroups{false};
    inline void setMscntGroups(bool on) { g_mscntGroups.store(on, std::memory_order_release); }
    inline bool mscntGroups() { return g_mscntGroups.load(std::memory_order_acquire); }
}

#endif
