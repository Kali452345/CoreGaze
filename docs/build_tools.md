# Build Tools: CMake & Inno Setup Locations

This document describes where CMake and Inno Setup are located on your system and how to use them to compile and distribute your own programs.

## CMake

### What is CMake?
CMake is a cross-platform build system generator that converts `CMakeLists.txt` files into native build files (`.ninja`, `.sln`, etc.).

### Installation Locations

CMake can be installed in multiple locations. The build scripts check them in this priority order:

| Location | Path | Notes |
|----------|------|-------|
| **Standalone (Preferred)** | `C:\Program Files\CMake\bin\cmake.exe` | Recommended standalone installation |
| **Standalone (32-bit)** | `C:\Program Files (x86)\CMake\bin\cmake.exe` | Alternate 32-bit installation |
| **Visual Studio 2022 Community** | `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` | Bundled with Community Edition |
| **Visual Studio 2022 Professional** | `C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` | Bundled with Professional Edition |
| **Visual Studio 2022 Enterprise** | `C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` | Bundled with Enterprise Edition |
| **Visual Studio 2022 BuildTools** | `C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` | Bundled with BuildTools |
| **System PATH** | (any directory in `%PATH%`) | If `cmake.exe` is in PATH, it will be found automatically |
| **From CMakeCache.txt** | (previous build directory) | Previously cached location from a prior build |

### How to Install CMake

#### Option 1: Standalone Installation (Recommended)
1. Download from: https://cmake.org/download/
2. Run the installer and choose "Add CMake to system PATH" during installation
3. Verify: Open PowerShell and run `cmake --version`

#### Option 2: Visual Studio Bundle
If you have Visual Studio 2022 installed:
- CMake is automatically included
- No additional installation needed
- Already integrated into the build system

### Using CMake for Your Program

#### Basic Commands

Configure a build (generates build files):
```powershell
cmake -S <source_directory> -B <build_directory> -DCMAKE_BUILD_TYPE=Release
```

Build the project:
```powershell
cmake --build <build_directory> --config Release
```

Example:
```powershell
# For a project in C:\MyApp
cmake -S C:\MyApp -B C:\MyApp\build -DCMAKE_BUILD_TYPE=Release
cmake --build C:\MyApp\build --config Release
```

#### Example CMakeLists.txt for Your Program

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyApplication CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add your source files
add_executable(MyApp
    src/main.cpp
    src/utils.cpp
)

# Link libraries
target_link_libraries(MyApp PRIVATE windowsapp)

# Set output directory
set_target_properties(MyApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
```

---

## Inno Setup

### What is Inno Setup?
Inno Setup is a free installer/uninstaller generator for Windows programs. It compiles `.iss` script files into `.exe` installers.

### Installation Locations

Inno Setup installation is detected in this priority order:

| Location | Method | Notes |
|----------|--------|-------|
| **System PATH** | Environment Variable | If `iscc.exe` is in PATH |
| **Windows Registry (HKCU)** | `HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*` | Current user installations |
| **Windows Registry (HKLM)** | `HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*` | System-wide installations |
| **Windows Registry (Wow6432Node)** | `HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*` | 32-bit application registry |

Typical installation path: `C:\Program Files (x86)\Inno Setup 6\ISCC.exe`

### How to Install Inno Setup

1. Download from: https://jrsoftware.org/isdl.php
2. Run the installer (`innosetup-6.x.x.exe`)
3. Accept default installation to `C:\Program Files (x86)\Inno Setup 6\`
4. The installer automatically adds it to PATH (may require restart)
5. Verify: Open PowerShell and run `iscc.exe /?`

### Using Inno Setup for Your Program

#### Compiler Command

```powershell
iscc.exe /O"<output_directory>" "<path_to_script.iss>"
```

#### Example `.iss` Script (installer configuration)

```ini
[Setup]
AppId={{12345678-1234-1234-1234-123456789012}}
AppName=MyApplication
AppVersion=1.0.0.0
AppPublisher=MyCompany
DefaultDirName={autopf}\MyApplication
DefaultGroupName=MyApplication
OutputBaseFilename=MyApp-Setup-1.0.0
Compression=lzma2
SolidCompression=yes
UninstallDisplayIcon={app}\MyApp.exe

[Files]
Source: "build\bin\MyApp.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\MyApplication"; Filename: "{app}\MyApp.exe"
Name: "{commondesktop}\MyApplication"; Filename: "{app}\MyApp.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"

[Run]
Filename: "{app}\MyApp.exe"; Description: "Launch MyApplication"; Flags: nowait postinstall skipifsilent
```

#### Build an Installer

```powershell
# Compile the installer script
iscc.exe /O"C:\output" "C:\MyApp\installer\MyApp.iss"

# Output file will be: C:\output\MyApp-Setup-1.0.0.exe
```

---

## Full Build Workflow for Your Program

### Step 1: Set Up CMake
```powershell
# Configure the build system
cmake -S C:\MyProgram -B C:\MyProgram\build -DCMAKE_BUILD_TYPE=Release
```

### Step 2: Compile with CMake
```powershell
# Build the executable
cmake --build C:\MyProgram\build --config Release
```

### Step 3: Create Installer Script
Create `C:\MyProgram\installer\MyProgram.iss` with your program details.

### Step 4: Build Installer with Inno Setup
```powershell
# Compile the installer
iscc.exe /O"C:\MyProgram\dist" "C:\MyProgram\installer\MyProgram.iss"
```

### Automated Build Script (PowerShell)

Create a file called `build.ps1`:

```powershell
param(
    [string]$SourceDirectory = $PSScriptRoot,
    [string]$BuildDirectory = "$PSScriptRoot\build",
    [string]$CMakePath = "",
    [string]$InnoCompilerPath = ""
)

# Use CMake from PATH or provided path
if ([string]::IsNullOrWhiteSpace($CMakePath)) {
    $cmake = "cmake"
} else {
    $cmake = $CMakePath
}

# Use Inno Setup from PATH or provided path
if ([string]::IsNullOrWhiteSpace($InnoCompilerPath)) {
    $iscc = "iscc.exe"
} else {
    $iscc = $InnoCompilerPath
}

# Configure
Write-Host "Configuring CMake..."
& $cmake -S $SourceDirectory -B $BuildDirectory -DCMAKE_BUILD_TYPE=Release

# Build
Write-Host "Building..."
& $cmake --build $BuildDirectory --config Release

# Create installer
Write-Host "Creating installer..."
& $iscc /O"$BuildDirectory\dist" "$SourceDirectory\installer\MyProgram.iss"

Write-Host "Build complete! Installer at: $BuildDirectory\dist\MyProgram-Setup-*.exe"
```

Run it:
```powershell
.\build.ps1
# Or with custom tool paths:
.\build.ps1 -CMakePath "C:\cmake\bin\cmake.exe" -InnoCompilerPath "C:\InnoSetup\ISCC.exe"
```

---

## Troubleshooting

### CMake Not Found
**Problem:** "CMake executable not found"

**Solution:**
- Install standalone CMake from https://cmake.org/download/
- Or use from Visual Studio: `"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`
- Or pass explicitly: `-CMakePath "C:\path\to\cmake.exe"`

### Inno Setup Not Found
**Problem:** "Inno Setup compiler not found"

**Solution:**
- Install from https://jrsoftware.org/isdl.php
- Ensure it's in PATH: `where iscc.exe`
- Or pass explicitly: `-InnoCompilerPath "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"`

### Build Fails
- Check `CMakeCache.txt` in build directory for previous configuration
- Delete entire build directory and try again: `Remove-Item -Recurse -Force build\`
- Check Visual Studio or compiler is installed (C++ workload required)
