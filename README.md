# DLSS5-Feeder

**DLSS 5 neural rendering in games that ship without any DLSS — D3D11, D3D12, 32-bit, even DirectX 9.**

DLSS 5's neural-rendering add-on only works by hooking a game's own DLSS calls. A game that has no
DLSS never makes those calls, so the add-on sits idle. **DLSS5-Feeder makes the calls itself.** It
builds a complete DLSS DLAA "contract" out of what ReShade already has — the frame being processed,
the depth buffer, and estimated optical-flow motion vectors — runs a genuine DLSS evaluate, lets the
DLSS 5 neural-rendering add-on hook into that evaluate, and copies the neural result back into the
frame. All inside ReShade's effect chain.

```
game frame → ReShade effects → [motion vectors] → [DLSS5_Feed] → DLSS5-Feeder:
                                                    depth + MV     DLSS DLAA + DLSS 5 neural rendering
                                                                   ↓
                                    neural output written back over the frame → later effects → present
```

## Contents

- [Status](#status)
- [Install for a 64-bit game](#install-for-a-64-bit-game)
- [Install for a 32-bit game](#install-for-a-32-bit-game-beta)
- [Install for a DirectX 9 game](#install-for-a-directx-9-game-beta)
- [How it works](#how-it-works)
  - [The 32-bit path](#the-32-bit-path)
  - [The DirectX 9 path](#the-directx-9-path)
- [Requirements](#requirements)
- [Configuration](#configuration)
- [Logs and troubleshooting](#logs-and-troubleshooting)
- [Building](#building)
- [Limitations and roadmap](#limitations-and-roadmap)
- [Credits](#credits)
- [License](#license)

## Status

Proven working in five games covering every supported path:

| Game | Bitness / API | Result |
| --- | --- | --- |
| **Metro 2033 Redux** | 64-bit D3D11 | 4K DLAA + NR, HDR backbuffer |
| **The Lord of the Rings: War in the North – Legacy Edition** | 64-bit D3D12 | 4K, same-device path, 120 fps |
| **Splinter Cell: Blacklist** | 32-bit D3D11 | 60 fps, cross-process host |
| **BioShock Remastered** | 32-bit D3D11 (D3D9→D3D11 wrapper) | 4K, Luma HDR |
| **Fable Anniversary** | 32-bit **D3D9** via dgVoodoo2 | 1440p, 60 fps |

In each, the DLSS 5 add-on reports `feature 18 created … inline feature 18 evaluation succeeded`,
driven entirely by ReShade depth + estimated motion vectors.

It is not game-specific: any D3D11 or D3D12 game with a working ReShade depth buffer and a motion
vector provider should work — 64-bit directly, 32-bit via a bundled 64-bit helper process, D3D9 via
a wrapper.

**This is beta software.** Expect the temporal quality of *estimated* motion vectors (some ghosting
in fast motion, softness on thin moving geometry), and the HUD is processed along with the scene.

## Install for a 64-bit game

1. **ReShade with add-on support** — from **https://reshade.me**, run the installer, pick your game's
   `.exe`, choose **Direct3D 10/11/12**, and tick **"Enable loading of add-ons"** (the full /
   unsigned build). This puts `dxgi.dll` next to the game.
2. **DLSS5-Feeder** — from the
   **[latest release](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest)** download
   **`dlss5-feed.addon64`** and **`DLSS5_Feed.fx`**. Put `dlss5-feed.addon64` next to the game `.exe`
   (same folder as `dxgi.dll`), and `DLSS5_Feed.fx` into `reshade-shaders\Shaders\`.
3. **Motion vectors** — iMMERSE **LaunchPad** from
   **https://github.com/martymcmodding/iMMERSE** → green **Code ▸ Download ZIP**. Copy into your
   game's `reshade-shaders\` folder: `Shaders\MartysMods_LAUNCHPAD.fx`, the whole
   `Shaders\MartysMods\` folder, and `Textures\iMMERSE_bluenoise_opt.png`.
   *(Alternative: any provider exposing `texMotionVectors`, e.g.
   [ReshadeMotionEstimation](https://github.com/JakobPCoder/ReshadeMotionEstimation) — see
   [Configuration](#configuration).)*
4. **DLSS 5 neural-rendering add-on** — `renodx-dlss5.addon64` and its model `nvngx_dlssnr.dll`.
   Easiest is the **RHI** installer, which downloads and deploys them for you:
   **https://github.com/RankFTW/RHI/releases** (or get them from the RenoDX Discord). Put both next
   to the game `.exe`. Also drop a **`nvngx_dlss.dll`** there (any DLSS game has one, or use
   [DLSS Swapper](https://github.com/beeradmoore/dlss-swapper)).
5. **Turn it on in-game** — press **Home** for the ReShade overlay, enable **MartysMods_Launchpad**,
   then enable **DLSS 5 Feed** *below it*, and enable neural rendering in the **DLSS 5 Neural
   Rendering** panel. Keep the game's MSAA/SSAA **off**.

Check `dlss5-feed.log` (next to the game `.exe`) for `feature ready … DLAA` and `frame N delivered`.
`dlss5-feed.cfg` is created automatically with working defaults.

> **Do I need the DLSS 5 DX11 *bridge*?** **No.** DLSS5-Feeder does the bridge's job for games that
> have no DLSS. The bridge — **https://github.com/NIGos/dlss5-dx11-bridge/releases** — is only for
> DX11 games that *already* have their own DLSS; don't run both for the same game.

## Install for a 32-bit game (beta)

NGX and the DLSS 5 add-on are both 64-bit-only, so on a 32-bit game DLSS5-Feeder splits in two: a
tiny 32-bit add-on lives in the game and ships frames to a bundled 64-bit helper process, which does
all the actual DLSS/NGX work (details in [The 32-bit path](#the-32-bit-path)).

1. **ReShade with add-on support** — as above, but the installer must detect your game as **32-bit**
   and install the x86 build. (`dxgi.dll` should be ~4.4 MB; check its file properties if unsure.)
2. **DLSS5-Feeder for 32-bit** — from the
   **[latest release](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest)** download
   **`dlss5-feed.addon32`**, **`DLSS5_Feed.fx`** and **`dlss5-feed-host64.exe`**.
   - `dlss5-feed.addon32` → next to the game `.exe`
   - `DLSS5_Feed.fx` → `reshade-shaders\Shaders\`
   - create a **`host64\`** folder next to the game `.exe` and put `dlss5-feed-host64.exe` in it
3. **Fill `host64\` with the 64-bit pieces** — the helper is a self-contained "game" of its own and
   needs its **own** copies: a 64-bit ReShade `dxgi.dll`, `renodx-dlss5.addon64`, `nvngx_dlssnr.dll`
   and `nvngx_dlss.dll`. (Run the ReShade installer once against any 64-bit game to obtain the x64
   `dxgi.dll`, or extract `ReShade64.dll` from the installer and rename it.)
4. **Motion vectors** — LaunchPad into the game's `reshade-shaders\`, exactly as in step 3 of the
   64-bit instructions.
5. **Turn it on in-game** — as above. The first fed frame spawns `host64\dlss5-feed-host64.exe`,
   which opens a window titled **"32-bit DLSS 5 Feeder"**. **Press Home in that window** (not in the
   game) to reach the DLSS 5 Neural Rendering panel and its tuning sliders — the add-on and the game
   never share a ReShade instance, so that window *is* the tuning UI. Set `host_window=0` in
   `dlss5-feed.cfg` once you are happy, to keep it out of the way (closing it also just hides it).

You will have a separate window, that is where you can customize DLSS 5 addon settings:

<img width="1880" height="1058" alt="image" src="https://github.com/user-attachments/assets/57abd732-94d2-401c-a524-6536006f3c86" />

## Install for a DirectX 9 game (beta)

A real D3D9 device cannot work directly: ReShade on D3D9 caps at Shader Model 3, so **no** motion
vector provider (LaunchPad, ReshadeMotionEstimation, qUINT — all SM5) can even compile, and D3D9 has
no shared NT handles or fences for the transport. The fix is to translate D3D9 to D3D11 first with
**[dgVoodoo2](http://dege.freeweb.hu/dgVoodoo2/)**, which turns the game into the ordinary supported
case — SM5 shaders, shareable textures, real fences.

**Confirm you need this first:** run the game once with ReShade installed and look in `ReShade.log`.
If the *runtime* is created after `Redirecting Direct3DCreate9` and you see `IDirect3DDevice9`, it is
a genuine D3D9 game. (If instead you see `D3D11CreateDevice` and `Using feature level b000/b100`, the
game already wraps to D3D11 — skip this section, it is just a normal 32-bit install.)

1. **Download dgVoodoo2** from **http://dege.freeweb.hu/dgVoodoo2/** and unzip it.
2. **Copy three files next to the game's `.exe`** — this is the folder containing the executable,
   which is often *not* the game's root folder (Fable Anniversary's is `Binaries\Win32\`):
   - `MS\x86\D3D9.dll` — from the **`MS`** folder (DirectX), and **`x86`** for a 32-bit game
     (`x64` only if the game is 64-bit)
   - `dgVoodoo.conf`
   - `dgVoodooCpl.exe`
3. **Configure it.** Run `dgVoodooCpl.exe` **from that folder** (it only edits the `dgVoodoo.conf`
   sitting beside it), or edit `dgVoodoo.conf` directly. In the **`[DirectX]`** section:

   | Setting | Value | Why |
   | --- | --- | --- |
   | `DisableAndPassThru` | **`false`** | **The shipped default is `true`**, which makes dgVoodoo forward everything to the real D3D9 and do nothing at all. This is the single most common reason "dgVoodoo doesn't seem to do anything". |
   | `VRAM` | **`1GB`** | The default `256` MB is a *virtual* card size reported to the game and causes **"ran out of video memory"** crashes regardless of your real GPU. Do not use `2GB`: 2048 MB in bytes is `0x80000000`, which overflows a signed 32-bit integer and old engines mishandle it. |
   | `VideoCard` | `internal3D` | dgVoodoo's own virtual card; exposes the most capabilities. |
   | `dgVoodooWatermark` | `true` | Temporarily — it is your proof dgVoodoo is actually running. |

   And in **`[General]`**: `OutputAPI = d3d11_fl11_0` (or higher).
4. **Verify before going further.** Launch the game: the **dgVoodoo watermark must appear**. If it
   does not, dgVoodoo is not loading (wrong folder, wrong architecture, or `DisableAndPassThru` is
   still `true`) and nothing else will work. `ReShade.log` should now show `D3D11CreateDevice`,
   `Using feature level b000`, and `Recreated runtime environment` — a real D3D11 runtime.
5. **Install DLSS5-Feeder normally** — follow
   [Install for a 32-bit game](#install-for-a-32-bit-game-beta) (or the 64-bit steps for a 64-bit
   D3D9 game). ReShade must be installed as **`dxgi.dll`**, never as `d3d9.dll` — dgVoodoo owns that
   filename now and the two would fight.
6. Turn the watermark off once everything works.

## How it works

* `DLSS5_Feed.fx` (companion effect) converts the provider's motion vectors (delta-UV,
  `prev_uv = uv + mv`) into `DLSS5_MV` (RG16F, **pixels**), and copies the raw hardware depth with
  ReShade's orientation fixes into `DLSS5_Depth` (R32F).
* `dlss5-feed.addon64` registers with the ReShade add-on API. After the `DLSS5_Feed` technique
  renders, it takes the backbuffer + those two textures and runs `NGX_D3D12_EVALUATE_DLSS` in DLAA
  mode (render size = output size, no jitter). The DLSS 5 neural-rendering add-on
  (`renodx-dlss5.addon64`) detours that D3D12 evaluate and inserts its neural pass — it cannot tell
  the contract is synthetic.
* On a **D3D11 game** the frame is copied into textures **shared** with a private D3D12 device
  (shared NT handles + a shared fence), and the D3D12 output is blitted back onto the backbuffer.
* On a **D3D12 game** there is no transport at all: NGX runs on the game's own device and queue,
  motion vectors and depth are consumed zero-copy straight from the effect textures, and the feature
  survives alt-tabs and effect reloads untouched (only a real resolution change rebuilds).
* NGX calls are wrapped in SEH, a command list the add-on crashed in is discarded rather than
  submitted, and NGX is reinitialized after repeated failures — a faulting closed-source add-on
  disables the feed instead of taking the game down.

### The 32-bit path

NGX and the DLSS 5 add-on only exist as x64 code, and a 32-bit process cannot load an x64 DLL — so
`dlss5-feed.addon32` does none of the NGX work itself. Instead:

* It creates the four Color/Output/Depth/MV textures as **cross-process shared** D3D11 resources
  (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`) on the game's own device, plus two shared fences.
* It spawns `dlss5-feed-host64.exe` and hands it the texture/fence handles over a named pipe
  (`DuplicateHandle` across the process boundary — the same WDDM sharing the driver already uses,
  just one hop further).
* The host — a genuine 64-bit process — opens those shared resources on **its own D3D12 device**,
  runs the same DLSS DLAA evaluate the 64-bit add-on runs in-process, and signals a fence back.
  No frame data ever crosses into system memory; every copy stays GPU-to-GPU.
* Because the DLSS 5 add-on is itself a ReShade add-on, the host disguises itself as a game to load
  it: a window with a minimal D3D12 swap chain lets its own bundled ReShade (`host64\dxgi.dll`)
  attach and the add-on arm its hooks, exactly as in a real D3D12 title. That window doubles as the
  tuning UI.
* If the host process dies, the pipe breaks, the add-on notices and disables itself — the game keeps
  rendering normally.
* Verified end to end with a deliberate split-screen test (`mode=1`): the host copies only the left
  half of the frame back, so a visibly half-black screen proves the full round trip — game → shared
  texture → host → shared fence → game's backbuffer — actually reaches the display, not just the logs.

### The DirectX 9 path

dgVoodoo2 sits in front as `D3D9.dll` and translates the game's D3D9 calls onto its own D3D11 device.
ReShade (installed as `dxgi.dll`) hooks that D3D11 device rather than the game's D3D9 one, so from
DLSS5-Feeder's point of view it is simply a D3D11 game and the 32-bit path applies unchanged. The
translation is what makes SM5 motion-vector shaders, shared NT-handle textures and fences available
at all — none of which exist on real D3D9.

## Requirements

| Piece | Notes |
| --- | --- |
| D3D11 or D3D12 game, 32- or 64-bit | NGX is 64-bit only, hence the helper process for 32-bit games. D3D9 works through [dgVoodoo2](#install-for-a-directx-9-game-beta). D3D10 and Vulkan are not supported. |
| ReShade 6.8+ **with add-on support** | Generic Depth add-on enabled and picking the scene depth. |
| DLSS 5 neural-rendering add-on (`renodx-dlss5.addon64`) + `nvngx_dlssnr.dll` | from its own author; this project does not include it. |
| `nvngx_dlss.dll` | a DLSS Super Resolution runtime next to the game (the driver's copy is used otherwise). |
| A motion vector provider | iMMERSE **LaunchPad** (proprietary, **not** bundled) or any `texMotionVectors` provider such as **ReshadeMotionEstimation** (CC BY-NC 4.0). Install it yourself. |
| `dlss5-feed.addon64` (or `.addon32` + `host64\`) + `DLSS5_Feed.fx` | this project. |

## Configuration

`dlss5-feed.cfg` is created automatically next to the add-on and re-read while the game runs.

| Key | Default | Meaning |
| --- | --- | --- |
| `enabled` | 1 | 0 disables everything. |
| `mode` | 2 | 0 inert · 1 transport test (no NGX; on 32-bit it copies only the left half, so a split screen proves the round trip) · 2 full DLSS path. |
| `hdr` | -1 | -1 auto (FP16 / R11G11B10 backbuffer = HDR), 0 force SDR, 1 force HDR. |
| `depth_inverted` | -1 | -1 follow `RESHADE_DEPTH_INPUT_IS_REVERSED`, 0/1 force. |
| `flags` | -1 | raw `DLSS.Feature.Create.Flags` override. |
| `reset_every` | 0 | 1 = NGX Reset every frame (no temporal history; diagnostic). |
| `warmup_rebuild` | 180 | re-create the feature once after N delivered frames (works around the DLSS 5 add-on latching STANDBY on its first create). |
| `rebuild` | 0 | change the number to re-create the feature once, by hand. |
| `log_frames` | 3 | first N frames logged in detail. |
| `create_delay` | 60 | frames to hold a feature (re)build after a runtime (re)init — the DLSS 5 add-on arms its NGX hooks asynchronously, and calling in too early can crash. 0 disables. |
| `mv_scale_x/y` | 1.0 | extra motion-vector multiplier. |
| `host_window` | 1 | **32-bit games only.** 1 shows the helper's window (press Home there for the DLSS 5 tuning panel); 0 hides it once you are done tuning. |

In `DLSS5_Feed.fx`'s own UI:

* **Motion vector provider** — live switch between LaunchPad and a `texMotionVectors` provider.
* **MV_SIGN** — if the image doubles or smears while moving, flip a component.
* **DLSS 5 Feed – debug view** technique — shows the vectors/depth being sent (static scene = grey,
  motion = colour).

Whether LaunchPad is *installed* is a compile-time choice, because its headers must be `#include`d
and cannot coexist with `ReShade.fxh`. Set **`DLSS5_MV_SOURCE`** under *Edit ▸ Preprocessor
definitions*: `0` = LaunchPad installed (default; the runtime dropdown then offers both), `1` =
`texMotionVectors` provider only.

## Logs and troubleshooting

| File | Contents |
| --- | --- |
| `dlss5-feed.log` | next to the game exe: resolved effect handles, the session, the contract (`feature ready: WxH DLAA, flags=…`), `frame N delivered`, timing every 600 frames, crash breadcrumbs. |
| `ReShade.log` | which graphics API ReShade attached to, shader compile errors. |
| `host64\dlss5-feed-host.log` | 32-bit games: the helper's own session and per-frame state. |
| `host64\ReShade.log` | 32-bit games: the DLSS 5 add-on's messages (`feature 18 created`, `inline feature 18 evaluation succeeded`). |

Common cases:

* **"unknown technique" for DLSS5_Feed / Launchpad** — the shaders are not in
  `reshade-shaders\Shaders\`, or the runtime is D3D9 and they cannot compile (see
  [the D3D9 section](#install-for-a-directx-9-game-beta)).
* **Nothing happens, no `dlss5-feed.log`** — ReShade's architecture does not match the game's
  (a 64-bit `dxgi.dll` cannot load into a 32-bit game, and vice versa).
* **"ran out of video memory" with dgVoodoo** — raise `VRAM` in `dgVoodoo.conf`; the default 256 MB
  is a virtual limit unrelated to your real GPU.
* **DLSS 5 panel stuck in STANDBY** — the add-on missed the first create; the built-in warm-up
  re-creates the feature a few seconds in, which normally clears it.

## Building

MSVC (v143/v145) + Windows SDK. Dependencies not vendored: the **NGX SDK** (see
[`external/ngx/README.md`](external/ngx/README.md)); the ReShade add-on headers *are* included under
`external/reshade/include` (BSD-3-Clause, Patrick Mours).

| Script | Output | Needs |
| --- | --- | --- |
| `build.bat` | `build\dlss5-feed.addon64` | NGX SDK |
| `build-addon32.bat` | `build\dlss5-feed.addon32` | ReShade headers only |
| `host\build-host.bat` | `host\dlss5-feed-host64.exe` | NGX SDK |
| `spike\build-spike.bat` | the standalone 32↔64-bit shared-resource proof used during development | — |

NGX links against the Release CRT, so the builds use `/MD`.

## Limitations and roadmap

* **DLAA only** — render resolution = output resolution = the game's backbuffer. No upscaling perf
  gain yet; a jittered render-at-lower / output-at-higher upscaling mode is future work.
* Estimated motion vectors → temporal artifacts in fast motion; the UI is processed with the scene
  (a UI mask / pre-UI colour capture is future work).
* Exclusive-fullscreen swapchain churn can make some games reload effects repeatedly; windowed is
  smoother.
* Depends on a closed-source, community-distributed DLSS 5 add-on and the NGX runtime; both can change.
* The **32-bit and D3D9 paths are beta** — see [`PLAN-32BIT.md`](PLAN-32BIT.md) for the full design
  and known risks. Cross-process adds a small amount of scheduling jitter versus the in-process
  64-bit path (not measured as a problem so far).

## Credits

* **D3D11↔D3D12 shared-texture / fence transport** adapted from NIGos'
  [dlss5-dx11-bridge](https://github.com/NIGos/dlss5-dx11-bridge) (MIT) — not re-hosted here.
* **Motion vectors:** Pascal Gilcher's iMMERSE LaunchPad, and Jakob Wapenhensch's
  [ReshadeMotionEstimation](https://github.com/JakobPCoder/ReshadeMotionEstimation) (CC BY-NC 4.0) —
  both consumed at runtime, neither bundled.
* **DLSS 5 neural rendering:** the RenoDX community's `renodx-dlss5` add-on.
* **ReShade** add-on API by Patrick Mours.
* **dgVoodoo2** by Dege — the D3D9 translation layer that makes the DirectX 9 path possible.
* **D3D12 stability findings** independently confirmed by the
  [Pizzawookiee fork](https://github.com/Pizzawookiee/DLSS5-Feeder)'s diagnostics.

## License

MIT — see [LICENSE](LICENSE). This covers only the code in this repository (`src/`, `shaders/`,
`host/`); the dependencies above keep their own licenses.
