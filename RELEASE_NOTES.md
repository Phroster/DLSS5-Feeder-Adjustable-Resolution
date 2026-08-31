# Version 0.4.0

DLSS5-Feeder Adjustable Resolution provides one live 50–100% work-resolution slider
for the injected equal-input/output DLAA and DLSS Neural Rendering stage.

The release is deliberately minimal. Download one ZIP, extract it, and copy two
completed files:

1. Copy `dlss5-feed.addon64` beside the game's executable.
2. Copy `DLSS5_Feed.fx` beside `MartysMods_LAUNCHPAD.fx`.
3. Enable `MartysMods_Launchpad` above `DLSS5_Feed` in ReShade.
4. Open **DLSS 5 Feed Work Resolution** and select the desired percentage.

No PowerShell command or installer is required. The release also contains the README,
MIT license, and third-party notices. Required ReShade, LaunchPad, RenoDX, and NVIDIA
runtime components are not redistributed.

At 70%, each image axis is processed at 70% and the injected stage uses approximately
49% as many work pixels as at 100%. This does not guarantee a 51% reduction in total
GPU usage because the game's own renderer and fixed Feeder/ReShade costs remain.

The slider is persisted through `ReShade.ini`, applies after a 400 ms debounce, and
rebuilds the private NGX resources without reloading ReShade's interface.
