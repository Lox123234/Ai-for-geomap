import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoMapAI.View3D

// 3D view: point clouds, models and terrains of the project drawn by the GPU (View3DItem),
// a context bar (navigation tools, predefined views, projection, colours, vertical
// exaggeration), the picked point, 3D measurements and the axes of the view.
Item {
    id: workspace
    property var host
    readonly property bool software: GraphicsInfo.api === GraphicsInfo.Software

    component BarSeparator: Rectangle { implicitWidth: 1; implicitHeight: 22; color: Theme.line; Layout.leftMargin: 4; Layout.rightMargin: 4 }
    // A text button of the context bar bound to a command.
    component BarCommand: ToolButton {
        id: barButton
        required property string commandId
        property string label: commands.title(commandId)
        readonly property bool active: workspace.host.commandChecked(commandId)
        text: label
        font.pixelSize: 11
        implicitHeight: 28
        leftPadding: 8; rightPadding: 8
        focusPolicy: Qt.NoFocus
        enabled: commands.isReady(commandId) && workspace.host.commandEnabled(commandId)
        palette.buttonText: active ? "#FFFFFF" : enabled ? Theme.text : Theme.faint
        HoverHandler { id: barHover }
        ToolTip.visible: barHover.hovered
        ToolTip.delay: 500
        ToolTip.text: commands.revision >= 0 ? commands.tooltip(commandId) : ""
        onClicked: workspace.host.runCommand(commandId)
        background: Rectangle { radius: 4; color: barButton.active ? Theme.accent : barHover.hovered && barButton.enabled ? Theme.hover : "transparent" }
    }

    Rectangle { anchors.fill: parent; color: Theme.bg }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Context bar of the 3D view
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 38
            color: Theme.toolbar
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
            Flickable {
                anchors.fill: parent
                anchors.leftMargin: 6
                contentWidth: bar.implicitWidth
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                RowLayout {
                    id: bar
                    height: parent.height
                    spacing: 2
                    ToolIconButton { commandId: "tool.rotate"; host: workspace.host; implicitWidth: 30; implicitHeight: 30 }
                    ToolIconButton { commandId: "tool.pan"; host: workspace.host; implicitWidth: 30; implicitHeight: 30 }
                    ToolIconButton { commandId: "tool.scale"; host: workspace.host; implicitWidth: 30; implicitHeight: 30 }
                    ToolIconButton { commandId: "tool.ruler"; host: workspace.host; implicitWidth: 30; implicitHeight: 30 }
                    BarSeparator {}
                    BarCommand { commandId: "view.top"; label: "Сверху" }
                    BarCommand { commandId: "view.front"; label: "С юга" }
                    BarCommand { commandId: "view.side"; label: "С запада" }
                    BarCommand { commandId: "view.iso"; label: "Изометрия" }
                    BarCommand { commandId: "view.reset"; label: "Показать всё" }
                    BarSeparator {}
                    BarCommand { commandId: "view.projection"; label: scene3d.orthographic ? "Ортография" : "Перспектива" }
                    BarSeparator {}
                    Label { text: "Цвет"; color: Theme.muted; font.pixelSize: 11 }
                    ComboBox {
                        id: colorBox
                        implicitWidth: 132; implicitHeight: 26
                        font.pixelSize: 11
                        model: [{ text: "Исходный", value: "rgb" }, { text: "По высоте", value: "height" },
                                { text: "Классы", value: "classes" }, { text: "Интенсивность", value: "intensity" }]
                        textRole: "text"; valueRole: "value"
                        currentIndex: Math.max(0, ["rgb", "height", "classes", "intensity"].indexOf(scene3d.colorMode))
                        onActivated: scene3d.colorMode = currentValue
                        ToolTip.visible: hovered; ToolTip.delay: 500
                        ToolTip.text: "Цвет: исходный (RGB, текстура), шкала высот, классы точек LAS (земля, растительность, здания…) или интенсивность отражения. Облака без классов или интенсивности остаются в исходном цвете."
                    }
                    Label { text: "Высоты ×"; color: Theme.muted; font.pixelSize: 11; Layout.leftMargin: 6 }
                    ComboBox {
                        implicitWidth: 64; implicitHeight: 26
                        font.pixelSize: 11
                        model: [1, 1.5, 2, 3, 5, 10]
                        currentIndex: Math.max(0, model.indexOf(scene3d.verticalScale))
                        onActivated: scene3d.verticalScale = model[currentIndex]
                        ToolTip.visible: hovered; ToolTip.delay: 500
                        ToolTip.text: "Вертикальное преувеличение рельефа (только отображение; координаты и измерения — истинные)"
                    }
                    BarCommand { commandId: "view.axes"; label: "Оси" }
                    Item { implicitWidth: 8 }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            View3DItem {
                id: view
                objectName: "view3d"
                anchors.fill: parent
                scene: scene3d
                visible: !workspace.software
                focus: true
                Keys.onPressed: function(event) {
                    var step = event.modifiers & Qt.ShiftModifier ? 15 : 5
                    if (event.key === Qt.Key_Left) scene3d.orbit(-step / 0.3, 0)
                    else if (event.key === Qt.Key_Right) scene3d.orbit(step / 0.3, 0)
                    else if (event.key === Qt.Key_Up) scene3d.orbit(0, -step / 0.3)
                    else if (event.key === Qt.Key_Down) scene3d.orbit(0, step / 0.3)
                    else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) scene3d.zoom(1)
                    else if (event.key === Qt.Key_Minus) scene3d.zoom(-1)
                    else if (event.key === Qt.Key_Escape) scene3d.clearMeasurement()
                    else return
                    event.accepted = true
                }
            }

            // Without GPU rendering (software Qt Quick) the 3D view cannot draw.
            Label {
                anchors.centerIn: parent
                width: Math.min(parent.width - 40, 560)
                visible: workspace.software
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                text: "3D-просмотр требует аппаратной графики (Direct3D, Vulkan, Metal или OpenGL), а интерфейс сейчас рисуется программно. "
                    + "Слои сцены загружаются и измеряются, но не отображаются. Справка → Графический ускоритель."
                color: Theme.warn; font.pixelSize: 13
            }

            // Empty scene: results of the project to open
            ColumnLayout {
                anchors.centerIn: parent
                spacing: 12
                visible: scene3d.layerCount === 0 && !workspace.software
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "3D-сцена пуста"
                    color: Theme.text; font.pixelSize: 20; font.bold: true
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.maximumWidth: 520
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    text: "Откройте результаты обработки проекта или файлы: облака точек PLY/LAS, модели OBJ/PLY, модели высот GeoTIFF."
                    color: Theme.muted; font.pixelSize: 12
                }
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: 6
                    spacing: 8
                    Repeater {
                        model: [{ kind: "pointcloud", title: "Облако точек" }, { kind: "mesh", title: "3D-модель" },
                                { kind: "dsm", title: "Рельеф DSM" }, { kind: "dtm", title: "Рельеф DTM" }]
                        delegate: FlatButton {
                            required property var modelData
                            readonly property var result: projectTree.revision >= 0 ? projectTree.result(modelData.kind) : ({})
                            text: modelData.title
                            emphasis: modelData.kind === "pointcloud"
                            enabled: result.path !== undefined
                            onClicked: workspace.host.open3D(modelData.kind, result.path)
                            ToolTip.visible: hovered; ToolTip.delay: 500
                            ToolTip.text: result.path !== undefined ? result.path : "Нет в результатах обработки проекта"
                        }
                    }
                }
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8
                    FlatButton { text: "Импорт облака точек…"; compact: true; onClicked: workspace.host.runCommand("import.pointCloud") }
                    FlatButton { text: "Импорт 3D-модели…"; compact: true; onClicked: workspace.host.runCommand("import.model") }
                }
            }

            // Legend of the point classes (colour mode "Классы").
            Rectangle {
                visible: scene3d.colorMode === "classes" && scene3d.layerCount > 0
                anchors.top: parent.top; anchors.left: parent.left; anchors.margins: 10
                width: legendColumn.implicitWidth + 16; height: legendColumn.implicitHeight + 12
                radius: 6
                color: "#CC171B22"; border.color: Theme.line
                ColumnLayout {
                    id: legendColumn
                    anchors.centerIn: parent
                    spacing: 2
                    Repeater {
                        model: [{ c: "#a6763c", t: "земля" }, { c: "#a6d66e", t: "низкая растительность" }, { c: "#50af46", t: "средняя растительность" },
                                { c: "#227832", t: "высокая растительность" }, { c: "#e15a46", t: "здания" }, { c: "#3c82e6", t: "вода" },
                                { c: "#ff00ff", t: "шум" }, { c: "#bebebe", t: "без класса" }]
                        delegate: RowLayout {
                            required property var modelData
                            spacing: 6
                            Rectangle { implicitWidth: 10; implicitHeight: 10; radius: 2; color: modelData.c }
                            Label { text: modelData.t; color: Theme.text; font.pixelSize: 10 }
                        }
                    }
                }
            }
            // Loading
            Rectangle {
                anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 10
                visible: scene3d.loading
                width: loadingRow.implicitWidth + 20; height: 30; radius: 6
                color: "#CC20252E"; border.color: Theme.line
                RowLayout {
                    id: loadingRow
                    anchors.centerIn: parent
                    spacing: 8
                    BusyIndicator { implicitWidth: 18; implicitHeight: 18; running: scene3d.loading }
                    Label { text: "Загрузка слоёв…"; color: Theme.text; font.pixelSize: 11 }
                }
            }

            // Picked point, measurement, navigation hint
            Rectangle {
                anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 10
                visible: scene3d.layerCount > 0
                width: Math.min(parent.width - 140, infoColumn.implicitWidth + 20)
                height: infoColumn.implicitHeight + 14
                radius: 6
                color: "#CC171B22"; border.color: Theme.line
                ColumnLayout {
                    id: infoColumn
                    anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 10; anchors.rightMargin: 10
                    spacing: 2
                    Label {
                        objectName: "pick3d"
                        Layout.fillWidth: true
                        visible: scene3d.pickText.length > 0
                        text: scene3d.pickText
                        color: Theme.text; font.pixelSize: 12; font.family: "Consolas"
                        elide: Text.ElideRight
                    }
                    Label {
                        objectName: "measure3d"
                        Layout.fillWidth: true
                        visible: scene3d.measureText.length > 0
                        text: scene3d.measureText
                        color: Theme.good; font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Label {
                        text: scene3d.tool === "measure" ? "Измерение: щёлкните две точки · Esc — сбросить"
                            : "ЛКМ — " + (scene3d.tool === "pan" ? "сдвиг" : scene3d.tool === "zoom" ? "масштаб" : "вращение")
                              + " · ПКМ/СКМ — сдвиг · колесо — масштаб · щелчок — координаты точки"
                        color: Theme.faint; font.pixelSize: 10
                    }
                }
            }

            // Axes of the view: X east (red), Y north (green), Z up (blue)
            Canvas {
                id: axes
                anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 10
                width: 92; height: 92
                visible: scene3d.showAxes && scene3d.layerCount > 0
                property real yaw: scene3d.yaw
                property real pitch: scene3d.pitch
                onYawChanged: requestPaint()
                onPitchChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    ctx.fillStyle = "rgba(23,27,34,0.75)"
                    ctx.beginPath(); ctx.arc(46, 46, 45, 0, 2 * Math.PI); ctx.fill()
                    var y = yaw * Math.PI / 180, p = pitch * Math.PI / 180
                    // Screen right = (cos y, -sin y, 0); screen up = (sin y sin p, cos y sin p, cos p).
                    var list = [{ v: [1, 0, 0], c: "#E4605E", t: "X" }, { v: [0, 1, 0], c: "#7FD17F", t: "Y" }, { v: [0, 0, 1], c: "#6FA8F0", t: "Z" }]
                    list.forEach(function(a) {
                        a.sx = a.v[0] * Math.cos(y) - a.v[1] * Math.sin(y)
                        a.sy = a.v[0] * Math.sin(y) * Math.sin(p) + a.v[1] * Math.cos(y) * Math.sin(p) + a.v[2] * Math.cos(p)
                        // > 0 towards the viewer: drawn last, over the others
                        a.d = -(a.v[0] * Math.sin(y) * Math.cos(p) + a.v[1] * Math.cos(y) * Math.cos(p) - a.v[2] * Math.sin(p))
                    })
                    list.sort(function(a, b) { return a.d - b.d })
                    ctx.lineWidth = 2.5
                    ctx.font = "bold 11px sans-serif"
                    list.forEach(function(a) {
                        var ex = 46 + a.sx * 32, ey = 46 - a.sy * 32
                        ctx.strokeStyle = a.c; ctx.fillStyle = a.c
                        ctx.beginPath(); ctx.moveTo(46, 46); ctx.lineTo(ex, ey); ctx.stroke()
                        ctx.fillText(a.t, 46 + a.sx * 40 - 4, 46 - a.sy * 40 + 4)
                    })
                }
            }
        }
    }
}
