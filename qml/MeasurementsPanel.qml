import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Saved measurements of the project (map view, right panel): name, kind, result, model and
// method; a click shows the measurement on the map, double click renames, CSV export.
Item {
    id: panel
    property var host

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.panelHeader
            color: Theme.toolbar
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10; anchors.rightMargin: 4
                spacing: 4
                SectionTitle { text: "ИЗМЕРЕНИЯ" }
                Label { text: gis.measurements.length.toString(); color: Theme.faint; font.pixelSize: 10 }
                Item { Layout.fillWidth: true }
                FlatButton {
                    text: "CSV…"; compact: true; implicitHeight: 24
                    enabled: gis.measurements.length > 0
                    onClicked: exportDialog.open()
                    ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Сохранить все измерения в CSV (с геометрией WKT в WGS 84)"
                }
            }
        }
        Label {
            Layout.fillWidth: true
            Layout.margins: 10
            visible: gis.measurements.length === 0
            wrapMode: Text.Wrap
            text: "Сохранённых измерений нет. Измерьте длину, площадь, профиль или объём на карте и нажмите «Сохранить» в табличке результата."
            color: Theme.faint; font.pixelSize: 11
        }
        ListView {
            id: list
            objectName: "measurementList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: gis.measurements
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                id: row
                required property var modelData
                width: list.width - 10
                x: 5
                height: column.implicitHeight + 10
                radius: 4
                color: modelData.shown ? Theme.accentSoft : rowMouse.containsMouse ? Theme.hover : "transparent"
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: gis.showMeasurement(row.modelData.shown ? "" : row.modelData.id)
                    onDoubleClicked: { nameField.text = row.modelData.name; nameField.visible = true; nameField.forceActiveFocus(); nameField.selectAll() }
                }
                ColumnLayout {
                    id: column
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.margins: 5
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            visible: !nameField.visible
                            text: row.modelData.name
                            elide: Text.ElideRight
                            color: Theme.text; font.pixelSize: 12; font.bold: true
                        }
                        TextField {
                            id: nameField
                            Layout.fillWidth: true
                            visible: false
                            implicitHeight: 24; font.pixelSize: 12
                            onAccepted: { gis.renameMeasurement(row.modelData.id, text); visible = false }
                            onActiveFocusChanged: if (!activeFocus) visible = false
                            Keys.onEscapePressed: visible = false
                        }
                        Label { text: row.modelData.kindTitle; color: Theme.faint; font.pixelSize: 10 }
                        ToolButton {
                            implicitWidth: 20; implicitHeight: 20; padding: 0
                            display: AbstractButton.IconOnly
                            icon.source: Theme.icon("close"); icon.width: 12; icon.height: 12; icon.color: Theme.muted
                            text: "Удалить измерение"
                            onClicked: gis.removeMeasurement(row.modelData.id)
                            ToolTip.visible: hovered; ToolTip.text: text; ToolTip.delay: 500
                            background: Rectangle { radius: 3; color: parent.hovered ? Theme.hover : "transparent" }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: row.modelData.text
                        color: "#FFD666"; font.pixelSize: 11
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: row.modelData.shown
                        wrapMode: Text.Wrap
                        text: row.modelData.details + "\nМетод: " + row.modelData.method + "\nCRS карты: " + row.modelData.crs
                        color: Theme.muted; font.pixelSize: 10
                    }
                    Label {
                        text: row.modelData.created + (row.modelData.layer.length > 0 ? " · «" + row.modelData.layer + "»" : "")
                        color: Theme.faint; font.pixelSize: 9
                    }
                }
            }
        }
    }
    FileDialog {
        id: exportDialog
        title: "Сохранить измерения"
        fileMode: FileDialog.SaveFile
        nameFilters: ["CSV (*.csv)"]
        defaultSuffix: "csv"
        onAccepted: {
            var error = gis.exportMeasurements(selectedFile)
            if (error.length > 0) panel.host.showError(error)
            else panel.host.flash("Измерения сохранены: " + gis.displayPath(selectedFile))
        }
    }
}
