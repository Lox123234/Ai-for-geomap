import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Local AI models: their cards (task, licence, origin, files with SHA-256, limitations), the
// runtime, and the folder for the user's own models. Nothing is downloaded from here.
Dialog {
    id: dialog
    objectName: "aiModelsDialog"
    title: "Модели ИИ"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(760, Overlay.overlay ? Overlay.overlay.width - 40 : 760)
    height: Math.min(640, Overlay.overlay ? Overlay.overlay.height - 40 : 640)
    padding: 16
    standardButtons: Dialog.Close

    readonly property var taskTitles: ({ "interactive-segmentation": "выделение объекта по щелчку", "inpainting": "удаление объектов, заполнение пропусков" })

    contentItem: ColumnLayout {
        spacing: 10
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: "Модели работают на этом компьютере (" + aiModels.runtimeVersion + "), снимки никуда не отправляются. "
                  + "Модель используется, только если её карточка полная (лицензия, источник) и файлы совпадают с SHA-256."
            color: Theme.muted; font.pixelSize: 11
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: aiModels.models
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width - 12
                height: card.implicitHeight + 16
                radius: 6
                color: Theme.field
                border.color: modelData.valid && modelData.supported ? Theme.line : Theme.bad
                ColumnLayout {
                    id: card
                    anchors.fill: parent; anchors.margins: 8
                    spacing: 3
                    RowLayout {
                        Label { text: modelData.name || modelData.id; color: Theme.text; font.pixelSize: 13; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
                        Label {
                            text: !modelData.valid ? "не используется" : !modelData.supported ? "задача не поддерживается" : modelData.chosen ? "используется" : "доступна"
                            color: !modelData.valid || !modelData.supported ? Theme.bad : Theme.good; font.pixelSize: 11
                        }
                    }
                    Label { visible: modelData.error.length > 0; text: modelData.error; color: Theme.bad; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Repeater {
                        model: [["Задача", dialog.taskTitles[modelData.task] || modelData.task], ["Лицензия", modelData.license],
                                ["Авторы", modelData.authors], ["Источник", modelData.source], ["Веса", modelData.weights],
                                ["Преобразование", modelData.conversion], ["Вход", modelData.input], ["Выход", modelData.output],
                                ["Ограничения", modelData.limitations], ["Папка", modelData.directory]]
                        delegate: RowLayout {
                            required property var modelData
                            visible: modelData[1] !== undefined && String(modelData[1]).length > 0
                            Layout.fillWidth: true
                            Label { text: modelData[0]; color: Theme.faint; font.pixelSize: 10; Layout.preferredWidth: 100; Layout.alignment: Qt.AlignTop }
                            Label { text: modelData[1]; color: Theme.muted; font.pixelSize: 10; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        }
                    }
                    Repeater {
                        model: modelData.files
                        delegate: Label {
                            required property var modelData
                            text: modelData.role + ": " + modelData.name + "  SHA-256 " + modelData.sha256.substring(0, 16) + "…"
                            color: Theme.faint; font.pixelSize: 10; font.family: "Consolas"
                        }
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: aiModels.models.length === 0
                text: "Модели не найдены"
                color: Theme.faint
            }
        }
        RowLayout {
            spacing: 8
            FlatButton { text: "Папка своих моделей"; compact: true; onClicked: aiModels.openModelsFolder() }
            FlatButton { text: "Обновить список"; compact: true; onClicked: aiModels.refresh() }
            Item { Layout.fillWidth: true }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: "Своя модель: папка в «" + aiModels.userModelsFolder + "» с файлом model.json (как у MobileSAM: название, задача, лицензия, источник, "
                  + "файлы с ролями и SHA-256) и файлами ONNX. Поддерживаются задачи «interactive-segmentation» (кодировщик и декодер SAM) "
                  + "и «inpainting» (модель вида LaMa: изображение и маска 512 × 512)."
            color: Theme.faint; font.pixelSize: 10
        }
    }
}
