import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Read-only integrity check of the open project.
Dialog {
    id: dialog
    objectName: "projectCheckDialog"
    title: "Проверка целостности проекта"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 620
    height: Math.min(520, Overlay.overlay ? Overlay.overlay.height - 40 : 520)
    padding: 16
    standardButtons: Dialog.Close
    property var checkResult: ({})
    onOpened: checkResult = projects.checkProject()

    contentItem: ColumnLayout {
        spacing: 8
        Label {
            text: dialog.checkResult.ok === true ? "Проблем не найдено" : "Найдены проблемы"
            color: dialog.checkResult.ok === true ? Theme.good : Theme.bad; font.pixelSize: 14; font.bold: true
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ColumnLayout {
                width: dialog.availableWidth - 16
                spacing: 3
                Repeater {
                    model: dialog.checkResult.problems || []
                    delegate: Label { required property string modelData; Layout.fillWidth: true; wrapMode: Text.Wrap; text: "✕ " + modelData; color: Theme.bad; font.pixelSize: 11 }
                }
                Repeater {
                    model: dialog.checkResult.warnings || []
                    delegate: Label { required property string modelData; Layout.fillWidth: true; wrapMode: Text.Wrap; text: "⚠ " + modelData; color: Theme.warn; font.pixelSize: 11 }
                }
                Repeater {
                    model: dialog.checkResult.checked || []
                    delegate: Label { required property string modelData; Layout.fillWidth: true; wrapMode: Text.Wrap; text: "✓ " + modelData; color: Theme.good; font.pixelSize: 11 }
                }
            }
        }
        RowLayout {
            FlatButton { text: "Проверить снова"; compact: true; onClicked: dialog.checkResult = projects.checkProject() }
            Item { Layout.fillWidth: true }
        }
    }
}
