<#
.SYNOPSIS
    Run the test suites for usb-rectangle-screen-gauge.

.DESCRIPTION
    Two layers, cheapest first:

      1. unit tests   - pure C (gauge_math / gauge_theme / gauge_presets /
                        gfx / gauge_render / the screen protocol and panel)
                        compiled with the system GCC against a stubbed panel
                        and a fake USB transport.  Sub-second, no hardware.
      2. contract     - Python guards on assumptions the C tests cannot see:
                        the generated fonts, the enabled text, the host preview
                        renderer, no graphics library creeping in, and the
                        documentation links.

    Tests register themselves with a constructor in the unit framework, so
    there is no list to keep up to date.

.EXAMPLE
    tools\test.ps1                       # unit + contracts
    tools\test.ps1 -Filter gauge_math    # one suite
#>
[CmdletBinding()]
param(
    [string] $Gcc    = 'C:\msys64\mingw64\bin\gcc.exe',
    [string] $Filter = '',
    [switch] $SkipUnit
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot 'build\tests'
$exe      = Join-Path $buildDir 'unit_tests.exe'

$failed = @()

function Write-Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "    $msg" -ForegroundColor Green }
function Write-Bad($msg)  { Write-Host "    $msg" -ForegroundColor Red }

# ---------------------------------------------------------------------------
# 1. unit tests
# ---------------------------------------------------------------------------
if (-not $SkipUnit) {
    Write-Step 'unit tests'

    if (-not (Test-Path $Gcc)) {
        Write-Bad "gcc not found at $Gcc"
        Write-Host '    Install MSYS2 (pacman -S mingw-w64-x86_64-gcc) or pass -Gcc <path>.'
        $failed += 'unit tests (no compiler)'
    } else {
        New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

        # MinGW's gcc needs its own bin directory on PATH to find its DLLs when
        # it is launched by absolute path.
        $gccDir = Split-Path -Parent $Gcc
        $env:PATH = "$gccDir;$env:PATH"

        # The app sources that have no OS underneath them: the tests link the
        # real renderer, the real protocol and the real panel state machine,
        # with only the panel's one output call replaced by stub_bsp.c.
        $sources = @(
            'tests\unit\test_main.c'
            'tests\unit\test_framework.c'
            'tests\unit\stub_bsp.c'
            'tests\unit\test_gauge_math.c'
            'tests\unit\test_gauge_theme.c'
            'tests\unit\test_gauge_presets.c'
            'tests\unit\test_gfx.c'
            'tests\unit\test_gauge_render.c'
            'tests\unit\test_vocore_proto.c'
            'tests\unit\test_vocore_panel.c'
            'src\gauge_math.c'
            'src\gauge_theme.c'
            'src\gauge_presets.c'
            'src\gauge_render.c'
            'src\gfx.c'
            'src\gfx_text.c'
            'src\gfx_font_data.c'
            'src\vocore_proto.c'
            'src\vocore_panel.c'
        ) | ForEach-Object { Join-Path $repoRoot $_ }

        $args = @(
            '-std=gnu17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
            '-O1', '-g',
            '-Iinclude',
            '-Itests/unit'
        ) + $sources + @('-o', $exe, '-lm')

        Push-Location $repoRoot
        try {
            & $Gcc @args
            if ($LASTEXITCODE -ne 0) {
                Write-Bad 'unit tests failed to compile'
                $failed += 'unit tests (compile)'
            } else {
                if ($Filter) { & $exe $Filter } else { & $exe }
                if ($LASTEXITCODE -ne 0) {
                    Write-Bad 'unit tests reported failures'
                    $failed += 'unit tests'
                } else {
                    Write-Ok 'all unit tests passed'
                }
            }
        } finally {
            Pop-Location
        }
    }
}

# ---------------------------------------------------------------------------
# 2. contract checks
# ---------------------------------------------------------------------------
Write-Step 'contract checks'
Push-Location $repoRoot
try {
    & python 'tests\contracts\test_contracts.py'
    if ($LASTEXITCODE -ne 0) {
        Write-Bad 'contract checks failed'
        $failed += 'contracts'
    } else {
        Write-Ok 'all contracts hold'
    }
} finally {
    Pop-Location
}

# ---------------------------------------------------------------------------
Write-Host ''
if ($failed.Count -eq 0) {
    Write-Host '==================================================' -ForegroundColor Green
    Write-Host '  ALL SUITES PASSED (unit, contracts)' -ForegroundColor Green
    Write-Host '==================================================' -ForegroundColor Green
    exit 0
} else {
    Write-Host '==================================================' -ForegroundColor Red
    Write-Host '  FAILURES:' -ForegroundColor Red
    foreach ($f in $failed) { Write-Host "    - $f" -ForegroundColor Red }
    Write-Host '==================================================' -ForegroundColor Red
    exit 1
}
