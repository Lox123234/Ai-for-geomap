import QtQuick
import QtQuick.Controls

// The flat button of the GeoMap AI dark theme.
Button {
    id: flat
    property bool emphasis: false
    property bool caution: false
    property bool compact: false
    implicitHeight: compact ? 32 : 40
    implicitWidth: label.implicitWidth + (compact ? 24 : 32)
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
        color: !flat.enabled ? "#637080" : flat.emphasis ? "#0c1c29"
            : flat.caution ? "#ffb5b5" : "#dce8f5"
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: 8
        color: !flat.enabled ? "#26303b" : flat.emphasis
            ? (flat.hovered ? "#88d2ff" : "#56b6f7")
            : flat.caution ? (flat.hovered ? "#4d2b36" : "#352630")
            : flat.hovered ? "#344255" : "#283341"
        border.color: flat.emphasis ? "transparent" : "#3b4a5d"
    }
}
