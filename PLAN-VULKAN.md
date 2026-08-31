# DLSS5-Feeder for Vulkan games — plan (rev. 2, after review)

## Context

DLSS5-Feeder covers D3D11 (private D3D12 device + shared textures), D3D12 (same-device), and D3D9
(via a dgVoodoo2 wrapper). Vulkan is the last major API left. First target: **DOOM (2016)**,
`E:\SteamLibrary\steamapps\common\DOOM\DOOMx64vk.exe` — 64-bit Vulkan, no DLSS of its own (the
folder's `DOOMx64.exe` is the OpenGL build; ignore it).

Facts established up front, which decide the design:

**1. The DLSS 5 neural-rendering add-on is D3D12-only.** Every NGX symbol in
`renodx-dlss5.addon64` is `NVSDK_NGX_D3D12_*`:

```
NVSDK_NGX_D3D12_AllocateParameters   NVSDK_NGX_D3D12_Init_Ext
NVSDK_NGX_D3D12_CreateFeature        NVSDK_NGX_D3D12_ReleaseFeature
NVSDK_NGX_D3D12_EvaluateFeature      NVSDK_NGX_D3D12_Shutdown1
NVSDK_NGX_D3D12_EvaluateFeature_C    NVSDK_NGX_D3D12_DestroyParameters
```

No `NVSDK_NGX_VULKAN_*`, no `vkGetDeviceProcAddr`, no `vulkan-1`. NGX itself has a Vulkan API, but
using it would be pointless — the add-on would never see the call and never insert its neural pass.
**The evaluate stays on a private D3D12 device**, exactly as on the D3D11 path.

**2. ReShade's add-on API already implements cross-API sharing.** Verified in the vendored headers
(`reshade_api_device.hpp`, `reshade_api_pipeline.hpp`):

* `device::create_resource(desc, …, shared_handle)` with `resource_flags::shared |
  shared_nt_handle`: *"When that variable is a valid handle, the resource is **imported** from that
  shared handle."*
* `device::create_fence(initial, fence_flags::shared, …, shared_handle)`: same import semantics —
  *"Shared fences can be imported/exported from/to **different graphics APIs** and/or processes."*
* `command_queue::signal(fence, value)` / `wait(fence, value)`: queue-side semaphore operations,
  executed inside ReShade's own queue synchronization.

So the Vulkan path is the D3D11 path with the transport rewritten in **ReShade API calls instead of
raw interop**: our private D3D12 device creates the shared textures and fences (existing code,
unchanged), and the Vulkan side imports them *through ReShade*, which also registers the images in
its internal tracking — required for its `barrier()`/`copy_texture_region()` to work on them at all
(they look up per-resource data ReShade only has for resources it created or wrapped; raw imported
`VkImage`s would not be in that registry).

```
 Vulkan game process
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │ ReShade (Vulkan layer) → MV provider → DLSS5_Feed.fx → dlss5-feed.addon64:    │
 │                                                                              │
 │   api::resource imports of D3D12 shared textures ──┐                         │
 │   ReShade barrier() + copy_texture_region()         │  same process,         │
 │   command_queue::signal(fence_in, n)                │  two APIs              │
 │                                                     ▼                        │
 │   private D3D12 device ── NGX_D3D12_EVALUATE_DLSS ── renodx-dlss5 hooks here  │
 │                                                     │                        │
 │   command_queue::wait(fence_out, n) ◄───────────────┘                        │
 │   blit shared Output over the backbuffer (copy_texture_region)               │
 └──────────────────────────────────────────────────────────────────────────────┘
```

## 1. Transport

**Direction: D3D12 creates, Vulkan imports.** Vulkan can import D3D12 memory
(`VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`, dedicated allocation) and a D3D12 fence
(`VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT` → timeline semaphore — the two are the same
object by design). The reverse does not exist: D3D12 cannot open Vulkan-exported opaque handles.
Same one-way lesson `MakeSharedPair` already encodes for D3D11.

All of that machinery lives *inside ReShade's backend*, though. Our code only does:

1. D3D12 side (ours, unchanged): `CreateCommittedResource(D3D12_HEAP_FLAG_SHARED)` +
   `CreateSharedHandle` for Color/Output/Depth/MV; `CreateFence(D3D12_FENCE_FLAG_SHARED)` ×2 +
   `CreateSharedHandle`.
2. Vulkan side: `rt->get_device()->create_resource(desc, nullptr, usage, &res, &handle)` per
   texture and `create_fence(0, fence_flags::shared | non_monitored?, &f, &handle)` per fence,
   passing the D3D12 handles in. ReShade performs the Vulkan import and owns the objects.

**Formats.** Must match exactly across the boundary. The mapping table needs at least:
`R8G8B8A8_UNORM`, **`B8G8R8A8_UNORM`** (the most common Vulkan swapchain format — easy to forget),
`R16G16B16A16_FLOAT`, `R10G10B10A2_UNORM`, `R32_FLOAT`, `R16G16_FLOAT`, plus the `_SRGB` views
treated as their UNORM base as usual. When Output and backbuffer differ (e.g. RGBA8 output over a
BGRA8 backbuffer), the copy home **must be a blit** — a plain image copy between same-size formats
is legal in Vulkan but bit-reinterprets (red and blue swap). ReShade's `copy_texture_region` blits
when formats differ; never use a raw copy there.

## 2. The blocking unknown — extensions AND features (phase 0)

Vulkan device extensions and features are baked in at `vkCreateDevice`. The import needs
`VK_KHR_external_memory_win32` + `VK_KHR_external_semaphore_win32`, and D3D12-fence import needs
timeline semaphores — which are gated by the **`timelineSemaphore` feature** in
`VkDeviceCreateInfo::pNext`, not just an extension string.

Two facts frame the odds:

* **The game cannot have enabled any of this.** DOOM's Vulkan renderer shipped July 2016;
  `VK_KHR_external_memory_win32` was published in early 2017 and timeline semaphores in 2019. Date
  arithmetic settles the game's side: no.
* **But the device is created through ReShade's Vulkan layer**, and a layer may append extensions
  and features at `vkCreateDevice`. ReShade enables extras for its own use (it needs timeline
  semaphores itself, and its shared-fence API implies the external-semaphore path on capable
  drivers). So the question is decided by **ReShade's version and behaviour**, not by the game.

**The probe must be behavioural, not cosmetic.** `vkGetDeviceProcAddr` returning non-NULL does not
prove an extension was enabled (drivers return pointers for unenabled extensions; calling them is
UB). Phase 0 therefore = a real end-to-end attempt inside DOOM: create one small shared D3D12
texture + one shared fence on our private device, try `create_resource`/`create_fence` imports via
ReShade, `signal`/`wait` once, log every result. Green = proceed; red = the layer fallback.

The same probe run answers two more things for free, from logs we already write:

* **Does ReShade-as-a-layer hook our private D3D12 device?** The NR add-on needs `init_device` for
  it. Proven when ReShade loads as `dxgi.dll` (Metro); the layer loading mode is the same DLL and
  should install the same hooks, but it is unverified — the "D3D12 NGX hooks installed" line in the
  host ReShade log is the tell.
* **Are the add-ons even discovered?** For Vulkan games the ReShade DLL does not live next to the
  exe (global implicit layer). If `dlss5-feed.addon64`/`renodx-dlss5.addon64` next to the exe are
  not picked up, `AddonPath=.` in the game's ReShade.ini is the fix; document whichever is true.

**Fallback if the probe is red:** a minimal Vulkan implicit layer of our own (`dlss5-feed-layer`)
that intercepts `vkCreateDevice` and appends the two extensions **and the `timelineSemaphore`
feature** (and, if needed at instance level, intercepts `vkCreateInstance` likewise). JSON manifest +
registry entry, ordered before ReShade's layer. ~250 lines, well-trodden mechanism (ReShade itself
is such a layer), but a second shippable component and an extra install step — only if the probe
says so.

## 3. Sync model

No raw `vkQueueSubmit` of our own — ever. `VkQueue`s are externally synchronized, the game (idTech 6
is aggressively multi-threaded) and ReShade both submit to them, and ReShade's layer serializes that
through its own locks; a bare submit on the native queue would bypass those locks and race both.
ReShade's `command_queue::signal/wait` do the same thing from inside its synchronization.

Per frame, inside `reshade_render_technique` for the `DLSS5_Feed` technique:

1. On ReShade's command list: `barrier()` + `copy_texture_region()` backbuffer/depth/MV into the
   three imported input images (their "rest" usages mirror the D3D12 path's).
2. `queue->signal(fence_in, n)` — ReShade flushes its immediate command list before a signal, so
   ordering is handled.
3. D3D12 side, unchanged: `g.queue->Wait(fence12_in, n)` → SEH-guarded evaluate → `Signal(fence12_out, n)`.
4. `queue->wait(fence_out, n)` — the game's queue waits GPU-side; no CPU stall.
5. Blit Output over the backbuffer via `copy_texture_region` (blit semantics for format conversion),
   then restore usages.

Cfg knob `pipeline=1` for a +1-frame model (wait on `n-1`) stays in the plan as the jitter escape
hatch; ship the same-frame model first, as everywhere else.

## 4. What changes in the code

| Piece | Change |
| --- | --- |
| `InitSession` / NGX / `CreateDlssFeature` / evaluate / SEH / grace / warm-up | **none** — the private-D3D12 half is reused verbatim |
| `Feed` struct | ReShade-typed additions only: imported `api::resource[4]`, `api::fence` ×2 (no raw Vulkan handles, no function pointers) |
| `MakeSharedPair` | D3D12-create half reused; new `ImportSharedViaReShade()` using `create_resource`/`create_fence` |
| `FeedFrame` | third branch `FeedFrameVk` — structurally `FeedFrame12` with the transport copies added and `command_queue::signal/wait` instead of native fence calls |
| format helpers | DXGI ↔ `api::format` table incl. BGRA8; blit-not-copy rule on the way home |
| `dlss5-feed.cfg` | `pipeline` (0/1) |

No Vulkan SDK dependency, no `vulkan-1.dll` loading, no raw `Vk*` types in our code at all — that is
the main payoff of routing everything through ReShade's API. Non-Vulkan games are untouched.

## 5. Install differences (README later)

* ReShade for Vulkan is a **global implicit layer** (ReShade installer), not a `dxgi.dll`.
* Add-ons + `nvngx_*.dll` still go next to the game exe **if** discovery works there (phase-0
  answer); otherwise document `AddonPath`.
* If our own layer is needed, it ships with a manifest + registry setup step.

## 6. Risks

* **Extensions/features not enabled and ReShade's layer does not add them** (§2) — the one that
  forces the extra component. Behavioural probe first.
* **ReShade's shared-handle import path on the Vulkan backend is less exercised** than D3D — if
  `create_resource`-import fails on a format the extension supports, fall back to raw Vulkan import
  for that resource only (accepting manual layout barriers around ReShade's tracking).
* **NR add-on arming in a Vulkan process** (§2 probe) — mechanism proven only under dxgi.dll loading.
* **DOOM's depth via Generic Depth on Vulkan** — historically the weakest depth path in ReShade;
  `depth_inverted`, the debug view and `ReshadeMotionEstimation` (if LaunchPad misbehaves on SPIR-V)
  are the knobs.
* **Queue-family ownership** of imported images if DOOM presents from a non-graphics queue — start
  graphics-queue-only, exclusive sharing.
* DOOM's own TAA should be **off** so DLAA is not fed pre-blurred pixels.

## 7. Phases

1. **Probe (half a day).** In-game behavioural import test + the two free answers (NR hooks armed?
   add-ons discovered?). Go / need-a-layer decision with evidence, not dates or ProcAddr guesses.
2. **Transport (`mode=1`).** Frame into the shared image and straight back — the split-screen trick
   proves the round trip visually. (The former "import spike" collapses into the probe.)
3. **Full path (`mode=2`).** DLAA + NR; `MV_SIGN`/debug-view validation as on every other title.
4. **Polish.** `pipeline` knob, README section, release.

2–3 sessions if the probe is green; +1 for the layer if it is not.

---

*Rev. 2 notes — what the review changed:* the probe became behavioural (ProcAddr proves nothing);
the `timelineSemaphore` **feature** requirement was added; the game-can't-have-enabled-it date fact
and the ReShade-layer-may-have counter-fact replaced a vaguer framing; the entire raw-Vulkan
transport (hand-rolled `VkImportMemoryWin32HandleInfoKHR`, bare `vkQueueSubmit` semaphore posts) was
replaced by ReShade's documented shared-handle `create_resource`/`create_fence` + queue
`signal`/`wait`, which also fixes two latent bugs the rev. 1 design would have hit: ReShade barriers
crashing on unregistered raw `VkImage`s, and a queue-submission race outside ReShade's locks. BGRA8
and the blit-not-copy rule were added to the format story.
