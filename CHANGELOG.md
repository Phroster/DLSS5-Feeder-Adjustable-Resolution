# Changelog

## 0.3.1 - 2026-08-30

- Returned to one shared DLSS/DLAA plus Neural Rendering resolution slider.
- Retained full-resolution guide capture and add-on-side dynamic resizing, so slider
  changes no longer reload the ReShade effect or reset its UI.
- Removed unsupported split input/output controls and RenoDX WIP-upscaling guidance.
- Kept the 400 ms debounce, exact motion-vector scaling, persistence, and rollback.

## 0.3.0 - 2026-08-30 (withdrawn experiment)

- Split the scale control into DLSS/DLAA input and Neural Rendering/output sliders.
- Moved color, depth, and motion-vector resizing from effect compilation into a dynamic
  D3D11 multi-render-target resampling pass inside the add-on.
- Removed effect reloads from slider changes, preserving the ReShade UI state.
- Added a 400 ms shared debounce and exact per-axis motion-vector correction.
- Added independent DLSS render and target extents plus automatic quality-mode selection.
- Enforced the signed-runtime constraint that NR/output cannot be below DLSS input; the
  most recently moved slider wins when synchronization is required.
- Added recovery from a rejected experimental scale without leaving the add-on disabled.
- Extended logs and verification for split input/output/guide contracts.

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
