import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Обработка → Оценка качества снимков: the sharpness of the active chunk's photos, worst first;
// blurred ones switched off in the chunk (they stay in the project, the processing skips them).
Dialog {
    id: dialog
    objectName: "qualityDialog"
    title: "Качество снимков"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 640
    height: Math.min(600, Overlay.overlay ? Overlay.overlay.height - 40 : 600)
    padding: 16
    standardButtons: Dialog.Close
    property var host
    readonly property bool allMeasured: photoQuality.measured > 0 && photoQuality.measured === chunks.activePhotoCount

    function qualityColor(q) {
        if (q < photoQuality.threshold) return Theme.bad
        if (q < photoQuality.threshold + 0.15) return Theme.warn
        return Theme.good
    }
    function report(e) { if (e.length) dialog.host.showError(e) }

    Connections {
        target: photoQuality
        function onFinished(ok, message) {
            if (ok) dialog.host.flash(message)
            else dialog.host.showError(message)
        }
    }

    contentItem: ColumnLayout {
        spacing: 12
        RowLayout {
            spacing: 22
            ColumnLayout {
                spacing: 0
                Label { text: "Нечётких"; color: Theme.muted; font.pixelSize: 11 }
                Label {
                    objectName: "qualityBlurred"
                    text: photoQuality.measured > 0 ? photoQuality.blurred : "—"
                    color: photoQuality.blurred > 0 ? Theme.bad : Theme.good
                    font.pixelSize: 26; font.weight: Font.DemiBold
                }
            }
            ColumnLayout {
                spacing: 0
                Label { text: "Оценено"; color: Theme.muted; font.pixelSize: 11 }
                Label {
                    text: photoQuality.measured + " из " + chunks.activePhotoCount
                    color: Theme.text; font.pixelSize: 26; font.weight: Font.DemiBold
                }
            }
            ColumnLayout {
                spacing: 0
                visible: chunks.activeDisabledCount > 0
                Label { text: "Отключено"; color: Theme.muted; font.pixelSize: 11 }
                Label { text: chunks.activeDisabledCount; color: Theme.warn; font.pixelSize: 26; font.weight: Font.DemiBold }
            }
            Item { Layout.fillWidth: true }
            ColumnLayout {
                spacing: 2
                Label { text: "Порог"; color: Theme.muted; font.pixelSize: 11 }
                SpinBox {
                    id: thresholdBox
                    objectName: "qualityThreshold"
                    from: 5; to: 95; stepSize: 5
                    value: Math.round(photoQuality.threshold * 100)
                    editable: true
                    implicitWidth: 136; implicitHeight: 30; font.pixelSize: 12
                    textFromValue: function(v) { return (v / 100).toLocaleString(Qt.locale("ru_RU"), 'f', 2) }
                    valueFromText: function(t) { return Math.round(Number.fromLocaleString(Qt.locale("ru_RU"), t) * 100) }
                    onValueModified: photoQuality.threshold = value / 100
                    ToolTip.visible: hovered; ToolTip.delay: 500
                    ToolTip.text: "Ниже порога — снимок нечёткий. Metashape советует 0,5"
                }
            }
            FlatButton {
                objectName: "qualityEstimate"
                Layout.alignment: Qt.AlignBottom
                text: dialog.allMeasured ? "Оценить заново" : "Оценить"
                emphasis: !dialog.allMeasured
                enabled: !photoQuality.busy && chunks.activePhotoCount > 0
                onClicked: dialog.report(photoQuality.estimate(dialog.allMeasured))
                ToolTip.visible: hovered; ToolTip.delay: 500
                ToolTip.text: "Резкость самой чёткой части снимка, относительно лучших снимков проекта"
            }
        }

        ListView {
            id: list
            objectName: "qualityList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: photoQuality.photos
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: item
                required property var modelData
                width: ListView.view.width - 12
                height: 52
                radius: 4
                color: projects.selectedIndex === modelData.row ? Theme.accentSoft : (rowMouse.containsMouse ? Theme.hover : "transparent")
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: projects.selectPhoto(item.modelData.row)
                    onDoubleClicked: { projects.selectPhoto(item.modelData.row); dialog.close(); dialog.host.runCommand("view.photo") }
                }
                RowLayout {
                    anchors.left: parent.left; anchors.right: inUse.left
                    anchors.top: parent.top; anchors.bottom: parent.bottom
                    anchors.margins: 4
                    spacing: 10
                    opacity: item.modelData.enabled ? 1 : 0.45
                    Rectangle {
                        implicitWidth: 62; implicitHeight: 44; radius: 3; color: Theme.field; clip: true
                        Image {
                            anchors.fill: parent
                            source: item.modelData.thumb
                            sourceSize.width: 124; sourceSize.height: 88
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }
                    }
                    Label {
                        text: item.modelData.name
                        Layout.preferredWidth: 170
                        elide: Text.ElideMiddle
                        color: Theme.text; font.pixelSize: 12
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 6; radius: 3
                        color: Theme.field
                        Rectangle {
                            visible: item.modelData.quality >= 0
                            width: parent.width * Math.max(0.02, item.modelData.quality)
                            height: parent.height; radius: 3
                            color: dialog.qualityColor(item.modelData.quality)
                        }
                    }
                    Label {
                        text: item.modelData.text
                        Layout.preferredWidth: 40
                        horizontalAlignment: Text.AlignRight
                        color: item.modelData.quality >= 0 ? dialog.qualityColor(item.modelData.quality) : Theme.faint
                        font.pixelSize: 14; font.weight: Font.DemiBold
                    }
                }
                CheckBox {
                    id: inUse
                    objectName: "qualityUse" + item.modelData.name
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    checked: item.modelData.enabled
                    enabled: !photoQuality.busy
                    onToggled: dialog.report(chunks.setPhotosEnabled([item.modelData.id], checked))
                    ToolTip.visible: hovered; ToolTip.delay: 500
                    ToolTip.text: checked ? "В обработке. Снять — снимок останется в проекте, но не пойдёт в обработку" : "Отключён: в обработку не идёт"
                }
            }
            Label {
                anchors.centerIn: parent
                visible: photoQuality.measured === 0 && !photoQuality.busy
                text: chunks.activePhotoCount > 0 ? "Снимки ещё не оценены" : "В блоке нет снимков"
                color: Theme.faint
            }
        }

        RowLayout {
            visible: photoQuality.busy
            spacing: 10
            ProgressBar {
                Layout.fillWidth: true
                from: 0; to: Math.max(1, photoQuality.total); value: photoQuality.done
            }
            Label { text: photoQuality.done + " / " + photoQuality.total; color: Theme.text; font.pixelSize: 12 }
            FlatButton { text: "Отменить"; compact: true; caution: true; onClicked: photoQuality.cancel() }
        }
        RowLayout {
            visible: !photoQuality.busy
            spacing: 8
            FlatButton {
                objectName: "qualityDisable"
                text: photoQuality.blurredEnabled > 0 ? "Отключить нечёткие (" + photoQuality.blurredEnabled + ")" : "Отключить нечёткие"
                caution: true; compact: true
                enabled: photoQuality.blurredEnabled > 0
                onClicked: dialog.report(photoQuality.disableBlurred())
                ToolTip.visible: hovered; ToolTip.delay: 500
                ToolTip.text: "Снимки останутся в проекте, но не пойдут в обработку блока"
            }
            FlatButton {
                text: "Включить все"; compact: true
                enabled: chunks.activeDisabledCount > 0
                onClicked: dialog.report(photoQuality.enableAll())
            }
            Item { Layout.fillWidth: true }
        }
    }
}
