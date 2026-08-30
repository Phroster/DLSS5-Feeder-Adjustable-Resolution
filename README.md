# DLSS5-Feeder NR50

NR50 is a focused derivative of
[DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) for the MGS4 Master
Collection PC release. It reduces the cost of the DLAA plus DLSS Neural Rendering
post-chain without lowering the game's own render resolution.

At a 2560x1440 backbuffer the path is:

```text
MGS4 2560x1440
  -> half-resolution color/depth/motion vectors
  -> DLAA 1280x720
  -> RenoDX Neural Rendering Feature 18 at 1280x720
  -> linear upscale to the 2560x1440 backbuffer
```

DLAA and Neural Rendering therefore process 75% fewer pixels. The game, UI, and
backbuffer remain native 1440p.

## Status

- Version 0.1.0 uses a fixed 50% linear scale.
- The final upscale is linear. A selectable scaler/quality control is future work.
- The add-on was validated on the author's MGS4/ReShade/RenoDX setup. Other games and
  versions are not currently supported.
- This is an experimental graphics modification. The installer creates an exact,
  non-destructive rollback before changing anything.

## Prerequisites

Install these separately before NR50:

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
the add-on and shader, and puts LaunchPad before NR50 in the active preset.

In the RenoDX overlay, keep **Enable Upscaling WIP** disabled. NR50 supplies the
reduced Neural Rendering extent itself.

## Verify

After launching the game and reaching a rendered scene, close the game and run:

```powershell
.\Verify-NR50.ps1 -GameDir 'G:\path\to\MGS4'
```

A healthy 1440p result reports a matched 1280x720 Feeder and RenoDX contract, a
successful inline Feature 18 evaluation, and no fallback marker.

## Roll back

Each install creates `_NR50-Backup-yyyyMMdd-HHmmss` inside the game directory. Run
`Restore-NR50.ps1` from the newest backup. It restores files that existed and removes
only NR50 targets that did not exist before installation; the backup itself is kept.

## Build

See [BUILDING.md](BUILDING.md). The source build deliberately requires separately
obtained upstream Feeder/ReShade headers and the NVIDIA DLSS SDK; proprietary files are
not vendored.

## What this project owns

The NR50 delta is the reduced-resolution color/depth/motion-vector preparation,
motion-vector correction, independent render/output/backbuffer extents, matched DLAA
contract used by RenoDX Feature 18, native-backbuffer upscale, lifecycle logging, and
the installer/verifier tooling.

The NGX host, shared D3D11/D3D12 transport, DLSS implementation, RenoDX injection,
ReShade, iMMERSE, and MGS4 belong to their respective authors. See [LICENSE](LICENSE)
and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
