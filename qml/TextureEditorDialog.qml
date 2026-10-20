import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Инструменты → Редактор текстуры: the texture of a job's model in a copy — colour of every atlas,
// spots painted over by the inpainting model, one step back, the copy in 3D.
Dialog {
    id: dialog
    objectName: "textureEditorDialog"
    title: "Редактор текстуры"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(1100, Overlay.overlay ? Overlay.overlay.width - 40 : 1100)
    height: Math.min(720, Overlay.overlay ? Overlay.overlay.height - 40 : 720)
    padding: 14
    standardButtons: Dialog.Close
    property var host
    property int atlas: 0
    property var strokes: []          // {x, y} in [0, 1] of the atlas; NaN between strokes
    property bool brush: false
    property real brushSize: 0.01     // radius, in the atlas width

    onOpened: {
        if (textures.models.length > 0 && textures.jobId.length === 0) {
            var e = textures.open(textures.models[0].job)
            if (e.length) host.showError(e)
        }
    }
    function source() { return textures.atlases.length > 0 ? "image://texture/" + atlas + "/" + textures.revision + "/" + [textures.brightness, textures.contrast, textures.saturation, textures.warmth, textures.gamma].join("_") : "" }

    Connections {
        target: textures
        function onFinished(ok, message) { if (ok) dialog.host.flash(message); else dialog.host.showError(message) }
    }

    contentItem: ColumnLayout {
        spacing: 10
        RowLayout {
            spacing: 8
            ComboBox {
                id: modelBox
                objectName: "textureModel"
                Layout.preferredWidth: 320
                implicitHeight: 30
                model: textures.models
                textRole: "title"; valueRole: "job"
                currentIndex: Math.max(0, indexOfValue(textures.jobId))
                onActivated: { var e = textures.open(currentValue); if (e.length) dialog.host.showError(e); dialog.atlas = 0; dialog.strokes = [] }
                font.pixelSize: 12
                enabled: !textures.busy
            }
            Label { visible: textures.models.length === 0; text: "Нет заданий с 3D-моделью"; color: Theme.warn; font.pixelSize: 12 }
            Item { Layout.fillWidth: true }
            FlatButton { compact: true; text: "Отменить шаг"; enabled: textures.canUndo && !textures.busy; onClicked: textures.undo() }
            FlatButton { compact: true; text: "Как после обработки"; caution: true; enabled: textures.jobId.length > 0 && !textures.busy
                         onClicked: { var e = textures.restoreOriginal(); if (e.length) dialog.host.showError(e) } }
            FlatButton { compact: true; text: "Папка"; enabled: textures.jobId.length > 0; onClicked: textures.openFolder()
                         ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Текстуры копии — для правки во внешнем редакторе" }
            FlatButton {
                objectName: "texture3d"
                compact: true; emphasis: true; text: "В 3D"; enabled: textures.jobId.length > 0 && !textures.busy
                onClicked: { var e = textures.showIn3D(); if (e.length && e.indexOf("уже") < 0) dialog.host.showError(e); else { dialog.close(); dialog.host.runCommand("view.3d") } }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10
            // Atlases.
            ListView {
                id: atlasList
                Layout.preferredWidth: 150
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: textures.atlases
                delegate: Rectangle {
                    required property var modelData
                    width: atlasList.width
                    height: 118
                    radius: 5
                    color: modelData.index === dialog.atlas ? Theme.accentSoft : Theme.field
                    border.color: modelData.index === dialog.atlas ? Theme.accent : Theme.line
                    Image {
                        anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; anchors.topMargin: 6
                        width: parent.width - 12; height: 84
                        fillMode: Image.PreserveAspectFit
                        sourceSize: Qt.size(160, 160)
                        cache: false
                        source: "image://texture/" + modelData.index + "/" + textures.revision
                    }
                    Label {
                        anchors.bottom: parent.bottom; anchors.bottomMargin: 6; anchors.horizontalCenter: parent.horizontalCenter
                        text: modelData.width + " × " + modelData.height
                        color: Theme.muted; font.pixelSize: 10
                    }
                    MouseArea { anchors.fill: parent; onClicked: { dialog.atlas = modelData.index; dialog.strokes = [] } }
                }
            }
            // The atlas, with the brush.
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#15191e"
                radius: 4
                clip: true
                Image {
                    id: atlasImage
                    objectName: "textureAtlas"
                    anchors.fill: parent
                    anchors.margins: 6
                    fillMode: Image.PreserveAspectFit
                    asynchronous: false
                    cache: false
                    source: dialog.source()
                    readonly property real shownW: paintedWidth
                    readonly property real shownH: paintedHeight
                    readonly property real x0: (width - paintedWidth) / 2
                    readonly property real y0: (height - paintedHeight) / 2
                    Canvas {
                        id: strokeCanvas
                        anchors.fill: parent
                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.reset()
                            ctx.strokeStyle = "rgba(255, 80, 80, 0.55)"
                            ctx.lineCap = "round"; ctx.lineJoin = "round"
                            ctx.lineWidth = Math.max(2, dialog.brushSize * 2 * atlasImage.shownW)
                            var started = false
                            ctx.beginPath()
                            for (var i = 0; i < dialog.strokes.length; ++i) {
                                var p = dialog.strokes[i]
                                if (isNaN(p.x)) { started = false; continue }
                                var x = atlasImage.x0 + p.x * atlasImage.shownW, y = atlasImage.y0 + p.y * atlasImage.shownH
                                if (!started) { ctx.moveTo(x, y); ctx.lineTo(x + 0.1, y); started = true } else ctx.lineTo(x, y)
                            }
                            ctx.stroke()
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: dialog.brush && !textures.busy
                        cursorShape: dialog.brush ? Qt.CrossCursor : Qt.ArrowCursor
                        function add(mx, my, gap) {
                            var nx = (mx - atlasImage.x0) / atlasImage.shownW, ny = (my - atlasImage.y0) / atlasImage.shownH
                            if (nx < 0 || ny < 0 || nx > 1 || ny > 1) return
                            var s = dialog.strokes.slice()
                            if (gap && s.length > 0) s.push({ x: NaN, y: NaN })
                            s.push({ x: nx, y: ny })
                            dialog.strokes = s
                            strokeCanvas.requestPaint()
                        }
                        onPressed: function(mouse) { add(mouse.x, mouse.y, true) }
                        onPositionChanged: function(mouse) { if (pressed) add(mouse.x, mouse.y, false) }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: textures.atlases.length === 0
                    text: "Выберите модель"
                    color: Theme.faint
                }
                BusyIndicator { anchors.centerIn: parent; running: textures.busy; visible: textures.busy }
            }
            // Colour and the brush.
            ColumnLayout {
                Layout.fillWidth: false   // a layout fills by default: the atlas gets the room
                Layout.preferredWidth: 230
                Layout.maximumWidth: 230
                Layout.fillHeight: true
                spacing: 6
                SectionTitle { text: "ЦВЕТ" }
                Repeater {
                    model: [{ t: "Яркость", p: "brightness", from: -1, to: 1, neutral: 0 }, { t: "Контраст", p: "contrast", from: -1, to: 1, neutral: 0 },
                            { t: "Насыщенность", p: "saturation", from: -1, to: 1, neutral: 0 }, { t: "Теплота", p: "warmth", from: -1, to: 1, neutral: 0 },
                            { t: "Гамма", p: "gamma", from: 0.5, to: 2, neutral: 1 }]
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 0
                        RowLayout {
                            Label { text: modelData.t; color: Theme.muted; font.pixelSize: 11; Layout.fillWidth: true }
                            Label { text: textures[modelData.p].toLocaleString(Qt.locale("ru_RU"), 'f', 2); color: Theme.faint; font.pixelSize: 10 }
                        }
                        Slider {
                            Layout.fillWidth: true
                            from: modelData.from; to: modelData.to
                            value: textures[modelData.p]
                            enabled: textures.atlases.length > 0 && !textures.busy
                            onMoved: textures[modelData.p] = value
                            onPressedChanged: if (!pressed && Math.abs(value - modelData.neutral) < 0.03) textures[modelData.p] = modelData.neutral
                        }
                    }
                }
                RowLayout {
                    FlatButton {
                        objectName: "textureApply"
                        compact: true; emphasis: true; text: "Применить"; Layout.fillWidth: true
                        enabled: textures.atlases.length > 0 && !textures.busy
                        onClicked: { var e = textures.applyAdjustments(); if (e.length) dialog.host.showNotice(e) }
                        ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Ко всем текстурам модели"
                    }
                    FlatButton { compact: true; text: "Сброс"; onClicked: textures.resetAdjustments() }
                }
                SectionTitle { text: "ПЯТНА"; Layout.topMargin: 8 }
                FlatButton {
                    objectName: "textureBrush"
                    compact: true; checkable: true; checked: dialog.brush; emphasis: checked; Layout.fillWidth: true
                    text: checked ? "Кисть включена" : "Кисть"
                    enabled: textures.atlases.length > 0
                    onToggled: dialog.brush = checked
                    ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Закрасьте пятно, затем «Дорисовать»"
                }
                RowLayout {
                    Label { text: "Размер"; color: Theme.muted; font.pixelSize: 11 }
                    Slider { Layout.fillWidth: true; from: 0.002; to: 0.04; value: dialog.brushSize; onMoved: { dialog.brushSize = value; strokeCanvas.requestPaint() } }
                }
                RowLayout {
                    FlatButton {
                        objectName: "textureRetouch"
                        compact: true; emphasis: true; text: "Дорисовать"; Layout.fillWidth: true
                        enabled: dialog.strokes.length > 0 && !textures.busy
                        onClicked: {
                            var e = textures.retouch(dialog.atlas, dialog.strokes, dialog.brushSize)
                            if (e.length) dialog.host.showError(e)
                            else { dialog.strokes = []; strokeCanvas.requestPaint() }
                        }
                        ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Закрашенное дорисует ИИ (LaMa) по окружению"
                    }
                    FlatButton { compact: true; text: "Очистить"; enabled: dialog.strokes.length > 0; onClicked: { dialog.strokes = []; strokeCanvas.requestPaint() } }
                }
                Item { Layout.fillHeight: true }
                Label {
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: textures.busy ? textures.status : "Правки — в копии модели; исходная не меняется"
                    color: Theme.faint; font.pixelSize: 10
                }
            }
        }
    }
}
