param(
    [Parameter(Mandatory = $true)]
    [string]$CertificatePath,

    [Parameter(Mandatory = $true)]
    [string]$CertificatePassword,

    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [string]$SigntoolPath = "signtool.exe",
    [string]$ExecutablePath = "$PSScriptRoot\..\build\CoreGaze.exe",
    [string]$InstallerPath = ""
)

function Resolve-RequiredPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathToResolve,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    $resolved = Resolve-Path -Path $PathToResolve -ErrorAction SilentlyContinue
    if ($null -eq $resolved) {
        throw "$Label not found at path: $PathToResolve"
    }

    return $resolved.Path
}

function Sign-TargetFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TargetPath,
        [Parameter(Mandatory = $true)]
        [string]$Signtool,
        [Parameter(Mandatory = $true)]
        [string]$CertPath,
        [Parameter(Mandatory = $true)]
        [string]$CertPassword,
        [Parameter(Mandatory = $true)]
        [string]$Timestamp
    )

    & $Signtool sign /fd sha256 /td sha256 /tr $Timestamp /f $CertPath /p $CertPassword $TargetPath
    if ($LASTEXITCODE -ne 0) {
        throw "SignTool failed for $TargetPath with exit code $LASTEXITCODE"
    }

    & $Signtool verify /pa $TargetPath
    if ($LASTEXITCODE -ne 0) {
        throw "SignTool verification failed for $TargetPath with exit code $LASTEXITCODE"
    }
}

try {
    $signtoolCommand = Get-Command -Name $SigntoolPath -ErrorAction SilentlyContinue
    if ($null -eq $signtoolCommand) {
        throw "SignTool not found. Ensure signtool.exe is installed and in PATH, or pass -SigntoolPath."
    }

    $resolvedCertPath = Resolve-RequiredPath -PathToResolve $CertificatePath -Label "Certificate"
    $resolvedExePath = Resolve-RequiredPath -PathToResolve $ExecutablePath -Label "Executable"

    Sign-TargetFile -TargetPath $resolvedExePath -Signtool $signtoolCommand.Source -CertPath $resolvedCertPath -CertPassword $CertificatePassword -Timestamp $TimestampUrl
    Write-Host "Signed executable: $resolvedExePath"

    if ($InstallerPath -ne "") {
        $resolvedInstallerPath = Resolve-RequiredPath -PathToResolve $InstallerPath -Label "Installer"
        Sign-TargetFile -TargetPath $resolvedInstallerPath -Signtool $signtoolCommand.Source -CertPath $resolvedCertPath -CertPassword $CertificatePassword -Timestamp $TimestampUrl
        Write-Host "Signed installer: $resolvedInstallerPath"
    }

    Write-Host "Code signing completed successfully."
}
catch {
    Write-Error $_
    exit 1
}
