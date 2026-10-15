import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// DEM editor: inside the outlined area (a selected polygon or the AI selection) the heights of
// the elevation model are recomputed in the program's copy of it; undoable.
Dialog {
    id: dialog
    objectName: "demEditorDialog"
    title: "Редактор модели высот"
    modal: false
    anchors.centerIn: Overlay.overlay
    width: 540
    padding: 16
    standardButtons: Dialog.Close
    property string operation: "interpolate"
    property string resultText: ""
    property bool resultOk: true
    readonly property var operations: [
        { id: "interpolate", title: "Убрать объект (высоты по окружению)", text: "Машины, люди, краны, кусты и деревья исчезают с DSM: высоты внутри контура восстанавливаются по его краю (обратные расстояния)." },
        { id: "plane", title: "Выровнять по плоскости контура", text: "Плоскость, проведённая через высоты по краю контура (МНК): площадки, дороги, откосы." },
        { id: "level", title: "Задать высоту", text: "Все ячейки внутри контура получают заданную высоту: проектная отметка, зеркало воды." },
        { id: "smooth", title: "Сгладить", text: "Сглаживание шума внутри контура (среднее 5 × 5 ячеек, три прохода)." }
    ]
    function number(text) { return Number(String(text).replace(",", ".").replace(/\s/g, "")) }
    Connections {
        target: gis
        function onAnalysisFinished(ok, message) { if (dialog.visible) { dialog.resultOk = ok; dialog.resultText = message } }
    }
    contentItem: ColumnLayout {
        spacing: 8
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: gis.hasSelection || gis.hasAiCandidate
                  ? "Контур выбран. Модель: " + (gis.layerCount >= 0 ? (gis.terrainLayerId().length > 0 ? gis.layerDetails(gis.terrainLayerId()).name : "нет на карте") : "")
                  : "Сначала обведите участок: нарисуйте полигон и выберите его (Правка / выбор) или выделите объект ИИ-щелчком."
            color: gis.hasSelection || gis.hasAiCandidate ? Theme.text : Theme.warn; font.pixelSize: 12
        }
        Repeater {
            model: dialog.operations
            delegate: RadioButton {
                required property var modelData
                text: modelData.title
                checked: dialog.operation === modelData.id
                onClicked: dialog.operation = modelData.id
                font.pixelSize: 12
            }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: dialog.operations.filter(function(o) { return o.id === dialog.operation })[0].text
            color: Theme.faint; font.pixelSize: 11
        }
        GridLayout {
            columns: 4
            columnSpacing: 8
            Label { text: "Запас вокруг контура, м"; color: Theme.muted; font.pixelSize: 11 }
            TextField { id: grow; objectName: "demGrow"; text: "0,5"; implicitWidth: 80; selectByMouse: true }
            Label { visible: dialog.operation === "level"; text: "Высота, м"; color: Theme.muted; font.pixelSize: 11; Layout.leftMargin: 10 }
            TextField { id: level; objectName: "demLevel"; visible: dialog.operation === "level"; text: "0"; implicitWidth: 100; selectByMouse: true }
        }
        RowLayout {
            spacing: 10
            FlatButton {
                objectName: "demRun"
                text: "Применить"; emphasis: true
                enabled: !gis.analysisBusy
                onClicked: {
                    dialog.resultText = ""
                    var e = gis.editDem(dialog.operation, dialog.number(level.text), dialog.number(grow.text))
                    if (e.length > 0) { dialog.resultOk = false; dialog.resultText = e }
                }
            }
            FlatButton {
                text: "Заполнить пропуски модели"; compact: true
                enabled: !gis.analysisBusy
                onClicked: {
                    dialog.resultText = ""
                    var e = gis.fillDemGaps("")
                    if (e.length > 0) { dialog.resultOk = false; dialog.resultText = e }
                }
                ToolTip.visible: hovered; ToolTip.delay: 500
                ToolTip.text: "Дыры внутри модели (не её внешний край) заполняются по окружению; результат — новая копия"
            }
            BusyIndicator { implicitWidth: 22; implicitHeight: 22; running: gis.analysisBusy; visible: gis.analysisBusy }
        }
        Label {
            Layout.fillWidth: true; visible: dialog.resultText.length > 0; wrapMode: Text.Wrap
            text: dialog.resultText; color: dialog.resultOk ? Theme.good : Theme.bad; font.pixelSize: 12
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: "Правки идут в копию модели в папке analysis проекта; она заменяет модель на карте, исходный файл не меняется. Ctrl+Z отменяет правку."
            color: Theme.faint; font.pixelSize: 10
        }
    }
}
