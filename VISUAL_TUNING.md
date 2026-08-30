# Optional MGS4 visual tune

`presets/MGS4-VisualTune.ini` captures the author's accepted 1440p presentation stack:

```text
LaunchPad -> DLSS5 Feeder -> Colourfulness -> Levels -> Clarity -> RCAS
```

It uses a 70% shared DLSS/DLAA/Neural Rendering scale, restrained color and level
adjustments, low-strength mid-frequency Clarity, and AMD FidelityFX RCAS as the final
post-upscale sharpening pass.

## Additional shader requirements

The preset references third-party shaders which are not redistributed here:

- `Colourfulness.fx`
- `Levels.fx`
- AstrayFX `Clarity.fx`
- CShade `cRCAS.fx` plus its `shared` include directory
- iMMERSE LaunchPad and its existing dependencies

Install those from their own sources and licenses. The preset itself contains only
configuration values.

## Use

1. Copy `presets/MGS4-VisualTune.ini` into the MGS4 directory.
2. Open ReShade and select `MGS4-VisualTune.ini` from the preset selector.
3. Keep RenoDX **Enable Upscaling WIP** disabled.
4. Disable MGS4's built-in FXAA to avoid stacking a blur pass before DLAA/NR.

The preset starts RCAS at `0.40`. A value of `0.35` is the softer baseline; avoid
exceeding approximately `0.45` at a 70% work scale because foliage, hair, specular
edges, and UI text can begin to shimmer or look harsh.

Clarity is intentionally limited to `0.08`. It adds mid-frequency texture separation
rather than another edge-sharpening pass. Do not stack CAS, DELC, NIS, or another
sharpener on top of RCAS.

If skies or fog show obvious color banding, qUINT Debanding can be tested before RCAS
with automatic bit-depth detection and a search radius around `0.25`. It is not enabled
by default because unnecessary debanding can erase subtle gradients.
