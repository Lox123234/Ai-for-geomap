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
- **Qt 6.5+** (Qt Quick, Qt Quick Controls, Core, Gui; Qt Test for tests)
- **CMake 3.21+**, C++17 compiler
- Windows: **Visual Studio 2022 C++ workload**, **Qt for MSVC 2022 64-bit**

No Python interpreter or local web server is needed to run the desktop app.

## Build on Windows

1. Install Visual Studio 2022 Community with "Desktop development with C++", CMake, and Qt Online Installer (Qt **MSVC 2022 64-bit** kit).
2. Open **Developer PowerShell for VS 2022** in this repository.
3. Run (change the path for your installed Qt version):

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1 -QtDir "C:\Qt\6.8.3\msvc2022_64"
```

The script builds and deploys the Qt runtime and QML plugins next to:

`dist\GeoMapAI\GeoMapAI.exe`

The program is a portable folder, not a single-file executable. Keep the deployed Qt DLLs and plugins next to the EXE. To create an installer, add WiX or Inno Setup in a subsequent release.

For development, you can also open `CMakeLists.txt` in Qt Creator with the same Qt kit.

## Build on Linux

Install Qt 6 development packages matching your distribution, including Qt Quick Controls 2 and the QML runtime modules, plus CMake and a C++17 compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/GeoMapAI
```

Depending on the distribution, `GeoMapAI` may reside in the build directory or a configuration subdirectory.

## Run tests

Tests build when `Qt6::Test` is installed:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The test suite checks synthetic JPEG EXIF GPS coordinates, basic PNG dimensions and missing-file handling.

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
- JPEG and standard TIFF EXIF GPS are supported; proprietary maker notes, XMP sidecars, and advanced TIFF GPS variants are not yet covered. PNG EXIF metadata may not be recognized.
- Image previews depend on Qt image-format plugins (the Windows deployment script deploys the relevant plugins).
- Large images may require additional thumbnail caching for fluid scrolling.
- The GPS scatter plot preserves relative ordering of coordinates but does not preserve geographic distance or aspect ratio; no map tiles are downloaded.
- No photogrammetry, AI inference, terrain rendering or GCP support is present in this version.

## License

Application source: **MIT**, see [LICENSE](LICENSE). The Qt framework has its own licensing terms (e.g., LGPL/GPL/commercial, subject to Qt module and distribution conditions). Future third-party processing components will retain their respective licenses; those do not automatically become MIT simply by being used with GeoMap AI.
