import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import GeoMapAI.Map

// Centre of the map view: context bar, the map (MapView painted by MapService), measurement
// box, tool hint and scale bar. Tools are chosen in the main toolbar or the Tools menu.
Item {
    id: canvas
    property var host
    property var controller

    readonly property var toolTitles: ({ "pan": "Перемещение", "length": "Линейка", "area": "Площадь",
                                         "point": "Точка", "line": "Линия", "polygon": "Полигон", "edit": "Правка / выбор",
                                         "profile": "Профиль высот", "volume": "Объём" })
    readonly property bool profileShown: gis.profile.valid === true && !profileClosed
    property bool profileClosed: false
    Connections {
        target: gis
        function onProfileChanged() { canvas.profileClosed = false; profileChart.requestPaint() }
    }

    function terrainName() {
        var id = gis.terrainLayerId()
        if (id.length === 0) return "нет на карте"
        var d = gis.layerDetails(id)
        return d.name || id
    }
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
                id: measureBox
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
                    RowLayout {
                        spacing: 10
                        Label { text: gis.measurementText; color: "#FFD666"; font.pixelSize: 14; font.bold: true }
                        FlatButton {
                            objectName: "saveMeasurement"
                            visible: gis.canSaveMeasurement
                            text: "Сохранить"; compact: true; implicitHeight: 24
                            onClicked: { if (gis.saveMeasurement("").length > 0) canvas.host.flash("Измерение сохранено в панели «Измерения»") }
                            ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Сохранить результат в проекте (панель «Измерения» справа)"
                        }
                    }
                    Label {
                        Layout.maximumWidth: 560; wrapMode: Text.Wrap
                        visible: gis.measurementDetails.length > 0
                        text: gis.measurementDetails; color: Theme.muted; font.pixelSize: 10
                    }
                }
            }
            Label {
                id: toolHint
                visible: ["length", "area", "line", "polygon", "profile", "volume"].indexOf(gis.tool) >= 0
                // Centred, below the measurement box when both would overlap.
                anchors.horizontalCenter: parent.horizontalCenter; anchors.top: parent.top
                anchors.topMargin: measureBox.visible && measureBox.width + 24 > (parent.width - width) / 2 ? measureBox.height + 20 : 12
                text: "Щелчок — вершина · двойной щелчок, Enter или правая кнопка — завершить · Backspace — убрать вершину · Esc — отмена"
                color: Theme.text; font.pixelSize: 11
                padding: 6
                background: Rectangle { color: "#CC1A1F27"; radius: 5 }
            }
            // Volume tool: what the volumes are measured from.
            Rectangle {
                visible: gis.tool === "volume"
                anchors.right: parent.right; anchors.margins: 12
                anchors.top: toolHint.visible ? toolHint.bottom : parent.top
                anchors.topMargin: toolHint.visible ? 8 : 12
                width: volumeColumn.implicitWidth + 20
                height: volumeColumn.implicitHeight + 16
                radius: 6
                color: "#E61A1F27"; border.color: Theme.line
                ColumnLayout {
                    id: volumeColumn
                    anchors.fill: parent; anchors.margins: 8
                    spacing: 4
                    Label { text: "База объёма"; color: Theme.muted; font.pixelSize: 10; font.bold: true }
                    ComboBox {
                        id: baseBox
                        objectName: "volumeBase"
                        implicitWidth: 250; implicitHeight: 28
                        font.pixelSize: 11
                        model: [{ t: "Средняя высота по контуру", v: "boundaryMean" }, { t: "Наименьшая высота по контуру", v: "boundaryMin" },
                                { t: "Плоскость по контуру (на склоне)", v: "bestFit" }, { t: "Заданная высота", v: "level" },
                                { t: "Другая поверхность (DTM, съёмка)", v: "surface" }]
                        textRole: "t"; valueRole: "v"
                        currentIndex: Math.max(0, indexOfValue(gis.volumeBase))
                        onActivated: gis.volumeBase = currentValue
                    }
                    RowLayout {
                        visible: gis.volumeBase === "level"
                        Label { text: "Высота, м"; color: Theme.muted; font.pixelSize: 11 }
                        TextField {
                            implicitWidth: 110; implicitHeight: 28; font.pixelSize: 11; selectByMouse: true
                            text: gis.volumeLevel.toLocaleString(Qt.locale("ru_RU"), 'f', 2)
                            onEditingFinished: { var v = Number(text.replace(",", ".").replace(/\s/g, "")); if (!isNaN(v)) gis.volumeLevel = v }
                        }
                    }
                    ComboBox {
                        visible: gis.volumeBase === "surface"
                        implicitWidth: 250; implicitHeight: 28; font.pixelSize: 11
                        model: gis.layerCount >= 0 ? gis.elevationLayers() : []
                        textRole: "name"; valueRole: "id"
                        displayText: currentIndex >= 0 ? currentText : "Выберите поверхность"
                        currentIndex: indexOfValue(gis.volumeSurface)
                        onActivated: gis.volumeSurface = currentValue
                    }
                    Label {
                        text: "Модель: " + (gis.layerCount >= 0 && gis.selectedLayer !== undefined ? canvas.terrainName() : "")
                        color: Theme.faint; font.pixelSize: 10
                    }
                }
            }
            // Elevation profile of the last line drawn with the profile tool.
            Rectangle {
                id: profilePanel
                objectName: "profilePanel"
                visible: canvas.profileShown
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 12
                height: Math.min(230, parent.height * 0.42)
                radius: 6
                color: "#F21A1F27"; border.color: Theme.line
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 8
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "ПРОФИЛЬ ВЫСОТ"; color: Theme.muted; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2 }
                        Label {
                            Layout.fillWidth: true
                            text: gis.profile.valid ? "«" + gis.profile.layer + "» · " + canvas.formatLength(gis.profile.length)
                                  + " · подъём " + gis.profile.ascent.toLocaleString(Qt.locale("ru_RU"), 'f', 1) + " м · спуск "
                                  + gis.profile.descent.toLocaleString(Qt.locale("ru_RU"), 'f', 1) + " м · наибольший уклон "
                                  + gis.profile.maxSlope.toLocaleString(Qt.locale("ru_RU"), 'f', 1) + "°" : ""
                            color: Theme.text; font.pixelSize: 11; elide: Text.ElideRight
                        }
                        Label { id: hoverLabel; color: "#50E6FF"; font.pixelSize: 11 }
                        FlatButton { text: "CSV…"; compact: true; implicitHeight: 24; onClicked: profileFile.open() }
                        FlatButton { text: "×"; compact: true; implicitHeight: 24; onClicked: { canvas.profileClosed = true; gis.profileMarker = -1 } }
                    }
                    Canvas {
                        id: profileChart
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        property var p: gis.profile
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        readonly property real chartLeft: 52
                        readonly property real chartBottom: 20
                        function niceStep(range, count) {
                            var raw = range / count, mag = Math.pow(10, Math.floor(Math.log(raw) / Math.LN10))
                            var steps = [1, 2, 2.5, 5, 10]
                            for (var i = 0; i < steps.length; ++i) if (steps[i] * mag >= raw) return steps[i] * mag
                            return 10 * mag
                        }
                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.reset()
                            if (!p || !p.valid) return
                            var w = width - chartLeft - 8, h = height - chartBottom - 6
                            var z0 = p.minZ, z1 = p.maxZ
                            if (z1 - z0 < 1) { z0 -= 0.5; z1 += 0.5 }
                            var pad = (z1 - z0) * 0.08; z0 -= pad; z1 += pad
                            var L = Math.max(1e-6, p.length)
                            function X(d) { return chartLeft + d / L * w }
                            function Y(z) { return 6 + (z1 - z) / (z1 - z0) * h }
                            ctx.font = "10px sans-serif"
                            ctx.strokeStyle = "#343C48"; ctx.fillStyle = "#96A2B3"; ctx.lineWidth = 1
                            var zs = niceStep(z1 - z0, 4)
                            for (var z = Math.ceil(z0 / zs) * zs; z <= z1; z += zs) {
                                ctx.beginPath(); ctx.moveTo(chartLeft, Y(z)); ctx.lineTo(chartLeft + w, Y(z)); ctx.stroke()
                                ctx.fillText(z.toLocaleString(Qt.locale("ru_RU"), 'f', zs < 1 ? 1 : 0), 4, Y(z) + 3)
                            }
                            var ds = niceStep(L, 6)
                            for (var d = 0; d <= L + 1e-9; d += ds) {
                                ctx.beginPath(); ctx.moveTo(X(d), 6); ctx.lineTo(X(d), 6 + h); ctx.stroke()
                                ctx.fillText(canvas.formatLength(d), X(d) - 10, height - 4)
                            }
                            // Filled terrain section, broken where there is no data.
                            var dist = p.distances, hts = p.heights
                            var grad = ctx.createLinearGradient(0, 6, 0, 6 + h)
                            grad.addColorStop(0, "rgba(80, 200, 140, 0.55)"); grad.addColorStop(1, "rgba(53, 120, 212, 0.15)")
                            var start = -1
                            for (var i = 0; i <= hts.length; ++i) {
                                var ok = i < hts.length && isFinite(hts[i])
                                if (ok && start < 0) start = i
                                if ((!ok || i === hts.length) && start >= 0) {
                                    ctx.beginPath(); ctx.moveTo(X(dist[start]), 6 + h)
                                    for (var k = start; k < i; ++k) ctx.lineTo(X(dist[k]), Y(hts[k]))
                                    ctx.lineTo(X(dist[i - 1]), 6 + h); ctx.closePath()
                                    ctx.fillStyle = grad; ctx.fill()
                                    ctx.beginPath(); ctx.moveTo(X(dist[start]), Y(hts[start]))
                                    for (k = start + 1; k < i; ++k) ctx.lineTo(X(dist[k]), Y(hts[k]))
                                    ctx.strokeStyle = "#7FD1AE"; ctx.lineWidth = 2; ctx.stroke()
                                    start = -1
                                }
                            }
                            if (gis.profileMarker >= 0 && gis.profileMarker < hts.length && isFinite(hts[gis.profileMarker])) {
                                var mx = X(dist[gis.profileMarker]), my = Y(hts[gis.profileMarker])
                                ctx.strokeStyle = "#50E6FF"; ctx.lineWidth = 1
                                ctx.beginPath(); ctx.moveTo(mx, 6); ctx.lineTo(mx, 6 + h); ctx.stroke()
                                ctx.fillStyle = "#50E6FF"; ctx.beginPath(); ctx.arc(mx, my, 4, 0, 2 * Math.PI); ctx.fill()
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onPositionChanged: function(mouse) {
                                var p = profileChart.p
                                if (!p || !p.valid) return
                                var w = profileChart.width - profileChart.chartLeft - 8
                                var d = (mouse.x - profileChart.chartLeft) / w * p.length
                                var best = 0
                                for (var i = 1; i < p.distances.length; ++i) if (Math.abs(p.distances[i] - d) < Math.abs(p.distances[best] - d)) best = i
                                gis.profileMarker = best
                                hoverLabel.text = canvas.formatLength(p.distances[best]) + ": "
                                    + (isFinite(p.heights[best]) ? p.heights[best].toLocaleString(Qt.locale("ru_RU"), 'f', 2) + " м" : "нет данных")
                                profileChart.requestPaint()
                            }
                            onExited: { gis.profileMarker = -1; hoverLabel.text = ""; profileChart.requestPaint() }
                        }
                    }
                }
            }
            FileDialog {
                id: profileFile
                title: "Сохранить профиль высот"
                fileMode: FileDialog.SaveFile
                nameFilters: ["CSV (*.csv)"]
                defaultSuffix: "csv"
                onAccepted: {
                    var error = gis.exportProfile(selectedFile)
                    if (error.length > 0) canvas.host.showError(error)
                    else canvas.host.flash("Профиль сохранён: " + gis.displayPath(selectedFile))
                }
            }            Label {
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
                anchors.bottomMargin: canvas.profileShown ? profilePanel.height + 26 : 14
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
