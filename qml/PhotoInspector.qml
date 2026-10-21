import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Right panel of the photo view: the selected photo in a short table (EXIF, GPS, flight data from
// XMP, quality), switching it off in the chunk and removing it.
Item {
    id: inspector
    property var host

    function prettyBytes(b) {
        if (b === undefined || b === null) return ""
        var ru = Qt.locale("ru_RU")
        if (b < 1048576) return Math.max(1, Math.round(b / 1024)) + " КБ"
        if (b < 1073741824) return (b / 1048576).toLocaleString(ru, 'f', 1) + " МБ"
        return (b / 1073741824).toLocaleString(ru, 'f', 2) + " ГБ"
    }
    function coord(x) { return Number(x).toLocaleString(Qt.locale("ru_RU"), 'f', 6) + "°" }
    readonly property bool selected: projects.selectedIndex >= 0
    readonly property var photo: projects.selectedPhoto
    // Re-evaluated on every change of the chunks (their notify signal).
    readonly property bool photoOn: !selected || chunks.activeDisabledCount < 0 || chunks.photoEnabled(photo.id || "")

    // One row: a short name on the left, the value on the right; hidden when there is no value.
    component Field: RowLayout {
        id: field
        property string name: ""
        property string value: ""
        visible: value.length > 0
        Layout.fillWidth: true
        spacing: 8
        Label { text: field.name; color: Theme.muted; font.pixelSize: 11; Layout.preferredWidth: 62; Layout.alignment: Qt.AlignTop | Qt.AlignLeft }
        Label { text: field.value; color: Theme.text; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.panelHeader
            color: Theme.toolbar
            SectionTitle { anchors.verticalCenter: parent.verticalCenter; x: 10; text: "СНИМОК" }
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
                spacing: 7
                Item { implicitHeight: 4 }
                Label {
                    visible: !inspector.selected
                    text: projects.hasProject ? "Выберите снимок" : "Проект не открыт"
                    color: Theme.faint; font.pixelSize: 12
                }
                Label {
                    visible: inspector.selected
                    text: inspector.photo.name || ""
                    color: Theme.text; font.pixelSize: 14; font.weight: Font.DemiBold
                    elide: Text.ElideMiddle; Layout.fillWidth: true
                }
                Label {
                    visible: inspector.selected
                    text: [inspector.photo.width ? inspector.photo.width + " × " + inspector.photo.height : "",
                           inspector.prettyBytes(inspector.photo.bytes), inspector.photo.camera || ""].filter(function(s) { return s.length > 0 }).join(" · ")
                    color: Theme.muted; font.pixelSize: 11
                    elide: Text.ElideRight; Layout.fillWidth: true
                }
                Item { implicitHeight: 2 }
                Field { name: "Снято"; value: inspector.selected ? (inspector.photo.takenAt || "") : "" }
                Field {
                    name: "GPS"
                    value: !inspector.selected ? "" : inspector.photo.hasGps
                        ? inspector.coord(inspector.photo.latitude) + ", " + inspector.coord(inspector.photo.longitude) : "нет"
                }
                ColumnLayout {
                    objectName: "photoFlight"
                    // Read from the photo's XMP whenever the selection changes.
                    readonly property var info: inspector.selected ? projects.flightInfo(projects.selectedIndex) : ({})
                    visible: !!(info.height || info.gimbal || info.gnss)
                    Layout.fillWidth: true
                    spacing: 7
                    Field { name: "Высота"; value: parent.info.height || "" }
                    Field {
                        name: "Подвес"; value: parent.info.gimbal || ""
                        ToolTip.visible: gimbalHover.hovered && !!parent.info.drone; ToolTip.delay: 400
                        ToolTip.text: "Дрон: " + (parent.info.drone || "")
                        HoverHandler { id: gimbalHover }
                    }
                    Field {
                        objectName: "photoGnss"
                        name: "ГНСС"; value: parent.info.gnss || ""
                        ToolTip.visible: gnssHover.hovered; ToolTip.delay: 400
                        ToolTip.text: "Решение и σ положения (в плане / по высоте) из XMP. OpenDroneMap учитывает её при выравнивании"
                        HoverHandler { id: gnssHover }
                    }
                }
                Field {
                    objectName: "photoQualityLine"
                    readonly property real q: inspector.selected && photoQuality.measured >= 0 ? photoQuality.qualityOf(inspector.photo.id || "") : -1
                    name: "Качество"
                    value: !inspector.selected ? ""
                         : [q < 0 ? "" : q.toLocaleString(Qt.locale("ru_RU"), 'f', 2) + (q < photoQuality.threshold ? " · нечёткий" : ""),
                            inspector.photoOn ? "" : "отключён"].filter(function(s) { return s.length > 0 }).join(" · ")
                }
                Item { implicitHeight: 4 }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            spacing: 6
            FlatButton {
                objectName: "photoToggle"
                Layout.fillWidth: true; compact: true
                text: inspector.photoOn ? "Отключить снимок" : "Включить снимок"
                enabled: inspector.selected
                onClicked: {
                    var e = chunks.setPhotosEnabled([inspector.photo.id], !inspector.photoOn)
                    if (e.length && inspector.host) inspector.host.showError(e)
                }
                ToolTip.visible: hovered; ToolTip.delay: 500
                ToolTip.text: inspector.photoOn ? "Снимок останется в проекте, но не пойдёт в обработку блока" : "Вернуть снимок в обработку блока"
            }
            FlatButton {
                compact: true; caution: true
                text: "Удалить"
                enabled: inspector.host && inspector.host.commandEnabled("project.removePhoto")
                onClicked: inspector.host.runCommand("project.removePhoto")
                ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Удалить снимок из проекта"
            }
        }
    }
}
