# Architecture — GeoMap AI 0.1.1

## Process

Single native Qt process. C++ handles files, persistence and background work; Qt Quick/QML draws the interface. There is no browser, local HTTP service, Python interpreter, sign-in or telemetry.

## Layers and targets

```text
qml/Main.qml                 UI (QML)
app/main.cpp                 composition root: QML engine, image provider, settings
app/ThumbnailProvider        image://thumbs for QML (Qt Quick)
------------------------------------------------------------- GeoMapCore (static library, no QML)
app/ProjectManager           project lifecycle, selection, import orchestration, persistence
app/PhotoModel               QAbstractListModel for the photo list
app/ExifReader               bounded JPEG APP1 / TIFF IFD metadata parser
src/import/ImportJob         worker-thread import: scan, dedupe, metadata, copy, thumbnail
src/import/ThumbnailCache    <project>/cache/thumbs previews (thread-safe functions)
src/core/ProjectFormat       file names and path rules of the project folder
```

`GeoMapCore` is linked by the app and by the three test executables; it depends on Qt Core and Gui only. The layout evolves towards the `src/core`, `src/import`, `src/processing`, … structure of the master specification; the remaining `app/` classes move there when they are next reworked.

## Threads

| Thread | Work |
| --- | --- |
| GUI | QML, `ProjectManager`, model updates, manifest writes |
| `GeoMapImport` (one per import) | folder scan, duplicate check, free-space check, EXIF, copy, thumbnail |
| Thumbnail pool (`idealThreadCount / 2`, min 2) | cache lookups and missing thumbnails requested by QML |
| QML pixmap reader / render thread | Qt Quick internals (full-size preview decoding) |

`ImportJob` never touches the model. It publishes results into a mutex-protected buffer and raises `resultsAvailable` once until the GUI thread drains it with `takeResults()`; progress is throttled to ~10 updates per second. Cancellation is checked between files, so a file is either fully copied and registered or not copied at all. `ProjectManager` appends batches to the model, writes manifest checkpoints every 2 s, and on finish writes the manifest and reports per-reason skip counts. Destroying `ProjectManager` during an import (window closed) cancels, joins the worker and records everything copied.

## Thumbnails

`ImportJob` creates the thumbnail of each photo before publishing it, so new list rows show it at once. QML requests `image://thumbs/<file>`; `ThumbnailProvider` resolves the name inside `<project>/photos` only (unsafe names are rejected by `ProjectFormat::isSafePhotoFile`) and loads or rebuilds the cached JPEG on its pool. Responses follow Qt's `imageresponseprovider` pattern: the runnable reports through a queued signal, so a response the engine deleted is just disconnected.

`ProjectManager::projectDirectoryChanged` is emitted **before** the photo model resets, so the delegates created by the reset already resolve thumbnails in the new project.

## Project lock

`geomap.lock` (`QLockFile`, no time-based staleness) is held while a project is open. A second process fails to open the project with a message; locks of dead processes are replaced. Read-only locations open without a lock.

## Invariants

1. Imported images live in `photos/` under the project directory; originals are never modified or removed.
2. Manifest paths to images must be `photos/<name>`; anything else is ignored on load.
3. `QSaveFile` replaces manifests atomically.
4. Long-running work (import, folder scan, thumbnail generation) does not run on the GUI thread.
5. Location scatter plots are not presented as GIS basemaps.
6. QML never iterates a list *property* of a C++ object: in Qt 6 such a list stays bound to the property and every element access calls the getter again (measured: 201 getter calls for a 100-element loop). Loops take a detached copy from a `Q_INVOKABLE` method (`geoPointsSnapshot()`), and canvases repaint only while visible.

## Extension seam for v0.2

Introduce an `IProcessingEngine` interface independent of the UI, a job queue with persisted states, and an ODM adapter running as a separate process/container (see `docs/ROADMAP.md`, stage B). GeoMap AI must remain useful without ODM installed.
