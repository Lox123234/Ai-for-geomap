import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Photos of the project under the project tree: thumbnails from the cache, GPS mark, selection.
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
                Label { text: projects.photoCount.toString(); color: Theme.faint; font.pixelSize: 10; Layout.leftMargin: 4 }
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
            model: projects.photoModel
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                id: photoRow
                required property int index
                required property string photoName
                required property url photoThumb
                required property bool photoHasGps
                required property int photoWidth
                required property int photoHeight
                width: photoList.width - 10
                x: 5
                height: 50
                radius: 4
                color: projects.selectedIndex === index ? Theme.accentSoft : (photoMouse.containsMouse ? Theme.hover : "transparent")
                Accessible.role: Accessible.ListItem
                Accessible.name: photoName
                Accessible.onPressAction: { projects.selectPhoto(index); panel.host.runCommand("view.photo") }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 8
                    Rectangle {
                        implicitWidth: 60; implicitHeight: 42; radius: 3; color: Theme.field; clip: true
                        Image {
                            anchors.fill: parent
                            source: photoRow.photoThumb
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
                            text: photoRow.photoName
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            color: Theme.text; font.pixelSize: 11; font.bold: true
                        }
                        Label {
                            text: photoRow.photoWidth + " × " + photoRow.photoHeight + (photoRow.photoHasGps ? "   ● GPS" : "   ○ без GPS")
                            color: photoRow.photoHasGps ? Theme.good : Theme.faint; font.pixelSize: 10
                        }
                    }
                }
                MouseArea {
                    id: photoMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: projects.selectPhoto(photoRow.index)
                    onDoubleClicked: { projects.selectPhoto(photoRow.index); panel.host.runCommand("view.photo") }
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
            Item { Layout.fillWidth: true }
            Label { text: projects.gpsCount + " / " + projects.photoCount; color: Theme.good; font.pixelSize: 11; font.bold: true }
        }
    }
}
