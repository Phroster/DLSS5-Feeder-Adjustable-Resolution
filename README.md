# DLSS5-Feeder

**DLSS 5 neural rendering in D3D11 and D3D12 games that ship without any DLSS.**

DLSS 5's neural-rendering add-on only works by hooking a game's own DLSS calls. A game that has no
DLSS never makes those calls, so the add-on sits idle. **DLSS5-Feeder makes the calls itself.** It
builds a complete DLSS DLAA "contract" out of what ReShade already has — the frame being processed,
the depth buffer, and iMMERSE **LaunchPad**'s optical-flow motion vectors — runs a genuine DLSS
evaluate (on a private D3D12 device for D3D11 games, directly on the game's own device for D3D12
games), lets the DLSS 5 neural-rendering add-on hook into that evaluate, and copies the neural
result back into the frame. All inside ReShade's effect chain.

```
game frame → ReShade effects → [MartysMods_Launchpad] → [DLSS5_Feed] → DLSS5-Feeder:
                                 motion vectors            depth + MV      DLSS DLAA + DLSS 5 NR on a private D3D12 device
                                                                           ↓
                                            neural output written back over the frame → later effects → present
```

### Status

Proven working in **Metro 2033 Redux** (64-bit, D3D11) and **The Lord of the Rings: War in the
North - Legacy Edition** (64-bit, D3D12), both without native DLSS: the DLSS 5 neural-rendering
add-on reports `feature 18 created … inline feature 18 evaluation succeeded` at native 4K/1440p,
driven entirely by ReShade depth + LaunchPad motion vectors. This is a first, rough version — expect
the temporal quality of estimated motion vectors (some ghosting in fast motion, softness on thin
moving geometry), and the HUD is processed along with the scene.

It is not game-specific: any 64-bit D3D11 or D3D12 game with a working ReShade depth buffer and
LaunchPad motion vectors should work.

## Install (5 steps)

You need a 64-bit **DirectX 11 or DirectX 12** game (DX9/DX10/32-bit won't work).

1. **ReShade with add-on support** — get it from **https://reshade.me** , run the installer, pick your
   game's `.exe`, choose **Direct3D 10/11/12**, and tick **"Enable loading of add-ons"** (the full /
   unsigned build). This puts `dxgi.dll` next to the game.
2. **DLSS 5 Feeder** — download **`dlss5-feed.addon64`** and **`DLSS5_Feed.fx`** from the
   **[latest release](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest)**.
   Put `dlss5-feed.addon64` next to the game `.exe` (same folder as `dxgi.dll`), and
   `DLSS5_Feed.fx` into the `reshade-shaders/Shaders/` folder.

3. **LaunchPad (motion vectors)** — from iMMERSE: **https://github.com/martymcmodding/iMMERSE** →
   green **Code ▸ Download ZIP**. Copy these into your game's `reshade-shaders/` folder:

   `Shaders\MartysMods_LAUNCHPAD.fx`, the whole `Shaders\MartysMods\` folder, and
   `Textures\iMMERSE_bluenoise_opt.png`.
4. **DLSS 5 neural-rendering add-on** — `renodx-dlss5.addon64` and its model `nvngx_dlssnr.dll`.
   Easiest is the **RHI** installer, which downloads and deploys them for you:
   **https://github.com/RankFTW/RHI/releases** (or get them from the RenoDX Discord). Put both next
   to the game `.exe`. Also drop a **`nvngx_dlss.dll`** there (any DLSS game has one, or use
   [DLSS Swapper](https://github.com/beeradmoore/dlss-swapper)).
5. **Turn it on in-game** — press **Home** for the ReShade overlay, enable **MartysMods_Launchpad**,
   then enable **DLSS 5 Feed** *below it*, and enable neural rendering in the **DLSS 5 Neural
   Rendering** panel. Keep the game's MSAA/SSAA **off**.

That's it. Check `dlss5-feed.log` (next to the game `.exe`) for `feature ready … DLAA` and
`frame N delivered`; the DLSS 5 add-on's `feature 18 created / evaluation succeeded` shows in
`ReShade.log`. `dlss5-feed.cfg` is created automatically with working defaults.

> **Do I need the DLSS 5 DX11 *bridge*?** **No.** DLSS5-Feeder does the bridge's job for games that
> have no DLSS. The bridge — **https://github.com/NIGos/dlss5-dx11-bridge/releases** — is only for
> DX11 games that *already* have their own DLSS; don't run both for the same game.

## Details

### How it works

* `DLSS5_Feed.fx` (companion effect) converts LaunchPad's `Deferred::MotionVectorsTex` (delta-UV,
  `prev_uv = uv + mv`) into `DLSS5_MV` (RG16F, **pixels**), copies the raw hardware depth with
  ReShade's orientation fixes into `DLSS5_Depth` (R32F), and re-requests LaunchPad's optical flow
  every frame via its IPC predication buffer.
* `dlss5-feed.addon64` registers with the ReShade add-on API. After the `DLSS5_Feed` technique
  renders, it takes the backbuffer + those two textures, copies them into textures **shared** with a
  private D3D12 device (shared NT handles + a shared fence), runs `NGX_D3D12_EVALUATE_DLSS` in DLAA
  mode (render size = output size, no jitter), and blits the D3D12 output back onto the backbuffer.
  The DLSS 5 neural-rendering add-on (`renodx-dlss5.addon64`) detours that D3D12 evaluate and inserts
  its neural pass — it cannot tell the contract is synthetic.
* On a **D3D12 game** there is no transport at all: NGX runs on the game's own device and queue,
  motion vectors and depth are consumed zero-copy straight from the effect textures, and the
  feature survives alt-tabs and effect reloads untouched (only a real resolution change rebuilds).
* NGX calls are wrapped in SEH: if the (closed-source) DLSS 5 add-on faults — e.g. across a
  resolution change — the feed disables itself and the game keeps running, rather than crashing.

### Requirements

| Piece | Notes |
| --- | --- |
| 64-bit D3D11 or D3D12 game | NGX is 64-bit only. DX9 / DX10 / Vulkan / 32-bit not supported. |
| ReShade 6.8+ **with add-on support** (`dxgi.dll`) | Generic Depth add-on enabled and picking the scene depth. |
| DLSS 5 neural-rendering add-on (`renodx-dlss5.addon64`) + `nvngx_dlssnr.dll` | from its own author; this project does not include it. |
| `nvngx_dlss.dll` | a DLSS Super Resolution runtime next to the game (the driver's copy is used otherwise). |
| iMMERSE **LaunchPad** (`MartysMods_LAUNCHPAD.fx` + `MartysMods/*.fxh` + `Textures/iMMERSE_bluenoise_opt.png`) | from https://github.com/martymcmodding/iMMERSE — install it yourself; it is proprietary and is **not** bundled here. |
| `dlss5-feed.addon64` + `DLSS5_Feed.fx` | this project. |

### `dlss5-feed.cfg` (created automatically)

| Key | Default | Meaning |
| --- | --- | --- |
| `enabled` | 1 | 0 disables everything. |
| `mode` | 2 | 0 inert · 1 transport test (frame out → back, no NGX) · 2 full DLSS path. |
| `hdr` | -1 | -1 auto (FP16 / R11G11B10 backbuffer = HDR), 0 force SDR, 1 force HDR. |
| `depth_inverted` | -1 | -1 follow `RESHADE_DEPTH_INPUT_IS_REVERSED`, 0/1 force. |
| `flags` | -1 | raw `DLSS.Feature.Create.Flags` override. |
| `reset_every` | 0 | 1 = NGX Reset every frame (no temporal history; diagnostic). |
| `warmup_rebuild` | 180 | re-create the feature once after N delivered frames (works around the DLSS 5 add-on latching STANDBY/FAILED on its first create). |
| `rebuild` | 0 | change the number to re-create the feature once, by hand. |
| `log_frames` | 3 | first N frames logged in detail. |
| `create_delay` | 60 | frames to hold a feature (re)build after a runtime (re)init -- the DLSS 5 add-on arms its NGX hooks asynchronously, and calling in too early can crash. 0 disables. |
| `mv_scale_x/y` | 1.0 | extra motion-vector multiplier. |

Motion-vector **sign** and scale are also exposed in `DLSS5_Feed.fx`'s UI; if the image doubles/smears
while moving, flip a component of **MV_SIGN**. The `DLSS 5 Feed - debug view` technique shows the
vectors/depth being sent (static scene = grey, motion = colour).

### Log

`dlss5-feed.log` next to the add-on: resolved effect handles, the D3D12/NGX session, the contract
(`feature ready: WxH DLAA, flags=…`), `frame N delivered`, a timing line every 600 frames, and a
crash breadcrumb. The DLSS 5 add-on's own state (`feature 18 created`, `inline feature 18 evaluation
succeeded`) appears in `ReShade.log`.

### Building

MSVC (v143/v145) + Windows SDK. Dependencies not vendored: the **NGX SDK** (see
[`external/ngx/README.md`](external/ngx/README.md)); the ReShade add-on headers *are* included under
`external/reshade/include` (BSD-3-Clause, Patrick Mours). Then run `build.bat` (edit the `vcvars64.bat`
path if needed). Output: `build\dlss5-feed.addon64`. NGX links against the Release SDK, so the build
uses `/MD`.

### Limitations & roadmap

* **DLAA only** — render resolution = output resolution = the game's backbuffer. No upscaling perf
  gain yet; a jittered render-at-lower / output-at-higher upscaling mode is future work.
* Estimated motion vectors → temporal artifacts in fast motion; the UI is processed with the scene
  (a UI mask / pre-UI colour capture is future work).
* Exclusive-fullscreen swapchain churn can make some games reload effects repeatedly; windowed is
  smoother.
* Depends on a closed-source, community-distributed DLSS 5 add-on and the NGX runtime; both can change.

## Credits

* **D3D11↔D3D12 shared-texture / fence transport** adapted from NIGos'
  [dlss5-dx11-bridge](https://github.com/NIGos/dlss5-dx11-bridge) (MIT) — not re-hosted here.
* **Motion vectors:** Pascal Gilcher's iMMERSE LaunchPad (consumed at runtime, not bundled).
* **DLSS 5 neural rendering:** the RenoDX community's `renodx-dlss5` add-on.
* **ReShade** add-on API by Patrick Mours.
* **D3D12 stability findings** independently confirmed by the
  [Pizzawookiee fork](https://github.com/Pizzawookiee/DLSS5-Feeder)'s diagnostics.

## License

MIT — see [LICENSE](LICENSE). This covers only the code in this repository (`src/`, `shaders/`); the
dependencies above keep their own licenses.
