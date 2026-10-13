# GeoMap AI

**Open-source desktop workspace for UAV photography and geospatial projects.**

`v0.1.1` — Qt 6/QML desktop application written in **C++17**, with no Python runtime, embedded website, accounts or subscription. Photos and projects stay on your computer.

> This is **not** yet a photogrammetry processor. Orthomosaics, dense point clouds, SfM, ODM integration, 3D reconstruction and machine-learning inference are milestones for later versions (see [docs/ROADMAP.md](docs/ROADMAP.md)). The GPS overview shows a scatter plot, **not a real basemap**.

## Available in v0.1.1

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

## Tests

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Three test executables cover EXIF parsing and corrupt files, projects and their format, background import, cancellation, thumbnails and project locking. Details and the manual checklist: [docs/TESTING.md](docs/TESTING.md).

## Documentation

| Document | Contents |
| --- | --- |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Stages A–G with checklists |
| [docs/AUDIT.md](docs/AUDIT.md) | Audit of 0.1.0: what worked, what was fixed, risks |
| [docs/BUILD.md](docs/BUILD.md) | Build, dependencies, presets, CI |
| [docs/TESTING.md](docs/TESTING.md) | Automated tests, acceptance results, manual checklist |
| [docs/PROJECT_FORMAT.md](docs/PROJECT_FORMAT.md) | Project folder, `geomap.json`, cache, lock |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Modules, threads, invariants |
| [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) | Third-party components and licenses |
| [CHANGELOG.md](CHANGELOG.md) | Changes per version |

## Limitations and known tradeoffs

- JPEG and standard TIFF EXIF GPS are supported; proprietary maker notes, XMP sidecars, and advanced TIFF GPS variants are not yet covered. PNG EXIF metadata is not read. EXIF is only searched in the first 8 MiB of a file.
- The full-size preview decodes the whole image (a 20 MP photo needs ~80 MB of memory); tiled viewing of large rasters is planned with the GIS stage.
- The GPS scatter plot preserves relative ordering of coordinates but not geographic distance or aspect ratio; no map tiles are downloaded.
- No photogrammetry, AI inference, terrain rendering or GCP support is present in this version.
- Recent projects that were moved or deleted stay in the list; opening one reports that `geomap.json` is missing.
- There is no installer, code signing or automatic update yet; Windows SmartScreen may warn about the unsigned EXE. The portable folder does not yet bundle the third-party license texts required for redistribution (see [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)), so it should not be published as is.
- Linux build files are provided but the Linux build has not been verified yet.

## License

Application source: **MIT**, see [LICENSE](LICENSE). Qt and the other bundled components keep their own licenses (Qt modules used here are available under LGPL-3.0); see [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md). Future processing components (e.g. OpenDroneMap) keep their respective licenses; they do not become MIT by being used with GeoMap AI.
