pragma Singleton
import QtQuick

// Colours and sizes of the professional workspace (palette of the Professional Edition spec).
QtObject {
    readonly property color bg: "#171B22"            // main background, centre area
    readonly property color panel: "#20252E"         // side and bottom panels
    readonly property color toolbar: "#252B35"       // menu bar, toolbars, panel headers
    readonly property color field: "#161A21"         // inputs, lists
    readonly property color hover: "#2C3340"
    readonly property color pressed: "#333C4B"
    readonly property color accent: "#3578D4"        // active tool, selection
    readonly property color accentHover: "#4A8BE3"
    readonly property color accentSoft: "#24395A"    // selected rows
    readonly property color text: "#E3E8EF"
    readonly property color muted: "#96A2B3"
    readonly property color faint: "#647083"
    readonly property color line: "#343C48"           // separators
    readonly property color good: "#7FD1AE"
    readonly property color warn: "#F2C46D"
    readonly property color bad: "#F09A9A"

    readonly property int menuHeight: 30
    readonly property int toolbarHeight: 44
    readonly property int leftWidth: 280
    readonly property int rightWidth: 310
    readonly property int bottomHeight: 170
    readonly property int statusHeight: 26
    readonly property int panelHeader: 30

    function icon(name) {
        return name && name.length > 0 ? "qrc:/qt/qml/GeoMapAI/icons/" + name + ".svg" : ""
    }
}
