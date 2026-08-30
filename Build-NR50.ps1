[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $FeederRoot,

    [Parameter(Mandatory)]
    [string] $NgxRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = $PSScriptRoot
$reshadeInclude = Join-Path $FeederRoot 'external\reshade\include'
$reshadeHeader = Join-Path $reshadeInclude 'reshade.hpp'
$ngxInclude = Join-Path $NgxRoot 'include'
$ngxHeader = Join-Path $ngxInclude 'nvsdk_ngx.h'
$ngxLibDir = Join-Path $NgxRoot 'lib\Windows_x86_64\x64'
$ngxLib = Join-Path $ngxLibDir 'nvsdk_ngx_d.lib'

foreach ($required in @($reshadeHeader, $ngxHeader, $ngxLib)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing build dependency: $required"
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'vswhere.exe was not found. Install Visual Studio 2022 Build Tools with C++ support.'
}
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'A Visual Studio x64 C++ toolchain was not found.' }
$vcvars = Join-Path $vsInstall 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) { throw "Missing $vcvars" }

$environment = & "$env:SystemRoot\System32\cmd.exe" /d /s /c "`"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Failed to initialize the Visual Studio build environment.' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}

$outputDir = Join-Path $repo 'build\Release'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$resource = Join-Path $outputDir 'version.res'
$object = Join-Path $outputDir 'dlss5-feed.obj'
$output = Join-Path $outputDir 'dlss5-feed.addon64'

& rc.exe /nologo "/fo$resource" (Join-Path $repo 'src\version.rc')
if ($LASTEXITCODE -ne 0) { throw "Resource compilation failed with exit code $LASTEXITCODE." }

$compile = @(
    '/nologo', '/std:c++17', '/O2', '/GL', '/EHsc', '/MD', '/LD',
    "/I$reshadeInclude", "/I$ngxInclude", ('/Fo' + $object),
    (Join-Path $repo 'src\dlss5-feed.cpp'), $resource,
    '/link', '/LTCG', '/OPT:REF', '/OPT:ICF', "/LIBPATH:$ngxLibDir",
    'nvsdk_ngx_d.lib', 'd3d11.lib', 'd3d12.lib', 'dxgi.lib', 'd3dcompiler.lib',
    'user32.lib', 'advapi32.lib', "/OUT:$output",
    ("/IMPLIB:" + (Join-Path $outputDir 'dlss5-feed.lib'))
)
& cl.exe @compile
if ($LASTEXITCODE -ne 0) { throw "C++ compilation failed with exit code $LASTEXITCODE." }

$binDir = Join-Path $repo 'bin'
New-Item -ItemType Directory -Path $binDir -Force | Out-Null
$binOutput = Join-Path $binDir 'dlss5-feed.addon64'
Copy-Item -LiteralPath $output -Destination $binOutput -Force
$hash = (Get-FileHash -LiteralPath $binOutput -Algorithm SHA256).Hash
Write-Host "Built $binOutput" -ForegroundColor Green
Write-Host "SHA-256: $hash"
