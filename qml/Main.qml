import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    visible: true
    width: 1500
    height: 900
    minimumWidth: 1100
    minimumHeight: 720
    title: projects.hasProject ? projects.projectName + " — GeoMap AI" : "GeoMap AI"
    color: "#11151b"
    // Basic-style parts that are not restyled below (Dialog title, DialogButtonBox,
    // standard buttons, scroll bars) take their colors from the palette.
    palette {
        window: "#202936"
        windowText: "#e5f0fb"
        base: "#121b26"
        text: "#ecf2fa"
        button: "#283341"
        buttonText: "#dce8f5"
        highlight: "#56b6f7"
        highlightedText: "#0c1c29"
        mid: "#56667a"
        dark: "#8d9aaa"
    }

    readonly property color bg: "#11151b"
    readonly property color surface: "#191f28"
    readonly property color surface2: "#202835"
    readonly property color line: "#303b4b"
    readonly property color muted: "#8d9aaa"
    readonly property color white: "#ecf2fa"
    readonly property color accent: "#56b6f7"
    property int activeView: 0   // 0 photo viewer, 1 geo coverage
    property real imageZoom: 1.0
    property string lastError: ""
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

    onSelectedJobIdChanged: refreshJob()

    function durationText(seconds) {
        if (seconds === undefined || seconds === null || seconds < 0) return "—"
        var s = Math.floor(seconds), h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60)
        return h > 0 ? h + " ч " + m + " мин" : m > 0 ? m + " мин " + (s % 60) + " с" : s + " с"
    }
    function stateColor(state) {
        switch (state) {
        case "completed": return "#82d7b7"
        case "failed": return "#f4a6a6"
        case "running": case "preparing": return "#56b6f7"
        case "cancelling": case "cancelled": case "interrupted": return "#f5cf7c"
        default: return "#8d9aaa"
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

    function prettyBytes(b) {
        if (b === undefined || b === null) return "—"
        if (b < 1024) return b + " B"
        if (b < 1048576) return (b / 1024).toFixed(1) + " KB"
        if (b < 1073741824) return (b / 1048576).toFixed(1) + " MB"
        return (b / 1073741824).toFixed(2) + " GB"
    }
    function coord(x) { return Number(x).toFixed(6) + "°" }

    component FlatButton: Button {
        id: flat
        property bool emphasis: false
        property bool caution: false
        property bool compact: false
        implicitHeight: compact ? 32 : 40
        implicitWidth: label.implicitWidth + (compact ? 24 : 32)
        padding: 0
        font.pixelSize: compact ? 12 : 13
        font.weight: Font.DemiBold
        hoverEnabled: true
        contentItem: Text {
            id: label
            text: flat.text
            font: flat.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: !flat.enabled ? "#637080" : flat.emphasis ? "#0c1c29"
                : flat.caution ? "#ffb5b5" : "#dce8f5"
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 8
            color: !flat.enabled ? "#26303b" : flat.emphasis
                ? (flat.hovered ? "#88d2ff" : "#56b6f7")
                : flat.caution ? (flat.hovered ? "#4d2b36" : "#352630")
                : flat.hovered ? "#344255" : "#283341"
            border.color: flat.emphasis ? "transparent" : "#3b4a5d"
        }
    }

    component SectionTitle: Text {
        color: "#8494a8"
        font.pixelSize: 10
        font.bold: true
        font.letterSpacing: 1.8
    }

    component DetailLine: ColumnLayout {
        id: detailLine
        property string field: ""
        property string value: "—"
        spacing: 5
        Label { text: detailLine.field; color: "#8897a9"; font.pixelSize: 11 }
        Label {
            text: detailLine.value && detailLine.value.length > 0 ? detailLine.value : "—"
            color: "#e4edf9"; font.pixelSize: 12
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
        }
    }

    // Project commands are unavailable while an import is running.
    Shortcut { sequence: StandardKey.New; enabled: !projects.busy; onActivated: createDialog.open() }
    Shortcut { sequence: StandardKey.Open; enabled: !projects.busy; onActivated: openProjectDialog.open() }
    Shortcut { sequence: StandardKey.Save; enabled: !projects.busy; onActivated: projects.saveProject() }
    Shortcut { sequence: "Ctrl+W"; enabled: !projects.busy; onActivated: window.guardProcessing(function() { projects.closeProject() }) }
    Shortcut { sequence: "Ctrl+I"; enabled: projects.hasProject && !projects.busy; onActivated: photoDialog.open() }

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

    // Application header
    header: Rectangle {
        height: 64
        color: "#171d26"
        border.color: window.line
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 22
            anchors.rightMargin: 20
            spacing: 14
            Rectangle {
                width: 34; height: 34; radius: 10
                color: "#1d3e56"
                border.color: "#3876a0"
                Text {
                    anchors.centerIn: parent
                    text: "G"; font.pixelSize: 20; font.bold: true; color: "#75ceff"
                }
            }
            ColumnLayout {
                spacing: 1
                Label { text: "GEOMAP AI"; font.bold: true; font.letterSpacing: 2; font.pixelSize: 14; color: window.white }
                Label { text: "PHOTOGRAMMETRY WORKSPACE"; font.pixelSize: 9; font.letterSpacing: 1.3; color: window.muted }
            }
            Rectangle { width: 1; height: 30; color: window.line; Layout.leftMargin: 14; Layout.rightMargin: 4 }
            Label {
                text: projects.hasProject ? projects.projectName : "Проект не открыт"
                font.pixelSize: 13
                color: "#b7c6d8"
                elide: Text.ElideMiddle
                Layout.maximumWidth: 250
            }
            Item { Layout.fillWidth: true }
            FlatButton { text: "+ Новый проект"; compact: true; enabled: !projects.busy; onClicked: createDialog.open() }
            FlatButton { text: "Открыть"; compact: true; enabled: !projects.busy; onClicked: openProjectDialog.open() }
            FlatButton { text: "Сохранить"; compact: true; enabled: projects.hasProject && !projects.busy; onClicked: projects.saveProject() }
            Rectangle { width: 1; height: 28; color: window.line }
            Rectangle {
                implicitWidth: versionBadge.implicitWidth + 24; height: 28; radius: 7
                color: "#1b3230"
                border.color: "#305c51"
                Label {
                    id: versionBadge
                    anchors.centerIn: parent
                    text: "LOCAL  •  " + Qt.application.version.split(".").slice(0, 2).join(".")
                    color: "#82d7b7"; font.pixelSize: 10; font.bold: true
                }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Left: project tree and photo list
        Rectangle {
            Layout.preferredWidth: 292
            Layout.fillHeight: true
            color: window.surface
            border.color: window.line
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 11
                RowLayout {
                    Layout.fillWidth: true
                    SectionTitle { text: "ПРОЕКТ" }
                    Item { Layout.fillWidth: true }
                    Label { text: projects.hasProject ? "АКТИВЕН" : "НЕ ОТКРЫТ"; color: projects.hasProject ? "#74cba7" : window.muted; font.pixelSize: 10 }
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 68
                    radius: 9
                    color: "#202a37"
                    border.color: "#354459"
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 9
                        Rectangle {
                            width: 36; height: 36; radius: 8; color: "#294863"
                            Label { anchors.centerIn: parent; text: "▣"; color: "#80cfff"; font.pixelSize: 19 }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Label {
                                text: projects.hasProject ? projects.projectName : "Нет проекта"
                                Layout.fillWidth: true; elide: Text.ElideRight
                                font.bold: true; font.pixelSize: 12; color: window.white
                            }
                            Label {
                                text: projects.hasProject ? projects.photoCount + " фотографий" : "Создайте или откройте"
                                color: window.muted; font.pixelSize: 11
                            }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    FlatButton { text: "+ Файлы"; Layout.fillWidth: true; enabled: projects.hasProject && !projects.busy; onClicked: photoDialog.open() }
                    FlatButton { text: "+ Папка"; Layout.fillWidth: true; enabled: projects.hasProject && !projects.busy; onClicked: photoFolderDialog.open() }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: window.line; Layout.topMargin: 5; Layout.bottomMargin: 6 }
                RowLayout {
                    Layout.fillWidth: true
                    SectionTitle { text: "ФОТОГРАФИИ" }
                    Item { Layout.fillWidth: true }
                    Label { text: projects.photoCount.toString(); font.pixelSize: 11; color: window.muted }
                }
                ListView {
                    id: photoList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 5
                    model: projects.photoModel
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    delegate: Rectangle {
                        required property int index
                        required property string photoName
                        required property url photoThumb
                        required property bool photoHasGps
                        required property int photoWidth
                        required property int photoHeight
                        width: photoList.width - 8
                        height: 72
                        radius: 9
                        color: projects.selectedIndex === index ? "#25465f" : (photoMouse.containsMouse ? "#263341" : "transparent")
                        border.color: projects.selectedIndex === index ? "#4e95c2" : "transparent"
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 10
                            Rectangle {
                                width: 78; height: 54; radius: 5; color: "#111b25"; clip: true
                                // Pre-rotated preview from the project's thumbnail cache.
                                Image {
                                    anchors.fill: parent
                                    source: photoThumb
                                    sourceSize.width: 156
                                    sourceSize.height: 108
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                    cache: true
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                Label {
                                    text: photoName
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                    color: window.white; font.pixelSize: 11; font.bold: true
                                }
                                Label { text: photoWidth + " × " + photoHeight; color: window.muted; font.pixelSize: 10 }
                                Label { text: photoHasGps ? "● GPS" : "○ Без GPS"; color: photoHasGps ? "#7bd0b1" : "#8c99a8"; font.pixelSize: 10 }
                            }
                        }
                        MouseArea {
                            id: photoMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: { projects.selectPhoto(index); window.activeView = 0 }
                        }
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: projects.hasProject && projects.photoCount === 0
                        text: "Список пуст\n\nДобавьте снимки с БПЛА"
                        horizontalAlignment: Text.AlignHCenter
                        color: window.muted; font.pixelSize: 12
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: window.line }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "GPS снимков"; color: window.muted; font.pixelSize: 11 }
                    Item { Layout.fillWidth: true }
                    Label { text: projects.gpsCount + " / " + projects.photoCount; color: "#a8dbd0"; font.pixelSize: 11; font.bold: true }
                }
            }
        }

        // Center workspace
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: window.bg
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Rectangle {
                    Layout.fillWidth: true
                    height: 55
                    color: "#151b24"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 18
                        anchors.rightMargin: 18
                        spacing: 8
                        FlatButton {
                            text: "▣  Снимки"; compact: true
                            emphasis: window.activeView === 0
                            enabled: projects.hasProject
                            onClicked: window.activeView = 0
                        }
                        FlatButton {
                            text: "⌖  GPS-обзор"; compact: true
                            emphasis: window.activeView === 1
                            enabled: projects.hasProject
                            onClicked: window.activeView = 1
                        }
                        FlatButton {
                            text: "◈  Обработка"; compact: true
                            emphasis: window.activeView === 2
                            enabled: projects.hasProject
                            onClicked: window.activeView = 2
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: window.activeView === 0 ? (projects.photoCount ? "ПРОСМОТР ФОТОГРАФИЙ" : "ПУСТОЙ ПРОЕКТ")
                                : window.activeView === 1 ? "СХЕМА GPS-ТОЧЕК · БЕЗ ПОДЛОЖКИ"
                                : "ФОТОГРАММЕТРИЯ · " + processing.engineName.toUpperCase()
                            color: window.muted; font.pixelSize: 10; font.letterSpacing: 0.7
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: window.line }

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
                                                    // Runtime choice: bundled ODM (no Docker) or Docker; automatic prefers bundled.
                                                    ComboBox {
                                                        id: backendBox
                                                        visible: processing.backends.length > 0 || processing.preferredBackend.length > 0
                                                        implicitHeight: 28
                                                        font.pixelSize: 11
                                                        enabled: !processing.active && !processing.installing && !processing.checkingEnvironment
                                                        textRole: "title"
                                                        valueRole: "value"
                                                        model: [{ value: "", title: "Режим: авто" }, { value: "native", title: "Встроенный" },
                                                                { value: "docker", title: "Docker" }]
                                                        currentIndex: indexOfValue(processing.preferredBackend)
                                                        onActivated: processing.preferredBackend = currentValue
                                                    }
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

        // Right properties inspector (photo details; the processing view uses the width itself)
        Rectangle {
            Layout.preferredWidth: 275
            Layout.fillHeight: true
            visible: window.activeView !== 2
            color: window.surface
            border.color: window.line
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 17
                spacing: 15
                // Details scroll on short screens (e.g. 1920x1080 at 125 %) so the actions stay visible.
                ScrollView {
                    id: inspectorScroll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true
                    ColumnLayout {
                        width: inspectorScroll.availableWidth
                        spacing: 15
                        SectionTitle { text: "ИНСПЕКТОР" }
                        Rectangle { Layout.fillWidth: true; height: 1; color: window.line }
                        SectionTitle { text: "ПРОЕКТ" }
                        DetailLine { field: "Название"; value: projects.hasProject ? projects.projectName : "—"; Layout.fillWidth: true }
                        DetailLine { field: "Снимков"; value: projects.photoCount.toString(); Layout.fillWidth: true }
                        DetailLine { field: "С геометками"; value: projects.gpsCount.toString(); Layout.fillWidth: true }
                        Rectangle { Layout.fillWidth: true; height: 1; color: window.line }
                        SectionTitle { text: "ВЫБРАННЫЙ СНИМОК" }
                        DetailLine { field: "Имя файла"; value: projects.selectedPhoto.name || "—"; Layout.fillWidth: true }
                        DetailLine { field: "Размер"; value: projects.selectedPhoto.width ? projects.selectedPhoto.width + " × " + projects.selectedPhoto.height : "—"; Layout.fillWidth: true }
                        DetailLine { field: "Объём"; value: projects.selectedIndex >= 0 ? window.prettyBytes(projects.selectedPhoto.bytes) : "—"; Layout.fillWidth: true }
                        DetailLine { field: "Камера"; value: projects.selectedPhoto.camera || "—"; Layout.fillWidth: true }
                        DetailLine { field: "Дата съёмки (EXIF)"; value: projects.selectedPhoto.takenAt || "—"; Layout.fillWidth: true }
                        DetailLine {
                            field: "GPS координаты"
                            value: projects.selectedPhoto.hasGps
                                ? window.coord(projects.selectedPhoto.latitude) + "\n" + window.coord(projects.selectedPhoto.longitude)
                                : "Не обнаружены"
                            Layout.fillWidth: true
                        }
                    }
                }
                FlatButton {
                    Layout.fillWidth: true
                    text: "Открыть папку проекта"
                    enabled: projects.hasProject
                    onClicked: projects.revealProject()
                }
                FlatButton {
                    Layout.fillWidth: true
                    text: "Закрыть проект"
                    enabled: projects.hasProject && !projects.busy
                    onClicked: window.guardProcessing(function() { projects.closeProject() })
                }
                FlatButton {
                    Layout.fillWidth: true
                    text: "Удалить снимок из проекта"
                    caution: true
                    enabled: projects.selectedIndex >= 0 && !projects.busy
                    onClicked: deleteDialog.open()
                }
            }
        }
    }

    footer: Rectangle {
        height: 34
        color: "#121922"
        border.color: window.line
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 8
            Rectangle { width: 7; height: 7; radius: 4; color: projects.busy ? "#f5c77a" : "#61cea7" }
            Label {
                visible: !projects.busy
                text: projects.statusMessage
                color: "#a9b9ca"; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true
            }
            // Background import: real per-file progress reported by the worker.
            RowLayout {
                visible: projects.busy
                Layout.fillWidth: true
                spacing: 10
                Label {
                    text: projects.importCancelling ? "Отмена импорта…"
                        : projects.importScanning ? "Поиск снимков… найдено " + projects.importTotal
                        : "Импорт " + projects.importProcessed + " из " + projects.importTotal
                    color: "#e2edfa"; font.pixelSize: 11
                }
                Rectangle {
                    visible: !projects.importScanning && projects.importTotal > 0
                    Layout.preferredWidth: 220
                    height: 6; radius: 3; color: "#26303b"
                    Rectangle {
                        width: parent.width * Math.min(1, projects.importProcessed / Math.max(1, projects.importTotal))
                        height: parent.height; radius: 3; color: window.accent
                    }
                }
                Label {
                    text: projects.importCurrentFile
                    color: "#8d9aaa"; font.pixelSize: 11; elide: Text.ElideMiddle; Layout.fillWidth: true
                }
                FlatButton {
                    text: "Отменить импорт"; compact: true; implicitHeight: 24
                    enabled: !projects.importCancelling
                    onClicked: projects.cancelImport()
                }
            }
            FlatButton {
                visible: !projects.busy && projects.importIssues.length > 0
                text: "Пропущенные файлы: " + projects.importIssues.length
                compact: true; implicitHeight: 24
                onClicked: issuesDialog.open()
            }
            // Running processing job (real engine stage), also while another view is shown.
            FlatButton {
                visible: processing.active
                text: window.activeJobText.length > 0 ? window.activeJobText : "Обработка"
                compact: true; implicitHeight: 24
                Layout.maximumWidth: 380
                onClicked: {
                    window.activeView = 2
                    if (processing.activeJobId.length > 0) window.selectedJobId = processing.activeJobId
                }
            }
            Label { text: "OFFLINE  /  OPEN SOURCE  /  v" + Qt.application.version; color: "#71849a"; font.pixelSize: 10; font.letterSpacing: 0.6 }
        }
    }
}
