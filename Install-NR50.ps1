[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $GameDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FullPath([string] $Path) {
    return [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar)
}

function Assert-UnderGame([string] $Path, [string] $Root) {
    $full = Get-FullPath $Path
    $prefix = (Get-FullPath $Root) + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to change a path outside the selected game directory: $full"
    }
    return $full
}

if (-not $GameDir) {
    Add-Type -AssemblyName System.Windows.Forms
    $picker = [Windows.Forms.FolderBrowserDialog]::new()
    $picker.Description = 'Select the MGS4 directory containing mgs4.exe'
    $picker.ShowNewFolderButton = $false
    if ($picker.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) {
        throw 'Installation cancelled.'
    }
    $GameDir = $picker.SelectedPath
}

$game = Get-FullPath $GameDir
if (-not (Test-Path -LiteralPath $game -PathType Container)) {
    throw "Game directory does not exist: $game"
}
if (-not (Test-Path -LiteralPath (Join-Path $game 'mgs4.exe') -PathType Leaf)) {
    throw "mgs4.exe was not found in: $game"
}
if (Get-Process -Name 'mgs4' -ErrorAction SilentlyContinue) {
    throw 'MGS4 is running. Close it before installing NR50.'
}

$requiredFiles = @(
    'dxgi.dll',
    'renodx-dlss5.addon64',
    'nvngx_dlss.dll',
    'nvngx_dlssnr.dll',
    'ReShade.ini'
)
foreach ($relative in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $game $relative) -PathType Leaf)) {
        throw "Missing prerequisite: $relative"
    }
}

$shaderRoot = Join-Path $game 'reshade-shaders\Shaders'
$launchPads = @(Get-ChildItem -LiteralPath $shaderRoot -Filter 'MartysMods_LAUNCHPAD.fx' -File -Recurse -ErrorAction SilentlyContinue)
if ($launchPads.Count -ne 1) {
    throw "Expected exactly one MartysMods_LAUNCHPAD.fx below '$shaderRoot'; found $($launchPads.Count)."
}
$launchPad = $launchPads[0]
$martysMods = Join-Path $launchPad.Directory.FullName 'MartysMods'
if (-not (Test-Path -LiteralPath $martysMods -PathType Container)) {
    throw "The required MartysMods include directory is not beside $($launchPad.FullName)."
}

$reshadeIni = Join-Path $game 'ReShade.ini'
$presetLine = Get-Content -LiteralPath $reshadeIni | Where-Object { $_ -match '^\s*PresetPath\s*=' } | Select-Object -First 1
if (-not $presetLine) {
    throw 'ReShade.ini does not contain PresetPath=.'
}
$presetValue = ($presetLine -replace '^\s*PresetPath\s*=\s*', '').Trim().Trim('"')
if (-not $presetValue) {
    $presetValue = 'ReShadePreset.ini'
}
$presetPath = if ([IO.Path]::IsPathRooted($presetValue)) {
    Get-FullPath $presetValue
} else {
    Get-FullPath (Join-Path $game $presetValue)
}
$presetPath = Assert-UnderGame $presetPath $game
if (-not (Test-Path -LiteralPath $presetPath -PathType Leaf)) {
    throw "The active ReShade preset was not found: $presetPath"
}

$repoRoot = $PSScriptRoot
$sourceAddon = Join-Path $repoRoot 'bin\dlss5-feed.addon64'
$sourceShader = Join-Path $repoRoot 'shaders\DLSS5_Feed.fx'
foreach ($source in @($sourceAddon, $sourceShader)) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Release payload is incomplete: $source"
    }
}

$addonTarget = Assert-UnderGame (Join-Path $game 'dlss5-feed.addon64') $game
$shaderTarget = Assert-UnderGame (Join-Path $launchPad.Directory.FullName 'DLSS5_Feed.fx') $game
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupDir = Join-Path $game "_NR50-Backup-$stamp"
New-Item -ItemType Directory -Path $backupDir -ErrorAction Stop | Out-Null

$entries = [Collections.Generic.List[object]]::new()
function Save-Target {
    param(
        [string] $Target,
        [string] $BackupName,
        [bool] $Modified
    )
    $targetFull = Assert-UnderGame $Target $game
    $exists = Test-Path -LiteralPath $targetFull -PathType Leaf
    $backupFile = $null
    if ($exists) {
        $backupFile = $BackupName
        Copy-Item -LiteralPath $targetFull -Destination (Join-Path $backupDir $backupFile) -Force
    }
    $relative = [IO.Path]::GetRelativePath($game, $targetFull)
    $entries.Add([ordered]@{
        targetRelative = $relative
        backupFile = $backupFile
        existed = [bool]$exists
        modified = $Modified
    })
}

Save-Target -Target $addonTarget -BackupName 'addon.original' -Modified $true
Save-Target -Target $shaderTarget -BackupName 'shader.original' -Modified $true
Save-Target -Target $presetPath -BackupName 'preset.original' -Modified $true
Save-Target -Target $reshadeIni -BackupName 'ReShade.ini.original' -Modified $false

$manifest = [ordered]@{
    schema = 1
    createdUtc = [DateTime]::UtcNow.ToString('o')
    gameDir = $game
    packageVersion = '0.2.0'
    entries = $entries
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $backupDir 'manifest.json') -Encoding utf8

$restoreScript = @'
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$manifestPath = Join-Path $PSScriptRoot 'manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$game = [IO.Path]::GetFullPath([string]$manifest.gameDir).TrimEnd([IO.Path]::DirectorySeparatorChar)
if (Get-Process -Name 'mgs4' -ErrorAction SilentlyContinue) {
    throw 'MGS4 is running. Close it before restoring the backup.'
}
$prefix = $game + [IO.Path]::DirectorySeparatorChar
foreach ($entry in $manifest.entries) {
    if (-not [bool]$entry.modified) { continue }
    $target = [IO.Path]::GetFullPath((Join-Path $game ([string]$entry.targetRelative)))
    if (-not $target.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to restore a target outside the recorded game directory: $target"
    }
    if ([bool]$entry.existed) {
        $source = Join-Path $PSScriptRoot ([string]$entry.backupFile)
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Backup payload is missing: $source"
        }
        Copy-Item -LiteralPath $source -Destination $target -Force
        Write-Host "Restored $target"
    } elseif (Test-Path -LiteralPath $target -PathType Leaf) {
        Remove-Item -LiteralPath $target -Force
        Write-Host "Removed NR50-created file $target"
    }
}
Write-Host "Rollback complete. Backup retained at $PSScriptRoot" -ForegroundColor Green
'@
$restoreScript | Set-Content -LiteralPath (Join-Path $backupDir 'Restore-NR50.ps1') -Encoding utf8

try {
    Copy-Item -LiteralPath $sourceAddon -Destination $addonTarget -Force
    Copy-Item -LiteralPath $sourceShader -Destination $shaderTarget -Force

    $lines = [Collections.Generic.List[string]]::new()
    foreach ($line in Get-Content -LiteralPath $presetPath) { $lines.Add($line) }
    $techniqueIndex = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*Techniques\s*=') { $techniqueIndex = $i; break }
    }
    $requiredTechniques = @(
        'MartysMods_Launchpad@MartysMods_LAUNCHPAD.fx',
        'DLSS5_Feed@DLSS5_Feed.fx'
    )
    $existing = @()
    if ($techniqueIndex -ge 0) {
        $existingValue = $lines[$techniqueIndex] -replace '^\s*Techniques\s*=\s*', ''
        $existing = @($existingValue -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    }
    $remaining = @($existing | Where-Object { $_ -notin $requiredTechniques })
    $newLine = 'Techniques=' + (($requiredTechniques + $remaining) -join ',')
    if ($techniqueIndex -ge 0) {
        $lines[$techniqueIndex] = $newLine
    } else {
        $lines.Insert(0, $newLine)
    }
    $lines | Set-Content -LiteralPath $presetPath -Encoding utf8
} catch {
    Write-Warning "Installation failed after backup creation. Run '$backupDir\Restore-NR50.ps1' to restore the original state."
    throw
}

$installedHash = (Get-FileHash -LiteralPath $addonTarget -Algorithm SHA256).Hash
Write-Host 'DLSS5 Feeder Resolution Scale 0.2.0 installed successfully.' -ForegroundColor Green
Write-Host "Game:     $game"
Write-Host "Preset:   $presetPath"
Write-Host "Backup:   $backupDir"
Write-Host "SHA-256:  $installedHash"
Write-Host 'Keep RenoDX "Enable Upscaling WIP" disabled, launch a scene, then run Verify-NR50.ps1.'
