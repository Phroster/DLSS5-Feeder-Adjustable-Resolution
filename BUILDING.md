# Building DLSS5-Feeder Adjustable Resolution

The checked-in v0.4.0 binary is built from this repository's source against these
recorded dependency revisions. The slider path was runtime-verified before the latest
version/metadata rebuild; smoke-test the current binary in a compatible game before
publishing or mirroring the release:

- DLSS5-Feeder/ReShade headers:
  [`c452ddc09d3d3ec5e51a9ee2178ead6674fefbac`](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/c452ddc09d3d3ec5e51a9ee2178ead6674fefbac)
- NVIDIA DLSS SDK 310.7.0:
  [`a291cc7d2cc642a51566f3dfd5376f635cd1b284`](https://github.com/NVIDIA/DLSS/commit/a291cc7d2cc642a51566f3dfd5376f635cd1b284)

Newer upstream revisions may change interfaces or behavior. Pin these revisions when
reproducing the existing binary, and review upstream changes deliberately before
upgrading either dependency.

## Requirements

- Windows 11 x64
- Visual Studio 2022 Build Tools with the Desktop development with C++ workload
- A local checkout of upstream DLSS5-Feeder, including its ReShade submodule headers
- A separately obtained NVIDIA DLSS SDK checkout

The NVIDIA SDK is subject to its own terms. Confirm that your intended build and
distribution comply with those terms; this repository does not grant rights to SDK
headers, libraries, or runtimes.

The expected inputs are:

```text
<FeederRoot>\external\reshade\include\reshade.hpp
<NgxRoot>\include\nvsdk_ngx.h
<NgxRoot>\lib\Windows_x86_64\x64\nvsdk_ngx_d.lib
```

Build from the repository root:

```powershell
.\Build-Resolution-Control.ps1 `
  -FeederRoot 'C:\src\DLSS5-Feeder' `
  -NgxRoot 'C:\SDKs\NVIDIA-DLSS'
```

The script discovers the x64 Visual Studio toolchain, compiles `src\dlss5-feed.cpp`
and `src\version.rc`, writes `build\Release\dlss5-feed.addon64`, copies the result to
`bin`, and prints its SHA-256 hash.

No NVIDIA SDK header, import library, runtime DLL, RenoDX binary, ReShade distribution,
iMMERSE shader, or game file is copied into the repository or release package.

Compiler, linker, and SDK revisions can affect the final binary hash. Always publish
the hash produced by the build script and test the resulting add-on in a rendered scene
before treating it as release-ready.
