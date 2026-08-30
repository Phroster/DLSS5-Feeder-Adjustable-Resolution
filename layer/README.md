# VK_LAYER_feed_vk

A ~19 KB Vulkan layer whose only job is to make DLSS5-Feeder's Vulkan transport
possible in games that would otherwise block it.

## Why it exists

The transport imports the feeder's D3D12 fences and textures into the game's own
`VkDevice`. That needs four KHR external-interop extensions plus the
`timelineSemaphore` feature — and Vulkan requires all of them to be enabled at
**`vkCreateDevice`**, long before a ReShade add-on gets to see the device. Games
enable only what they need:

| Game | Situation |
| --- | --- |
| DOOM (2016) | all present (ReShade enables them) — layer **not** needed |
| Tekken 3 Recomp | only `timeline_semaphore` + `external_memory_win32` — feed fails without the layer |

Nothing in-process can fix this after the fact, and the game is closed-source.
A layer is the only place the extension list can still be changed.

**You only need this if `dlss5-feed.log` says the entry points are missing** —
it names this layer as the fix.

## Use

```
run-with-feed-layer.bat "E:\path\to\game.exe"
```

That sets `VK_LAYER_PATH` (this folder) and `VK_INSTANCE_LAYERS=VK_LAYER_feed_vk`
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

`build-layer.bat` (MSVC + the Vulkan headers under `external/vulkan`). Output:
`VkLayer_feed_vk.dll`, used together with `VkLayer_feed_vk.json` in this folder.

## Credit

The need for this layer, and three of its implementation traps, were reported by
the Tekken 3 Recomp integration: match the loader chain node on `sType` **and**
`function == VK_LAYER_LINK_INFO` (the first `sType` match is often the features
node, where a 32-bit flag sits where a pointer is expected → instant AV); resolve
`vkEnumerateDeviceExtensionProperties` by name through GIPA rather than linking
it; and prefer `VK_LAYER_PATH` over the registry.
