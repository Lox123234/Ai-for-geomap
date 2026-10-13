<#
.SYNOPSIS
    Builds GeoMap AI (Release x64, MSVC) and deploys a portable folder to dist\GeoMapAI.
.PARAMETER QtDir
    Qt kit directory, e.g. C:\Qt\6.8.3\msvc2022_64. Detected automatically when omitted.
.PARAMETER Generator
    CMake Visual Studio generator. Default: "Visual Studio 17 2022".
.PARAMETER SkipTests
    Do not run ctest after the build.
#>
param(
    [string]$QtDir,
    [string]$Generator = 'Visual Studio 17 2022',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Get-QtKitVersion([string]$kit) {
    # <root>\<6.x.y>\<msvc20xx_64>
    $name = Split-Path -Leaf (Split-Path -Parent $kit)
    if ($name -match '^\d+\.\d+(\.\d+)?$') { return [version]$name }
    return $null
}

function Find-QtKit {
    $roots = @()
    if ($env:QTDIR) { $roots += $env:QTDIR }                                 # <kit>
    if ($env:Qt6_DIR) { $roots += [IO.Path]::GetFullPath((Join-Path $env:Qt6_DIR '..\..\..')) }  # <kit>\lib\cmake\Qt6
    $qtpaths = Get-Command qtpaths.exe -ErrorAction SilentlyContinue
    if ($qtpaths) { $roots += (Split-Path -Parent (Split-Path -Parent $qtpaths.Source)) }
    foreach ($drive in Get-PSDrive -PSProvider FileSystem) { $roots += (Join-Path $drive.Root 'Qt') }
    $roots += (Join-Path $env:USERPROFILE 'Qt')

    $kits = @()
    foreach ($r in ($roots | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique)) {
        # Accept either a kit directory itself or a Qt root containing <version>\<kit>.
        $candidates = @(Get-Item $r) + @(Get-ChildItem $r -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue })
        foreach ($c in $candidates) {
            if ($c.Name -notmatch '^msvc20\d\d_64$') { continue }   # MSVC x64 only, never MinGW/ARM
            if (!(Test-Path (Join-Path $c.FullName 'lib\cmake\Qt6\Qt6Config.cmake'))) { continue }
            $v = Get-QtKitVersion $c.FullName
            if ($v -and $v -ge [version]'6.5') {
                $kits += [pscustomobject]@{ Path = $c.FullName; Version = $v; Msvc = $c.Name }
            }
        }
    }
    $best = $kits | Sort-Object Version, Msvc -Descending | Select-Object -First 1
    if (!$best) { throw 'No Qt 6.5+ MSVC x64 kit found. Install Qt (MSVC 2022 64-bit) or pass -QtDir.' }
    return $best.Path
}

if (!$QtDir) { $QtDir = Find-QtKit; Write-Host "Detected Qt kit: $QtDir" }
$QtDir = (Resolve-Path $QtDir).Path
if ((Split-Path -Leaf $QtDir) -notmatch '^msvc20\d\d_64$') {
    throw "Qt kit must be an MSVC 64-bit kit (msvc20xx_64); got: $QtDir"
}
$Deploy = Join-Path $QtDir 'bin\windeployqt.exe'
if (!(Test-Path $Deploy)) { throw "windeployqt not found in $QtDir\bin" }

# Locate the Visual Studio instance that matches the generator (17 -> 2022).
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio 2022 (C++ workload) or Build Tools 2022.' }
if ($Generator -notmatch '^Visual Studio (\d+)') { throw "Only Visual Studio generators are supported: $Generator" }
$vsMajor = [int]$Matches[1]
$vsPath = & $vswhere -products * -version "[$vsMajor.0,$($vsMajor + 1).0)" `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath
if (!$vsPath) { throw "No Visual Studio $vsMajor installation with MSVC x64 tools found." }
Write-Host "Visual Studio: $vsPath"

Push-Location $root
try {
    cmake -S . -B build -G $Generator -A x64 "-DCMAKE_PREFIX_PATH=$QtDir"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    cmake --build build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

    if (!$SkipTests) {
        ctest --test-dir build -C Release --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
    }

    $exe = Join-Path $root 'build\Release\GeoMapAI.exe'
    if (!(Test-Path $exe)) { throw "Executable missing: $exe" }
    $out = Join-Path $root 'dist\GeoMapAI'
    # dist\GeoMapAI is generated output only; start clean so no stale DLLs remain.
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    Copy-Item $exe (Join-Path $out 'GeoMapAI.exe') -Force

    # The VC++ runtime is copied app-local below instead of bundling vc_redist.x64.exe.
    & $Deploy --release --qmldir (Join-Path $root 'qml') --no-compiler-runtime --no-system-dxc-compiler `
        --dir $out (Join-Path $out 'GeoMapAI.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed' }

    $redist = Get-ChildItem (Join-Path $vsPath 'VC\Redist\MSVC') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+' } | Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue } |
        Select-Object -First 1
    if ($redist) {
        Copy-Item (Join-Path $redist.FullName '*.dll') $out -Force
        Write-Host "MSVC runtime: $($redist.FullName)"
    } else {
        Write-Warning 'MSVC runtime DLLs not found; target PCs will need the VC++ 2015-2022 x64 Redistributable.'
    }

    Write-Host "Ready: $(Join-Path $out 'GeoMapAI.exe')" -ForegroundColor Green
} finally {
    Pop-Location
}
