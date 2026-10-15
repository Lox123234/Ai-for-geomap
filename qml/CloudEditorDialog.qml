import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Point cloud editor: points inside the outline drawn on the map removed, kept or given a class;
// or the whole cloud classified by its height above the DTM and its colour. A new LAS file each
// time; it opens in the 3D view.
Dialog {
    id: dialog
    objectName: "cloudEditorDialog"
    title: "Облако точек: правка и классификация"
    modal: false
    anchors.centerIn: Overlay.overlay
    width: 600
    padding: 16
    standardButtons: Dialog.Close
    property var host
    property string operation: "deleteInside"
    property string cloud: ""
    property string dtm: ""
    property string resultText: ""
    property bool resultOk: true
    readonly property var operations: [
        { id: "deleteInside", title: "Удалить точки внутри контура", text: "Машины, люди, шум, временные объекты пропадают из облака." },
        { id: "keepInside", title: "Оставить только точки внутри контура", text: "Обрезка облака по участку работ." },
        { id: "classifyInside", title: "Присвоить класс точкам внутри контура", text: "Ручная классификация участка." },
        { id: "classify", title: "Классифицировать всё облако (по DTM и цвету)", text: "Земля — в пределах 0,25 м от DTM; зелёные точки выше — растительность низкая (< 0,5 м), средняя (< 2 м), высокая; не зелёные выше 2 м — здания; глубже 1 м под DTM — шум. Правило, а не нейросеть: проверьте результат в 3D." }
    ]
    readonly property var classes: [{ v: 1, t: "1 — прочее" }, { v: 2, t: "2 — земля" }, { v: 3, t: "3 — низкая растительность" },
        { v: 4, t: "4 — средняя растительность" }, { v: 5, t: "5 — высокая растительность" }, { v: 6, t: "6 — здания" },
        { v: 7, t: "7 — шум" }, { v: 9, t: "9 — вода" }, { v: 17, t: "17 — мосты" }]

    function openFor(op) {
        operation = op
        var pc = projectTree.result("pointcloud")
        cloud = pc.path ? appInfo.siblingLas(pc.path) : ""
        var d = projectTree.result("dtm")
        dtm = d.path || ""
        resultText = ""
        open()
    }
    Connections {
        target: gis
        function onCloudEditFinished(ok, message, path) {
            dialog.resultOk = ok
            dialog.resultText = message
            if (ok && path.length > 0) dialog.host.open3D("pointcloud", path)
        }
    }
    contentItem: ColumnLayout {
        spacing: 8
        Label { text: "Облако точек (LAS)"; color: Theme.muted; font.pixelSize: 11 }
        RowLayout {
            Layout.fillWidth: true
            TextField { Layout.fillWidth: true; text: dialog.cloud; selectByMouse: true; onEditingFinished: dialog.cloud = text; font.pixelSize: 11 }
            FlatButton { text: "Обзор…"; compact: true; onClicked: lasFile.open() }
        }
        Label {
            visible: dialog.cloud.length === 0
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: "В результатах нет облака LAS. Профили «Сбалансированный» и «Высокое качество» сохраняют его рядом с LAZ; можно выбрать файл вручную."
            color: Theme.warn; font.pixelSize: 11
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
        ComboBox {
            id: classBox
            visible: dialog.operation === "classifyInside"
            implicitWidth: 280
            model: dialog.classes; textRole: "t"; valueRole: "v"; currentIndex: 6
        }
        RowLayout {
            visible: dialog.operation === "classify"
            Layout.fillWidth: true
            Label { text: "DTM"; color: Theme.muted; font.pixelSize: 11 }
            TextField { Layout.fillWidth: true; text: dialog.dtm; selectByMouse: true; onEditingFinished: dialog.dtm = text; font.pixelSize: 11 }
        }
        Label {
            visible: dialog.operation !== "classify"
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: gis.hasSelection || gis.hasAiCandidate ? "Контур: выбранный полигон или ИИ-выделение на карте."
                                                         : "Сначала обведите участок на карте: полигон (и выберите его) или ИИ-выделение."
            color: gis.hasSelection || gis.hasAiCandidate ? Theme.muted : Theme.warn; font.pixelSize: 11
        }
        RowLayout {
            spacing: 10
            FlatButton {
                objectName: "cloudRun"
                text: dialog.operation === "classify" ? "Классифицировать" : "Применить"; emphasis: true
                enabled: !gis.analysisBusy && dialog.cloud.length > 0
                onClicked: {
                    dialog.resultText = ""
                    var e = gis.editPointCloud(dialog.cloud, dialog.operation, classBox.currentValue || 7, dialog.dtm)
                    if (e.length > 0) { dialog.resultOk = false; dialog.resultText = e }
                }
            }
            BusyIndicator { implicitWidth: 22; implicitHeight: 22; running: gis.analysisBusy; visible: gis.analysisBusy }
            Label { Layout.fillWidth: true; text: gis.analysisStatus; color: Theme.muted; font.pixelSize: 11; elide: Text.ElideRight }
        }
        Label {
            Layout.fillWidth: true; visible: dialog.resultText.length > 0; wrapMode: Text.Wrap
            text: dialog.resultText; color: dialog.resultOk ? Theme.good : Theme.bad; font.pixelSize: 12
        }
    }
    FileDialog {
        id: lasFile
        title: "Облако точек LAS"
        nameFilters: ["LAS (*.las)"]
        onAccepted: dialog.cloud = gis.displayPath(selectedFile)
    }
}
