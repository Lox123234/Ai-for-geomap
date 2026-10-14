import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import GeoMapAI.Map

// The GIS map of the project: layers, tools, measurements and layer properties.
// Everything comes from the `gis` service (MapService); coordinates are in the map CRS.
Item {
    id: root
    signal errorMessage(string text)
    signal noticeMessage(string text)

    property string crsTarget: ""        // "" = CRS of the map, else id of the layer to assign a CRS to
    property var details: ({})

    readonly property color white: "#ecf2fa"
    readonly property color muted: "#8d9aaa"
    readonly property color line: "#303b4b"

    function refreshDetails() {
        root.details = gis.selectedLayer.length > 0 ? gis.layerDetails(gis.selectedLayer) : ({})
    }
    function formatLength(m) {
        return m >= 1000 ? (m / 1000).toLocaleString(Qt.locale("ru_RU"), 'f', m >= 10000 ? 0 : 1) + " км"
                         : m.toLocaleString(Qt.locale("ru_RU"), 'f', m < 1 ? 2 : 0) + " м"
    }

    Connections {
        target: gis
        function onSelectionChanged() { root.refreshDetails() }
        function onLayersChanged() { root.refreshDetails() }
        function onErrorOccurred(message) { root.errorMessage(message) }
        function onExportFinished(ok, message) { if (ok) root.noticeMessage(message); else root.errorMessage(message) }
    }

    FileDialog {
        id: addLayerDialog
        title: "Добавить слой на карту"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Геоданные (*.tif *.tiff *.vrt *.img *.asc *.geojson *.json *.kml *.gpkg)",
                      "Растры (*.tif *.tiff *.vrt *.img *.asc)", "Векторы (*.geojson *.json *.kml *.gpkg)", "Все файлы (*)"]
        onAccepted: {
            var error = gis.addLayerFromUrl(selectedFile)
            if (error.length > 0) root.errorMessage(error)
        }
    }
    FileDialog {
        id: exportMapDialog
        title: "Экспорт карты"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: ["Изображение PNG (*.png)", "Документ PDF (*.pdf)"]
        onAccepted: gis.exportMap(selectedFile)
    }
    FileDialog {
        id: exportDrawingsDialog
        title: "Экспорт нарисованных объектов"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "geojson"
        nameFilters: ["GeoJSON (*.geojson)", "GeoPackage (*.gpkg)", "KML (*.kml)"]
        onAccepted: {
            var error = gis.exportDrawings(selectedFile)
            if (error.length > 0) root.errorMessage(error)
            else root.noticeMessage("Объекты сохранены: " + gis.displayPath(selectedFile))
        }
    }

    // Choosing a CRS: never guessed for data that has none; the user enters an EPSG code and
    // sees what it means before applying it.
    Dialog {
        id: crsDialog
        title: root.crsTarget.length === 0 ? "Система координат карты" : "Система координат слоя"
        anchors.centerIn: Overlay.overlay
        modal: true
        width: 540
        background: Rectangle { color: "#202936"; border.color: "#566176"; radius: 12 }
        onOpened: { crsField.text = ""; crsField.forceActiveFocus() }
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: root.crsTarget.length === 0
                    ? "Все слои пересчитываются в эту систему координат. Измерения выполняются геодезически независимо от неё."
                    : "У файла нет описания системы координат. Укажите её, только если знаете точно (например, из паспорта данных): "
                      + "неверная CRS сместит слой, а измерения по нему будут ошибочными."
                color: root.crsTarget.length === 0 ? root.muted : "#f5cf7c"; font.pixelSize: 12
            }
            TextField {
                id: crsField
                Layout.fillWidth: true
                placeholderText: "EPSG-код, например 32615, 3857, 4326, или EPSG:…"
                color: root.white
                placeholderTextColor: "#66768a"
                selectByMouse: true
                background: Rectangle { color: "#121b26"; radius: 7; border.color: "#405066" }
            }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: crsField.text.trim().length > 0 ? gis.describeCrs(crsField.text) : ""
                color: text.startsWith("✕") ? "#f4a6a6" : "#82d7b7"; font.pixelSize: 12
            }
        }
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: {
            var error = root.crsTarget.length === 0 ? gis.setMapCrs(crsField.text) : gis.assignLayerCrs(root.crsTarget, crsField.text)
            if (error.length > 0) root.errorMessage(error)
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Layers
        Rectangle {
            Layout.preferredWidth: 300
            Layout.fillHeight: true
            color: "#151b24"
            border.color: root.line
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    SectionTitle { text: "СЛОИ КАРТЫ" }
                    Item { Layout.fillWidth: true }
                    Label { text: gis.layerCount.toString(); color: root.muted; font.pixelSize: 11 }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    FlatButton { text: "+ Слой…"; compact: true; Layout.fillWidth: true; onClicked: addLayerDialog.open() }
                    FlatButton { text: "CRS карты"; compact: true; onClicked: { root.crsTarget = ""; crsDialog.open() } }
                }
                Label {
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: "CRS карты: " + gis.crsLabel
                    color: gis.crsDefined ? "#b7c6d8" : "#f5cf7c"; font.pixelSize: 11
                }
                Label {
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    visible: gis.crsNote.length > 0
                    text: "Выбрана " + gis.crsNote
                    color: "#6f7f92"; font.pixelSize: 10
                }
                Label {
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    visible: gis.problems.length > 0
                    text: "⚠ " + gis.problems.join("\n⚠ ")
                    color: "#f4a6a6"; font.pixelSize: 11
                }
                ListView {
                    id: layerList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: gis.layerModel
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
                        width: layerList.width - 8
                        height: layerColumn.implicitHeight + 14
                        radius: 8
                        color: selected ? "#25465f" : rowMouse.containsMouse ? "#263341" : "#1a2430"
                        border.color: selected ? "#4e95c2" : "transparent"
                        Accessible.role: Accessible.ListItem
                        Accessible.name: "Слой " + name
                        Accessible.onPressAction: gis.selectedLayer = layerId
                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: gis.selectedLayer = layerRow.layerId
                        }
                        ColumnLayout {
                            id: layerColumn
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            anchors.margins: 7
                            spacing: 3
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                CheckBox {
                                    checked: layerRow.layerVisible
                                    padding: 0
                                    onToggled: gis.setLayerVisible(layerRow.layerId, checked)
                                    Accessible.name: "Показать слой " + layerRow.name
                                }
                                Rectangle {
                                    implicitWidth: 16; implicitHeight: 12; radius: 2
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
                                    color: root.white; font.pixelSize: 12; font.bold: true
                                }
                                ToolButton {
                                    text: "▲"; implicitWidth: 22; implicitHeight: 22; font.pixelSize: 9
                                    onClicked: gis.moveLayer(layerRow.layerId, 1)
                                    ToolTip.visible: hovered; ToolTip.text: "Выше (рисуется поверх)"
                                }
                                ToolButton {
                                    text: "▼"; implicitWidth: 22; implicitHeight: 22; font.pixelSize: 9
                                    onClicked: gis.moveLayer(layerRow.layerId, -1)
                                    ToolTip.visible: hovered; ToolTip.text: "Ниже"
                                }
                            }
                            Label {
                                Layout.fillWidth: true; wrapMode: Text.Wrap
                                visible: layerRow.summary.length > 0
                                text: layerRow.summary
                                color: root.muted; font.pixelSize: 10
                            }
                            Label {
                                Layout.fillWidth: true; wrapMode: Text.Wrap
                                visible: layerRow.status.length > 0
                                text: (layerRow.loading ? "⏳ " : "⚠ ") + layerRow.status
                                color: layerRow.loading ? "#9fd3f5" : "#f5cf7c"; font.pixelSize: 10
                            }
                            FlatButton {
                                visible: layerRow.needsCrs
                                text: "Указать CRS…"; compact: true
                                onClicked: { root.crsTarget = layerRow.layerId; crsDialog.open() }
                            }
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: "Колесо — масштаб, перетаскивание — сдвиг. Результаты обработки добавляются кнопкой «На карту» во вкладке «Обработка»."
                    color: "#6f7f92"; font.pixelSize: 10
                }
            }
        }

        // Map
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            // Tools wrap onto a second row when the window is narrow or the properties panel is open.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: toolFlow.implicitHeight + 16
                color: "#151b24"
                border.color: root.line
                Flow {
                    id: toolFlow
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.margins: 8; anchors.leftMargin: 10; anchors.rightMargin: 10
                    spacing: 5
                    Repeater {
                        model: [{ id: "pan", title: "Обзор" }, { id: "length", title: "Длина" }, { id: "area", title: "Площадь" },
                                { id: "point", title: "Точка" }, { id: "line", title: "Линия" }, { id: "polygon", title: "Полигон" },
                                { id: "edit", title: "Правка" }]
                        delegate: FlatButton {
                            required property var modelData
                            text: modelData.title; compact: true
                            emphasis: gis.tool === modelData.id
                            enabled: gis.crsDefined
                            onClicked: gis.tool = modelData.id
                        }
                    }
                    Rectangle { width: 1; height: 30; color: root.line }

                    FlatButton { text: "↶"; compact: true; enabled: gis.canUndo; onClicked: gis.undo()
                                 ToolTip.visible: hovered; ToolTip.text: "Отменить: " + gis.undoText }
                    FlatButton { text: "↷"; compact: true; enabled: gis.canRedo; onClicked: gis.redo()
                                 ToolTip.visible: hovered; ToolTip.text: "Повторить: " + gis.redoText }
                    FlatButton { text: "Удалить"; compact: true; caution: true; enabled: gis.hasSelection; onClicked: gis.deleteSelected() }
                    Rectangle { width: 1; height: 30; color: root.line }
                    FlatButton { text: "Всё"; compact: true; enabled: gis.crsDefined; onClicked: gis.zoomToAll()

                                 ToolTip.visible: hovered; ToolTip.text: "Показать все слои" }
                    FlatButton { text: "Экспорт карты…"; compact: true; enabled: gis.crsDefined && !gis.exporting; onClicked: exportMapDialog.open() }
                    FlatButton { text: "Экспорт объектов…"; compact: true; enabled: gis.drawingCount > 0; onClicked: exportDrawingsDialog.open() }
                }
            }
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                MapView {
                    id: mapView
                    anchors.fill: parent
                    service: gis
                }
                Rectangle {
                    visible: gis.measurementText.length > 0
                    anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 12
                    width: Math.min(parent.width - 24, measureColumn.implicitWidth + 24)
                    height: measureColumn.implicitHeight + 16
                    radius: 8
                    color: "#e617212d"
                    border.color: "#3b4a5d"
                    ColumnLayout {
                        id: measureColumn
                        anchors.fill: parent; anchors.margins: 8
                        spacing: 3
                        Label { text: gis.measurementText; color: "#ffd666"; font.pixelSize: 14; font.bold: true }
                        Label {
                            Layout.maximumWidth: 560; wrapMode: Text.Wrap
                            visible: gis.measurementDetails.length > 0
                            text: gis.measurementDetails; color: root.muted; font.pixelSize: 10
                        }
                    }
                }
                Label {
                    visible: ["length", "area", "line", "polygon"].indexOf(gis.tool) >= 0
                    anchors.horizontalCenter: parent.horizontalCenter; anchors.top: parent.top; anchors.topMargin: 12
                    text: "Щелчок — вершина · двойной щелчок, Enter или правая кнопка — завершить · Backspace — убрать вершину · Esc — отмена"
                    color: "#dce8f5"; font.pixelSize: 11
                    padding: 6
                    background: Rectangle { color: "#cc17212d"; radius: 6 }
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
                        text: root.formatLength(gis.scaleBarMeters) + "   " + gis.scaleText
                        color: "white"; font.pixelSize: 11; font.bold: true
                        style: Text.Outline; styleColor: "#10151c"
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 28
                color: "#121922"
                border.color: root.line
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12; anchors.rightMargin: 12
                    spacing: 18
                    Label { text: gis.cursorText; color: "#dce8f5"; font.pixelSize: 11; font.family: "Consolas" }
                    Label { text: gis.cursorWgs84; color: root.muted; font.pixelSize: 11 }
                    Label { text: gis.cursorValue; color: "#a8dbd0"; font.pixelSize: 11 }
                    Item { Layout.fillWidth: true }
                    Label { text: gis.gisVersion; color: "#5e6d80"; font.pixelSize: 10 }
                }
            }
        }

        // Layer properties
        Rectangle {
            Layout.preferredWidth: 270
            Layout.fillHeight: true
            visible: root.details.id !== undefined
            color: "#191f28"
            border.color: root.line
            ScrollView {
                id: propertiesScroll
                anchors.fill: parent
                anchors.margins: 12
                contentWidth: availableWidth
                clip: true
                ColumnLayout {
                    width: propertiesScroll.availableWidth
                    spacing: 8
                    SectionTitle { text: "СВОЙСТВА СЛОЯ" }
                    Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.details.name || ""; color: root.white; font.bold: true; font.pixelSize: 13 }
                    Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.details.summary || ""; color: "#b7c6d8"; font.pixelSize: 11 }
                    Label { text: "Исходная CRS"; color: root.muted; font.pixelSize: 10 }
                    Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.details.sourceCrs || "—"; color: root.white; font.pixelSize: 11 }
                    Label { text: "CRS карты"; color: root.muted; font.pixelSize: 10 }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: (root.details.mapCrs || "—") + (root.details.reprojected ? "\n(растр пересчитывается в CRS карты на лету)" : "")
                        color: root.white; font.pixelSize: 11
                    }
                    Label { text: "Файл"; color: root.muted; font.pixelSize: 10; visible: (root.details.source || "").length > 0 }
                    Label { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; visible: (root.details.source || "").length > 0
                            text: root.details.source || ""; color: "#b7c6d8"; font.pixelSize: 10 }
                    Label { text: "Прозрачность"; color: root.muted; font.pixelSize: 10 }
                    Slider {
                        Layout.fillWidth: true
                        from: 0; to: 1
                        value: root.details.opacity !== undefined ? root.details.opacity : 1
                        onMoved: gis.setLayerOpacity(root.details.id, value)
                    }

                    // Raster style
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.details.type === "raster" && root.details.style !== undefined
                        spacing: 6
                        SectionTitle { text: "ОТОБРАЖЕНИЕ"; Layout.topMargin: 6 }
                        ComboBox {
                            id: modeBox
                            Layout.fillWidth: true
                            model: ["Цвет (3 канала)", "Один канал (шкала)"]
                            enabled: (root.details.bands || 0) >= 1
                            currentIndex: root.details.style && root.details.style.mode === "single" ? 1 : 0
                            onActivated: gis.setLayerStyle(root.details.id, { mode: currentIndex === 1 ? "single" : "rgb" })
                        }
                        GridLayout {
                            columns: 2
                            visible: modeBox.currentIndex === 0
                            Label { text: "Красный канал"; color: root.muted; font.pixelSize: 11 }
                            SpinBox { from: 1; to: root.details.bands || 1; value: root.details.style ? root.details.style.red : 1
                                      onValueModified: gis.setLayerStyle(root.details.id, { red: value }) }
                            Label { text: "Зелёный канал"; color: root.muted; font.pixelSize: 11 }
                            SpinBox { from: 1; to: root.details.bands || 1; value: root.details.style ? root.details.style.green : 1
                                      onValueModified: gis.setLayerStyle(root.details.id, { green: value }) }
                            Label { text: "Синий канал"; color: root.muted; font.pixelSize: 11 }
                            SpinBox { from: 1; to: root.details.bands || 1; value: root.details.style ? root.details.style.blue : 1
                                      onValueModified: gis.setLayerStyle(root.details.id, { blue: value }) }
                        }
                        GridLayout {
                            columns: 2
                            visible: modeBox.currentIndex === 1
                            Label { text: "Канал"; color: root.muted; font.pixelSize: 11 }
                            SpinBox { from: 1; to: root.details.bands || 1; value: root.details.style ? root.details.style.band : 1
                                      onValueModified: gis.setLayerStyle(root.details.id, { band: value }) }
                            Label { text: "Шкала"; color: root.muted; font.pixelSize: 11 }
                            ComboBox {
                                Layout.fillWidth: true
                                model: ["Серая", "Рельеф", "Спектр"]
                                currentIndex: !root.details.style ? 1 : root.details.style.ramp === "gray" ? 0 : root.details.style.ramp === "spectral" ? 2 : 1
                                onActivated: gis.setLayerStyle(root.details.id, { ramp: ["gray", "terrain", "spectral"][currentIndex] })
                            }
                            Label { text: "Минимум"; color: root.muted; font.pixelSize: 11 }
                            TextField {
                                Layout.fillWidth: true
                                text: root.details.style ? Number(root.details.style.min).toFixed(2) : ""
                                color: root.white
                                background: Rectangle { color: "#121b26"; radius: 6; border.color: "#405066" }
                                onEditingFinished: gis.setLayerStyle(root.details.id, { min: Number(text.replace(",", ".")) })
                            }
                            Label { text: "Максимум"; color: root.muted; font.pixelSize: 11 }
                            TextField {
                                Layout.fillWidth: true
                                text: root.details.style ? Number(root.details.style.max).toFixed(2) : ""
                                color: root.white
                                background: Rectangle { color: "#121b26"; radius: 6; border.color: "#405066" }
                                onEditingFinished: gis.setLayerStyle(root.details.id, { max: Number(text.replace(",", ".")) })
                            }
                        }
                        CheckBox {
                            visible: modeBox.currentIndex === 1
                            text: "Отмывка рельефа"
                            checked: root.details.style ? root.details.style.hillshade : false
                            onToggled: gis.setLayerStyle(root.details.id, { hillshade: checked })
                        }
                    }

                    // Vector colour
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.details.type !== "raster" && root.details.type !== undefined
                        SectionTitle { text: "ЦВЕТ"; Layout.topMargin: 6 }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 6
                            Repeater {
                                model: ["#ffc43d", "#7ed321", "#f472b6", "#a78bfa", "#54d6ff", "#ff9f43", "#ffffff", "#ff5a5a"]
                                delegate: Rectangle {
                                    required property string modelData
                                    width: 24; height: 24; radius: 5; color: modelData
                                    border.color: String(root.details.color) === modelData ? "white" : "#3b4a5d"
                                    border.width: String(root.details.color) === modelData ? 2 : 1
                                    MouseArea { anchors.fill: parent; onClicked: gis.setLayerColor(root.details.id, parent.color) }
                                }
                            }
                        }
                    }

                    Label { Layout.fillWidth: true; wrapMode: Text.Wrap; visible: (root.details.status || "").length > 0
                            text: root.details.status || ""; color: "#f5cf7c"; font.pixelSize: 11 }
                    FlatButton { Layout.fillWidth: true; Layout.topMargin: 8; text: "Показать слой целиком"; compact: true
                                 onClicked: gis.zoomToLayer(root.details.id) }
                    FlatButton { Layout.fillWidth: true; text: "Указать CRS…"; compact: true; visible: root.details.needsCrs === true
                                 onClicked: { root.crsTarget = root.details.id; crsDialog.open() } }
                    FlatButton { Layout.fillWidth: true; text: "Убрать с карты"; compact: true; caution: true
                                 visible: root.details.removable === true
                                 onClicked: gis.removeLayer(root.details.id) }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        visible: root.details.removable === true
                        text: "Файл слоя не удаляется: убирается только ссылка на него из карты проекта."
                        color: "#6f7f92"; font.pixelSize: 10
                    }
                }
            }
        }
    }
}
