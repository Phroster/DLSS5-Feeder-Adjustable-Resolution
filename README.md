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

The game, UI, and backbuffer remain native 1440p at every slider position. Only the
DLAA and Neural Rendering work textures change. Example positions are:

| Preset | Work resolution at 1440p | Relative pixel work |
| --- | ---: | ---: |
| 100% | 2560x1440 | 100% |
| 75% | 1920x1080 | 56.25% |
| 66% | 1688x950 | 43.5% |
| 50% | 1280x720 | 25% |

## Status

- Version 0.2.0 provides a native ReShade slider from 50% through 100% in one-percent
  steps. Reduced sizes are rounded down to even dimensions for NGX.
- Slider input is debounced: approximately 0.4 seconds after movement stops, only
  `DLSS5_Feed.fx` recompiles and the NGX feature safely rebuilds.
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

In the RenoDX overlay, keep **Enable Upscaling WIP** disabled. The Feeder supplies the
reduced Neural Rendering extent itself.

## Select a resolution

Open ReShade's **Home** tab, expand **DLSS 5 Feed Resolution Scale**, and choose
**DLAA / Neural Rendering resolution**. Move the slider anywhere from 50% to 100%.
The add-on persists the selection in ReShade's configuration, waits until dragging
stops, reloads the companion effect once, and rebuilds the DLSS/NR contract. A value of
50% is the original NR50 behavior.

## Verify

After launching the game and reaching a rendered scene, close the game and run:

```powershell
.\Verify-NR50.ps1 -GameDir 'G:\path\to\MGS4'
```

A healthy result reports the selected percentage and work resolution, a matching
Feeder/RenoDX contract, a successful inline Feature 18 evaluation, and no fallback
marker.

## Roll back

Each install creates `_NR50-Backup-yyyyMMdd-HHmmss` inside the game directory. Run
`Restore-NR50.ps1` from the newest backup. It restores files that existed and removes
only NR50 targets that did not exist before installation; the backup itself is kept.

## Build

See [BUILDING.md](BUILDING.md). The source build deliberately requires separately
obtained upstream Feeder/ReShade headers and the NVIDIA DLSS SDK; proprietary files are
not vendored.

## What this project owns

The project delta is the selectable-resolution color/depth/motion-vector preparation,
motion-vector correction, independent render/output/backbuffer extents, native ReShade
slider control, debounced shader/NGX rebuilding, matched DLAA contract used by RenoDX
Feature 18, native-backbuffer upscale, lifecycle logging, and installer/verifier tooling.

The NGX host, shared D3D11/D3D12 transport, DLSS implementation, RenoDX injection,
ReShade, iMMERSE, and MGS4 belong to their respective authors. See [LICENSE](LICENSE)
and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
