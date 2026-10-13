# GeoMap AI

**Open-source desktop workspace for UAV photography and geospatial projects.**

`v0.1.0` — Qt 6/QML desktop foundation, written in **C++17**, with no Python runtime, embedded website, accounts or subscription.

> This is **not** yet a photogrammetry processor. Orthomosaics, dense point clouds, SfM, ODM integration, 3D reconstruction and machine-learning inference are milestones for later versions. The GPS overview currently shows a scatter plot, **not a real basemap**.

## Available in v0.1

- Native application window with dark, resizable three-panel layout
- Create, open, save and close **local projects**
- Persist project metadata atomically in `geomap.json`
- Import images individually or recursively from a folder (`JPEG`, `PNG`, `TIFF`)
- Copy all imported photos into the project so projects can be moved between computers
- Read image dimensions and extract EXIF camera/date/GPS data from JPEG APP1 and TIFF IFD, without Python
- Browse images through a sidebar and filmstrip; preview and zoom
- Show GPS positions as a schematic scatter plot with a clearly labelled non-geographic projection
- Remove a project copy of an image without deleting the original file
- Recent projects and keyboard shortcuts: `Ctrl+N`, `Ctrl+O`, `Ctrl+S`, `Ctrl+I`, `Ctrl+W`

## Requirements

- Windows 10/11 (x64) or Linux desktop
- **Qt 6.5+** with Qt Quick, Qt Quick Controls, Qt Quick Dialogs, Core, Gui; **Qt Image Formats** (TIFF support); Qt Test for tests
- **CMake 3.21+** (3.22+ to run the tests with `ctest` on Windows without adding Qt to `PATH`), C++17 compiler
- Windows: **Visual Studio 2022** (Community or Build Tools) with the C++ desktop workload (MSVC v143 x64, Windows SDK) and the Qt **MSVC 2022 64-bit** kit. MinGW Qt kits cannot be mixed with MSVC.
- Optional: Git, Qt Creator, Ninja

No Python interpreter or local web server is needed to build or run the desktop app.

Verified configuration: Windows 11 x64, Qt 6.8.3 `msvc2022_64`, Visual Studio 2022 Build Tools 17.14 (MSVC 14.44), Windows SDK 10.0.26100, CMake 4.4.3.

## Install dependencies on Windows

```powershell
winget install --id Kitware.CMake -e
winget install --id Git.Git -e
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Install Qt with the official [Qt Online Installer](https://www.qt.io/download-qt-installer-oss) (open-source edition; a Qt account is required). Select **Qt 6.x → MSVC 2022 64-bit** and, under *Additional Libraries*, **Qt Image Formats** (without it TIFF files are rejected on import).

## Build on Windows

From a regular PowerShell window in this repository:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1
```

The script:

1. finds the newest Qt 6.5+ `msvc20xx_64` kit (`QTDIR`, `Qt6_DIR`, `qtpaths` on `PATH`, `<drive>:\Qt`, `%USERPROFILE%\Qt`) — or pass `-QtDir "C:\Qt\6.8.3\msvc2022_64"`;
2. locates Visual Studio 2022 with `vswhere`, configures CMake (`Visual Studio 17 2022`, x64) and builds **Release**;
3. runs the tests with `ctest` (skip with `-SkipTests`);
4. recreates `dist\GeoMapAI`, runs `windeployqt` (Qt DLLs, QML modules, platform/image-format plugins) and copies the MSVC runtime DLLs next to the EXE.

Other options: `-Generator "Visual Studio 18 2026"` to use a newer Visual Studio instance.

Manual build (equivalent to the script without deployment):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:\Qt\6.8.3\msvc2022_64"
cmake --build build --config Release --parallel
```

For development, you can also open `CMakeLists.txt` in Qt Creator with the same Qt kit.

## Run GeoMap AI

```powershell
.\dist\GeoMapAI\GeoMapAI.exe
```

`dist\GeoMapAI` is a portable folder, not a single-file executable: keep the Qt DLLs, `qml\`, `platforms\`, `imageformats\` and other plugin folders next to the EXE. The folder can be copied to another location or another Windows 10/11 x64 PC; it does not need Qt, Qt Creator, Python or the VC++ Redistributable installed. To create an installer, add WiX or Inno Setup in a subsequent release.

Settings (recent projects) are stored per user in `HKEY_CURRENT_USER\Software\GeoMapAI\GeoMap AI`.

## Build on Linux

Install Qt 6 development packages matching your distribution, including Qt Quick Controls 2 and the QML runtime modules, plus CMake and a C++17 compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/GeoMapAI
```

Depending on the distribution, `GeoMapAI` may reside in the build directory or a configuration subdirectory.

## Run tests

Tests build when `Qt6::Test` is installed (`BUILD_TESTING` is on by default). On Windows, after the build:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

On Linux/single-config generators omit `-C Release`. The tests use temporary directories only and keep their settings out of the user profile.

- `GeoMapAITests` (`tests/test_exif.cpp`): EXIF GPS, camera and date from JPEG; PNG/TIFF dimensions; Cyrillic paths; missing, empty, garbage and truncated files; malformed EXIF (out-of-range offsets, IFD loops, huge counts, zero denominators, invalid coordinates).
- `GeoMapAIProjectTests` (`tests/test_project.cpp`): create/save/close/reopen; empty projects; JPEG/PNG/TIFF import with Cyrillic names; duplicate import; relative manifest paths and moving a project to another folder; removing photos keeps originals (including read-only ones); corrupt images and corrupt or unsafe `geomap.json`; recursive folder import that skips the project's own copies; `importFiles` called from JavaScript with a `list<url>` like `FileDialog.selectedFiles`.

## Project format

```text
My Survey/
  geomap.json              # portable JSON manifest (schemaVersion: 1)
  photos/
    51dc...e3.jpg          # imported copy
    c42a...90.tif
```

Images are *copied*, not linked. The project can be moved or backed up as a folder. The manifest is written with `QSaveFile` for atomic replacement. Source image paths are stored only for informational/deduplication purposes; originals are not required to reopen a project.

## Planned roadmap

| Version | Focus |
| --- | --- |
| 0.1 | Offline projects, image import, EXIF/GPS, desktop UI |
| 0.2 | Interchangeable processing engine, local OpenDroneMap integration, processing progress |
| 0.3 | Orthophoto/DEM GIS workspace, measurement, layers, real CRS support |
| 0.4 | Local segmentation/inference (ONNX), editing results |
| 1.0 | Reliable desktop distribution, CLI/batch processing, full documentation |

## Limitations and known tradeoffs

- Import copies run on the UI thread in this first build. Very large datasets can briefly freeze the interface. A threaded import queue is a high-priority next step.
- JPEG and standard TIFF EXIF GPS are supported; proprietary maker notes, XMP sidecars, and advanced TIFF GPS variants are not yet covered. PNG EXIF metadata is not read. EXIF is only searched in the first 8 MiB of a file.
- Image previews depend on Qt image-format plugins (the Windows deployment script deploys the relevant plugins). TIFF needs the Qt Image Formats module.
- Large images may require additional thumbnail caching for fluid scrolling.
- The GPS scatter plot preserves relative ordering of coordinates but does not preserve geographic distance or aspect ratio; no map tiles are downloaded.
- No photogrammetry, AI inference, terrain rendering or GCP support is present in this version.
- Recent projects that were moved or deleted stay in the list; opening one reports that `geomap.json` is missing.
- On screens where the 1500×900 window does not fit (e.g. 1920×1080 at 125 % scaling) the window starts maximized; the inspector scrolls when its details do not fit.
- There is no installer, code signing or automatic update yet; Windows SmartScreen may warn about the unsigned EXE.
- The Windows build was verified on Qt 6.8.3; Linux and macOS builds were not re-tested for this release.

## License

Application source: **MIT**, see [LICENSE](LICENSE). The Qt framework has its own licensing terms (e.g., LGPL/GPL/commercial, subject to Qt module and distribution conditions). Future third-party processing components will retain their respective licenses; those do not automatically become MIT simply by being used with GeoMap AI.
