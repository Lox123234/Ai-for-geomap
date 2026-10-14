# Сборка GeoMap AI

## Требования

| Компонент | Windows | Linux |
| --- | --- | --- |
| Компилятор C++17 | Visual Studio 2022 (Community или Build Tools), MSVC v143 x64, Windows SDK | GCC 11+ или Clang 14+ |
| CMake | 3.21+ (3.22+ для запуска тестов через `ctest` без Qt в `PATH`) | 3.21+ |
| Qt | 6.5+, комплект **MSVC 2022 64-bit**; модули Quick, Quick Controls, Quick Dialogs, Test; дополнение **Qt Image Formats** (TIFF) | 6.5+ из пакетов дистрибутива или онлайн-установщика |
| Генератор | Visual Studio 17 2022 | Ninja |
| GDAL ≥ 3.8, PROJ ≥ 9 | vcpkg (манифест `vcpkg.json`): копия из Visual Studio (`VC\vcpkg`, компонент «vcpkg package manager») или своя с `VCPKG_ROOT` | `libgdal-dev`, `libproj-dev` |

Проверенная конфигурация: Windows 11 x64, Qt 6.8.3 `msvc2022_64`, VS 2022 Build Tools 17.14 (MSVC 14.44), Windows SDK 10.0.26100, CMake 4.4.3, vcpkg из Visual Studio 2026 Community (baseline манифеста — тег 2026.07.29), GDAL 3.12.4, PROJ 9.8.1.

**Первая сборка на Windows** собирает GDAL, PROJ и их библиотеки из исходников (18 пакетов, около 12 минут на 16 потоках; загрузка исходников ~100 МБ). Результат кэшируется vcpkg (`%LOCALAPPDATA%\vcpkg\archives`), следующие конфигурации берут готовые пакеты. Триплет `x64-windows-geomap` (папка `triplets/`) собирает только Release-DLL тем же набором инструментов MSVC v143, что и приложение, — поставляемый рантайм VC++ подходит и к ним.

Комплекты Qt для MinGW нельзя смешивать с MSVC: скрипт сборки принимает только `msvc20xx_64`.

## Установка зависимостей

### Windows

```powershell
winget install --id Kitware.CMake -e
winget install --id Git.Git -e
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Qt — официальным [онлайн-установщиком](https://www.qt.io/download-qt-installer-oss) (open-source, нужна учётная запись Qt): **Qt 6.x → MSVC 2022 64-bit** и в *Additional Libraries* — **Qt Image Formats**.

### Linux (Ubuntu 24.04+)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build \
  qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-dialogs qml6-module-qtquick-templates qml6-module-qtquick-window \
  qml6-module-qtqml-workerscript qt6-image-formats-plugins libgl1-mesa-dev \
  libgdal-dev libproj-dev
```

## Сборка на Windows

Обычная PowerShell в корне репозитория:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1
```

Скрипт:

1. находит самый новый комплект Qt 6.5+ `msvc20xx_64` (`QTDIR`, `Qt6_DIR`, `qtpaths` в `PATH`, `<диск>:\Qt`, `%USERPROFILE%\Qt`);
2. находит Visual Studio 2022 через `vswhere` и vcpkg (`VCPKG_ROOT`, `VCPKG_INSTALLATION_ROOT` или `VC\vcpkg` любой установленной Visual Studio), конфигурирует CMake (`Visual Studio 17 2022`, x64, toolchain vcpkg) в `build/` и собирает Release; vcpkg при этом собирает или берёт из кэша GDAL и PROJ;
3. запускает тесты (`ctest`);
4. пересоздаёт папку развёртывания, запускает `windeployqt` (DLL Qt, QML-модули, плагины), копирует рантайм MSVC, DLL GDAL/PROJ и их данные (`share\proj\proj.db`, `share\gdal`) рядом с EXE.

| Параметр | Назначение |
| --- | --- |
| `-QtDir C:\Qt\6.8.3\msvc2022_64` | Явный комплект Qt |
| `-SkipTests` | Не запускать тесты |
| `-OutDir <папка>` | Папка развёртывания (по умолчанию `dist\GeoMapAI`). Скрипт отказывается очищать папку, которая не похожа на развёртывание GeoMap AI |
| `-Zip` | Дополнительно создать `GeoMapAI-<версия>-win64.zip` рядом с папкой |
| `-Generator "Visual Studio 18 2026"` | Другой экземпляр Visual Studio |

Результат: `dist\GeoMapAI\GeoMapAI.exe` — переносимая папка (около 110 МБ, из них ~37 МБ — GDAL/PROJ и `proj.db`). Её можно скопировать на другой ПК с Windows 10/11 x64; Qt, Qt Creator, Python, GDAL, PROJ и VC++ Redistributable там не нужны.

## CMake-пресеты

```powershell
$env:QT_ROOT_DIR = 'C:\Qt\6.8.3\msvc2022_64'
$env:VCPKG_ROOT = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg'   # или своя копия vcpkg
cmake --preset windows-msvc
cmake --build --preset windows-release
ctest --preset windows-release
```

```bash
cmake --preset linux-release          # системный Qt или QT_ROOT_DIR
cmake --build --preset linux-release
ctest --preset linux-release          # QT_QPA_PLATFORM=offscreen
```

Пресеты не выполняют развёртывание; для переносимой папки используйте скрипт.

Интеграционный тест с настоящим OpenDroneMap включается параметром CMake `-DGEOMAP_ODM_DATASET=<папка со снимками>` (нужны Docker и образ ODM; см. `docs/TESTING.md`). Docker для сборки не нужен.

## CI

`.github/workflows/ci.yml` собирает и тестирует проект на Windows (MSVC, через `build-windows.ps1`) и Linux (GCC, через пресеты, GDAL/PROJ из пакетов Ubuntu) для Qt 6.8.3. На Windows vcpkg раннера переводится на baseline манифеста, собранные пакеты хранятся в кэше Actions (ключ — хэш `vcpkg.json`, `vcpkg-configuration.json` и `triplets/`): холодная сборка GDAL занимает ~20 минут, повторная — минуты. Архив Windows-сборки выкладывается артефактом только при ручном запуске (`workflow_dispatch`). Интеграционный тест с ODM в CI не запускается.

## Параметры запуска приложения

| Параметр | Назначение |
| --- | --- |
| `--settings-dir <папка>` | Хранить настройки (недавние проекты) в `<папка>/GeoMapAI/GeoMap AI.ini` вместо реестра. Удобно для переносимого режима и тестов |
| `--help`, `--version` | Справка и версия |
| переменная `GEOMAP_DOCKER` | Полный путь к клиенту `docker`, если он не в `PATH` и не в стандартной папке Docker Desktop |

## Типичные проблемы

| Симптом | Причина и решение |
| --- | --- |
| Тест падает с `0xc0000135` | Не найдены DLL Qt. CMake 3.22+ добавляет `<Qt>\bin` в `PATH` тестов автоматически; на 3.21 добавьте вручную |
| Окно не появляется, процесс сразу завершается | Ошибка загрузки QML. Запустите с `QT_FORCE_STDERR_LOGGING=1` и перенаправьте stderr в файл |
| TIFF не импортируется | Не установлен модуль Qt Image Formats (нет `imageformats\qtiff.dll`) |
| `Qt kit must be an MSVC 64-bit kit` | Указан комплект MinGW или 32-битный |
| `Refusing to clear …` | `-OutDir` указывает на папку, которая не является развёртыванием GeoMap AI |
| `vcpkg not found` | Нет ни `VCPKG_ROOT`, ни vcpkg в Visual Studio. Добавьте компонент «vcpkg package manager» в установщике Visual Studio или клонируйте https://github.com/microsoft/vcpkg и задайте `VCPKG_ROOT` |
| Первая конфигурация идёт 10–20 минут | vcpkg собирает GDAL и PROJ; это однократно, дальше используется кэш |
| На карте «proj.db не найден» / CRS не распознаются | В папке программы нет `share\proj\proj.db`: поставка собрана не скриптом. Скопируйте `build\vcpkg_installed\x64-windows-geomap\share\proj` в `<папка программы>\share\proj` |
