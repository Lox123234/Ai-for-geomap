import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Main window of the professional workspace: menu bar and toolbar built from the command
// registry, project tree and photos on the left, the active view in the centre (photos, map,
// 3D scene, processing, GPS scheme), properties on the right, jobs and journal at the
// bottom, status bar. Every menu item, toolbar button and shortcut runs runCommand(id).
ApplicationWindow {
    id: window
    objectName: "mainWindow"
    visible: true
    width: 1600
    height: 940
    minimumWidth: 1180
    minimumHeight: 660
    title: projects.hasProject ? projects.projectName + " — GeoMap AI" : "GeoMap AI"
    color: Theme.bg
    // Basic-style parts that are not restyled (Dialog title, DialogButtonBox, standard buttons,
    // scroll bars, spin boxes) take their colours from the palette.
    palette {
        window: Theme.panel
        windowText: Theme.text
        base: Theme.field
        text: Theme.text
        button: "#2A303B"
        buttonText: Theme.text
        highlight: Theme.accent
        highlightedText: "#FFFFFF"
        mid: "#4A5464"
        dark: Theme.muted
        light: "#3A4250"
        toolTipBase: "#2A303B"
        toolTipText: Theme.text
    }

    // Colours used by the processing view (kept from 0.2, now from the theme).
    readonly property color bg: Theme.bg
    readonly property color surface: Theme.panel
    readonly property color surface2: "#262C37"
    readonly property color line: Theme.line
    readonly property color muted: Theme.muted
    readonly property color white: Theme.text
    readonly property color accent: Theme.accent
    property int activeView: 0   // 0 photo viewer, 1 GPS scheme, 2 processing, 3 map, 4 3D scene
    property real imageZoom: 1.0
    property string lastError: ""
    property string lastNotice: ""
    // Set only when the user confirms a folder: FolderDialog.selectedFolder already
    // holds the dialog's start folder as soon as it opens, even if it is cancelled.
    property url newProjectFolder: ""
    // Processing view (activeView 2): selected profile, last pre-flight check, selected job.
    property string profileId: "fast"
    property var preflightResult: ({})
    property string selectedJobId: ""
    property var jobInfo: ({})
    property string activeJobText: ""
    property int clockTick: 0
    property bool showOptions: false
    property var optionValues: ({})      // structured engine options set by the user
    property var optionDefs: []          // detached copy of processing.optionDefinitions
    property int detailTab: 0            // 0 results, 1 quality, 2 log
    // Confirmations before stopping a running job (closing the window or the project).
    property bool closeConfirmed: false
    property var pendingProjectAction: null
    // Workspace layout (persisted in WorkspaceState).
    property bool showLeft: true
    property bool showRight: true
    property bool showBottom: true
    property bool photosShown: true
    property string transientMessage: ""

    onSelectedJobIdChanged: refreshJob()

    function durationText(seconds) {
        if (seconds === undefined || seconds === null || seconds < 0) return "—"
        var s = Math.floor(seconds), h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60)
        return h > 0 ? h + " ч " + m + " мин" : m > 0 ? m + " мин " + (s % 60) + " с" : s + " с"
    }
    function stateColor(state) {
        switch (state) {
        case "completed": return Theme.good
        case "failed": return Theme.bad
        case "running": case "preparing": return "#7FB3F0"
        case "cancelling": case "cancelled": case "interrupted": return Theme.warn
        default: return Theme.muted
        }
    }
    function refreshPreflight() {
        window.preflightResult = projects.hasProject
            ? processing.preflight(window.profileId, extraOptionsField.text, window.optionValues) : ({})
    }
    function profileTitle(id) {
        var list = processing.profiles
        for (var i = 0; i < list.length; ++i) if (list[i].id === id) return list[i].title
        return id
    }
    function setOption(id, value) {
        var values = Object.assign({}, window.optionValues)
        if (value === "" || value === false || value === undefined || value === null) delete values[id]
        else values[id] = value
        window.optionValues = values
        preflightTimer.restart()
    }
    function refreshJob() {
        var info = window.selectedJobId.length > 0 ? processing.jobDetails(window.selectedJobId) : ({})
        if (window.selectedJobId.length > 0 && info.id === undefined) {
            window.selectedJobId = ""   // deleted, or another project
            return
        }
        window.jobInfo = info
        // Follow the log end unless the user scrolled up to read.
        var bar = logScroll.ScrollBar.vertical
        var follow = bar.position + bar.size >= 0.98
        logArea.text = info.log || ""
        if (follow) Qt.callLater(function() { bar.position = Math.max(0, 1 - bar.size) })
    }
    function refreshActiveJob() {
        if (!processing.active) { window.activeJobText = ""; return }
        if (processing.activeJobId.length === 0) { window.activeJobText = "Обработка: в очереди"; return }
        var info = processing.jobDetails(processing.activeJobId)
        window.activeJobText = "Обработка: " + (info.stageIndex > 0
            ? "этап " + info.stageIndex + " из " + info.stageCount + " · " + info.stageTitle
            : info.stateText)
    }
    // Runs a project switch now, or after the user agrees to stop the running job.
    function guardProcessing(action) {
        if (!processing.active) { action(); return }
        window.pendingProjectAction = action
        stopProcessingDialog.open()
    }
    function showError(text) { window.lastError = text; errorDialog.open() }
    function showNotice(text) { window.lastNotice = text; noticeDialog.open() }
    function flash(text) { window.transientMessage = text; transientTimer.restart() }

    // ------------------------------------------------------------------ commands
    // Availability of a command in the current state (menu items, toolbar, shortcuts).
    function commandEnabled(id) {
        var has = projects.hasProject, idle = !projects.busy, map = has && gis.crsDefined
        switch (id) {
        case "file.new": case "file.open": return idle
        case "file.recent": return idle && projects.recentProjects.length > 0
        case "file.save": case "file.close": return has && idle
        case "import.photos": case "import.folder": case "project.addPhotos": return has && idle
        case "import.data": case "import.orthophoto": case "import.dem": case "import.vector": return has
        case "export.map": return map && !gis.exporting
        case "export.drawings": return has && gis.drawingCount > 0
        case "edit.undo": return has && gis.canUndo
        case "edit.redo": return has && gis.canRedo
        case "edit.delete": return window.activeView === 3 ? gis.hasSelection : (window.activeView === 0 && projects.selectedIndex >= 0 && idle)
        case "edit.deselect": return gis.hasSelection
        case "edit.geometry": case "tool.select": case "tool.area":
        case "tool.point": case "tool.line": case "tool.polygon": case "tool.vertices": return map
        // Terrain analysis needs the map (and a DEM on it, which the tools and dialog check).
        case "tool.profile": case "tool.volume": case "analysis.longProfile": case "analysis.cutFill":
        case "view.slope": case "view.aspect": case "view.contours": case "analysis.terrain":
        case "analysis.slope": case "analysis.aspect": case "analysis.contours": case "analysis.elevationDiff": case "analysis.compare": return map
        // Navigation and the ruler work in the 3D view too (without a map CRS).
        case "tool.pan": case "tool.ruler": return window.activeView === 4 ? has : map
        case "view.3d": case "view.pointCloud": case "import.pointCloud": case "import.model": case "tool.rotate": case "tool.scale":
        case "view.projection": case "view.axes": case "view.top": case "view.front": case "view.side": case "view.iso": return has
        case "project.removePhoto": return projects.selectedIndex >= 0 && idle
        case "project.crs": case "project.reveal": return has
        case "proc.analyze": case "proc.align": case "proc.denseCloud": case "proc.mesh": case "proc.texture":
        case "proc.dsm": case "proc.dtm": case "proc.orthophoto": case "proc.queue": case "proc.setup": case "proc.classify": return has
        case "proc.start": return has && idle && processing.environmentChecked && processing.engineAvailable
        case "proc.stop": return processing.active
        case "view.photo": case "view.map": case "view.processing": case "view.gps": case "view.reset":
        case "view.cameras": case "view.dem": return has
        case "view.basemap": return has
        case "view.hillshade": return window.activeView === 3 && mapController.details.type === "raster"
                                      && mapController.details.style !== undefined && mapController.details.style.mode === "single"
        case "file.exit": case "edit.preferences": case "view.panelProject": case "view.panelProperties": case "view.panelLog":
        case "view.resetLayout": case "help.docs": case "help.shortcuts": case "help.environment": case "help.gpu":
        case "help.versions": case "help.licenses": case "help.bugReport": case "help.about": return true
        }
        return false
    }
    // Checked state for views, tools, panels and toggles.
    function commandChecked(id) {
        if (!projects.hasProject && (id.indexOf("view.") === 0 || id.indexOf("tool.") === 0) && id.indexOf("view.panel") !== 0) return false
        switch (id) {
        case "view.photo": return window.activeView === 0
        case "view.gps": return window.activeView === 1
        case "view.processing": return window.activeView === 2
        case "view.map": return window.activeView === 3
        case "view.3d": return window.activeView === 4
        case "view.projection": return scene3d.orthographic
        case "view.axes": return scene3d.showAxes
        case "tool.rotate": return window.activeView === 4 && scene3d.tool === "orbit"
        case "tool.scale": return window.activeView === 4 && scene3d.tool === "zoom"
        case "tool.pan": return (window.activeView === 3 && gis.tool === "pan") || (window.activeView === 4 && scene3d.tool === "pan")
        case "tool.select": case "edit.geometry": case "tool.vertices": return window.activeView === 3 && gis.tool === "edit"
        case "tool.ruler": return (window.activeView === 3 && gis.tool === "length") || (window.activeView === 4 && scene3d.tool === "measure")
        case "tool.area": return window.activeView === 3 && gis.tool === "area"
        case "tool.point": return window.activeView === 3 && gis.tool === "point"
        case "tool.line": return window.activeView === 3 && gis.tool === "line"
        case "tool.polygon": return window.activeView === 3 && gis.tool === "polygon"
        case "tool.profile": case "analysis.longProfile": return window.activeView === 3 && gis.tool === "profile"
        case "tool.volume": case "analysis.cutFill": return window.activeView === 3 && gis.tool === "volume"
        case "view.panelProject": return window.showLeft
        case "view.panelProperties": return window.showRight
        case "view.panelLog": return window.showBottom
        case "view.cameras": return window.photosShown
        case "view.basemap": return gis.hasBasemap && gis.basemapVisible
        case "view.hillshade": return mapController.details.style !== undefined && mapController.details.style.hillshade === true
        }
        return false
    }
    function setTool(tool) { window.activeView = 3; gis.tool = tool }
    function set3DTool(tool) { window.activeView = 4; scene3d.tool = tool }
    // Opens a file in the 3D view: point cloud, model, or a DEM as terrain (with the project's
    // orthophoto as texture when it covers it). A file already in the scene is just shown.
    function open3D(kind, path) {
        if (!path) return
        var orthophoto = projectTree.result("orthophoto")
        var error = kind === "pointcloud" ? scene3d.addPointCloud(path)
                  : kind === "mesh" ? scene3d.addModel(path)
                  : kind === "dsm" || kind === "dtm" || kind === "terrain" ? scene3d.addTerrain(path, orthophoto.path || "")
                  : scene3d.addFile(path)
        window.activeView = 4
        if (error.length > 0 && error.indexOf("уже в 3D") < 0) window.showError(error)
    }
    function open3DResult(kind) {
        var r = projectTree.result(kind)
        if (r.path === undefined) {
            window.showNotice("В результатах проекта нет такого слоя. Постройте его (Обработка) или откройте файл: Файл → Импорт облака точек / 3D-модели.")
            return
        }
        window.open3D(kind, r.path)
    }
    function openProcessing(profile) {
        window.activeView = 2
        if (profile) window.profileId = profile
        window.refreshPreflight()
    }
    function startProcessing() {
        window.activeView = 2
        window.refreshPreflight()
        if (window.preflightResult.ok !== true) {
            window.showError("Запуск недоступен: см. предварительную проверку во вкладке «Обработка».")
            return
        }
        var id = processing.startJob(window.profileId, extraOptionsField.text, window.optionValues)
        if (id.length > 0) window.selectedJobId = id
    }
    function stopProcessing() {
        var id = processing.activeJobId.length > 0 ? processing.activeJobId : window.selectedJobId
        if (id.length > 0) processing.cancelJob(id)
    }
    // Adds a result of the processing to the map (once) and shows it.
    function showResultOnMap(path) {
        var id = gis.layerIdForSource(path)
        if (id.length === 0) {
            var error = gis.addLayerFile(path)
            if (error.length > 0) { window.showError(error); return }
            id = gis.layerIdForSource(path)
        }
        window.activeView = 3
        if (id.length > 0) { gis.selectedLayer = id; gis.zoomToLayer(id) }
    }
    function showDem() {
        var dem = projectTree.result("dsm")
        if (dem.path === undefined) dem = projectTree.result("dtm")
        if (dem.path === undefined) {
            window.showNotice("В проекте нет модели высот. Постройте DSM (Обработка → Построение DSM) или добавьте файл: Файл → Импорт DEM/DSM/DTM.")
            return
        }
        window.showResultOnMap(dem.path)
    }
    function openDoc(name) {
        var path = name.length > 0 ? appInfo.docPath(name) : appInfo.docsDirectory
        if (path.length === 0 || !appInfo.openPath(path))
            window.showError("Документация не найдена рядом с программой (папка docs).")
    }
    function resetLayout() {
        workspace.clear()
        window.showLeft = true
        window.showRight = true
        window.showBottom = true
        bottomPanel.collapsed = false
        leftPanel.SplitView.preferredWidth = Theme.leftWidth
        rightPanel.SplitView.preferredWidth = Theme.rightWidth
        bottomPanel.SplitView.preferredHeight = Theme.bottomHeight
        treePanel.SplitView.preferredHeight = 300
        mapLayersPanel.SplitView.preferredHeight = 330
        window.flash("Расположение панелей сброшено")
    }
    function saveLayout() {
        workspace.setValue("rootSplit", rootSplit.saveState())
        workspace.setValue("mainSplit", mainSplit.saveState())
        workspace.setValue("leftSplit", leftSplit.saveState())
        workspace.setValue("mapRightSplit", mapRightSplit.saveState())
        workspace.setValue("showLeft", window.showLeft)
        workspace.setValue("showRight", window.showRight)
        workspace.setValue("showBottom", window.showBottom)
        workspace.setValue("bottomCollapsed", bottomPanel.collapsed)
        workspace.setValue("maximized", window.visibility === Window.Maximized)
        if (window.visibility === Window.Windowed) workspace.setValue("geometry", Qt.rect(window.x, window.y, window.width, window.height))
    }
    function flag(key, fallback) {
        var v = workspace.value(key, fallback)
        return v === true || v === "true"
    }
    function restoreLayout() {
        window.showLeft = flag("showLeft", true)
        window.showRight = flag("showRight", true)
        window.showBottom = flag("showBottom", true)
        bottomPanel.collapsed = flag("bottomCollapsed", false)
        var states = { rootSplit: rootSplit, mainSplit: mainSplit, leftSplit: leftSplit, mapRightSplit: mapRightSplit }
        for (var key in states)
            if (workspace.contains(key)) states[key].restoreState(workspace.value(key))
        if (workspace.contains("geometry")) {
            var g = workspace.value("geometry")
            if (g.width >= window.minimumWidth && g.height >= window.minimumHeight) {
                window.width = g.width; window.height = g.height
                window.x = g.x; window.y = g.y
            }
        }
        if (flag("maximized", false)) window.showMaximized()
    }

    // Handlers of the ready commands; commandEnabled() decides when they may run.
    readonly property var handlers: ({
        "file.new": function() { createDialog.open() },
        "file.open": function() { openProjectDialog.open() },
        "file.save": function() { projects.saveProject() },
        "file.close": function() { window.guardProcessing(function() { projects.closeProject() }) },
        "file.exit": function() { window.close() },
        "import.photos": function() { photoDialog.open() },
        "import.folder": function() { photoFolderDialog.open() },
        "import.data": function() { window.activeView = 3; mapController.addLayer("data", "Импорт геоданных") },
        "import.orthophoto": function() { window.activeView = 3; mapController.addLayer("raster", "Импорт ортофотоплана") },
        "import.dem": function() { window.activeView = 3; mapController.addLayer("raster", "Импорт DEM/DSM/DTM") },
        "import.vector": function() { window.activeView = 3; mapController.addLayer("vector", "Импорт векторных данных") },
        "export.map": function() { window.activeView = 3; mapController.exportMap() },
        "export.drawings": function() { mapController.exportDrawings() },
        "edit.undo": function() { gis.undo() },
        "edit.redo": function() { gis.redo() },
        "edit.delete": function() { if (window.activeView === 3) gis.deleteSelected(); else deleteDialog.open() },
        "edit.deselect": function() { gis.clearSelection() },
        "edit.geometry": function() { window.setTool("edit") },
        "edit.preferences": function() { shortcutsDialog.open() },
        "project.addPhotos": function() { photoDialog.open() },
        "project.removePhoto": function() { deleteDialog.open() },
        "project.crs": function() { window.activeView = 3; mapController.chooseMapCrs() },
        "project.reveal": function() { projects.revealProject() },
        "proc.analyze": function() { window.openProcessing("") },
        "proc.align": function() { window.openProcessing("align") },
        "proc.denseCloud": function() { window.openProcessing("balanced") },
        "proc.mesh": function() { window.openProcessing("balanced") },
        "proc.texture": function() { window.openProcessing("balanced") },
        "proc.dsm": function() { window.openProcessing("balanced") },
        "proc.dtm": function() { window.openProcessing("high") },
        "proc.orthophoto": function() { window.openProcessing("fast") },
        "proc.queue": function() { window.activeView = 2 },
        "proc.setup": function() { window.openProcessing(""); window.showOptions = true },
        "proc.classify": function() { window.openProcessing(""); window.showOptions = true },
        "proc.start": function() { window.startProcessing() },
        "proc.stop": function() { window.stopProcessing() },
        "view.photo": function() { window.activeView = 0 },
        "view.gps": function() { window.activeView = 1 },
        "view.processing": function() { window.activeView = 2 },
        "view.map": function() { window.activeView = 3 },
        "view.3d": function() { window.activeView = 4 },
        "view.pointCloud": function() { window.open3DResult("pointcloud") },
        "import.pointCloud": function() { scene3dDialog.kind = "pointcloud"; scene3dDialog.open() },
        "import.model": function() { scene3dDialog.kind = "mesh"; scene3dDialog.open() },
        "view.projection": function() { window.activeView = 4; scene3d.orthographic = !scene3d.orthographic },
        "view.axes": function() { window.activeView = 4; scene3d.showAxes = !scene3d.showAxes },
        "view.top": function() { window.activeView = 4; scene3d.setPreset("top") },
        "view.front": function() { window.activeView = 4; scene3d.setPreset("front") },
        "view.side": function() { window.activeView = 4; scene3d.setPreset("left") },
        "view.iso": function() { window.activeView = 4; scene3d.setPreset("iso") },
        "tool.rotate": function() { window.set3DTool("orbit") },
        "tool.scale": function() { window.set3DTool("zoom") },
        "view.dem": function() { window.showDem() },
        "view.hillshade": function() { gis.setLayerStyle(mapController.details.id, { hillshade: !(mapController.details.style.hillshade === true) }) },
        "view.cameras": function() { gis.setLayerVisible("photos", !window.photosShown) },
        "view.basemap": function() {
            window.activeView = 3
            if (gis.hasBasemap) gis.setBasemapVisible(!gis.basemapVisible)
            else mapController.addBasemap()
        },
        "view.reset": function() { if (window.activeView === 3) gis.zoomToAll(); else if (window.activeView === 4) scene3d.fitAll(); else window.imageZoom = 1.0 },
        "view.panelProject": function() { window.showLeft = !window.showLeft },
        "view.panelProperties": function() { window.showRight = !window.showRight },
        "view.panelLog": function() { window.showBottom = !window.showBottom },
        "view.resetLayout": function() { window.resetLayout() },
        "tool.select": function() { window.setTool("edit") },
        "tool.vertices": function() { window.setTool("edit") },
        "tool.pan": function() { if (window.activeView === 4) window.set3DTool("pan"); else window.setTool("pan") },
        "tool.ruler": function() { if (window.activeView === 4) window.set3DTool("measure"); else window.setTool("length") },
        "tool.area": function() { window.setTool("area") },
        "tool.point": function() { window.setTool("point") },
        "tool.line": function() { window.setTool("line") },
        "tool.polygon": function() { window.setTool("polygon") },
        "tool.profile": function() { window.setTool("profile") },
        "tool.volume": function() { window.setTool("volume") },
        "analysis.longProfile": function() { window.setTool("profile") },
        "analysis.cutFill": function() { window.setTool("volume") },
        "analysis.terrain": function() { window.activeView = 3; terrainDialog.openFor("slope") },
        "view.slope": function() { window.activeView = 3; terrainDialog.openFor("slope") },
        "analysis.slope": function() { window.activeView = 3; terrainDialog.openFor("slope") },
        "view.aspect": function() { window.activeView = 3; terrainDialog.openFor("aspect") },
        "analysis.aspect": function() { window.activeView = 3; terrainDialog.openFor("aspect") },
        "view.contours": function() { window.activeView = 3; terrainDialog.openFor("contours") },
        "analysis.contours": function() { window.activeView = 3; terrainDialog.openFor("contours") },
        "analysis.elevationDiff": function() { window.activeView = 3; terrainDialog.openFor("difference") },
        "analysis.compare": function() { window.activeView = 3; terrainDialog.openFor("difference") },
        "help.docs": function() { window.openDoc("") },
        "help.shortcuts": function() { shortcutsDialog.open() },
        "help.environment": function() { processing.checkEnvironment(); environmentDialog.open() },
        "help.gpu": function() { gpuDialog.open() },
        "help.versions": function() { aboutDialog.open() },
        "help.licenses": function() { window.openDoc("DEPENDENCIES.md") },
        "help.bugReport": function() {
            if (appInfo.openLogDirectory())
                window.showNotice("Открыта папка журналов программы. Приложите файл geomap-<дата>.log к описанию ошибки; при ошибке обработки — также log.txt из папки задания.")
            else window.showError("Не удалось открыть папку журналов: " + appInfo.logDirectory)
        },
        "help.about": function() { aboutDialog.open() }
    })
    // Runs a command if it is ready and enabled; returns whether it ran.
    function runCommand(id) {
        if (!commands.isReady(id)) { window.flash(commands.title(id) + ": " + commands.unavailableReason(id)); return false }
        var handler = window.handlers[id]
        if (typeof handler !== "function" || !window.commandEnabled(id)) return false
        handler()
        return true
    }
    // Ready commands without a handler (must stay empty; checked by the UI test).
    function unhandledCommands() {
        var missing = [], ids = commands.ids()
        for (var i = 0; i < ids.length; ++i)
            if (commands.isReady(ids[i]) && commands.command(ids[i]).kind !== "recent" && typeof window.handlers[ids[i]] !== "function")
                missing.push(ids[i])
        return missing
    }

    // ------------------------------------------------------------------ project tree
    function selectTreeItem(item) {
        if (item.kind === "layer") gis.selectedLayer = item.payload
        else if (item.kind === "job") window.selectedJobId = item.payload
        if (!item.available && item.status) window.flash(item.title + ": " + item.status)
    }
    function openJob(id) { window.activeView = 2; window.selectedJobId = id }
    function openTreeItem(item) {
        switch (item.kind) {
        case "project": case "photos": window.activeView = 0; return
        case "orthophoto": case "dsm": case "dtm": if (item.available) window.showResultOnMap(item.payload); return
        case "pointcloud": case "mesh": if (item.available) window.open3D(item.kind, item.payload); return
        case "report": if (item.available) processing.openOutput(item.jobId, item.file); return
        case "layers": window.activeView = 3; return
        case "layer": window.activeView = 3; gis.selectedLayer = item.payload; gis.zoomToLayer(item.payload); return
        case "jobs": window.activeView = 2; return
        case "job": window.openJob(item.payload); return
        }
        if (!item.available && item.status) window.flash(item.title + ": " + item.status)
    }
    function treeActions(item) {
        if (!item || !item.kind) return []
        switch (item.kind) {
        case "project": return [{ id: "reveal", title: "Открыть папку проекта" }, { id: "close", title: "Закрыть проект" }]
        case "photos": return [{ id: "open", title: "Открыть просмотр снимков" }, { id: "import", title: "Импорт фотографий…" }]
        case "orthophoto": case "dsm": case "dtm":
            return item.available ? [{ id: "open", title: "Показать на карте" }, { id: "open3d", title: "Открыть в 3D (рельеф)" },
                                     { id: "external", title: "Открыть во внешней программе" },
                                     { id: "folder", title: "Открыть папку задания" }] : [{ id: "none", title: item.status, enabled: false }]
        case "pointcloud": case "mesh":
            return item.available ? [{ id: "open3d", title: "Открыть в 3D" }, { id: "external", title: "Открыть во внешней программе" },
                                     { id: "folder", title: "Открыть папку задания" }] : [{ id: "none", title: item.status, enabled: false }]
        case "report":
            return item.available ? [{ id: "external", title: "Открыть во внешней программе" }, { id: "folder", title: "Открыть папку задания" }]
                                  : [{ id: "none", title: item.status, enabled: false }]
        case "layers": return [{ id: "add", title: "Импорт геоданных…" }]
        case "layer": return [{ id: "zoom", title: "Показать слой целиком" }, { id: "toggle", title: "Показать / скрыть" },
                              { id: "remove", title: "Убрать с карты", enabled: item.payload !== "photos" && item.payload !== "drawings" }]
        case "job": return [{ id: "open", title: "Открыть задание" }, { id: "folder", title: "Открыть папку задания" }]
        }
        return item.available ? [] : [{ id: "none", title: item.status || "Недоступно", enabled: false }]
    }
    function runTreeAction(action, item) {
        switch (action) {
        case "open": window.openTreeItem(item); break
        case "open3d": window.open3D(item.kind, item.payload); break
        case "reveal": window.runCommand("project.reveal"); break
        case "close": window.runCommand("file.close"); break
        case "import": window.runCommand("import.photos"); break
        case "add": window.runCommand("import.data"); break
        case "external": processing.openOutput(item.jobId, item.file); break
        case "folder": processing.revealJob(item.kind === "job" ? item.payload : item.jobId); break
        case "zoom": window.activeView = 3; gis.zoomToLayer(item.payload); break
        case "toggle": gis.setLayerVisible(item.payload, !(gis.layerDetails(item.payload).visible === true)); break
        case "remove": gis.removeLayer(item.payload); break
        }
    }

    Timer { id: transientTimer; interval: 6000; onTriggered: window.transientMessage = "" }

    // Shortcuts of all commands (user-configurable in "Горячие клавиши").
    Instantiator {
        model: commands.ids()
        delegate: Shortcut {
            required property string modelData
            sequence: commands.revision >= 0 ? commands.shortcut(modelData) : ""
            enabled: sequence.length > 0 && commands.isReady(modelData) && window.commandEnabled(modelData)
            autoRepeat: false
            onActivated: window.runCommand(modelData)
        }
    }

    // Menu bar: built once from the registry (items keep their bindings to the window state).
    menuBar: MenuBar {
        id: mainMenuBar
        background: Rectangle {
            implicitHeight: Theme.menuHeight
            color: Theme.toolbar
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
        }
        delegate: MenuBarItem {
            id: menuBarItem
            implicitHeight: Theme.menuHeight
            leftPadding: 10; rightPadding: 10
            contentItem: Text {
                text: menuBarItem.text.replace("&", "")
                color: menuBarItem.highlighted ? "#FFFFFF" : Theme.text
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle { color: menuBarItem.highlighted ? Theme.accent : "transparent" }
        }
    }
    Component {
        id: menuComponent
        Menu {
            topPadding: 4; bottomPadding: 4
            background: Rectangle { implicitWidth: 280; color: Theme.toolbar; border.color: Theme.line; radius: 4 }
        }
    }
    Component { id: menuItemComponent; CommandMenuItem {} }
    Component {
        id: separatorComponent
        MenuSeparator {
            topPadding: 3; bottomPadding: 3
            contentItem: Rectangle { implicitHeight: 1; color: Theme.line }
        }
    }
    Component {
        id: recentMenuComponent
        Menu {
            id: recentMenu
            title: commands.title("file.recent")
            enabled: window.commandEnabled("file.recent")
            topPadding: 4; bottomPadding: 4
            background: Rectangle { implicitWidth: 420; color: Theme.toolbar; border.color: Theme.line; radius: 4 }
            Instantiator {
                model: projects.recentProjects
                delegate: MenuItem {
                    required property string modelData
                    text: modelData
                    implicitHeight: 26
                    onTriggered: { var path = modelData; window.guardProcessing(function() { projects.openRecent(path) }) }
                }
                onObjectAdded: (index, object) => recentMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => recentMenu.removeItem(object)
            }
        }
    }
    function menuAt(index) { return mainMenuBar.menuAt(index) }
    function buildMenus() {
        var menus = commands.menus
        for (var i = 0; i < menus.length; ++i) {
            var menu = menuComponent.createObject(mainMenuBar, { title: menus[i].title })
            var items = menus[i].items
            var widest = 260
            for (var k = 0; k < items.length; ++k) {
                // Items are created inside the menu's list so that they belong to the scene at once.
                if (items[k] === "-") menu.addItem(separatorComponent.createObject(menu.contentItem))
                else if (items[k] === "file.recent") menu.addMenu(recentMenuComponent.createObject(menu))
                else {
                    var item = menuItemComponent.createObject(menu.contentItem, { commandId: items[k], host: window })
                    widest = Math.max(widest, item.implicitWidth)
                    menu.addItem(item)
                }
            }
            menu.width = widest + menu.leftPadding + menu.rightPadding
            mainMenuBar.addMenu(menu)
        }
    }
    Component.onCompleted: {
        buildMenus()
        restoreLayout()
        // Without a saved layout, a window larger than the screen (e.g. 1920x1080 at 125 %) is maximized.
        if (!workspace.contains("geometry") && !flag("maximized", false)
            && (window.width > Screen.desktopAvailableWidth || window.height > Screen.desktopAvailableHeight))
            window.showMaximized()
    }

    header: MainToolBar { host: window }

    TerrainDialog { id: terrainDialog }
    // Results of terrain analysis started while the dialog was closed (or after it was closed).
    Connections {
        target: gis
        function onAnalysisFinished(ok, message) {
            if (terrainDialog.visible) return
            if (ok) window.flash(message)
            else window.showError(message)
        }
    }
    MapController {
        id: mapController
        onErrorMessage: function(text) { window.showError(text) }
        onNoticeMessage: function(text) { window.showNotice(text) }
    }
    Connections {
        target: gis
        function onLayersChanged() { window.photosShown = gis.layerDetails("photos").visible !== false }
    }
    Connections {
        target: gis.layerModel
        function onDataChanged() { window.photosShown = gis.layerDetails("photos").visible !== false }
    }

    ShortcutsDialog { id: shortcutsDialog; host: window; parent: Overlay.overlay; anchors.centerIn: parent }

    Dialog {
        id: aboutDialog
        title: "О GeoMap AI"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: 520
        background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: 8 }
        contentItem: ColumnLayout {
            spacing: 6
            Label { text: "GeoMap AI " + appInfo.version; color: Theme.text; font.pixelSize: 18; font.bold: true }
            Label { text: "Открытая настольная программа для фотограмметрии и ГИС. Исходный код — MIT."; color: Theme.muted; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line; Layout.topMargin: 6; Layout.bottomMargin: 6 }
            Label { text: "Qt " + appInfo.qtVersion; color: Theme.text; font.pixelSize: 12 }
            Label { text: gis.gisVersion; color: Theme.text; font.pixelSize: 12 }
            Label { text: "Движок обработки: " + processing.engineName + " " + processing.engineVersion
                          + (processing.environmentChecked ? " · " + (processing.engineAvailable ? "доступен" : "недоступен") : " · окружение не проверено")
                    color: Theme.text; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: "Сторонние компоненты сохраняют свои лицензии: Справка → Библиотеки и лицензии."; color: Theme.faint; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true; Layout.topMargin: 6 }
        }
        standardButtons: Dialog.Close
    }
    Dialog {
        id: environmentDialog
        title: "Проверка окружения"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: 600
        background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: 8 }
        contentItem: ColumnLayout {
            spacing: 6
            SectionTitle { text: "ОБРАБОТКА" }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: processing.checkingEnvironment ? "Проверка…" : processing.environmentChecked ? processing.environmentSummary : "Не проверено"
                color: processing.engineAvailable ? Theme.good : Theme.warn; font.pixelSize: 12
            }
            Repeater {
                model: processing.environmentProblems
                delegate: Label { required property string modelData; text: "✕ " + modelData; color: Theme.bad; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
            }
            Repeater {
                model: processing.environmentWarnings
                delegate: Label { required property string modelData; text: "⚠ " + modelData; color: Theme.warn; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
            }
            SectionTitle { text: "ГИС"; Layout.topMargin: 8 }
            Label { text: gis.gisVersion; color: Theme.text; font.pixelSize: 12 }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: gis.problems.length > 0 ? "⚠ " + gis.problems.join("\n⚠ ") : "Данные PROJ и GDAL найдены, сеть не используется."
                color: gis.problems.length > 0 ? Theme.bad : Theme.good; font.pixelSize: 11
            }
        }
        standardButtons: Dialog.Close
    }
    Dialog {
        id: gpuDialog
        title: "Диагностика GPU"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: 560
        background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: 8 }
        contentItem: ColumnLayout {
            spacing: 6
            Label {
                text: "Интерфейс рисуется через: " + (GraphicsInfo.api === GraphicsInfo.Direct3D11 ? "Direct3D 11"
                    : GraphicsInfo.api === GraphicsInfo.Direct3D12 ? "Direct3D 12"
                    : GraphicsInfo.api === GraphicsInfo.OpenGL ? "OpenGL"
                    : GraphicsInfo.api === GraphicsInfo.Vulkan ? "Vulkan"
                    : GraphicsInfo.api === GraphicsInfo.Software ? "программный рендерер (без GPU)" : "другой API")
                color: Theme.text; font.pixelSize: 12
            }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: !processing.environmentChecked ? "GPU для обработки: окружение ещё не проверено (Справка → Проверка окружения)."
                    : processing.gpuName.length > 0 ? "GPU для обработки (SIFT в OpenDroneMap): " + processing.gpuName
                    : "GPU NVIDIA для обработки не найден: OpenDroneMap использует CPU."
                color: Theme.text; font.pixelSize: 12
            }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: "Подробная диагностика (VRAM, драйверы) и ограничения памяти — этап F."
                color: Theme.faint; font.pixelSize: 11
            }
        }
        standardButtons: Dialog.Close
    }


    Connections {
        target: projects
        function onErrorOccurred(message) {
            window.lastError = message
            errorDialog.open()
        }
        // The GPS canvas paints on the GUI thread; repaint it only while it is shown
        // (it repaints itself when it becomes visible).
        function onProjectChanged() {
            window.activeView = 0
            window.imageZoom = 1.0
            window.selectedJobId = ""
            window.jobInfo = ({})
            if (gpsCanvas.visible) gpsCanvas.requestPaint()
        }
        function onPhotosChanged() {
            if (gpsCanvas.visible) gpsCanvas.requestPaint()
            preflightTimer.restart()
        }
        function onSelectionChanged() {
            window.imageZoom = 1.0
            if (gpsCanvas.visible) gpsCanvas.requestPaint()
        }
        function onImportFinished() {
            if (window.closeAfterImport) window.close()
        }
    }

    Connections {
        target: processing
        function onErrorOccurred(message) {
            window.lastError = message
            errorDialog.open()
        }
        function onJobUpdated(id) {
            if (id === window.selectedJobId) window.refreshJob()
            window.refreshActiveJob()
        }
        function onLogUpdated(id) { if (id === window.selectedJobId) window.refreshJob() }
        function onJobsChanged() {
            window.refreshActiveJob()
            preflightTimer.restart()
        }
        function onEnvironmentChanged() {
            window.optionDefs = processing.optionDefinitions   // one read, see ARCHITECTURE invariant 6
            preflightTimer.restart()
        }
    }
    // The pre-flight check reads every photo's size and the free disk space; coalesce triggers.
    Timer { id: preflightTimer; interval: 300; onTriggered: if (processingWorkspace.visible) window.refreshPreflight() }
    // Elapsed time of the running job in the details panel.
    Timer {
        interval: 1000; repeat: true
        running: processingWorkspace.visible && window.jobInfo.running === true
        onTriggered: window.clockTick++
    }

    FileDialog {
        id: photoDialog
        title: "Добавить фотографии в проект"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Фотографии (*.jpg *.jpeg *.png *.tif *.tiff)", "Все файлы (*)"]
        onAccepted: projects.importFiles(selectedFiles)
    }
    FolderDialog {
        id: photoFolderDialog
        title: "Выберите папку с фотографиями"
        onAccepted: projects.importFolder(selectedFolder)
    }
    FileDialog {
        id: scene3dDialog
        property string kind: "pointcloud"
        title: kind === "pointcloud" ? "Импорт облака точек в 3D-сцену" : "Импорт 3D-модели в 3D-сцену"
        nameFilters: kind === "pointcloud" ? ["Облака точек (*.ply *.las)", "Все файлы (*)"]
                                           : ["3D-модели (*.obj *.ply)", "Все файлы (*)"]
        onAccepted: {
            var error = scene3d.addUrl(selectedFile, kind)
            window.activeView = 4
            if (error.length > 0 && error.indexOf("уже в 3D") < 0) window.showError(error)
        }
    }
    FolderDialog {
        id: projectLocationDialog
        title: "Где сохранить новый проект?"
        onAccepted: {
            window.newProjectFolder = selectedFolder
            projectLocation.text = projects.displayPath(selectedFolder)
        }
    }
    FolderDialog {
        id: openProjectDialog
        title: "Выберите папку существующего GeoMap AI проекта"
        onAccepted: {
            var folder = selectedFolder
            window.guardProcessing(function() { projects.openProject(folder) })
        }
    }

    Dialog {
        id: createDialog
        title: "Новый проект"
        anchors.centerIn: Overlay.overlay
        width: 475
        modal: true
        padding: 24
        background: Rectangle { color: "#202936"; radius: 14; border.color: "#39485b" }
        header: Label {
            text: "  СОЗДАНИЕ ПРОЕКТА"
            font.bold: true
            font.pixelSize: 13
            color: "#e5f0fb"
            topPadding: 24; leftPadding: 17; bottomPadding: 10
        }
        contentItem: ColumnLayout {
            spacing: 12
            Label { text: "Название"; color: window.muted; font.pixelSize: 12 }
            TextField {
                id: projectNameField
                Layout.fillWidth: true
                text: "Новая съёмка"
                selectByMouse: true
                color: window.white
                placeholderTextColor: window.muted
                background: Rectangle { color: "#121b26"; radius: 7; border.color: "#405066" }
            }
            Label { text: "Расположение"; color: window.muted; font.pixelSize: 12 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextField {
                    id: projectLocation
                    Layout.fillWidth: true
                    readOnly: true
                    text: "Папка не выбрана"
                    color: window.white
                    background: Rectangle { color: "#121b26"; radius: 7; border.color: "#405066" }
                }
                FlatButton { text: "Обзор"; onClicked: projectLocationDialog.open() }
            }
        }
        // Layouts have no padding properties; Control provides them around the row.
        footer: Control {
            leftPadding: 24; rightPadding: 24; topPadding: 14; bottomPadding: 18
            contentItem: RowLayout {
                spacing: 10
                Item { Layout.fillWidth: true }
                FlatButton { text: "Отмена"; onClicked: createDialog.close() }
                FlatButton {
                    text: "Создать"
                    emphasis: true
                    enabled: projectNameField.text.trim().length > 0 && window.newProjectFolder.toString().length > 0
                    onClicked: window.guardProcessing(function() {
                        if (projects.createProject(window.newProjectFolder, projectNameField.text)) createDialog.close()
                    })
                }
            }
        }
    }

    Dialog {
        id: errorDialog
        title: "Ошибка"
        width: 450
        anchors.centerIn: Overlay.overlay
        modal: true
        background: Rectangle { color: "#202936"; border.color: "#566176"; radius: 12 }
        contentItem: Label {
            text: window.lastError
            color: "#f4c6c6"
            wrapMode: Text.Wrap
            padding: 22
        }
        standardButtons: Dialog.Ok
    }

    Dialog {
        id: noticeDialog
        title: "Готово"
        width: 450
        anchors.centerIn: Overlay.overlay
        modal: true
        background: Rectangle { color: "#202936"; border.color: "#566176"; radius: 12 }
        contentItem: Label {
            text: window.lastNotice
            color: window.white
            wrapMode: Text.Wrap
            padding: 22
        }
        standardButtons: Dialog.Ok
    }

    Dialog {
        id: deleteDialog
        title: "Удалить фотографию?"
        anchors.centerIn: Overlay.overlay
        modal: true
        width: 400
        background: Rectangle { color: "#202936"; border.color: "#566176"; radius: 12 }
        contentItem: Label {
            text: "Копия снимка будет безвозвратно удалена из текущего проекта. Исходный файл останется на месте."
            color: window.white
            wrapMode: Text.Wrap
            padding: 22
        }
        standardButtons: Dialog.Yes | Dialog.Cancel
        onAccepted: projects.removeSelectedPhoto()
    }

    Dialog {
        id: issuesDialog
        title: "Пропущенные при импорте файлы"
        anchors.centerIn: Overlay.overlay
        modal: true
        width: Math.min(820, window.width - 80)
        height: Math.min(520, window.height - 80)
        background: Rectangle { color: "#202936"; border.color: "#566176"; radius: 12 }
        contentItem: ListView {
            clip: true
            model: projects.importIssues
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Label {
                required property string modelData
                width: ListView.view.width - 12
                text: modelData
                wrapMode: Text.WrapAnywhere
                color: window.white; font.pixelSize: 12
                topPadding: 3; bottomPadding: 3
            }
        }
        standardButtons: Dialog.Close
    }

    // Closing during an import: stop it first, keep what was copied, then quit.
    property bool closeAfterImport: false
    onClosing: function(close) {
        if (projects.busy) {
            close.accepted = false
            closeDuringImportDialog.open()
        } else if (processing.active && !window.closeConfirmed) {
            close.accepted = false
            closeDuringProcessingDialog.open()
        }
        if (close.accepted) window.saveLayout()
    }
    Dialog {
        id: closeDuringProcessingDialog
        title: "Идёт обработка"
        anchors.centerIn: Overlay.overlay
        modal: true
        width: 460
        background: Rectangle { color: "#202936"; border.color: "#566176"; radius: 12 }
        contentItem: Label {
            text: "Остановить обработку и закрыть программу? Задание будет отмечено как прерванное; "
                + "файлы, которые движок уже создал, останутся в папке задания."
            color: window.white
            wrapMode: Text.Wrap
            padding: 22
        }
        standardButtons: Dialog.Yes | Dialog.Cancel
        onAccepted: {
            window.closeConfirmed = true
            window.close()
        }
    }
    Dialog {
        id: stopProcessingDialog
        title: "Идёт обработка"
        anchors.centerIn: Overlay.overlay
        modal: true
        width: 460
        background: Rectangle { color: "#202936"; border.color: "#566176"; radius: 12 }
        contentItem: Label {
            text: "В текущем проекте выполняется обработка. Остановить её? Задание будет отмечено как прерванное."
            color: window.white
            wrapMode: Text.Wrap
            padding: 22
        }
        standardButtons: Dialog.Yes | Dialog.Cancel
        onAccepted: {
            var action = window.pendingProjectAction
            window.pendingProjectAction = null
            if (action) action()
        }
        onRejected: window.pendingProjectAction = null
    }
    Dialog {
        id: installDialog
        title: processing.installTitle + "?"
        anchors.centerIn: Overlay.overlay
        modal: true
        width: 560
        background: Rectangle { color: "#202936"; border.color: "#566176"; radius: 12 }
        contentItem: Label {
            text: processing.installPrompt
            color: window.white
            wrapMode: Text.Wrap
            padding: 22
        }
        standardButtons: Dialog.Yes | Dialog.Cancel
        onAccepted: processing.installRuntime()
    }
    Dialog {
        id: deleteJobDialog
        title: "Удалить задание?"
        anchors.centerIn: Overlay.overlay
        modal: true
        width: 460
        background: Rectangle { color: "#202936"; border.color: "#566176"; radius: 12 }
        contentItem: Label {
            text: "Папка задания " + (window.jobInfo.id || "") + " со всеми результатами и журналом будет удалена безвозвратно. "
                + "Снимки проекта не затрагиваются."
            color: window.white
            wrapMode: Text.Wrap
            padding: 22
        }
        standardButtons: Dialog.Yes | Dialog.Cancel
        onAccepted: if (processing.deleteJob(window.selectedJobId)) window.selectedJobId = ""
    }
    Dialog {
        id: closeDuringImportDialog
        title: "Импорт ещё выполняется"
        anchors.centerIn: Overlay.overlay
        modal: true
        width: 430
        background: Rectangle { color: "#202936"; border.color: "#566176"; radius: 12 }
        contentItem: Label {
            text: "Остановить импорт и закрыть программу? Уже скопированные снимки останутся в проекте."
            color: window.white
            wrapMode: Text.Wrap
            padding: 22
        }
        standardButtons: Dialog.Yes | Dialog.Cancel
        onAccepted: {
            window.closeAfterImport = true
            projects.cancelImport()
        }
    }

    // A tab of the centre area, bound to a view command.
    component ViewTab: ToolButton {
        id: tab
        required property string commandId
        readonly property bool active: window.commandChecked(commandId)
        text: commands.title(commandId)
        icon.source: Theme.icon(commands.icon(commandId))
        icon.width: 16
        icon.height: 16
        display: AbstractButton.TextBesideIcon
        enabled: commands.isReady(commandId) && window.commandEnabled(commandId)
        font.pixelSize: 12
        implicitHeight: 32
        leftPadding: 10; rightPadding: 12
        focusPolicy: Qt.NoFocus
        palette.windowText: tab.active ? "#FFFFFF" : Theme.text
        opacity: enabled ? 1.0 : 0.4
        HoverHandler { id: tabHover }
        ToolTip.visible: tabHover.hovered
        ToolTip.delay: 500
        ToolTip.text: commands.revision >= 0 ? commands.tooltip(commandId) : ""
        onClicked: window.runCommand(commandId)
        background: Rectangle {
            color: tab.active ? Theme.accentSoft : tabHover.hovered && tab.enabled ? Theme.hover : "transparent"
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 2; color: Theme.accent; visible: tab.active }
        }
    }
    component PanelHandle: Rectangle {
        implicitWidth: 4
        implicitHeight: 4
        color: SplitHandle.pressed ? Theme.accent : SplitHandle.hovered ? "#3E4756" : Theme.bg
    }

    SplitView {
        id: rootSplit
        anchors.fill: parent
        orientation: Qt.Vertical
        handle: PanelHandle {}

        SplitView {
            id: mainSplit
            SplitView.fillHeight: true
            orientation: Qt.Horizontal
            handle: PanelHandle {}

            // Left: project tree and photos
            Rectangle {
                id: leftPanel
                visible: window.showLeft
                SplitView.preferredWidth: Theme.leftWidth
                SplitView.minimumWidth: 200
                SplitView.maximumWidth: 560
                color: Theme.panel
                SplitView {
                    id: leftSplit
                    anchors.fill: parent
                    orientation: Qt.Vertical
                    handle: PanelHandle {}
                    ProjectTreePanel {
                        id: treePanel
                        host: window
                        SplitView.preferredHeight: 300
                        SplitView.minimumHeight: 120
                    }
                    PhotoListPanel {
                        host: window
                        SplitView.fillHeight: true
                        SplitView.minimumHeight: 100
                    }
                }
            }

            // Centre: view tabs and the active view
            Rectangle {
                id: centerArea
                SplitView.fillWidth: true
                SplitView.minimumWidth: 480
                color: window.bg
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 34
                        color: Theme.panel
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 4
                            anchors.rightMargin: 12
                            spacing: 0
                            ViewTab { commandId: "view.photo" }
                            ViewTab { commandId: "view.map" }
                            ViewTab { commandId: "view.3d" }
                            ViewTab { commandId: "view.processing" }
                            ViewTab { commandId: "view.gps" }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: !projects.hasProject ? "" : window.activeView === 0 ? (projects.photoCount ? "ПРОСМОТР ФОТОГРАФИЙ" : "ПУСТОЙ ПРОЕКТ")
                                    : window.activeView === 1 ? "СХЕМА GPS-ТОЧЕК · НЕ КАРТА"
                                    : window.activeView === 3 ? "КАРТА · " + (gis.crsDefined ? gis.crsLabel.toUpperCase() : "CRS НЕ ЗАДАН")
                                    : window.activeView === 4 ? "3D-СЦЕНА · " + scene3d.status.toUpperCase()
                                    : "ФОТОГРАММЕТРИЯ · " + processing.engineName.toUpperCase()
                                color: Theme.muted; font.pixelSize: 10; font.letterSpacing: 0.7
                                elide: Text.ElideRight
                                Layout.maximumWidth: centerArea.width * 0.45
                            }
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                    // No project dashboard
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 18
                        visible: !projects.hasProject
                        Rectangle {
                            Layout.alignment: Qt.AlignHCenter
                            width: 80; height: 80; radius: 23
                            color: "#1b3445"; border.color: "#33617a"
                            Label { anchors.centerIn: parent; text: "G"; font.pixelSize: 46; font.bold: true; color: "#70c4f8" }
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: "Добро пожаловать в GeoMap AI"
                            color: window.white; font.pixelSize: 24; font.bold: true
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: "Локальная работа с аэрофотоснимками и геоданными"
                            color: window.muted; font.pixelSize: 13
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 12
                            spacing: 10
                            FlatButton { text: "+ Создать проект"; emphasis: true; onClicked: createDialog.open() }
                            FlatButton { text: "Открыть проект"; onClicked: openProjectDialog.open() }
                        }
                        SectionTitle { text: "НЕДАВНИЕ ПРОЕКТЫ"; Layout.topMargin: 30; visible: projects.recentProjects.length > 0 }
                        Repeater {
                            model: projects.recentProjects
                            delegate: Button {
                                required property string modelData
                                Layout.alignment: Qt.AlignHCenter
                                Layout.maximumWidth: 440
                                text: modelData
                                font.pixelSize: 11
                                onClicked: projects.openRecent(modelData)
                                contentItem: Text { text: parent.text; color: "#accbe1"; elide: Text.ElideMiddle; horizontalAlignment: Text.AlignHCenter }
                                background: Rectangle { color: parent.hovered ? "#283849" : "#1b2530"; border.color: "#35475b"; radius: 6 }
                            }
                        }
                    }

                    // Empty project
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 14
                        visible: projects.hasProject && projects.photoCount === 0 && window.activeView === 0
                        Label { Layout.alignment: Qt.AlignHCenter; text: "▧"; color: "#517996"; font.pixelSize: 64 }
                        Label { Layout.alignment: Qt.AlignHCenter; text: "Загрузите первую съёмку"; color: window.white; font.pixelSize: 21; font.bold: true }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: "JPEG, PNG и TIFF · данные EXIF и GPS сохраняются локально"
                            color: window.muted; font.pixelSize: 12
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 8
                            FlatButton { text: "Добавить фотографии"; emphasis: true; enabled: !projects.busy; onClicked: photoDialog.open() }
                            FlatButton { text: "Импортировать папку"; enabled: !projects.busy; onClicked: photoFolderDialog.open() }
                        }
                    }

                    // Photo viewer
                    Item {
                        id: photoWorkspace
                        anchors.fill: parent
                        visible: projects.hasProject && projects.photoCount > 0 && window.activeView === 0
                        Rectangle { anchors.fill: parent; color: "#10151c" }
                        Image {
                            id: preview
                            anchors.centerIn: parent
                            width: Math.max(100, parent.width - 50)
                            height: Math.max(100, parent.height - 55)
                            source: projects.selectedPhoto.url || ""
                            fillMode: Image.PreserveAspectFit
                            smooth: true
                            asynchronous: true
                            autoTransform: true
                            transform: Scale {
                                origin.x: preview.width / 2
                                origin.y: preview.height / 2
                                xScale: window.imageZoom
                                yScale: window.imageZoom
                            }
                        }
                        WheelHandler {
                            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                            onWheel: function(event) {
                                window.imageZoom = Math.max(0.25, Math.min(6, window.imageZoom * (event.angleDelta.y > 0 ? 1.15 : 1 / 1.15)))
                            }
                        }
                        Row {
                            anchors.bottom: parent.bottom
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottomMargin: 18
                            spacing: 8
                            FlatButton { text: "−"; compact: true; onClicked: window.imageZoom = Math.max(0.25, window.imageZoom / 1.25) }
                            Rectangle {
                                color: "#253140"; width: 74; height: 32; radius: 7
                                Label { anchors.centerIn: parent; text: Math.round(window.imageZoom * 100) + "%"; color: "#e2edfa"; font.pixelSize: 12 }
                            }
                            FlatButton { text: "+"; compact: true; onClicked: window.imageZoom = Math.min(6, window.imageZoom * 1.25) }
                            FlatButton { text: "По размеру"; compact: true; onClicked: window.imageZoom = 1.0 }
                        }
                        Label {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.margins: 16
                            padding: 10
                            text: projects.selectedPhoto.name || ""
                            font.pixelSize: 12; color: "#d4e2f2"
                            background: Rectangle { color: "#CC17212d"; radius: 7 }
                        }
                    }

                    // GPS overview: truthful point distribution, NOT a basemap.
                    Item {
                        id: gpsWorkspace
                        anchors.fill: parent
                        visible: projects.hasProject && window.activeView === 1
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 18
                            spacing: 10
                            RowLayout {
                                Layout.fillWidth: true
                                Label { text: "Расположение снимков"; color: window.white; font.bold: true; font.pixelSize: 16 }
                                Item { Layout.fillWidth: true }
                                Label { text: projects.gpsCount + " снимков с GPS"; color: window.muted; font.pixelSize: 12 }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                color: "#111e2a"
                                radius: 12
                                border.color: "#294052"
                                Canvas {
                                    id: gpsCanvas
                                    anchors.fill: parent
                                    anchors.margins: 22
                                    onWidthChanged: requestPaint()
                                    onHeightChanged: requestPaint()
                                    onVisibleChanged: requestPaint()
                                    onPaint: {
                                        if (!visible) return
                                        var ctx = getContext("2d")
                                        ctx.clearRect(0, 0, width, height)
                                        ctx.strokeStyle = "#263d4a"
                                        ctx.lineWidth = 1
                                        for (var g=0;g<=10;g++) {
                                            ctx.beginPath(); ctx.moveTo(g * width / 10, 0); ctx.lineTo(g * width / 10, height); ctx.stroke()
                                            ctx.beginPath(); ctx.moveTo(0, g * height / 10); ctx.lineTo(width, g * height / 10); ctx.stroke()
                                        }
                                        var pts = projects.geoPointsSnapshot()   // one O(n) copy, see ProjectManager.h
                                        if (!pts || pts.length === 0) return
                                        var minLat = pts[0].latitude, maxLat = minLat
                                        var minLon = pts[0].longitude, maxLon = minLon
                                        for (var i=1;i<pts.length;i++) {
                                            minLat = Math.min(minLat, pts[i].latitude); maxLat = Math.max(maxLat, pts[i].latitude)
                                            minLon = Math.min(minLon, pts[i].longitude); maxLon = Math.max(maxLon, pts[i].longitude)
                                        }
                                        var dLat = Math.max(maxLat-minLat, 0.00000001)
                                        var dLon = Math.max(maxLon-minLon, 0.00000001)
                                        for (var j=0;j<pts.length;j++) {
                                            var x = 30 + (pts[j].longitude-minLon)/dLon * (width-60)
                                            var y = height-30 - (pts[j].latitude-minLat)/dLat * (height-60)
                                            if (maxLon === minLon) x = width/2
                                            if (maxLat === minLat) y = height/2
                                            ctx.beginPath(); ctx.arc(x, y, pts[j].index === projects.selectedIndex ? 7 : 3, 0, Math.PI*2)
                                            ctx.fillStyle = pts[j].index === projects.selectedIndex ? "#f5cf7c" : "#56b6f7"
                                            ctx.fill()
                                        }
                                    }
                                }
                                ColumnLayout {
                                    anchors.centerIn: parent
                                    spacing: 12
                                    visible: projects.gpsCount === 0
                                    Label { text: "Нет GPS-координат"; color: window.white; font.pixelSize: 17; font.bold: true; Layout.alignment: Qt.AlignHCenter }
                                    Label { text: "Добавьте фотографии с геометками, чтобы увидеть схему их расположения"; color: window.muted; font.pixelSize: 12; Layout.alignment: Qt.AlignHCenter }
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: "Схема показывает относительное расположение GPS-точек. Масштаб по осям независим; это не географическая карта и не ортофотоплан."
                                wrapMode: Text.Wrap
                                color: window.muted
                                font.pixelSize: 11
                            }
                        }
                    }
                    // GIS map (MapCanvas.qml); layers and properties are in the right panel.
                    MapCanvas {
                        anchors.fill: parent
                        visible: projects.hasProject && window.activeView === 3
                        host: window
                        controller: mapController
                    }
                    // 3D scene (View3DWorkspace.qml); its layers and settings are in the right panel.
                    View3DWorkspace {
                        anchors.fill: parent
                        visible: projects.hasProject && window.activeView === 4
                        host: window
                    }
                    // Processing: environment, new job, job history and details. Everything shown
                    // comes from the engine (stages, log, files); there is no estimated percentage.
                    Item {
                        id: processingWorkspace
                        anchors.fill: parent
                        visible: projects.hasProject && window.activeView === 2
                        onVisibleChanged: {
                            if (!visible) return
                            if (!processing.environmentChecked && !processing.checkingEnvironment) processing.checkEnvironment()
                            window.refreshPreflight()
                            window.refreshJob()
                        }
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 16

                            // Settings scroll; the start button below stays visible on short screens.
                            ColumnLayout {
                                Layout.preferredWidth: 340
                                Layout.maximumWidth: 340
                                Layout.fillHeight: true
                                spacing: 8
                                ScrollView {
                                    id: processingSetup
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    contentWidth: availableWidth
                                    clip: true
                                    ColumnLayout {
                                        width: processingSetup.availableWidth - 10
                                        spacing: 10
                                        SectionTitle { text: "ДВИЖОК РЕКОНСТРУКЦИИ" }
                                        Rectangle {
                                            Layout.fillWidth: true
                                            implicitHeight: envColumn.implicitHeight + 24
                                            radius: 10
                                            color: "#1a2430"
                                            border.color: !processing.environmentChecked ? "#354459"
                                                : processing.engineAvailable ? "#305c51" : "#6a3d45"
                                            ColumnLayout {
                                                id: envColumn
                                                anchors.fill: parent
                                                anchors.margins: 12
                                                spacing: 6
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    Label { text: processing.engineName + " " + processing.engineVersion; color: window.white; font.bold: true; font.pixelSize: 13 }
                                                    Item { Layout.fillWidth: true }
                                                    Label { text: "встроен в программу"; color: window.muted; font.pixelSize: 10 }
                                                }
                                                Label {
                                                    Layout.fillWidth: true
                                                    wrapMode: Text.Wrap
                                                    font.pixelSize: 12
                                                    text: processing.checkingEnvironment ? "Проверка окружения…"
                                                        : processing.environmentChecked ? processing.environmentSummary : "Окружение не проверено"
                                                    color: processing.checkingEnvironment || !processing.environmentChecked ? window.muted
                                                        : processing.engineAvailable ? "#82d7b7" : "#f4a6a6"
                                                }
                                                Repeater {
                                                    model: processing.environmentProblems
                                                    delegate: Label {
                                                        required property string modelData
                                                        Layout.fillWidth: true
                                                        visible: modelData !== processing.environmentSummary
                                                        text: modelData; wrapMode: Text.Wrap; color: "#f4c6c6"; font.pixelSize: 11
                                                    }
                                                }
                                                Repeater {
                                                    model: processing.environmentWarnings
                                                    delegate: Label {
                                                        required property string modelData
                                                        Layout.fillWidth: true
                                                        text: "⚠ " + modelData; wrapMode: Text.Wrap; color: "#f5cf7c"; font.pixelSize: 11
                                                    }
                                                }
                                                Repeater {
                                                    model: processing.environmentHints
                                                    delegate: Label {
                                                        required property string modelData
                                                        Layout.fillWidth: true
                                                        text: modelData; wrapMode: Text.Wrap; color: window.muted; font.pixelSize: 11
                                                    }
                                                }
                                                RowLayout {
                                                    Layout.topMargin: 4
                                                    spacing: 8
                                                    FlatButton {
                                                        text: "Проверить снова"; compact: true
                                                        enabled: !processing.checkingEnvironment && !processing.installing
                                                        onClicked: processing.checkEnvironment()
                                                    }
                                                    FlatButton {
                                                        text: processing.installTitle; compact: true; emphasis: true
                                                        visible: processing.canInstall && !processing.installing
                                                        enabled: !processing.checkingEnvironment && !processing.active
                                                        onClicked: installDialog.open()
                                                    }
                                                    FlatButton {
                                                        text: "Отменить"; compact: true; caution: true
                                                        visible: processing.installing
                                                        onClicked: processing.cancelInstall()
                                                    }
                                                }
                                                Label {
                                                    Layout.fillWidth: true
                                                    visible: processing.installStatus.length > 0
                                                    text: processing.installStatus
                                                    color: processing.installing ? "#e2edfa" : window.muted
                                                    font.pixelSize: 11; wrapMode: Text.Wrap
                                                }
                                            }
                                        }

                                        SectionTitle { text: "НОВАЯ ОБРАБОТКА"; Layout.topMargin: 10 }
                                        Repeater {
                                            model: processing.profiles
                                            delegate: Rectangle {
                                                id: profileCard
                                                required property var modelData
                                                readonly property bool chosen: window.profileId === modelData.id
                                                Layout.fillWidth: true
                                                implicitHeight: profileText.implicitHeight + 20
                                                Accessible.role: Accessible.RadioButton
                                                Accessible.name: modelData.title
                                                Accessible.checked: chosen
                                                Accessible.onPressAction: profileMouse.clicked(null)
                                                radius: 9
                                                color: chosen ? "#25465f" : profileMouse.containsMouse ? "#263341" : "#1a2430"
                                                border.color: chosen ? "#4e95c2" : "#354459"
                                                ColumnLayout {
                                                    id: profileText
                                                    anchors.fill: parent
                                                    anchors.margins: 10
                                                    spacing: 4
                                                    Label { text: (profileCard.chosen ? "● " : "○ ") + profileCard.modelData.title; color: window.white; font.bold: true; font.pixelSize: 12 }
                                                    Label {
                                                        Layout.fillWidth: true
                                                        text: profileCard.modelData.description
                                                        wrapMode: Text.Wrap; color: "#b7c6d8"; font.pixelSize: 11
                                                    }
                                                    Label {
                                                        Layout.fillWidth: true
                                                        visible: profileCard.chosen
                                                        text: profileCard.modelData.arguments
                                                        wrapMode: Text.WrapAnywhere; color: window.muted; font.pixelSize: 10; font.family: "Consolas"
                                                    }
                                                }
                                                MouseArea {
                                                    id: profileMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    onClicked: {
                                                        window.profileId = profileCard.modelData.id
                                                        window.refreshPreflight()
                                                    }
                                                }
                                            }
                                        }
                                        // Settings by stage, like the dialogs of desktop photogrammetry suites;
                                        // an empty value keeps the profile's (or ODM's) default.
                                        FlatButton {
                                            Layout.fillWidth: true
                                            Layout.topMargin: 4
                                            compact: true
                                            text: (window.showOptions ? "▾ " : "▸ ") + "Параметры обработки"
                                                + (Object.keys(window.optionValues).length > 0 ? " · изменено: " + Object.keys(window.optionValues).length : "")
                                            onClicked: window.showOptions = !window.showOptions
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            visible: window.showOptions
                                            spacing: 6
                                            Label {
                                                Layout.fillWidth: true; wrapMode: Text.Wrap
                                                text: "Пустое значение — как в выбранном профиле. Настройки применяются поверх профиля."
                                                color: "#6f7f92"; font.pixelSize: 10
                                            }
                                            Repeater {
                                                model: window.optionDefs
                                                delegate: ColumnLayout {
                                                    id: optionRow
                                                    required property var modelData
                                                    readonly property var value: window.optionValues[modelData.id]
                                                    Layout.fillWidth: true
                                                    spacing: 3
                                                    SectionTitle {
                                                        visible: optionRow.modelData.firstInGroup === true
                                                        text: optionRow.modelData.group.toUpperCase()
                                                        Layout.topMargin: 8
                                                    }
                                                    CheckBox {
                                                        visible: optionRow.modelData.type === "flag"
                                                        Layout.fillWidth: true
                                                        text: optionRow.modelData.title
                                                        font.pixelSize: 11
                                                        checked: optionRow.value === true
                                                        onToggled: window.setOption(optionRow.modelData.id, checked)
                                                    }
                                                    Label {
                                                        visible: optionRow.modelData.type !== "flag"
                                                        text: optionRow.modelData.title + (optionRow.modelData.unit ? ", " + optionRow.modelData.unit : "")
                                                        color: "#c9d6e4"; font.pixelSize: 11
                                                    }
                                                    ComboBox {
                                                        visible: optionRow.modelData.type === "choice"
                                                        Layout.fillWidth: true
                                                        implicitHeight: 30
                                                        font.pixelSize: 11
                                                        textRole: "title"
                                                        valueRole: "value"
                                                        model: [{ value: "", title: "По профилю" }].concat(optionRow.modelData.choices || [])
                                                        currentIndex: Math.max(0, indexOfValue(optionRow.value === undefined ? "" : optionRow.value))
                                                        onActivated: window.setOption(optionRow.modelData.id, currentValue)
                                                    }
                                                    TextField {
                                                        visible: optionRow.modelData.type === "integer" || optionRow.modelData.type === "number"
                                                        Layout.fillWidth: true
                                                        implicitHeight: 30
                                                        font.pixelSize: 11
                                                        placeholderText: "по профилю (" + optionRow.modelData.min + "–" + optionRow.modelData.max + ")"
                                                        text: optionRow.value === undefined ? "" : String(optionRow.value)
                                                        color: window.white
                                                        placeholderTextColor: "#66768a"
                                                        selectByMouse: true
                                                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                                                        background: Rectangle { color: "#121b26"; radius: 6; border.color: "#405066" }
                                                        onEditingFinished: {
                                                            var t = text.trim().replace(",", ".")
                                                            window.setOption(optionRow.modelData.id, t.length === 0 ? "" : Number(t))
                                                        }
                                                    }
                                                    Label {
                                                        visible: (optionRow.modelData.help || "").length > 0
                                                        Layout.fillWidth: true
                                                        wrapMode: Text.Wrap
                                                        text: optionRow.modelData.help || ""
                                                        color: "#6f7f92"; font.pixelSize: 10
                                                    }
                                                }
                                            }
                                            FlatButton {
                                                text: "Сбросить параметры"; compact: true
                                                enabled: Object.keys(window.optionValues).length > 0
                                                onClicked: { window.optionValues = ({}); preflightTimer.restart() }
                                            }
                                        }
                                        Label { text: "Параметры ODM для опытных (необязательно)"; color: window.muted; font.pixelSize: 11; Layout.topMargin: 4 }
                                        TextField {
                                            id: extraOptionsField
                                            Layout.fillWidth: true
                                            placeholderText: "например: --dsm --orthophoto-resolution 3"
                                            selectByMouse: true
                                            color: window.white
                                            placeholderTextColor: "#66768a"
                                            font.family: "Consolas"; font.pixelSize: 12
                                            background: Rectangle { color: "#121b26"; radius: 7; border.color: "#405066" }
                                            onTextChanged: preflightTimer.restart()
                                        }

                                        // Pre-flight check of the current project for the chosen profile.
                                        Rectangle {
                                            Layout.fillWidth: true
                                            Layout.topMargin: 4
                                            implicitHeight: checkColumn.implicitHeight + 20
                                            radius: 9
                                            color: "#151d27"
                                            border.color: window.line
                                            visible: window.preflightResult.images !== undefined
                                            ColumnLayout {
                                                id: checkColumn
                                                anchors.fill: parent
                                                anchors.margins: 10
                                                spacing: 4
                                                Label {
                                                    Layout.fillWidth: true; wrapMode: Text.Wrap
                                                    text: "Снимков в задании: " + window.preflightResult.images
                                                        + " (с GPS: " + window.preflightResult.withGps + "), " + window.preflightResult.inputText
                                                    color: window.white; font.pixelSize: 11
                                                }
                                                Label {
                                                    Layout.fillWidth: true; wrapMode: Text.Wrap
                                                    text: "Место на диске проекта: нужно около " + window.preflightResult.neededText
                                                        + ", свободно " + window.preflightResult.freeText
                                                    color: window.muted; font.pixelSize: 11
                                                }
                                                Repeater {
                                                    model: window.preflightResult.errors || []
                                                    delegate: Label {
                                                        required property string modelData
                                                        Layout.fillWidth: true
                                                        text: "✕ " + modelData; wrapMode: Text.Wrap; color: "#f4a6a6"; font.pixelSize: 11
                                                    }
                                                }
                                                Repeater {
                                                    model: window.preflightResult.warnings || []
                                                    delegate: Label {
                                                        required property string modelData
                                                        Layout.fillWidth: true
                                                        text: "⚠ " + modelData; wrapMode: Text.Wrap; color: "#f5cf7c"; font.pixelSize: 11
                                                    }
                                                }
                                                Repeater {
                                                    model: window.preflightResult.notes || []
                                                    delegate: Label {
                                                        required property string modelData
                                                        Layout.fillWidth: true
                                                        text: "ℹ " + modelData; wrapMode: Text.Wrap; color: "#9fd3f5"; font.pixelSize: 11
                                                    }
                                                }
                                                Label {
                                                    Layout.fillWidth: true; wrapMode: Text.WrapAnywhere
                                                    visible: (window.preflightResult.arguments || "").length > 0
                                                    text: "Параметры ODM: " + (window.preflightResult.arguments || "")
                                                    color: "#6f7f92"; font.pixelSize: 10; font.family: "Consolas"
                                                }
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: "Время и качество реконструкции зависят от перекрытия и качества снимков, GPS/GCP и ресурсов компьютера; они не гарантируются."
                                            wrapMode: Text.Wrap; color: "#6f7f92"; font.pixelSize: 10
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: projects.busy
                                    text: "Дождитесь окончания импорта."
                                    color: "#f5cf7c"; font.pixelSize: 11
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: !projects.busy && window.preflightResult.ok === false
                                    text: "Запуск недоступен: см. проверку выше."
                                    color: "#f4a6a6"; font.pixelSize: 11
                                }
                                FlatButton {
                                    Layout.fillWidth: true
                                    text: processing.active ? "Добавить в очередь" : "Запустить обработку"
                                    emphasis: true
                                    enabled: window.preflightResult.ok === true && !projects.busy
                                    onClicked: {
                                        var id = processing.startJob(window.profileId, extraOptionsField.text, window.optionValues)
                                        if (id.length > 0) window.selectedJobId = id
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: 10
                                RowLayout {
                                    Layout.fillWidth: true
                                    SectionTitle { text: "ЗАДАНИЯ" }
                                    Item { Layout.fillWidth: true }
                                    Label { text: processing.jobCount.toString(); color: window.muted; font.pixelSize: 11 }
                                }
                                ListView {
                                    id: jobList
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: Math.max(60, Math.min(contentHeight, 230))
                                    clip: true
                                    spacing: 4
                                    model: processing.jobModel
                                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                    delegate: Rectangle {
                                        id: jobRow
                                        required property string jobId
                                        required property string jobState
                                        required property string stateText
                                        required property string profileTitle
                                        required property var createdAt
                                        required property string stageTitle
                                        required property int stageIndex
                                        required property int stageCount
                                        required property string message
                                        required property bool active
                                        Accessible.role: Accessible.ListItem
                                        Accessible.name: "Задание " + jobId + ", " + stateText + ", " + profileTitle
                                        Accessible.onPressAction: window.selectedJobId = jobId
                                        width: jobList.width - 10
                                        height: 50
                                        radius: 8
                                        color: window.selectedJobId === jobId ? "#25465f" : jobMouse.containsMouse ? "#263341" : "#1a2430"
                                        border.color: window.selectedJobId === jobId ? "#4e95c2" : "transparent"
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 10; anchors.rightMargin: 10
                                            spacing: 10
                                            Rectangle {
                                                implicitWidth: stateLabel.implicitWidth + 14; implicitHeight: 22; radius: 6
                                                color: "#121922"
                                                border.color: window.stateColor(jobRow.jobState)
                                                Label { id: stateLabel; anchors.centerIn: parent; text: jobRow.stateText; color: window.stateColor(jobRow.jobState); font.pixelSize: 10; font.bold: true }
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 3
                                                Label {
                                                    text: jobRow.profileTitle + " · " + Qt.formatDateTime(jobRow.createdAt, "dd.MM.yyyy hh:mm") + " · " + jobRow.jobId
                                                    color: window.white; font.pixelSize: 11; font.bold: true
                                                    Layout.fillWidth: true; elide: Text.ElideRight
                                                }
                                                Label {
                                                    text: jobRow.active && jobRow.stageIndex > 0
                                                        ? "Этап " + jobRow.stageIndex + " из " + jobRow.stageCount + " · " + jobRow.stageTitle
                                                        : jobRow.message
                                                    color: window.muted; font.pixelSize: 10
                                                    Layout.fillWidth: true; elide: Text.ElideRight
                                                }
                                            }
                                        }
                                        MouseArea {
                                            id: jobMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            onClicked: window.selectedJobId = jobRow.jobId
                                        }
                                    }
                                    Label {
                                        anchors.centerIn: parent
                                        visible: processing.jobCount === 0
                                        text: "Заданий пока нет"
                                        color: window.muted; font.pixelSize: 12
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    radius: 10
                                    color: "#151d27"
                                    border.color: window.line
                                    Label {
                                        anchors.centerIn: parent
                                        width: parent.width - 40
                                        horizontalAlignment: Text.AlignHCenter
                                        wrapMode: Text.Wrap
                                        visible: window.jobInfo.id === undefined
                                        text: processing.jobCount > 0 ? "Выберите задание, чтобы увидеть этапы, журнал и результаты" : ""
                                        color: window.muted; font.pixelSize: 12
                                    }
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 8
                                        visible: window.jobInfo.id !== undefined
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Label { text: "Задание " + (window.jobInfo.id || ""); color: window.white; font.bold: true; font.pixelSize: 14 }
                                            Label { text: window.jobInfo.stateText || ""; color: window.stateColor(window.jobInfo.state); font.bold: true; font.pixelSize: 12 }
                                            Item { Layout.fillWidth: true }
                                            FlatButton {
                                                text: "Отменить"; compact: true; caution: true
                                                visible: window.jobInfo.active === true
                                                enabled: window.jobInfo.state !== "cancelling"
                                                onClicked: processing.cancelJob(window.selectedJobId)
                                            }
                                            // Continue in the same folder: ODM reuses the stages already done. A partial
                                            // job (alignment only) continues with the profile chosen on the left.
                                            FlatButton {
                                                text: window.jobInfo.partial ? "Продолжить: «" + window.profileTitle(window.profileId) + "»" : "Продолжить"
                                                compact: true; emphasis: true
                                                visible: window.jobInfo.canResume === true
                                                enabled: processing.engineAvailable && !projects.busy
                                                    && !(window.jobInfo.partial && window.profileId === "align")
                                                onClicked: {
                                                    if (window.jobInfo.partial)
                                                        processing.resumeJob(window.selectedJobId, window.profileId, window.optionValues, extraOptionsField.text)
                                                    else
                                                        processing.resumeJob(window.selectedJobId)
                                                }
                                            }
                                            FlatButton { text: "Открыть папку"; compact: true; onClicked: processing.revealJob(window.selectedJobId) }
                                            FlatButton {
                                                text: "Удалить"; compact: true; caution: true
                                                visible: window.jobInfo.active === false
                                                onClicked: deleteJobDialog.open()
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true; wrapMode: Text.Wrap
                                            // clockTick re-evaluates the elapsed time of a running job every second.
                                            text: {
                                                window.clockTick
                                                var info = window.jobInfo
                                                var started = info.startedMs !== undefined && info.startedMs >= 0
                                                var elapsed = info.running && started ? (Date.now() - info.startedMs) / 1000 : info.seconds
                                                return (info.engine || "") + " · " + (info.profileTitle || "") + " · снимков: " + info.imageCount
                                                    + (started ? " · время: " + window.durationText(elapsed) : "")
                                            }
                                            color: "#b7c6d8"; font.pixelSize: 11
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            visible: window.jobInfo.stageIndex > 0
                                            text: "Этап " + window.jobInfo.stageIndex + " из " + window.jobInfo.stageCount + ": " + window.jobInfo.stageTitle
                                                + (window.jobInfo.active ? "" : " (последний выполнявшийся)")
                                            color: window.white; font.pixelSize: 12
                                        }
                                        Row {
                                            spacing: 3
                                            visible: window.jobInfo.stageCount > 0 && window.jobInfo.stageIndex > 0
                                            Repeater {
                                                model: window.jobInfo.stageCount || 0
                                                delegate: Rectangle {
                                                    required property int index
                                                    width: 20; height: 5; radius: 2
                                                    color: index < window.jobInfo.stageIndex - 1 ? "#3f7fa8"
                                                        : index === window.jobInfo.stageIndex - 1 ? window.stateColor(window.jobInfo.state)
                                                        : "#26303b"
                                                }
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true; wrapMode: Text.Wrap
                                            visible: window.jobInfo.active === true
                                            text: "Этапы ODM сильно различаются по длительности, поэтому номер этапа не означает долю выполненной работы."
                                            color: "#6f7f92"; font.pixelSize: 10
                                        }
                                        Label {
                                            Layout.fillWidth: true; wrapMode: Text.Wrap
                                            text: window.jobInfo.message || ""
                                            color: window.jobInfo.state === "failed" ? "#f4c6c6" : "#d4e2f2"; font.pixelSize: 12
                                        }
                                        Label {
                                            Layout.fillWidth: true; wrapMode: Text.WrapAnywhere
                                            text: "Параметры: " + (window.jobInfo.arguments || "")
                                            color: window.muted; font.pixelSize: 10; font.family: "Consolas"
                                        }

                                        Repeater {
                                            model: window.jobInfo.notes || []
                                            delegate: Label {
                                                required property string modelData
                                                Layout.fillWidth: true
                                                text: "ℹ " + modelData; wrapMode: Text.Wrap; color: "#9fd3f5"; font.pixelSize: 11
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.topMargin: 4
                                            spacing: 6
                                            FlatButton {
                                                text: "Результаты" + ((window.jobInfo.outputs || []).length > 0 ? " · " + window.jobInfo.outputs.length : "")
                                                compact: true; emphasis: window.detailTab === 0
                                                onClicked: window.detailTab = 0
                                            }
                                            FlatButton {
                                                text: "Качество"
                                                compact: true; emphasis: window.detailTab === 1
                                                onClicked: window.detailTab = 1
                                            }
                                            FlatButton {
                                                text: "Журнал"
                                                compact: true; emphasis: window.detailTab === 2
                                                onClicked: window.detailTab = 2
                                            }
                                            Item { Layout.fillWidth: true }
                                            Label {
                                                visible: window.detailTab === 2
                                                text: "последние строки · полностью: log.txt в папке задания"
                                                color: "#6f7f92"; font.pixelSize: 10
                                            }
                                        }
                                        StackLayout {
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            Layout.minimumHeight: 120
                                            currentIndex: window.detailTab

                                        ScrollView {
                                            id: resultsScroll
                                            clip: true
                                            contentWidth: availableWidth
                                            ColumnLayout {
                                                width: resultsScroll.availableWidth - 8
                                                spacing: 8
                                                Label {
                                                    visible: (window.jobInfo.outputs || []).length === 0
                                                    Layout.fillWidth: true; wrapMode: Text.Wrap
                                                    text: window.jobInfo.active ? "Результаты появятся после завершения обработки."
                                                        : window.jobInfo.partial ? "Это выравнивание без построения результатов. Посмотрите «Качество» и продолжите обработку."
                                                        : "Результатов нет."
                                                    color: window.muted; font.pixelSize: 12
                                                }
                                                Repeater {
                                                    model: window.jobInfo.outputs || []
                                                    delegate: RowLayout {
                                                        id: outputRow
                                                        required property var modelData
                                                        Layout.fillWidth: true
                                                        spacing: 10
                                                        ColumnLayout {
                                                            Layout.fillWidth: true
                                                            spacing: 2
                                                            Label {
                                                                text: outputRow.modelData.title + " · " + outputRow.modelData.size
                                                                color: window.white; font.pixelSize: 12; font.bold: true
                                                            }
                                                            Label {
                                                                Layout.fillWidth: true; elide: Text.ElideMiddle
                                                                text: outputRow.modelData.details + "  ·  " + outputRow.modelData.file
                                                                color: window.muted; font.pixelSize: 10
                                                            }
                                                        }
                                                        FlatButton {
                                                            text: "На карту"; compact: true; implicitHeight: 28
                                                            visible: ["orthophoto", "dsm", "dtm"].indexOf(outputRow.modelData.kind) >= 0
                                                            onClicked: window.showResultOnMap(outputRow.modelData.path)
                                                        }
                                                        FlatButton {
                                                            text: "В 3D"; compact: true; implicitHeight: 28
                                                            visible: ["pointcloud", "mesh", "dsm", "dtm"].indexOf(outputRow.modelData.kind) >= 0
                                                            onClicked: window.open3D(outputRow.modelData.kind, outputRow.modelData.path)
                                                        }
                                                        FlatButton {
                                                            text: "Открыть"; compact: true; implicitHeight: 28
                                                            onClicked: processing.openOutput(window.selectedJobId, outputRow.modelData.file)
                                                        }
                                                    }
                                                }
                                                Label {
                                                    Layout.fillWidth: true; wrapMode: Text.Wrap
                                                    visible: (window.jobInfo.missing || []).length > 0
                                                    text: "Не созданы в этом режиме: " + (window.jobInfo.missing || []).join(", ")
                                                    color: window.muted; font.pixelSize: 11
                                                }
                                            }
                                        }

                                        // Accuracy as reported by the engine (stats.json), like the report summary of
                                        // desktop photogrammetry suites. Nothing here is estimated by GeoMap AI.
                                        ScrollView {
                                            id: qualityScroll
                                            clip: true
                                            contentWidth: availableWidth
                                            ColumnLayout {
                                                width: qualityScroll.availableWidth - 8
                                                spacing: 6
                                                Label {
                                                    visible: (window.jobInfo.quality || {}).rows === undefined
                                                    Layout.fillWidth: true; wrapMode: Text.Wrap
                                                    text: window.jobInfo.active ? "Отчёт о качестве появится после выравнивания снимков."
                                                                                : "Движок не сообщил статистику для этого задания."
                                                    color: window.muted; font.pixelSize: 12
                                                }
                                                Repeater {
                                                    model: (window.jobInfo.quality || {}).warnings || []
                                                    delegate: Label {
                                                        required property string modelData
                                                        Layout.fillWidth: true
                                                        text: "⚠ " + modelData; wrapMode: Text.Wrap; color: "#f5cf7c"; font.pixelSize: 11
                                                    }
                                                }
                                                Repeater {
                                                    model: (window.jobInfo.quality || {}).rows || []
                                                    delegate: RowLayout {
                                                        id: qualityRow
                                                        required property var modelData
                                                        Layout.fillWidth: true
                                                        spacing: 12
                                                        Label {
                                                            Layout.preferredWidth: 300
                                                            text: qualityRow.modelData.label
                                                            wrapMode: Text.Wrap
                                                            color: window.muted; font.pixelSize: 12
                                                        }
                                                        Label {
                                                            Layout.fillWidth: true
                                                            text: qualityRow.modelData.value
                                                            wrapMode: Text.Wrap
                                                            color: window.white; font.pixelSize: 12; font.bold: true
                                                        }
                                                    }
                                                }
                                                Label {
                                                    visible: ((window.jobInfo.quality || {}).steps || []).length > 0
                                                    Layout.fillWidth: true; wrapMode: Text.Wrap
                                                    Layout.topMargin: 6
                                                    text: "Время шагов выравнивания: " + ((window.jobInfo.quality || {}).steps || []).map(function(s) {
                                                        return s.title + " — " + window.durationText(s.seconds) }).join(", ")
                                                    color: "#6f7f92"; font.pixelSize: 10
                                                }
                                                Label {
                                                    visible: (window.jobInfo.quality || {}).source !== undefined
                                                    Layout.fillWidth: true; wrapMode: Text.Wrap
                                                    text: "Источник: " + ((window.jobInfo.quality || {}).source || "") + " в папке задания (данные OpenDroneMap)."
                                                    color: "#6f7f92"; font.pixelSize: 10
                                                }
                                            }
                                        }

                                        ScrollView {
                                            id: logScroll
                                            clip: true
                                            background: Rectangle { color: "#0f141b"; radius: 7; border.color: "#2a3646" }
                                            TextArea {
                                                id: logArea
                                                readOnly: true
                                                selectByMouse: true
                                                wrapMode: TextEdit.NoWrap
                                                font.family: "Consolas"; font.pixelSize: 11
                                                color: "#c9d6e4"
                                                selectionColor: "#2f5d80"
                                                background: null
                                            }
                                        }
                                        }   // StackLayout
                                    }
                                }
                            }
                        }
                    }
                    }

                // Bottom photo strip
                Rectangle {
                    Layout.fillWidth: true
                    height: projects.hasProject && projects.photoCount > 0 && window.activeView === 0 ? 118 : 0
                    visible: height > 0
                    color: "#171e27"
                    border.color: window.line
                    ListView {
                        id: filmstrip
                        anchors.fill: parent
                        anchors.margins: 12
                        orientation: ListView.Horizontal
                        spacing: 10
                        clip: true
                        model: projects.photoModel
                        delegate: Rectangle {
                            required property int index
                            required property url photoThumb
                            width: 132; height: 90
                            radius: 7; color: "#202b37"; clip: true
                            border.color: index === projects.selectedIndex ? "#6ac9ff" : "#3a4554"
                            border.width: index === projects.selectedIndex ? 2 : 1
                            Image {
                                anchors.fill: parent; anchors.margins: 3
                                source: photoThumb; sourceSize.width: 264; sourceSize.height: 180
                                fillMode: Image.PreserveAspectCrop; asynchronous: true
                            }
                            MouseArea { anchors.fill: parent; onClicked: projects.selectPhoto(index) }
                        }
                    }
                }
                }
            }

            // Right: properties of the active view
            Rectangle {
                id: rightPanel
                visible: window.showRight
                SplitView.preferredWidth: Theme.rightWidth
                SplitView.minimumWidth: 240
                SplitView.maximumWidth: 600
                color: Theme.panel
                PhotoInspector {
                    anchors.fill: parent
                    visible: !projects.hasProject || window.activeView === 0 || window.activeView === 1
                    host: window
                }
                SplitView {
                    id: mapRightSplit
                    anchors.fill: parent
                    visible: projects.hasProject && window.activeView === 3
                    orientation: Qt.Vertical
                    handle: PanelHandle {}
                    MapLayersPanel {
                        id: mapLayersPanel
                        controller: mapController
                        SplitView.preferredHeight: 330
                        SplitView.minimumHeight: 140
                    }
                    MapLayerProperties {
                        controller: mapController
                        SplitView.fillHeight: true
                        SplitView.minimumHeight: 140
                    }
                    MeasurementsPanel {
                        host: window
                        SplitView.preferredHeight: 190
                        SplitView.minimumHeight: 80
                    }
                }
                View3DPanel {
                    anchors.fill: parent
                    visible: projects.hasProject && window.activeView === 4
                    host: window
                }
                // Processing: engine state and the selected job
                ColumnLayout {
                    anchors.fill: parent
                    visible: projects.hasProject && window.activeView === 2
                    spacing: 0
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: Theme.panelHeader
                        color: Theme.toolbar
                        SectionTitle { anchors.verticalCenter: parent.verticalCenter; x: 10; text: "СОСТОЯНИЕ ОБРАБОТКИ" }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.margins: 12
                        spacing: 8
                        Label { text: "Движок"; color: Theme.muted; font.pixelSize: 10 }
                        Label {
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                            text: processing.engineName + " " + processing.engineVersion + "\n"
                                + (processing.checkingEnvironment ? "проверка…" : processing.environmentChecked
                                   ? (processing.engineAvailable ? "доступен · " + processing.environmentSummary : "недоступен") : "не проверен")
                            color: processing.engineAvailable ? Theme.good : Theme.warn; font.pixelSize: 11
                        }
                        Label { text: "Профиль"; color: Theme.muted; font.pixelSize: 10 }
                        Label { text: window.profileTitle(window.profileId); color: Theme.text; font.pixelSize: 12 }
                        Label { text: "Выбранное задание"; color: Theme.muted; font.pixelSize: 10; Layout.topMargin: 6 }
                        Label {
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                            text: window.jobInfo.id === undefined ? "—"
                                : window.jobInfo.id + "\n" + window.jobInfo.profileTitle + " · " + window.jobInfo.stateText
                                  + (window.jobInfo.stageIndex > 0 ? "\nэтап " + window.jobInfo.stageIndex + " из " + window.jobInfo.stageCount + " · " + window.jobInfo.stageTitle : "")
                                  + "\nрезультатов: " + (window.jobInfo.outputs ? window.jobInfo.outputs.length : 0)
                            color: window.jobInfo.id === undefined ? Theme.faint : window.stateColor(window.jobInfo.state); font.pixelSize: 11
                        }
                        Label { text: "Заданий в проекте"; color: Theme.muted; font.pixelSize: 10; Layout.topMargin: 6 }
                        Label { text: processing.jobCount.toString(); color: Theme.text; font.pixelSize: 12 }
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }

        BottomPanel {
            id: bottomPanel
            visible: window.showBottom
            host: window
            SplitView.preferredHeight: Theme.bottomHeight
            SplitView.minimumHeight: collapsed ? Theme.panelHeader : 90
            SplitView.maximumHeight: collapsed ? Theme.panelHeader : 640
        }
    }

    footer: Rectangle {
        height: Theme.statusHeight
        color: Theme.toolbar
        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 12
            Rectangle { implicitWidth: 7; implicitHeight: 7; radius: 4; color: projects.busy ? Theme.warn : Theme.good }
            Label {
                visible: !projects.busy
                text: window.transientMessage.length > 0 ? window.transientMessage : projects.statusMessage
                color: window.transientMessage.length > 0 ? Theme.warn : Theme.muted
                font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true
            }
            // Background import: real per-file progress reported by the worker.
            RowLayout {
                visible: projects.busy
                Layout.fillWidth: true
                spacing: 8
                Label {
                    text: projects.importCancelling ? "Отмена импорта…"
                        : projects.importScanning ? "Поиск снимков… найдено " + projects.importTotal
                        : "Импорт " + projects.importProcessed + " из " + projects.importTotal
                    color: Theme.text; font.pixelSize: 11
                }
                Rectangle {
                    visible: !projects.importScanning && projects.importTotal > 0
                    Layout.preferredWidth: 160
                    implicitHeight: 5; radius: 3; color: Theme.field
                    Rectangle {
                        width: parent.width * Math.min(1, projects.importProcessed / Math.max(1, projects.importTotal))
                        height: parent.height; radius: 3; color: Theme.accent
                    }
                }
                Label {
                    text: projects.importCurrentFile
                    color: Theme.muted; font.pixelSize: 11; elide: Text.ElideMiddle; Layout.fillWidth: true
                }
                FlatButton {
                    text: "Отменить импорт"; compact: true; implicitHeight: 20; font.pixelSize: 11
                    enabled: !projects.importCancelling
                    onClicked: projects.cancelImport()
                }
            }
            FlatButton {
                visible: !projects.busy && projects.importIssues.length > 0
                text: "Пропущенные файлы: " + projects.importIssues.length
                compact: true; implicitHeight: 20; font.pixelSize: 11
                onClicked: issuesDialog.open()
            }
            Label {
                visible: window.activeView === 3 && gis.cursorText.length > 0
                text: gis.cursorText
                color: Theme.text; font.pixelSize: 11; font.family: "Consolas"
            }
            Label {
                visible: window.activeView === 3 && gis.cursorWgs84.length > 0
                text: gis.cursorWgs84
                color: Theme.muted; font.pixelSize: 11
            }
            Label {
                visible: window.activeView === 3 && gis.cursorValue.length > 0
                text: gis.cursorValue
                color: Theme.good; font.pixelSize: 11
            }
            Label {
                visible: window.activeView === 3 && projects.hasProject
                text: gis.crsDefined ? gis.crsLabel : "CRS не задан"
                color: gis.crsDefined ? Theme.muted : Theme.warn; font.pixelSize: 11
                elide: Text.ElideRight; Layout.maximumWidth: 260
            }
            // Running processing job (real engine stage), also while another view is shown.
            FlatButton {
                visible: processing.active
                text: window.activeJobText.length > 0 ? window.activeJobText : "Обработка"
                compact: true; implicitHeight: 20; font.pixelSize: 11
                Layout.maximumWidth: 360
                onClicked: window.openJob(processing.activeJobId.length > 0 ? processing.activeJobId : window.selectedJobId)
            }
            Label { text: "OFFLINE · v" + Qt.application.version; color: Theme.faint; font.pixelSize: 10; font.letterSpacing: 0.5 }
        }
    }
}
