import QtQuick
import QtQuick.Controls

// Menu item bound to a command of the registry. Ready commands show their shortcut on the
// right; planned ones are disabled and show the stage that brings them, engine-blocked ones
// say that the engine cannot do it. Check marks come from the window state (no internal toggle).
MenuItem {
    id: item
    required property string commandId
    property var host
    readonly property bool ready: commands.isReady(commandId)
    readonly property bool marked: ready && commands.isCheckable(commandId) && host ? host.commandChecked(commandId) : false
    readonly property string hint: {
        if (commands.revision < 0) return ""
        if (ready) return commands.shortcutText(commandId)
        var c = commands.command(commandId)
        return c.status === "engine" ? "нет в движке" : "этап " + c.stage
    }
    text: commands.title(commandId)
    enabled: ready && host !== undefined && host !== null && host.commandEnabled(commandId)
    implicitHeight: 26
    implicitWidth: 26 + titleText.implicitWidth + 40 + hintText.implicitWidth + 14
    onTriggered: host.runCommand(commandId)
    Accessible.name: text
    Accessible.description: ready ? commands.tooltip(commandId) : commands.unavailableReason(commandId)
    contentItem: Item {
        Text {
            anchors.left: parent.left; anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: "✓"; visible: item.marked
            color: Theme.text; font.pixelSize: 12
        }
        Text {
            id: titleText
            anchors.left: parent.left; anchors.leftMargin: 22
            anchors.verticalCenter: parent.verticalCenter
            text: item.text
            color: item.enabled ? (item.highlighted ? "#FFFFFF" : Theme.text) : Theme.faint
            font.pixelSize: 12
        }
        Text {
            id: hintText
            anchors.right: parent.right; anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: item.hint
            color: item.highlighted && item.enabled ? "#DCE8FF" : Theme.faint
            font.pixelSize: 11
        }
    }
    background: Rectangle {
        implicitWidth: 200
        color: item.highlighted && item.enabled ? Theme.accent : "transparent"
        radius: 3
    }
}
