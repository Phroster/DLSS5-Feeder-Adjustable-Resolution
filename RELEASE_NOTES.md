# Version 0.3.1

Version 0.3.1 returns to one proven shared DLSS/DLAA plus Neural Rendering resolution
control and removes the visible ReShade reload that followed slider movement in v0.2.

The ReShade effect exposes one 50-100% slider in one-percent steps. It captures
full-resolution guides while the add-on performs dynamic color/depth/motion resampling
into private work textures. Slider input is debounced, so dragging produces one safe
resource/NGX rebuild after movement stops without resetting the ReShade interface.

The DLSS input, native DLAA output, depth, motion vectors, and RenoDX Feature 18 all use
the same selected work resolution. RenoDX WIP upscaling should remain disabled.

The package contains a reversible installer, scoped rollback generation, a runtime log
verifier, the resolution-scale add-on, and its companion shader. Required third-party components
are deliberately not bundled; read the README and third-party notices before install.
