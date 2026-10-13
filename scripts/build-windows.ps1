param(
    [Parameter(Mandatory=$true)]
    [string]$QtDir
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$QtDir = (Resolve-Path $QtDir).Path
$QtBin = Join-Path $QtDir 'bin'
$Deploy = Join-Path $QtBin 'windeployqt.exe'
if (!(Test-Path $Deploy)) { throw "windeployqt not found in $QtBin" }

Push-Location $root
try {
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PREFIX_PATH=$QtDir"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    cmake --build build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    $exe = Join-Path $root 'build/Release/GeoMapAI.exe'
    if (!(Test-Path $exe)) { throw "Executable missing: $exe" }
    $out = Join-Path $root 'dist/GeoMapAI'
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    Copy-Item $exe (Join-Path $out 'GeoMapAI.exe') -Force
    & $Deploy --release --qmldir (Join-Path $root 'qml') --dir $out (Join-Path $out 'GeoMapAI.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed' }
    Write-Host "Ready: $out/GeoMapAI.exe" -ForegroundColor Green
} finally {
    Pop-Location
}
