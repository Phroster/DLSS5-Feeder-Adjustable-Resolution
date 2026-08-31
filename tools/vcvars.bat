@echo off
rem Enter a Visual Studio C++ build environment for the requested target.
rem
rem   call tools\vcvars.bat x64          (native 64-bit; was vcvars64.bat)
rem   call tools\vcvars.bat amd64_x86    (64-bit host, 32-bit target; was vcvarsamd64_x86.bat)
rem
rem The argument is passed straight to vcvarsall.bat. Resolution order:
rem   1. %VCVARSALL% if you already point it somewhere,
rem   2. whatever vswhere reports as the latest install with the C++ tools
rem      (this is what makes the GitHub Actions runners work),
rem   3. the VS 18 BuildTools path this project was developed against.
rem
rem Deliberately no setlocal: the whole point is to leave the environment behind.

if not "%VCVARSALL%"=="" goto :have

set "_vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%_vswhere%" goto :fallback

rem Through a temp file rather than `for /f` backticks: cmd mangles a backticked
rem command whose first token is a quoted path containing spaces.
set "_vsout=%TEMP%\dlss5-vswhere-%RANDOM%.txt"
"%_vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%_vsout%" 2>nul
for /f "usebackq tokens=* delims=" %%i in ("%_vsout%") do set "VCVARSALL=%%i\VC\Auxiliary\Build\vcvarsall.bat"
del "%_vsout%" >nul 2>&1
set "_vsout="

:fallback
set "_vswhere="
if exist "%VCVARSALL%" goto :have
set "VCVARSALL=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"

:have
if not exist "%VCVARSALL%" (
    echo [vcvars] no Visual Studio C++ toolset found. 1>&2
    echo [vcvars] Set VCVARSALL to your vcvarsall.bat and try again. 1>&2
    exit /b 1
)

call "%VCVARSALL%" %1 >nul
if errorlevel 1 (
    echo [vcvars] "%VCVARSALL%" %1 failed. 1>&2
    exit /b 1
)
exit /b 0
