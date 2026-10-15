import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// "Экспорт результатов": the results of a processing job copied to a folder of the user's
// choice; rasters as GeoTIFF or COG, in their CRS or another one, optionally resampled.
Dialog {
    id: dialog
    objectName: "exportDialog"
    title: "Экспорт результатов"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 620
    padding: 16
    standardButtons: Dialog.Close

    property var host
    property string jobId: ""
    property var items: []
    property var checked: ({})
    property url folder: ""
    property string resultText: ""
    property bool resultOk: true
    readonly property bool anyRaster: items.some(function(i) { return i.raster && checked[i.file] === true })
    readonly property int count: items.filter(function(i) { return checked[i.file] === true }).length
    readonly property string mapEpsg: {
        var m = /EPSG:(\d+)/.exec(gis.crsLabel || "")
        return m ? m[1] : ""
    }

    // kind: "" (all), "dem" (DSM and DTM), "orthophoto", "pointcloud", "mesh" or "report".
    function openFor(kind) {
        dialog.jobId = (dialog.host && dialog.host.selectedJobId.length > 0 && exporter.results(dialog.host.selectedJobId).length > 0)
            ? dialog.host.selectedJobId : exporter.latestJobWithResults()
        dialog.items = dialog.jobId.length > 0 ? exporter.results(dialog.jobId) : []
        var c = {}
        for (var i = 0; i < dialog.items.length; ++i) {
            var k = dialog.items[i].kind
            c[dialog.items[i].file] = kind === "" || k === kind || (kind === "dem" && (k === "dsm" || k === "dtm"))
        }
        dialog.checked = c
        dialog.resultText = ""
        open()
    }
    function setChecked(file, on) { var c = Object.assign({}, dialog.checked); c[file] = on; dialog.checked = c }
    function crsText() {
        switch (crsBox.currentIndex) {
        case 1: return dialog.mapEpsg
        case 2: return "4326"
        case 3: return otherCrs.text.trim()
        }
        return ""
    }

    Connections {
        target: exporter
        function onFinished(ok, message, files) { dialog.resultOk = ok; dialog.resultText = message }
    }
    FolderDialog {
        id: folderDialog
        title: "Папка для экспорта"
        onAccepted: dialog.folder = selectedFolder
    }

    contentItem: ColumnLayout {
        spacing: 8
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: dialog.jobId.length > 0 ? "Задание " + dialog.jobId : "В проекте нет заданий с результатами. Сначала выполните обработку."
            color: dialog.jobId.length > 0 ? Theme.muted : Theme.warn; font.pixelSize: 11
        }
        Repeater {
            model: dialog.items
            delegate: CheckBox {
                required property var modelData
                Layout.fillWidth: true
                checked: dialog.checked[modelData.file] === true
                text: modelData.title + " · " + modelData.size + "   " + modelData.file
                font.pixelSize: 12
                onToggled: dialog.setChecked(modelData.file, checked)
            }
        }
        GridLayout {
            visible: dialog.anyRaster
            columns: 2
            columnSpacing: 10
            rowSpacing: 6
            Layout.topMargin: 6
            Label { text: "Система координат растров"; color: Theme.muted; font.pixelSize: 11 }
            RowLayout {
                ComboBox {
                    id: crsBox
                    objectName: "exportCrs"
                    implicitWidth: 250
                    model: ["Как в результате", dialog.mapEpsg.length > 0 ? "CRS карты (EPSG:" + dialog.mapEpsg + ")" : "CRS карты не задана",
                            "WGS 84 (EPSG:4326)", "Другая (EPSG или WKT)…"]
                    font.pixelSize: 11
                }
                TextField { id: otherCrs; visible: crsBox.currentIndex === 3; placeholderText: "например, 3857"; implicitWidth: 150 }
            }
            Label { text: "Формат"; color: Theme.muted; font.pixelSize: 11 }
            RowLayout {
                RadioButton { id: tiffButton; text: "GeoTIFF"; checked: true; font.pixelSize: 11 }
                RadioButton { id: cogButton; text: "Cloud Optimized GeoTIFF (COG)"; font.pixelSize: 11 }
            }
            Label { text: "Размер ячейки"; color: Theme.muted; font.pixelSize: 11 }
            RowLayout {
                TextField { id: cellField; implicitWidth: 90; placeholderText: "как есть"; selectByMouse: true }
                Label { text: "в единицах выбранной CRS (м для UTM, градусы для WGS 84)"; color: Theme.faint; font.pixelSize: 10 }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            Label { text: "Папка"; color: Theme.muted; font.pixelSize: 11 }
            Label {
                Layout.fillWidth: true
                text: dialog.folder.toString().length > 0 ? gis.displayPath(dialog.folder) : "не выбрана"
                elide: Text.ElideMiddle
                color: Theme.text; font.pixelSize: 11
            }
            FlatButton { text: "Выбрать…"; compact: true; onClicked: folderDialog.open() }
        }
        RowLayout {
            spacing: 10
            FlatButton {
                objectName: "exportRun"
                text: "Экспортировать (" + dialog.count + ")"
                emphasis: true
                enabled: !exporter.busy && dialog.count > 0 && dialog.folder.toString().length > 0
                onClicked: {
                    dialog.resultText = ""
                    var files = dialog.items.filter(function(i) { return dialog.checked[i.file] === true }).map(function(i) { return i.file })
                    var cell = Number(cellField.text.replace(",", "."))
                    var error = exporter.start(dialog.jobId, files, dialog.folder,
                                               { crs: dialog.crsText(), cog: cogButton.checked, resolution: isNaN(cell) ? 0 : cell })
                    if (error.length > 0) { dialog.resultOk = false; dialog.resultText = error }
                }
            }
            FlatButton { text: "Отменить"; compact: true; visible: exporter.busy; caution: true; onClicked: exporter.cancel() }
            BusyIndicator { implicitWidth: 22; implicitHeight: 22; running: exporter.busy; visible: exporter.busy }
            Label { Layout.fillWidth: true; text: exporter.status; color: Theme.muted; font.pixelSize: 11; elide: Text.ElideRight }
        }
        Label {
            objectName: "exportResult"
            Layout.fillWidth: true
            visible: dialog.resultText.length > 0
            wrapMode: Text.Wrap
            text: dialog.resultText
            color: dialog.resultOk ? Theme.good : Theme.bad; font.pixelSize: 12
        }
        FlatButton {
            visible: dialog.resultOk && dialog.resultText.length > 0
            text: "Открыть папку"; compact: true
            onClicked: Qt.openUrlExternally(dialog.folder)
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "Исходные файлы задания не меняются; существующие файлы в папке не перезаписываются. Модель копируется вместе с материалами и текстурами."
            color: Theme.faint; font.pixelSize: 10
        }
    }
}
