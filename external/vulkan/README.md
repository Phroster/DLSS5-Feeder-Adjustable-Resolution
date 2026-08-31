# Vulkan headers (not vendored)

The Vulkan transport (`src/feed_vk.h`) needs the Khronos Vulkan headers, which are
**not** committed here (they are large and change often). `vulkan-1.dll` is loaded
dynamically, so no Vulkan library is linked -- only the headers are needed to build.

Drop the Khronos `Vulkan-Headers` here so the include path `external/vulkan` resolves
`<vulkan/vulkan_core.h>`, `<vulkan/vulkan_win32.h>` and their `vk_video/*` sub-includes:

```
git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers
copy   Vulkan-Headers\include\vulkan\*.h    external\vulkan\vulkan\
copy   Vulkan-Headers\include\vk_video\*.h  external\vulkan\vk_video\
```

Apache-2.0; only needed to build the Vulkan path.
