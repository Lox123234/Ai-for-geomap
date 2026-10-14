import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoMapAI.Map

// Centre of the map view: context bar, the map (MapView painted by MapService), measurement
// box, tool hint and scale bar. Tools are chosen in the main toolbar or the Tools menu.
Item {
    id: canvas
    property var host
    property var controller

    readonly property var toolTitles: ({ "pan": "Перемещение", "length": "Линейка", "area": "Площадь",
                                         "point": "Точка", "line": "Линия", "polygon": "Полигон", "edit": "Правка / выбор" })

    function formatLength(m) {
        return m >= 1000 ? (m / 1000).toLocaleString(Qt.locale("ru_RU"), 'f', m >= 10000 ? 0 : 1) + " км"
                         : m.toLocaleString(Qt.locale("ru_RU"), 'f', m < 1 ? 2 : 0) + " м"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        // Context bar of the map
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 34
            color: Theme.panel
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10; anchors.rightMargin: 8
                spacing: 6
                Label { text: "Инструмент:"; color: Theme.faint; font.pixelSize: 11 }
                Label { text: canvas.toolTitles[gis.tool] || gis.tool; color: Theme.text; font.pixelSize: 11; font.bold: true }
                Rectangle { implicitWidth: 1; implicitHeight: 18; color: Theme.line; Layout.leftMargin: 6; Layout.rightMargin: 6 }
                FlatButton { text: "Все слои"; compact: true; implicitHeight: 24; enabled: gis.crsDefined; onClicked: gis.zoomToAll() }
                FlatButton { text: "Удалить объект"; compact: true; implicitHeight: 24; caution: true; enabled: gis.hasSelection
                             onClicked: canvas.host.runCommand("edit.delete") }
                Item { Layout.fillWidth: true }
                FlatButton { text: "Экспорт карты…"; compact: true; implicitHeight: 24
                             enabled: canvas.host && canvas.host.commandEnabled("export.map"); onClicked: canvas.host.runCommand("export.map") }
                FlatButton { text: "Экспорт объектов…"; compact: true; implicitHeight: 24
                             enabled: canvas.host && canvas.host.commandEnabled("export.drawings"); onClicked: canvas.host.runCommand("export.drawings") }
            }
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            MapView {
                id: mapView
                objectName: "mapView"
                anchors.fill: parent
                service: gis
            }
            Rectangle {
                visible: gis.measurementText.length > 0
                anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 12
                width: Math.min(parent.width - 24, measureColumn.implicitWidth + 24)
                height: measureColumn.implicitHeight + 16
                radius: 6
                color: "#E61A1F27"
                border.color: Theme.line
                ColumnLayout {
                    id: measureColumn
                    anchors.fill: parent; anchors.margins: 8
                    spacing: 3
                    Label { text: gis.measurementText; color: "#FFD666"; font.pixelSize: 14; font.bold: true }
                    Label {
                        Layout.maximumWidth: 560; wrapMode: Text.Wrap
                        visible: gis.measurementDetails.length > 0
                        text: gis.measurementDetails; color: Theme.muted; font.pixelSize: 10
                    }
                }
            }
            Label {
                visible: ["length", "area", "line", "polygon"].indexOf(gis.tool) >= 0
                anchors.horizontalCenter: parent.horizontalCenter; anchors.top: parent.top; anchors.topMargin: 12
                text: "Щелчок — вершина · двойной щелчок, Enter или правая кнопка — завершить · Backspace — убрать вершину · Esc — отмена"
                color: Theme.text; font.pixelSize: 11
                padding: 6
                background: Rectangle { color: "#CC1A1F27"; radius: 5 }
            }
            Label {
                anchors.centerIn: parent
                visible: !gis.crsDefined
                width: Math.min(parent.width - 60, 460)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: "Система координат карты ещё не выбрана.\nДобавьте снимки с GPS или слой с известной CRS (Файл → Импорт геоданных), либо задайте её: Проект → Система координат карты."
                color: Theme.muted; font.pixelSize: 13
            }
            // Scale bar: ground distance measured geodesically at the centre of the view.
            Item {
                visible: gis.scaleBarPixels > 0
                anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 14
                width: gis.scaleBarPixels; height: 34
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 3; color: "white" }
                Rectangle { anchors.bottom: parent.bottom; width: 2; height: 10; color: "white" }
                Rectangle { anchors.bottom: parent.bottom; anchors.right: parent.right; width: 2; height: 10; color: "white" }
                Label {
                    anchors.bottom: parent.bottom; anchors.bottomMargin: 8
                    text: canvas.formatLength(gis.scaleBarMeters) + "   " + gis.scaleText
                    color: "white"; font.pixelSize: 11; font.bold: true
                    style: Text.Outline; styleColor: "#10151c"
                }
            }
        }
    }
}
