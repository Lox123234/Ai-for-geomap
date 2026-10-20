import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Terrain analysis of an elevation model on the map: slope (degrees or percent), aspect or
// contour lines. The result is written to <project>/analysis and added to the map as a layer.
Dialog {
    id: dialog
    objectName: "terrainDialog"
    title: "Анализ рельефа"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 540
    padding: 16
    standardButtons: Dialog.Close

    property string kind: "slope"        // slope | slopePercent | aspect | contours | difference
    property var layers: []
    property string layerId: ""
    property string otherLayerId: ""
    property string resultText: ""
    property bool resultOk: true

    readonly property var kinds: [
        { id: "slope", title: "Карта уклонов, градусы", text: "Зелёный — пологие, красный — крутые" },
        { id: "slopePercent", title: "Карта уклонов, проценты", text: "Уклон в процентах: подъём на 100 м пути (10 % ≈ 5,7°). Удобно для дорог и дренажа." },
        { id: "aspect", title: "Экспозиция склонов", text: "Куда обращён склон: 0° — север, 90° — восток" },
        { id: "contours", title: "Горизонтали", text: "Линии равных высот с заданным шагом; высота — в поле «elev» слоя GeoPackage." },
        { id: "difference", title: "Разность моделей A − B", text: "DSM − DTM — высота деревьев и зданий; две съёмки — изменения" }
    ]

    function openFor(k) {
        kind = k
        layers = gis.elevationLayers()
        var preferred = gis.terrainLayerId()
        layerId = preferred.length > 0 ? preferred : (layers.length > 0 ? layers[0].id : "")
        otherLayerId = ""
        pickOther()
        resultText = ""
        suggest()
        open()
    }
    // Model B of the difference: another model of heights (not a slope map or a viewshed), else none.
    function pickOther() {
        if (otherLayerId.length > 0 && otherLayerId !== layerId) return
        otherLayerId = ""
        for (var i = 0; i < layers.length; ++i)
            if (layers[i].id !== layerId && !layers[i].derived) { otherLayerId = layers[i].id; break }
    }
    function suggest() {
        if (layerId.length === 0) return
        var s = gis.contourSuggestion(layerId)
        intervalField.text = Number(s.interval).toLocaleString(Qt.locale("ru_RU"), 'f', s.interval < 1 ? 2 : (s.interval % 1 === 0 ? 0 : 1))
        baseField.text = "0"
    }
    function number(text) { return Number(String(text).replace(",", ".").replace(/\s/g, "")) }

    Connections {
        target: gis
        function onAnalysisFinished(ok, message) { dialog.resultOk = ok; dialog.resultText = message }
    }

    contentItem: ColumnLayout {
        spacing: 10
        Label { text: dialog.kind === "difference" ? "Модель A" : "Модель высот"; color: Theme.muted; font.pixelSize: 11 }
        ComboBox {
            id: layerBox
            Layout.fillWidth: true
            model: dialog.layers
            textRole: "name"; valueRole: "id"
            enabled: dialog.layers.length > 0
            currentIndex: Math.max(0, indexOfValue(dialog.layerId))
            onActivated: { dialog.layerId = currentValue; dialog.pickOther(); dialog.suggest() }
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.layers.length === 0
            wrapMode: Text.Wrap
            text: "На карте нет модели высот — добавьте DSM или DTM"
            color: Theme.warn; font.pixelSize: 12
        }
        Label { text: "Что построить"; color: Theme.muted; font.pixelSize: 11; Layout.topMargin: 4 }
        Repeater {
            model: dialog.kinds
            delegate: RadioButton {
                required property var modelData
                text: modelData.title
                checked: dialog.kind === modelData.id
                onClicked: dialog.kind = modelData.id
                font.pixelSize: 12
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: dialog.kinds.filter(function(k) { return k.id === dialog.kind })[0].text
            color: Theme.faint; font.pixelSize: 11
        }
        RowLayout {
            visible: dialog.kind === "difference"
            Layout.fillWidth: true
            Label { text: "Модель B (вычитается)"; color: Theme.muted; font.pixelSize: 11 }
            ComboBox {
                Layout.fillWidth: true
                model: dialog.layers
                textRole: "name"; valueRole: "id"
                currentIndex: indexOfValue(dialog.otherLayerId)
                displayText: currentIndex >= 0 ? currentText : "Нужна вторая модель высот"
                onActivated: dialog.otherLayerId = currentValue
            }
        }
        GridLayout {
            visible: dialog.kind === "contours"
            columns: 4
            columnSpacing: 8
            Label { text: "Шаг, м"; color: Theme.muted; font.pixelSize: 11 }
            TextField { id: intervalField; implicitWidth: 90; selectByMouse: true; inputMethodHints: Qt.ImhFormattedNumbersOnly }
            Label { text: "Базовый уровень, м"; color: Theme.muted; font.pixelSize: 11; Layout.leftMargin: 10 }
            TextField { id: baseField; implicitWidth: 90; selectByMouse: true; text: "0"; inputMethodHints: Qt.ImhFormattedNumbersOnly }
        }
        RowLayout {
            Layout.topMargin: 6
            spacing: 10
            FlatButton {
                objectName: "terrainRun"
                text: "Построить"
                emphasis: true
                enabled: !gis.analysisBusy && dialog.layerId.length > 0
                onClicked: {
                    dialog.resultText = ""
                    var error = gis.analyzeTerrain(dialog.layerId, dialog.kind, dialog.number(intervalField.text), dialog.number(baseField.text),
                                                   dialog.otherLayerId)
                    if (error.length > 0) { dialog.resultOk = false; dialog.resultText = error }
                }
            }
            BusyIndicator { implicitWidth: 22; implicitHeight: 22; running: gis.analysisBusy; visible: gis.analysisBusy }
            Label { Layout.fillWidth: true; text: gis.analysisStatus; color: Theme.muted; font.pixelSize: 11; elide: Text.ElideRight }
        }
        Label {
            objectName: "terrainResult"
            Layout.fillWidth: true
            visible: dialog.resultText.length > 0
            wrapMode: Text.Wrap
            text: dialog.resultText
            color: dialog.resultOk ? Theme.good : Theme.bad; font.pixelSize: 12
        }
    }
}
