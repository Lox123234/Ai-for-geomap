# Architecture — GeoMap AI 0.3.1

## Process

Single native Qt process. C++ handles files, persistence and background work; Qt Quick/QML draws the interface. There is no browser, local HTTP service, sign-in or telemetry. Photogrammetric reconstruction runs as a separate program that GeoMap AI starts, watches and stops: OpenDroneMap's official Windows build ("native" runtime, with its own embedded Python) or OpenDroneMap in a Docker container. GeoMap AI itself never needs Docker or Python.

## Layers and targets

```text
qml/Main.qml                 main window: menu bar and toolbar from the command registry, panels (SplitView),
                             runCommand/commandEnabled/commandChecked, dialogs, processing view
qml/*.qml                    Theme (palette), MainToolBar, ToolIconButton, CommandMenuItem, ProjectTreePanel,
                             PhotoListPanel, PhotoInspector, BottomPanel, MapController (map dialogs), MapCanvas,
                             MapLayersPanel, MapLayerProperties, ShortcutsDialog; qml/icons/*.svg (own icons)
app/main.cpp                 composition root: services, workspace objects, QML engine
------------------------------------------------------------- GeoMapUi (static library + QML module "GeoMapAI")
app/ThumbnailProvider        image://thumbs for QML (Qt Quick)
app/MapView                  QQuickPaintedItem: paints the map through MapService, forwards mouse/wheel/keys
------------------------------------------------------------- GeoMapCore (static library, no QML)
app/ProjectManager           project lifecycle, selection, import orchestration, persistence
app/PhotoModel               QAbstractListModel for the photo list
app/ExifReader               bounded JPEG APP1 / TIFF IFD metadata parser
src/import/ImportJob         worker-thread import: scan, dedupe, metadata, copy, thumbnail
src/import/ThumbnailCache    <project>/cache/thumbs previews (thread-safe functions)
src/core/ProjectFormat       file names and path rules of the project folder
src/processing/IProcessingEngine   engine interface: capabilities, environment, jobs, outputs
src/processing/OdmEngine           OpenDroneMap: native runtime (bundled/installed Windows build) or Docker;
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
app/MapTiles                       tile cache (256 MB) and rendering pool
app/MapService                     the map: layers and their model, view, tools, measurements, drawings with
                                   undo/redo, map.json, exports (PNG/PDF, GeoJSON/GPKG/KML); QML API "gis"
src/ui/commands/CommandRegistry    commands.json (resource): commands, menus, toolbar; user shortcuts in QSettings
src/ui/workspace/WorkspaceState    window and panel layout in QSettings ("workspace/")
src/ui/workspace/AppLog            operations journal (model + daily file in <data>/logs); Journal wires the services
src/ui/workspace/ProjectTreeModel  tree of the project panel from the services; ProjectTreeFilter for the search
src/ui/workspace/AppInfo           versions, documentation and log folders for the Help menu
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
| `docker` client processes | `version` / `image inspect` / `info`, `pull`, `run` (ODM), `stop` / `rm -f`; their output is read asynchronously on the GUI thread |
| Native ODM process tree | `python.exe run.py` and everything it starts, in one job object; output read asynchronously on the GUI thread |
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
    ◀── properties,       │ queue, one job at a time                              │ docker run --rm … (QProcess)
        JobListModel      │ jobs/<id>/job.json (QSaveFile), log.txt               │ stageChanged / logLine / finished
                          ▼                                                       ▼
                  preparation thread → jobs/<id>/images            collectOutputs → verified Artifact list
```

- States: `queued → preparing → running → completed | failed | cancelled`, with `cancelling` in between, and `interrupted` for jobs that were active when the application stopped or the project was closed. Each transition is written to `job.json` before signals go out.
- Runtimes: the native runtime starts ODM's own `python.exe run.py` with the variables of ODM's `win32env.bat`; every process ODM spawns joins a Windows job object with kill-on-close, so cancel, exit and even a crash of GeoMap AI end the whole tree. Job folders whose path ODM's narrow-API components cannot open (spaces, non-Latin letters) are reached through a temporary NTFS junction in an ASCII-only folder. The Docker runtime bind-mounts the job folder and the photos read-only.
- `cancel()` is asynchronous: native — terminate the job object; Docker — `docker stop -t 10`, then `docker rm -f` if the client does not exit. `abort()` is the synchronous stop used on exit and on project switch, after the user confirmed in the UI.
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
10. GeoMap AI stays fully usable without Docker or ODM; only the processing view reports them missing.
11. Downloaded executables are run only after their SHA-256 matches a value pinned in the source.
12. Native engine processes never outlive GeoMap AI (kill-on-close job object, also on a crash). Docker containers are removed on exit, or when the project is opened again after a crash.
13. A CRS is never guessed for data that has none; automatic choices (UTM zone of the data) are deterministic and shown.
14. GIS source files are opened read-only; derived data (pyramids) lives in the project cache and can be deleted.
15. GDAL and PROJ run offline: no network access, no `.aux.xml` files next to the user's data (`GDAL_PAM_ENABLED=NO`).
