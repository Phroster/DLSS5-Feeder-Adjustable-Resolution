@echo off
rem VkLayer_feed_vk.dll -- appends the external-interop extensions the feeder needs.
cd /d "%~dp0"
setlocal
call "%~dp0..\tools\vcvars.bat" x64 || exit /b 1
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /I..\external\vulkan feed_vk_layer.cpp ^
   /Fe:VkLayer_feed_vk.dll ^
   /link /OUT:VkLayer_feed_vk.dll /EXPORT:vkNegotiateLoaderLayerInterfaceVersion kernel32.lib
if errorlevel 1 exit /b 1
endlocal
echo layer built.
