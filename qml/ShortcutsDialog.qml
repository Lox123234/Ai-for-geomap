import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shortcuts of all available commands; a field captures a new key combination (Backspace
// clears it, Esc cancels). Conflicts are refused with the name of the other command. Also
// resets the panel layout.
Dialog {
    id: dialog
    property var host
    title: "Горячие клавиши и интерфейс"
    modal: true
    width: Math.min(720, parent ? parent.width - 80 : 720)
    height: Math.min(640, parent ? parent.height - 80 : 640)
    background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: 8 }
    property string message: ""
    property bool messageIsError: false

    contentItem: ColumnLayout {
        spacing: 8
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: "Щёлкните поле и нажмите сочетание. Backspace — убрать."
            color: Theme.muted; font.pixelSize: 11
        }
        TextField {
            id: filter
            Layout.fillWidth: true
            implicitHeight: 28
            placeholderText: "Поиск команды"
            placeholderTextColor: Theme.faint
            color: Theme.text
            background: Rectangle { color: Theme.field; radius: 4; border.color: filter.activeFocus ? Theme.accent : Theme.line }
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: {
                var all = commands.revision >= 0 ? commands.shortcutList() : []
                var f = filter.text.trim().toLowerCase()
                return f.length === 0 ? all : all.filter(function(c) { return c.title.toLowerCase().indexOf(f) >= 0 || c.shortcutText.toLowerCase().indexOf(f) >= 0 })
            }
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: RowLayout {
                id: row
                required property var modelData
                width: list.width - 14
                height: 30
                spacing: 8
                Label { text: row.modelData.title; color: Theme.text; font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideRight }
                Label {
                    visible: row.modelData.changed
                    text: "по умолчанию: " + (row.modelData.defaultShortcut || "—")
                    color: Theme.faint; font.pixelSize: 10
                }
                TextField {
                    id: keyField
                    Layout.preferredWidth: 170
                    implicitHeight: 26
                    readOnly: true
                    text: activeFocus ? "Нажмите сочетание…" : row.modelData.shortcutText
                    color: activeFocus ? Theme.warn : Theme.text
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    background: Rectangle { color: Theme.field; radius: 4; border.color: keyField.activeFocus ? Theme.accent : Theme.line }
                    Accessible.name: "Сочетание для команды " + row.modelData.title
                    Keys.onPressed: function(event) {
                        event.accepted = true
                        if (event.key === Qt.Key_Escape) { keyField.focus = false; return }
                        var text = (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete) && event.modifiers === Qt.NoModifier
                            ? "" : commands.keyText(event.key, event.modifiers)
                        if (text === "" && !(event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete)) return
                        var error = commands.setShortcut(row.modelData.id, text)
                        dialog.messageIsError = error.length > 0
                        dialog.message = error.length > 0 ? error : (text.length > 0 ? "«" + row.modelData.title + "»: " + text : "«" + row.modelData.title + "»: сочетание убрано")
                        keyField.focus = false
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            visible: dialog.message.length > 0
            text: dialog.message
            color: dialog.messageIsError ? Theme.bad : Theme.good; font.pixelSize: 12
        }
        RowLayout {
            Layout.fillWidth: true
            FlatButton { text: "Сбросить все клавиши"; compact: true; onClicked: { commands.resetShortcuts(); dialog.message = "Сочетания по умолчанию восстановлены."; dialog.messageIsError = false } }
            FlatButton { text: "Сбросить расположение панелей"; compact: true; onClicked: dialog.host.runCommand("view.resetLayout") }
            Item { Layout.fillWidth: true }
        }
    }
    standardButtons: Dialog.Close
    onOpened: { message = ""; filter.text = "" }
}
