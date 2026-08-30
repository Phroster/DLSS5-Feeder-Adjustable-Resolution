[CmdletBinding()]
param(
    [string] $Version = '0.1.0'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$artifacts = Join-Path $repo 'artifacts'
$stage = Join-Path (Join-Path $repo 'build') ("package-" + [guid]::NewGuid().ToString('N'))
$packageRoot = Join-Path $stage "DLSS5-Feeder-NR50-v$Version"
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null

$rootFiles = @(
    'README.md',
    'LICENSE',
    'THIRD_PARTY_NOTICES.md',
    'CHANGELOG.md',
    'Install-NR50.ps1',
    'Verify-NR50.ps1'
)
foreach ($relative in $rootFiles) {
    $source = Join-Path $repo $relative
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing release file: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $packageRoot $relative)
}

foreach ($directory in @('bin', 'shaders')) {
    $source = Join-Path $repo $directory
    if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "Missing release directory: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $packageRoot $directory) -Recurse
}

New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
$zip = Join-Path $artifacts "DLSS5-Feeder-NR50-v$Version.zip"
Compress-Archive -LiteralPath $packageRoot -DestinationPath $zip -CompressionLevel Optimal -Force
Write-Host "Created $zip" -ForegroundColor Green
Write-Host "SHA-256: $((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)"
