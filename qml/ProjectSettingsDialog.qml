import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Project settings: name, description, where it is, map CRS, processing options.
Dialog {
    id: dialog
    objectName: "projectSettingsDialog"
    title: "Настройки проекта"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 560
    padding: 16
    standardButtons: Dialog.Close
    property var host
    property string message: ""
    onOpened: { nameField.text = projects.projectName; descriptionField.text = projects.description; message = "" }

    contentItem: GridLayout {
        columns: 2
        columnSpacing: 12
        rowSpacing: 8
        Label { text: "Название"; color: Theme.muted; font.pixelSize: 11 }
        TextField {
            id: nameField
            objectName: "projectNameField"
            Layout.fillWidth: true; selectByMouse: true
            onEditingFinished: { var e = projects.renameProject(text); dialog.message = e.length > 0 ? e : "" }
        }
        Label { text: "Описание"; color: Theme.muted; font.pixelSize: 11; Layout.alignment: Qt.AlignTop }
        TextArea {
            id: descriptionField
            Layout.fillWidth: true; Layout.preferredHeight: 70; wrapMode: TextArea.Wrap; selectByMouse: true
            placeholderText: "Заказчик, объект, дата съёмки, примечания"
            onEditingFinished: projects.description = text
            background: Rectangle { color: Theme.field; radius: 4; border.color: Theme.line }
        }
        Label { text: "Папка"; color: Theme.muted; font.pixelSize: 11 }
        RowLayout {
            Layout.fillWidth: true
            Label { Layout.fillWidth: true; text: projects.projectPath; color: Theme.text; font.pixelSize: 11; elide: Text.ElideMiddle }
            FlatButton { text: "Открыть"; compact: true; onClicked: projects.revealProject() }
        }
        Label { text: "Создан"; color: Theme.muted; font.pixelSize: 11 }
        Label { text: projects.createdAt.length > 0 ? new Date(projects.createdAt).toLocaleString(Qt.locale("ru_RU"), Locale.ShortFormat) : "—"; color: Theme.text; font.pixelSize: 11 }
        Label { text: "Снимки"; color: Theme.muted; font.pixelSize: 11 }
        Label { text: projects.photoCount + " (с GPS: " + projects.gpsCount + ")"; color: Theme.text; font.pixelSize: 11 }
        Label { text: "CRS карты"; color: Theme.muted; font.pixelSize: 11 }
        RowLayout {
            Layout.fillWidth: true
            Label { Layout.fillWidth: true; text: gis.crsDefined ? gis.crsLabel : "не задана"; color: Theme.text; font.pixelSize: 11; elide: Text.ElideRight }
            FlatButton { text: "Изменить…"; compact: true; onClicked: { dialog.close(); dialog.host.runCommand("project.crs") } }
        }
        Label { text: "Обработка"; color: Theme.muted; font.pixelSize: 11 }
        CheckBox {
            checked: processing.autoTrueOrthophoto
            onToggled: processing.autoTrueOrthophoto = checked
            text: "Строить истинный ортофотоплан после обработки"
            font.pixelSize: 11
        }
        Item { width: 1 }
        RowLayout {
            spacing: 8
            FlatButton { text: "Проверить целостность…"; compact: true; onClicked: { dialog.close(); dialog.host.runCommand("project.check") } }
            FlatButton { text: "Сохранить как…"; compact: true; onClicked: { dialog.close(); dialog.host.runCommand("file.saveAs") } }
        }
        Label {
            Layout.columnSpan: 2; Layout.fillWidth: true; visible: dialog.message.length > 0; wrapMode: Text.Wrap
            text: dialog.message; color: Theme.bad; font.pixelSize: 11
        }
    }
}
