<#
.SYNOPSIS
    Builds GeoMap AI (Release x64, MSVC) and deploys a portable folder to dist\GeoMapAI.
.PARAMETER QtDir
    Qt kit directory, e.g. C:\Qt\6.8.3\msvc2022_64. Detected automatically when omitted.
.PARAMETER Generator
    CMake Visual Studio generator. Default: "Visual Studio 17 2022".
.PARAMETER SkipTests
    Do not run ctest after the build.
.PARAMETER OutDir
    Deployment folder. Default: dist\GeoMapAI. It is recreated on every run.
.PARAMETER Zip
    Also pack the deployment folder into GeoMapAI-<version>-win64[-odm].zip next to it.
.PARAMETER OdmDir
    Folder of an installed OpenDroneMap 3.6.x for Windows (official ODM_Setup_3.6.x.exe, e.g. the
    one GeoMap AI installs into %LOCALAPPDATA%\GeoMapAI\engines\odm-3.6.2). It is copied unmodified
    to <OutDir>\engines\odm, so the deployment processes photos without Docker or any installation.
#>
param(
    [string]$QtDir,
    [string]$Generator = 'Visual Studio 17 2022',
    [switch]$SkipTests,
    [string]$OutDir,
    [switch]$Zip,
    [string]$OdmDir
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

# vcpkg provides GDAL and PROJ (vcpkg.json, triplet x64-windows-geomap): VCPKG_ROOT,
# the GitHub runner's copy, or the one bundled with Visual Studio.
function Find-VcpkgToolchain {
    $roots = @($env:VCPKG_ROOT, $env:VCPKG_INSTALLATION_ROOT)
    $roots += & $vswhere -all -products * -property installationPath | ForEach-Object { Join-Path $_ 'VC\vcpkg' }
    foreach ($r in ($roots | Where-Object { $_ })) {
        $file = Join-Path $r 'scripts\buildsystems\vcpkg.cmake'
        if (Test-Path $file) { return $file }
    }
    throw 'vcpkg not found: install it (https://vcpkg.io) and set VCPKG_ROOT, or add the vcpkg component of Visual Studio.'
}
$toolchain = Find-VcpkgToolchain
Write-Host "vcpkg toolchain: $toolchain"

Push-Location $root
try {
    cmake -S . -B build -G $Generator -A x64 "-DCMAKE_PREFIX_PATH=$QtDir" "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DCMAKE_GENERATOR_INSTANCE=$vsPath"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    cmake --build build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

    if (!$SkipTests) {
        ctest --test-dir build -C Release --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
    }

    $exe = Join-Path $root 'build\Release\GeoMapAI.exe'
    if (!(Test-Path $exe)) { throw "Executable missing: $exe" }
    $out = if ($OutDir) { [IO.Path]::GetFullPath($OutDir) } else { Join-Path $root 'dist\GeoMapAI' }
    # The deployment folder is generated output only; start clean so no stale DLLs remain.
    # Never wipe a folder that is not a previous deployment (e.g. a mistyped -OutDir).
    if (Test-Path $out) {
        if ((Get-ChildItem $out -Force | Select-Object -First 1) -and !(Test-Path (Join-Path $out 'GeoMapAI.exe'))) {
            throw "Refusing to clear $out : it does not look like a GeoMap AI deployment."
        }
        Remove-Item -Recurse -Force $out
    }
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

    # GDAL/PROJ and their libraries, plus proj.db and GDAL's data (found by GeoMap AI in share\).
    $gis = Join-Path $root 'build\vcpkg_installed\x64-windows-geomap'
    if (!(Test-Path (Join-Path $gis 'bin\gdal.dll'))) { throw "GDAL not found in $gis" }
    Get-ChildItem (Join-Path $gis 'bin\*.dll') | Where-Object { $_.Name -notlike 'pkgconf*' } | ForEach-Object { Copy-Item $_.FullName $out -Force }
    New-Item -ItemType Directory -Force -Path (Join-Path $out 'share') | Out-Null
    Copy-Item (Join-Path $gis 'share\proj') (Join-Path $out 'share\proj') -Recurse -Force
    Copy-Item (Join-Path $gis 'share\gdal') (Join-Path $out 'share\gdal') -Recurse -Force
    Write-Host "GDAL/PROJ: $gis"

    if ($OdmDir) {
        # Bundled OpenDroneMap (AGPL-3.0, a separate program): copied as installed, without the
        # uninstaller that belongs to the source installation. GeoMap AI re-points its Python
        # environment to the new folder on first use (what ODM's winpostinstall.bat does).
        $odm = (Resolve-Path $OdmDir).Path
        if (!(Test-Path (Join-Path $odm 'run.py')) -or !(Test-Path (Join-Path $odm 'venv\Scripts\python.exe'))) {
            throw "Not an OpenDroneMap installation: $odm"
        }
        $odmVersion = (Get-Content (Join-Path $odm 'VERSION') -Raw).Trim()
        if ($odmVersion -notmatch '^3\.6\.') { throw "OpenDroneMap 3.6.x expected, found $odmVersion" }
        $target = Join-Path $out 'engines\odm'
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        $copyArgs = @($odm, $target, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/XF', 'unins*.*')
        & robocopy @copyArgs | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "Copying OpenDroneMap failed (robocopy $LASTEXITCODE)" }
        if ($redist) {
            # ODM's tools need the VC++ runtime; its installer adds it system-wide only with admin rights.
            $bin = Join-Path $target 'SuperBuild\install\bin'
            Get-ChildItem (Join-Path $redist.FullName '*.dll') | Where-Object { !(Test-Path (Join-Path $bin $_.Name)) } |
                ForEach-Object { Copy-Item $_.FullName $bin }
        }
        Set-Content -Encoding ASCII (Join-Path $target 'SOURCE.txt') @(
            "OpenDroneMap $odmVersion - official Windows build, unmodified (ODM_Setup_$odmVersion.exe).",
            'License: GNU Affero General Public License v3.0, see LICENSE and licenses\ in this folder.',
            "Source code of this version: https://github.com/OpenDroneMap/ODM/tree/v$odmVersion",
            'OpenDroneMap is a separate program that GeoMap AI runs as an external process.')
        Write-Host "OpenDroneMap $odmVersion bundled: $target"
    }

    if ($Zip) {
        $cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
        if ($cmake -notmatch 'project\(GeoMapAI VERSION ([0-9.]+)') { throw 'Project version not found in CMakeLists.txt' }
        $suffix = if ($OdmDir) { '-odm' } else { '' }
        $archive = Join-Path (Split-Path -Parent $out) "GeoMapAI-$($Matches[1])-win64$suffix.zip"
        if (Test-Path $archive) { Remove-Item -Force $archive }
        # bsdtar (Windows 10+) packs the ~20 000 files of ODM much faster than Compress-Archive.
        $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
        if (Test-Path $tar) {
            & $tar -a -c -f $archive -C (Split-Path -Parent $out) (Split-Path -Leaf $out)
            if ($LASTEXITCODE -ne 0) { throw 'Packing the archive failed' }
        } else {
            Compress-Archive -Path $out -DestinationPath $archive -Force
        }
        Write-Host "Archive: $archive"
    }

    Write-Host "Ready: $(Join-Path $out 'GeoMapAI.exe')" -ForegroundColor Green
} finally {
    Pop-Location
}
