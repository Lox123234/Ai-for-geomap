import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoMapAI.Georef

// The image pane of the georeferencing (Инструменты → Привязка изображения по точкам): the image
// with the points "from" of the links; the map is beside it and the link table in the right panel
// (GeorefPanel.qml).
Item {
    id: pane
    property var host
    property alias view: imageView

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: bar.implicitHeight + 12
            color: Theme.toolbar
            Flow {
                id: bar
                anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 10; anchors.rightMargin: 8
                spacing: 6
                Label {
                    text: "СНИМОК · " + georef.imageName
                    width: Math.min(implicitWidth, Math.max(120, bar.width - 300))
                    elide: Text.ElideMiddle
                    color: Theme.muted; font.pixelSize: 11; font.weight: Font.DemiBold
                    height: 28; verticalAlignment: Text.AlignVCenter
                }
                FlatButton { compact: true; text: "Показать всё"; onClicked: imageView.fitAll() }
                FlatButton { compact: true; text: "+"; implicitWidth: 30; onClicked: imageView.zoomBy(1.5)
                             ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Крупнее (колесо мыши)" }
                FlatButton { compact: true; text: "−"; implicitWidth: 30; onClicked: imageView.zoomBy(1 / 1.5)
                             ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Мельче (колесо мыши)" }
                FlatButton {
                    compact: true; checkable: true; checked: imageView.loupe; emphasis: checked
                    text: "Лупа"
                    onToggled: imageView.loupe = checked
                    ToolTip.visible: hovered; ToolTip.delay: 500
                    ToolTip.text: "Увеличенный участок под курсором — чтобы поставить точку точно в пиксель"
                }
            }
        }
        GeorefImageView {
            id: imageView
            objectName: "georefImage"
            Layout.fillWidth: true
            Layout.fillHeight: true
            service: georef

            Label {
                anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 8
                visible: imageView.cursorText.length > 0
                text: imageView.cursorText
                color: Theme.text; font.pixelSize: 11
                background: Rectangle { color: "#C0111B26"; radius: 3 }
                padding: 3
            }
            // What to do next.
            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                anchors.margins: 10
                visible: georef.status.length > 0
                color: georef.pending ? "#E0352B10" : "#E0111B26"
                border.color: georef.pending ? "#F5C542" : Theme.line
                radius: 6
                implicitHeight: hint.implicitHeight + 14
                Label {
                    id: hint
                    anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 10; anchors.rightMargin: 10
                    text: georef.status
                    wrapMode: Text.Wrap
                    color: georef.pending ? "#FFE9A8" : Theme.text
                    font.pixelSize: 12
                }
            }
            BusyIndicator { anchors.centerIn: parent; running: georef.loading; visible: georef.loading }
        }
    }
}
