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

    $splitPattern = 'feature ready:\s*DLSS/DLAA input\s+(?<iw>\d+)x(?<ih>\d+)\s+\((?<ip>\d+)%\)\s*->\s*NR/output\s+(?<ow>\d+)x(?<oh>\d+)\s+\((?<op>\d+)%\)\s*->\s*(?<bw>\d+)x(?<bh>\d+)\s+backbuffer'
    $legacyPattern = 'feature ready:\s*(?<w>\d+)x(?<h>\d+)\s+(?:shared\s+DLSS/DLAA/NR\s+at\s+(?<scale>\d+)%|DLAA/NR(?:50|\s+at\s+(?<scale2>\d+)%))\s*->\s*(?<bw>\d+)x(?<bh>\d+)\s+backbuffer'
    $splitMatches = [regex]::Matches($feedText, $splitPattern, 'IgnoreCase')
    $legacyMatches = [regex]::Matches($feedText, $legacyPattern, 'IgnoreCase')
    $renoMatches = [regex]::Matches($reshadeText, 'feature 18 created.*?NR input\s+(\d+)x(\d+)\s*->\s*output\s+(\d+)x(\d+)\s+with guides\s+(\d+)x(\d+)', 'IgnoreCase')
    $evalMatches = [regex]::Matches($reshadeText, 'inline feature 18 evaluation succeeded', 'IgnoreCase')

    if (($splitMatches.Count -eq 0) -and ($legacyMatches.Count -eq 0)) {
        Fail 'Feeder did not report a ready DLAA/NR resolution contract in dlss5-feed.log.'
    }
    if ($renoMatches.Count -eq 0) {
        Fail 'RenoDX did not report creation of the expected Feature 18 contract in ReShade.log.'
    }
    if ($evalMatches.Count -eq 0) {
        Fail 'RenoDX did not report a successful inline Feature 18 evaluation.'
    } else {
        Pass 'RenoDX reported a successful inline Feature 18 evaluation.'
    }

    if (($splitMatches.Count -gt 0) -and ($renoMatches.Count -gt 0)) {
        $f = $splitMatches[$splitMatches.Count - 1]
        $r = $renoMatches[$renoMatches.Count - 1]
        $inputWidth = [int]$f.Groups['iw'].Value
        $inputHeight = [int]$f.Groups['ih'].Value
        $outputWidth = [int]$f.Groups['ow'].Value
        $outputHeight = [int]$f.Groups['oh'].Value
        $inputScale = [int]$f.Groups['ip'].Value
        $outputScale = [int]$f.Groups['op'].Value
        $backbufferWidth = [int]$f.Groups['bw'].Value
        $backbufferHeight = [int]$f.Groups['bh'].Value
        $feedInput = "${inputWidth}x${inputHeight}"
        $feedOutput = "${outputWidth}x${outputHeight}"
        $backbuffer = "${backbufferWidth}x${backbufferHeight}"
        $renoInput = "$($r.Groups[1].Value)x$($r.Groups[2].Value)"
        $renoOutput = "$($r.Groups[3].Value)x$($r.Groups[4].Value)"
        $renoGuides = "$($r.Groups[5].Value)x$($r.Groups[6].Value)"

        $expectedInputWidth = if ($inputScale -eq 100) { $backbufferWidth } else { ([int][math]::Floor(($backbufferWidth * $inputScale) / 100.0)) -band -2 }
        $expectedInputHeight = if ($inputScale -eq 100) { $backbufferHeight } else { ([int][math]::Floor(($backbufferHeight * $inputScale) / 100.0)) -band -2 }
        $expectedOutputWidth = if ($outputScale -eq 100) { $backbufferWidth } else { ([int][math]::Floor(($backbufferWidth * $outputScale) / 100.0)) -band -2 }
        $expectedOutputHeight = if ($outputScale -eq 100) { $backbufferHeight } else { ([int][math]::Floor(($backbufferHeight * $outputScale) / 100.0)) -band -2 }

        if (($inputWidth -ne $expectedInputWidth) -or ($inputHeight -ne $expectedInputHeight) -or
            ($outputWidth -ne $expectedOutputWidth) -or ($outputHeight -ne $expectedOutputHeight)) {
            Fail "Scale mismatch: expected input ${expectedInputWidth}x${expectedInputHeight} and output ${expectedOutputWidth}x${expectedOutputHeight}, got $feedInput and $feedOutput."
        } elseif (($feedInput -eq $renoInput) -and ($feedOutput -eq $renoOutput) -and ($feedInput -eq $renoGuides)) {
            Pass "Matched split contract: DLSS/DLAA $feedInput ($inputScale%) -> NR/output $feedOutput ($outputScale%) -> $backbuffer."
        } else {
            Fail "Contract mismatch: Feeder input=$feedInput/output=$feedOutput; RenoDX input=$renoInput/output=$renoOutput/guides=$renoGuides."
        }
    } elseif (($legacyMatches.Count -gt 0) -and ($renoMatches.Count -gt 0)) {
        $f = $legacyMatches[$legacyMatches.Count - 1]
        $r = $renoMatches[$renoMatches.Count - 1]
        $feedInput = "$($f.Groups['w'].Value)x$($f.Groups['h'].Value)"
        $backbuffer = "$($f.Groups['bw'].Value)x$($f.Groups['bh'].Value)"
        $scale = if ($f.Groups['scale'].Success) { [int]$f.Groups['scale'].Value } elseif ($f.Groups['scale2'].Success) { [int]$f.Groups['scale2'].Value } else { 50 }
        $renoInput = "$($r.Groups[1].Value)x$($r.Groups[2].Value)"
        $renoOutput = "$($r.Groups[3].Value)x$($r.Groups[4].Value)"
        $renoGuides = "$($r.Groups[5].Value)x$($r.Groups[6].Value)"
        if (($feedInput -eq $renoInput) -and ($feedInput -eq $renoOutput) -and ($feedInput -eq $renoGuides)) {
            Pass "Matched shared $scale% contract: $feedInput -> $backbuffer."
        } else {
            Fail "Legacy contract mismatch: Feeder=$feedInput, RenoDX input=$renoInput, output=$renoOutput, guides=$renoGuides."
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
    Write-Host 'DLSS5 Feeder resolution-scale verification failed.' -ForegroundColor Red
    exit 1
}
Write-Host 'DLSS5 Feeder resolution-scale verification passed.' -ForegroundColor Green
exit 0
