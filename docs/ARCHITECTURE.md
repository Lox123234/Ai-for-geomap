# Architecture — GeoMap AI 0.1

## Process

Single native Qt process. C++ (`ProjectManager`, `PhotoModel`, `ExifReader`) handles files and persistence. Qt Quick/QML draws the interface. There is no browser, local HTTP service, Python interpreter, sign-in or telemetry.

## Modules

- `ProjectManager`: project lifecycle, import, portable JSON persistence, recent projects and selection
- `PhotoModel`: `QAbstractListModel` exposing filename, thumbnail URL, camera, GPS and dimensions to QML
- `ExifReader`: metadata parser for standard JPEG APP1 and TIFF images; guarded offsets and counts to avoid accidental out-of-range metadata reads
- `qml/Main.qml`: editor shell, sidebar, image view, GPS scatter plot, inspector, dialogs, shortcuts

## Invariants

1. Imported images live in `photos/` under the project directory.
2. Manifest paths to images must be simple `photos/<file>` relative paths; external/parent traversal paths are rejected on load.
3. `QSaveFile` replaces manifests atomically.
4. The original source is not removed by deleting an imported photo.
5. Location scatter plots are not presented as GIS basemaps.

## Extension seam for v0.2

Introduce an `IProcessingEngine` interface independent of Qt UI. A worker/task manager will coordinate photos -> reconstruction output, progress messages, cancellation and logs. Build a local ODM adapter in a separate process/container when available. GeoMap AI itself must remain useful without ODM installed.
