<#
.SYNOPSIS
  Configures and builds the project from a plain PowerShell prompt.

.DESCRIPTION
  Finds whichever Visual Studio edition is installed (Community, Professional,
  Build Tools...) through vswhere, enters its developer environment, and runs
  CMake with the requested preset.

  vcpkg is found through CONVERTER_VCPKG_ROOT or VCPKG_ROOT. Both are read
  BEFORE entering the developer environment, because VS's dev shell overwrites
  VCPKG_ROOT with the vcpkg bundled inside Visual Studio, which only supports
  manifest mode and cannot see globally installed packages.

  If a CMakeUserPresets.json with a "local-windows" preset exists, it is used
  by default; that is where machine-specific paths belong.

.EXAMPLE
  .\scripts\build.ps1                          # Release, Ninja
  .\scripts\build.ps1 -Preset windows-x64-vs   # generate/build the .slnx
  .\scripts\build.ps1 -EngineOnly              # just the engine and convctl
  .\scripts\build.ps1 -Clean                   # wipe the build dir first
#>
[CmdletBinding()]
param(
    [string]$Preset = "",
    [ValidateSet("Debug", "Release")][string]$Config = "Release",
    [switch]$EngineOnly,
    [switch]$Clean,
    [string]$Target = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# --- vcpkg, captured before the dev shell can change it -----------------------
$vcpkgRoot = $env:CONVERTER_VCPKG_ROOT
if (-not $vcpkgRoot) { $vcpkgRoot = $env:VCPKG_ROOT }

$userPresets = Join-Path $root "CMakeUserPresets.json"
$hasLocalPreset = (Test-Path $userPresets) -and ((Get-Content $userPresets -Raw) -match '"local-windows"')
if (-not $Preset) { $Preset = if ($hasLocalPreset) { "local-windows" } else { "windows-x64" } }

if (-not $vcpkgRoot -and -not $Preset.StartsWith("local-")) {
    throw "Set VCPKG_ROOT (or CONVERTER_VCPKG_ROOT) to your vcpkg checkout, e.g. `$env:VCPKG_ROOT = 'C:\vcpkg'"
}

# --- locate Visual Studio -----------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; is any Visual Studio product installed?" }

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio with the C++ toolset was found." }

$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path $cmake)) { throw "CMake is not part of this VS install: $cmake" }

Write-Host "Visual Studio : $vsPath"
Write-Host "CMake         : $((& $cmake --version | Select-Object -First 1))"
Write-Host "Preset        : $Preset"

# --- enter the developer environment -------------------------------------------
$devShell = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"
& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
Set-Location $root

# Put back the vcpkg the presets are meant to see.
if ($vcpkgRoot) { $env:VCPKG_ROOT = $vcpkgRoot } else { Remove-Item Env:VCPKG_ROOT -ErrorAction SilentlyContinue }

# --- configure + build ----------------------------------------------------------
# Native tools write warnings to stderr, which PowerShell 5.1 turns into a
# terminating error under "Stop". Judge them by exit code instead.
$ErrorActionPreference = "Continue"

if ($EngineOnly) {
    if (-not $vcpkgRoot) { throw "EngineOnly needs VCPKG_ROOT or CONVERTER_VCPKG_ROOT." }
    $buildDir = "build/engine-only"
    if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }
    & $cmake -S . -B $buildDir -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_BUILD_TYPE=$Config" `
        "-DCONVERTER_ENGINE_ONLY=ON" `
        "-DVCPKG_TARGET_TRIPLET=x64-windows-static" `
        "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot/scripts/buildsystems/vcpkg.cmake"
    if ($LASTEXITCODE) { throw "configure failed" }
    $buildArgs = @("--build", $buildDir)
    if ($Target) { $buildArgs += @("--target", $Target) }
    & $cmake @buildArgs
    if ($LASTEXITCODE) { throw "build failed" }
    exit 0
}

# Every preset builds into build/<preset name> (see binaryDir in the presets).
if ($Clean -and (Test-Path "build/$Preset")) { Remove-Item -Recurse -Force "build/$Preset" }

$configureArgs = @("--preset", $Preset)
if (-not $Preset.EndsWith("-vs")) { $configureArgs += "-DCMAKE_MAKE_PROGRAM=$ninja" }
& $cmake @configureArgs
if ($LASTEXITCODE) { throw "configure failed" }

$buildArgs = @("--build", "--preset", "$Preset-$($Config.ToLower())")
if ($Target) { $buildArgs += @("--target", $Target) }
& $cmake @buildArgs
if ($LASTEXITCODE) { throw "build failed" }
