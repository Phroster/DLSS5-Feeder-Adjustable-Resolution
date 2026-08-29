@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist build mkdir build
rc /nologo /fo build\version.res src\version.rc
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /Iexternal\reshade\include /Iexternal\ngx /Fobuild\ /Fdbuild\ src\dlss5-feed.cpp /link /OUT:build\dlss5-feed.addon64 build\version.res external\ngx\libs\nvsdk_ngx_d.lib kernel32.lib user32.lib advapi32.lib ole32.lib
endlocal
