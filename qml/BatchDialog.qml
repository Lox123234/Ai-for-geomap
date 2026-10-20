import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Batch processing: several profiles one after another for the photos of the project, and the
// results of every job copied into a folder as soon as they are ready (after the true orthophoto).
Dialog {
    id: dialog
    objectName: "batchDialog"
    title: "Пакетная обработка"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 520
    padding: 16
    standardButtons: Dialog.Close
    property var host
    property var chosen: []
    property url folder: ""
    function toggle(id, on) {
        var list = chosen.filter(function(p) { return p !== id })
        if (on) list.push(id)
        chosen = list
    }
    function run() {
        var ids = []
        // In the order of the list of profiles, not of the clicks.
        for (var i = 0; i < processing.profiles.length; ++i) {
            var p = processing.profiles[i]
            if (chosen.indexOf(p.id) < 0) continue
            var id = processing.startJob(p.id, "", {})
            if (id.length > 0) ids.push(id)
        }
        if (ids.length === 0) { dialog.host.showError("Ни одно задание не запущено — см. сообщение обработки."); return }
        dialog.host.startBatch(ids, exportBox.checked ? folder : "")
        dialog.host.flash("В очереди заданий: " + ids.length + (exportBox.checked ? " · результаты будут сохранены в папку" : ""))
        dialog.host.activeView = 2
        close()
    }

    contentItem: ColumnLayout {
        spacing: 8
        Label { text: "Профили — по очереди, для всех снимков проекта"; color: Theme.text; font.pixelSize: 12 }
        Repeater {
            model: processing.profiles
            delegate: CheckBox {
                required property var modelData
                text: modelData.title
                font.pixelSize: 12
                checked: dialog.chosen.indexOf(modelData.id) >= 0
                onToggled: dialog.toggle(modelData.id, checked)
            }
        }
        CheckBox {
            id: exportBox
            text: "Сохранять результаты каждого задания в папку"
            font.pixelSize: 12
        }
        RowLayout {
            visible: exportBox.checked
            spacing: 8
            Label {
                Layout.fillWidth: true; elide: Text.ElideMiddle
                text: dialog.folder.toString().length > 0 ? gis.displayPath(dialog.folder) : "папка не выбрана"
                color: dialog.folder.toString().length > 0 ? Theme.text : Theme.warn; font.pixelSize: 11
            }
            FlatButton { text: "Выбрать…"; compact: true; onClicked: folderDialog.open() }
        }
        FlatButton {
            text: "Запустить"; emphasis: true
            enabled: dialog.chosen.length > 0 && processing.engineAvailable && projects.photoCount > 0
                     && (!exportBox.checked || dialog.folder.toString().length > 0)
            onClicked: dialog.run()
        }
    }
    AppFolderDialog {
        id: folderDialog
        kind: "export"
        title: "Папка для результатов"
        onAccepted: dialog.folder = selectedFolder
    }
}
