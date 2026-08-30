# Changelog

## 0.2.0 - 2026-08-30

- Added a native ReShade 50-100% integer resolution slider.
- Persisted the selected scale in ReShade's configuration.
- Debounced slider movement and added coordinated effect recompilation plus NGX
  resource/feature rebuilding after movement stops.
- Rounded reduced work extents down to even pixels and derived motion-vector scale from
  the exact compiled texture dimensions.
- Extended runtime logs and verification to validate arbitrary selected scales.

## 0.1.0 - 2026-08-30

- Added a fixed 50%-linear-dimension DLAA/Neural Rendering path.
- Kept the game backbuffer at native resolution while evaluating DLAA and NR at one
  quarter of the native pixel count.
- Added half-resolution color, depth, and motion-vector preparation with motion-vector
  scaling for the reduced evaluation extent.
- Added a final linear upscale to the native backbuffer.
- Added resolution lifecycle logging and contract verification.
- Added reversible installer, verifier, source build, and release packaging scripts.
