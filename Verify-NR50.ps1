[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [string] $GameDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$game = [IO.Path]::GetFullPath($GameDir).TrimEnd([IO.Path]::DirectorySeparatorChar)
$failed = $false

function Pass([string] $Message) { Write-Host "[PASS] $Message" -ForegroundColor Green }
function Fail([string] $Message) {
    $script:failed = $true
    Write-Host "[FAIL] $Message" -ForegroundColor Red
}

if (-not (Test-Path -LiteralPath (Join-Path $game 'mgs4.exe') -PathType Leaf)) {
    throw "mgs4.exe was not found in: $game"
}

$addon = Join-Path $game 'dlss5-feed.addon64'
$shader = @(Get-ChildItem -LiteralPath (Join-Path $game 'reshade-shaders\Shaders') -Filter 'DLSS5_Feed.fx' -File -Recurse -ErrorAction SilentlyContinue)
if (Test-Path -LiteralPath $addon -PathType Leaf) {
    $version = (Get-Item -LiteralPath $addon).VersionInfo.ProductVersion
    Pass "Add-on present (version $version, SHA-256 $((Get-FileHash -LiteralPath $addon -Algorithm SHA256).Hash))."
} else {
    Fail 'dlss5-feed.addon64 is missing from the game directory.'
}
if ($shader.Count -eq 1) {
    Pass "Companion shader present at $($shader[0].FullName)."
} else {
    Fail "Expected exactly one DLSS5_Feed.fx below reshade-shaders\Shaders; found $($shader.Count)."
}

$feedLog = Join-Path $game 'dlss5-feed.log'
$reshadeLog = Join-Path $game 'ReShade.log'
if (-not (Test-Path -LiteralPath $feedLog -PathType Leaf)) { Fail "Missing runtime log: $feedLog" }
if (-not (Test-Path -LiteralPath $reshadeLog -PathType Leaf)) { Fail "Missing runtime log: $reshadeLog" }

if ((Test-Path -LiteralPath $feedLog -PathType Leaf) -and (Test-Path -LiteralPath $reshadeLog -PathType Leaf)) {
    $feedText = Get-Content -LiteralPath $feedLog -Raw
    $reshadeText = Get-Content -LiteralPath $reshadeLog -Raw

    $feedMatches = [regex]::Matches($feedText, 'feature ready:\s*(\d+)x(\d+)\s+DLAA/NR50\s*->\s*(\d+)x(\d+)\s+backbuffer', 'IgnoreCase')
    $renoMatches = [regex]::Matches($reshadeText, 'feature 18 created.*?NR input\s+(\d+)x(\d+)\s*->\s*output\s+(\d+)x(\d+)\s+with guides\s+(\d+)x(\d+)', 'IgnoreCase')
    $evalMatches = [regex]::Matches($reshadeText, 'inline feature 18 evaluation succeeded', 'IgnoreCase')

    if ($feedMatches.Count -eq 0) {
        Fail 'Feeder did not report a ready NR50 contract in dlss5-feed.log.'
    }
    if ($renoMatches.Count -eq 0) {
        Fail 'RenoDX did not report creation of the expected Feature 18 contract in ReShade.log.'
    }
    if ($evalMatches.Count -eq 0) {
        Fail 'RenoDX did not report a successful inline Feature 18 evaluation.'
    } else {
        Pass 'RenoDX reported a successful inline Feature 18 evaluation.'
    }

    if (($feedMatches.Count -gt 0) -and ($renoMatches.Count -gt 0)) {
        $f = $feedMatches[$feedMatches.Count - 1]
        $r = $renoMatches[$renoMatches.Count - 1]
        $feedInput = "$($f.Groups[1].Value)x$($f.Groups[2].Value)"
        $backbuffer = "$($f.Groups[3].Value)x$($f.Groups[4].Value)"
        $renoInput = "$($r.Groups[1].Value)x$($r.Groups[2].Value)"
        $renoOutput = "$($r.Groups[3].Value)x$($r.Groups[4].Value)"
        $renoGuides = "$($r.Groups[5].Value)x$($r.Groups[6].Value)"
        if (($feedInput -eq $renoInput) -and ($feedInput -eq $renoOutput) -and ($feedInput -eq $renoGuides)) {
            Pass "Matched contract: $feedInput DLAA/NR50 -> $backbuffer backbuffer."
        } else {
            Fail "Contract mismatch: Feeder=$feedInput, RenoDX input=$renoInput, output=$renoOutput, guides=$renoGuides."
        }
    }

    $badMarkers = @('0xBAD00005', 'fallback')
    $foundBad = @($badMarkers | Where-Object { $feedText -match [regex]::Escape($_) -or $reshadeText -match [regex]::Escape($_) })
    if ($foundBad.Count -eq 0) {
        Pass 'No known fallback or 0xBAD00005 failure marker appears in the current logs.'
    } else {
        Fail "Failure marker(s) present in current logs: $($foundBad -join ', ')."
    }
}

if ($failed) {
    Write-Host 'NR50 verification failed.' -ForegroundColor Red
    exit 1
}
Write-Host 'NR50 verification passed.' -ForegroundColor Green
exit 0
