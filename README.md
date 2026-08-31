# DLSS5-Feeder Work Resolution Slider for MGS4

This project adds a live **50–100% work-resolution slider** to the injected DLAA
and DLSS Neural Rendering post-process used by DLSS5-Feeder in the PC release of
Metal Gear Solid 4: Guns of the Patriots Master Collection.

Its purpose is simple: lower the slider when the Neural Rendering stage is too
expensive for your GPU, or raise it when you want more image detail. MGS4 continues
to render at the resolution selected in the game, while the costly injected
DLAA/Neural Rendering stage operates on a separate adjustable resolution.

> [!IMPORTANT]
> This is an experimental, MGS4-specific Direct3D 11 graphics modification. It is
> not conventional DLSS Super Resolution, Frame Generation, Multi Frame Generation,
> Ray Reconstruction, or a replacement for the game's resolution setting.

## What the slider controls

The percentage is applied to both image dimensions used by the injected DLAA and
Neural Rendering stage. Pixel count therefore changes approximately with the square
of the slider value:

```text
relative work pixels ≈ (resolution scale / 100)²
```

At a native 2560×1440 backbuffer:

| Slider | DLAA/NR work resolution | Pixels relative to 100% |
| ---: | ---: | ---: |
| 100% | 2560×1440 | 100% |
| 90% | 2304×1296 | 81% |
| 80% | 2048×1152 | 64% |
| 75% | 1920×1080 | 56.25% |
| **70%** | **1792×1008** | **49%** |
| 66% | 1688×950 | 43.5% |
| 60% | 1536×864 | 36% |
| 50% | 1280×720 | 25% |

Lower values can reduce GPU time, bandwidth, and private texture memory used by the
injected DLAA/Neural Rendering path. They can improve frame rate when that path is the
GPU bottleneck, or reduce utilization, power, and temperature when the game is capped.

The table does **not** describe total GPU savings. MGS4's main rendering, full-size
guide capture, LaunchPad, cross-API synchronization, final expansion, later ReShade
effects, and other fixed overhead are not reduced by the same percentage. The scaled
work textures and transfer volume do become smaller. An uncapped game may still remain
near 100% GPU utilization while producing more frames.

## How it works

MGS4 does not make native DLSS calls. DLSS5-Feeder creates the DLSS contract that the
RenoDX DLSS Neural Rendering add-on needs to observe:

```text
MGS4 renders the completed frame at the selected native resolution
        ↓
ReShade captures color, raw depth, and LaunchPad motion vectors
        ↓
The add-on copies them at 100%, or resamples them below 100%
        ↓
NGX runs an equal-input/output DLAA evaluation at that resolution
        ↓
RenoDX inserts DLSS Neural Rendering Feature 18 at the same resolution
        ↓
The add-on linearly expands the result into the native-size backbuffer
        ↓
Optional post-processing such as NR Detail Forge runs at native output size
```

Because NGX input and output are equal here, the carrier is **DLAA at a selectable
work resolution**. The final step is a spatial expansion to the native backbuffer;
this fork is not selecting NVIDIA's normal DLSS Quality, Balanced, or Performance
Super Resolution modes.

The game and backbuffer dimensions remain native. However, the Feeder processes the
completed frame, so the HUD and UI are included. Very low settings can therefore
soften UI text as well as the 3D image.

## Choosing a scale

- **100%** — maximum detail and maximum cost for the injected stage.
- **80–90%** — quality-focused compromise.
- **70–75%** — balanced starting range. The author's verified 1440p baseline is 70%.
- **60–66%** — stronger performance bias with more visible softness.
- **50%** — maximum available reduction; the DLAA/NR stage processes one quarter of
  the 100% pixel count.

Image quality and performance depend on the scene, GPU, display resolution, and the
rest of the ReShade stack. Use measurements from your own system rather than treating
these values as fixed NVIDIA quality presets.

## Live slider behavior

Open ReShade's **Home** tab, expand **DLSS 5 Feed Work Resolution**, and change
**DLAA + Neural Rendering work scale**.

- Range: 50–100%, in one-percent steps.
- Below 100%, each axis is `floor(native axis × scale / 100)` and then rounded down
  to an even number. At 100%, the native dimensions are preserved exactly.
- The add-on waits about 400 ms after the last movement before applying the value.
- Only its private textures and NGX feature are rebuilt; ReShade is not reloaded.
- The open ReShade panel, selection, and scroll position remain intact.
- A brief hitch can occur after each applied scale change while GPU work is drained
  and the NGX feature is recreated. The default warm-up rebuild may cause one more.
- The selected value is persisted as `[DLSS5Feeder] ResolutionPercent` in
  `ReShade.ini` and restored on the next run.

A fresh configuration starts at 50%. The persisted add-on setting is restored during
runtime initialization; a later live uniform or preset change may then become the new
persisted value after the 400 ms debounce. The optional visual preset deliberately
omits this setting. Select and verify the desired scale after changing presets.

## Tested scope

The supported target is:

- Metal Gear Solid 4 Master Collection PC release
- The 64-bit Direct3D 11 game path
- ReShade 6.8 or newer with add-on support
- iMMERSE LaunchPad with its adjacent `MartysMods` include directory
- RenoDX DLSS Neural Rendering add-on and authorized NVIDIA NGX runtimes
- An NVIDIA RTX GPU and driver capable of creating the required NGX feature

The verified author configuration is Windows 11, an RTX 4080, ReShade 6.8.0, SDR,
2560×1440 output, and a 70% work scale:

```text
1792×1008 shared DLSS/DLAA/NR work resolution → 2560×1440 backbuffer
Feature 18 inline evaluation succeeded; no fallback marker
```

Other games, renderers, MGS4 builds, HDR paths, and GPU generations have not been
validated and are not currently supported.

## Known conflicts and limitations

- Keep RenoDX **Enable Upscaling (WIP)** disabled. This fork owns the final expansion.
- Coexistence with NVIDIA Smooth Motion and OptiScaler is not validated for this fork.
  If corruption, stutter, or silent Neural Rendering loss occurs, disable them first
  and retest the Feeder alone.
- Do not use this experimental add-on in competitive or anti-cheat-protected games.
- The whole completed frame is processed, including HUD/UI.
- LaunchPad supplies estimated optical flow rather than game-native motion vectors.
  Disocclusion, particles, moving geometry, and fine UI elements can still artifact.
- The final native-size expansion is linear. Lower work scales inevitably discard
  detail; the optional sharpener improves presentation but cannot recreate missing
  source information.
- The distributed add-on and PowerShell scripts are not code-signed. Source, build
  instructions, and SHA-256 output are provided so users can inspect what they run.

## Requirements

Install these separately before this project:

1. [ReShade 6.8+](https://reshade.me/) with add-on support, installed as `dxgi.dll`
   beside `mgs4.exe`, with the scene depth buffer selected and working.
2. [iMMERSE](https://github.com/martymcmodding/iMMERSE), including
   `MartysMods_LAUNCHPAD.fx` and its sibling `MartysMods` directory.
3. The RenoDX DLSS Neural Rendering add-on, normally installed through
   [RHI](https://github.com/RankFTW/RHI/releases) or its original distribution source:
   `renodx-dlss5.addon64` and `nvngx_dlssnr.dll`.
4. An authorized `nvngx_dlss.dll` runtime beside the game. The automatic installer
   deliberately requires this explicit local file.

These third-party components are deliberately not redistributed. The installer checks
that the required files and layout exist; it cannot establish their authenticity,
license status, or compatibility with every version. Read
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) before building or redistributing a
package.

## Automatic installation

Download and extract the [latest release](https://github.com/Phroster/DLSS5-Feeder-NR50/releases/latest),
close MGS4, and run PowerShell in the extracted folder:

> [!NOTE]
> The latest published release is still the v0.3.1 core slider. The rewritten
> documentation, NR Detail Forge 2 preset, PowerShell 5.1 fix, and hardened packager
> on `main` are currently unreleased and require a new versioned release before this
> repository is made public.

```powershell
.\Install-NR50.ps1 -GameDir 'G:\path\to\MGS4'
```

If `-GameDir` is omitted, a folder picker opens. The installer:

1. confirms that `mgs4.exe` and the expected prerequisite files are present;
2. locates exactly one iMMERSE LaunchPad installation;
3. refuses to continue while MGS4 is running;
4. creates a scoped rollback under `_NR50-Backup-yyyyMMdd-HHmmss`;
5. installs the add-on and companion shader; and
6. places LaunchPad and DLSS5 Feed at the start of the active technique order.

If Windows marks the downloaded script as blocked, inspect it first and unblock only
that file:

```powershell
Unblock-File -LiteralPath .\Install-NR50.ps1
```

Do not disable antivirus protection or weaken the machine-wide PowerShell execution
policy to install this project.

## Manual installation

Users who do not want to run the installer can install the core manually:

1. Close MGS4 and back up the game directory's existing add-on, shader, active preset,
   and `ReShade.ini`.
2. Copy `bin\dlss5-feed.addon64` beside `mgs4.exe`.
3. Copy `shaders\DLSS5_Feed.fx` beside `MartysMods_LAUNCHPAD.fx`. The adjacent
   `MartysMods` directory is required by its relative includes.
4. In ReShade, enable and order the techniques as:

   ```text
   MartysMods_Launchpad → DLSS5_Feed → optional post-processing
   ```

5. Confirm that RenoDX DLSS Neural Rendering is active and its WIP upscaling option is
   disabled.

Manual installation does not create the automatic restore script.

## Verification

Launch the game, reach a normally rendered scene for several seconds, close the game,
and run:

```powershell
.\Verify-NR50.ps1 -GameDir 'G:\path\to\MGS4'
```

The verifier checks:

- add-on and companion-shader presence;
- the work resolution reported by the Feeder;
- a matching RenoDX Feature 18 contract;
- successful inline Feature 18 evaluation; and
- known fallback/failure markers in the current logs.

It does not benchmark frame rate, total GPU utilization, visual quality, dependency
authenticity, or every possible driver/add-on combination.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| The image is too soft or HUD text is blurry | Raise the work scale. The completed frame, including UI, passes through the reduced DLAA/NR stage. |
| GPU usage does not fall | The game or another effect may be the bottleneck. Compare GPU frame time or power at the same scene and frame cap; the slider does not control total utilization directly. |
| Static images look sharp but movement smears | Confirm LaunchPad is enabled above DLSS5 Feed and that its optical flow is working. Leave `MV_SIGN=1,1` and `MV_SCALE=1` unless diagnosing a proven vector problem. |
| The slider returns to another value | A persisted value is restored at runtime initialization, and a later preset load may emit another value. Set the live slider after selecting the preset, stop moving it for 400 ms, and verify it remains selected. |
| Neural Rendering remains in STANDBY briefly | The default configuration performs one warm-up feature rebuild after the pipeline settles. If it never activates, inspect both logs and run the verifier. |
| Visual corruption or Neural Rendering silently stops | Test the Feeder alone: disable NVIDIA Smooth Motion and OptiScaler, restart the game, and verify the contract again. |
| `DLSS5_Feed.fx` fails to compile | Confirm LaunchPad and the matching `MartysMods` include directory are installed together, and that the Feeder shader is beside them. |
| Black output, repeated rebuilds, or fallback | Restore the backup, then inspect `dlss5-feed.log` and `ReShade.log`. Do not repeatedly force a rejected configuration. |

## Rollback

Every automatic installation creates a timestamped backup inside the selected game
directory. With MGS4 closed, run the restore script from the newest backup:

```powershell
& 'G:\path\to\MGS4\_NR50-Backup-yyyyMMdd-HHmmss\Restore-NR50.ps1'
```

It restores files the installer changed and removes only Feeder targets the installer
created. The backup directory itself is retained. Runtime state is outside that scoped
rollback: `ReShade.ini`, `dlss5-feed.cfg`, and `dlss5-feed.log` are not removed. Back up
`ReShade.ini` before manually deleting the exact `[DLSS5Feeder]` section if a completely
clean configuration reset is required.

## Optional MGS4 visual preset

The slider does not require any extra color or sharpening effects. An optional preset
is included for users who want the author's tested 1440p presentation stack:

```text
LaunchPad → DLSS5 Feed → Colourfulness → Levels → NR Detail Forge 2
```

`NRDetailForge.fx` is this project's original, native-output, single-pass sharpener. It
uses directional 17-tap reconstruction, separate fine/surface/microcontrast bands,
noise rejection, chroma preservation, and adaptive anti-ringing protection. It replaces
the older Clarity + RCAS stack instead of adding another sharpener on top.

Because it runs at native output size, NR Detail Forge has its own fixed GPU cost and
can offset part of the savings from a lower work scale. Disable it when benchmarking
the core slider, then re-enable it for visual evaluation.

The visual preset is optional and installed manually. See
[VISUAL_TUNING.md](VISUAL_TUNING.md) for exact files, technique order, controls, and
rollback instructions. It recommends 70%, but it does not override the persisted work
scale.

## Building

See [BUILDING.md](BUILDING.md). The build requires separately obtained upstream
DLSS5-Feeder/ReShade headers and the NVIDIA DLSS SDK. Proprietary SDK files and runtime
DLLs are not vendored.

## What this fork adds

Relative to the recorded upstream DLSS5-Feeder base, this repository adds:

- one persisted 50–100% shared work-resolution slider;
- full-resolution guide capture followed by add-on-side color/depth/motion
  copy-or-resampling;
- exact motion-vector correction for the reduced work extent;
- separate work and native-backbuffer dimensions;
- a 400 ms debounce and private resource/NGX rebuild without ReShade reload;
- native-backbuffer expansion after the shared DLAA/Neural Rendering evaluation;
- lifecycle and resolution-contract logging;
- bounded installation, scoped rollback, verification, build, and packaging tools; and
- the optional original NR Detail Forge 2 post-NR shader and MGS4 presentation preset.

The NGX host, shared D3D11/D3D12 transport foundations, DLSS implementation, RenoDX
injection, ReShade, iMMERSE, and MGS4 belong to their respective authors.

## Credits and license

This project is derived from
[DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) by Jean-Laurent ROUZIES,
which includes work derived from
[dlss5-dx11-bridge](https://github.com/NIGos/dlss5-dx11-bridge) by NIGos.

The project-specific source and shaders are distributed under the MIT License. Required
third-party components retain their own licenses and are not covered or redistributed
by this repository. See [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

This project is not affiliated with or endorsed by NVIDIA, RenoDX, ReShade, Marty's
Mods, Konami, or the upstream authors. Use it at your own risk and keep the generated
rollback until the installation has been fully tested.
