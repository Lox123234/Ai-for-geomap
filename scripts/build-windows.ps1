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
    Also pack the deployment folder into GeoMapAI-<version>-win64.zip next to it.
.PARAMETER OdmDir
    OpenDroneMap 3.6.x for Windows to build in. Default: <repo>\.odm\odm-3.6.2 (see -FetchOdm).
    OpenDroneMap is part of every GeoMap AI deployment: it is copied unmodified to
    <OutDir>\engines\odm, and the program runs it from there (no Docker, WSL or installation).
.PARAMETER FetchOdm
    Prepare <repo>\.odm\odm-3.6.2 once: download the official signed ODM_Setup_3.6.2.exe from GitHub
    (233.5 MB, accepted only with the pinned SHA-256) unless -OdmInstaller gives it, and install it
    silently for the current user into that folder (Windows lists it as "ODM" in installed apps).
.PARAMETER OdmInstaller
    Local ODM_Setup_3.6.2.exe for -FetchOdm instead of the download (its SHA-256 is still checked).
.PARAMETER NoOdm
    Developer/CI build without OpenDroneMap: everything works except processing.
.PARAMETER Installer
    Also build the Windows installer GeoMapAI-<version>-win64-setup.exe next to the deployment
    folder (installer\GeoMapAI.nsi): per-user, no administrator rights, Start menu and desktop
    shortcuts, uninstall from "Apps". Needs NSIS 3 (makensis): <repo>\.tools\nsis-*, PATH or
    Program Files.
.PARAMETER FetchAi
    Prepare the local AI once: ONNX Runtime 1.30.0 (MIT, official release archive, 78.8 MB) into
    <repo>\.deps and the weights named in models\<id>\model.json into <repo>\.deps\models\<id>
    (MobileSAM 44 MB, LaMa 208 MB). Every file is accepted only with its pinned SHA-256.
.PARAMETER NoAi
    Build and deploy without ONNX Runtime and models: the AI tools say that this build has none.
#>
param(
    [string]$QtDir,
    [string]$Generator = 'Visual Studio 17 2022',
    [switch]$SkipTests,
    [string]$OutDir,
    [switch]$Zip,
    [string]$OdmDir,
    [switch]$FetchOdm,
    [string]$OdmInstaller,
    [switch]$NoOdm,
    [switch]$Installer,
    [switch]$FetchAi,
    [switch]$NoAi
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# Local AI: ONNX Runtime pinned like in CMakeLists.txt; model weights pinned in their cards.
$ortName = 'onnxruntime-win-x64-1.30.0'
$ortUrl = "https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/$ortName.zip"
$ortSha256 = 'c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949'
$ortDir = Join-Path $root ".deps\$ortName"
$weightsDir = Join-Path $root '.deps\models'
function Get-Verified([string]$url, [string]$file, [string]$sha) {
    if (!(Test-Path $file)) {
        New-Item -ItemType Directory -Force -Path (Split-Path $file) | Out-Null
        Write-Host "Downloading $url"
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $url -OutFile "$file.part" -UseBasicParsing
        Move-Item "$file.part" $file -Force
    }
    $hash = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLowerInvariant()
    if ($hash -ne $sha) { throw "$file has SHA-256 $hash, expected $sha" }
}
if ($FetchAi -and !$NoAi) {
    if (!(Test-Path (Join-Path $ortDir 'lib\onnxruntime.dll'))) {
        $zip = Join-Path $root ".deps\$ortName.zip"
        Get-Verified $ortUrl $zip $ortSha256
        Expand-Archive -Path $zip -DestinationPath (Join-Path $root '.deps') -Force
    }
    foreach ($card in Get-ChildItem (Join-Path $root 'models\*\model.json')) {
        $id = Split-Path (Split-Path $card) -Leaf
        foreach ($f in (Get-Content $card -Raw -Encoding UTF8 | ConvertFrom-Json).files) {
            Get-Verified $f.url (Join-Path $weightsDir "$id\$($f.name)") $f.sha256
        }
    }
}

# OpenDroneMap 3.6.2 for Windows, pinned like in src/processing/OdmEngine.h.
$odmInstallerUrl = 'https://github.com/OpenDroneMap/ODM/releases/download/v3.6.2/ODM_Setup_3.6.2.exe'
$odmInstallerSha256 = 'c9dbfabf0d066529a7b6c753fd45bf3366a0314266160cd677373bab4caff294'
$odmCache = Join-Path $root '.odm'
if (!$NoOdm) {
    if (!$OdmDir) { $OdmDir = Join-Path $odmCache 'odm-3.6.2' }
    if ($FetchOdm -and !(Test-Path (Join-Path $OdmDir 'run.py'))) {
        New-Item -ItemType Directory -Force -Path $odmCache | Out-Null
        $installer = if ($OdmInstaller) { $OdmInstaller } else { Join-Path $odmCache 'ODM_Setup_3.6.2.exe' }
        if (!(Test-Path $installer)) {
            Write-Host "Downloading $odmInstallerUrl"
            Invoke-WebRequest -Uri $odmInstallerUrl -OutFile $installer -UseBasicParsing
        }
        $hash = (Get-FileHash -Algorithm SHA256 $installer).Hash.ToLowerInvariant()
        if ($hash -ne $odmInstallerSha256) { throw "ODM installer has SHA-256 $hash, expected $odmInstallerSha256" }
        $installArgs = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CURRENTUSER /NOICONS /DIR=`"$OdmDir`" /LOG=`"$(Join-Path $odmCache 'odm-install.log')`""
        $proc = Start-Process -FilePath $installer -ArgumentList $installArgs -Wait -PassThru
        if ($proc.ExitCode -ne 0) { throw "ODM installer failed (exit $($proc.ExitCode)), see $odmCache\odm-install.log" }
    }
    if (!(Test-Path (Join-Path $OdmDir 'run.py'))) {
        throw "OpenDroneMap not found in $OdmDir. It is part of GeoMap AI: run with -FetchOdm (233.5 MB download), " +
              "-OdmDir <installed ODM 3.6.x>, or -NoOdm for a developer build without processing."
    }
}

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
            if ($v -and $v -ge [version]'6.7') {
                $kits += [pscustomobject]@{ Path = $c.FullName; Version = $v; Msvc = $c.Name }
            }
        }
    }
    $best = $kits | Sort-Object Version, Msvc -Descending | Select-Object -First 1
    if (!$best) { throw 'No Qt 6.7+ MSVC x64 kit found. Install Qt (MSVC 2022 64-bit, with Qt Shader Tools) or pass -QtDir.' }
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
    # -NoAi points CMake at a folder without ONNX Runtime: the build has no local AI.
    $ortArg = if ($NoAi) { "-DGEOMAP_ONNXRUNTIME_DIR=$root/.deps/no-onnxruntime" } else { "-DGEOMAP_ONNXRUNTIME_DIR=$ortDir" }
    cmake -S . -B build -G $Generator -A x64 "-DCMAKE_PREFIX_PATH=$QtDir" "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DCMAKE_GENERATOR_INSTANCE=$vsPath" $ortArg
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

    # Licences of what is distributed (docs\DEPENDENCIES.md, "Обязанности при публикации"):
    # LGPL-3.0 and GPL-3.0 for Qt, the SBOM of the Qt build (its third-party components), the
    # copyright files of GDAL, PROJ and their libraries, and the notices of the whole delivery.
    $licenses = Join-Path $out 'licenses'
    New-Item -ItemType Directory -Force -Path $licenses | Out-Null
    Copy-Item (Join-Path $root 'installer\licenses\*.txt') $licenses -Force
    Copy-Item (Join-Path $root 'installer\THIRD_PARTY_NOTICES.txt') $out -Force
    $qtSbom = Join-Path $QtDir 'sbom'
    if (Test-Path $qtSbom) {
        New-Item -ItemType Directory -Force -Path (Join-Path $licenses 'qt') | Out-Null
        Copy-Item (Join-Path $qtSbom '*.spdx.json') (Join-Path $licenses 'qt') -Force
    } else {
        Write-Warning "Qt SBOM not found in ${qtSbom}: licenses\qt stays empty."
    }
    foreach ($package in 'gdal', 'proj', 'libgeotiff', 'tiff', 'libjpeg-turbo', 'libpng', 'zlib', 'liblzma', 'sqlite3', 'expat', 'json-c') {
        $copyright = Join-Path $gis "share\$package\copyright"
        if (!(Test-Path $copyright)) { throw "Licence of $package missing: $copyright" }
        New-Item -ItemType Directory -Force -Path (Join-Path $licenses $package) | Out-Null
        Copy-Item $copyright (Join-Path $licenses $package) -Force
    }

    # Local AI: ONNX Runtime (MIT) with its notices, and every model folder (card, licence,
    # weights) whose weights match the card's SHA-256. Without them the AI tools explain why.
    if (!$NoAi) {
        if (!(Test-Path (Join-Path $ortDir 'lib\onnxruntime.dll'))) {
            throw "ONNX Runtime not found in $ortDir. Run with -FetchAi (78.8 MB download) or -NoAi."
        }
        Copy-Item (Join-Path $ortDir 'lib\onnxruntime*.dll') $out -Force
        $ortLicenses = Join-Path $out 'licenses\onnxruntime'
        New-Item -ItemType Directory -Force -Path $ortLicenses | Out-Null
        foreach ($file in 'LICENSE', 'ThirdPartyNotices.txt', 'VERSION_NUMBER') {
            if (Test-Path (Join-Path $ortDir $file)) { Copy-Item (Join-Path $ortDir $file) $ortLicenses -Force }
        }
        foreach ($card in Get-ChildItem (Join-Path $root 'models\*\model.json')) {
            $id = Split-Path (Split-Path $card) -Leaf
            $target = Join-Path $out "models\$id"
            New-Item -ItemType Directory -Force -Path $target | Out-Null
            Copy-Item (Join-Path (Split-Path $card) '*') $target -Force
            foreach ($f in (Get-Content $card -Raw -Encoding UTF8 | ConvertFrom-Json).files) {
                $weights = Join-Path $weightsDir "$id\$($f.name)"
                if (!(Test-Path $weights)) { throw "Weights of model $id missing: $weights (run with -FetchAi)" }
                $hash = (Get-FileHash -Algorithm SHA256 $weights).Hash.ToLowerInvariant()
                if ($hash -ne $f.sha256) { throw "$weights has SHA-256 $hash, expected $($f.sha256)" }
                Copy-Item $weights $target -Force
            }
            Write-Host "AI model: $id"
        }
    }

    # Documentation for "Справка": docs\*.md plus README, CHANGELOG and the license.
    $docs = Join-Path $out 'docs'
    New-Item -ItemType Directory -Force -Path $docs | Out-Null
    # FEATURE_MATRIX.md is the developers' record of requirements, not for the program.
    Copy-Item (Join-Path $root 'docs\*.md') $docs -Exclude 'FEATURE_MATRIX.md' -Force
    foreach ($file in 'README.md', 'CHANGELOG.md', 'LICENSE') { Copy-Item (Join-Path $root $file) $docs -Force }

    if (!$NoOdm) {
        # Built-in OpenDroneMap (AGPL-3.0, a separate program): copied as installed, without the
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
            "A copy of the source archive is published with every GeoMap AI release: https://github.com/Lox123234/Ai-for-geomap/releases (ODM-$odmVersion-source.zip).",
            'OpenDroneMap is a separate program that GeoMap AI runs as an external process.')
        Write-Host "OpenDroneMap $odmVersion bundled: $target"
    }

    if ($Zip) {
        $cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
        if ($cmake -notmatch 'project\(GeoMapAI VERSION ([0-9.]+)') { throw 'Project version not found in CMakeLists.txt' }
        $suffix = if ($NoOdm) { '-no-odm' } else { '' }
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

    if ($Installer) {
        $makensis = Get-ChildItem (Join-Path $root '.tools') -Directory -Filter 'nsis-*' -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'makensis.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1
        if (!$makensis) { $makensis = (Get-Command makensis.exe -ErrorAction SilentlyContinue).Source }
        if (!$makensis) {
            $makensis = @("${env:ProgramFiles(x86)}\NSIS\makensis.exe", "$env:ProgramFiles\NSIS\makensis.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
        }
        if (!$makensis) { throw 'NSIS 3 (makensis.exe) not found: unpack nsis-3.x.zip into .tools\ or install NSIS.' }
        $cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
        if ($cmake -notmatch 'project\(GeoMapAI VERSION ([0-9.]+)') { throw 'Project version not found in CMakeLists.txt' }
        $version = $Matches[1]
        $suffix = if ($NoOdm) { '-no-odm' } else { '' }
        $setup = Join-Path (Split-Path -Parent $out) "GeoMapAI-$version-win64$suffix-setup.exe"
        # The license page reads UTF-16LE text (Cyrillic).
        $license = Join-Path $root 'build\installer-license.txt'
        [IO.File]::WriteAllText($license, [IO.File]::ReadAllText((Join-Path $root 'installer\license.txt'), [Text.Encoding]::UTF8), [Text.Encoding]::Unicode)
        Write-Host "Building the installer with $makensis (LZMA, a few minutes)..."
        & $makensis /V2 /INPUTCHARSET UTF8 "/DVERSION=$version" "/DSOURCE_DIR=$out" "/DOUTFILE=$setup" "/DROOT=$root" "/DLICENSE_FILE=$license" (Join-Path $root 'installer\GeoMapAI.nsi')
        if ($LASTEXITCODE -ne 0) { throw 'Building the installer failed' }
        Write-Host "Installer: $setup ($([Math]::Round((Get-Item $setup).Length / 1MB)) MB)" -ForegroundColor Green
    }

    Write-Host "Ready: $(Join-Path $out 'GeoMapAI.exe')" -ForegroundColor Green
} finally {
    Pop-Location
}
