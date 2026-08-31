# Changelog

## Unreleased

- Rewrote the public-facing documentation around the exact work-resolution contract:
  the slider controls the injected equal-input/output DLAA plus Neural Rendering stage,
  not MGS4's internal render resolution or total GPU utilization.
- Added exact work-resolution/pixel-count examples, practical scale guidance, manual
  installation, troubleshooting, known conflicts, and clearer verification limits.
- Added the original optional `NRDetailForge.fx` version 2 shader: directional 17-tap
  native-output reconstruction with separate detail bands, noise rejection, chroma
  preservation, and adaptive anti-ringing protection.
- Replaced the optional Clarity + RCAS preset stack with one coherent NR Detail Forge 2
  pass and removed the misleading assumption that a preset forces the persisted scale.
- Restored Windows PowerShell 5.1 compatibility in the bounded installer.
- Changed release packaging to an explicit file allowlist, added version-consistency
  checks and a SHA-256 sidecar, and removed temporary staging directories after use.
- Recorded the tested upstream Feeder and NVIDIA DLSS SDK revisions and clarified
  third-party licensing boundaries.
- Kept the core v0.3.1 slider behavior unchanged; rebuilt its binary only to correct
  the exported configuration/log description.

## 0.3.1 - 2026-08-30

- Returned to one shared DLSS/DLAA plus Neural Rendering resolution slider.
- Retained full-resolution guide capture and add-on-side dynamic resizing, so slider
  changes no longer reload the ReShade effect or reset its UI.
- Removed unsupported split input/output controls and RenoDX WIP-upscaling guidance.
- Kept the 400 ms debounce, exact motion-vector scaling, persistence, and rollback.
- Added an optional MGS4 visual preset using post-NR Colourfulness, Levels, low-strength
  Clarity, and RCAS without redistributing third-party shaders.

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
