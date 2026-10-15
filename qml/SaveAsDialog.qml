import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Save the project as a copy in another folder; the copy is opened, the original stays.
Dialog {
    id: dialog
    objectName: "saveAsDialog"
    title: "Сохранить проект как"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 560
    padding: 16
    standardButtons: Dialog.Cancel
    property var host
    property url folder
    property string error: ""
    readonly property real sizeMb: visible ? projects.projectSize() / 1048576 : 0
    onOpened: { nameField.text = projects.projectName + " — копия"; error = "" }

    contentItem: ColumnLayout {
        spacing: 8
        Label { text: "Папка, в которой появится копия"; color: Theme.muted; font.pixelSize: 11 }
        RowLayout {
            Layout.fillWidth: true
            Label { Layout.fillWidth: true; text: dialog.folder.toString().length > 0 ? gis.displayPath(dialog.folder) : "не выбрана"; color: Theme.text; elide: Text.ElideMiddle }
            FlatButton { text: "Выбрать…"; compact: true; onClicked: folderDialog.open() }
        }
        Label { text: "Название копии"; color: Theme.muted; font.pixelSize: 11 }
        TextField { id: nameField; Layout.fillWidth: true; selectByMouse: true }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: "Будет скопировано около " + dialog.sizeMb.toLocaleString(Qt.locale("ru_RU"), 'f', 1)
                  + " МБ: снимки, задания обработки с результатами, карта, анализ, измерения. Кэш миниатюр и пирамид не копируется."
            color: Theme.faint; font.pixelSize: 11
        }
        Label { Layout.fillWidth: true; visible: dialog.error.length > 0; text: dialog.error; wrapMode: Text.Wrap; color: Theme.bad; font.pixelSize: 11 }
        FlatButton {
            text: "Сохранить копию"; emphasis: true
            enabled: dialog.folder.toString().length > 0 && nameField.text.trim().length > 0 && !projects.busy
            onClicked: {
                dialog.error = projects.saveProjectAs(dialog.folder, nameField.text)
                if (dialog.error.length === 0) { dialog.close(); dialog.host.flash("Копирование проекта…") }
            }
        }
    }
    FolderDialog { id: folderDialog; title: "Куда сохранить копию проекта?"; onAccepted: dialog.folder = selectedFolder }
}
