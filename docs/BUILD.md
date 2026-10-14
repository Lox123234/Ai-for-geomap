# Сборка GeoMap AI

## Требования

| Компонент | Windows | Linux |
| --- | --- | --- |
| Компилятор C++17 | Visual Studio 2022 (Community или Build Tools), MSVC v143 x64, Windows SDK | GCC 11+ или Clang 14+ |
| CMake | 3.21+ (3.22+ для запуска тестов через `ctest` без Qt в `PATH`) | 3.21+ |
| Qt | 6.5+, комплект **MSVC 2022 64-bit**; модули Quick, Quick Controls, Quick Dialogs, Test; дополнение **Qt Image Formats** (TIFF) | 6.5+ из пакетов дистрибутива или онлайн-установщика |
| Генератор | Visual Studio 17 2022 | Ninja |

Проверенная конфигурация: Windows 11 x64, Qt 6.8.3 `msvc2022_64`, VS 2022 Build Tools 17.14 (MSVC 14.44), Windows SDK 10.0.26100, CMake 4.4.3.

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
  qml6-module-qtqml-workerscript qt6-image-formats-plugins libgl1-mesa-dev
```

## Сборка на Windows

Обычная PowerShell в корне репозитория:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1
```

Скрипт:

1. находит самый новый комплект Qt 6.5+ `msvc20xx_64` (`QTDIR`, `Qt6_DIR`, `qtpaths` в `PATH`, `<диск>:\Qt`, `%USERPROFILE%\Qt`);
2. находит Visual Studio 2022 через `vswhere`, конфигурирует CMake (`Visual Studio 17 2022`, x64) в `build/` и собирает Release;
3. запускает тесты (`ctest`);
4. пересоздаёт папку развёртывания, запускает `windeployqt` (DLL Qt, QML-модули, плагины) и копирует рантайм MSVC рядом с EXE.

| Параметр | Назначение |
| --- | --- |
| `-QtDir C:\Qt\6.8.3\msvc2022_64` | Явный комплект Qt |
| `-SkipTests` | Не запускать тесты |
| `-OutDir <папка>` | Папка развёртывания (по умолчанию `dist\GeoMapAI`). Скрипт отказывается очищать папку, которая не похожа на развёртывание GeoMap AI |
| `-Zip` | Дополнительно создать `GeoMapAI-<версия>-win64.zip` рядом с папкой |
| `-Generator "Visual Studio 18 2026"` | Другой экземпляр Visual Studio |

Результат: `dist\GeoMapAI\GeoMapAI.exe` — переносимая папка (около 70 МБ, архив около 27 МБ). Её можно скопировать на другой ПК с Windows 10/11 x64; Qt, Qt Creator, Python и VC++ Redistributable там не нужны.

## CMake-пресеты

```powershell
$env:QT_ROOT_DIR = 'C:\Qt\6.8.3\msvc2022_64'
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

`.github/workflows/ci.yml` собирает и тестирует проект на Windows (MSVC, через `build-windows.ps1`) и Linux (GCC, через пресеты) для Qt 6.8.3. Архив Windows-сборки выкладывается артефактом только при ручном запуске (`workflow_dispatch`). Интеграционный тест с ODM в CI не запускается.

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
