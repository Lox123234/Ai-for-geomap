# GeoMap AI

**Open-source desktop workspace for UAV photography and geospatial projects.**

`v0.4.0` — Qt 6/QML desktop application written in **C++17**, with no Python runtime, embedded website, accounts or subscription. Photos and projects stay on your computer.

> Reconstruction (orthophoto, point cloud, elevation models, textured model) is done by **OpenDroneMap built into GeoMap AI**: its official Windows build ships inside the program folder (`engines\odm`) and runs as part of the application — no Docker, no WSL, nothing to install separately, no administrator rights. Projects, import, browsing and the map work even without it. The **map** (GDAL/PROJ, bundled) shows results and other geodata in a real coordinate system, offline, and the **3D view** shows point clouds, textured models and terrain on the GPU; machine learning is a milestone for later versions (see [docs/ROADMAP.md](docs/ROADMAP.md)). There is no background basemap (no map tiles are downloaded).

## Available in v0.4.0

- Professional workspace (Russian interface): main menu (File, Edit, Project, Processing, View, Tools, Analysis, Help) and a grouped icon toolbar built from one list of 144 commands — features that are not implemented yet are shown disabled with the stage that brings them ([docs/UI_COMMANDS.md](docs/UI_COMMANDS.md)); project tree with results, layers and jobs; photo, map, 3D and processing views; properties panel; jobs and operations journal; resizable panels with saved layout; configurable shortcuts
- Create, open, save and close **local projects**; recent projects
- Portable projects: imported photos are **copied** into the project, paths are relative, the manifest `geomap.json` is written atomically
- Import JPEG, PNG and TIFF files or whole folders (recursively) **in the background**: per-file progress, cancel, photos appear while importing, a report of skipped files with reasons, free-space check
- EXIF camera, date and GPS from JPEG APP1 and TIFF IFD, without Python
- Thumbnail cache for fast browsing of hundreds of photos; preview with zoom
- GPS overview: a quick schematic scatter plot of photo positions (non-geographic)
- **Map** (GDAL 3.12.4, PROJ 9.8.1): orthophotos, DSM/DTM and other GeoTIFF/COG rasters with tiles and background pyramids (tested with a 19944×19624 raster), GeoJSON/KML/GeoPackage vectors, photo positions; layer order, visibility, opacity, styles (RGB, single band with colour ramps and hillshade), legend, state saved in the project; one map CRS with on-the-fly reprojection, and **no guessing for files without a CRS** (the user assigns one); cursor coordinates (map CRS and WGS 84) and raster values; scale; **geodesic** length and area; drawing and editing points, lines and polygons with undo/redo; export of the map to PNG/PDF (legend, CRS, scale, sources) and of drawings to GeoJSON/GeoPackage/KML
- **3D view** (GPU through Qt RHI: Direct3D/Vulkan/Metal/OpenGL): point clouds (PLY, uncompressed LAS; ODM clouds), OBJ models with MTL textures and PLY meshes, DSM/DTM as terrain draped with the orthophoto or coloured by height; orbit/pan/zoom, top/south/west/isometric views, perspective or orthographic, axes, vertical exaggeration; point coordinates and distance/height difference/slope measurements (metres only for georeferenced data)
- Remove a project copy of an image without touching the original file
- A project open in one window is locked against writes from a second window
- Keyboard shortcuts: `Ctrl+N`, `Ctrl+O`, `Ctrl+S`, `Ctrl+I`, `Ctrl+W`
- **Processing** with OpenDroneMap 3.6.2: built into the program (no Docker/WSL, no installation, no admin rights); four profiles including alignment-only for stepwise work, ~30 stage settings with validation (SIFT on NVIDIA GPUs), pre-flight check, job queue, real engine stages and live log, cancel and resume, job history kept in the project, verified results (GeoTIFF CRS and pixel size, LAS point count, report) and a quality report from ODM's statistics (aligned images, reprojection error, GPS RMSE/CE90/LE90, GSD)

## Requirements

- Windows 10/11 x64 (verified) or Linux x64 (built and tested in CI on Ubuntu 24.04; not yet tried on a desktop)
- To build: CMake 3.21+, a C++17 compiler (Visual Studio 2022 on Windows), Qt 6.7+ with Qt Quick, Quick Controls, Quick Dialogs, Qt Image Formats and Qt Shader Tools; GDAL ≥ 3.8 and PROJ ≥ 9 (Windows: built by vcpkg from `vcpkg.json` on the first configure, ~12 minutes once; Linux: `libgdal-dev`, `libproj-dev`)

No Python interpreter or local web server is needed to build or run the desktop app.

## Install on Windows

Run `GeoMapAI-<version>-win64-setup.exe`: a Russian wizard installs GeoMap AI with the built-in OpenDroneMap for the current user (no administrator rights) into `%LOCALAPPDATA%\Programs\GeoMapAI`, with Start menu and optional desktop shortcuts; uninstall it from Settings → Apps (projects are kept). The installer is built with `scripts\build-windows.ps1 -Installer` (NSIS).

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
| [docs/METASHAPE_FEATURE_PARITY.md](docs/METASHAPE_FEATURE_PARITY.md) | Feature-by-feature status against Agisoft Metashape Professional (tested / implemented / planned / blocked) |
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
- GeoMap AI is **not** a replacement for Agisoft Metashape Professional: GCP, editors, terrain analysis, multispectral processing and many professional tools are not implemented yet; the current state of each feature is in [docs/METASHAPE_FEATURE_PARITY.md](docs/METASHAPE_FEATURE_PARITY.md).
- Processing needs about 1 GB on disk for the built-in OpenDroneMap. On Linux there is no official ODM build: processing uses a native ODM build given by `GEOMAP_ODM_HOME` (not verified yet). Reconstruction time and quality depend on photo overlap and quality, GPS/GCP and the computer; they are not guaranteed, and GeoMap AI does not claim parity with commercial suites such as Agisoft Metashape (no manual GCP marking yet).
- Recent projects that were moved or deleted stay in the list; opening one reports that `geomap.json` is missing.
- There is no installer, code signing or automatic update yet; Windows SmartScreen may warn about the unsigned EXE. The portable folder does not yet bundle the third-party license texts required for redistribution (see [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)), so it should not be published as is.
- The Linux build is verified only in CI (Ubuntu 24.04 with its GDAL 3.8 and PROJ 9.4).

## License

Application source: **MIT**, see [LICENSE](LICENSE). Qt and the other bundled components keep their own licenses (Qt modules used here are available under LGPL-3.0; GDAL, PROJ and their libraries under MIT and compatible permissive licenses); see [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md). OpenDroneMap (AGPL-3.0) is a separate program: GeoMap AI runs it as an external process, and the full package ships it unmodified with its license and a pointer to its source code. It keeps its license and does not become MIT by being distributed with GeoMap AI.
