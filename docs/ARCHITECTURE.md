# Architecture — GeoMap AI 0.4.0

## Process

Single native Qt process. C++ handles files, persistence and background work; Qt Quick/QML draws the interface. There is no browser, local HTTP service, sign-in or telemetry. Photogrammetric reconstruction is done by OpenDroneMap built into GeoMap AI: its official Windows build ships in `<app>\engines\odm` with its own Python and tools, and GeoMap AI starts, watches and stops it as a child process tree. No Docker, WSL or separate installation; ODM stays a separate process because it is AGPL-3.0 (GeoMap AI remains MIT) and so that its crashes or memory exhaustion cannot take the application down.

## Layers and targets

```text
qml/Main.qml                 main window: menu bar and toolbar from the command registry, panels (SplitView),
                             runCommand/commandEnabled/commandChecked, dialogs, processing view
qml/*.qml                    Theme (palette), MainToolBar, ToolIconButton, CommandMenuItem, ProjectTreePanel,
                             PhotoListPanel, PhotoInspector, BottomPanel, MapController (map dialogs), MapCanvas,
                             MapLayersPanel, MapLayerProperties, ShortcutsDialog, View3DWorkspace (3D view with
                             its context bar and overlays), View3DPanel (scene layers, display); qml/icons/*.svg
app/main.cpp                 composition root: services, workspace objects, QML engine
------------------------------------------------------------- GeoMapUi (static library + QML module "GeoMapAI")
app/ThumbnailProvider        image://thumbs for QML (Qt Quick)
app/MapView                  QQuickPaintedItem: paints the map through MapService, forwards mouse/wheel/keys
app/View3DItem               QQuickRhiItem: draws a Scene3DService snapshot with the GPU (QRhi); mouse → camera
shaders/*.vert|frag          GLSL 440 for points and meshes, compiled by qsb into every graphics API
------------------------------------------------------------- GeoMapCore (static library, no QML)
app/ProjectManager           project lifecycle, selection, import orchestration, persistence
app/PhotoModel               QAbstractListModel for the photo list
app/ExifReader               bounded JPEG APP1 / TIFF IFD metadata parser
src/import/ImportJob         worker-thread import: scan, dedupe, metadata, copy, thumbnail
src/import/ThumbnailCache    <project>/cache/thumbs previews (thread-safe functions)
src/core/ProjectFormat       file names and path rules of the project folder
src/processing/IProcessingEngine   engine interface: capabilities, environment, jobs, outputs
src/processing/OdmEngine           OpenDroneMap built in (engines\odm; restore from the pinned installer if missing);
                                   profiles, option whitelist, install, stage parsing, results, quality
src/processing/Platform            data folder, ASCII-safe paths, NTFS junctions, process trees (job objects)
src/processing/Downloader          HTTPS download accepted only with the pinned SHA-256
src/processing/ProcessingService   job queue, states, jobs/<id>/job.json, log, QML API, JobListModel
src/processing/GeoTiffInfo         bounded (Big)TIFF/GeoKeys reader for result panels
src/processing/ProcessingTypes     JobState, ProcessingJob, Artifact, EnvironmentReport (+ JSON)
src/gis/GisEnvironment             GDAL/PROJ start-up: data folders (proj.db), offline settings, versions, problems
src/gis/Crs                        CRS value type (EPSG/WKT/user input without network) and coordinate transforms
src/gis/Geodesy                    geodesic length/area on the ellipsoid (PROJ geodesic.h, Karney)
src/gis/RasterSource               GeoTIFF/COG/DEM: metadata, reprojection to the map CRS (warped VRT), tiles,
                                   styles (RGB, ramp, hillshade), values under the cursor; PyramidBuilder (cache)
src/gis/VectorSource               GeoJSON/KML/GeoPackage reading (OGR) and writing
src/gis/TerrainAnalysis            slope/aspect (gdaldem), contours (GDALContourGenerateEx), profiles (bilinear,
                                   geodesic distances), cut/fill volumes against a base plane or another surface
app/MapTiles                       tile cache (256 MB) and rendering pool
app/MapService                     the map: layers and their model, view, tools, measurements, drawings with
                                   undo/redo, map.json, exports (PNG/PDF, GeoJSON/GPKG/KML); QML API "gis"
src/ui/commands/CommandRegistry    commands.json (resource): commands, menus, toolbar; user shortcuts in QSettings
src/ui/workspace/WorkspaceState    window and panel layout in QSettings ("workspace/")
src/ui/workspace/AppLog            operations journal (model + daily file in <data>/logs); Journal wires the services
src/ui/workspace/ProjectTreeModel  tree of the project panel from the services; ProjectTreeFilter for the search
src/ui/workspace/AppInfo           versions, documentation and log folders for the Help menu
src/view3d/Scene3D                 3D data and loaders: PLY/LAS clouds, OBJ (MTL textures) and PLY meshes, DEM → terrain
                                   mesh with orthophoto drape; float coordinates relative to a double origin
src/view3d/Camera3D                orbit camera (Z up, yaw 0 = north), fitting to the data, rays; point and triangle picking
src/view3d/Scene3DService          the 3D scene: layers loaded in the background, common origin, camera, picking and
                                   measurements, display settings; QML API "scene3d"; no GPU or QML needed
```

The interface is a QML module in the static library `GeoMapUi` (URI `GeoMapAI`, every file at the module root `qrc:/qt/qml/GeoMapAI/`), linked by the executable and by `GeoMapAIUiTests`, which loads the real window offscreen.

## Commands

Every user action of the main window is a command of `src/ui/commands/commands.json`: id, title, icon, default shortcut, status (`ready`, `planned` with its stage, `engine` when OpenDroneMap cannot do the step alone) and, for ready ones, what it does, when it is enabled and which code handles it. QML builds the menu bar, the toolbar and one `Shortcut` per command from the registry; all of them call `runCommand(id)`, which looks up `handlers[id]` and checks `commandEnabled(id)`. `unhandledCommands()` lists ready commands without a handler and must be empty (UI test). Planned commands are visible, disabled, with the reason. `docs/UI_COMMANDS.md` is generated from the JSON (`scripts/generate-ui-commands.ps1`), and the workspace test checks that it lists every command.

`GeoMapCore` is linked by the app and by the test executables; it depends on Qt Core, Gui and Network, GDAL and PROJ (no QML). `GeoMapUi` adds Qt Quick and the QML module. The layout evolves towards the `src/core`, `src/import`, `src/processing`, … structure of the master specification; the remaining `app/` classes move there when they are next reworked.

## Threads

| Thread | Work |
| --- | --- |
| GUI | QML, `ProjectManager`, model updates, manifest writes |
| `GeoMapImport` (one per import) | folder scan, duplicate check, free-space check, EXIF, copy, thumbnail |
| Thumbnail pool (`idealThreadCount / 2`, min 2) | cache lookups and missing thumbnails requested by QML |
| QML pixmap reader / render thread | Qt Quick internals (full-size preview decoding) |
| Job preparation (one per job start) | reads each input photo's header, hard-links or copies it into `jobs/<id>/images` |
| ODM process tree | `python.exe run.py` and everything it starts, in one job object; output read asynchronously on the GUI thread |
| Network (Qt) | the ODM installer download, streamed to disk with incremental SHA-256 |
| Map tile pool (`idealThreadCount / 2`, min 2) | raster tiles: GDAL reads (with pyramids) and styling into 256×256 images; stale requests are dropped |
| Global thread pool | per-layer preparation: band statistics, pyramid building with progress and cancel, vector loading |
| Export thread (one per export) | map PNG/PDF rendering from a snapshot of the layers and view |

`ImportJob` never touches the model. It publishes results into a mutex-protected buffer and raises `resultsAvailable` once until the GUI thread drains it with `takeResults()`; progress is throttled to ~10 updates per second. Cancellation is checked between files, so a file is either fully copied and registered or not copied at all. `ProjectManager` appends batches to the model, writes manifest checkpoints every 2 s, and on finish writes the manifest and reports per-reason skip counts. Destroying `ProjectManager` during an import (window closed) cancels, joins the worker and records everything copied.

## Thumbnails

`ImportJob` creates the thumbnail of each photo before publishing it, so new list rows show it at once. QML requests `image://thumbs/<file>`; `ThumbnailProvider` resolves the name inside `<project>/photos` only (unsafe names are rejected by `ProjectFormat::isSafePhotoFile`) and loads or rebuilds the cached JPEG on its pool. Responses follow Qt's `imageresponseprovider` pattern: the runnable reports through a queued signal, so a response the engine deleted is just disconnected.

`ProjectManager::projectDirectoryChanged` is emitted **before** the photo model resets, so the delegates created by the reset already resolve thumbnails in the new project.

## Project lock

`geomap.lock` (`QLockFile`, no time-based staleness) is held while a project is open. A second process fails to open the project with a message; locks of dead processes are replaced. Read-only locations open without a lock.

## Processing

`ProcessingService` owns jobs; an `IProcessingEngine` runs one prepared job and reports only what really happens (stage ids, log lines, exit code). QML talks to the service, never to the engine.

```text
QML ──Q_INVOKABLE──▶ ProcessingService ──createJob / start / cancel / abort──▶ IProcessingEngine (OdmEngine)
    ◀── properties,       │ queue, one job at a time                              │ python run.py … (QProcess, job object)
        JobListModel      │ jobs/<id>/job.json (QSaveFile), log.txt               │ stageChanged / logLine / finished
                          ▼                                                       ▼
                  preparation thread → jobs/<id>/images            collectOutputs → verified Artifact list
```

- States: `queued → preparing → running → completed | failed | cancelled`, with `cancelling` in between, and `interrupted` for jobs that were active when the application stopped or the project was closed. Each transition is written to `job.json` before signals go out.
- Runtime: ODM's own `python.exe run.py` with the variables of ODM's `win32env.bat`; every process ODM spawns joins a Windows job object with kill-on-close, so cancel, exit and even a crash of GeoMap AI end the whole tree. Job folders whose path ODM's narrow-API components cannot open (spaces, non-Latin letters) are reached through a temporary NTFS junction in an ASCII-only folder. On Linux a native ODM build is started through its `run.sh` (GEOMAP_ODM_HOME; not verified yet).
- `cancel()` terminates the job object (the process's `finished()` completes the cancellation). `abort()` is the synchronous stop used on exit and on project switch, after the user confirmed in the UI. Jobs of 0.2.0 that ran in Docker stay readable but cannot be resumed.
- Installation of the native runtime: `Downloader` fetches ODM's signed installer and accepts it only with the pinned SHA-256; the Inno Setup installer runs silently per user (no admin); the VC++ runtime shipped with GeoMap AI is copied next to ODM's tools; `run.py --version` verifies the result. An ODM installation registered elsewhere is never replaced (ODM's installer would uninstall it).
- Jobs continue in place (`resumeJob`): the engine drops stage-limiting options and ODM reuses finished stages; a partial job (alignment only) continues with another profile. Quality figures come from ODM's `stats.json` via `qualityReport()`.
- `ProcessingService::setProject` follows `ProjectManager::projectDirectoryChanged`: it stops the previous project's work, then loads `jobs/*/job.json`.
- In `main.cpp` the service is created after `ProjectManager` and before the QML engine, so on exit it is destroyed after the UI and before the project closes: a running job is recorded as interrupted while its project is still open.
- Progress is the engine's real stage (k of N); there is no percentage, because ODM stages differ by orders of magnitude in duration.
- A result is registered only if its file exists, is non-empty and its header matches (GeoTIFF GeoKeys, `LASF`, `%PDF`). Exit code 0 without any result is a failure.

## Map (GIS)

`MapService` owns the layers of the open project (`map.json`) in one **map CRS**. Rasters are reprojected on the fly with a warped VRT; vectors are transformed once on load. The view is a centre and a resolution (map units per screen pixel); `MapView` only forwards input and asks the service to paint.

- CRS policy: the map CRS is the saved one; a new map takes the UTM zone (WGS 84) of the photos' GPS positions, or else the CRS of the first layer added (the UTM zone of its centre if it is in degrees). The choice is deterministic and shown as a note; the user can change it at any time ("CRS карты"). A layer without CRS is **never placed by guessing**: it shows "CRS не задан" until the user enters one (EPSG/WKT) and sees its description first. GeoJSON and KML are WGS 84 by their specifications.
- Rasters: tiles in a per-layer grid in map CRS, level L covers 256·2^L source pixels. Large files without overviews (> 4096 px) get pyramids built in the background into `<project>/cache/maps/<hash>.vrt(.ovr)`; the source file is opened read-only and never modified. Until a tile is ready the nearest coarser cached tile is drawn.
- Measurements: geodesic on the ellipsoid of the map CRS (Karney, PROJ); planar values in the map CRS are shown alongside. The scale bar measures ground distance at the centre of the view.
- Drawings: points, lines and polygons in `vectors/drawings.geojson` (WGS 84), every change is an undo command (add, remove, move vertex).
- Exports: PNG at twice the view size; PDF on A4 landscape (10 mm margins, 200 dpi) with the printed scale; both with legend, CRS, date, scale bar, grid north and sources. Drawings export to GeoJSON (WGS 84), GeoPackage (map CRS) or KML.

## 3D view

`Scene3DService` (no GPU) holds layers with immutable shared data: point clouds (positions and colours), meshes (vertices, normals, UVs, colours, parts with textures), terrains built from a DEM (a grid of at most 1024×1024 vertices, nodata cells left as holes, the orthophoto cut to the DEM extent as texture when it has the same CRS). Data is loaded on a pool of two threads; a layer becomes drawable when its load finishes.

- Coordinates: UTM values do not fit a float with centimetre precision, so every layer keeps float positions relative to its own origin (double): ODM outputs use the offset in `odm_georeferencing/coords.txt`, LAS the minimum of its header, other files their first point, a DEM its lower-left corner. The scene origin is the first loaded layer's; each layer is drawn with a float offset from it.
- Georeferencing: a layer is georeferenced when its CRS is known (ODM `coords.txt`, a CRS record in LAS, a GeoTIFF CRS). Only a scene of georeferenced layers reports values in metres; otherwise picks and measurements are in file units, marked as such. Layers in different CRSs are not reprojected yet (ODM results of one project share one).
- Rendering: `View3DItem` (a `QQuickRhiItem`, the same Direct3D/Vulkan/Metal/OpenGL device as Qt Quick) takes a snapshot of the scene in `synchronize()`, uploads changed layers once (immutable vertex and index buffers, mipmapped textures) and draws points as instanced screen-sized quads (round, by `discard`) and meshes with texture, vertex colours or a height ramp, two-sided sun lighting (azimuth 315°, elevation 45°) and a vertical exaggeration that is visual only (picking divides it out).
- Clouds larger than the point budget are thinned on load (every k-th point); there is no level-of-detail structure yet.
- With the software Qt Quick renderer there is no QRhi: the view says so; layers still load and can be measured.

## Invariants

1. Imported images live in `photos/` under the project directory; originals are never modified or removed.
2. Manifest paths to images must be `photos/<name>`; anything else is ignored on load.
3. `QSaveFile` replaces manifests atomically.
4. Long-running work (import, folder scan, thumbnail generation) does not run on the GUI thread.
5. Location scatter plots are not presented as GIS basemaps.
6. QML never iterates a list *property* of a C++ object: in Qt 6 such a list stays bound to the property and every element access calls the getter again (measured: 201 getter calls for a 100-element loop). Loops take a detached copy from a `Q_INVOKABLE` method (`geoPointsSnapshot()`), and canvases repaint only while visible.

7. Engines run as separate processes started with argument lists, never through a shell; user-supplied engine options pass a whitelist first.
8. A job writes only inside its `jobs/<id>/` directory; its input photos are mounted read-only.
9. Nothing is reported as completed unless it was observed: jobs that were active at shutdown load as `interrupted`.
10. GeoMap AI stays fully usable without ODM (e.g. a developer build); only the processing view reports it missing.
11. Downloaded executables are run only after their SHA-256 matches a value pinned in the source.
12. Engine processes never outlive GeoMap AI (kill-on-close job object, also on a crash).
13. A CRS is never guessed for data that has none; automatic choices (UTM zone of the data) are deterministic and shown.
14. GIS source files are opened read-only; derived data (pyramids) lives in the project cache and can be deleted.
15. GDAL and PROJ run offline: no network access, no `.aux.xml` files next to the user's data (`GDAL_PAM_ENABLED=NO`).
16. Metric values (metres) are shown only for data in a known CRS; ungeoreferenced 3D data is measured in file units, labelled.
