import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Right panel of the photo views: the project and the selected photo (EXIF, GPS).
Item {
    id: inspector
    property var host

    function prettyBytes(b) {
        if (b === undefined || b === null) return "—"
        if (b < 1024) return b + " B"
        if (b < 1048576) return (b / 1024).toFixed(1) + " KB"
        if (b < 1073741824) return (b / 1048576).toFixed(1) + " MB"
        return (b / 1073741824).toFixed(2) + " GB"
    }
    function coord(x) { return Number(x).toFixed(6) + "°" }
    // Re-evaluated on every change of the chunks (their notify signal).
    readonly property bool photoOn: projects.selectedIndex < 0 || chunks.activeDisabledCount < 0 || chunks.photoEnabled(projects.selectedPhoto.id || "")

    component DetailLine: ColumnLayout {
        id: detailLine
        property string field: ""
        property string value: "—"
        spacing: 3
        Layout.fillWidth: true
        Label { text: detailLine.field; color: Theme.muted; font.pixelSize: 10 }
        Label {
            text: detailLine.value && detailLine.value.length > 0 ? detailLine.value : "—"
            color: Theme.text; font.pixelSize: 12
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.panelHeader
            color: Theme.toolbar
            SectionTitle { anchors.verticalCenter: parent.verticalCenter; x: 10; text: "СВОЙСТВА СНИМКА" }
        }
        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                width: scroll.availableWidth - 24
                x: 12
                spacing: 12
                Item { implicitHeight: 2 }
                SectionTitle { text: "ПРОЕКТ" }
                DetailLine { field: "Название"; value: projects.hasProject ? projects.projectName : "—" }
                DetailLine { field: "Папка"; value: projects.hasProject ? projects.projectPath : "—" }
                DetailLine { field: "Снимков / с GPS"; value: projects.photoCount + " / " + projects.gpsCount }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
                SectionTitle { text: "ВЫБРАННЫЙ СНИМОК" }
                DetailLine { field: "Имя файла"; value: projects.selectedPhoto.name || "—" }
                DetailLine { field: "Размер"; value: projects.selectedPhoto.width ? projects.selectedPhoto.width + " × " + projects.selectedPhoto.height : "—" }
                DetailLine { field: "Объём"; value: projects.selectedIndex >= 0 ? inspector.prettyBytes(projects.selectedPhoto.bytes) : "—" }
                DetailLine { field: "Камера"; value: projects.selectedPhoto.camera || "—" }
                DetailLine { field: "Дата съёмки (EXIF)"; value: projects.selectedPhoto.takenAt || "—" }
                DetailLine {
                    field: "GPS (WGS 84)"
                    value: projects.selectedIndex < 0 ? "—" : projects.selectedPhoto.hasGps
                        ? inspector.coord(projects.selectedPhoto.latitude) + "\n" + inspector.coord(projects.selectedPhoto.longitude)
                        : "Не обнаружены"
                }
                ColumnLayout {
                    objectName: "photoFlight"
                    // Read from the photo's XMP whenever the selection changes.
                    readonly property var info: projects.selectedIndex >= 0 ? projects.flightInfo(projects.selectedIndex) : ({})
                    visible: !!(info.height || info.gimbal || info.drone || info.gnss)
                    Layout.fillWidth: true
                    spacing: 12
                    DetailLine { visible: !!parent.info.height; field: "Высота"; value: parent.info.height || "" }
                    DetailLine { visible: !!parent.info.gimbal; field: "Подвес"; value: parent.info.gimbal || "" }
                    DetailLine { visible: !!parent.info.drone; field: "Дрон"; value: parent.info.drone || "" }
                    DetailLine {
                        objectName: "photoGnss"
                        visible: !!parent.info.gnss
                        field: "ГНСС"
                        value: parent.info.gnss || ""
                        ToolTip.visible: gnssHover.hovered; ToolTip.delay: 400
                        ToolTip.text: "Из XMP снимка: решение и σ положения (в плане / по высоте). OpenDroneMap учитывает её при выравнивании"
                        HoverHandler { id: gnssHover }
                    }
                }
                DetailLine {
                    objectName: "photoQualityLine"
                    field: "Качество"
                    readonly property real q: projects.selectedIndex >= 0 && photoQuality.measured >= 0 ? photoQuality.qualityOf(projects.selectedPhoto.id || "") : -1
                    value: projects.selectedIndex < 0 ? "—"
                         : (q < 0 ? "не оценено" : q.toLocaleString(Qt.locale("ru_RU"), 'f', 2) + (q < photoQuality.threshold ? " · нечёткий" : ""))
                           + (inspector.photoOn ? "" : " · отключён")
                }
                Item { implicitHeight: 4 }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            spacing: 6
            FlatButton {
                Layout.fillWidth: true; compact: true
                text: commands.title("project.reveal")
                enabled: inspector.host && inspector.host.commandEnabled("project.reveal")
                onClicked: inspector.host.runCommand("project.reveal")
            }
            FlatButton {
                objectName: "photoToggle"
                Layout.fillWidth: true; compact: true
                text: inspector.photoOn ? "Отключить снимок" : "Включить снимок"
                enabled: projects.selectedIndex >= 0
                onClicked: {
                    var e = chunks.setPhotosEnabled([projects.selectedPhoto.id], !inspector.photoOn)
                    if (e.length && inspector.host) inspector.host.showError(e)
                }
                ToolTip.visible: hovered; ToolTip.delay: 500
                ToolTip.text: inspector.photoOn ? "Снимок останется в проекте, но не пойдёт в обработку блока" : "Вернуть снимок в обработку блока"
            }
            FlatButton {
                Layout.fillWidth: true; compact: true; caution: true
                text: "Удалить снимок из проекта"
                enabled: inspector.host && inspector.host.commandEnabled("project.removePhoto")
                onClicked: inspector.host.runCommand("project.removePhoto")
            }
        }
    }
}
