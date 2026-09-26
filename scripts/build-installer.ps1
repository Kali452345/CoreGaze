param(
    [string]$InnoCompilerPath = "",
    [string]$InstallerScriptPath = "$PSScriptRoot\..\installer\CoreGaze.iss",
    [switch]$PerUserInstall,
    [switch]$BuildFirst,
    # Defaults to the version baked into CoreGaze.exe (COREGAZE_VERSION in CMakeLists.txt).
    [string]$Version = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ─── Helper: Resolve ISCC.exe ─────────────────────────────────────────────────
function Resolve-InnoCompilerPath {
    param([string]$RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        if (Test-Path $RequestedPath) { return (Resolve-Path $RequestedPath).Path }
        throw "Inno Setup compiler not found at '$RequestedPath'."
    }

    # 1. PATH
    $cmd = Get-Command "iscc.exe" -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    # 2. Known install locations
    $candidates = @(
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 7\ISCC.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }

    # 3. Registry scan
    $uninstallRoots = @(
        "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
    )
    foreach ($root in $uninstallRoots) {
        foreach ($entry in @(Get-ItemProperty -Path $root -ErrorAction SilentlyContinue)) {
            if ($null -eq $entry.DisplayName -or $entry.DisplayName -notlike "Inno Setup*") { continue }
            foreach ($prop in @("InstallLocation", "DisplayIcon")) {
                $base = $entry.$prop
                if (-not [string]::IsNullOrWhiteSpace($base)) {
                    $candidate = Join-Path (Split-Path $base -Parent) "ISCC.exe"
                    if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
                }
            }
        }
    }

    return $null
}

# ─── Optional: auto-install Inno Setup via winget ─────────────────────────────
function Install-InnoSetup {
    $winget = Get-Command "winget" -ErrorAction SilentlyContinue
    if ($null -eq $winget) {
        throw "winget not found. Please install Inno Setup manually from https://jrsoftware.org/isdl.php"
    }

    Write-Host "Inno Setup not found - installing via winget..."
    & winget install --id JRSoftware.InnoSetup --exact --accept-package-agreements --accept-source-agreements --silent
    if ($LASTEXITCODE -ne 0) {
        throw "winget failed to install Inno Setup (exit code $LASTEXITCODE)."
    }
    Write-Host "Inno Setup installed successfully."
}

# ─── Optional: build CoreGaze.exe first ───────────────────────────────────────
if ($BuildFirst.IsPresent) {
    Write-Host "Building CoreGaze.exe..."
    $buildScript = Join-Path $PSScriptRoot "build-release.ps1"
    & $buildScript
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
}

# ─── Resolve CoreGaze.exe ─────────────────────────────────────────────────────
$buildDir = Join-Path $PSScriptRoot "..\build"
$exePath  = Join-Path $buildDir "CoreGaze.exe"
if (-not (Test-Path $exePath)) {
    Write-Error "CoreGaze.exe not found at '$exePath'. Run build-release.ps1 first, or pass -BuildFirst."
    exit 1
}

# ─── Resolve version from the exe so the installer can never disagree with it ─
$exeVersionInfo = (Get-Item $exePath).VersionInfo
$exeVersion = "{0}.{1}.{2}" -f $exeVersionInfo.FileMajorPart, $exeVersionInfo.FileMinorPart, $exeVersionInfo.FileBuildPart
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $exeVersion
} elseif ($Version -ne $exeVersion) {
    Write-Error "Requested version '$Version' does not match CoreGaze.exe ($exeVersion). Rebuild with: cmake -S . -B build `"-DCOREGAZE_VERSION=$Version`""
    exit 1
}
Write-Host "Installer version: $Version"

# ─── Resolve ISCC.exe (auto-install if missing) ───────────────────────────────
$iscc = Resolve-InnoCompilerPath -RequestedPath $InnoCompilerPath

if ($null -eq $iscc) {
    Install-InnoSetup

    # Refresh PATH for newly installed binaries
    $env:PATH = [System.Environment]::GetEnvironmentVariable("PATH", "Machine") + ";" +
                [System.Environment]::GetEnvironmentVariable("PATH", "User")

    $iscc = Resolve-InnoCompilerPath -RequestedPath ""
    if ($null -eq $iscc) {
        Write-Error "Inno Setup installed but ISCC.exe still not found. Please open a new terminal and retry."
        exit 1
    }
}

Write-Host "Using Inno Setup compiler: $iscc"

# ─── Resolve .iss script ──────────────────────────────────────────────────────
$issPath = Resolve-Path $InstallerScriptPath -ErrorAction SilentlyContinue
if ($null -eq $issPath) {
    Write-Error "Installer script not found at: $InstallerScriptPath"
    exit 1
}

# ─── Create output directory ──────────────────────────────────────────────────
$outDir = Join-Path $buildDir "installer"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

# ─── Compile ──────────────────────────────────────────────────────────────────
$compilerArgs = @()
if ($PerUserInstall.IsPresent) { $compilerArgs += "/DPerUserInstall=1" }
# MinGW builds statically link the CRT - no VC++ Redist needed
$compilerArgs += "/DNoVcRedistBundle"
$compilerArgs += "/DMyAppVersion=$Version"
$compilerArgs += $issPath.Path

Write-Host "Compiling installer..."
& $iscc @compilerArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "Inno Setup compilation failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

$installerGlob = Join-Path $outDir "CoreGaze-Setup-*.exe"
$installerFile = Get-Item $installerGlob -ErrorAction SilentlyContinue | Select-Object -Last 1
if ($installerFile) {
    $sizeMB = [math]::Round($installerFile.Length / 1MB, 2)
    Write-Host ""
    Write-Host "=== Installer ready ===" -ForegroundColor Green
    Write-Host "  Path : $($installerFile.FullName)" -ForegroundColor Cyan
    Write-Host "  Size : $sizeMB MB" -ForegroundColor Cyan
} else {
    Write-Host "Installer compiled successfully."
}
