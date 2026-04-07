# Code Signing

CoreGaze includes signing-ready tooling for executable and installer artifacts.

## What Is Implemented
- `scripts/sign-artifacts.ps1` signs and verifies:
  - `CoreGaze.exe`
  - installer `.exe` (optional)
- CMake has optional post-build signing variables and hook controls.
- CI template workflow exists at `.github/workflows/release-signing-template.yml`.

## Required Inputs
- PFX certificate path
- PFX password
- Timestamp URL (default: `http://timestamp.digicert.com`)
- `signtool.exe` in PATH or explicit path

## Local Usage Example
- `./scripts/sign-artifacts.ps1 -CertificatePath "C:\\secure\\codesign.pfx" -CertificatePassword "<secret>"`

## CI Usage
- Provide secrets:
  - `COREGAZE_CODESIGN_CERT_B64`
  - `COREGAZE_CODESIGN_CERT_PASSWORD`
- Optionally set variable:
  - `COREGAZE_TIMESTAMP_URL`

## Notes
- Without a valid certificate, signing remains disabled by design.
- Unsinged builds still compile and run normally.
