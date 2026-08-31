[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = Join-Path $repo 'build'
$artifacts = Join-Path $repo 'artifacts'
$stage = Join-Path $buildRoot ("package-" + [guid]::NewGuid().ToString('N'))
$packageRoot = Join-Path $stage "DLSS5-Feeder-Adjustable-Resolution-v$Version"

if ($Version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    throw "Version must be a stable semantic version such as 0.4.0; got '$Version'."
}

# Keep the runtime package explicit. Recursive directory copies can silently include
# an untracked experiment that happened to be present in a developer's checkout.
$payloadFiles = @(
    'README.md',
    'LICENSE',
    'THIRD_PARTY_NOTICES.md',
    'CHANGELOG.md',
    'RELEASE_NOTES.md',
    'BUILDING.md',
    'Build-Resolution-Control.ps1',
    'Install-Resolution-Control.ps1',
    'Verify-Resolution-Control.ps1',
    'bin\dlss5-feed.addon64',
    'src\dlss5-feed.cpp',
    'src\version.rc',
    'shaders\DLSS5_Feed.fx'
)

if ((& git -C $repo rev-parse --is-inside-work-tree 2>$null) -ne 'true') {
    throw "Release packaging requires a Git worktree: $repo"
}

$worktreeChanges = @(& git -C $repo status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw 'git status failed.' }
if ($worktreeChanges.Count -ne 0) {
    throw "Refusing to package a dirty worktree. Commit or remove every change first.`n$($worktreeChanges -join "`n")"
}

foreach ($relative in $payloadFiles) {
    & git -C $repo ls-files --error-unmatch -- $relative *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "Release payload is not tracked by Git: $relative"
    }
}

$headCommit = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Could not resolve HEAD.' }
$tagCommit = (& git -C $repo rev-parse -q --verify "refs/tags/v$Version^{commit}" 2>$null)
if ($LASTEXITCODE -eq 0) {
    $tagCommit = $tagCommit.Trim()
    if ($tagCommit -ne $headCommit) {
        throw "Tag v$Version already points to $tagCommit, not current HEAD $headCommit. Choose a new version; never replace a published version from different source."
    }
}

# A shallow/developer checkout may not have local tags even when the version is already
# published. Query origin directly so an old release cannot be replaced accidentally.
$remoteTagLines = @(& git -C $repo ls-remote --tags origin "refs/tags/v$Version" "refs/tags/v$Version^{}" 2>$null)
if ($LASTEXITCODE -ne 0) {
    throw 'Could not verify version tags on origin. Release packaging stops when remote version history cannot be checked.'
}
if ($remoteTagLines.Count -gt 0) {
    $dereferenced = @($remoteTagLines | Where-Object { $_ -match '\^\{\}\s*$' } | Select-Object -First 1)
    $selectedRemoteTag = if ($dereferenced.Count -gt 0) { $dereferenced[0] } else { $remoteTagLines[0] }
    $remoteTagCommit = ($selectedRemoteTag -split '\s+')[0]
    if ($remoteTagCommit -ne $headCommit) {
        throw "Remote tag v$Version already points to $remoteTagCommit, not current HEAD $headCommit. Choose a new version; never replace a published version from different source."
    }
}

$binary = Join-Path $repo 'bin\dlss5-feed.addon64'
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    throw "Missing release binary: $binary"
}

# Version numbers exist in the binary, source/resource metadata, installer, and the
# package filename. Refuse a mixed-version archive rather than publishing ambiguity.
$expectedBinaryVersion = "$Version.0"
$binaryVersion = (Get-Item -LiteralPath $binary).VersionInfo.ProductVersion
if ($binaryVersion -ne $expectedBinaryVersion) {
    throw "Binary ProductVersion is '$binaryVersion'; expected '$expectedBinaryVersion'."
}

$sourceText = Get-Content -LiteralPath (Join-Path $repo 'src\dlss5-feed.cpp') -Raw
$resourceText = Get-Content -LiteralPath (Join-Path $repo 'src\version.rc') -Raw
$installerText = Get-Content -LiteralPath (Join-Path $repo 'Install-Resolution-Control.ps1') -Raw
$releaseNotesText = Get-Content -LiteralPath (Join-Path $repo 'RELEASE_NOTES.md') -Raw
if ($sourceText -notmatch ('#define\s+FEED_VERSION\s+"' + [regex]::Escape($Version) + '-single"')) {
    throw "src\dlss5-feed.cpp does not declare FEED_VERSION $Version-single."
}
if ($resourceText -notmatch ('VALUE\s+"ProductVersion",\s+"' + [regex]::Escape($expectedBinaryVersion) + '"')) {
    throw "src\version.rc does not declare ProductVersion $expectedBinaryVersion."
}
if ($installerText -notmatch ("packageVersion\s*=\s*'" + [regex]::Escape($Version) + "'")) {
    throw "Install-Resolution-Control.ps1 does not declare packageVersion $Version."
}
if ($releaseNotesText -notmatch ('(?m)^# Version ' + [regex]::Escape($Version) + '$')) {
    throw "RELEASE_NOTES.md is not headed '# Version $Version'."
}

try {
    New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null

    foreach ($relative in $payloadFiles) {
        $source = Join-Path $repo $relative
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Missing release file: $source"
        }

        $destination = Join-Path $packageRoot $relative
        $destinationDirectory = Split-Path -Parent $destination
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }

    New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
    $zip = Join-Path $artifacts "DLSS5-Feeder-Adjustable-Resolution-v$Version.zip"
    Compress-Archive -LiteralPath $packageRoot -DestinationPath $zip -CompressionLevel Optimal -Force

    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    $checksum = "$zip.sha256"
    "$hash *$([IO.Path]::GetFileName($zip))" | Set-Content -LiteralPath $checksum -Encoding ascii

    Write-Host "Created $zip" -ForegroundColor Green
    Write-Host "SHA-256: $hash"
    Write-Host "Checksum: $checksum"
}
finally {
    $fullStage = [IO.Path]::GetFullPath($stage)
    $stagePrefix = [IO.Path]::GetFullPath($buildRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) +
                   [IO.Path]::DirectorySeparatorChar
    if ($fullStage.StartsWith($stagePrefix, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $fullStage -PathType Container)) {
        Remove-Item -LiteralPath $fullStage -Recurse -Force
    }
}
