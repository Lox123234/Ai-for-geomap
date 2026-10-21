import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Photos of the active chunk under the project tree: thumbnails from the cache, GPS mark, selection.
Rectangle {
    id: panel
    property var host
    color: Theme.panel

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.panelHeader
            color: Theme.toolbar
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10; anchors.rightMargin: 4
                spacing: 2
                SectionTitle { text: "ФОТОГРАФИИ" }
                Label { text: chunks.activePhotoCount.toString(); color: Theme.faint; font.pixelSize: 10; Layout.leftMargin: 4 }
                // The active chunk, when the project has several.
                ComboBox {
                    objectName: "chunkBox"
                    visible: chunks.count > 1
                    Layout.preferredWidth: 130; implicitHeight: 24
                    font.pixelSize: 11
                    textRole: "name"; valueRole: "id"
                    model: chunks.chunks
                    currentIndex: {
                        for (var i = 0; i < chunks.chunks.length; ++i) if (chunks.chunks[i].active) return i
                        return -1
                    }
                    onActivated: chunks.setActive(currentValue)
                    ToolTip.visible: hovered; ToolTip.delay: 500
                    ToolTip.text: "Активный блок: его снимки показываются и обрабатываются"
                }
                Item { Layout.fillWidth: true }
                ToolIconButton { commandId: "import.photos"; host: panel.host; implicitWidth: 26; implicitHeight: 26 }
                ToolIconButton { commandId: "import.folder"; host: panel.host; implicitWidth: 26; implicitHeight: 26 }
            }
        }
        ListView {
            id: photoList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            topMargin: 4
            model: chunks.photoModel   // the photos of the active chunk
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                id: photoItem
                required property int photoRow   // the project's row (selection)
                required property string photoId
                required property string photoName
                required property url photoThumb
                required property bool photoHasGps
                required property int photoWidth
                required property int photoHeight
                required property bool photoEnabled   // goes into the chunk's processing
                // Re-evaluated whenever photoQuality changes (its notify signal).
                readonly property real quality: photoQuality.measured >= 0 ? photoQuality.qualityOf(photoId) : -1
                width: photoList.width - 10
                x: 5
                height: 50
                radius: 4
                color: projects.selectedIndex === photoItem.photoRow ? Theme.accentSoft : (photoMouse.containsMouse ? Theme.hover : "transparent")
                Accessible.role: Accessible.ListItem
                Accessible.name: photoName
                Accessible.onPressAction: { projects.selectPhoto(photoItem.photoRow); panel.host.runCommand("view.photo") }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 8
                    opacity: photoItem.photoEnabled ? 1 : 0.45
                    Rectangle {
                        implicitWidth: 60; implicitHeight: 42; radius: 3; color: Theme.field; clip: true
                        Image {
                            anchors.fill: parent
                            source: photoItem.photoThumb
                            sourceSize.width: 120
                            sourceSize.height: 84
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            cache: true
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            text: photoItem.photoName
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            color: Theme.text; font.pixelSize: 11; font.bold: true
                        }
                        Label {
                            text: photoItem.photoWidth + " × " + photoItem.photoHeight + (photoItem.photoHasGps ? "   ● GPS" : "   ○ без GPS")
                            color: photoItem.photoHasGps ? Theme.good : Theme.faint; font.pixelSize: 10
                        }
                    }
                    Label {
                        visible: !photoItem.photoEnabled || photoItem.quality >= 0
                        text: photoItem.photoEnabled ? photoItem.quality.toLocaleString(Qt.locale("ru_RU"), 'f', 2) : "выкл."
                        color: !photoItem.photoEnabled ? Theme.faint
                             : photoItem.quality < photoQuality.threshold ? Theme.bad
                             : photoItem.quality < photoQuality.threshold + 0.15 ? Theme.warn : Theme.muted
                        font.pixelSize: 10; font.weight: Font.DemiBold
                        Layout.rightMargin: 2
                        ToolTip.visible: qualityHover.hovered; ToolTip.delay: 400
                        ToolTip.text: photoItem.photoEnabled ? "Качество (резкость)" : "Отключён: в обработку не идёт"
                        HoverHandler { id: qualityHover }
                    }
                }
                MouseArea {
                    id: photoMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (mouse) => {
                        projects.selectPhoto(photoItem.photoRow)
                        if (mouse.button === Qt.RightButton) {
                            photoMenu.photoId = photoItem.photoId
                            photoMenu.photoOn = photoItem.photoEnabled
                            photoMenu.popup()
                        }
                    }
                    onDoubleClicked: (mouse) => {
                        if (mouse.button !== Qt.LeftButton) return
                        projects.selectPhoto(photoItem.photoRow); panel.host.runCommand("view.photo")
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: projects.hasProject && projects.photoCount === 0
                text: "Снимков нет\nФайл → Импорт фотографий"
                horizontalAlignment: Text.AlignHCenter
                color: Theme.faint; font.pixelSize: 11
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            Label { text: "С GPS"; color: Theme.muted; font.pixelSize: 11 }
            Label { text: projects.gpsCount + " / " + projects.photoCount; color: Theme.good; font.pixelSize: 11; font.bold: true }
            Item { Layout.fillWidth: true }
            Label { visible: chunks.activeDisabledCount > 0; text: "Отключено"; color: Theme.muted; font.pixelSize: 11 }
            Label { visible: chunks.activeDisabledCount > 0; text: chunks.activeDisabledCount; color: Theme.warn; font.pixelSize: 11; font.bold: true }
        }
    }

    Menu {
        id: photoMenu
        property string photoId: ""
        property bool photoOn: true
        MenuItem {
            text: photoMenu.photoOn ? "Отключить снимок" : "Включить снимок"
            implicitHeight: 26
            onTriggered: {
                var e = chunks.setPhotosEnabled([photoMenu.photoId], !photoMenu.photoOn)
                if (e.length) panel.host.showError(e)
            }
        }
        MenuItem {
            text: commands.title("proc.quality")
            implicitHeight: 26
            enabled: panel.host && panel.host.commandEnabled("proc.quality")
            onTriggered: panel.host.runCommand("proc.quality")
        }
    }
}
