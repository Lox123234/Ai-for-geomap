import QtQuick
import QtQuick.Controls

// The flat text button of the GeoMap AI dark theme.
Button {
    id: flat
    property bool emphasis: false
    property bool caution: false
    property bool compact: false
    implicitHeight: compact ? 28 : 34
    implicitWidth: label.implicitWidth + (compact ? 20 : 28)
    padding: 0
    font.pixelSize: compact ? 12 : 13
    font.weight: Font.DemiBold
    hoverEnabled: true
    contentItem: Text {
        id: label
        text: flat.text
        font: flat.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: !flat.enabled ? Theme.faint : flat.emphasis ? "#FFFFFF"
            : flat.caution ? "#FFB8B8" : Theme.text
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: 5
        color: !flat.enabled ? "#232933" : flat.emphasis
            ? (flat.down ? "#2C66B5" : flat.hovered ? Theme.accentHover : Theme.accent)
            : flat.caution ? (flat.hovered ? "#4A2A33" : "#33262D")
            : flat.down ? Theme.pressed : flat.hovered ? Theme.hover : "#2A303B"
        border.color: flat.emphasis ? "transparent" : Theme.line
    }
}
