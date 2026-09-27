<#
.SYNOPSIS
    Build the gauge for the VoCore USB2.0 screen.

.DESCRIPTION
    One gcc invocation: the app in src/ (console, frame loop, panel layer,
    libusb transport) plus the drawing code it shares with nothing else -
    gfx and gauge are just part of this program.  The panel's geometry is
    compiled in as 480x800.

    libusb is loaded at runtime by the app, so nothing is linked against it
    here; this script only makes sure a libusb-1.0.dll is sitting next to the
    executable.  It looks for that DLL in this order:

      1. $env:GAUGE_LIBUSB_DLL
      2. the Python libusb-package (pip install libusb-package)
      3. C:\msys64\mingw64\bin\libusb-1.0.dll

.EXAMPLE
    tools\build.ps1
    tools\build.ps1 -Gcc C:\msys64\mingw64\bin\gcc.exe
#>
[CmdletBinding()]
param(
    [string] $Gcc = 'C:\msys64\mingw64\bin\gcc.exe',
    [string] $Out = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $repoRoot 'build' }
$exe = Join-Path $Out 'gauge.exe'

if (-not (Test-Path $Gcc)) {
    throw "gcc not found at $Gcc (install MSYS2 or pass -Gcc <path>)"
}
$gccDir = Split-Path -Parent $Gcc
$env:PATH = "$gccDir;$env:PATH"

New-Item -ItemType Directory -Force -Path $Out | Out-Null

$sources = @(
    'src\main.c'
    'src\app_time.c'
    'src\app_bsp.c'
    'src\app_gauge.c'
    'src\app_tests.c'
    'src\app_console.c'
    'src\usb_libusb.c'
    'src\vocore_proto.c'
    'src\vocore_panel.c'
    'src\gfx.c'
    'src\gfx_text.c'
    'src\gfx_font_data.c'
    'src\gauge_math.c'
    'src\gauge_theme.c'
    'src\gauge_presets.c'
    'src\gauge_render.c'
) | ForEach-Object { Join-Path $repoRoot $_ }

$args = @(
    '-std=gnu17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
    '-O2', '-g',
    '-DGFX_W=480', '-DGFX_H=800',
    '-Iinclude'
) + $sources + @('-o', $exe, '-lm', '-lwinmm')

Write-Host "==> building $exe" -ForegroundColor Cyan
& $Gcc @args
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

# ---------------------------------------------------------------------------
# a libusb to load at runtime
# ---------------------------------------------------------------------------
$dll = $null
if ($env:GAUGE_LIBUSB_DLL -and (Test-Path $env:GAUGE_LIBUSB_DLL)) {
    $dll = $env:GAUGE_LIBUSB_DLL
}
if (-not $dll) {
    $candidate = Join-Path $gccDir 'libusb-1.0.dll'
    if (Test-Path $candidate) { $dll = $candidate }
}
if (-not $dll) {
    # the Python libusb-package wheels, if one is installed for this user
    $patterns = @(
        "$env:LOCALAPPDATA\Packages\PythonSoftwareFoundation.Python.*\LocalCache\local-packages\Python*\site-packages\libusb_package\libusb-1.0.dll",
        "$env:APPDATA\Python\Python*\site-packages\libusb_package\libusb-1.0.dll",
        "$env:LOCALAPPDATA\Programs\Python\Python*\Lib\site-packages\libusb_package\libusb-1.0.dll"
    )
    foreach ($pattern in $patterns) {
        $hit = Get-Item $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) {
            $dll = $hit.FullName
            break
        }
    }
}
if (-not $dll) {
    $candidate = 'C:\msys64\mingw64\bin\libusb-1.0.dll'
    if (Test-Path $candidate) { $dll = $candidate }
}

if ($dll) {
    Copy-Item $dll $Out -Force
    Write-Host "    libusb: $dll" -ForegroundColor Green
} else {
    Write-Host "    no libusb-1.0.dll found - set GAUGE_LIBUSB_DLL or pip install libusb-package" -ForegroundColor Yellow
}

Write-Host "    run: $exe" -ForegroundColor Green
Write-Host "    (the panel is a WinUSB device; the VoCore driver package installs that binding)"
