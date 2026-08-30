# DLSS5-Feeder for Vulkan games — plan

## Context

DLSS5-Feeder currently covers D3D11 (private D3D12 device + shared textures), D3D12 (same-device),
and D3D9 (via a dgVoodoo2 wrapper). Vulkan is the last major API left. First target:
**DOOM (2016)**, `E:\SteamLibrary\steamapps\common\DOOM\DOOMx64vk.exe` — a 64-bit Vulkan renderer
with no DLSS of its own (the folder also ships `DOOMx64.exe`, the OpenGL build; ignore it).

Two facts were established before writing this plan, and they decide the whole design.

**1. The DLSS 5 neural-rendering add-on is D3D12-only.** Every NGX symbol in
`renodx-dlss5.addon64` is `NVSDK_NGX_D3D12_*`:

```
NVSDK_NGX_D3D12_AllocateParameters   NVSDK_NGX_D3D12_Init_Ext
NVSDK_NGX_D3D12_CreateFeature        NVSDK_NGX_D3D12_ReleaseFeature
NVSDK_NGX_D3D12_EvaluateFeature      NVSDK_NGX_D3D12_Shutdown1
NVSDK_NGX_D3D12_EvaluateFeature_C    NVSDK_NGX_D3D12_DestroyParameters
```

No `NVSDK_NGX_VULKAN_*`, no `vkGetDeviceProcAddr`, no `vulkan-1`. So even though NGX *does* have a
Vulkan API, running DLSS on the game's Vulkan device would be pointless — the add-on would never see
the call and would never insert its neural pass. **The evaluate must stay on a D3D12 device.**

**2. ReShade exposes the Vulkan primitives we need.** `device_api::vulkan` exists, and `get_native()`
returns a real `VkDevice` for a device, `VkCommandBuffer` for a command list, `VkQueue` for a queue
(`reshade_api_device.hpp:270-273`).

So the Vulkan path is *structurally the D3D11 path with a different far side*: game → shared
resources → private D3D12 device → NGX DLAA + NR → shared result → back over the frame. Everything
from `InitSession` through `CreateDlssFeature` and the evaluate is reused **unchanged**; only the
transport is new.

```
 Vulkan game process
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │ ReShade (Vulkan layer) → LaunchPad → DLSS5_Feed.fx → dlss5-feed.addon64:      │
 │                                                                              │
 │   VkImages imported from D3D12 shared resources ──┐                          │
 │   vkCmdCopyImage frame/depth/MV into them          │  same process,          │
 │   signal timeline semaphore (= imported D3D12 fence)│  two APIs              │
 │                                                    ▼                         │
 │   private D3D12 device ── NGX_D3D12_EVALUATE_DLSS ── renodx-dlss5 hooks here  │
 │                                                    │                         │
 │   wait semaphore ◄─────────────────────────────────┘                         │
 │   blit shared Output image over the backbuffer                               │
 └──────────────────────────────────────────────────────────────────────────────┘
```

## 1. Transport: Vulkan ↔ D3D12 in one process

**Direction: D3D12 creates, Vulkan imports.** D3D12 cannot open a Vulkan-exported handle, but Vulkan
can import a D3D12 one — this is exactly what `VK_KHR_external_memory_win32` is for. Same shape as
`MakeSharedPair`, which already learned this lesson on D3D11 (only one direction worked).

* **Images.** D3D12 `CreateCommittedResource(D3D12_HEAP_FLAG_SHARED)` + `CreateSharedHandle` for
  Color / Output / Depth / MV, exactly as today. On the Vulkan side: `VkImage` created with
  `VkExternalMemoryImageCreateInfo`, memory imported with `VkImportMemoryWin32HandleInfoKHR`
  (`handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`, falling back to
  `D3D11_TEXTURE_BIT`), and — **required for imported D3D12 resources** — a
  `VkMemoryDedicatedAllocateInfo` naming the image.
* **Fences.** D3D12 `CreateFence(D3D12_FENCE_FLAG_SHARED)` + `CreateSharedHandle`; Vulkan imports it
  as a **timeline semaphore** via `VkImportSemaphoreWin32HandleInfoKHR` with
  `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT`. A D3D12 fence and a Vulkan timeline semaphore
  are the same object by design, so the existing `fence_value` counter keeps working verbatim.
* **Formats must match exactly** across the boundary: `R8G8B8A8_UNORM ↔ VK_FORMAT_R8G8B8A8_UNORM`,
  `R16G16B16A16_FLOAT ↔ VK_FORMAT_R16G16B16A16_SFLOAT`, `R32_FLOAT ↔ VK_FORMAT_R32_SFLOAT`,
  `R16G16_FLOAT ↔ VK_FORMAT_R16G16_SFLOAT`. `TypedColorFormat`/`OutputFormatFor` gain a
  DXGI→VkFormat companion table; unmapped formats disable the feed with a clear log line, as today.

## 2. The blocking unknown — device extensions (phase 0)

Vulkan device extensions must be enabled **at `vkCreateDevice` time**. If DOOM does not enable
`VK_KHR_external_memory_win32` and `VK_KHR_external_semaphore_win32`, we cannot import anything, and
by the time our add-on gets `init_device` the device already exists. There is no post-hoc way in.

**This is the make-or-break, and it is cheap to test.** A ~30-line probe in the add-on: on
`init_device` with `device_api::vulkan`, `LoadLibrary("vulkan-1.dll")`, then `vkGetDeviceProcAddr`
for `vkGetMemoryWin32HandlePropertiesKHR`, `vkImportSemaphoreWin32HandleKHR` and
`vkGetSemaphoreCounterValue`. Non-null means the extensions are live and the whole plan proceeds;
null means they are not.

**If they are null**, the fallback is a small **Vulkan layer of our own** (`dlss5-feed-layer`) that
intercepts `vkCreateDevice` and appends the two extensions to `VkDeviceCreateInfo` before calling
down — a JSON manifest plus a registry entry, the same mechanism ReShade itself uses for Vulkan.
Perhaps 250 lines, well-trodden, but it is a second shippable component and an extra install step,
so it must not be assumed until the probe says so.

## 3. Sync model

Semaphore signal/wait are **submit-level** operations in Vulkan — they cannot be recorded into a
command buffer, and ReShade owns the submit of its own command list. The trick that avoids needing
our own command pool or queue:

> `vkQueueSubmit` with `commandBufferCount = 0` and one signal (or wait) semaphore is legal and
> nearly free.

Per frame, inside `reshade_render_technique` for the `DLSS5_Feed` technique:

1. On ReShade's `command_list` (a `VkCommandBuffer`), via ReShade's own `barrier()` + `copy_texture_region`:
   transition and copy backbuffer/depth/MV into the three imported images. Using ReShade's API keeps
   its layout tracking correct, exactly as the D3D12 path does.
2. `rt->get_command_queue()->flush_immediate_command_list()` — submits that work.
3. Bare `vkQueueSubmit` on the game's `VkQueue`: signal the imported semaphore to `n`.
4. `g.queue->Wait(g.fence12, n)` then the existing D3D12 evaluate, then `Signal(fence12, n_out)` —
   **all unchanged code**.
5. Bare `vkQueueSubmit`: wait semaphore `n_out`.
6. Blit the Output image over the backbuffer (ReShade `copy_texture_region`, or a small graphics
   pass if a format conversion is needed).

`VkQueue` is externally synchronized and ReShade documents its `command_queue` as not thread-safe
(`reshade_api_device.hpp:1211`). Our callback runs on the render thread inside ReShade's effect
processing, so no ReShade submit races us — but a heavily threaded engine like idTech 6 may submit
from elsewhere. Mitigation: a cfg knob `pipeline=1` selecting a **+1-frame model** (wait on `n-1`,
never blocking the current frame), which also hides scheduler jitter. Ship model A, keep B one line
away.

## 4. What changes in the code

| Piece | Change |
| --- | --- |
| `InitSession` / NGX / `CreateDlssFeature` / evaluate | **none** — the D3D12 session is already what we want |
| `Feed` struct | add `VkDevice`, `VkQueue`, imported `VkImage`/`VkDeviceMemory`/`VkSemaphore` arrays, loaded fn pointers |
| `MakeSharedPair` | gains a Vulkan-import sibling (`ImportSharedToVulkan`) |
| `FeedFrame` | third branch `FeedFrameVk`, mirroring `FeedFrame12`'s structure |
| `BlitOutputToBackbuffer` | Vulkan variant, ideally just `copy_texture_region` |
| format helpers | DXGI ↔ VkFormat table |
| `dlss5-feed.cfg` | `pipeline` (0/1) |

Vulkan is loaded dynamically (`vulkan-1.dll` + `vkGetDeviceProcAddr`); no SDK dependency is added to
the build, and non-Vulkan games are unaffected. The existing SEH guards, abort-don't-submit
behaviour, hook-arming grace and warm-up rebuild all apply unchanged, since the NGX half is identical.

## 5. Install differences (for the README later)

ReShade for Vulkan is **not** a `dxgi.dll` next to the exe — it installs as a **Vulkan layer**
(global, via the ReShade installer, registry-registered). Consequences:

* The add-on still goes next to the game exe, and ReShade finds it there.
* `renodx-dlss5.addon64` + `nvngx_dlssnr.dll` + `nvngx_dlss.dll` also go next to the exe, as usual —
  our private D3D12 device is created in-process, so the NR add-on hooks it exactly as it does today.
* If our own layer turns out to be needed, it needs its own manifest + registry entry, and must be
  ordered *before* ReShade's layer.

## 6. Risks

* **Extensions not enabled** (§2) — the one that can force a whole extra component. Test first.
* **DOOM's depth buffer** — Vulkan depth access through ReShade's Generic Depth is less battle-tested
  than D3D11's; DOOM may also use a reversed/unusual depth format. `depth_inverted` and the debug
  view already exist for this.
* **LaunchPad on Vulkan** — ReShade compiles effects to SPIR-V and compute is available, so it should
  work, but it has not been verified on this path. `ReshadeMotionEstimation` is the fallback provider.
* **Queue thread-safety** (§3) — mitigated by the pipelined mode.
* **Layout/queue-family ownership** — imported images are `VK_SHARING_MODE_EXCLUSIVE`; if DOOM uses
  multiple queue families we may need ownership transfers. Start on the graphics queue only.
* DOOM 2016 ships its own TAA; it should be **off** so DLAA is not fed pre-blurred pixels.

## 7. Phases

1. **Probe (half a day).** Add the Vulkan-extension probe to the add-on, run DOOM, read the log.
   Go / need-a-layer decision. Also confirms ReShade attaches at all and logs the swapchain format.
2. **Import spike.** Import one D3D12 shared texture + fence into the game's Vulkan device; log
   success and the `VkFormat`s. No frame path yet.
3. **Transport (`mode=1`).** Copy the frame into the shared image and straight back out — the
   split-screen trick from the 32-bit work proves the round trip visually.
4. **Full path (`mode=2`).** DLAA + NR, `MV_SIGN`/debug-view validation as done for the other games.
5. **Polish.** `pipeline` knob, README section, release.

Realistically 2–4 sessions if the probe is green; add one for the layer if it is not.
