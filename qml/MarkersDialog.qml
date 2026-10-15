import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Markers of the project: named points with WGS 84 coordinates and the height of the elevation
// model under them; rename, note, show on the map, delete, export CSV.
Dialog {
    id: dialog
    objectName: "markersDialog"
    title: "Маркеры"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 720
    height: Math.min(560, Overlay.overlay ? Overlay.overlay.height - 40 : 560)
    padding: 16
    standardButtons: Dialog.Close
    property var host

    function fmt(v, d) { return v.toLocaleString(Qt.locale("ru_RU"), 'f', d) }

    contentItem: ColumnLayout {
        spacing: 8
        RowLayout {
            spacing: 8
            FlatButton {
                text: "Поставить маркер"; compact: true; emphasis: true; enabled: gis.crsDefined
                onClicked: { dialog.close(); dialog.host.runCommand("tool.marker") }
            }
            FlatButton { text: "Экспорт CSV…"; compact: true; enabled: gis.markers.length > 0; onClicked: markersFile.open() }
            CheckBox { text: "Показывать на карте"; checked: gis.showMarkers; onToggled: gis.showMarkers = checked; font.pixelSize: 11 }
            Item { Layout.fillWidth: true }
            Label { text: "Маркеров: " + gis.markers.length; color: Theme.muted; font.pixelSize: 11 }
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: gis.markers
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width - 12
                height: row.implicitHeight + 12
                radius: 5
                color: Theme.field
                border.color: Theme.line
                RowLayout {
                    id: row
                    anchors.fill: parent; anchors.margins: 6
                    spacing: 8
                    TextField {
                        implicitWidth: 120; text: modelData.name; selectByMouse: true; font.pixelSize: 12
                        onEditingFinished: gis.renameMarker(modelData.id, text)
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Label {
                            text: dialog.fmt(modelData.lat, 7) + "°, " + dialog.fmt(modelData.lon, 7) + "°"
                                  + (modelData.hasZ ? " · высота " + dialog.fmt(modelData.z, 2) + " м (" + modelData.heightSource + ")" : " · высоты нет")
                            color: Theme.text; font.pixelSize: 11
                        }
                        Label {
                            visible: isFinite(modelData.x)
                            text: "X " + dialog.fmt(modelData.x, 3) + "  Y " + dialog.fmt(modelData.y, 3) + " · " + gis.crsLabel
                            color: Theme.faint; font.pixelSize: 10
                        }
                        TextField {
                            Layout.fillWidth: true; placeholderText: "Заметка"; text: modelData.note; selectByMouse: true; font.pixelSize: 11
                            onEditingFinished: gis.setMarkerNote(modelData.id, text)
                        }
                    }
                    FlatButton { text: "Показать"; compact: true; onClicked: { gis.zoomToMarker(modelData.id); dialog.host.runCommand("view.map") } }
                    FlatButton { text: "Удалить"; compact: true; caution: true; onClicked: gis.removeMarker(modelData.id) }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: gis.markers.length === 0
                width: parent.width - 40
                wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter
                text: "Маркеров пока нет. «Поставить маркер» и щелчок по карте — точка с координатами и высотой модели под ней."
                color: Theme.faint
            }
        }
    }
    FileDialog {
        id: markersFile
        title: "Сохранить маркеры"
        fileMode: FileDialog.SaveFile
        nameFilters: ["CSV (*.csv)"]
        defaultSuffix: "csv"
        onAccepted: {
            var error = gis.exportMarkers(selectedFile)
            if (error.length > 0) dialog.host.showError(error)
            else dialog.host.flash("Маркеры сохранены: " + gis.displayPath(selectedFile))
        }
    }
}
