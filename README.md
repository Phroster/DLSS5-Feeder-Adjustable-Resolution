# DLSS5-Feeder

**DLSS 5 neural rendering in D3D11 and D3D12 games that ship without any DLSS — including 32-bit ones.**

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

It is not game-specific: any D3D11 or D3D12 game with a working ReShade depth buffer and LaunchPad
motion vectors should work — 64-bit directly, 32-bit via a bundled 64-bit helper process (below).

**Beta (v0.3.0):** 32-bit support is new and proven on one title so far —
**Splinter Cell: Blacklist** (32-bit D3D11): full DLAA + neural rendering running at a locked 60 fps,
confirmed by a deliberate split-screen test (see "How the 32-bit path works"). Expect rough edges
outside that one game; report what you find.

## Install (5 steps)

You need a **DirectX 11 or DirectX 12** game, 32-bit or 64-bit (DX9/DX10/Vulkan won't work). The
steps below are for a **64-bit** game; for a 32-bit game see
["Install for a 32-bit game"](#install-for-a-32-bit-game-beta) instead.

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

## Install for a 32-bit game (beta)

NGX and the DLSS 5 add-on are both 64-bit-only, so on a 32-bit game DLSS5-Feeder splits in two: a
tiny 32-bit add-on lives in the game and ships frames to a bundled 64-bit helper process, which does
all the actual DLSS/NGX work (details in ["How the 32-bit path works"](#how-the-32-bit-path-works)).

1. **ReShade with add-on support** — same as step 1 above, but the installer must detect your game
   as **32-bit** and install the x86 build. Confirm `dxgi.dll` next to the game `.exe` is the 32-bit
   one (Explorer's file properties, or `ReShade32.dll`'s size, ~4.4 MB, is a good sign).
2. **DLSS5-Feeder for 32-bit** — from the **[latest release](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest)**
   download **`dlss5-feed.addon32`**, **`DLSS5_Feed.fx`**, and **`dlss5-feed-host64.exe`**.
   Put `dlss5-feed.addon32` next to the game `.exe`; `DLSS5_Feed.fx` into `reshade-shaders/Shaders/`
   as before. Create a **`host64`** folder next to the game `.exe` and put `dlss5-feed-host64.exe`
   in it, alongside its **own** 64-bit `dxgi.dll` (from the ReShade installer's 64-bit build),
   `renodx-dlss5.addon64`, `nvngx_dlssnr.dll` and `nvngx_dlss.dll` (steps 3–4 below, x64 versions).
   The helper is a self-contained "game" of its own — it needs its own copy of these.
3. **LaunchPad** and **4. the DLSS 5 neural-rendering add-on** — same as steps 3–4 above; the
   `renodx-dlss5.addon64` + `nvngx_*.dll` files go in `host64\`, not next to the 32-bit game exe.
4. **Turn it on in-game** — same as step 5 above. The first fed frame spawns
   `host64\dlss5-feed-host64.exe`, which opens a small window titled *"DLSS 5 Feed host"*. **Press
   Home in that window** (not the game) to reach the DLSS 5 Neural Rendering panel and its tuning
   sliders — this is the 32-bit equivalent of enabling/tuning it in the game's own overlay, since the
   add-on and the game never share a ReShade instance. Set `host_window=0` in `dlss5-feed.cfg` once
   you're happy with the settings, to keep the window out of the way (closing it also just hides it).

Logs: `dlss5-feed.log` next to the game exe (the 32-bit side) and `host64\dlss5-feed-host.log` +
`host64\ReShade.log` (the 64-bit side, where the DLSS 5 add-on's own messages appear).

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

### How the 32-bit path works

NGX and the DLSS 5 add-on only exist as x64 code, and a 32-bit process cannot load an x64 DLL — so
`dlss5-feed.addon32` does none of the NGX work itself. Instead:

* It creates the four Color/Output/Depth/MV textures as **cross-process shared** D3D11 resources
  (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`) on the game's own device, and two shared fences.
* It spawns `dlss5-feed-host64.exe` (from `host64\`) and hands it the texture/fence handles over a
  named pipe (`DuplicateHandle` across the process boundary — the same WDDM sharing mechanism
  ReShade and the game's own driver already use, just one hop further).
* The host — a genuine 64-bit process — opens those shared resources on **its own D3D12 device**,
  runs the same DLSS DLAA evaluate the 64-bit add-on runs in-process, and signals a fence back.
  No frame data ever crosses into system memory; every copy stays GPU-to-GPU.
* Because the DLSS 5 add-on is itself a ReShade add-on, the host disguises itself as a game to load
  it: a hidden window with a minimal D3D12 swap chain lets its own bundled ReShade (`host64\dxgi.dll`)
  attach and the DLSS 5 add-on arm its hooks, exactly as it would in a real D3D12 title.
* If the host process ever dies, the pipe breaks, the 32-bit add-on notices and disables itself —
  the game keeps rendering normally.
* Verified end-to-end with a deliberate split-screen test (`mode=1`): the host copies only the left
  half of the frame back, so a visibly half-black screen proves the full round trip — game → shared
  texture → host → shared fence → game's backbuffer — actually reaches the display, not just the logs.

### Requirements

| Piece | Notes |
| --- | --- |
| 64-bit D3D11 or D3D12 game | NGX is 64-bit only. DX9 / DX10 / Vulkan / 32-bit not supported. |
| ReShade 6.8+ **with add-on support** (`dxgi.dll`) | Generic Depth add-on enabled and picking the scene depth. |
| DLSS 5 neural-rendering add-on (`renodx-dlss5.addon64`) + `nvngx_dlssnr.dll` | from its own author; this project does not include it. |
| `nvngx_dlss.dll` | a DLSS Super Resolution runtime next to the game (the driver's copy is used otherwise). |
| iMMERSE **LaunchPad** (`MartysMods_LAUNCHPAD.fx` + `MartysMods/*.fxh` + `Textures/iMMERSE_bluenoise_opt.png`) | from https://github.com/martymcmodding/iMMERSE — install it yourself; it is proprietary and is **not** bundled here. |
| `dlss5-feed.addon64` + `DLSS5_Feed.fx` | this project. |

**32-bit games additionally need:** `dlss5-feed.addon32` (instead of the 64-bit add-on) next to the
game, plus a `host64\` folder holding `dlss5-feed-host64.exe` and its **own** 64-bit copies of
`dxgi.dll` (ReShade), `renodx-dlss5.addon64` and both `nvngx_*.dll` files — everything the 64-bit
path needs, just relocated into the helper's folder since it runs as its own process.

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
| `host_window` | 1 | **32-bit games only.** 1 shows the helper's window (press Home there for the DLSS 5 tuning panel); 0 keeps it hidden once you're done tuning. |

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

The 32-bit pieces build separately: `build-addon32.bat` (needs only the ReShade headers, no NGX —
outputs `build\dlss5-feed.addon32`) and `host\build-host.bat` (needs the NGX SDK like the 64-bit
add-on — outputs `host\dlss5-feed-host64.exe`). `spike\build-spike.bat` builds the standalone
32-bit/64-bit shared-resource proof used during development; not needed to use the project.

### Limitations & roadmap

* **DLAA only** — render resolution = output resolution = the game's backbuffer. No upscaling perf
  gain yet; a jittered render-at-lower / output-at-higher upscaling mode is future work.
* Estimated motion vectors → temporal artifacts in fast motion; the UI is processed with the scene
  (a UI mask / pre-UI colour capture is future work).
* Exclusive-fullscreen swapchain churn can make some games reload effects repeatedly; windowed is
  smoother.
* Depends on a closed-source, community-distributed DLSS 5 add-on and the NGX runtime; both can change.
* **32-bit path is beta**, proven on one title (Splinter Cell: Blacklist) — see
  [`PLAN-32BIT.md`](PLAN-32BIT.md) for the full design and known risks. Cross-process adds a small
  amount of scheduling jitter versus the in-process 64-bit path (not measured as a problem so far).
  32-bit **D3D9** games (e.g. BioShock Remastered) are explicitly out of scope for now — no shared
  fences, more restricted shared-surface formats; would need a different transport.

## Credits

* **D3D11↔D3D12 shared-texture / fence transport** adapted from NIGos'
  [dlss5-dx11-bridge](https://github.com/NIGos/dlss5-dx11-bridge) (MIT) — not re-hosted here.
* **Motion vectors:** Pascal Gilcher's iMMERSE LaunchPad (consumed at runtime, not bundled).
* **DLSS 5 neural rendering:** the RenoDX community's `renodx-dlss5` add-on.
* **ReShade** add-on API by Patrick Mours.
* **D3D12 stability findings** independently confirmed by the
  [Pizzawookiee fork](https://github.com/Pizzawookiee/DLSS5-Feeder)'s diagnostics.

## License

MIT — see [LICENSE](LICENSE). This covers only the code in this repository (`src/`, `shaders/`,
`host/`); the dependencies above keep their own licenses.
