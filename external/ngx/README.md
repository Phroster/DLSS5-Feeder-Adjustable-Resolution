# NGX SDK (not included)

This add-on links NVIDIA's NGX SDK, which is **not redistributed** in this repository.

To build, drop the NGX SDK here so the paths match `build.bat`:

```
external/ngx/nvsdk_ngx.h
external/ngx/nvsdk_ngx_defs.h
external/ngx/nvsdk_ngx_defs_dlssd.h
external/ngx/nvsdk_ngx_helpers.h
external/ngx/nvsdk_ngx_helpers_dlssd.h
external/ngx/nvsdk_ngx_params.h
external/ngx/nvsdk_ngx_params_dlssd.h
external/ngx/libs/nvsdk_ngx_d.lib      (Release, /MD)
```

Get it from the NVIDIA DLSS Super Resolution SDK: https://github.com/NVIDIA/DLSS
(headers under `include/`, the import library under `lib/Windows_x86_64/x64/` -- that one
targets VS2015+/UCRT; the `vs2010`/`vs2012`/`vs2013` siblings fail to link with LNK2038).
The runtime DLL (`nvngx_dlss.dll`) and the DLSS 5 neural-rendering model (`nvngx_dlssnr.dll`)
are supplied at runtime by the game / the DLSS 5 add-on, not by this build.
