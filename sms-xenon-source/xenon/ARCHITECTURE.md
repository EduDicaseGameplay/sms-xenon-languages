# Xenon port architecture (plan, 2026-10-28)

The goal is a stable 30 fps (with MSAA) on a retail console. On 2026-10-27 the Delfino Plaza frame
took ~56 ms, all of it CPU on one hardware thread:

| Part | ms |
|---|---|
| Game logic | 12 |
| J3D/JPA | 6 |
| GX API | 3 |
| GX core (display lists, vertex decode, register → state) | 15 |
| D3D back end | 20 |

The GPU finishes less than 1 ms after the CPU.

Moving the back end to a second core alone would leave that thread at ~35 ms. The render work has
to get cheaper as well as move.

## Principles

1. **The game layer stays shared with upstream.** This covers decomp code, JSystem logic, audio
   logic and disc/files. Port changes there stay small, as port-patches.
2. **The platform layer is our own** (`xenon/`). It is replaced, not adapted, where Xenon differs
   from a PC:
   - EDRAM is the EFB;
   - vertex fetch is big-endian and reads GX formats;
   - one fixed GPU, so shaders and state are built offline;
   - 3 in-order cores with 2 SMT threads each.
3. **Strangler migration.** The native path takes over one draw category at a time. The emulated
   path stays behind an env switch. A category moves only when xregress passes.
4. **Data ownership.** The render side owns every cache and GPU resource. The game side hands it
   immutable snapshots. Anything taken from game RAM is copied or versioned at submit time and
   never read late, because J3D rewrites its arrays every frame.
5. **Fidelity.** Differences invisible to the eye are allowed for speed; xregress uses a tolerance
   on native paths. Anything the CPU reads back (I8 goop maps, depth copies, peeks) stays exact,
   because it drives gameplay. Dolphin is the reference.
6. **Latency.** A render pipeline lagging up to one frame is accepted. The ring is consumed as it
   is written (streaming), so the usual added lag is a few ms.
7. **Every phase ends with a console measurement**, at worst frame and p99, not the average.
8. **Kind to the hardware**, after performance: the priority is stable 30 fps > fidelity > efficiency >
   platform manners. Where efficiency costs frame time (a short spin on a fence against a sleep and
   wake), frame time wins.
   - No spin or busy-wait: block on vsync, events and fences.
   - Idle hardware threads sleep. Work that can be built once is not redone every frame.
   - Nothing runs faster than the 30 Hz present. Idle time per thread is measured like frame time.
   - The port should feel first-party: Guide and controller-disconnect pause, the title-safe area, no first-visit pop-in.

## Target layout

| HW thread (core) | Work |
|---|---|
| 0 (core 0) | Game: all guest threads as fibers, logic, J3D, GX API. Produces native render packets. |
| 2 (core 1) | Render: owns the D3D device and executes packets. Little work, since everything is prebuilt. |
| 4 (core 2) | Jobs: texture decode and tiling, EFB write-back untile, display-list compiles. |
| 1, 3, 5 | Light work only: audio (XAudio2), IO. They share a pipeline with 0/2/4. |

The render path, step by step:

1. **GX API.** `gx_api.cpp` stays as it is. Its register shadow is cheap (3 ms) and keeps the API
   semantics exact. It is rewritten only if a profile says so.
2. **State.** Register writes mark dirty groups. At draw time each dirty group maps to a key, and
   key → prebuilt native block: shader pair, render and sampler states, constant layout. The
   translation runs once per distinct state, not once per draw.
3. **Static geometry.** Display lists are compiled once into static VB/IB in physical memory, with
   their state ops pre-decoded. They are validated by upstream's display-list cache logic (hash,
   write stamps, periodic recheck), which is reused. This is the missing "arena", one mesh ref per
   draw.
4. **Vertex formats.** Vertex declarations read the GX formats as they are: big-endian shorts and
   bytes, with the frac scale in a constant. No CPU vertex decode.
   - Experiment: Xenos `vfetch` with a separate index per attribute (GX indexed attributes),
     without de-indexing.
   - Fallback: de-index at compile time.
5. **Dynamic geometry.** `GXBegin` immediate mode, CPU-deformed vertices and particles are copied
   once into a per-frame transient ring (write-combined, never read by the CPU).
6. **Packets.** The game side emits SetState(key), SetConsts(block), Draw(mesh or transient
   range), Copy, Present and Sync. The render side resolves keys and draws.
7. **TEV.** Float shaders instead of exact 8-bit integer emulation, with fewer variants. The
   compiled cache ships in the package, collected from the xregress suites.
8. **EFB.** Colour copies used only for display stay in EDRAM/GPU memory. The CPU-read copies
   (I8, depth, peeks) stay exact and are the only sync points.

## Phases

| # | Work | Exit criterion |
|---|---|---|
| 0 | **Measure.** Console input record and replay; per-frame p50, p99 and max per thread. Scenes: plaza, Pinna Park, Noki Bay, Sirena, Bianco Hills. Logic ceiling with rendering skipped. Sync points per frame. | Numbers say whether game + J3D fit in ~25 ms, and how often the CPU waits for the GPU. |
| 1 | **Packet ring and render executor, consumed inline** (same thread). The current back end emits packets. | xregress exact; no slowdown. |
| 2 | **Static geometry and state keys:** compiled display lists, native vertex formats, prebuilt state blocks. | xregress within tolerance; GX core + D3D cut sharply on the console. |
| 3 | **Float TEV shaders**, and the shader cache shipped in the package. | Tolerance holds; no compile stalls. |
| 4 | **Executor on HW thread 2** (`AcquireThreadOwnership`), jobs on 4, streaming ring, sync points. `SMS_XE_RENDER_THREAD=0` keeps the inline mode. | Plaza at 30 fps at p99. |
| 5 | **Profile-driven CPU work:** VMX128 PSMTX and skinning, load-hit-store, PGO. | Headroom on the heaviest scenes. |
| 6 | **MSAA**, the 1.5x scale, 16:9, line width. | 30 fps at p99 with them on. |

Phase 0 decides the weight of phase 5: if game logic plus J3D alone exceed ~25 ms, it moves up.

## Progress

- **Phase 0** (2026-10-28): console bench (`xenon/platform/xe_bench.cpp`, `xenon/tools/bench`). At the
  real clock, game logic without audio takes 22-27 ms a frame, GX core 12-15, render ~17. The DSP
  mixer moved to HW thread 4 first (port patch 14): Gelato 67.6 -> 54.6 ms, plaza 67.0 -> 56.0.
- **Phase 1** (2026-10-28): render packets (`xe_gx.h`, `xe_gx_exec.cpp`). Every command-stream call
  of the device is in the executor; the game side emits packets and waits on fence tokens. Kicked
  every 64 KiB, at present and before reads. xregress: every suite as before (frames differ from run
  to run under Xenia by the same few hundred pixels with the old build too). Console: Gelato 54.4 ms,
  plaza 55.9 (no change); executing each packet at once (`SMS_XE_KICK=0`) costs up to 12 ms.

- **Phase 2, first steps** (2026-10-28): timebase timers of a draw's parts in `[perf]` showed
  constants at 14 ms of a plaza frame. XF constants now go up by the rows that changed (port patch
  15 reports each XF word), the other constants only when their registers changed, and display-list
  runs are drawn from an arena (static VB/IB) instead of being copied into every batch. Console:
  plaza 55.9 -> 43.0 ms (p99 92 -> 72), Gelato 54.4 -> 44.5 (p99 81 -> 59).

- **Phase 2, core split** (2026-10-28, e6b52eb): the GX core is timed by timebase parts (port patch
  16); port patch 11's QPC clock and the vertex-decode timer were themselves several ms a frame.
  Other constants are rebuilt by changed group. Console: plaza 45.6 ms, Gelato 42.4.

### Phase 4, revised (2026-10-28)

Measured: HW thread 0 runs the game (~22 ms), the GX core (~8-10) and the game side of the
packets (~7-11); the executor is 2-4 ms. Moving the executor alone leaves ~38 ms. So the render
thread takes **everything below the GX API**, as Dolphin's dual-core mode does: the GX core
(command parsing, display lists, vertex loading), the packet building and the executor.

- **Queue.** The game thread writes a single-producer ring: register writes from the API, pipe
  bytes (GXBegin data, J3D's direct writes), display-list calls, and the few other calls into the
  core (array setup, TLUT loads, texture invalidation, HUD and widescreen settings, memory-written
  stamps). The render thread consumes it as it is written. `SMS_XE_GX_THREAD=0` executes each
  entry at once on the game thread (the exact mode xregress checks first).
- **Two register shadows.** `gx_api.cpp` keeps its own copy of the registers it reads back
  (`g_api`, as the SDK's `__GXData`), which display lists do not change; the core's `g` belongs to
  the render thread.
- **Lag.** At most one frame: the game's display copy of frame N+1 waits until the render thread
  has shown frame N. SMS issues `GXCopyDisp` + `GXFlush` without `GXDrawDone`, so the game already
  lets the GPU run a frame behind and double-buffers what the GPU reads (XFBs, J3D's skinned
  vertices).
- **What the CPU reads back.** All of it goes through draw-sync callbacks or write-backs:
  - Callbacks (Mario's occlusion peek, the sun's depth peeks, the goop counters' pixel
    metrics, the time recorder) come at tokens 0x7E, 0x7C and 0x7D. In plaza these are at draws
    2, ~306 and ~610 of ~640, so waiting for them would serialise the frame. In threaded mode
    the callbacks run on the game thread at the token, as now, but their peeks and metrics answer
    from the render thread's snapshot of the same token one frame earlier (the EFB colour and
    depth resolved there, the metric counters). This is deterministic, one frame late, and not
    visible.
  - Write-backs are already stored one frame late, at the next display copy. The render thread
    hands each one over, and the game thread stores it at that display copy, as now.
  - `GXPeek*`/`GXReadPixMetric` outside callbacks, `GXDrawDone`, `GXPC_ReadXFB`: a full sync.
- **Steps.** (1) the queue, inline only; (2) `g_api` and every API entry through the queue,
  inline, xregress exact; (3) the render thread, with sync everywhere; (4) snapshots and
  write-back hand-over; (5) lag bound and console bench.

- **Phase 4** (2026-10-29): the GX core, the packet building and the executor run on HW thread 2
  (`xe_gx_queue.cpp`, port patches 17 and 18), by default; `SMS_XE_GX_THREAD=0` keeps them inline.
  The game may run a frame ahead; draw-sync callbacks run at once and read peeks and pixel metrics
  from the render thread's snapshots of the frame before; write-backs are handed over and stored by
  the game at its display copy. Two traps: the render thread must never call the game's OS
  (`port_irq_check` from flushBatch ran the fiber scheduler there and hung the game), and Present
  waited for the GPU's previous frame until `D3DCREATE_BUFFER_2_FRAMES` (now the default).
  Console (1x, 4:3): Gelato 42.4 -> 33.3 ms (p95 33.3), plaza 45.6 -> 33.7 (p95 35); a 21-minute
  play session held a median of 30.1 fps. xregress: every suite as before except 55 and 60, where
  gameplay reads pixel metrics a frame later and drifts from the references, deterministically.
  Then the render thread got lighter (texture bind memo; word compares, as the XDK's memcmp took
  1.6 us for a 460-byte shader key): Gelato busy 32 -> 24 ms. All nine bench scenes now average
  33.27 ms (30 fps), plaza 33.70 (p99 48); 720p held 30 fps in a play test.
  Next: plaza's spikes, load hitches, EFB copies at the internal scale (blurry reflections at 720p).

Left for phase 4 by phase 1: the game side still creates resources (textures, shaders, declarations,
queries, rings), fills new textures through LockRect, reads query results with GetData after a kick,
and releases its write-back texture references. Each needs checking for thread safety against a
device owned by HW thread 2, or moving to the render side.

## Traps known in advance

- **D3D ownership.** The device is used from one thread at a time, so ownership moves explicitly
  (`AcquireThreadOwnership`/`ReleaseThreadOwnership`).
- **Write-combined memory** is never read by the CPU: reads are very slow.
- **Sync points.** `GXDrawDone`, draw-sync tokens and peeks must be counted before phase 4. If the
  plaza syncs every frame, the pipeline serialises.
- **Xenia is not the console.** A linear Resolve came back scrambled under Xenia, and Xenia
  resumes threads through a closed handle where the console does not. Every phase ends on the
  console.
- **SMT siblings.** Thread 1 shares core 0's pipeline with thread 0, so heavy work there slows
  the game.
