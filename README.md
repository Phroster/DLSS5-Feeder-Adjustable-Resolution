# DLSS5-Feeder Resolution Scale

DLSS5-Feeder Resolution Scale (originally NR50) is a focused derivative of
[DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) for the MGS4 Master
Collection PC release. It reduces the cost of the DLAA plus DLSS Neural Rendering
post-chain without lowering the game's own render resolution.

At a 2560x1440 backbuffer with the slider at 50%, the path is:

```text
MGS4 2560x1440
  -> half-resolution color/depth/motion vectors
  -> DLAA 1280x720
  -> RenoDX Neural Rendering Feature 18 at 1280x720
  -> linear upscale to the 2560x1440 backbuffer
```

The game, UI, and backbuffer remain native 1440p at every slider position. The add-on
now creates its own work textures from full-resolution ReShade guides, so scale changes
do not recompile the effect or reset the ReShade interface. Example dimensions are:

| Scale | Resolution at 1440p | Relative pixel count |
| --- | ---: | ---: |
| 100% | 2560x1440 | 100% |
| 75% | 1920x1080 | 56.25% |
| 66% | 1688x950 | 43.5% |
| 50% | 1280x720 | 25% |

## Status

- Version 0.3.1 provides one shared 50-100% DLSS/DLAA plus Neural Rendering slider.
- Slider input is debounced for approximately 0.4 seconds. The add-on resizes its own
  resources and rebuilds NGX without reloading `DLSS5_Feed.fx` or resetting ReShade's UI.
- Reduced sizes are rounded down to even dimensions for NGX.
- The DLSS/DLAA input, output, depth, motion vectors, and RenoDX Feature 18 contract all
  use the same selected native work resolution.
- The final spatial expansion still uses a linear sampler.
- The add-on was validated on the author's MGS4/ReShade/RenoDX setup. Other games and
  versions are not currently supported.
- This is an experimental graphics modification. The installer creates an exact,
  non-destructive rollback before changing anything.

## Prerequisites

Install these separately before the resolution-scale add-on:

1. ReShade 6.8 or newer with add-on support.
2. iMMERSE LaunchPad plus its sibling `MartysMods` include directory.
3. The RenoDX DLSS Neural Rendering add-on (`renodx-dlss5.addon64`).
4. Authorized `nvngx_dlss.dll` and `nvngx_dlssnr.dll` runtime files.

These prerequisites are not redistributed here. Read
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) before building or sharing a package.

## Install

Download and extract the release, then run PowerShell in that folder:

```powershell
.\Install-NR50.ps1 -GameDir 'G:\path\to\MGS4'
```

If `-GameDir` is omitted, a folder picker opens. The installer checks every
prerequisite, refuses to operate while MGS4 is running, backs up each target, installs
the add-on and shader, and puts LaunchPad before the Feeder in the active preset.

Keep RenoDX **Enable Upscaling (WIP)** disabled.

## Select the resolution

Open ReShade's **Home** tab, expand **DLSS 5 Feed Resolution Scale**, and move
**DLSS resolution scale**. It ranges from 50% to 100% and is persisted. Approximately
0.4 seconds after you stop moving it, the add-on rebuilds only its private textures and NGX feature.
The ReShade effect list, selected panel, and scroll position remain intact.

## Verify

After launching the game and reaching a rendered scene, close the game and run:

```powershell
.\Verify-NR50.ps1 -GameDir 'G:\path\to\MGS4'
```

A healthy result reports the selected percentage and work resolution, a matching
Feeder/RenoDX native contract, a successful inline Feature 18 evaluation, and no
fallback marker.

## Roll back

Each install creates `_NR50-Backup-yyyyMMdd-HHmmss` inside the game directory. Run
`Restore-NR50.ps1` from the newest backup. It restores files that existed and removes
only NR50 targets that did not exist before installation; the backup itself is kept.

## Build

See [BUILDING.md](BUILDING.md). The source build deliberately requires separately
obtained upstream Feeder/ReShade headers and the NVIDIA DLSS SDK; proprietary files are
not vendored.

## What this project owns

The project delta is full-resolution guide capture, add-on-side color/depth/motion-vector
resampling, motion-vector correction, separate work/backbuffer extents,
one native ReShade slider, debounced resource/NGX rebuilding without effect reloads,
the synthetic DLAA/DLSS carrier used by RenoDX Feature 18, native-backbuffer expansion,
lifecycle logging, and installer/verifier tooling.

The NGX host, shared D3D11/D3D12 transport, DLSS implementation, RenoDX injection,
ReShade, iMMERSE, and MGS4 belong to their respective authors. See [LICENSE](LICENSE)
and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
