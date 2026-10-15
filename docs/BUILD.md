# Сборка GeoMap AI

## Требования

| Компонент | Windows | Linux |
| --- | --- | --- |
| Компилятор C++17 | Visual Studio 2022 (Community или Build Tools), MSVC v143 x64, Windows SDK | GCC 11+ или Clang 14+ |
| CMake | 3.21+ (3.22+ для запуска тестов через `ctest` без Qt в `PATH`) | 3.21+ |
| Qt | 6.7+ (проверено 6.8.3), комплект **MSVC 2022 64-bit**; модули Quick, Quick Controls, Quick Dialogs, Test; дополнения **Qt Image Formats** (TIFF) и **Qt Shader Tools** (шейдеры 3D-вида, только для сборки) | 6.7+ (`QQuickRhiItem`): онлайн-установщик или пакеты дистрибутива с Qt 6.7+ |
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

Qt — официальным [онлайн-установщиком](https://www.qt.io/download-qt-installer-oss) (open-source, нужна учётная запись Qt): **Qt 6.x → MSVC 2022 64-bit**, в *Additional Libraries* — **Qt Image Formats** и **Qt Shader Tools**.

### Linux

3D-вид требует Qt 6.7+: в Ubuntu 24.04 пакеты Qt — 6.4, поэтому там Qt ставится онлайн-установщиком или aqtinstall (как в CI), а пакеты ниже нужны для остального. В дистрибутивах с Qt 6.7+ в пакетах добавьте `qt6-shadertools-dev`.

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
4. пересоздаёт папку развёртывания, запускает `windeployqt` (DLL Qt, QML-модули, плагины), копирует рантайм MSVC, DLL GDAL/PROJ и их данные (`share\proj\proj.db`, `share\gdal`), документацию (`docs`) и **встроенный OpenDroneMap** (`engines\odm`).

| Параметр | Назначение |
| --- | --- |
| `-QtDir C:\Qt\6.8.3\msvc2022_64` | Явный комплект Qt |
| `-SkipTests` | Не запускать тесты |
| `-OutDir <папка>` | Папка развёртывания (по умолчанию `dist\GeoMapAI`). Скрипт отказывается очищать папку, которая не похожа на развёртывание GeoMap AI |
| `-Zip` | Дополнительно создать `GeoMapAI-<версия>-win64.zip` рядом с папкой |
| `-FetchOdm` | Один раз подготовить `.odm\odm-3.6.2`: загрузить официальный установщик ODM 3.6.2 (233,5 МБ, проверка SHA-256) и тихо установить в эту папку для текущего пользователя |
| `-OdmInstaller <файл>` | Локальный `ODM_Setup_3.6.2.exe` для `-FetchOdm` вместо загрузки |
| `-OdmDir <папка>` | Другая папка ODM 3.6.x вместо `.odm\odm-3.6.2` |
| `-NoOdm` | Сборка разработчика без ODM (всё, кроме обработки); так собирает CI |
| `-Installer` | Дополнительно собрать установщик `dist\GeoMapAI-<версия>-win64-setup.exe` (NSIS 3: `.tools\nsis-*`, `PATH` или Program Files) |
| `-Generator "Visual Studio 18 2026"` | Другой экземпляр Visual Studio |

Результат: `dist\GeoMapAI\GeoMapAI.exe` — переносимая папка: около 110 МБ без ODM (из них ~37 МБ — GDAL/PROJ и `proj.db`) и около 1,1 ГБ с встроенным ODM (архив ~400 МБ). Без ODM скрипт не собирает поставку, если не указан `-NoOdm`. Её можно скопировать на другой ПК с Windows 10/11 x64 в папку с правом записи (не `Program Files`); Qt, Qt Creator, Python, GDAL, PROJ, Docker, WSL и VC++ Redistributable там не нужны.

### Установщик

`-Installer` собирает один файл `GeoMapAI-<версия>-win64-setup.exe` (≈ 400 МБ со встроенным ODM) по сценарию `installer\GeoMapAI.nsi`: мастер на русском (лицензии, состав, папка), установка **для текущего пользователя без прав администратора** в `%LOCALAPPDATA%\Programs\GeoMapAI`, ярлыки в меню «Пуск» и (по выбору) на рабочем столе, запись в «Параметры → Приложения» для удаления, предложение запустить программу. Обновление заменяет предыдущую версию целиком; установщик и программа удаления ждут закрытия GeoMap AI. Удаляется только папка программы — проекты, снимки, результаты и папка данных `%LOCALAPPDATA%\GeoMapAI` остаются. В непустую чужую папку установщик не ставит (программа удаляется вместе с папкой).

NSIS один раз: скачать официальный `nsis-3.x.zip` (https://nsis.sourceforge.io/Download), сверить контрольную сумму с SourceForge и распаковать в `.tools\`. Иконка программы и картинки мастера рисуются скриптами `scripts\make-icon.ps1` и `scripts\make-installer-art.ps1` (результаты лежат в репозитории).

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

Интеграционный тест с настоящим OpenDroneMap включается параметром CMake `-DGEOMAP_ODM_DATASET=<папка со снимками>`; ODM берётся из `GEOMAP_ODM_HOME` (см. `docs/TESTING.md`).

## CI

`.github/workflows/ci.yml` собирает и тестирует проект на Windows (MSVC, через `build-windows.ps1`) и Linux (GCC, через пресеты, GDAL/PROJ из пакетов Ubuntu) для Qt 6.8.3. На Windows vcpkg раннера переводится на baseline манифеста, собранные пакеты хранятся в кэше Actions (ключ — хэш `vcpkg.json`, `vcpkg-configuration.json` и `triplets/`): холодная сборка GDAL занимает ~20 минут, повторная — минуты. Архив Windows-сборки выкладывается артефактом только при ручном запуске (`workflow_dispatch`). Интеграционный тест с ODM в CI не запускается.

## Параметры запуска приложения

| Параметр | Назначение |
| --- | --- |
| `--settings-dir <папка>` | Хранить настройки (недавние проекты) в `<папка>/GeoMapAI/GeoMap AI.ini` вместо реестра. Удобно для переносимого режима и тестов |
| `--help`, `--version` | Справка и версия |

## Типичные проблемы

| Симптом | Причина и решение |
| --- | --- |
| Тест падает с `0xc0000135` | Не найдены DLL Qt. CMake 3.22+ добавляет `<Qt>\bin` в `PATH` тестов автоматически; на 3.21 добавьте вручную |
| Окно не появляется, процесс сразу завершается | Ошибка загрузки QML. Запустите с `QT_FORCE_STDERR_LOGGING=1` и перенаправьте stderr в файл |
| TIFF не импортируется | Не установлен модуль Qt Image Formats (нет `imageformats\qtiff.dll`) |
| `Could not find a package configuration file provided by "Qt6ShaderTools"` | Не установлен Qt Shader Tools (онлайн-установщик → *Additional Libraries*) |
| Вкладка «3D-сцена» пишет, что нужна аппаратная графика | Qt Quick запущен с программной отрисовкой (`QT_QUICK_BACKEND=software` или нет драйвера): уберите переменную, обновите драйвер видеокарты |
| `Qt kit must be an MSVC 64-bit kit` | Указан комплект MinGW или 32-битный |
| `Refusing to clear …` | `-OutDir` указывает на папку, которая не является развёртыванием GeoMap AI |
| `vcpkg not found` | Нет ни `VCPKG_ROOT`, ни vcpkg в Visual Studio. Добавьте компонент «vcpkg package manager» в установщике Visual Studio или клонируйте https://github.com/microsoft/vcpkg и задайте `VCPKG_ROOT` |
| Первая конфигурация идёт 10–20 минут | vcpkg собирает GDAL и PROJ; это однократно, дальше используется кэш |
| На карте «proj.db не найден» / CRS не распознаются | В папке программы нет `share\proj\proj.db`: поставка собрана не скриптом. Скопируйте `build\vcpkg_installed\x64-windows-geomap\share\proj` в `<папка программы>\share\proj` |
