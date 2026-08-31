@echo off
rem VkLayer_feed_vk.dll -- appends the external-interop extensions the feeder needs.
rem
rem Built for both architectures: x64 games use the 64-bit add-on's transport, 32-bit
rem games on DXVK use the 32-bit one (issue #15). The x86 pair lives in its own
rem subdirectory because the loader tries EVERY manifest it finds on VK_LAYER_PATH --
rem two same-named manifests in one directory would have it load the wrong-bitness DLL
rem and simply skip the layer.
cd /d "%~dp0"
setlocal
call "%~dp0..\tools\vcvars.bat" x64 || exit /b 1
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /I..\external\vulkan feed_vk_layer.cpp ^
   /Fe:VkLayer_feed_vk.dll ^
   /link /OUT:VkLayer_feed_vk.dll /DEF:feed_vk_layer.def kernel32.lib
if errorlevel 1 exit /b 1
endlocal

setlocal
if not exist x86 mkdir x86
call "%~dp0..\tools\vcvars.bat" amd64_x86 || exit /b 1
rem Separate object/pdb names so the two architectures cannot clobber each other.
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /I..\external\vulkan feed_vk_layer.cpp ^
   /Fox86\ /Fdx86\ /Fe:x86\VkLayer_feed_vk32.dll ^
   /link /OUT:x86\VkLayer_feed_vk32.dll /DEF:feed_vk_layer.def kernel32.lib
if errorlevel 1 exit /b 1
endlocal
rem x86\VkLayer_feed_vk32.json is checked in next to the DLL it points at.
echo layer built (x64 + x86).
