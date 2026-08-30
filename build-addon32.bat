@echo off
rem dlss5-feed.addon32 -- the 32-bit in-game half (no NGX; it lives in the host).
cd /d "%~dp0"
if not exist build mkdir build
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat" >nul
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /Iexternal\reshade\include /Fobuild\ /Fdbuild\ ^
   src\dlss5-feed32.cpp ^
   /link /OUT:build\dlss5-feed.addon32 d3d11.lib kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
endlocal
echo addon32 built.
