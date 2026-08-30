NR50 v0.1.0 is the first standalone release of the reduced-cost DLAA plus Neural
Rendering path for the MGS4 Master Collection PC release.

At a native 2560x1440 backbuffer it evaluates DLAA and RenoDX Feature 18 at 1280x720,
then scales the result back to the native backbuffer. This reduces the pixel workload
of those two post-processing stages by 75% without changing MGS4's own output or UI
resolution.

The package contains a reversible installer, exact rollback generation, a runtime log
verifier, the NR50 add-on, and its companion shader. Required third-party components
are deliberately not bundled; read the README and third-party notices before install.
