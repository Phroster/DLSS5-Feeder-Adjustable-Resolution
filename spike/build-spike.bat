@echo off
rem Phase-0 spikes: the D3D11/D3D12 cross-process pair, and the OpenGL interop pair.
rem Spikes may link opengl32.lib -- there is no ReShade in their processes.
cd /d "%~dp0"

setlocal
call "%~dp0..\tools\vcvars.bat" x64 || exit /b 1
cl /nologo /O2 /EHsc /W3 spike-host64.cpp /Fe:spike-host64.exe d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /W3 spike-gl64.cpp /Fe:spike-gl64.exe d3d12.lib dxgi.lib opengl32.lib gdi32.lib user32.lib
if errorlevel 1 exit /b 1
endlocal

setlocal
call "%~dp0..\tools\vcvars.bat" amd64_x86 || exit /b 1
cl /nologo /O2 /EHsc /W3 spike-client32.cpp /Fe:spike-client32.exe d3d11.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /W3 spike-gl32.cpp /Fe:spike-gl32.exe opengl32.lib gdi32.lib user32.lib
if errorlevel 1 exit /b 1
endlocal

echo spike built.
