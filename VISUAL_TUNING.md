# Optional MGS4 visual preset

The resolution slider works independently of every optional visual effect. This preset
is provided for users who want the author's tested 2560×1440 presentation stack:

```text
LaunchPad → DLSS5 Feed → Colourfulness → Levels → NR Detail Forge 2
```

The older Clarity + RCAS combination has been removed. `NRDetailForge.fx` is one
purpose-built post-Neural-Rendering pass, so it can recover fine and mid-frequency
definition without stacking several unrelated sharpeners.

## What NR Detail Forge 2 does

NR Detail Forge runs after the Feeder has returned its result to the native-size
backbuffer. It does not change the DLSS/DLAA/Neural Rendering work resolution.

The shader uses:

- a 17-tap isotropic neighborhood;
- directional reconstruction that follows coherent edges;
- separate fine-detail, surface-detail, and low-contrast recovery bands;
- line continuity and reconstruction-noise rejection;
- luminance-only sharpening with chroma preservation; and
- adaptive soft limiting plus a local anti-ringing envelope.

It is a single spatial pass with no depth, motion-vector, temporal-history, or extra
shader-pack include dependency beyond ReShade's standard `ReShade.fxh`. It can improve
the presentation of a reduced work scale, but it cannot recreate source information
that was discarded before it ran.

## Required files

The current repository contains the project-owned pieces:

```text
shaders\NRDetailForge.fx
presets\MGS4-VisualTune.ini
```

The preset also references third-party effects which are not redistributed here:

- `Colourfulness.fx` from the standard
  [ReShade shader collection](https://github.com/crosire/reshade-shaders)
- `Levels.fx` from [SweetFX](https://github.com/CeeJayDK/SweetFX)
- iMMERSE `MartysMods_LAUNCHPAD.fx` and its dependencies

Install those from their own sources and under their own licenses.

## Install the optional preset

1. Close MGS4.
2. Back up the active ReShade preset.
3. Copy `shaders\NRDetailForge.fx` into a directory covered by ReShade's effect
   search path. Placing it beside `DLSS5_Feed.fx` is the simplest option.
4. Copy `presets\MGS4-VisualTune.ini` into the MGS4 directory.
5. Launch MGS4, open ReShade's **Home** tab, and select
   `MGS4-VisualTune.ini` from the preset selector.
6. Confirm this active order:

   ```text
   MartysMods_Launchpad
   DLSS5_Feed
   Colourfulness
   Levels
   NRDetailForge
   ```

7. Set **DLAA + Neural Rendering work scale** separately. The tested starting point
   is 70%.
8. Keep RenoDX **Enable Upscaling (WIP)** disabled.
9. Disable MGS4's built-in FXAA to avoid adding a blur pass before DLAA/NR.

The Feeder restores its persisted work scale during runtime initialization, while a
later live preset/uniform change may become the new persisted value. This preset omits
the scale entirely: select it, set 70% separately, and verify the live control.

## Included values

Color and levels:

```ini
Colourfulness=0.25
BlackPoint=5
WhitePoint=249
```

NR Detail Forge 2:

```ini
NRDF_MASTER_STRENGTH=1.50
NRDF_FINE_DETAIL=1.35
NRDF_MID_DETAIL=0.72
NRDF_MICRO_RECOVERY=0.90
NRDF_DIRECTIONAL_RECOVERY=0.90
NRDF_NOISE_REJECTION=0.58
NRDF_EDGE_PROTECTION=0.82
NRDF_RINGING_PROTECTION=0.88
```

These are intentionally strong settings for the 70%-to-1440p path. They were chosen
as a coherent set; setting every slider to its maximum is not recommended because
detail gain and protection controls then fight each other.

## Adjusting the sharpener

- Still too soft: raise `Fine detail` first in small steps, then
  `Master reconstruction strength`.
- Surfaces need more presence: raise `Surface and texture detail` or
  `Low-contrast recovery`.
- Thin outlines appear: raise `Ringing and halo protection`.
- Fine texture crawls or sparkles in motion: raise `Noise and sparkle rejection`, or
  lower `Fine detail`.
- Hard silhouettes look harsh: raise `Strong-edge protection`.
- Do not stack CAS, RCAS, NIS, DELC, Clarity, or another sharpener on top while
  evaluating NR Detail Forge.

Judge changes in motion as well as in screenshots. A spatial filter can look extremely
sharp in a still frame while amplifying temporal reconstruction residue during camera
movement.

## Rollback

To return to the core slider only:

1. Select the previous ReShade preset or disable `NRDetailForge`.
2. Restore the preset backup if needed.
3. Delete `NRDetailForge.fx` only if the game is closed and no other preset references
   it.

The core Feeder, its persisted scale, and its automatic `_NR50-Backup-*` rollback are
independent of this optional visual preset.
