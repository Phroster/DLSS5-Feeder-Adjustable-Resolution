# DLSS5-Feeder Adjustable Resolution

DLSS5-Feeder Adjustable Resolution adds one live **50–100% work-resolution slider**
to DLSS5-Feeder. It lets users trade image detail against the GPU cost of the injected
DLAA and DLSS Neural Rendering stage without changing the game's own resolution.

That slider is the entire purpose of this fork. It contains no game preset, color
tuning, sharpening preset, or automatic performance policy.

> [!IMPORTANT]
> This is adjustable resolution, not automatic dynamic resolution. It is also not
> conventional DLSS Super Resolution, Frame Generation, Multi Frame Generation, Ray
> Reconstruction, or an override for a game's internal resolution setting.

## What the slider controls

The percentage is applied to both dimensions used by the injected equal-input/output
DLAA and Neural Rendering stage. Its approximate pixel workload is therefore:

```text
relative work pixels ≈ (resolution scale / 100)²
```

For a 2560×1440 backbuffer:

| Slider | DLAA/NR work resolution | Pixels relative to 100% |
| ---: | ---: | ---: |
| 100% | 2560×1440 | 100% |
| 90% | 2304×1296 | 81% |
| 80% | 2048×1152 | 64% |
| 75% | 1920×1080 | 56.25% |
| 70% | 1792×1008 | 49% |
| 66% | 1688×950 | 43.5% |
| 60% | 1536×864 | 36% |
| 50% | 1280×720 | 25% |

Below 100%, each work axis is calculated as
`floor(native axis × percentage / 100)` and rounded down to an even number. At 100%,
the native dimensions are preserved exactly.

Lower values reduce the work textures, transfer volume, and pixel processing used by
the injected DLAA/Neural Rendering path. This can improve performance if that path is
the GPU bottleneck. It does not imply the same percentage reduction in total GPU usage:
the game still renders normally, and full-size guide capture, motion-vector generation,
cross-API synchronization, final expansion, later ReShade effects, CPU work, and other
fixed costs remain.

## How it works

DLSS5-Feeder creates a synthetic DLSS contract in games that do not make suitable DLSS
calls themselves. This fork changes only the resolution used by that injected stage:

```text
Game renders its completed frame at the selected native resolution
        ↓
ReShade captures color and depth; LaunchPad supplies optical-flow motion vectors
        ↓
The add-on copies them at 100%, or resamples them to the selected work resolution
        ↓
NGX runs an equal-input/output DLAA evaluation at that work resolution
        ↓
The DLSS Neural Rendering add-on intercepts the evaluation at the same resolution
        ↓
The result is linearly expanded back into the native-size backbuffer
        ↓
Any later ReShade effects run normally
```

Because the NGX input and target dimensions are equal, this is a **DLAA contract at a
selectable work resolution**. It does not select NVIDIA's standard DLSS Quality,
Balanced, or Performance Super Resolution modes.

The Feeder processes the completed frame, so HUD and UI elements pass through the same
reduced stage. Low values can soften UI text as well as the 3D image.

## Live behavior

Open ReShade's **Home** tab, expand **DLSS 5 Feed Work Resolution**, and change
**DLAA + Neural Rendering work scale**.

- Range: 50–100%, in one-percent steps.
- A fresh configuration begins at 50%.
- The add-on waits about 400 ms after the final slider movement before applying it.
- It rebuilds only its private textures and NGX feature; ReShade is not reloaded.
- The overlay's current panel, selection, and scroll position remain intact.
- A brief hitch can occur after an applied change while GPU work is drained and the
  NGX feature is recreated. The default warm-up rebuild may cause one additional hitch.
- The chosen value is stored as `[DLSS5Feeder] ResolutionPercent` in `ReShade.ini`.

The persisted setting is restored during runtime initialization. A later live uniform
or preset change may become the new stored value after the debounce, so verify the live
control after changing ReShade presets.

## Compatibility

This branch currently targets a **64-bit Direct3D 11 game** with:

- [ReShade 6.8+](https://reshade.me/) with add-on support;
- a working scene depth buffer;
- [iMMERSE LaunchPad](https://github.com/martymcmodding/iMMERSE), including
  `MartysMods_LAUNCHPAD.fx` and its adjacent `MartysMods` directory;
- the RenoDX DLSS Neural Rendering add-on and `nvngx_dlssnr.dll`, obtained from its
  original source or a tool such as [RHI](https://github.com/RankFTW/RHI/releases);
- an authorized local `nvngx_dlss.dll`; and
- an NVIDIA RTX GPU/driver capable of creating the required NGX feature.

The installer validates the expected files and shader layout. It cannot establish the
authenticity, license status, or compatibility of third-party files. Those files are not
redistributed by this repository.

The generic path still depends on the host game's accessible depth and the quality of
LaunchPad's estimated optical flow. Disocclusion, particles, moving geometry, and HUD
elements may artifact. HDR and APIs other than the current 64-bit D3D11 path are not
claimed as supported by this fork.

Coexistence with NVIDIA Smooth Motion and OptiScaler is not validated here. If visual
corruption, stutter, or silent Neural Rendering loss occurs, disable them and verify the
Feeder by itself first. Keep RenoDX **Enable Upscaling (WIP)** disabled because this fork
owns the final expansion.

## Automatic installation

The generic installer operates on the directory you select; it does not contain a game
name or engine-specific patch.

Close the game, extract the package, and run:

```powershell
.\Install-Resolution-Control.ps1 -GameDir 'D:\Games\ExampleGame'
```

If `-GameDir` is omitted, a folder picker opens. The installer:

1. confirms the directory contains ReShade and the required local DLSS/RenoDX files;
2. locates exactly one compatible LaunchPad shader and include directory;
3. refuses to continue if an executable inside the selected directory is running;
4. creates a scoped backup under
   `_DLSS5-Feeder-Resolution-Control-Backup-yyyyMMdd-HHmmss`;
5. installs `dlss5-feed.addon64` and `DLSS5_Feed.fx`; and
6. places LaunchPad and DLSS5 Feed at the start of the active ReShade technique order.

The scripts and add-on are not code-signed. Inspect downloaded files before running
them. If Windows blocks the installer, unblock only that reviewed file:

```powershell
Unblock-File -LiteralPath .\Install-Resolution-Control.ps1
```

Do not disable antivirus protection or weaken the machine-wide PowerShell execution
policy.

## Manual installation

1. Close the game and back up the existing add-on, active ReShade preset, and
   `ReShade.ini`.
2. Copy `bin\dlss5-feed.addon64` beside the game's executable.
3. Copy `shaders\DLSS5_Feed.fx` beside `MartysMods_LAUNCHPAD.fx`. Its relative includes
   require the adjacent `MartysMods` directory.
4. Enable and order the techniques as:

   ```text
   MartysMods_Launchpad → DLSS5_Feed → other ReShade effects
   ```

5. Confirm that DLSS Neural Rendering is active and RenoDX WIP upscaling is disabled.

Manual installation does not create the automatic restore script.

## Verification

Launch the game, reach a normally rendered scene for several seconds, close the game,
and run:

```powershell
.\Verify-Resolution-Control.ps1 -GameDir 'D:\Games\ExampleGame'
```

The verifier checks:

- add-on and companion-shader presence;
- the work resolution reported by the Feeder;
- a matching Neural Rendering Feature 18 contract;
- successful inline Feature 18 evaluation; and
- known fallback/failure markers in the current logs.

It does not benchmark frame rate, total GPU utilization, visual quality, or third-party
file authenticity.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Image or UI is too soft | Raise the work scale. The completed frame passes through the selected work resolution. |
| Total GPU usage does not fall | The game or another effect may be the bottleneck. Compare GPU frame time or power in the same scene and at the same frame cap. |
| Static images look sharp but movement smears | Confirm LaunchPad is enabled above DLSS5 Feed and optical flow is working. Keep `MV_SIGN=1,1` and `MV_SCALE=1` unless diagnosing a demonstrated vector error. |
| The slider returns to another value | Select the ReShade preset first, set the live slider, stop moving it for 400 ms, and verify that the chosen value remains. |
| Neural Rendering remains in STANDBY briefly | The default configuration performs one warm-up rebuild. If it never activates, inspect both logs and run the verifier. |
| Visual corruption or Neural Rendering silently stops | Disable other frame/upscaler injection paths, restart, and test the Feeder alone. |
| `DLSS5_Feed.fx` fails to compile | Confirm LaunchPad and its matching `MartysMods` include directory are installed together and the Feeder shader is beside them. |
| Black output, repeated rebuilds, or fallback | Restore the backup, then inspect `dlss5-feed.log` and `ReShade.log`. |

## Rollback

With the game closed, run the restore script from the newest backup:

```powershell
& 'D:\Games\ExampleGame\_DLSS5-Feeder-Resolution-Control-Backup-yyyyMMdd-HHmmss\Restore-Resolution-Control.ps1'
```

It restores files changed by the installer and removes only Feeder targets that the
installer created. The backup directory remains. Runtime state is outside that scoped
rollback: `ReShade.ini`, `dlss5-feed.cfg`, and `dlss5-feed.log` are not removed. Back up
`ReShade.ini` before manually deleting the exact `[DLSS5Feeder]` section if a completely
clean configuration reset is required.

## Building

See [BUILDING.md](BUILDING.md). Building requires separately obtained ReShade headers
and the NVIDIA DLSS SDK. Proprietary SDK files and runtimes are not vendored.

## What this fork adds

Relative to the recorded upstream DLSS5-Feeder base, this repository adds only the
adjustable work-resolution path and the tooling needed to install and verify it:

- one persisted 50–100% shared work-resolution slider;
- full-resolution guide capture followed by add-on-side copy/resampling;
- exact motion-vector correction for the selected work extent;
- separate work and native-backbuffer dimensions;
- a 400 ms debounce and private resource/NGX rebuild without ReShade reload;
- native-backbuffer expansion after the shared DLAA/Neural Rendering evaluation;
- resolution-contract and lifecycle logging; and
- bounded installation, scoped rollback, verification, build, and packaging scripts.

## Credits and license

This project is derived from
[DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) by Jean-Laurent ROUZIES,
which contains work derived from
[dlss5-dx11-bridge](https://github.com/NIGos/dlss5-dx11-bridge) by NIGos.

Project-specific source is distributed under the MIT License. Required third-party
components retain their own licenses and are not redistributed. See [LICENSE](LICENSE)
and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

This project is not affiliated with or endorsed by NVIDIA, RenoDX, ReShade, Marty's
Mods, or the upstream authors. Use it at your own risk and keep the generated backup
until the installation is fully tested.
