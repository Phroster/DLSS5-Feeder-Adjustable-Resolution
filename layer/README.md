# VK_LAYER_feed_vk

A ~19 KB Vulkan layer whose only job is to make DLSS5-Feeder's Vulkan transport
possible in games that would otherwise block it. **Since v0.5.2 it is a fallback**:
the add-on does the same thing from inside the process (`src/feed_vk_hook.h`) and
needs no launcher.

## Why it exists

The transport imports the feeder's D3D12 fences and textures into the game's own
`VkDevice`. That needs four KHR external-interop extensions plus the
`timelineSemaphore` feature — and Vulkan requires all of them to be enabled at
**`vkCreateDevice`**. Games enable only what they need:

| Game | Situation |
| --- | --- |
| DOOM (2016) | all present — nothing to add |
| Tekken 3 Recomp | only `timeline_semaphore` + `external_memory_win32` (both added by ReShade itself) — the rest has to be appended |

The add-on normally appends them itself: ReShade loads add-ons inside its
`vkCreateInstance` hook, before the game's `vkCreateDevice`, and the add-on hooks
that export. This layer does the identical job from outside the process, for the
case where that hook cannot run (it was not installed, or the game creates its
device some way the hook does not intercept — `dlss5-feed.log` says which).

**You only need this if `dlss5-feed.log` still says the entry points are missing** —
it names this layer as the fallback.

## Use

```
run-with-feed-layer.bat        "E:\path\to\game.exe"     64-bit game
x86\run-with-feed-layer32.bat  "E:\path\to\game.exe"     32-bit game (DXVK)
```

The 32-bit build lives in its own `x86\` subdirectory, and that is not tidiness:
the Vulkan loader tries **every** manifest it finds on `VK_LAYER_PATH`, so two
same-named manifests in one folder would have it load the wrong-bitness DLL and
silently skip the layer. Each script points `VK_LAYER_PATH` at its own folder.

Both set `VK_LAYER_PATH` and `VK_INSTANCE_LAYERS=VK_LAYER_feed_vk`
for that launch only. Registry implicit-layer keys are deliberately avoided:
overlays and capture tools rewrite `HKCU\...\Vulkan\ImplicitLayers` behind your
back, and a per-launch environment variable cannot be clobbered or leak into
other games.

For a Steam game, put the same two `set` lines in the game's launch options via a
wrapper script, or just launch the exe directly with the bat.

`feed-vk-layer.log` appears next to the DLL and lists exactly which extensions
were already present, which were added, and whether `timelineSemaphore` had to be
switched on.

## What it does

In `vkCreateDevice` only: append the extensions from the list below that the
driver supports and the app did not already request, enable `timelineSemaphore`
if no features struct in the chain already does, then call down. Everything else
passes straight through — no dispatch tables, no per-frame cost, no behaviour
change for the game.

```
VK_KHR_external_memory[_win32]      VK_KHR_dedicated_allocation
VK_KHR_external_semaphore[_win32]   VK_KHR_get_memory_requirements2
VK_KHR_timeline_semaphore
```

If `vkCreateDevice` then fails, the layer retries with the app's original,
untouched list — it can never be the reason a game refuses to start.

## Building

`build-layer.bat` (MSVC + the Vulkan headers under `external/vulkan`) builds both
architectures from the same source: `VkLayer_feed_vk.dll` here, with
`VkLayer_feed_vk.json`, and `x86\VkLayer_feed_vk32.dll` with
`x86\VkLayer_feed_vk32.json`.

Both link with `/DEF:feed_vk_layer.def`. On x86 `VKAPI_CALL` is `__stdcall`, so a
bare `/EXPORT:` or `__declspec(dllexport)` would emit the decorated `_name@4` and
the loader's `GetProcAddress("vkNegotiateLoaderLayerInterfaceVersion")` would miss
it. A `.def` exports the plain name on both architectures, so there is no
arch-specific export rule to get wrong.

## Credit

The need for this layer, and three of its implementation traps, were reported by
the Tekken 3 Recomp integration: match the loader chain node on `sType` **and**
`function == VK_LAYER_LINK_INFO` (the first `sType` match is often the features
node, where a 32-bit flag sits where a pointer is expected → instant AV); resolve
`vkEnumerateDeviceExtensionProperties` by name through GIPA rather than linking
it; and prefer `VK_LAYER_PATH` over the registry.
