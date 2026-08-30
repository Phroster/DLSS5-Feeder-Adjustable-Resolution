@echo off
rem Phase-0 spike: 64-bit host (D3D12) + 32-bit client (D3D11).
cd /d "%~dp0"

setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /EHsc /W3 spike-host64.cpp /Fe:spike-host64.exe d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
endlocal

setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat" >nul
cl /nologo /O2 /EHsc /W3 spike-client32.cpp /Fe:spike-client32.exe d3d11.lib
if errorlevel 1 exit /b 1
endlocal

echo spike built.
