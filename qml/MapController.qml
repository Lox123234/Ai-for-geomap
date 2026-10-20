import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Dialogs of the map and the functions that open them, shared by the menus, the toolbar, the
// layer panel and the layer properties. Results go to the window through the signals.
Item {
    id: controller
    signal errorMessage(string text)
    signal noticeMessage(string text)

    property string crsTarget: ""        // "" = CRS of the map, else id of the layer to assign a CRS to
    property var details: ({})           // properties of the selected layer

    readonly property var rasterFilters: ["Растры (*.tif *.tiff *.vrt *.img *.asc)", "Все файлы (*)"]
    readonly property var vectorFilters: ["Векторы (*.geojson *.json *.kml *.gpkg)", "Все файлы (*)"]
    readonly property var allFilters: ["Геоданные (*.tif *.tiff *.vrt *.img *.asc *.geojson *.json *.kml *.gpkg)",
                                       "Растры (*.tif *.tiff *.vrt *.img *.asc)", "Векторы (*.geojson *.json *.kml *.gpkg)", "Все файлы (*)"]

    function refreshDetails() {
        controller.details = gis.selectedLayer.length > 0 ? gis.layerDetails(gis.selectedLayer) : ({})
    }
    // kind: "data" (any), "raster" (orthophoto, DEM) or "vector"; the format is detected from the file.
    function addLayer(kind, title) {
        addLayerDialog.title = title || "Добавить слой на карту"
        addLayerDialog.nameFilters = kind === "raster" ? rasterFilters : kind === "vector" ? vectorFilters : allFilters
        addLayerDialog.open()
    }
    function addBasemap() { basemapDialog.open() }
    function exportMap() { exportMapDialog.open() }
    function exportDrawings() { exportDrawingsDialog.open() }
    function chooseMapCrs() { controller.crsTarget = ""; crsDialog.open() }
    function assignLayerCrs(layerId) { controller.crsTarget = layerId; crsDialog.open() }

    Connections {
        target: gis
        function onSelectionChanged() { controller.refreshDetails() }
        function onLayersChanged() { controller.refreshDetails() }
        function onErrorOccurred(message) { controller.errorMessage(message) }
        function onExportFinished(ok, message) { if (ok) controller.noticeMessage(message); else controller.errorMessage(message) }
    }

    AppFileDialog {
        id: addLayerDialog
        kind: "import"
        title: "Добавить слой на карту"
        fileMode: FileDialog.OpenFile
        nameFilters: controller.allFilters
        onAccepted: {
            var error = gis.addLayerFromUrl(selectedFile)
            if (error.length > 0) controller.errorMessage(error)
        }
    }
    AppFileDialog {
        id: basemapDialog
        kind: "import"
        title: "Подложка карты из файла"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Подложки (*.mbtiles *.gpkg *.tif *.tiff *.vrt)", "Все файлы (*)"]
        onAccepted: {
            var error = gis.addBasemap(selectedFile)
            if (error.length > 0) controller.errorMessage(error)
        }
    }
    AppFileDialog {
        id: exportMapDialog
        kind: "export"
        title: "Экспорт карты"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: ["Изображение PNG (*.png)", "Документ PDF (*.pdf)"]
        onAccepted: gis.exportMap(selectedFile)
    }
    AppFileDialog {
        id: exportDrawingsDialog
        kind: "export"
        title: "Экспорт нарисованных объектов"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "geojson"
        nameFilters: ["GeoJSON (*.geojson)", "GeoPackage (*.gpkg)", "KML (*.kml)"]
        onAccepted: {
            var error = gis.exportDrawings(selectedFile)
            if (error.length > 0) controller.errorMessage(error)
            else controller.noticeMessage("Объекты сохранены: " + gis.displayPath(selectedFile))
        }
    }

    // Choosing a CRS: never guessed for data that has none; the user enters an EPSG code and
    // sees what it means before applying it.
    Dialog {
        id: crsDialog
        title: controller.crsTarget.length === 0 ? "Система координат карты" : "Система координат слоя"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: 540
        background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: 8 }
        onOpened: { crsField.text = ""; crsField.forceActiveFocus() }
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: controller.crsTarget.length === 0
                    ? "Все слои пересчитаются в эту систему."
                    : "У файла нет системы координат. Укажите её, только если знаете точно."
                color: controller.crsTarget.length === 0 ? Theme.muted : Theme.warn; font.pixelSize: 12
            }
            TextField {
                id: crsField
                Layout.fillWidth: true
                placeholderText: "EPSG-код, например 32615, 3857, 4326, или EPSG:…"
                color: Theme.text
                placeholderTextColor: Theme.faint
                selectByMouse: true
                background: Rectangle { color: Theme.field; radius: 4; border.color: crsField.activeFocus ? Theme.accent : Theme.line }
            }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: crsField.text.trim().length > 0 ? gis.describeCrs(crsField.text) : ""
                color: text.startsWith("✕") ? Theme.bad : Theme.good; font.pixelSize: 12
            }
        }
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: {
            var error = controller.crsTarget.length === 0 ? gis.setMapCrs(crsField.text) : gis.assignLayerCrs(controller.crsTarget, crsField.text)
            if (error.length > 0) controller.errorMessage(error)
        }
    }
}
