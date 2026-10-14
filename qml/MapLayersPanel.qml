import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Map layers (right panel of the map view): CRS of the map, layer list top first with
// visibility, legend swatch, order, summary and status.
Item {
    id: panel
    property var controller

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
                SectionTitle { text: "СЛОИ КАРТЫ" }
                Label { text: gis.layerCount.toString(); color: Theme.faint; font.pixelSize: 10 }
                Item { Layout.fillWidth: true }
                FlatButton { text: "+ Слой…"; compact: true; implicitHeight: 24; onClicked: panel.controller.addLayer("data") }
                FlatButton { text: "CRS"; compact: true; implicitHeight: 24; onClicked: panel.controller.chooseMapCrs()
                             ToolTip.visible: hovered; ToolTip.text: "Система координат карты"; ToolTip.delay: 500 }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            Layout.bottomMargin: 4
            spacing: 3
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: "CRS карты: " + gis.crsLabel
                color: gis.crsDefined ? Theme.text : Theme.warn; font.pixelSize: 11
            }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                visible: gis.crsNote.length > 0
                text: "Выбрана " + gis.crsNote
                color: Theme.faint; font.pixelSize: 10
            }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                visible: gis.problems.length > 0
                text: "⚠ " + gis.problems.join("\n⚠ ")
                color: Theme.bad; font.pixelSize: 11
            }
        }
        ListView {
            id: layerList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: gis.layerModel
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                id: layerRow
                required property string layerId
                required property string name
                required property string layerType
                required property bool layerVisible
                required property string status
                required property string summary
                required property bool needsCrs
                required property bool loading
                required property color layerColor
                required property var legend
                readonly property bool selected: gis.selectedLayer === layerId
                width: layerList.width - 10
                x: 5
                height: layerColumn.implicitHeight + 10
                radius: 4
                color: selected ? Theme.accentSoft : rowMouse.containsMouse ? Theme.hover : "transparent"
                Accessible.role: Accessible.ListItem
                Accessible.name: "Слой " + name
                Accessible.onPressAction: gis.selectedLayer = layerId
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: gis.selectedLayer = layerRow.layerId
                    onDoubleClicked: gis.zoomToLayer(layerRow.layerId)
                }
                ColumnLayout {
                    id: layerColumn
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.margins: 5
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        CheckBox {
                            checked: layerRow.layerVisible
                            padding: 0
                            implicitWidth: 20; implicitHeight: 20
                            onToggled: gis.setLayerVisible(layerRow.layerId, checked)
                            Accessible.name: "Показать слой " + layerRow.name
                        }
                        Rectangle {
                            implicitWidth: 16; implicitHeight: 11; radius: 2
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0; color: layerRow.legend.length > 0 ? layerRow.legend[0].color : (layerRow.layerType === "raster" ? "#6e8c6e" : layerRow.layerColor) }
                                GradientStop { position: 1; color: layerRow.legend.length > 0 ? layerRow.legend[layerRow.legend.length - 1].color : (layerRow.layerType === "raster" ? "#9ab09a" : layerRow.layerColor) }
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: layerRow.name
                            elide: Text.ElideMiddle
                            color: Theme.text; font.pixelSize: 12; font.bold: true
                        }
                        ToolButton {
                            implicitWidth: 20; implicitHeight: 20; padding: 0
                            display: AbstractButton.IconOnly
                            icon.source: Theme.icon("chevron-up"); icon.width: 14; icon.height: 14; icon.color: Theme.muted
                            text: "Выше (рисуется поверх)"
                            onClicked: gis.moveLayer(layerRow.layerId, 1)
                            ToolTip.visible: hovered; ToolTip.text: text; ToolTip.delay: 500
                            background: Rectangle { radius: 3; color: parent.hovered ? Theme.hover : "transparent" }
                        }
                        ToolButton {
                            implicitWidth: 20; implicitHeight: 20; padding: 0
                            display: AbstractButton.IconOnly
                            icon.source: Theme.icon("chevron-down"); icon.width: 14; icon.height: 14; icon.color: Theme.muted
                            text: "Ниже"
                            onClicked: gis.moveLayer(layerRow.layerId, -1)
                            ToolTip.visible: hovered; ToolTip.text: text; ToolTip.delay: 500
                            background: Rectangle { radius: 3; color: parent.hovered ? Theme.hover : "transparent" }
                        }
                    }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        Layout.leftMargin: 25
                        visible: layerRow.summary.length > 0
                        text: layerRow.summary
                        color: Theme.muted; font.pixelSize: 10
                    }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        Layout.leftMargin: 25
                        visible: layerRow.status.length > 0
                        text: (layerRow.loading ? "⏳ " : "⚠ ") + layerRow.status
                        color: layerRow.loading ? "#9fc3f0" : Theme.warn; font.pixelSize: 10
                    }
                    FlatButton {
                        Layout.leftMargin: 25
                        visible: layerRow.needsCrs
                        text: "Указать CRS…"; compact: true; implicitHeight: 24
                        onClicked: panel.controller.assignLayerCrs(layerRow.layerId)
                    }
                }
            }
        }
    }
}
