# GeoMap AI

**Open-source desktop workspace for UAV photography and geospatial projects.**

`v0.2.1` — Qt 6/QML desktop application written in **C++17**, with no Python runtime, embedded website, accounts or subscription. Photos and projects stay on your computer.

> Reconstruction (orthophoto, point cloud, elevation models, textured model) is done by **OpenDroneMap** running locally as a separate program — on Windows its official build runs **without Docker or WSL** (bundled with GeoMap AI or installed by it in one click); Docker is optional. Projects, import and browsing work without OpenDroneMap. Map/GIS viewing of the results, 3D viewing and machine learning are milestones for later versions (see [docs/ROADMAP.md](docs/ROADMAP.md)). The GPS overview shows a scatter plot, **not a real basemap**.

## Available in v0.2.1

- Native application window with a dark three-panel layout (Russian interface)
- Create, open, save and close **local projects**; recent projects
- Portable projects: imported photos are **copied** into the project, paths are relative, the manifest `geomap.json` is written atomically
- Import JPEG, PNG and TIFF files or whole folders (recursively) **in the background**: per-file progress, cancel, photos appear while importing, a report of skipped files with reasons, free-space check
- EXIF camera, date and GPS from JPEG APP1 and TIFF IFD, without Python
- Thumbnail cache for fast browsing of hundreds of photos; preview with zoom
- GPS positions as a schematic scatter plot with a clearly labelled non-geographic projection
- Remove a project copy of an image without touching the original file
- A project open in one window is locked against writes from a second window
- Keyboard shortcuts: `Ctrl+N`, `Ctrl+O`, `Ctrl+S`, `Ctrl+I`, `Ctrl+W`
- **Processing** with OpenDroneMap 3.6.2: built-in Windows runtime (no Docker/WSL, no admin rights) or Docker; four profiles including alignment-only for stepwise work, ~30 stage settings with validation (SIFT on NVIDIA GPUs), pre-flight check, job queue, real engine stages and live log, cancel and resume, job history kept in the project, verified results (GeoTIFF CRS and pixel size, LAS point count, report) and a quality report from ODM's statistics (aligned images, reprojection error, GPS RMSE/CE90/LE90, GSD)

## Requirements

- Windows 10/11 x64 (verified) or Linux x64 (build configuration provided, not yet verified)
- To build: CMake 3.21+, a C++17 compiler (Visual Studio 2022 on Windows), Qt 6.5+ with Qt Quick, Quick Controls, Quick Dialogs and Qt Image Formats

No Python interpreter or local web server is needed to build or run the desktop app.

## Build and run on Windows

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1
.\dist\GeoMapAI\GeoMapAI.exe
```

The script finds Qt and Visual Studio, builds Release x64, runs the tests and deploys a portable folder `dist\GeoMapAI` (Qt libraries, QML modules, plugins and the MSVC runtime next to the EXE). Copy the whole folder to run it on another Windows 10/11 x64 PC; Qt, Qt Creator, Python and the VC++ Redistributable are not required there. Add `-Zip` to also produce `dist\GeoMapAI-<version>-win64.zip`.

Dependencies, CMake presets, Linux, CI and troubleshooting: [docs/BUILD.md](docs/BUILD.md).

Settings (recent projects) are stored per user in `HKEY_CURRENT_USER\Software\GeoMapAI\GeoMap AI`, or in an INI file when started with `--settings-dir <folder>`.

## Processing photos with OpenDroneMap

1. Open a project with overlapping drone photos and switch to the **◈ Обработка** (Processing) tab. The environment check runs automatically.
2. If OpenDroneMap is not there yet, click **Установить OpenDroneMap**: GeoMap AI downloads the official signed Windows installer (233.5 MB, SHA-256 verified) and installs it for the current user — no Docker, no WSL, no administrator rights; about 1 GB on disk. The full package `GeoMapAI-<version>-win64-odm.zip` already contains it.
3. Choose a profile — **Только выравнивание** (alignment only), **Быстрый просмотр**, **Сбалансированный** or **Высокое качество** — optionally adjust **Параметры обработки**, read the pre-flight summary and click **Запустить обработку**.
4. Follow the stage and the live log; when the job finishes, see **Результаты** (files with CRS and size) and **Качество** (aligned images, reprojection error, GPS errors, GSD). An alignment-only, cancelled or interrupted job can be continued with **Продолжить**.

Each job lives in its own folder `jobs/<id>/` inside the project, so later runs never overwrite earlier results. Runtimes, profiles, settings and troubleshooting: [docs/PROCESSING_ODM.md](docs/PROCESSING_ODM.md).
## Tests

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Four test executables cover EXIF parsing and corrupt files, projects and their format, background import, cancellation, thumbnails, project locking, and processing jobs with a scriptable engine (success, failure, cancel, queue, restart). An optional fifth runs a real OpenDroneMap reconstruction. Details and the manual checklist: [docs/TESTING.md](docs/TESTING.md).

## Documentation

| Document | Contents |
| --- | --- |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Stages A–G with checklists |
| [docs/AUDIT.md](docs/AUDIT.md) | Audit of 0.1.0: what worked, what was fixed, risks |
| [docs/BUILD.md](docs/BUILD.md) | Build, dependencies, presets, CI |
| [docs/TESTING.md](docs/TESTING.md) | Automated tests, acceptance results, manual checklist |
| [docs/PROJECT_FORMAT.md](docs/PROJECT_FORMAT.md) | Project folder, `geomap.json`, jobs, cache, lock |
| [docs/PROCESSING_ODM.md](docs/PROCESSING_ODM.md) | OpenDroneMap processing: requirements, profiles, results, troubleshooting |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Modules, threads, invariants |
| [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) | Third-party components and licenses |
| [CHANGELOG.md](CHANGELOG.md) | Changes per version |

## Limitations and known tradeoffs

- JPEG and standard TIFF EXIF GPS are supported; proprietary maker notes, XMP sidecars, and advanced TIFF GPS variants are not yet covered. PNG EXIF metadata is not read. EXIF is only searched in the first 8 MiB of a file.
- The full-size preview decodes the whole image (a 20 MP photo needs ~80 MB of memory); tiled viewing of large rasters is planned with the GIS stage.
- The GPS scatter plot preserves relative ordering of coordinates but not geographic distance or aspect ratio; no map tiles are downloaded.
- Results open in external programs (e.g. QGIS for GeoTIFF/LAZ); built-in map and 3D viewers come with stages C and D. Only the "Fast preview" profile has been verified on real data so far. AI inference and GCP support are not present yet.
- Processing on Windows needs about 1 GB for OpenDroneMap (built-in runtime); on Linux it needs Docker Engine and about 2.4 GB for the ODM image. In our measurements the Windows build of ODM extracts features ~3x slower than ODM in Docker, while whole small jobs took about the same time. Reconstruction time and quality depend on photo overlap and quality, GPS/GCP and the computer; they are not guaranteed, and GeoMap AI does not claim parity with commercial suites such as Agisoft Metashape (no manual GCP marking yet).
- Recent projects that were moved or deleted stay in the list; opening one reports that `geomap.json` is missing.
- There is no installer, code signing or automatic update yet; Windows SmartScreen may warn about the unsigned EXE. The portable folder does not yet bundle the third-party license texts required for redistribution (see [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)), so it should not be published as is.
- Linux build files are provided but the Linux build has not been verified yet.

## License

Application source: **MIT**, see [LICENSE](LICENSE). Qt and the other bundled components keep their own licenses (Qt modules used here are available under LGPL-3.0); see [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md). OpenDroneMap (AGPL-3.0) is a separate program: GeoMap AI runs it as an external process, and the full package ships it unmodified with its license and a pointer to its source code. It keeps its license and does not become MIT by being distributed with GeoMap AI.
