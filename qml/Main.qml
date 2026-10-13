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

    Shortcut { sequence: StandardKey.New; onActivated: createDialog.open() }
    Shortcut { sequence: StandardKey.Open; onActivated: openProjectDialog.open() }
    Shortcut { sequence: StandardKey.Save; onActivated: projects.saveProject() }
    Shortcut { sequence: "Ctrl+W"; onActivated: projects.closeProject() }
    Shortcut { sequence: "Ctrl+I"; onActivated: { if (projects.hasProject) photoDialog.open() } }

    Connections {
        target: projects
        function onErrorOccurred(message) {
            window.lastError = message
            errorDialog.open()
        }
        function onProjectChanged() {
            window.activeView = 0
            window.imageZoom = 1.0
            gpsCanvas.requestPaint()
        }
        function onPhotosChanged() { gpsCanvas.requestPaint() }
        function onSelectionChanged() {
            window.imageZoom = 1.0
            gpsCanvas.requestPaint()
        }
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
        onAccepted: projects.openProject(selectedFolder)
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
                    onClicked: {
                        if (projects.createProject(window.newProjectFolder, projectNameField.text)) createDialog.close()
                    }
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
            FlatButton { text: "+ Новый проект"; compact: true; onClicked: createDialog.open() }
            FlatButton { text: "Открыть"; compact: true; onClicked: openProjectDialog.open() }
            FlatButton { text: "Сохранить"; compact: true; enabled: projects.hasProject && !projects.busy; onClicked: projects.saveProject() }
            Rectangle { width: 1; height: 28; color: window.line }
            Rectangle {
                implicitWidth: 91; height: 28; radius: 7
                color: "#1b3230"
                border.color: "#305c51"
                Label { anchors.centerIn: parent; text: "LOCAL  •  0.1"; color: "#82d7b7"; font.pixelSize: 10; font.bold: true }
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
                        required property url photoUrl
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
                                Image {
                                    anchors.fill: parent
                                    source: photoUrl
                                    sourceSize.width: 156
                                    sourceSize.height: 108
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                    cache: true
                                    autoTransform: true
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
                        Item { Layout.fillWidth: true }
                        Label {
                            text: window.activeView === 0 ? (projects.photoCount ? "ПРОСМОТР ФОТОГРАФИЙ" : "ПУСТОЙ ПРОЕКТ") : "СХЕМА GPS-ТОЧЕК · БЕЗ ПОДЛОЖКИ"
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
                        visible: projects.hasProject && projects.photoCount === 0
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
                            FlatButton { text: "Добавить фотографии"; emphasis: true; onClicked: photoDialog.open() }
                            FlatButton { text: "Импортировать папку"; onClicked: photoFolderDialog.open() }
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
                                        var ctx = getContext("2d")
                                        ctx.clearRect(0, 0, width, height)
                                        ctx.strokeStyle = "#263d4a"
                                        ctx.lineWidth = 1
                                        for (var g=0;g<=10;g++) {
                                            ctx.beginPath(); ctx.moveTo(g * width / 10, 0); ctx.lineTo(g * width / 10, height); ctx.stroke()
                                            ctx.beginPath(); ctx.moveTo(0, g * height / 10); ctx.lineTo(width, g * height / 10); ctx.stroke()
                                        }
                                        var pts = projects.geoPoints
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
                            required property url photoUrl
                            width: 132; height: 90
                            radius: 7; color: "#202b37"; clip: true
                            border.color: index === projects.selectedIndex ? "#6ac9ff" : "#3a4554"
                            border.width: index === projects.selectedIndex ? 2 : 1
                            Image {
                                anchors.fill: parent; anchors.margins: 3
                                source: photoUrl; sourceSize.width: 264; sourceSize.height: 180
                                fillMode: Image.PreserveAspectCrop; asynchronous: true; autoTransform: true
                            }
                            MouseArea { anchors.fill: parent; onClicked: projects.selectPhoto(index) }
                        }
                    }
                }
            }
        }

        // Right properties inspector
        Rectangle {
            Layout.preferredWidth: 275
            Layout.fillHeight: true
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
                    onClicked: projects.closeProject()
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
            Label { text: projects.busy ? "Импорт выполняется" : projects.statusMessage; color: "#a9b9ca"; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true }
            Label { text: "OFFLINE  /  OPEN SOURCE  /  v0.1.0"; color: "#71849a"; font.pixelSize: 10; font.letterSpacing: 0.6 }
        }
    }
}
