<#
.SYNOPSIS
    Runs every GeoMap AI test and prints a summary: unit, UI and 3D tests (ctest), then, with
    -Dataset, a real reconstruction with the built-in OpenDroneMap and the checks of its results in
    the 3D view (GPU render) and in the main window.
.PARAMETER Dataset
    Folder of overlapping drone photos for the OpenDroneMap integration test (e.g. Brighton Beach).
.PARAMETER OdmHome
    OpenDroneMap folder to process with (e.g. the installed <app>\engines\odm). Default: the one
    next to the test executables or .odm\odm-3.6.2.
.PARAMETER Output
    Folder for screenshots and rendered images. Default: build\test-output.
.PARAMETER Build
    Build folder. Default: <repo>\build.
#>
param(
    [string]$Dataset,
    [string]$OdmHome,
    [string]$Output,
    [string]$Build
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$root = Split-Path -Parent $PSScriptRoot
if (!$Build) { $Build = Join-Path $root 'build' }
if (!$Output) { $Output = Join-Path $Build 'test-output' }
$bin = Join-Path $Build 'Release'
if (!(Test-Path (Join-Path $bin 'GeoMapAITests.exe'))) { throw "Тесты не собраны: $bin" }

# Qt DLLs for the test executables (the kit the build folder was configured with).
$cache = Get-Content (Join-Path $Build 'CMakeCache.txt')
$qt = ($cache | Where-Object { $_ -match '^CMAKE_PREFIX_PATH:[A-Z]+=(.+)$' } | ForEach-Object { $Matches[1] } | Select-Object -First 1)
if ($qt) { $env:PATH = "$(Join-Path $qt 'bin');$env:PATH" }
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:GEOMAP_TEST_OUTPUT = $Output
New-Item -ItemType Directory -Force -Path $Output | Out-Null

$results = New-Object System.Collections.Generic.List[object]
function Step([string]$title, [scriptblock]$body) {
    Write-Host ''
    Write-Host "=== $title ===" -ForegroundColor Cyan
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & $body
    $ok = $LASTEXITCODE -eq 0
    $results.Add([pscustomobject]@{ Шаг = $title; Итог = $(if ($ok) { 'ПРОЙДЕН' } else { 'ОШИБКА' }); Время = '{0:mm\:ss}' -f $timer.Elapsed })
}
function Run-Test([string]$exe, [string[]]$arguments = @()) {
    # Straight to the console: Qt Test then writes Unicode itself (through a pipe it would use the
    # ANSI code page and garble Cyrillic).
    & (Join-Path $bin $exe) @arguments
}
Step 'Модульные, UI- и 3D-тесты (ctest)' {
    ctest --test-dir $Build -C Release -LE integration --output-on-failure
}

if ($Dataset) {
    if ($OdmHome) { $env:GEOMAP_ODM_HOME = $OdmHome }
    $env:GEOMAP_ODM_DATASET = $Dataset
    $env:GEOMAP_ODM_KEEP = '1'
    # Work links and logs of the test, not the user's data folder.
    $env:GEOMAP_DATA_DIR = Join-Path $env:TEMP 'GeoMapAI-test-data'
    $env:QTEST_FUNCTION_TIMEOUT = '21600000'
    $started = Get-Date
    Step 'Реальная обработка встроенным OpenDroneMap (отмена, быстрый и полный профиль)' {
        Run-Test 'GeoMapAIOdmIntegrationTest.exe'
    }
    # The project the integration test kept, and its full-profile job (with a DTM).
    $kept = Get-ChildItem $env:TEMP -Directory -Filter 'GeoMap ODM *' | Where-Object { $_.CreationTime -ge $started.AddMinutes(-1) } |
        Sort-Object CreationTime -Descending | Select-Object -First 1
    $project = if ($kept) { Get-ChildItem $kept.FullName -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'geomap.json') } | Select-Object -First 1 }
    $job = if ($project) { Get-ChildItem (Join-Path $project.FullName 'jobs') -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'odm_dem\dtm.tif') } | Select-Object -First 1 }
    if ($job) {
        $env:GEOMAP_3D_JOB = $job.FullName
        $env:GEOMAP_UI_PROJECT = $project.FullName
        Write-Host "Проект с результатами: $($project.FullName)"
        Step 'Результаты ODM в 3D: загрузка, согласованность облака, модели и DSM' { Run-Test 'GeoMapAIView3DTests.exe' }
        Step 'Результаты ODM на видеокарте: облако, модель, DSM, DTM, вкладка 3D' { Run-Test 'GeoMapAIView3DRenderTest.exe' }
        Step 'Главное окно с настоящим проектом (снимки окна)' { $env:QT_QPA_PLATFORM = 'offscreen'; Run-Test 'GeoMapAIUiTests.exe'; Remove-Item Env:QT_QPA_PLATFORM }
    } else {
        $results.Add([pscustomobject]@{ Шаг = 'Проверка результатов ODM в 3D и в окне'; Итог = 'НЕ ЗАПУЩЕНА (нет проекта с DTM)'; Время = '' })
    }
}

Write-Host ''
Write-Host '=== Итог ===' -ForegroundColor Cyan
foreach ($r in $results) {
    $color = if ($r.Итог -eq 'ПРОЙДЕН') { 'Green' } else { 'Red' }
    Write-Host ("{0,-12} {1,6}  {2}" -f $r.Итог, $r.Время, $r.Шаг) -ForegroundColor $color
}
Write-Host "Снимки и изображения: $Output"
if ($results | Where-Object { $_.Итог -ne 'ПРОЙДЕН' }) { Write-Host 'Есть ошибки.' -ForegroundColor Red; exit 1 }
Write-Host 'Все шаги пройдены.' -ForegroundColor Green
