# 32-bit Vulkan support (issue #15 — "32 Bit DXVK Hook")

> ## Implementation status — CODE COMPLETE, spike verified, not yet run in a game
>
> Phases 0–5 below are implemented. What changed against the plan as written, and why:
>
> * **Phase 2 was already half-done.** The OpenGL work (v0.7.0) shipped protocol v2 with a
>   `client_kind` field and a host-creates-the-textures path — the exact mechanism this plan
>   needed. So instead of inventing a `FeedBuild::api` field, Vulkan is simply a third
>   `FeedClientKind`, and the host's Build handler branches on `FeedHostCreatesTextures()`
>   rather than on GL specifically. `FEED_IPC_VERSION` went 2 → 3 for the one genuinely new
>   field, `FeedBuildAck::output_fmt`.
> * **`src/feed_fmt.h`** exists but the 64-bit add-on still carries its own copies: moving them
>   would have been a behaviour-neutral edit to the most-proven file in the repo for no gain
>   this release. The header's functions are `FeedFmt*`-prefixed so the 32-bit add-on can
>   include it while the proven D3D11 branch keeps its own local `TypedColorFormat` untouched,
>   as the plan intended.
>
> **Phase 0 ran on the dev machine (RTX 5090, 2026-08-31) and passed.** The four gating
> questions are answered:
>
> | Probe | Answer |
> | --- | --- |
> | (a) 32-bit ICD extensions | All seven present — `external_memory{,_win32}`, `external_semaphore{,_win32}`, `dedicated_allocation`, `get_memory_requirements2`, `timeline_semaphore` — out of 271 device extensions, API 1.4 |
> | (b) `src/feed_vk.h` as x86 | Compiles and `FeedVkLoad` resolves every entry point |
> | (c) D3D12 texture import at 32-bit | Succeeds both **with and without** storage usage, on the same resource (the OUTPUT and COLOR cases) |
> | (d) memory-type heuristic | Allowed mask `0x3`, lowest-set-bit picks index 0. That type reports *no* `DEVICE_LOCAL` bit — the NVIDIA ICD's external-import type — and the round trip is byte-correct, so the heuristic holds. Worth re-reading if another vendor ever matters. |
>
> Both directions of the frame counter crossed on a shared D3D12 fence, and both patterns
> round-tripped through the same memory.
>
> **What is NOT verified:** anything above the transport. No 32-bit Vulkan game has run this —
> not DXVK's `vkCreateDevice` resolution style against the in-process hook (risk 1), not the
> BGRA8 output path (risk 2), not ReShade x86 as a Vulkan layer. Verification steps 3–6 below
> are still owed.
>
> One gap worth naming inside phase 0 itself: the spike round-trips `R8G8B8A8_UNORM`, but the
> real DXVK case is **BGRA8 with `VK_IMAGE_USAGE_STORAGE_BIT`** on the OUTPUT slot, which needs
> the driver to report storage-image support for `VK_FORMAT_B8G8R8A8_UNORM`. NVIDIA does, and
> the 64-bit DOOM path already leans on it, so this is very likely fine — but it is the one
> format combination the phase-0 evidence does not actually cover. The import-failure log names
> it explicitly if it ever bites.

## Context

GitHub issue #15 (skoriandlp-arch) asks for a 32-bit Vulkan transport: WoW 3.3.5 (32-bit)
through DXVK → ReShade x86 → `dlss5-feed32` → `host64` → D3D12/DLSS 5. Today the 32-bit
add-on hard-stops at `src/dlss5-feed32.cpp:1162-1164` ("only Direct3D 11 games are
supported"). The reporter confirms everything upstream of the gate works: the addon32
loads under 32-bit ReShade on DXVK's Vulkan device, and MV + depth are detected.

**Verdict: feasible.** The host is nearly API-agnostic (it consumes DXGI formats, NT
handles and a pipe — it never learns the game's API); the 64-bit Vulkan transport
(`src/feed_vk.h`, `src/feed_vk_hook.h`) is self-contained and arch-portable apart from a
few handle-punning casts; cross-bitness NT-handle duplication is already proven by the
shipped fence direction (host→game). The one structural change: the shared-texture
creation direction must flip for Vulkan — D3D12 cannot open Vulkan-exported memory, so
the **host creates the textures and duplicates handles into the game** (the direction
PLAN-32BIT.md originally proposed, and the same pattern the fences already use at
`host/dlss5-feed-host64.cpp:717-726`). Driver support confirmed on the dev machine
(SysWOW64 loader + NVIDIA `nv-vk32.json` ICD + external_memory/semaphore/timeline
extensions on the 64-bit query); a 32-bit behavioural probe is phase 0.

Settled decisions: evaluate stays out-of-process in host64 (NGX is x64-only); protocol
v2 with an `api` field and **enforced** version match (today only the magic is checked);
game-side queue ops via ReShade (`rs_queue->signal/wait` on imported timeline semaphores
punned as `api::fence`), never raw `vkQueueSubmit`; HostDrain freeze protection via
`vkWaitSemaphores`; 4 slots (no MASK); work resolution fixed at 100% in v1.

## Implementation plan

### Phase 0 — cross-bitness interop spike (gates everything)
New pair in `spike/` (modeled on the existing spike pair; `spike/build-spike.bat` gains
both):
- `spike/spike-vkhost64.cpp` (~200 ln): D3D12 device; shared fence + one 64×64 RGBA8
  texture via `CreateCommittedResource(D3D12_HEAP_FLAG_SHARED, ALLOW_SIMULTANEOUS_ACCESS,
  STATE_COMMON)` — the exact shape of `MakeSharedTexVk` (`src/dlss5-feed.cpp:1807-1829`);
  pipe; `DuplicateHandle` both INTO the 32-bit client; wait timeline signal; read back
  and verify the pattern.
- `spike/spike-vkclient32.cpp` (~300 ln): prints 32-bit device-extension list (answers
  whether the 32-bit NVIDIA ICD exposes external_memory_win32 / external_semaphore_win32
  / timeline_semaphore); creates a VkDevice with the `kFeedVkWanted` list
  (`src/feed_vk_hook.h:36-44`) + timelineSemaphore feature; **includes `../src/feed_vk.h`**
  (compiling it proves x86-cleanliness); imports the fence + texture (with and without
  storage usage), fills, signals the timeline from a submit. Prints the chosen memory
  type (validates the lowest-set-bit heuristic).

### Phase 1 — shared-code prep (zero behavior change on x64)
- `src/feed_vk.h`: portable handle-pun helpers — `FeedVkHandle<H>(uint64_t)` /
  `FeedVkValue(H)` for non-dispatchable handles (`uint64_t` on x86, pointers on x64),
  `FeedVkDispatch<H>` for dispatchable ones; add `vkWaitSemaphores` +
  `vkGetSemaphoreCounterValue` to `FeedVkLoad` (core name first, KHR fallback).
- `src/dlss5-feed.cpp`: fix the ill-formed-on-x86 call sites through the new helpers
  (`:1764`, `:1794-1795`, `:2471-2474`, `:2499`, `:2619`) so the two add-ons stay
  textually parallel.
- New `src/feed_fmt.h` (~70 ln, mostly moved): `TypedColorFormat`, `OutputFormatFor`
  (the 64-bit BGRA-aware superset — DXVK swapchains are almost always BGRA8, and this
  mapping is what keeps the copy-home a raw copy instead of an sRGB-converting blit,
  issue #11), `SameTexelLayout` etc. Used by the new Vulkan branch and the host; the
  proven feed32 D3D11 branch keeps its local logic untouched this release.
- Rebuild x64, confirm no functional diff on a 64-bit Vulkan game.

### Phase 2 — protocol v2 + host
- `src/feed_ipc.h`: `FEED_IPC_VERSION 2`; `enum FeedApi { FEED_API_D3D11, FEED_API_VULKAN }`;
  `FeedBuild` gains `uint32_t api` (appended, pack(1) preserved); `FeedBuildAck` gains
  `uint64_t tex[FEED_SLOTS]` (game-process handle values, Vulkan mode only) and
  `uint32_t output_fmt` — **required**: the host now owns the OUTPUT texture and applies
  the typed-UAV-store fallback (`ResolveOutputFormat`, needs the host's device), and the
  game must know the real format for the VkImage import and copy-vs-blit choice.
- `host/dlss5-feed-host64.cpp`: enforce `hello.version == FEED_IPC_VERSION` in `Serve()`
  (:704-710) — ack then bail on mismatch, before any Build read (struct sizes differ).
  New `MakeSharedTexForGame(...)` (~40 ln): D3D12 committed-shared texture +
  `CreateSharedHandle` + `DuplicateHandle` into the game (fence pattern at :724-726),
  local handle kept for teardown. Build handler branches on `b.api`: D3D11 verbatim;
  Vulkan skips the duplicate-out loop, resolves output format host-side, creates the 4
  textures (COLOR=b.color_fmt, OUTPUT resolved+uav, DEPTH=R32_FLOAT, MV=R16G16_FLOAT).
  `ALLOW_SIMULTANEOUS_ACCESS` + COMMON promotion/decay means `Evaluate()` needs no
  changes. Frame handler and exit path unchanged — the exit `Signal(UINT64_MAX)` (:903)
  releases `vkWaitSemaphores` waiters too (same kernel object).

### Phase 3 — `src/dlss5-feed32.cpp` Vulkan branch (~+500 ln)
- Include `feed_vk.h` + `feed_vk_hook.h` (after the local `Log`); add MinHook to the
  32-bit build. State gains `is_vulkan`, `rs_dev/rs_queue`, punned `rs_fence_in/out`,
  `FeedVk vk`, `vk_img/vk_mem[FEED_SLOTS]`, `vk_sem_in/out`, `vk_layout_init`.
- `OnRenderTechnique` (:1395-1401) dispatches on `get_api()`: d3d11 → existing
  `FeedFrame`; vulkan → new `FeedFrameVk`; else disable ("only Direct3D 11 and Vulkan
  games are supported by the 32-bit add-on"). This removes the issue-#15 gate.
- `BuildSharedVk` (~140 ln): release-with-`wait_idle` → formats via `feed_fmt.h`
  (work res pinned 100%, log once if the slider is set) → `EnsureHost` → `FeedBuild`
  with `api=FEED_API_VULKAN, tex[]={0}` → on ack: `FeedVkLoad` (on failure, the
  hook/layer triage diagnostic from dlss5-feed.cpp:1771-1782, pointing at the x86
  layer) → import fences as timeline semaphores + pun to `api::fence` → import the 4
  textures (`storage` only for OUTPUT, matching dlss5-feed.cpp:1840) → close the
  duplicated handles after import (driver keeps its own reference).
- `FeedFrameVk` (~200 ln): structural transplant of dlss5-feed.cpp:2376-2661 minus MASK
  and in-process NGX, plus the pipe. Our images to GENERAL once; game bb/mv/depth to
  `copy_source` via `cl->barrier` (ReShade layout tracking); raw `vkCmdCopyImage` ×3 in;
  mode==1 keeps the left-half transport test; full path: flush →
  `rs_queue->signal(rs_fence_in, n)` → pipe `'F'` → `rs_queue->wait(rs_fence_out, n)`
  (GPU-side; host CPU-signals on failure so it always resolves) → re-fetch the command
  list (fresh after flush) → copy home (`SameTexelLayout` ? copy : blit NEAREST) →
  backbuffer back to `render_target`.
- `EnsureHost` (:500-551): enforce `ack.version` match (both halves must update
  together). `HostDrain` (:405-432) gains the Vulkan arm: early-out via
  `vkGetSemaphoreCounterValue`, host-dead check as today, else `vkWaitSemaphores` 2 s.
  `HostClose`: destroy the semaphores after drain; force rebuild so a respawned host's
  new fences AND textures are re-imported. `OnDestroyDevice` handles the Vulkan device.
- Overlay: gate the work-resolution slider on Vulkan (disabled note), show the API in
  Status. `DllMain`: register `create_device` → `FeedVkHookInstall()` when vulkan;
  `FeedVkHookRemove()` on detach (mandatory — stale jmp crashes the next
  `vkCreateDevice` when ReShade reloads add-ons per instance).

### Phase 4 — builds, x86 layer, CI
- `build-addon32.bat`: `/Iexternal\vulkan /Iexternal\minhook\include`, MinHook sources +
  **`hde\hde32.c`** (vendored, currently compiled nowhere; MinHook auto-selects HDE32
  under `_M_IX86`).
- `layer/build-layer.bat`: add an `amd64_x86` block → `layer/x86/VkLayer_feed_vk32.dll`
  + manifest **in its own subdirectory** (the loader tries every manifest on
  `VK_LAYER_PATH`; same-name manifests in one dir would try the wrong-bitness DLL).
  Add `layer/feed_vk_layer.def` and link with `/DEF:` on x86 — `VKAPI_CALL` is
  `__stdcall` there, so bare `/EXPORT:` would emit `_...@N`-decorated names the loader
  can't `GetProcAddress`. `run-with-feed-layer.bat` gains the 32-bit variant.
- CI: addon32 step gets the vulkan include (already fetched for x64); layer builds both
  arches; artifacts add the x86 layer pair; spike builds the new probes.

### Phase 5 — docs + issue
- README: "32-bit Vulkan game (DXVK)" install subsection — ReShade **x86 as a Vulkan
  layer**, NOT a local dxgi.dll (the d3d9.dll slot is DXVK's); host64\ folder unchanged;
  historical v1 work res fixed at 100%. The adjustable-resolution fork subsequently
  added scaled 32-bit Vulkan/DXVK support. Name DXVK as the audience. Append the v2
  protocol to PLAN-32BIT.md.
- Comment on issue #15 offering the reporter a dev build once local testing passes.

## Verification (each step gates the next)

1. Spike pair passes: extensions present at 32-bit, both imports succeed, pattern
   round-trips, memory type printed.
2. `host\dlss5-feed-host64.exe --test` unchanged (regression).
3. DXVK x86 on a 32-bit D3D9 game on hand (e.g. Fable Anniversary with DXVK swapped in
   for dgVoodoo2), ReShade x86 as Vulkan layer: first `mode=1` (left-half split screen
   proves duplication, import, both semaphores, copy-home — zero NGX involved), then
   `mode=2`; check `dlss5-feed.log`, `host64\dlss5-feed-host.log`, `host64\ReShade.log`
   ("feature 18 created").
4. Drain paths: kill host mid-game; overlay Apply-to-host; alt-tab/res change rebuild.
5. Regression: one 32-bit D3D11 game (Blacklist or BioShock) on the same binaries.
6. Dev build to the reporter for WoW 3.3.5.

## Risks (ranked)

1. DXVK resolves `vkCreateDevice` past the export hook — fallback is the x86 layer;
   failure mode is a clean diagnosed disable. Medium likelihood, low impact.
2. BGRA8 typed-UAV-store missing → R8G8B8A8 output → sRGB-converting blit → issue-#11
   washout on DXVK's BGRA swapchain. `ack.output_fmt` + `SameTexelLayout` make the good
   path automatic; bad path is cosmetic.
3. 32-bit ICD missing an interop extension — would be fatal; exactly what spike probe
   (a) settles on day one. Low likelihood.
4. Import validity of D3D12 committed textures at 32-bit (dedicated-only alloc, storage
   usage) — recipe proven at 64-bit on this driver; spike proves it at 32.
5. Memory-type lowest-set-bit heuristic in 32-bit address space — imported default-heap
   memory is device-local and never mapped; spike prints the type. Very low.
6. Teardown with work in flight — `wait_idle` in release + the drain ordering already
   proven on D3D11.

## Size estimate

~1,300-1,500 lines across ~13 files: dlss5-feed32.cpp +~500; spike pair +~500 (new);
host +~140; feed_ipc.h +~25; feed_vk.h +~35; feed_fmt.h ~70 (mostly moved);
dlss5-feed.cpp ~12 lines touched; build scripts/CI/.def ~30; docs ~100.
