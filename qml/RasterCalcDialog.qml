import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Vegetation indices and the raster calculator over the bands of a raster on the map (an
// orthophoto, a multispectral mosaic, a DEM). The result is a new layer in <project>/analysis.
Dialog {
    id: dialog
    objectName: "rasterCalcDialog"
    title: "Индексы и растровый калькулятор"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 580
    padding: 16
    standardButtons: Dialog.Close

    property string index: "exg"        // exg | vari | gli | ndvi | calc
    property var layers: []
    property string layerId: ""
    property string resultText: ""
    property bool resultOk: true
    readonly property var current: layers.filter(function(l) { return l.id === dialog.layerId })[0]
    readonly property int bands: current ? current.bands : 0

    readonly property var indices: [
        { id: "exg", title: "ExG — индекс зелени по RGB", text: "2g − r − b в долях яркости: растительность > 0,05, почва и асфальт около 0. Работает на обычном RGB-ортофотоплане." },
        { id: "vari", title: "VARI — устойчивый к освещению", text: "(G − R) / (G + R − B): меньше зависит от теней и дымки; растительность > 0,05." },
        { id: "gli", title: "GLI — индекс зелёного листа", text: "(2G − R − B) / (2G + R + B): густота зелёной листвы; растительность > 0,05." },
        { id: "ndvi", title: "NDVI — по ближнему инфракрасному каналу", text: "(NIR − Red) / (NIR + Red): нужен мультиспектральный снимок с каналом NIR. Здоровая растительность > 0,3." },
        { id: "calc", title: "Своё выражение (растровый калькулятор)", text: "Каналы B1, B2, …; числа; + − * / и скобки; функции sqrt(), abs(), min(a, b), max(a, b). Пример: (B4 − B3) / (B4 + B3)." }
    ]

    function openFor(k) {
        index = k
        layers = gis.rasterLayers()
        layerId = ""
        for (var i = 0; i < layers.length && layerId.length === 0; ++i)
            if ((k === "calc" || layers[i].rgb) && !layers[i].elevation) layerId = layers[i].id
        if (layerId.length === 0 && layers.length > 0) layerId = layers[0].id
        resultText = ""
        open()
    }
    function number(text) { return Number(String(text).replace(",", ".").replace(/\s/g, "")) }

    Connections {
        target: gis
        function onAnalysisFinished(ok, message) { if (dialog.visible) { dialog.resultOk = ok; dialog.resultText = message } }
    }

    contentItem: ColumnLayout {
        spacing: 10
        Label { text: "Растр"; color: Theme.muted; font.pixelSize: 11 }
        ComboBox {
            Layout.fillWidth: true
            model: dialog.layers
            textRole: "name"; valueRole: "id"
            enabled: dialog.layers.length > 0
            currentIndex: Math.max(0, indexOfValue(dialog.layerId))
            onActivated: dialog.layerId = currentValue
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.layers.length === 0
            wrapMode: Text.Wrap
            text: "На карте нет растров. Добавьте ортофотоплан: «Вид → Ортофотоплан» из результатов обработки или «Файл → Импорт ортофотоплана»."
            color: Theme.warn; font.pixelSize: 12
        }
        Label { text: "Каналов: " + dialog.bands; color: Theme.faint; font.pixelSize: 10; visible: dialog.bands > 0 }
        Label { text: "Что рассчитать"; color: Theme.muted; font.pixelSize: 11; Layout.topMargin: 4 }
        Repeater {
            model: dialog.indices
            delegate: RadioButton {
                required property var modelData
                text: modelData.title
                checked: dialog.index === modelData.id
                onClicked: dialog.index = modelData.id
                font.pixelSize: 12
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: dialog.indices.filter(function(k) { return k.id === dialog.index })[0].text
            color: Theme.faint; font.pixelSize: 11
        }
        GridLayout {
            visible: dialog.index !== "calc"
            columns: 8
            columnSpacing: 6
            Repeater {
                model: dialog.index === "ndvi" ? [{ t: "Red", f: "red", v: 3 }, { t: "NIR", f: "nir", v: 4 }]
                                               : [{ t: "R", f: "red", v: 1 }, { t: "G", f: "green", v: 2 }, { t: "B", f: "blue", v: 3 }]
                delegate: RowLayout {
                    required property var modelData
                    Label { text: modelData.t + " = B"; color: Theme.muted; font.pixelSize: 11 }
                    SpinBox {
                        objectName: "band_" + modelData.f
                        from: 1; to: Math.max(1, dialog.bands); value: Math.min(modelData.v, Math.max(1, dialog.bands))
                        implicitWidth: 90; implicitHeight: 28; editable: true
                        onValueChanged: bandValues[modelData.f] = value
                        Component.onCompleted: bandValues[modelData.f] = value
                    }
                }
            }
        }
        TextField {
            id: expression
            objectName: "calcExpression"
            visible: dialog.index === "calc"
            Layout.fillWidth: true
            placeholderText: "(B4 - B3) / (B4 + B3)"
            selectByMouse: true
            font.family: "Consolas"
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.index === "calc" && expression.text.length > 0
            readonly property string problem: dialog.layerId.length > 0 ? gis.checkRasterExpression(dialog.layerId, expression.text) : ""
            text: problem.length > 0 ? problem : "Выражение верно"
            color: problem.length > 0 ? Theme.bad : Theme.good; font.pixelSize: 11
        }
        RowLayout {
            Layout.topMargin: 6
            spacing: 10
            FlatButton {
                objectName: "calcRun"
                text: "Рассчитать"
                emphasis: true
                enabled: !gis.analysisBusy && dialog.layerId.length > 0
                onClicked: {
                    dialog.resultText = ""
                    var error = gis.calculateRaster(dialog.layerId, dialog.index, expression.text,
                                                    bandValues.red || 1, bandValues.green || 2, bandValues.blue || 3, bandValues.nir || 4)
                    if (error.length > 0) { dialog.resultOk = false; dialog.resultText = error }
                }
            }
            BusyIndicator { implicitWidth: 22; implicitHeight: 22; running: gis.analysisBusy; visible: gis.analysisBusy }
            Label { Layout.fillWidth: true; text: gis.analysisStatus; color: Theme.muted; font.pixelSize: 11; elide: Text.ElideRight }
        }
        Label {
            objectName: "calcResult"
            Layout.fillWidth: true
            visible: dialog.resultText.length > 0
            wrapMode: Text.Wrap
            text: dialog.resultText
            color: dialog.resultOk ? Theme.good : Theme.bad; font.pixelSize: 12
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "Результат — растр Float32 в папке analysis проекта, слой на карте со шкалой «почва → растительность». Прозрачные и пустые пиксели исходника остаются пустыми."
            color: Theme.faint; font.pixelSize: 10
        }
    }
    property var bandValues: ({})
}
