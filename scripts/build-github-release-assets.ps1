param(
    [string]$SourceDirectory = "$PSScriptRoot\..",
    [string]$BuildDirectory = "$PSScriptRoot\..\build",
    [string]$CMakePath = "",
    [string]$InnoCompilerPath = "",
    [switch]$PerUserInstall
)

function Get-LatestUnsuffixedInstaller {
    param([string]$InstallerDirectory)

    if (-not (Test-Path -Path $InstallerDirectory)) {
        return $null
    }

    return Get-ChildItem -Path $InstallerDirectory -File -Filter "CoreGaze-Setup-*.exe" |
        Where-Object { $_.BaseName -notmatch '-with-vcredist$|-no-vcredist$' } |
        Sort-Object -Property LastWriteTime -Descending |
        Select-Object -First 1
}

function Rename-InstallerVariant {
    param(
        [System.IO.FileInfo]$Installer,
        [string]$VariantSuffix
    )

    if ($null -eq $Installer) {
        throw "Installer artifact was not found for variant '$VariantSuffix'."
    }

    if ($Installer.BaseName -notmatch '^CoreGaze-Setup-(.+)$') {
        throw "Unexpected installer name format: $($Installer.Name)"
    }

    $version = $matches[1]
    $targetPath = Join-Path -Path $Installer.DirectoryName -ChildPath ("CoreGaze-Setup-{0}-{1}{2}" -f $version, $VariantSuffix, $Installer.Extension)

    if (Test-Path -Path $targetPath) {
        Remove-Item -Path $targetPath -Force
    }

    Move-Item -Path $Installer.FullName -Destination $targetPath -Force
    return $targetPath
}

$resolvedSourceDirectory = Resolve-Path -Path $SourceDirectory -ErrorAction SilentlyContinue
if ($null -eq $resolvedSourceDirectory) {
    Write-Error "Source directory not found at path: $SourceDirectory"
    exit 1
}

$releaseScriptPath = Join-Path -Path $PSScriptRoot -ChildPath "build-release.ps1"
$installerScriptPath = Join-Path -Path $PSScriptRoot -ChildPath "build-installer.ps1"
if (-not (Test-Path -Path $releaseScriptPath)) {
    Write-Error "Release script not found at path: $releaseScriptPath"
    exit 1
}
if (-not (Test-Path -Path $installerScriptPath)) {
    Write-Error "Installer script not found at path: $installerScriptPath"
    exit 1
}

$installerOutputDirectory = Join-Path -Path $BuildDirectory -ChildPath "installer"

$withBundleParams = @{
    SourceDirectory = $SourceDirectory
    BuildDirectory = $BuildDirectory
}
if (-not [string]::IsNullOrWhiteSpace($CMakePath)) {
    $withBundleParams.CMakePath = $CMakePath
}
if (-not [string]::IsNullOrWhiteSpace($InnoCompilerPath)) {
    $withBundleParams.InnoCompilerPath = $InnoCompilerPath
}
if ($PerUserInstall.IsPresent) {
    $withBundleParams.PerUserInstall = $true
}

Write-Host "Building installer variant with bundled VC++ runtime..."
& $releaseScriptPath @withBundleParams
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed building bundled VC++ runtime installer variant."
    exit $LASTEXITCODE
}

$withBundleInstaller = Get-LatestUnsuffixedInstaller -InstallerDirectory $installerOutputDirectory
try {
    $withBundleFinalPath = Rename-InstallerVariant -Installer $withBundleInstaller -VariantSuffix "with-vcredist"
}
catch {
    Write-Error $_.Exception.Message
    exit 1
}

$withoutBundleParams = @{
    InstallerScriptPath = (Join-Path -Path $resolvedSourceDirectory.Path -ChildPath "installer\CoreGaze.iss")
    ExcludeVcRedistBundle = $true
    SkipVcRedistDownload = $true
}
if (-not [string]::IsNullOrWhiteSpace($InnoCompilerPath)) {
    $withoutBundleParams.InnoCompilerPath = $InnoCompilerPath
}
if ($PerUserInstall.IsPresent) {
    $withoutBundleParams.PerUserInstall = $true
}

Write-Host "Building installer variant without bundled VC++ runtime..."
& $installerScriptPath @withoutBundleParams
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed building no-VC++-bundle installer variant."
    exit $LASTEXITCODE
}

$withoutBundleInstaller = Get-LatestUnsuffixedInstaller -InstallerDirectory $installerOutputDirectory
try {
    $withoutBundleFinalPath = Rename-InstallerVariant -Installer $withoutBundleInstaller -VariantSuffix "no-vcredist"
}
catch {
    Write-Error $_.Exception.Message
    exit 1
}

Write-Host "GitHub release assets generated successfully:"
Write-Host "  - $withBundleFinalPath"
Write-Host "  - $withoutBundleFinalPath"
