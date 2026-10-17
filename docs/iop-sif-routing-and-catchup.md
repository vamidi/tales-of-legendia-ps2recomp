# IOP hand-off: raw-SIF routing and the EE/IOP catch-up

> Design note for the "wire the IOP for real" workstream (session 8-9).
> Read this before touching `Kernel/Stubs/SIF.cpp`, `ps2_iop_transport.h`, or the
> service plumbing in `ps2xIOP/src/`.
>
> **Status:** implemented and verified - all 9 of the game's own IOP modules load
> into the emulated IOP, run, and answer their own RPCs (`E0100301` and
> `E0100302` are both 0). Stage C (file I/O on SID `0x80000001`) is still open.

---

## 1. The problem in one paragraph

This project statically recompiles the EE binary; the IOP is *not* recompiled, so
the game's IOP modules (DBCMAN, LIBSD, IXAUDIO, ...) never reach the EE image. The
runtime therefore ships an IOP **emulator** (`ps2xRuntime` ... `ps2xIOP`) that can
load ELF `.IRX` modules, run their MIPS, and execute their RPC server functions.
The missing link was the **EE-facing** half: the game's own statically linked
libkernel (`sceSifBindRpc` / `sceSifCallRpc`, not the runtime's stubs) drives the
SIF protocol directly through DMA, so those calls never passed through any runtime
function we could stub. Before this work, the runtime *fabricated* a plausible
reply for every call (zeros, plus a couple of hard-coded version answers) and the
emulated IOP was fed only passively. That is what made the game stall at each new
service: a fabricated reply satisfies a boot handshake, but it cannot answer a
service protocol the game then goes on to use.

The fix has three parts: **route** each raw-SIF call to the IOP when the IOP (or an
HLE service) owns the SID, **answer** it there for real, and **pump** the emulated
IOP forward enough that the service has actually registered its RPC handler by the
time the call arrives.

---

## 2. Where the traffic is intercepted

Everything lives in `PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp`.

The EE guest sends SIF traffic with `sceSifSendCmd`, which issues DMA descriptors.
The runtime sees each completed transfer through `emulateIopSifRpc(rdram, ctx,
runtime, xfer)`. It reads the packet header's command id at `+0x08`
(`kSifCmdHeaderCidOffset`) and handles three shapes:

| cid | Meaning | Action |
|-----|---------|--------|
| `kSifCmdRebootIop` | `SifIopResetPkt` - boot the IOP with a module list | latch IOP-booted + set SMFLG bit `0x40000` |
| `kSifCmdRpcBind` (`0x80000009`) | client bind | remember `client -> sid`, fabricate the REND reply |
| `kSifCmdRpcCall` (`0x8000000A`) | client call | **route it** (section 3), else fabricate |

Note the runtime's `sceSif*` stubs are a separate path used only by our own code;
the guest never reaches them. `emulateIopSifRpc` is the single choke point through
which every guest RPC must pass, and it is deliberately header-free (file-scope
statics in the `.cpp`) so the fix did not touch a header that `runner/*.cpp`
includes.

---

## 3. Stage A - routing a call to the IOP

### 3.1 Remember which SID each client bound

A `SifRpcCallPkt` carries a `SifRpcClientData *client` but **not** the sid: the sid
only appears in the earlier bind. The responder therefore records the association
at bind time:

```cpp
// SIF.cpp, in the kSifCmdRpcBind branch
g_sifRpcClientSid[clientPtr] = sid;
```

`g_sifRpcClientSid` is a file-scope `std::unordered_map<uint32_t, uint32_t>`.

### 3.2 Find the send payload

`sceSifSendCmd` emits **one descriptor for the command packet and one for the
payload**, in either order, and the payload descriptor is not referenced from the
packet. The responder remembers the last payload seen before and after the packet
(their EE address and size) and, on a call, picks the sibling whose **recorded size
matches the call's declared `send_size`**. If there is no match (and `send_size !=
0`), the call is *not* routed - it falls back to fabricating, and the log says
`payload descriptor not found -> fabricating`.

Once the send address and size are known, a `ps2x::iop::RpcRequest` is filled in
from the packet (client, server, sid, `rpc_number` as `function`, `rmode`, send
and receive buffers) and handed to the transport.

### 3.3 The gate: `canBindRpc`

The call is only routed when the IOP side claims the SID:

```cpp
if (boundSid != 0u && PS2IopTransport::canBindRpc(runtime, boundSid)) { ... route ... }
```

`PS2IopTransport::canBindRpc` (`ps2xRuntime/src/lib/ps2_iop_transport.h`) is a thin
friend shim onto `PS2Runtime::canBindIopRpc` -> `IopSubsystem::canBindRpc`, which
returns true if either

* the SID is in the HLE `routes` map (an HLE service owns it), or
* the emulated IOP has a real registered RPC server for it (`IopRpcBridge::hasRpcServer`).

This gate is what keeps the change **inert for services we do not emulate**: an
unclaimed SID keeps the previous fabrication behaviour exactly. It also means a
service becomes routable the moment its module finishes starting - the entire point
of the catch-up in section 4.

### 3.4 Dispatch order

`PS2Runtime::handleIopRpc` -> `IopSubsystem::handleRpc` tries the **emulator first,
HLE second**:

```cpp
RpcResult emulated = m_impl->emulator.handleRpc(request);
if (emulated.handled || !hle) return emulated;
return hle->handleRpc(request);
```

so a real loaded module always wins over an HLE stand-in for the same SID.

* **Emulated (`IopRpcBridge::handleRpc`)** - the module's own code answers. The
  bridge copies the EE send payload into the IOP-side buffer, calls the module's
  registered function (`executor.executeGuestFunction(...)`, i.e. runs real MIPS in
  the emulated IOP, passing the module's `.gp`), then copies the returned buffer
  back into the caller's EE receive buffer. See section 5.
* **HLE (`IopService::handleRpc`)** - a host-side implementation. No reply is
  fabricated by the shim: the service writes into `request.receive` itself through
  `IopHost` (`writeGuest` / `writeRpcWords`), bounded by the receive size.

### 3.5 What the caller sees

Whichever path answered, the responder still has to complete the call: it posts an
`RPC END` command (cid `0x80000008`) whose `+0x20` names the request cid, so the
guest's own `_SifRpcEndHandler` runs `client->end_function` and signals the
client's semaphore - which is what `sceSifCallRpc` is waiting on. When nothing
handled the call, the legacy branch zeroes the receive buffer and adds the two
hard-coded version answers (`kSifRpcGetVersion`, `kDbcManRpcCheckVersion`).

---

## 4. The catch-up (the part that made it actually work)

### 4.1 Why it is needed

The EE and the IOP are **separate processors**, and a loaded module registers its
RPC servers from the start-up thread its entry creates. That thread only runs while
the emulated IOP is pumped, and SIF traffic is usually what releases it. The host
loop, however, lets the EE run far ahead: it will load a module and immediately
call into it before the module has reached its `sceSifRegisterRpc`. Symptom: the
call is unclaimed, so it is fabricated, and the service then misbehaves in a way
that looks like a protocol bug rather than a timing one.

Measured on the sound gate: the guest's DTX call lands at log line 11729 and
IXAUDIO's `RegisterRpc sid=0x90000200` only at line 11736 - but the pump reclaims
it after **2 rounds**.

### 4.2 What it does

Before routing a call for a SID, `catchUpIopForSid(runtime, sid)` runs a **bounded**
IOP pump if the SID is not yet claimed:

* it runs at most `kIopCatchUpRounds = 32` rounds of `kIopCatchUpEeCycles =
  1048576` EE cycles each, stopping early the moment `canBindRpc(sid)` becomes true;
* it is **once per SID** (`g_sifIopCatchUpTriedSids`), so it cannot become a
  per-call cost;
* on failure it logs
  `[SIF-iop] IOP catch-up: sid=0x... still unclaimed after 32 round(s); iop
  cycles=... modules=... threads=... rpcServers=...`, which is the diagnostic you
  want when a service *never* registers (the counters come from
  `PS2Runtime::iopDebugSnapshot`).

`PS2IopTransport::catchUp` calls `PS2Runtime::advanceIopEeCycles`, which is private;
the transport class is a friend. `IopEmulator::runEeCycles(eeCycles)` converts the
EE budget to IOP cycles at the real ratio (EE 294 MHz : IOP 36.8 MHz, i.e.
`eeCycles / 8`).

**This is required semantics, not a workaround:** the EE legitimately runs ahead,
so the responder must let the IOP catch up before deciding that a service cannot
answer. The bound exists so a service that will never register costs 32 rounds
once rather than hanging the run.

### 4.3 Reset

`resetSifState()` (called at the top of `PS2Runtime::run()`) clears
`g_sifRpcClientSid` and `g_sifIopCatchUpTriedSids` along with the payload
bookkeeping, so a restarted runtime re-learns the binds and gets a fresh catch-up
budget.

---

## 5. The IOP side (`ps2xIOP`)

Two different mechanisms answer a routed call, and it matters which one you extend.

### 5.1 Real modules - `IopRpcBridge`

`ps2xIOP/src/emulator/services/iop_rpc.cpp` implements the `sceSif*` **imports** of
the emulated IOP kernel. Import 17, `sceSifRegisterRpc`, records the module's
server data:

```
server.serverData = cpu.gpr[4];  // SifRpcServerData *
server.sid        = cpu.gpr[5];
server.function   = cpu.gpr[6];  // the module's handler
server.gp         = cpu.gpr[28]; // needed to re-enter the module
server.buffer     = cpu.gpr[7];  // the IOP-side request buffer
                 + callback / queue from the caller's stack
```

into `m_servers[sid]`. A routed call then re-enters the module's own MIPS through
`executeGuestFunction(server.function, request.function, server.buffer,
request.send.size, 0, server.gp)`, with the payload already copied into
`server.buffer`, and copies the result buffer back out to the EE. This is how
KORGIVAG prints its banner, DBCMAN answers version/data queries, and IXAUDIO
answers the SJX/DTX family - all with their own code.

`hasRpcServer(sid)` feeds `canBindRpc`, so a SID becomes routable exactly when the
module registers.

### 5.2 HLE services - `IopService`

When the module that would own a service is not (or cannot be) loaded, a host-side
service stands in. The pattern is `ps2xIOP/src/modules/<name>.cpp` implementing
`ps2x::iop::detail::IopService`:

```cpp
std::string_view name() const override;                       // "loadfile"
std::span<const uint32_t> sids() const override;              // {0x80000006}
std::span<const std::string_view> moduleAliases() const override; // dormant until loaded
bool alwaysAvailable() const override;                        // or this
RpcResult handleRpc(const RpcRequest &request) override;      // write the reply
void reset() override;
```

and a one-line factory declaration in `ps2xIOP/src/module_factories.h`, a source
entry in `ps2xIOP/CMakeLists.txt`, and an `emplace_back` in `IopSubsystem::Impl`'s
constructor (`ps2xIOP/src/iop_subsystem.cpp`).

Activation is decided in `Impl::serviceActive`:

```cpp
return service.alwaysAvailable() || moduleManager.isLoaded(service.moduleAliases());
```

`rebuildRoutes()` then maps each active service's `sids()` into `routes`, and
`canBindRpc(sid)` consults `routes` before the emulator. Two flags matter:

* **`alwaysAvailable()`** - the service is part of the platform and must not wait
  for a module. `loadfile` (SID `0x80000006`) is the canonical case: it *is* the
  module loader, so it can never wait for a module to be marked loaded.
* **`moduleAliases()`** - the service is dormant until one of those IOP modules is
  actually loaded (DBCMAN, LIBSD, MCSERV behave this way).

`loadfile` is the model to copy for any new HLE service:
`ps2xIOP/src/modules/loadfile.cpp`. Its specifics are worth repeating because they
were expensive to discover: the EE passes one buffer laid out as
`{ int result; int moduleId; char name[...]; }` (so the path starts at `send+8`),
the reply is `{result (0 = ok, 0xFFFFFFFF = fail), moduleId}`, and the guest
**retries forever** on a negative result - which is how the layout was pinned down.

---

## 6. Files and invariants

| File | Role |
|------|------|
| `ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp` | Stage A routing, `emulateIopSifRpc`, `catchUpIopForSid` |
| `ps2xRuntime/src/lib/ps2_iop_transport.h` | friend shim: `canBindRpc`, `handleRpc`, `catchUp` |
| `ps2xRuntime/src/lib/ps2_runtime.cpp` | `canBindIopRpc`, `handleIopRpc`, `advanceIopEeCycles` |
| `ps2xIOP/src/iop_subsystem.cpp` | service list, `serviceActive`, `rebuildRoutes`, dispatch order |
| `ps2xIOP/src/emulator/services/iop_rpc.cpp` | real module servers (`sceSifRegisterRpc`, dispatch) |
| `ps2xIOP/src/modules/*.cpp` | HLE services (`loadfile`, `dbcman`, `libsd`, `mcserv`, ...) |

Invariants:

* `runner/*.cpp` is machine output and is never edited; the routing lives in the
  runtime instead.
* `SIF.cpp` keeps its state in file-scope statics so no header had to change. The
  headers touched by this work (`ps2_iop_transport.h`, `ps2xIOP/src/*.h`) are not
  included by `runner/*.cpp`, so nothing forced a full rebuild.
* Routing must stay **inert** for unclaimed SIDs: keep the fabrication fallback.

---

## 7. How to observe it

Run the game and read stderr. The relevant lines, in the order they appear:

| Log line | Meaning |
|----------|---------|
| `[SIF-iop] RPC BIND sid=0x... client=0x...` | a client bound a SID (recorded for routing) |
| `[IOP] RegisterRpc sid=0x... fn=0x...` | a real module registered its server (temp diagnostic) |
| `[SIF-iop] IOP catch-up: sid=0x... claimed ... after N round(s)` | the pump worked; the service is now routable |
| `[SIF-iop] IOP catch-up: sid=0x... still unclaimed after 32 round(s); ...` | the service never registered - read the counters |
| `[SIF-iop] RPC ROUTE sid=0x... rpc=0x... handled=1` | the call was routed and answered on the IOP side |
| `[SIF-iop] RPC CALL ... -> RPC END posted` | the call was completed back to the guest |

A `handled=0` on a `ROUTE` line means the SID was claimed but the specific function
was not recognised by the emulator or the HLE service - i.e. a protocol gap, not a
routing gap.

---

## 8. Open item - Stage C: file I/O on SID `0x80000001`

With sound up, the guest loads its first asset file: `FUN_001664d0` opens
`cdrom0:\AFS\PTN_INFO.BIN;1` (and `PTN_INFO.NAM;1`), which needs the fileio/ioman
service. The guest binds SID `0x80000001` (client `0x3e1f40`, `recv_size=8`) and
currently gets fabricated zeros, so the open "fails" and the guest lands in its
error trap at `0x1666b4` (`bltz $s4, 0x1666b4`).

IOPRP300.IMG's own `ioman` is not loaded, so the intended fix is an HLE `fileio`
service on SID `0x80000001`, built exactly like `loadfile` and backed by the host
VFS through `IopHost` (`translateGuestPath` / `openHostFile` / `hostFileSize` /
`readHostFile`). The RPC numbers the guest actually uses must be discovered
empirically from the log once init succeeds - the game has not sent them yet at the
time of writing.
