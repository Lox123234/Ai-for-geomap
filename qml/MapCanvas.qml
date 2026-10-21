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
                                         "profile": "Профиль высот", "volume": "Объём", "crossProfile": "Поперечные профили",
                                         "slope": "Уклон", "height": "Высота точки", "angle": "Угол", "viewshed": "Видимость",
                                         "aiSelect": "ИИ-выделение", "marker": "Маркер", "georef": "Привязка" })
    readonly property var toolHints: ({
        "slope": "Щелчок — первая точка, второй щелчок — уклон между ними · Esc — отмена",
        "angle": "Три щелчка: первая точка, вершина угла, вторая точка · Esc — отмена",
        "height": "Щелчок — высота точки по моделям высот на карте",
        "viewshed": "Щелчок — точка наблюдения (настройки справа)",
        "marker": "Щелчок — маркер с координатами и высотой",
        "aiSelect": "Щелчок — объект · Shift — добавить · Ctrl — исключить · Enter — готово",
        "georef": "Точка на снимке, затем — это место на карте · Esc — отмена" })
    function number(text) { var v = Number(String(text).replace(",", ".").replace(/\s/g, "")); return v }
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
                // Background map: none, a street map or satellite imagery, or a file.
                ComboBox {
                    id: basemapBox
                    objectName: "basemapBox"
                    implicitHeight: 24; implicitWidth: 210
                    font.pixelSize: 11
                    enabled: gis.crsDefined
                    textRole: "title"
                    valueRole: "id"
                    property var providers: gis.layerCount >= 0 ? gis.basemapProviders() : []
                    model: [{ id: "", title: "Подложка: нет" }].concat(providers.map(function(p) { return { id: p.id, title: p.title } }))
                                                            .concat([{ id: "file", title: "Подложка из файла…" }])
                    currentIndex: {
                        if (!gis.basemapVisible) return 0
                        for (var i = 0; i < providers.length; ++i) if (providers[i].current) return i + 1
                        return gis.hasBasemap ? providers.length + 1 : 0
                    }
                    onActivated: function(index) {
                        var id = currentValue
                        if (id === "") { gis.setBasemapVisible(false); return }
                        if (id === "file") { canvas.host.addBasemapFile(); return }
                        var p = providers[index - 1]
                        if (p.current) { gis.setBasemapVisible(true); return }
                        if (!p.accepted) { canvas.host.openBasemap(id); return }   // terms first
                        var e = gis.setOnlineBasemap(id)
                        if (e.length > 0) canvas.host.showError(e)
                    }
                }
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
                visible: ["length", "area", "line", "polygon", "profile", "volume", "crossProfile", "slope", "angle", "height", "viewshed", "aiSelect", "marker"].indexOf(gis.tool) >= 0
                // Centred, below the measurement box when both would overlap.
                anchors.horizontalCenter: parent.horizontalCenter; anchors.top: parent.top
                anchors.topMargin: measureBox.visible && measureBox.width + 24 > (parent.width - width) / 2 ? measureBox.height + 20 : 12
                text: canvas.toolHints[gis.tool] || "Щелчок — точка · двойной щелчок — готово · Esc — отмена"
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
            // Cross sections and viewshed: their settings.
            Rectangle {
                objectName: "terrainSettings"
                visible: gis.tool === "crossProfile" || gis.tool === "viewshed"
                anchors.right: parent.right; anchors.margins: 12
                anchors.top: toolHint.visible ? toolHint.bottom : parent.top
                anchors.topMargin: toolHint.visible ? 8 : 12
                width: settingsColumn.implicitWidth + 20
                height: settingsColumn.implicitHeight + 16
                radius: 6
                color: "#E61A1F27"; border.color: Theme.line
                ColumnLayout {
                    id: settingsColumn
                    anchors.fill: parent; anchors.margins: 8
                    spacing: 4
                    Label {
                        text: gis.tool === "viewshed" ? "Видимость" : "Поперечные профили"
                        color: Theme.muted; font.pixelSize: 10; font.bold: true
                    }
                    Repeater {
                        model: gis.tool === "viewshed"
                               ? [{ t: "Наблюдатель над поверхностью, м", p: "viewObserverHeight" }, { t: "Цель над поверхностью, м", p: "viewTargetHeight" },
                                  { t: "Радиус, м", p: "viewRadius" }]
                               : [{ t: "Шаг между профилями, м", p: "crossSpacing" }, { t: "Ширина в каждую сторону, м", p: "crossHalfWidth" }]
                        delegate: RowLayout {
                            required property var modelData
                            spacing: 6
                            Label { text: modelData.t; color: Theme.muted; font.pixelSize: 11; Layout.preferredWidth: 190 }
                            TextField {
                                objectName: modelData.p
                                implicitWidth: 80; implicitHeight: 26; font.pixelSize: 11; selectByMouse: true
                                text: gis[modelData.p].toLocaleString(Qt.locale("ru_RU"), 'f', 1)
                                onEditingFinished: { var v = canvas.number(text); if (!isNaN(v) && v >= 0) gis[modelData.p] = v }
                            }
                        }
                    }
                    Label { text: "Модель: " + (gis.layerCount >= 0 ? canvas.terrainName() : ""); color: Theme.faint; font.pixelSize: 10 }
                }
            }
            // AI selection: what the model proposes and what to do with it.
            Rectangle {
                objectName: "aiPanel"
                visible: gis.tool === "aiSelect"
                anchors.right: parent.right; anchors.margins: 12
                anchors.top: toolHint.visible ? toolHint.bottom : parent.top
                anchors.topMargin: toolHint.visible ? 8 : 12
                width: 300
                height: aiColumn.implicitHeight + 16
                radius: 6
                color: "#E61A1F27"; border.color: Theme.line
                ColumnLayout {
                    id: aiColumn
                    anchors.fill: parent; anchors.margins: 8
                    spacing: 6
                    RowLayout {
                        Label { text: "ИИ-ВЫДЕЛЕНИЕ"; color: Theme.muted; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2; Layout.fillWidth: true }
                        BusyIndicator { visible: gis.aiBusy; running: gis.aiBusy; implicitWidth: 18; implicitHeight: 18 }
                    }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        visible: aiModels.segmentationProblem.length > 0
                        text: aiModels.segmentationProblem; color: Theme.bad; font.pixelSize: 11
                    }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: gis.aiStatus.length > 0 ? gis.aiStatus : "Щёлкните по объекту на ортофотоплане: крыша, дерево, машина, водоём…"
                        color: Theme.text; font.pixelSize: 11
                    }
                    RowLayout {
                        spacing: 6
                        FlatButton {
                            objectName: "aiAccept"
                            text: "На карту"; compact: true; implicitHeight: 26; emphasis: true
                            enabled: gis.hasAiCandidate && !gis.aiBusy
                            onClicked: { var n = gis.acceptAiCandidate(); if (n.length > 0) canvas.host.flash("Добавлено в объекты: " + n) }
                            ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Добавить контур в нарисованные объекты (Enter)"
                        }
                        FlatButton {
                            objectName: "aiRemove"
                            text: "Убрать с ортофотоплана"; compact: true; implicitHeight: 26
                            enabled: gis.hasAiCandidate && !gis.aiBusy && aiModels.inpaintingProblem.length === 0
                            onClicked: { var e = gis.removeFromOrthophoto(0.3); if (e.length > 0) canvas.host.showError(e) }
                            ToolTip.visible: hovered; ToolTip.delay: 500
                            ToolTip.text: aiModels.inpaintingProblem.length > 0 ? aiModels.inpaintingProblem
                                          : "Дорисовать место объекта по окружению (в копии, можно отменить)"
                        }
                        FlatButton { text: "Сброс"; compact: true; implicitHeight: 26; enabled: gis.hasAiCandidate || gis.aiBusy; onClicked: gis.clearAiCandidate() }
                    }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: "Локально, без интернета: " + aiModels.runtimeVersion + ". Модели и их лицензии — Анализ → Модели ИИ."
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
                        Label {
                            text: gis.profile.sections > 0 ? "ПОПЕРЕЧНЫЙ ПРОФИЛЬ " + (gis.profile.section + 1) + " / " + gis.profile.sections : "ПРОФИЛЬ ВЫСОТ"
                            color: Theme.muted; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2
                        }
                        FlatButton { visible: gis.profile.sections > 0; text: "‹"; compact: true; implicitHeight: 24; enabled: gis.crossIndex > 0
                                     onClicked: gis.crossIndex = gis.crossIndex - 1 }
                        FlatButton { visible: gis.profile.sections > 0; text: "›"; compact: true; implicitHeight: 24; enabled: gis.crossIndex < gis.crossCount - 1
                                     onClicked: gis.crossIndex = gis.crossIndex + 1 }
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
            AppFileDialog {
                id: profileFile
                kind: "export"
                title: "Сохранить профиль высот"
                fileMode: FileDialog.SaveFile
                nameFilters: ["CSV (*.csv)"]
                defaultSuffix: "csv"
                onAccepted: {
                    var error = gis.crossCount > 0 ? gis.exportCrossSections(selectedFile) : gis.exportProfile(selectedFile)
                    if (error.length > 0) canvas.host.showError(error)
                    else canvas.host.flash((gis.crossCount > 0 ? "Поперечные профили сохранены: " : "Профиль сохранён: ") + gis.displayPath(selectedFile))
                }
            }            Label {
                anchors.centerIn: parent
                visible: !gis.crsDefined
                width: Math.min(parent.width - 60, 460)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: "Система координат не выбрана.\nДобавьте снимки с GPS или геоданные."
                color: Theme.muted; font.pixelSize: 13
            }
            // On-screen legend of the visible layers (Вид → Показать легенду).
            Rectangle {
                id: legendBox
                objectName: "mapLegend"
                visible: canvas.host && canvas.host.legendShown && gis.crsDefined
                anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 12
                anchors.bottomMargin: (canvas.profileShown ? profilePanel.height + 24 : 12) + (gis.basemapAttribution.length > 0 ? 16 : 0)
                width: 240
                height: Math.min(legendColumn.implicitHeight + 16, parent.height * 0.6)
                radius: 6
                color: "#E61A1F27"; border.color: Theme.line
                clip: true
                ColumnLayout {
                    id: legendColumn
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8
                    spacing: 5
                    Label { text: "ЛЕГЕНДА"; color: Theme.muted; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2 }
                    Repeater {
                        model: gis.layerModel
                        delegate: ColumnLayout {
                            id: entry
                            required property var model
                            readonly property var stops: model.legend || []
                            readonly property bool ramp: stops.length > 0
                            visible: model.layerVisible === true
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                spacing: 6
                                Rectangle {
                                    implicitWidth: 14; implicitHeight: 10; radius: entry.model.layerType === "photos" ? 5 : 2
                                    visible: !entry.ramp
                                    color: entry.model.layerType === "raster" ? "#6E8C6E" : entry.model.layerColor
                                }
                                Label { Layout.fillWidth: true; text: entry.model.name; color: Theme.text; font.pixelSize: 11; elide: Text.ElideRight }
                            }
                            Canvas {
                                visible: entry.ramp
                                Layout.fillWidth: true; implicitHeight: 8
                                onWidthChanged: requestPaint()
                                onPaint: {
                                    var ctx = getContext("2d")
                                    ctx.reset()
                                    var g = ctx.createLinearGradient(0, 0, width, 0)
                                    for (var i = 0; i < entry.stops.length; ++i) g.addColorStop(entry.stops[i].position, entry.stops[i].color)
                                    ctx.fillStyle = g
                                    ctx.fillRect(0, 0, width, height)
                                }
                            }
                            RowLayout {
                                visible: entry.ramp && entry.model.rasterStyle !== undefined
                                Layout.fillWidth: true
                                Label { text: entry.model.rasterStyle ? Number(entry.model.rasterStyle.min).toLocaleString(Qt.locale("ru_RU"), 'f', 2) : ""; color: Theme.faint; font.pixelSize: 9 }
                                Item { Layout.fillWidth: true }
                                Label { text: entry.model.rasterStyle ? Number(entry.model.rasterStyle.max).toLocaleString(Qt.locale("ru_RU"), 'f', 2) : ""; color: Theme.faint; font.pixelSize: 9 }
                            }
                        }
                    }
                }
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
            // Attribution of the online basemap (required by its terms).
            Label {
                objectName: "basemapAttribution"
                visible: gis.basemapAttribution.length > 0
                anchors.right: parent.right; anchors.bottom: parent.bottom
                anchors.bottomMargin: canvas.profileShown ? profilePanel.height + 12 : 0
                text: gis.basemapAttribution
                padding: 3; leftPadding: 6; rightPadding: 6
                color: "#2a2f36"; font.pixelSize: 10
                background: Rectangle { color: "#CCFFFFFF" }
            }
        }
    }
}
