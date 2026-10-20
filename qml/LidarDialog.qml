import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// LiDAR clouds of the project (Файл → Импорт LiDAR): what each LAS/LAZ holds, the cloud in 3D,
// a DSM or DTM built from it onto the map.
Dialog {
    id: dialog
    objectName: "lidarDialog"
    title: "LiDAR"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 760
    height: Math.min(560, Overlay.overlay ? Overlay.overlay.height - 40 : 560)
    padding: 16
    standardButtons: Dialog.Close
    property var host

    Connections {
        target: lidar
        function onFinished(ok, message, path) {
            if (ok) dialog.host.flash(message)
            else dialog.host.showError(message)
        }
    }

    contentItem: ColumnLayout {
        spacing: 10
        RowLayout {
            spacing: 8
            FlatButton {
                objectName: "lidarAdd"
                text: "Добавить LAS/LAZ…"; compact: true; emphasis: true
                enabled: !lidar.busy && lidar.available
                onClicked: lidarFiles.open()
            }
            Label {
                visible: !lidar.available
                text: "Нужен PDAL из состава OpenDroneMap"
                color: Theme.warn; font.pixelSize: 11
            }
            Item { Layout.fillWidth: true }
            Label { text: "Ячейка, м"; color: Theme.muted; font.pixelSize: 11 }
            TextField {
                id: cellField
                objectName: "lidarCell"
                implicitWidth: 80; implicitHeight: 28; font.pixelSize: 11
                placeholderText: "авто"
                validator: DoubleValidator { bottom: 0; locale: "ru_RU" }
                ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Размер ячейки DSM и DTM. Пусто — около двух точек на ячейку"
            }
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: lidar.clouds
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width - 12
                height: row.implicitHeight + 16
                radius: 6
                color: Theme.field
                border.color: Theme.line
                RowLayout {
                    id: row
                    anchors.fill: parent; anchors.margins: 8
                    spacing: 10
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: modelData.name; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold; elide: Text.ElideMiddle; Layout.fillWidth: true }
                        Label { text: modelData.summary; color: Theme.muted; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight }
                        Label {
                            text: modelData.crs + " · " + modelData.heights
                            color: Theme.faint; font.pixelSize: 10; Layout.fillWidth: true; elide: Text.ElideRight
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Label {
                                text: modelData.missing ? "Файл не найден: " + modelData.path : modelData.classes
                                color: modelData.missing ? Theme.bad : Theme.faint; font.pixelSize: 10; Layout.fillWidth: true; elide: Text.ElideRight
                            }
                            Label {
                                visible: modelData.dsm.length > 0 || modelData.dtm.length > 0
                                text: "✓ " + [modelData.dsm.length > 0 ? "DSM" : "", modelData.dtm.length > 0 ? "DTM" : ""].filter(function(s) { return s.length }).join(" · ")
                                color: Theme.good; font.pixelSize: 10; font.weight: Font.DemiBold
                            }
                        }
                    }
                    FlatButton {
                        text: "3D"; compact: true; enabled: !modelData.missing
                        onClicked: { var e = lidar.showIn3D(modelData.index); if (e.length) dialog.host.showError(e); else { dialog.close(); dialog.host.runCommand("view.3d") } }
                        ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Показать облако в 3D-сцене"
                    }
                    FlatButton {
                        objectName: "lidarDsm" + modelData.index
                        text: "DSM"; compact: true; enabled: !modelData.missing && !lidar.busy
                        onClicked: dialog.build(modelData.index, "dsm")
                        ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Модель поверхности: верх точек в каждой ячейке"
                    }
                    FlatButton {
                        objectName: "lidarDtm" + modelData.index
                        text: "DTM"; compact: true; enabled: !modelData.missing && !lidar.busy
                        onClicked: dialog.build(modelData.index, "dtm")
                        ToolTip.visible: hovered; ToolTip.delay: 500
                        ToolTip.text: modelData.hasGround ? "Модель рельефа по точкам земли из файла" : "Модель рельефа: земля найдётся фильтром SMRF"
                    }
                    ToolButton {
                        text: "✕"; font.pixelSize: 12; enabled: !lidar.busy
                        onClicked: lidar.remove(modelData.index)
                        ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Убрать из проекта (файл останется)"
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: lidar.count === 0
                text: "Облаков нет — добавьте LAS или LAZ"
                color: Theme.faint
            }
        }
        RowLayout {
            visible: lidar.busy
            spacing: 8
            BusyIndicator { running: lidar.busy; implicitWidth: 22; implicitHeight: 22 }
            Label { text: lidar.status; color: Theme.text; font.pixelSize: 12; Layout.fillWidth: true }
            FlatButton { text: "Отменить"; compact: true; caution: true; onClicked: lidar.cancel() }
        }
    }

    function build(index, kind) {
        var r = cellField.text.length > 0 ? Number.fromLocaleString(Qt.locale("ru_RU"), cellField.text) : 0
        var e = lidar.buildSurface(index, kind, isNaN(r) ? 0 : r)
        if (e.length) dialog.host.showError(e)
    }

    AppFileDialog {
        id: lidarFiles
        kind: "lidar"
        title: "Облака LiDAR"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["LiDAR (*.las *.laz)", "Все файлы (*)"]
        onAccepted: { var e = lidar.importUrls(selectedFiles); if (e.length) dialog.host.showError(e) }
    }
}
