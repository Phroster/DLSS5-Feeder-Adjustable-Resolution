Version 0.2.0 turns the original fixed NR50 path into a selectable-resolution ReShade
add-on for the MGS4 Master Collection PC release.

The ReShade effect now exposes a 50-100% slider in one-percent steps. The game and UI
remain at the native backbuffer resolution; only DLAA and RenoDX Feature 18 use the
selected work size. Slider input is debounced, so dragging produces one companion-effect
recompile and one safe NGX rebuild after movement stops. The value is persisted.

The package contains a reversible installer, exact rollback generation, a runtime log
verifier, the resolution-scale add-on, and its companion shader. Required third-party components
are deliberately not bundled; read the README and third-party notices before install.
