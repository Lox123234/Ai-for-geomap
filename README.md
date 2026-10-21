# GeoMap AI

**Open-source desktop workspace for UAV photography and geospatial projects.**

`v0.7.0` — Qt 6/QML desktop application written in **C++17**, with no Python runtime, embedded website, accounts or subscription. Photos and projects stay on your computer.

> Reconstruction (orthophoto, point cloud, elevation models, textured model) is done by **OpenDroneMap built into GeoMap AI**: its official Windows build ships unmodified inside the program folder (`engines\odm`, AGPL-3.0) and runs as a separate process — no Docker, no WSL, nothing to install separately, no administrator rights. The **map** (GDAL/PROJ, bundled) shows results and other geodata in a real coordinate system, the **3D view** shows point clouds, textured models and terrain on the GPU, and **local AI** (ONNX Runtime, in the program's own process) selects and removes objects. What it covers, feature by feature, is listed in [docs/FEATURE_MATRIX.md](docs/FEATURE_MATRIX.md).

## Available in v0.7.0

- Professional workspace (Russian interface): main menu and toolbar built from one list of 152 commands ([docs/UI_COMMANDS.md](docs/UI_COMMANDS.md)); project tree with chunks, results, layers and jobs; photo, map, 3D, split map + 3D and processing views; journal; saved layout
- Local, portable **projects**: photos copied into the project, relative paths, atomic manifest, background import of files and folders, EXIF/XMP (camera, time, GPS, DJI altitude, gimbal and drone attitude, RTK status and accuracy), thumbnail cache, photo masks (AI selection, rectangles, one mask on every photo), **image quality** (sharpness of each photo; blurred ones switched off in the chunk)
- **Processing** with the built-in OpenDroneMap 3.6.2: profiles from fast orthophoto to high quality (DSM, DTM, point cloud, textured model, report), pre-flight check, job queue, live stages and log, cancel, resume and automatic retry of a crashed stage, batch processing, overlap check, quality report; steps one by one (alignment, tie points, depth maps, 3D Tiles), camera optimisation with control points, GeoMap AI's own PDF report; works in folders with spaces and non-Latin names
- **Chunks**: split a large survey by a grid over the photo positions or by flights, process the parts separately, merge their orthophotos, DSM or DTM into one seamless mosaic
- **True orthophoto** of GeoMap AI built from the original photos and the dense point cloud: trees and roofs without smearing or "ghosts" (tested on a synthetic forest with a known answer)
- **Survey control**: the survey scheme (GPS against the alignment, footprints, flight path, RTK summary), ground control and check points marked on the photos, accuracy report (RMSE), camera calibrations, scale bars, markers
- **Georeferencing by links**: an image, a scanned map or a project photo beside the map, links by two clicks, affine/projective/polynomial fit with residuals, a GeoTIFF on the map; **LiDAR**: LAS/LAZ clouds in the project, in 3D, DSM and DTM from them
- **Map** (GDAL 3.12.4, PROJ 9.8.1): rasters and vectors in one map CRS with on-the-fly reprojection, styles, legend, grid, drawing and editing, measurements (geodesic length and area, profiles, volumes, slope, viewshed), terrain analysis (slope, aspect, contours, differences), vegetation indices and a raster calculator, DEM editor, export of the map and of results; **basemaps**: local MBTiles/GeoPackage/GeoTIFF or online servers (OpenStreetMap, Esri World Imagery, Sentinel-2 cloudless, OpenTopoMap, your own XYZ) — off by default, used only after you accept the server's terms, with its attribution shown
- **3D view** (Qt RHI: Direct3D/Vulkan/Metal/OpenGL): point clouds (PLY, LAS with classes and intensity), OBJ/PLY models, DSM/DTM terrain; the map's basemap under the scene and the job's photos at their camera positions; measurements, point cloud editing and classification, a texture editor working on a copy of the model
- **Local AI** (ONNX Runtime 1.30.0, CPU): MobileSAM object selection by a click and LaMa object removal on orthophotos (both Apache-2.0); every model has a card with its license and checksums
## Requirements

- Windows 10/11 x64 (verified) or Linux x64 (built and tested in CI on Ubuntu 24.04; not yet tried on a desktop)
- To build: CMake 3.21+, a C++17 compiler (Visual Studio 2022 on Windows), Qt 6.7+ with Qt Quick, Quick Controls, Quick Dialogs, Qt Image Formats and Qt Shader Tools; GDAL ≥ 3.8 and PROJ ≥ 9 (Windows: built by vcpkg from `vcpkg.json` on the first configure, ~12 minutes once; Linux: `libgdal-dev`, `libproj-dev`)

No Python interpreter or local web server is needed to build or run the desktop app.

## Install on Windows

Download `GeoMapAI-<version>-win64-setup.exe` from [Releases](https://github.com/Lox123234/Ai-for-geomap/releases) and run it (the installer is not code-signed yet, so Windows SmartScreen asks for confirmation): a Russian wizard installs GeoMap AI with the built-in OpenDroneMap for the current user (no administrator rights) into `%LOCALAPPDATA%\Programs\GeoMapAI`, with Start menu and optional desktop shortcuts; uninstall it from Settings → Apps (projects are kept). The installer is built with `scripts\build-windows.ps1 -Installer` (NSIS).

## Build and run on Windows

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1
.\dist\GeoMapAI\GeoMapAI.exe
```

The script finds Qt, Visual Studio and vcpkg (the copy inside Visual Studio or `VCPKG_ROOT`), builds Release x64, runs the tests and deploys a portable folder `dist\GeoMapAI` (Qt libraries, QML modules, plugins, GDAL/PROJ with `proj.db` and the MSVC runtime next to the EXE). Copy the whole folder to run it on another Windows 10/11 x64 PC; Qt, Qt Creator, Python, GDAL and the VC++ Redistributable are not required there. Add `-Zip` to also produce `dist\GeoMapAI-<version>-win64.zip`.

Dependencies, CMake presets, Linux, CI and troubleshooting: [docs/BUILD.md](docs/BUILD.md).

`GeoMapAI.exe <project folder>` opens that project at start.

Settings (recent projects) are stored per user in `HKEY_CURRENT_USER\Software\GeoMapAI\GeoMap AI`, or in an INI file when started with `--settings-dir <folder>`.

## Processing photos with OpenDroneMap

1. Open a project with overlapping drone photos and switch to the **◈ Обработка** (Processing) tab. The environment check runs automatically.
2. OpenDroneMap is part of the package (`engines\odm`); the environment card shows "Встроенный OpenDroneMap 3.6.2". If the folder was removed (or in a developer build), **Восстановить OpenDroneMap** downloads the same official signed build (233.5 MB, SHA-256 verified) and sets it up for the current user — no administrator rights.
3. Choose a profile — **Только выравнивание** (alignment only), **Быстрый просмотр**, **Сбалансированный** or **Высокое качество** — optionally adjust **Параметры обработки**, read the pre-flight summary and click **Запустить обработку**.
4. Follow the stage and the live log; when the job finishes, see **Результаты** (files with CRS and size) and **Качество** (aligned images, reprojection error, GPS errors, GSD). An alignment-only, cancelled or interrupted job can be continued with **Продолжить**.

Each job lives in its own folder `jobs/<id>/` inside the project, so later runs never overwrite earlier results. Runtimes, profiles, settings and troubleshooting: [docs/PROCESSING_ODM.md](docs/PROCESSING_ODM.md).

## The map

Open the **▦ Карта** (Map) tab. A project with GPS-tagged photos gets its map CRS automatically (the UTM zone of the photos, shown under the layer list); **CRS карты** changes it. Add results with **На карту** in the processing results, or any file with **+ Слой…**. A file without a coordinate system is listed as "CRS не задан" and stays off the map until you assign one with **Указать CRS…** — GeoMap AI never guesses. Measure with **Длина**/**Площадь** (geodesic on the ellipsoid, planar shown alongside), draw with **Точка**/**Линия**/**Полигон**, edit vertices with **Правка**, export with **Экспорт карты…** (PNG/PDF) and **Экспорт объектов…**. The map state lives in `map.json`, drawings in `vectors/drawings.geojson`, pyramids of large rasters in `cache/maps` (safe to delete). Details: [docs/GIS.md](docs/GIS.md).
## Tests

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Seven test executables cover EXIF parsing and corrupt files, projects and their format, background import, cancellation, thumbnails, project locking, processing jobs with a scriptable engine (success, failure, cancel, queue, restart), GIS with control values (coordinate transforms, geodesic distances and areas, a GeoTIFF matched against reference geometry, pyramids, vectors, drawing, export, persistence), the command registry and workspace, and the real main window offscreen (menus, toolbar, a handler for every available command, shortcuts, project tree, layout persistence). An optional eighth runs a real OpenDroneMap reconstruction. Details and the manual checklist: [docs/TESTING.md](docs/TESTING.md).

## Documentation

| Document | Contents |
| --- | --- |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Stages A–G with checklists, Professional Edition additions |
| [docs/FEATURE_MATRIX.md](docs/FEATURE_MATRIX.md) | Feature-by-feature status (tested / implemented / planned / blocked); in the source repository |
| [docs/UI_COMMANDS.md](docs/UI_COMMANDS.md) | Every menu item, toolbar button and shortcut: state, handler, availability |
| [docs/AUDIT.md](docs/AUDIT.md) | Audit of 0.1.0: what worked, what was fixed, risks |
| [docs/BUILD.md](docs/BUILD.md) | Build, dependencies, presets, CI |
| [docs/TESTING.md](docs/TESTING.md) | Automated tests, acceptance results, manual checklist |
| [docs/PROJECT_FORMAT.md](docs/PROJECT_FORMAT.md) | Project folder, `geomap.json`, jobs, cache, lock |
| [docs/PROCESSING_ODM.md](docs/PROCESSING_ODM.md) | OpenDroneMap processing: requirements, profiles, results, troubleshooting |
| [docs/GIS.md](docs/GIS.md) | The map: CRS rules, formats, layers, measurements, drawing, export, limits |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Modules, threads, invariants |
| [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) | Third-party components and licenses |
| [CHANGELOG.md](CHANGELOG.md) | Changes per version |

## Limitations and known tradeoffs

- JPEG and standard TIFF EXIF GPS are supported; proprietary maker notes, XMP sidecars, and advanced TIFF GPS variants are not yet covered. PNG EXIF metadata is not read. EXIF is only searched in the first 8 MiB of a file.
- The full-size photo preview decodes the whole image (a 20 MP photo needs ~80 MB of memory); large rasters on the map are tiled.
- The GPS overview scatter plot preserves relative ordering of coordinates but not geographic distance or aspect ratio; use the map for real positions.
- Map: no basemap (no tiles are downloaded), no WMS/XYZ, no network COG; JPEG 2000, NetCDF and other formats outside the bundled GDAL drivers do not open; KML styles are ignored; exported rasters/reprojected layers are not available yet. 3D view: no level-of-detail for huge clouds (thinned to a point budget), LAZ is not read yet (ODM clouds open through their PLY), no saved views, no surface area/volume in 3D. The "Fast preview" and "Balanced" profiles have been verified on real data. AI inference and GCP support are not present yet.
- GeoMap AI does not cover every professional tool yet: GCP, editors, terrain analysis, multispectral processing and many professional tools are not implemented yet; the current state of each feature is in [docs/FEATURE_MATRIX.md](docs/FEATURE_MATRIX.md).
- Processing needs about 1 GB on disk for the built-in OpenDroneMap. On Linux there is no official ODM build: processing uses a native ODM build given by `GEOMAP_ODM_HOME` (not verified yet). Reconstruction time and quality depend on photo overlap and quality, GPS/GCP and the computer; they are not guaranteed.
- Recent projects that were moved or deleted stay in the list; opening one reports that `geomap.json` is missing.
- There is no installer, code signing or automatic update yet; Windows SmartScreen may warn about the unsigned EXE. The portable folder does not yet bundle the third-party license texts required for redistribution (see [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)), so it should not be published as is.
- The Linux build is verified only in CI (Ubuntu 24.04 with its GDAL 3.8 and PROJ 9.4).

## License

Application source: **MIT**, see [LICENSE](LICENSE). Qt and the other bundled components keep their own licenses (Qt modules used here are available under LGPL-3.0; GDAL, PROJ and their libraries under MIT and compatible permissive licenses); see [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md). OpenDroneMap (AGPL-3.0) is a separate program: GeoMap AI runs it as an external process, and the full package ships it unmodified with its license and a pointer to its source code. It keeps its license and does not become MIT by being distributed with GeoMap AI.
