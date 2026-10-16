import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Properties of the selected map layer (right panel of the map view): source and map CRS,
// file, opacity, raster style or vector colour, zoom, CRS assignment, removal.
Item {
    id: panel
    property var controller
    readonly property var d: controller ? controller.details : ({})

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.panelHeader
            color: Theme.toolbar
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }
            SectionTitle { anchors.verticalCenter: parent.verticalCenter; x: 10; text: "СВОЙСТВА СЛОЯ" }
        }
        Label {
            Layout.fillWidth: true
            Layout.margins: 10
            visible: panel.d.id === undefined
            text: "Выберите слой в списке, чтобы увидеть и изменить его свойства."
            wrapMode: Text.Wrap
            color: Theme.faint; font.pixelSize: 11
        }
        ScrollView {
            id: propertiesScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: panel.d.id !== undefined
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                width: propertiesScroll.availableWidth - 20
                x: 10
                spacing: 8
            Item { implicitHeight: 2 }
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: panel.d.name || ""; color: Theme.text; font.bold: true; font.pixelSize: 13 }
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: panel.d.summary || ""; color: "#b7c6d8"; font.pixelSize: 11 }
            Label { text: "Исходная CRS"; color: Theme.muted; font.pixelSize: 10 }
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: panel.d.sourceCrs || "—"; color: Theme.text; font.pixelSize: 11 }
            Label { text: "CRS карты"; color: Theme.muted; font.pixelSize: 10 }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: (panel.d.mapCrs || "—") + (panel.d.reprojected ? "\n(растр пересчитывается в CRS карты на лету)" : "")
                color: Theme.text; font.pixelSize: 11
            }
            Label { text: "Файл"; color: Theme.muted; font.pixelSize: 10; visible: (panel.d.source || "").length > 0 }
            Label { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; visible: (panel.d.source || "").length > 0
                    text: panel.d.source || ""; color: "#b7c6d8"; font.pixelSize: 10 }
            Label { text: "Прозрачность"; color: Theme.muted; font.pixelSize: 10 }
            Slider {
                Layout.fillWidth: true
                from: 0; to: 1
                value: panel.d.opacity !== undefined ? panel.d.opacity : 1
                onMoved: gis.setLayerOpacity(panel.d.id, value)
            }

            // Raster style
            ColumnLayout {
                Layout.fillWidth: true
                visible: panel.d.type === "raster" && panel.d.style !== undefined
                spacing: 6
                SectionTitle { text: "ОТОБРАЖЕНИЕ"; Layout.topMargin: 6 }
                ComboBox {
                    id: modeBox
                    Layout.fillWidth: true
                    model: ["Цвет (3 канала)", "Один канал (шкала)"]
                    enabled: (panel.d.bands || 0) >= 1
                    currentIndex: panel.d.style && panel.d.style.mode === "single" ? 1 : 0
                    onActivated: gis.setLayerStyle(panel.d.id, { mode: currentIndex === 1 ? "single" : "rgb" })
                }
                GridLayout {
                    columns: 2
                    visible: modeBox.currentIndex === 0
                    Label { text: "Красный канал"; color: Theme.muted; font.pixelSize: 11 }
                    SpinBox { from: 1; to: panel.d.bands || 1; value: panel.d.style ? panel.d.style.red : 1
                              onValueModified: gis.setLayerStyle(panel.d.id, { red: value }) }
                    Label { text: "Зелёный канал"; color: Theme.muted; font.pixelSize: 11 }
                    SpinBox { from: 1; to: panel.d.bands || 1; value: panel.d.style ? panel.d.style.green : 1
                              onValueModified: gis.setLayerStyle(panel.d.id, { green: value }) }
                    Label { text: "Синий канал"; color: Theme.muted; font.pixelSize: 11 }
                    SpinBox { from: 1; to: panel.d.bands || 1; value: panel.d.style ? panel.d.style.blue : 1
                              onValueModified: gis.setLayerStyle(panel.d.id, { blue: value }) }
                }
                GridLayout {
                    columns: 2
                    visible: modeBox.currentIndex === 1
                    Label { text: "Канал"; color: Theme.muted; font.pixelSize: 11 }
                    SpinBox { from: 1; to: panel.d.bands || 1; value: panel.d.style ? panel.d.style.band : 1
                              onValueModified: gis.setLayerStyle(panel.d.id, { band: value }) }
                    Label { text: "Шкала"; color: Theme.muted; font.pixelSize: 11 }
                    ComboBox {
                        Layout.fillWidth: true
                        model: ["Серая", "Рельеф", "Спектр", "Уклоны", "Экспозиция"]
                        currentIndex: !panel.d.style ? 1 : Math.max(0, ["gray", "terrain", "spectral", "slope", "aspect"].indexOf(panel.d.style.ramp))
                        onActivated: gis.setLayerStyle(panel.d.id, { ramp: ["gray", "terrain", "spectral", "slope", "aspect"][currentIndex] })
                    }
                    Label { text: "Минимум"; color: Theme.muted; font.pixelSize: 11 }
                    TextField {
                        Layout.fillWidth: true
                        text: panel.d.style ? Number(panel.d.style.min).toFixed(2) : ""
                        color: Theme.text
                        background: Rectangle { color: Theme.field; radius: 6; border.color: Theme.line }
                        onEditingFinished: gis.setLayerStyle(panel.d.id, { min: Number(text.replace(",", ".")) })
                    }
                    Label { text: "Максимум"; color: Theme.muted; font.pixelSize: 11 }
                    TextField {
                        Layout.fillWidth: true
                        text: panel.d.style ? Number(panel.d.style.max).toFixed(2) : ""
                        color: Theme.text
                        background: Rectangle { color: Theme.field; radius: 6; border.color: Theme.line }
                        onEditingFinished: gis.setLayerStyle(panel.d.id, { max: Number(text.replace(",", ".")) })
                    }
                }
                CheckBox {
                    visible: modeBox.currentIndex === 1
                    text: "Отмывка рельефа"
                    checked: panel.d.style ? panel.d.style.hillshade : false
                    onToggled: gis.setLayerStyle(panel.d.id, { hillshade: checked })
                }
            }

            // Vector colour
            ColumnLayout {
                Layout.fillWidth: true
                visible: panel.d.type !== "raster" && panel.d.type !== undefined
                SectionTitle { text: "ЦВЕТ"; Layout.topMargin: 6 }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: ["#ffc43d", "#7ed321", "#f472b6", "#a78bfa", "#54d6ff", "#ff9f43", "#ffffff", "#ff5a5a"]
                        delegate: Rectangle {
                            required property string modelData
                            width: 24; height: 24; radius: 5; color: modelData
                            border.color: String(panel.d.color) === modelData ? "white" : Theme.line
                            border.width: String(panel.d.color) === modelData ? 2 : 1
                            MouseArea { anchors.fill: parent; onClicked: gis.setLayerColor(panel.d.id, parent.color) }
                        }
                    }
                }
            }

            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; visible: (panel.d.status || "").length > 0
                    text: panel.d.status || ""; color: Theme.warn; font.pixelSize: 11 }
            FlatButton { Layout.fillWidth: true; Layout.topMargin: 8; text: "Показать слой целиком"; compact: true
                         onClicked: gis.zoomToLayer(panel.d.id) }
            FlatButton { Layout.fillWidth: true; text: "Указать CRS…"; compact: true; visible: panel.d.needsCrs === true
                         onClicked: { panel.controller.assignLayerCrs(panel.d.id) } }
            FlatButton { Layout.fillWidth: true; text: "Убрать с карты"; compact: true; caution: true
                         visible: panel.d.removable === true
                         ToolTip.visible: hovered; ToolTip.delay: 600; ToolTip.text: "Файл остаётся на диске"
                         onClicked: gis.removeLayer(panel.d.id) }
            }
        }
    }
}
