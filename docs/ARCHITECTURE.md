# Architecture — GeoMap AI 0.2.0

## Process

Single native Qt process. C++ handles files, persistence and background work; Qt Quick/QML draws the interface. There is no browser, local HTTP service, Python interpreter, sign-in or telemetry. Photogrammetric reconstruction runs in a separate process (OpenDroneMap in a Docker container) that GeoMap AI starts, watches and stops; the application itself never needs Docker.

## Layers and targets

```text
qml/Main.qml                 UI (QML)
app/main.cpp                 composition root: QML engine, image provider, settings, processing service
app/ThumbnailProvider        image://thumbs for QML (Qt Quick)
------------------------------------------------------------- GeoMapCore (static library, no QML)
app/ProjectManager           project lifecycle, selection, import orchestration, persistence
app/PhotoModel               QAbstractListModel for the photo list
app/ExifReader               bounded JPEG APP1 / TIFF IFD metadata parser
src/import/ImportJob         worker-thread import: scan, dedupe, metadata, copy, thumbnail
src/import/ThumbnailCache    <project>/cache/thumbs previews (thread-safe functions)
src/core/ProjectFormat       file names and path rules of the project folder
src/processing/IProcessingEngine   engine interface: capabilities, environment, jobs, outputs
src/processing/OdmEngine           OpenDroneMap through the docker CLI (QProcess, argument lists, no shell)
src/processing/ProcessingService   job queue, states, jobs/<id>/job.json, log, QML API, JobListModel
src/processing/GeoTiffInfo         bounded (Big)TIFF/GeoKeys reader for result panels
src/processing/ProcessingTypes     JobState, ProcessingJob, Artifact, EnvironmentReport (+ JSON)
```

`GeoMapCore` is linked by the app and by the test executables; it depends on Qt Core and Gui only. The layout evolves towards the `src/core`, `src/import`, `src/processing`, … structure of the master specification; the remaining `app/` classes move there when they are next reworked.

## Threads

| Thread | Work |
| --- | --- |
| GUI | QML, `ProjectManager`, model updates, manifest writes |
| `GeoMapImport` (one per import) | folder scan, duplicate check, free-space check, EXIF, copy, thumbnail |
| Thumbnail pool (`idealThreadCount / 2`, min 2) | cache lookups and missing thumbnails requested by QML |
| QML pixmap reader / render thread | Qt Quick internals (full-size preview decoding) |
| Job preparation (one per job start) | reads each input photo's header, hard-links or copies it into `jobs/<id>/images` |
| `docker` client processes | `version` / `image inspect` / `info`, `pull`, `run` (ODM), `stop` / `rm -f`; their output is read asynchronously on the GUI thread |

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
- `cancel()` is asynchronous (`docker stop -t 10`, then `docker rm -f` if the client does not exit). `abort()` is the synchronous stop used on exit and on project switch, after the user confirmed in the UI.
- `ProcessingService::setProject` follows `ProjectManager::projectDirectoryChanged`: it stops the previous project's work, then loads `jobs/*/job.json`.
- In `main.cpp` the service is created after `ProjectManager` and before the QML engine, so on exit it is destroyed after the UI and before the project closes: a running job is recorded as interrupted while its project is still open.
- Progress is the engine's real stage (k of N); there is no percentage, because ODM stages differ by orders of magnitude in duration.
- A result is registered only if its file exists, is non-empty and its header matches (GeoTIFF GeoKeys, `LASF`, `%PDF`). Exit code 0 without any result is a failure.

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
