# DLSS5-Feeder for OpenGL games — plan (rev. 1)

## Context

DLSS5-Feeder covers D3D11 (private D3D12 device + shared textures), D3D12 (same-device), Vulkan
(private D3D12 + raw-Vulkan imports of the shared handles), D3D9 (dgVoodoo2), and 32-bit D3D11
(cross-process host). OpenGL is the last renderer left; today it hits the `default:` branch of
`FeedFrame` and disables itself. Both bitnesses are in scope for v1:

* **64-bit first target: DOOM (2016)**, `E:\SteamLibrary\steamapps\common\DOOM\DOOMx64.exe` — the
  OpenGL build sitting next to the Vulkan exe the Vulkan path was proven on. Same scenes, same
  provider settings; results are directly comparable. The game's own TAA off, as always.
* **32-bit target: a classic GL title** (proposal: DOOM 3, 2004 — 32-bit OpenGL, historically
  ReShade-friendly). Final pick is a phase-0 outcome: it must be a game where 32-bit ReShade's GL
  backend and at least one MV provider actually work.

Facts established up front, which decide the design:

**1. The DLSS 5 add-on is D3D12-only** (unchanged from PLAN-VULKAN: every NGX symbol in
`renodx-dlss5.addon64` is `NVSDK_NGX_D3D12_*`). The evaluate stays on a private D3D12 device; the
OpenGL path is the Vulkan path with the transport rewritten — `InitSessionGl` is `InitSessionVk`
with the import third swapped out.

**2. ReShade's API cannot carry this transport on GL — the whole per-frame GL side goes raw.**
Verified in the vendored headers:

* On Vulkan, an `api::fence` *is* a `VkSemaphore` (`reshade_api_pipeline.hpp`), which is what let
  the Vulkan path import raw and hand the semaphore back to ReShade for
  `command_queue::signal/wait`. On OpenGL, an `api::fence` is **"An opaque value"** — an internal
  struct, not a GL name. There is no way to wrap a raw GL semaphore into a ReShade fence. The
  Vulkan case-B trick is structurally unavailable.
* ReShade's `create_fence`/`create_resource` shared-handle import uses the OPAQUE_WIN32 external
  types and refuses D3D12-created handles — proven on the Vulkan backend (that finding is the whole
  reason `src/feed_vk.h` exists, see its header comment). The GL backend's analogues would be
  `GL_HANDLE_TYPE_OPAQUE_WIN32_EXT`, wrong for our `GL_HANDLE_TYPE_D3D12_RESOURCE_EXT` /
  `GL_HANDLE_TYPE_D3D12_FENCE_EXT` handles, for the same reason. (The backend source is not
  vendored, so phase 0 confirms this with one throwaway `create_fence`-import attempt, logged — but
  the plan does not depend on the answer, because of the fence-handle fact above.)

**3. Raw GL in the event callback is safe where raw `vkQueueSubmit` was not.** Vulkan queues are
externally-synchronized objects shared with the game and ReShade — a bare submit races both, which
forced every queue operation through ReShade. OpenGL has no queue object: every command enters the
**current context's single in-order stream on the calling thread**, and `reshade_render_technique`
fires while ReShade itself is issuing GL commands on that same thread/context (its GL "command
list" is immediate, like D3D11's). Our raw calls interleave in program order with ReShade's and
the game's — there is no lock to bypass. Working ReShade GL add-ons that issue raw GL for
GL↔D3D12 interop exist (the dlss5-opengl-bridge project is the existence proof); phase 0
re-verifies on our exact event.

**4. No device hook, no layer — the Vulkan path's biggest risk does not exist on GL.** Vulkan bakes
extensions and features in at `vkCreateDevice`, which forced `src/feed_vk_hook.h` (MinHook on
`vkCreateDevice`) and `layer/feed_vk_layer.cpp` as fallback. OpenGL has no creation-time opt-in:
`wglCreateContextAttribsARB` attributes select version/profile/flags only; extensions are
properties of the driver's context, discovered and resolved at runtime via `wglGetProcAddress` on
whatever context is current. If `GL_EXT_memory_object_win32` + `GL_EXT_semaphore_win32` are in the
extension string, we can use them, full stop. They are NVIDIA-supported on every DLSS-capable
driver; their absence means the frame is not being rendered on an NVIDIA GPU (wrong GPU on a
hybrid laptop, or non-NVIDIA hardware) — DLSS could not run there anyway, so it is a clean
`FeedDisable`, not a fallback component. `OnCreateDevice` needs **no** GL branch.

```
 OpenGL game process (64-bit)
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │ ReShade (opengl32.dll) → MV provider → DLSS5_Feed.fx → dlss5-feed.addon64:   │
 │                                                                              │
 │  raw GL: capture copies into imported aliases of the D3D12 shared textures   │
 │  glSemaphoreParameterui64vEXT(n) + glSignalSemaphoreEXT + glFlush ──┐        │
 │                                                     same process,   ▼        │
 │  private D3D12: queue->Wait(n) ── NGX_D3D12_EVALUATE ── renodx-dlss5 hooks   │
 │                 queue->Signal(n) ───────────────────────────────────┐        │
 │  raw GL: glWaitSemaphoreEXT(n) (server-side) ◄──────────────────────┘        │
 │  FBO-blit shared Output over the technique's render target                   │
 └──────────────────────────────────────────────────────────────────────────────┘
```

## 1. Transport (64-bit path)

**Direction: D3D12 creates, GL imports.** GL memory objects are import-only (there is no
memory-object *export* in `GL_EXT_external_objects_win32`), so the direction is forced — the same
one-way lesson as Vulkan and D3D11. The D3D12 half is byte-for-byte the one `MakeSharedTexVk`
already records: committed resource, `D3D12_HEAP_FLAG_SHARED`, `ALLOW_SIMULTANEOUS_ACCESS` (+UAV
for Output), `CreateSharedHandle`; two `D3D12_FENCE_FLAG_SHARED` fences + handles as in
`InitSessionVk`.

The GL half lives in a new **`src/feed_gl.h`**, mirroring `feed_vk.h`'s role and style:

* `struct FeedGl` — function-pointer table + `ok` flag. Loaded by `FeedGlLoad(FeedGl*)`:
  `GetModuleHandleW(L"opengl32.dll")` (never `LoadLibrary` — the module is already loaded, and when
  ReShade is installed it *is* the local `opengl32.dll`, whose exports forward correctly) →
  `GetProcAddress` for the GL 1.1 entries + `wglGetProcAddress`/`wglGetCurrentContext` →
  `wglGetProcAddress` for everything newer, resolved on the context current at call time. Presence
  gate is the **extension string**, not ProcAddr (the same lesson PLAN-VULKAN's probe encodes):
  a `glGetStringi(GL_EXTENSIONS, i)` loop (with the legacy `glGetString` fallback for
  compatibility contexts) must list `GL_EXT_memory_object`, `GL_EXT_memory_object_win32`,
  `GL_EXT_semaphore`, `GL_EXT_semaphore_win32`.
* `FeedGlImportImage(handle, size, w, h, internalfmt, …)` — `glCreateMemoryObjectsEXT` → set
  `GL_DEDICATED_MEMORY_OBJECT_EXT = GL_TRUE` (mandatory for committed D3D12 resources) →
  `glImportMemoryWin32HandleEXT(GL_HANDLE_TYPE_D3D12_RESOURCE_EXT)` → `glTexStorageMem2DEXT` into a
  fresh texture. The **size** parameter is the D3D12 allocation size
  (`ID3D12Device::GetResourceAllocationInfo`), not w×h×bpp — the helper takes it explicitly (this
  also keeps it usable from the 32-bit stub, which has no D3D12 device; see §5). The NT handle is
  duplicated by the driver, not consumed — kept for teardown like `tex_shared_vk[]` today.
* `FeedGlImportFence(handle)` — `glGenSemaphoresEXT` +
  `glImportSemaphoreWin32HandleEXT(GL_HANDLE_TYPE_D3D12_FENCE_EXT)`. Per use, the timeline value is
  set with `glSemaphoreParameterui64vEXT(sem, GL_D3D12_FENCE_VALUE_EXT, &n)` before
  `glSignalSemaphoreEXT` / `glWaitSemaphoreEXT`, with layouts `GL_LAYOUT_GENERAL_EXT` — the
  pairing for `ALLOW_SIMULTANEOUS_ACCESS` resources.
* Copy helpers: `FeedGlCopy` (`glCopyImageSubData` — zero state touched; used where the source is a
  `GL_TEXTURE_2D` of the exact same format: MV, Depth, Mask) and `FeedGlBlit`
  (`glBlitFramebuffer` through two persistent FBOs — used for the Color capture and the Output
  copy home, because a blit converts formats/channel order and can read what `glCopyImageSubData`
  cannot: renderbuffers and the default framebuffer). The blit attaches by decoded handle type —
  ReShade's GL handle encoding is documented in `reshade_api_resource.hpp` (type in the upper
  24 bits, name in the lower 32): `GL_TEXTURE_2D` → `glFramebufferTexture2D`, `GL_RENDERBUFFER` →
  `glFramebufferRenderbuffer`, default framebuffer → bind FBO 0 + `glReadBuffer(GL_BACK)` /
  `glDrawBuffer(GL_BACK)`.
* `FeedGlStateGuard` — saves/restores exactly what the helpers touch:
  `GL_READ_FRAMEBUFFER_BINDING`, `GL_DRAW_FRAMEBUFFER_BINDING`, read/draw buffer selection,
  scissor enable (blits honour scissor), `GL_FRAMEBUFFER_SRGB` enable. Nothing else is touched —
  no programs, no texture bindings (the helpers use DSA-style/bindless-parameter entry points).
  ReShade rebinds its own state for following passes anyway (same note as
  `BlitOutputToBackbuffer`), but the guard keeps us honest for the game.
* `FeedGlFormat(DXGI_FORMAT)` — the mapping table, sibling of `FeedVkFormat`:

  | DXGI | GL internal format |
  | --- | --- |
  | `R8G8B8A8_UNORM` | `GL_RGBA8` |
  | `B8G8R8A8_UNORM` | **none** — see below |
  | `R10G10B10A2_UNORM` | `GL_RGB10_A2` |
  | `R16G16B16A16_FLOAT` | `GL_RGBA16F` |
  | `R11G11B10_FLOAT` | `GL_R11F_G11F_B10F` |
  | `R32_FLOAT` | `GL_R32F` |
  | `R16G16_FLOAT` | `GL_RG16F` |
  | `R8_UNORM` | `GL_R8` |

  Extension tokens and function typedefs are declared locally in `feed_gl.h` (values copied
  verbatim from the Khronos registry `glext.h`, not from memory) on top of the Windows SDK's
  `<gl/GL.h>` — the header stays self-contained like `feed_ipc.h`; no new vendored dependency, no
  CI fetch.

**BGRA8.** GL has no sized BGRA8 internal format, and we choose the shared textures' formats — so
none is ever created: `BuildResourcesGl` runs the reported backbuffer format through
`TypedColorFormat` and then a GL-path remap `GlSafeColorFormat` that folds `B8G8R8A8`/`B8G8R8X8`
to `R8G8B8A8_UNORM`. The Color capture is a **blit**, which is component-wise (semantic RGBA, not
byte order), so a BGRA-flavoured game surface lands correctly in an RGBA8 shared texture; the copy
home is a blit for the same reason. `OutputFormatFor` already returns only GL-representable
formats (RGBA8/RGBA16F/RGB10A2). What ReShade's GL backend actually reports as the technique
target's format (`r8g8b8a8_unorm`, its `_srgb` sibling, something else) is a phase-0 log line; the
table plus remap covers every plausible answer.

**GL objects are not handed back to ReShade at all** — neither fences (impossible, fact 2) nor
resources (legal via the handle encoding, but pointless: with sync raw, copies through ReShade
would split one mechanism across two owners for zero benefit). One consequence worth noting: no
ReShade `barrier()` calls for the *game's* resources either — unlike Vulkan, GL needs none: the
in-order stream orders our reads after the provider's writes, and the semaphore signal/wait
carries the cross-API release/acquire. The GL path issues **zero** ReShade API calls per frame
beyond the existing `get_texture_binding`/`get_resource_from_view` lookups.

## 2. Blocking unknowns — and honest non-unknowns (phase 0)

Ranked by how much is actually at stake:

* **(a) Is the GL context current on the callback thread, and are raw GL calls legal there?**
  Near-certainly yes (fact 3; ReShade renders effects during `wglSwapBuffers` on the app's
  thread) — but it must be verified **on our event** (`reshade_render_technique`, fired between
  `reshade_begin_effects`/`finish_effects`). Probe: log `wglGetCurrentContext()`, the thread id,
  and a harmless `glGetError()` round trip from the callback.
* **(b) What does the technique `rtv` resolve to on GL?** `get_resource_from_view(rtv)` → decode
  type/name per the handle encoding. A ReShade-owned `GL_TEXTURE_2D` (most likely — its GL runtime
  renders effects into internal targets), a `GL_RENDERBUFFER`, or the default framebuffer are all
  handled by the blit design, but the answer decides which branch is the tested one, and the
  default-FB special representation is undocumented. This is the one genuine design-input unknown.
* **(c) Do the MV-provider .fx shaders compile under ReShade's GLSL codegen?** Genuinely unknown
  per provider. Not fatal (five providers are on the list), and the existing
  `ProviderCompileError` machinery reads the verdict from ReShade.log and puts it on the overlay
  unchanged. Probe: install the providers in the target game, read the overlay.
* **(d) Does the DLSS 5 add-on arm its NGX hooks in a process where ReShade loads as
  opengl32.dll?** The mechanism (ReShade hooks the private D3D12 device when `d3d12.dll` loads) is
  proven under dxgi.dll loading, and was the same open question the Vulkan probe retired for layer
  loading. Probe: the add-on's "hooks installed" line in the game's ReShade.log after
  `InitSessionGl` runs. Add-on *discovery* is a non-question here, unlike Vulkan:
  ReShade-as-opengl32.dll sits next to the exe, and add-ons load from its own directory.
* **(e) Multi-context games.** GL names live in the share group of the context current at import.
  A game that renders through several unshared contexts (or recreates its context) would strand
  our imports. Mitigation designed in regardless of the probe: `InitSessionGl` records
  `g.gl_ctx = wglGetCurrentContext()`; every `FeedFrameGl` compares and, on mismatch, tears the
  session down and rebuilds on the new context — the pattern the other paths already use for
  device recreation. Probe: log context churn over a few minutes of gameplay.
* **(f) 32-bit specifics** — see §5: driver parity of the EXT extensions in x86 processes, and
  cross-process D3D12→GL import (single-process interop is the proven case). Both answered by the
  32-bit spike.

**Phase-0 probe, two parts:**

1. **Standalone spike** (the `spike/` convention): `spike/spike-gl64.cpp` — hidden window + GL
   context + private D3D12 device in one 64-bit process. Creates one small shared texture + one
   shared fence on D3D12, imports both into GL, then round-trips: D3D12 writes a pattern and
   signals n; GL waits n, `glGetTexImage`-reads and verifies; GL clears the texture to a second
   pattern, signals n+1; D3D12 waits, reads back, verifies. PASS/FAIL per step on stdout. Builds
   in CI (no GPU needed to compile), runs on the dev machine without any game. The 32-bit
   companion `spike/spike-gl32.cpp` does the import half against handles served by
   `spike-gl64.exe --serve` over the existing spike pipe convention.
2. **In-game behavioural logging** in DOOM x64 GL (and the 32-bit target): probes (a)–(e) are all
   log lines added to the GL session-open path, which — like `InitSessionVk` before it — doubles
   as the probe: every import failure is logged with the exact extension and call that refused.

No fallback component exists or is needed if a probe is red: red on (a)/(b) means a design detail
changes (e.g. capture at a different event), red on extensions means the machine cannot run DLSS
at all, red on (d) means the same NR-arming investigation any path would need — not a new
shippable.

## 3. Sync model

Per frame, inside `reshade_render_technique` for the `DLSS5_Feed` technique — `FeedFrameGl`, a
sibling of `FeedFrameVk` whose entire D3D12 middle is reused verbatim:

1. **State guard in** (`FeedGlStateGuard` constructor; `GL_FRAMEBUFFER_SRGB` forced off for
   raw-bit moves, restored on exit).
2. **Capture:** `glCopyImageSubData` MV, Depth, (Mask if `g.mask_ok`) from the provider textures
   into the imported aliases — exact formats guaranteed by the existing validation gate
   (unchanged); Color via FBO blit from the `rtv` resource (handles texture / renderbuffer /
   default-FB and any format difference). No barriers of any kind: in-order stream (fact 3).
3. **Cross to D3D12:** `glSemaphoreParameterui64vEXT(sem_in, GL_D3D12_FENCE_VALUE_EXT, n)`;
   `glSignalSemaphoreEXT(sem_in, …, GL_LAYOUT_GENERAL_EXT)` listing the four input textures;
   `glFlush()` — without the flush the signal can sit in the client command buffer while D3D12's
   GPU wait starves.
4. **D3D12 side — unchanged machinery:** `g.queue->Wait(g.fence12_in, n)`; `BeginCommands`;
   COMMON→SRV/UAV barriers; `MvProbeRecord`; SEH-guarded evaluate; barriers back; `EndCommands`;
   `g.queue->Signal(g.fence12_out, n)` on success — **or CPU-side `g.fence12_out->Signal(n)` on
   any failure so GL never hangs on us** (the invariant `FeedFrameVk` already enforces, doubly
   important here because `glWaitSemaphoreEXT` has no timeout).
5. **Cross back:** set `sem_out` value n; `glWaitSemaphoreEXT(sem_out, …, GL_LAYOUT_GENERAL_EXT)`
   listing the Output texture — a server-side wait: the GL command stream stalls on the GPU, the
   CPU does not.
6. **Copy home:** FBO blit Output → the rtv resource (blit, never copy: converts RGBA8 → whatever
   the game target is). `mode=1` short-circuits steps 3–6 into the split-screen transport test:
   blit the LEFT half of the captured Color straight back — the same visual proof `FeedFrameVk`
   uses.
7. **State guard out.** `TimingTick` as everywhere.

Same-frame model, no CPU stalls in steady state (the only CPU waits are the existing bounded ones
in `BeginCommands`). The +1-frame escape hatch (wait on n−1) remains available later exactly as on
the other paths, and is not part of v1 — jitter-free DLAA has not needed it anywhere yet.

## 4. What changes in the code (64-bit)

| Piece | Change |
| --- | --- |
| NGX/private-D3D12 half: `CreateDlssFeature`, `RecreateFeatureOnly`, `SafeCreateDLSS`/`SafeEvaluateDLSS`, `BeginCommands`/`EndCommands`/`DrainGpu`/`AbortCommands`, MV probe, grace/warm-up/`g_ngx_dying` logic | **none — reused verbatim** |
| `ResolveHandles`, `ProviderCompileError`, `ReadMvProviderMode`, cfg system, format helpers (`TypedColorFormat`/`OutputFormatFor`/`IsHdrFormat`) | **none** (plus one new ~5-line `GlSafeColorFormat` remap) |
| `src/feed_gl.h` | **new** — `FeedGl` + `FeedGlLoad`, `FeedGlImportImage`, `FeedGlImportFence`, `FeedGlCopy`, `FeedGlBlit`, `FeedGlStateGuard`, `FeedGlFormat`; self-contained tokens/typedefs; compiles for x64 **and** x86 |
| `Feed` struct | additions: `FeedGl gl; HGLRC gl_ctx; uint32_t gl_tex[SLOT_COUNT], gl_memobj[SLOT_COUNT], gl_sem_in, gl_sem_out, gl_fbo_read, gl_fbo_draw; UINT64 gl_frame;` — reuses the existing `fence12_in/out`, `fence_in_handle/out_handle` and the `tex_shared_vk[]` NT-handle array (optionally renamed `tex_shared_ext[]`, mechanical) |
| `InitSessionGl` | new — clone of `InitSessionVk` with the import third replaced: `FeedGlLoad` (extension-string gate, rich probe logging), `FeedGlImportFence` ×2, `g.gl_ctx` recorded; no ReShade fence wrapping, no `rs_queue` use |
| `MakeSharedTexGl` | new — D3D12 half duplicated from `MakeSharedTexVk` (repo style is sibling duplication; the Vulkan path stays untouched), GL half = `FeedGlImportImage` with the `GetResourceAllocationInfo` size |
| `BuildResourcesGl` | new — clone of `BuildResourcesVk` calling `MakeSharedTexGl`, plus `GlSafeColorFormat` and the two persistent FBOs |
| `FeedFrameGl` | new — §3; the D3D12 middle block lifted from `FeedFrameVk` unchanged |
| `FeedFrame` switch | `case device_api::opengl: FeedFrameGl(...); break;` and the `default:` message updated |
| `ReleaseFrameResources` / `ShutdownSession` | GL branches: `glDeleteTextures`/`glDeleteMemoryObjectsEXT`/`glDeleteSemaphoresEXT`/`glDeleteFramebuffers` **only when `wglGetCurrentContext() == g.gl_ctx`** (else log-and-leak — the driver reclaims with the context); `glFinish` before same-size rebuilds, alongside the existing `DrainGpu` |
| `OnDestroyDevice` | fourth branch: `dev->get_api() == device_api::opengl && dev == g.rs_dev` → `g_ngx_dying = true; ShutdownSession();` |
| `OnCreateDevice` | **no change** (fact 4) |
| Overlay (`DrawOverlay`) | no structural change — the work-resolution slider correctly stays D3D11-only; v1 GL is DLAA at 100% |
| `dlss5-feed.cfg` | no new keys for v1; a `gl_srgb` tri-state is reserved as the §8 escape hatch, added only if phase 3 shows an encoding mismatch |

No opengl32.lib in `build.bat` — everything is runtime-resolved (and link-time binding would be
ambiguous anyway when ReShade *is* the local opengl32.dll). No new external dependency, no CI
header fetch, no hook, no layer.

## 5. The 32-bit OpenGL path

**What exists today** (the D3D11 route this extends): `src/dlss5-feed32.cpp` — a 32-bit ReShade
add-on with no NGX in it — creates the four shared textures on the **game's** D3D11 device
(`MakeShared`), sends its NT-handle *values* in `FeedBuild.tex[]` over the pipe (`src/feed_ipc.h`),
and `host/dlss5-feed-host64.cpp` duplicates them out of the game process and opens them on its own
D3D12 device. The host creates the two shared fences and duplicates them **into** the game; the
game opens them with `OpenSharedFence`. Per frame: game copies inputs, `Signal(fence_in, n)`,
sends `'F'`; host waits, evaluates, `Signal(fence_out, n)` (CPU-signal on failure); game GPU-waits
and blits. The stub currently refuses non-D3D11. Protocol v1 has no Mask slot (`FEED_SLOTS` = 4).

**Two candidate designs for GL:**

**(A) Direct memory-object import — recommended.** The host creates the D3D12 shared textures
(instead of opening game-created ones) and duplicates their handles into the game alongside the
fence handles it already duplicates in that direction today; the 32-bit stub imports textures and
fences with the **same `feed_gl.h`, compiled x86**. The creation direction *must* move host-side:
GL memory objects are import-only, so a 32-bit GL process cannot export — this is what forces the
protocol change; it is not optional in design A. Perturbation assessment:

* `feed_ipc.h`: version bump to 2. `FeedHello` gains `uint32_t client_kind` (0 = D3D11,
  game-creates — v1 semantics; 1 = GL, host-creates). `FeedBuildAck` gains
  `uint64_t tex[FEED_SLOTS]` + `uint64_t tex_size[FEED_SLOTS]` (host-duplicated handle values and
  the `GetResourceAllocationInfo` sizes the GL import needs — the stub has no D3D12 device to
  query them itself). `FeedBuild.tex[]` is zero for GL clients. Optionally the bump also adds the
  Mask slot for both kinds; v1 keeps parity (no mask cross-process) to bound scope.
* Host (`Serve`): one new branch in the `'B'` handler — when `client_kind == 1`, create the
  textures with a new `MakeSharedTexHost` (the `MakeSharedTexVk` D3D12 half: SHARED heap +
  `ALLOW_SIMULTANEOUS_ACCESS`; an *improvement* over today's D3D11→D3D12 direction, whose
  UAV-flag uncertainty `MakeSharedPair` logs) and `DuplicateHandle` them into the game. The `'F'`
  handler, evaluate, warm-up, reinit and window logic are untouched.
* Stub (`dlss5-feed32.cpp`): accept `device_api::opengl` in its `FeedFrame`; new `BuildSharedGl`
  (receive + import instead of create + send) and `FeedFrameGl` (the §3 sequence, minus the D3D12
  middle — that is the host's `'F'` handler, unchanged); the D3D11 blit shaders and `ctx4` are
  unused on GL (the copy home is an FBO blit).

**(B) `WGL_NV_DX_interop2` in-process, then the existing pipe unchanged.** The stub creates its
own 32-bit D3D11 device, makes the four shared textures exactly as today, registers them with
`wglDXRegisterObjectNV`, and per frame lock → FBO-blit into the registered aliases → unlock, then
runs the existing D3D11 fence + pipe flow verbatim. Pros: `feed_ipc.h` and the host untouched;
most of `dlss5-feed32.cpp` survives. Cons: a second, older interop mechanism to maintain
(lock/unlock model, per-frame overhead, well-known driver quirks, no D3D12 flavour); an extra
device in the game process; and the 32-bit GL path would be structurally alien to the 64-bit GL
path instead of sharing `feed_gl.h`.

**Recommendation: A.** One interop header for both bitnesses, and the handle-duplication direction
it needs already exists in the host for fences — the protocol change follows the code's own grain,
versioned so old/new stub–host pairs refuse each other cleanly. **B stays in this doc as the
written fallback** if the 32-bit spike red-flags design A (specifically: EXT extensions missing
from NVIDIA's x86 GL client, or cross-process import refused — neither expected, neither proven).

Open questions specific to 32-bit, all answered by `spike-gl32.exe` + the in-game probe on the
32-bit target: EXT_external_objects parity in x86 processes; cross-process D3D12→GL import; 32-bit
ReShade firing the same GL events (same codebase — near certain); and whether the chosen classic
title's context is modern enough for ReShade's GL backend (NVIDIA hands even GL-1.x-era games a
4.6 compatibility context, so the *extensions* will be there; ReShade's own minimum is the
constraint that picks the target game).

## 6. Build & CI

| Target | Change |
| --- | --- |
| `build.bat` | none (`feed_gl.h` is header-only, runtime-resolved; `<gl/GL.h>` is Windows SDK) |
| `build-addon32.bat` | none beyond the sources it already compiles picking up `feed_gl.h` |
| `host/build-host.bat` | none |
| `spike/build-spike.bat` | + `spike-gl64.exe` (x64: `opengl32.lib gdi32.lib user32.lib d3d12.lib dxgi.lib` — spikes may link opengl32.lib, no ReShade in their processes) and + `spike-gl32.exe` (x86: `opengl32.lib gdi32.lib user32.lib`) |
| `.github/workflows/build.yml` | the existing spike step compiles the new spikes for free; step name updated to mention GL |
| `README.md` | Building table: spike row note; requirements row gains OpenGL |

## 7. Install differences (README later)

* ReShade for OpenGL is a **local `opengl32.dll` next to the game exe** (the installer's OpenGL
  choice) — not a dxgi.dll, not a layer, no registry. 64-bit game → 64-bit ReShade +
  `dlss5-feed.addon64` + `renodx-dlss5.addon64` + `nvngx_*.dll` next to the exe, exactly like the
  D3D11 instructions; add-on discovery is automatic (ReShade's own directory).
* 32-bit game → 32-bit ReShade (`opengl32.dll` x86) + `dlss5-feed.addon32` next to the exe + the
  **`host64\`** folder verbatim from the existing 32-bit instructions — the helper, its bundled
  64-bit ReShade and the NR add-on live there, unchanged.
* Document that hybrid-laptop users must force the game onto the NVIDIA GPU (Windows graphics
  settings / NVIDIA control panel) — on the iGPU the interop extensions are absent and the feed
  disables itself with a message saying exactly that.

## 8. Risks

* **sRGB on blits** — the one GL-specific correctness trap. `glBlitFramebuffer` decodes an
  sRGB-encoded *source* attachment and encodes into an sRGB *draw* attachment only while
  `GL_FRAMEBUFFER_SRGB` is enabled; the D3D paths deliberately move raw bits (sRGB views folded to
  UNORM in `TypedColorFormat`). Design: the guard forces `GL_FRAMEBUFFER_SRGB` off,
  `BuildResourcesGl` logs `FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING` of the rtv; if phase 3 shows
  washed-out/dark output on a genuinely sRGB-encoded target, the reserved `gl_srgb` tri-state
  flips the enable for the two colour blits. Bounded, observable, knob-sized.
* **Layouts vs simultaneous access** — everything ours stays `GL_LAYOUT_GENERAL_EXT`, pairing with
  `ALLOW_SIMULTANEOUS_ACCESS`; no per-resource layout choreography like Vulkan's.
* **Depth conventions — analysed, no action needed.** GL's −1..1 NDC never reaches us: depth
  buffer values are window-space [0,1] after the viewport transform in both APIs (`glDepthRange`
  default; `glClipControl` reversed-Z games also land in [0,1]), and `DLSS5_Depth` arrives as the
  provider's R32F texture with ReShade's `RESHADE_DEPTH_INPUT_*` fixes already applied — identical
  semantics to every other path. The real depth risk is upstream: ReShade's GL depth-buffer
  *detection* is historically its weakest, so `depth_inverted` and provider choice remain the
  knobs, as on Vulkan.
* **MSAA default framebuffer** — if the rtv is multisampled the existing `samples != 1` gate skips
  cleanly; expectation is that ReShade has resolved before effects run (phase 0b confirms).
* **Compatibility/legacy contexts** — the extension string must be read both ways (`glGetStringi`
  + legacy fallback); NVIDIA's 4.6 compatibility contexts make even ancient games viable, but
  ReShade's own GL minimum picks the 32-bit target title.
* **Provider GLSL codegen** (unknown c) — mitigated by five providers + the existing
  compile-status surfacing; VORT first.
* **Context churn / share groups** (unknown e) — detected per frame, session rebuilt; a game
  rendering from unshared contexts per frame would defeat this (accepted; ReShade itself would
  misbehave there first).
* **Teardown without a current context** — GL deletes are skipped unless `g.gl_ctx` is current
  (leak-and-log; the OS/driver reclaims with the context). The D3D12 half tears down fully
  regardless.
* **`glWaitSemaphoreEXT` has no timeout** — a wedged evaluate would hang the GL stream; the
  CPU-signal-on-failure invariant (§3 step 4) plus `BeginCommands`' bounded waits make the D3D12
  side unable to leave `fence_out` unsignalled.
* **NR add-on arming under opengl32.dll loading** (unknown d) — same class of risk the Vulkan
  probe retired; evidence line in ReShade.log.
* **32-bit design A dependencies** (unknown f) — x86 extension parity and cross-process import;
  spike-answered before any 32-bit code is written, with design B as the written fallback.

## 9. Phases

1. **Probes (one session).** Build + run `spike-gl64.exe` and `spike-gl32.exe` (§2, §5); in DOOM
   x64 GL, land the logging-only skeleton and read answers (a)–(e); pick the 32-bit target title
   and repeat the in-game probe there. *Exit: every §2/§5 unknown has a logged answer; go/no-go on
   design A for 32-bit; rtv branch (b) identified.*
2. **64-bit transport (`mode=1`).** `feed_gl.h` + `InitSessionGl` + `BuildResourcesGl` +
   `FeedFrameGl` through the split-screen test. *Exit: half-frame round trip visible in DOOM GL;
   fence values advancing in dlss5-feed.log; no GL errors in the guard's `glGetError` sweep.*
3. **64-bit full path (`mode=2`).** *Exit: NR add-on lines ("feature created" / "evaluation
   succeeded") in ReShade.log; MV probe reporting sane pixel magnitudes; visually stable DLAA in
   motion; clean session teardown on quit.*
4. **32-bit path.** Protocol v2 + host `MakeSharedTexHost` + stub GL branch; transport test, then
   full path on the 32-bit title. *Exit: same criteria as 2–3, read from dlss5-feed.log +
   host64\dlss5-feed-host.log + host64\ReShade.log.*
5. **Polish.** README install section + Building/requirements rows, build.yml step rename, version
   bump, `gl_srgb` knob only if phase 3 demanded it. *Exit: green CI; README rows earned by hand.*

Estimate: 3–5 sessions — 2–3 for the 64-bit path if the probes are green (no layer contingency
exists to add), +1–2 for the 32-bit path, most of whose transport code is the already-proven
`feed_gl.h`.

---

*Flagged as unverified rather than asserted:* ReShade's GL backend implementation details (backend
sources are not vendored — only its headers): the OPAQUE_WIN32-only import behaviour of GL
`create_resource`/`create_fence` is inferred from the Vulkan precedent and double-checked by a
throwaway probe call; the rtv's GL representation, the reported backbuffer format, effect-time
context currency, 32-bit x86 driver extension parity, and cross-process GL import are all phase-0
items, not assumptions the design leans on.
