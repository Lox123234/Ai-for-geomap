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

    property string kind: "slope"        // slope | slopePercent | aspect | contours
    property var layers: []
    property string layerId: ""
    property string resultText: ""
    property bool resultOk: true

    readonly property var kinds: [
        { id: "slope", title: "Карта уклонов, градусы", text: "Крутизна склона в каждой ячейке: 0° — горизонтально. Шкала от зелёного (пологие) к красному (крутые)." },
        { id: "slopePercent", title: "Карта уклонов, проценты", text: "Уклон в процентах: подъём на 100 м пути (10 % ≈ 5,7°). Удобно для дорог и дренажа." },
        { id: "aspect", title: "Экспозиция склонов", text: "Куда обращён склон: 0° — север, 90° — восток, 180° — юг, 270° — запад. Ровные участки без значения." },
        { id: "contours", title: "Горизонтали", text: "Линии равных высот с заданным шагом; высота — в поле «elev» слоя GeoPackage." }
    ]

    function openFor(k) {
        kind = k
        layers = gis.elevationLayers()
        var preferred = gis.terrainLayerId()
        layerId = preferred.length > 0 ? preferred : (layers.length > 0 ? layers[0].id : "")
        resultText = ""
        suggest()
        open()
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
        Label { text: "Модель высот"; color: Theme.muted; font.pixelSize: 11 }
        ComboBox {
            id: layerBox
            Layout.fillWidth: true
            model: dialog.layers
            textRole: "name"; valueRole: "id"
            enabled: dialog.layers.length > 0
            currentIndex: Math.max(0, indexOfValue(dialog.layerId))
            onActivated: { dialog.layerId = currentValue; dialog.suggest() }
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.layers.length === 0
            wrapMode: Text.Wrap
            text: "На карте нет модели высот. Добавьте DSM или DTM: «Вид → Цифровая модель высот» (из результатов обработки) или «Файл → Импорт DEM/DSM/DTM»."
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
                    var error = gis.analyzeTerrain(dialog.layerId, dialog.kind, dialog.number(intervalField.text), dialog.number(baseField.text))
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
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "Результат сохраняется в папку analysis проекта и добавляется на карту отдельным слоем; исходная модель высот не меняется. Профиль и объём — инструменты на карте (панель «Измерения»)."
            color: Theme.faint; font.pixelSize: 10
        }
    }
}
