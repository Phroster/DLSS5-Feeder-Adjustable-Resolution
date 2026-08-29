# DLSS 5 Feed — DLSS 5 neural rendering for D3D11 games that have no DLSS

A ReShade add-on that *issues* the DLSS calls a game never makes. It builds a complete DLSS
DLAA "contract" out of what ReShade already has — the frame being processed, the Generic Depth
buffer and iMMERSE **LaunchPad**'s optical-flow motion vectors — runs a genuine DLSS evaluate on a
private D3D12 device, and writes the result back into the frame. The DLSS 5 neural-rendering
add-on (`renodx-dlss5.addon64`) detours exactly that D3D12 evaluate, so it treats the synthetic
contract like any game's DLSS and inserts its pass.

```
game frame → ReShade effects → [MartysMods_Launchpad] → [DLSS5_Feed] → add-on: DLSS DLAA + DLSS 5 NR (D3D12) → later effects → present
```

Expect: neural detail like a DLSS 5 game, temporal quality bounded by estimated motion vectors
(ghosting on fast pans, softness on thin moving geometry). The HUD is processed too.

## Requirements

| Piece | Notes |
| --- | --- |
| 64-bit D3D11 game | NGX is 64-bit only; DX12/DX9 not supported (yet) |
| ReShade 6.8+ **with add-on support** (`dxgi.dll`) | Generic Depth add-on enabled and picking the scene depth |
| `renodx-dlss5.addon64` + `nvngx_dlssnr.dll` | the DLSS 5 neural-rendering add-on and its model |
| `nvngx_dlss.dll` (optional) | a DLSS SR runtime next to the game; the driver's copy is used otherwise |
| iMMERSE `MartysMods_LAUNCHPAD.fx` + `MartysMods\*.fxh` + `Textures\iMMERSE_bluenoise_opt.png` | from github.com/martymcmodding/iMMERSE — install yourself, it is not redistributable |
| `dlss5-feed.addon64` + `shaders\DLSS5_Feed.fx` | this project |

## Install

1. Copy `dlss5-feed.addon64` next to the game executable (same folder as `dxgi.dll`).
2. Copy `DLSS5_Feed.fx` into `reshade-shaders\Shaders\`, next to `MartysMods_LAUNCHPAD.fx`.
3. In the ReShade overlay enable **MartysMods_Launchpad** and, *below it*, **DLSS 5 Feed**. Effects placed
   below "DLSS 5 Feed" run on top of the neural output (sharpening, grading…).
4. Enable neural rendering in the DLSS 5 add-on's own panel. Keep MSAA/SSAA off in the game so depth
   matches the backbuffer size.

`dlss5-feed.cfg` is written next to the add-on on first run and re-read while the game runs.

| Key | Default | Meaning |
| --- | --- | --- |
| `enabled` | 1 | 0 disables everything |
| `mode` | 2 | 0 inert · 1 transport test (frame goes out and comes straight back, no NGX) · 2 full DLSS path |
| `hdr` | -1 | -1 auto (FP16 / R11G11B10 backbuffer = HDR), 0 force SDR flags, 1 force HDR flags |
| `depth_inverted` | -1 | -1 follow `RESHADE_DEPTH_INPUT_IS_REVERSED`, 0/1 force |
| `flags` | -1 | raw `DLSS.Feature.Create.Flags` override |
| `reset_every` | 0 | 1 = NGX Reset every frame (no temporal history; diagnostic) |
| `warmup_rebuild` | 180 | re-create the DLSS feature once after N delivered frames (works around the DLSS 5 add-on latching STANDBY/FAILED on its first create) |
| `rebuild` | 0 | change the number to re-create the feature once, by hand |
| `log_frames` | 3 | first N frames are logged in detail |
| `mv_scale_x/y` | 1.0 | extra motion-vector multiplier (the effect already outputs pixels) |

The motion-vector **sign** and a scale are exposed in the `DLSS5_Feed.fx` UI; the `DLSS 5 Feed - debug view`
technique shows the vectors/depth that will be sent.

## Log

`dlss5-feed.log` next to the add-on: which effect handles were found, the D3D12/NGX session, the
contract that was built (`feature ready: WxH DLAA, flags=…`), `frame N delivered`, a timing line every
600 frames, and a crash breadcrumb if the process dies. The DLSS 5 add-on's own state
(`feature 18 created`, `inline feature 18 evaluation succeeded`) appears in `ReShade.log`.

## Building

MSVC (v143/v145) + Windows SDK. `build.bat` (edit the `vcvars64.bat` path if needed). The ReShade add-on
headers (`external\reshade\include`) and NVIDIA's NGX SDK (`external\ngx`, `nvsdk_ngx_d.lib`, needs `/MD`)
are vendored. Output: `build\dlss5-feed.addon64`.

## Credits

* D3D11↔D3D12 shared-texture/fence transport adapted from NIGos' [dlss5-dx11-bridge](https://github.com/NIGos/dlss5-dx11-bridge) (MIT).
* Motion vectors: Pascal Gilcher's iMMERSE LaunchPad (consumed, not bundled).
* DLSS 5 neural rendering: the RenoDX community's `renodx-dlss5` add-on.
