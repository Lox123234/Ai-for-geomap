import QtQuick
import QtQuick.Controls

// The main toolbar: command groups of the registry separated by thin lines. Every button is a
// ToolIconButton bound to a command id; the active view or tool is highlighted. A group with
// "views" shows only in those views (the map's drawing tools only on the map).
Rectangle {
    id: bar
    property var host
    readonly property string view: !host ? "" : ["photo", "gps", "processing", "map", "3d"][host.activeView] || ""
    implicitHeight: Theme.toolbarHeight
    color: Theme.toolbar
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }

    Flickable {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        contentWidth: groups.width
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        Row {
            id: groups
            height: parent.height
            spacing: 0
            Repeater {
                model: commands.toolbar
                delegate: Row {
                    id: group
                    required property var modelData
                    required property int index
                    visible: !modelData.views || modelData.views.indexOf(bar.view) >= 0
                    height: groups.height
                    spacing: 2
                    Item {
                        visible: group.index > 0
                        width: 13; height: parent.height
                        Rectangle { anchors.centerIn: parent; width: 1; height: 24; color: Theme.line }
                    }
                    Repeater {
                        model: group.modelData.items
                        delegate: ToolIconButton {
                            required property string modelData
                            anchors.verticalCenter: parent.verticalCenter
                            commandId: modelData
                            host: bar.host
                        }
                    }
                }
            }
        }
    }
}
