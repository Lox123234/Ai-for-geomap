import QtQuick
import QtQuick.Controls

// 34×34 toolbar button bound to a command of the registry: icon, tooltip with the shortcut,
// enabled and checked state come from the window's command functions, so the button, the menu
// item and the shortcut always agree. Planned commands stay visible but disabled, with the
// reason in the tooltip.
ToolButton {
    id: button
    required property string commandId
    property var host                  // the window: commandEnabled / commandChecked / runCommand
    readonly property bool ready: commands.isReady(commandId)
    readonly property bool active: host ? host.commandChecked(commandId) : false
    implicitWidth: 34
    implicitHeight: 34
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.NoFocus
    display: AbstractButton.IconOnly
    text: commands.title(commandId)
    icon.source: Theme.icon(commands.icon(commandId))
    icon.width: 19
    icon.height: 19
    icon.color: active ? "#FFFFFF" : enabled ? Theme.text : Theme.faint
    enabled: ready && host !== undefined && host !== null && host.commandEnabled(commandId)
    // Disabled controls get no hover events; keep the tooltip for planned commands.
    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered
    ToolTip.delay: 500
    ToolTip.text: commands.revision >= 0 ? commands.tooltip(commandId) : ""
    Accessible.name: commands.title(commandId)
    Accessible.description: commands.tooltip(commandId)
    onClicked: host.runCommand(commandId)
    background: Rectangle {
        radius: 5
        color: button.active ? Theme.accent : !button.enabled ? "transparent"
             : button.down ? Theme.pressed : hover.hovered ? Theme.hover : "transparent"
    }
}
