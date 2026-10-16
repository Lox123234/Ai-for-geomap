import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Chunks of the project: the list (active, rename, duplicate, delete), split of the active chunk
// (grid over the photo positions or by flights) and merge (photos into a new chunk, or the
// chunks' orthophotos/DSM/DTM into one mosaic).
Dialog {
    id: dialog
    objectName: "chunksDialog"
    title: "Блоки проекта"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 620
    padding: 16
    standardButtons: Dialog.Close
    property var host
    property int tab: 0
    property var selected: []
    property var preview: ({})
    function openFor(page) {
        tab = page === "split" ? 1 : page === "merge" ? 2 : 0
        selected = []
        updatePreview()
        open()
    }
    function toggle(id, on) {
        var list = selected.filter(function(c) { return c !== id })
        if (on) list.push(id)
        selected = list
    }
    function splitOptions() {
        return modeBox.currentIndex === 0
            ? { mode: "grid", rows: rowsBox.value, cols: colsBox.value, overlap: overlapBox.value, minPhotos: minBox.value }
            : { mode: "flights", gapMinutes: gapBox.value, minPhotos: minBox.value }
    }
    function updatePreview() { if (chunks.count > 0) preview = chunks.splitPreview(chunks.active, splitOptions()) }
    onTabChanged: if (tab === 1) updatePreview()
    Connections {
        target: chunks
        function onChanged() { if (dialog.visible && dialog.tab === 1) dialog.updatePreview() }
    }

    contentItem: ColumnLayout {
        spacing: 10
        TabBar {
            id: tabs
            Layout.fillWidth: true
            currentIndex: dialog.tab
            onCurrentIndexChanged: dialog.tab = currentIndex
            TabButton { text: "Блоки" }
            TabButton { text: "Разделить" }
            TabButton { text: "Объединить" }
        }
        StackLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 360
            currentIndex: dialog.tab

            // ---- list
            ColumnLayout {
                spacing: 6
                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: chunks.chunks
                    delegate: Rectangle {
                        id: row
                        required property var modelData
                        width: list.width
                        height: 62
                        radius: 4
                        color: row.modelData.active ? Theme.accentSoft : Theme.panel
                        border.color: Theme.line
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 8
                            RadioButton {
                                checked: row.modelData.active
                                onClicked: chunks.setActive(row.modelData.id)
                                ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Сделать активным"
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                TextField {
                                    Layout.fillWidth: true
                                    implicitHeight: 28
                                    text: row.modelData.name
                                    font.pixelSize: 12
                                    selectByMouse: true
                                    onEditingFinished: {
                                        if (text === row.modelData.name) return
                                        var e = chunks.rename(row.modelData.id, text)
                                        if (e.length > 0) { dialog.host.showError(e); text = row.modelData.name }
                                    }
                                }
                                Label {
                                    text: row.modelData.photos + " фото · с GPS " + row.modelData.gps + " · заданий " + row.modelData.jobs
                                          + (row.modelData.split ? " · часть разделения" : "")
                                    color: Theme.muted; font.pixelSize: 10
                                }
                            }
                            FlatButton {
                                text: "Копия"; compact: true
                                onClicked: chunks.duplicate(row.modelData.id)
                                ToolTip.visible: hovered; ToolTip.delay: 500
                                ToolTip.text: "Новый блок с теми же снимками (результаты обработки остаются у исходного)"
                            }
                            FlatButton {
                                text: "Удалить"; compact: true; caution: true
                                enabled: chunks.count > 1
                                onClicked: { removeDialog.chunkId = row.modelData.id; removeDialog.chunkName = row.modelData.name; removeDialog.open() }
                            }
                        }
                    }
                }
                RowLayout {
                    spacing: 8
                    FlatButton { text: "Новый блок"; onClicked: chunks.create("") }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: "Новые снимки попадают в активный блок; обработка — тоже по снимкам активного блока."
                        color: Theme.faint; font.pixelSize: 10
                    }
                }
            }

            // ---- split
            ColumnLayout {
                spacing: 8
                Label { text: "Разделить блок «" + chunks.activeName + "» (" + chunks.activePhotoCount + " фото)"; color: Theme.text; font.pixelSize: 12 }
                GridLayout {
                    columns: 2
                    columnSpacing: 10; rowSpacing: 6
                    Label { text: "Как"; color: Theme.muted; font.pixelSize: 11 }
                    ComboBox {
                        id: modeBox
                        objectName: "splitMode"
                        Layout.preferredWidth: 300; implicitHeight: 28; font.pixelSize: 11
                        model: ["Сеткой по положению снимков (GPS)", "По полётам (паузы между снимками)"]
                        onActivated: dialog.updatePreview()
                    }
                    Label { text: "Ряды × столбцы"; color: Theme.muted; font.pixelSize: 11; visible: modeBox.currentIndex === 0 }
                    RowLayout {
                        visible: modeBox.currentIndex === 0
                        SpinBox { id: rowsBox; from: 1; to: 20; value: 2; editable: true; onValueModified: dialog.updatePreview() }
                        Label { text: "×"; color: Theme.muted }
                        SpinBox { id: colsBox; from: 1; to: 20; value: 2; editable: true; onValueModified: dialog.updatePreview() }
                    }
                    Label { text: "Перекрытие частей, %"; color: Theme.muted; font.pixelSize: 11; visible: modeBox.currentIndex === 0 }
                    SpinBox { id: overlapBox; visible: modeBox.currentIndex === 0; from: 0; to: 50; value: 20; editable: true; onValueModified: dialog.updatePreview() }
                    Label { text: "Пауза между полётами, мин"; color: Theme.muted; font.pixelSize: 11; visible: modeBox.currentIndex === 1 }
                    SpinBox { id: gapBox; visible: modeBox.currentIndex === 1; from: 1; to: 240; value: 5; editable: true; onValueModified: dialog.updatePreview() }
                    Label { text: "Не меньше снимков в части"; color: Theme.muted; font.pixelSize: 11 }
                    SpinBox { id: minBox; from: 2; to: 1000; value: 3; editable: true; onValueModified: dialog.updatePreview() }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    color: dialog.preview.ok ? Theme.text : Theme.warn
                    text: dialog.preview.ok
                          ? "Частей: " + dialog.preview.parts.length + " — " + dialog.preview.parts.map(function(p) { return p.photos }).join(", ") + " фото"
                          : (dialog.preview.error || "")
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: (dialog.preview.notes || []).length > 0
                    text: (dialog.preview.notes || []).join(" · ")
                    color: Theme.muted; font.pixelSize: 10
                }
                Item { Layout.fillHeight: true }
                FlatButton {
                    objectName: "splitButton"
                    text: "Разделить"; emphasis: true
                    enabled: dialog.preview.ok === true
                    onClicked: {
                        var r = chunks.split(chunks.active, dialog.splitOptions())
                        if (!r.ok) { dialog.host.showError(r.error); return }
                        dialog.host.flash("Создано блоков: " + r.created.length + ". Обработайте каждый (Обработка → Пакетная обработка — по очереди), затем объедините результаты.")
                        dialog.tab = 0
                    }
                }
            }

            // ---- merge
            ColumnLayout {
                spacing: 8
                Label { text: "Блоки для объединения"; color: Theme.text; font.pixelSize: 12 }
                ListView {
                    id: mergeList
                    Layout.fillWidth: true
                    Layout.preferredHeight: 150
                    clip: true
                    model: chunks.chunks
                    delegate: CheckBox {
                        required property var modelData
                        width: mergeList.width
                        text: modelData.name + " — " + modelData.photos + " фото, готовых заданий " + modelData.completed
                        font.pixelSize: 11
                        checked: dialog.selected.indexOf(modelData.id) >= 0
                        onToggled: dialog.toggle(modelData.id, checked)
                    }
                }
                RowLayout {
                    spacing: 8
                    FlatButton {
                        text: "Снимки — в новый блок"
                        enabled: dialog.selected.length >= 2
                        onClicked: {
                            var id = chunks.mergePhotos(dialog.selected, "")
                            if (id.length > 0) { dialog.host.flash("Создан блок «" + chunks.activeName + "»: " + chunks.activePhotoCount + " фото"); dialog.tab = 0 }
                        }
                        ToolTip.visible: hovered; ToolTip.delay: 500
                        ToolTip.text: "Один блок со снимками всех выбранных (каждый снимок один раз) — для совместной обработки"
                    }
                }
                RowLayout {
                    spacing: 8
                    Label { text: "Сшить результаты"; color: Theme.muted; font.pixelSize: 11 }
                    ComboBox {
                        id: kindBox
                        implicitHeight: 28; Layout.preferredWidth: 170; font.pixelSize: 11
                        textRole: "title"; valueRole: "kind"
                        model: [{ kind: "orthophoto", title: "Ортофотопланы" }, { kind: "dsm", title: "DSM" }, { kind: "dtm", title: "DTM" }]
                    }
                    FlatButton {
                        objectName: "mergeResultsButton"
                        text: "Сшить"; emphasis: true
                        enabled: dialog.selected.length >= 2 && !chunks.merging
                        onClicked: {
                            var e = chunks.mergeResults(dialog.selected, kindBox.currentValue)
                            if (e.length > 0) dialog.host.showError(e)
                        }
                        ToolTip.visible: hovered; ToolTip.delay: 500
                        ToolTip.text: "Последние результаты блоков — в один растр (mosaics/): швы посередине перекрытий, мягкий переход; растр добавляется на карту"
                    }
                    FlatButton { text: "Отмена"; compact: true; visible: chunks.merging; onClicked: chunks.cancelMerge() }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: chunks.mergeStatus.length > 0
                    text: chunks.mergeStatus
                    color: Theme.muted; font.pixelSize: 11
                }
                Item { Layout.fillHeight: true }
            }
        }
    }

    Dialog {
        id: removeDialog
        property string chunkId
        property string chunkName
        title: "Удалить блок"
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label {
            width: 420
            wrapMode: Text.Wrap
            text: "Блок «" + removeDialog.chunkName + "» будет удалён. Снимки остаются в проекте (снимки, которых нет в других блоках, перейдут в первый блок); "
                  + "задания и их результаты остаются в списке заданий."
            color: Theme.text; font.pixelSize: 12
        }
        onAccepted: {
            var e = chunks.remove(chunkId)
            if (e.length > 0) dialog.host.showError(e)
        }
    }
}
