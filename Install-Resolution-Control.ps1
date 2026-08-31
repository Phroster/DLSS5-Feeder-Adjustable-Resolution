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

function Get-RunningExecutablesUnderRoot([string] $Root) {
    $prefix = (Get-FullPath $Root) + [IO.Path]::DirectorySeparatorChar
    return @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ExecutablePath -and
        ([IO.Path]::GetFullPath([string]$_.ExecutablePath)).StartsWith(
            $prefix,
            [StringComparison]::OrdinalIgnoreCase
        )
    })
}

if (-not $GameDir) {
    Add-Type -AssemblyName System.Windows.Forms
    $picker = [Windows.Forms.FolderBrowserDialog]::new()
    $picker.Description = 'Select the game directory containing ReShade.ini'
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
$running = @(Get-RunningExecutablesUnderRoot $game)
if ($running.Count -gt 0) {
    $names = @($running | ForEach-Object { "{0} (PID {1})" -f $_.Name, $_.ProcessId }) -join ', '
    throw "An executable inside the selected game directory is running. Close it before installing: $names"
}

$requiredFiles = @(
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

$reshadeProxies = @('dxgi.dll', 'd3d11.dll')
$foundProxies = @($reshadeProxies | Where-Object {
    Test-Path -LiteralPath (Join-Path $game $_) -PathType Leaf
})
if ($foundProxies.Count -eq 0) {
    throw "No supported ReShade proxy was found. Expected one of: $($reshadeProxies -join ', ')"
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
$backupDir = Join-Path $game "_DLSS5-Feeder-Resolution-Control-Backup-$stamp"
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
    # Assert-UnderGame already proved this target begins with "$game\". Using a
    # validated substring keeps the installer compatible with Windows PowerShell 5.1,
    # whose .NET Framework does not provide Path.GetRelativePath().
    $relative = $targetFull.Substring($game.Length + 1)
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
    packageVersion = '0.4.0'
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
$prefix = $game + [IO.Path]::DirectorySeparatorChar
$running = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
    $_.ExecutablePath -and
    ([IO.Path]::GetFullPath([string]$_.ExecutablePath)).StartsWith(
        $prefix,
        [StringComparison]::OrdinalIgnoreCase
    )
})
if ($running.Count -gt 0) {
    $names = @($running | ForEach-Object { "{0} (PID {1})" -f $_.Name, $_.ProcessId }) -join ', '
    throw "An executable inside the recorded game directory is running. Close it before restoring: $names"
}
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
        Write-Host "Removed resolution-control file $target"
    }
}
Write-Host "Rollback complete. Backup retained at $PSScriptRoot" -ForegroundColor Green
'@
$restoreScript | Set-Content -LiteralPath (Join-Path $backupDir 'Restore-Resolution-Control.ps1') -Encoding utf8

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
    Write-Warning "Installation failed after backup creation. Run '$backupDir\Restore-Resolution-Control.ps1' to restore the original state."
    throw
}

$installedHash = (Get-FileHash -LiteralPath $addonTarget -Algorithm SHA256).Hash
Write-Host 'DLSS5 Feeder Adjustable Resolution 0.4.0 installed successfully.' -ForegroundColor Green
Write-Host "Game:     $game"
Write-Host "Preset:   $presetPath"
Write-Host "Backup:   $backupDir"
Write-Host "SHA-256:  $installedHash"
Write-Host 'Keep RenoDX "Enable Upscaling WIP" disabled. The Feeder uses one shared DLAA + Neural Rendering work scale.'
Write-Host 'Launch a scene, then run Verify-Resolution-Control.ps1.'
