param(
    [string]$SourceDirectory = "$PSScriptRoot\..",
    [string]$BuildDirectory = "$PSScriptRoot\..\build",
    [string]$CMakePath = "",
    [string]$InnoCompilerPath = "",
    [switch]$PerUserInstall,
    [switch]$DownloadVcRedist
)

function Resolve-CMakeExecutable {
    param(
        [string]$RequestedPath,
        [string]$BuildDirectoryPath
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        $requestedCommand = Get-Command -Name $RequestedPath -ErrorAction SilentlyContinue
        if ($null -ne $requestedCommand) {
            return $requestedCommand.Source
        }

        $resolvedRequestedPath = Resolve-Path -Path $RequestedPath -ErrorAction SilentlyContinue
        if ($null -ne $resolvedRequestedPath) {
            return $resolvedRequestedPath.Path
        }

        throw "CMake executable not found at '$RequestedPath'."
    }

    $pathCommand = Get-Command -Name "cmake.exe" -ErrorAction SilentlyContinue
    if ($null -ne $pathCommand) {
        return $pathCommand.Source
    }

    if (-not [string]::IsNullOrWhiteSpace($BuildDirectoryPath)) {
        $cacheCandidate = Join-Path -Path $BuildDirectoryPath -ChildPath "CMakeCache.txt"
        if (Test-Path -Path $cacheCandidate) {
            $cachedCommandLine = Select-String -Path $cacheCandidate -Pattern '^CMAKE_COMMAND:INTERNAL=' -ErrorAction SilentlyContinue |
                Select-Object -First 1

            if ($null -ne $cachedCommandLine) {
                $cachedPath = ($cachedCommandLine.Line -split '=', 2)[1]
                if (-not [string]::IsNullOrWhiteSpace($cachedPath) -and (Test-Path -Path $cachedPath)) {
                    return (Resolve-Path -Path $cachedPath).Path
                }
            }
        }
    }

    $commonPaths = @(
        "$env:ProgramFiles\CMake\bin\cmake.exe",
        "$env:ProgramFiles(x86)\CMake\bin\cmake.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )

    foreach ($candidate in $commonPaths) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -Path $candidate)) {
            return (Resolve-Path -Path $candidate).Path
        }
    }

    $vsWherePath = "$env:ProgramFiles(x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -Path $vsWherePath) {
        $installationPath = & $vsWherePath -latest -products * -property installationPath
        if (-not [string]::IsNullOrWhiteSpace($installationPath)) {
            $candidate = Join-Path -Path $installationPath.Trim() -ChildPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path -Path $candidate) {
                return (Resolve-Path -Path $candidate).Path
            }
        }
    }

    throw "CMake executable not found. Install CMake or pass -CMakePath."
}

$resolvedSourceDirectory = Resolve-Path -Path $SourceDirectory -ErrorAction SilentlyContinue
if ($null -eq $resolvedSourceDirectory) {
    Write-Error "Source directory not found at path: $SourceDirectory"
    exit 1
}

$resolvedBuildDirectory = Resolve-Path -Path $BuildDirectory -ErrorAction SilentlyContinue
if ($null -eq $resolvedBuildDirectory) {
    New-Item -Path $BuildDirectory -ItemType Directory -Force | Out-Null
    $resolvedBuildDirectory = Resolve-Path -Path $BuildDirectory
}

try {
    $resolvedCMakePath = Resolve-CMakeExecutable -RequestedPath $CMakePath -BuildDirectoryPath $resolvedBuildDirectory.Path
}
catch {
    Write-Error $_.Exception.Message
    exit 1
}

$cachePath = Join-Path -Path $resolvedBuildDirectory.Path -ChildPath "CMakeCache.txt"
if (-not (Test-Path -Path $cachePath)) {
    Write-Host "Configuring CMake project (Release)..."
    & $resolvedCMakePath -S $resolvedSourceDirectory.Path -B $resolvedBuildDirectory.Path -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake configure failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
}

Write-Host "Building CoreGaze (unsigned)..."
& $resolvedCMakePath --build $resolvedBuildDirectory.Path --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake build failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

$installerBuilderPath = Join-Path -Path $PSScriptRoot -ChildPath "build-installer.ps1"
if (-not (Test-Path -Path $installerBuilderPath)) {
    Write-Error "Installer build script not found at path: $installerBuilderPath"
    exit 1
}

$installerParams = @{
    InstallerScriptPath = (Join-Path -Path $resolvedSourceDirectory.Path -ChildPath "installer\CoreGaze.iss")
}
if (-not [string]::IsNullOrWhiteSpace($InnoCompilerPath)) {
    $installerParams.InnoCompilerPath = $InnoCompilerPath
}
if ($PerUserInstall.IsPresent) {
    $installerParams.PerUserInstall = $true
}
if ($DownloadVcRedist.IsPresent) {
    $installerParams.DownloadVcRedist = $true
}

& $installerBuilderPath @installerParams
if ($LASTEXITCODE -ne 0) {
    Write-Error "Installer build step failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

$installerOutputDirectory = Join-Path -Path $resolvedBuildDirectory.Path -ChildPath "installer"
if (Test-Path -Path $installerOutputDirectory) {
    $latestInstaller = Get-ChildItem -Path $installerOutputDirectory -File -Filter "CoreGaze-Setup-*.exe" |
        Sort-Object -Property LastWriteTime -Descending |
        Select-Object -First 1

    if ($null -ne $latestInstaller) {
        Write-Host "Release build completed successfully."
        Write-Host "Installer: $($latestInstaller.FullName)"
        exit 0
    }
}

Write-Warning "Release flow completed, but installer output could not be located in '$installerOutputDirectory'."
