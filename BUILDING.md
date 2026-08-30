# Building NR50

## Requirements

- Windows 11 x64
- Visual Studio 2022 Build Tools with the Desktop development with C++ workload
- A local checkout of upstream DLSS5-Feeder, including its ReShade submodule headers
- A separately obtained NVIDIA DLSS SDK checkout

The expected inputs are:

```text
<FeederRoot>\external\reshade\include\reshade.hpp
<NgxRoot>\include\nvsdk_ngx.h
<NgxRoot>\lib\Windows_x86_64\x64\nvsdk_ngx_d.lib
```

Build from the repository root:

```powershell
.\Build-NR50.ps1 `
  -FeederRoot 'C:\src\DLSS5-Feeder' `
  -NgxRoot 'C:\SDKs\NVIDIA-DLSS'
```

The script discovers the x64 Visual Studio toolchain, compiles `src\dlss5-feed.cpp`
and `src\version.rc`, writes `build\Release\dlss5-feed.addon64`, copies the result to
`bin`, and prints its SHA-256 hash.

No NVIDIA SDK header, import library, runtime DLL, RenoDX binary, ReShade distribution,
iMMERSE shader, or game file is copied into the repository or release package.
