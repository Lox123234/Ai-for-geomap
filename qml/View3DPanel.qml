import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Right panel of the 3D view: layers of the scene (visibility, size, source, errors) and the
// display settings (point size, colours, lighting, point budget of new layers).
Item {
    id: panel
    property var host

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
                spacing: 4
                SectionTitle { text: "3D-СЦЕНА" }
                Label { text: scene3d.layerCount.toString(); color: Theme.faint; font.pixelSize: 10 }
                Item { Layout.fillWidth: true }
                FlatButton { text: "+ Облако…"; compact: true; implicitHeight: 24; onClicked: panel.host.runCommand("import.pointCloud") }
                FlatButton { text: "+ Модель…"; compact: true; implicitHeight: 24; onClicked: panel.host.runCommand("import.model") }
            }
        }
        Label {
            Layout.fillWidth: true
            Layout.margins: 10
            Layout.bottomMargin: 4
            wrapMode: Text.Wrap
            text: scene3d.status
            color: Theme.muted; font.pixelSize: 11
        }
        ListView {
            id: layerList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 80
            clip: true
            spacing: 2
            model: scene3d.layers
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                id: row
                required property var modelData
                width: layerList.width - 10
                x: 5
                height: rowColumn.implicitHeight + 10
                radius: 4
                color: rowHover.hovered ? Theme.hover : "transparent"
                HoverHandler { id: rowHover }
                ColumnLayout {
                    id: rowColumn
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.margins: 5
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        CheckBox {
                            checked: row.modelData.visible
                            enabled: !row.modelData.loading && row.modelData.error.length === 0
                            padding: 0
                            implicitWidth: 20; implicitHeight: 20
                            onToggled: scene3d.setLayerVisible(row.modelData.id, checked)
                            Accessible.name: "Показать " + row.modelData.name
                        }
                        // Icon only (tinted like the toolbar icons), not a button.
                        Button {
                            implicitWidth: 16; implicitHeight: 16; padding: 0
                            display: AbstractButton.IconOnly
                            focusPolicy: Qt.NoFocus
                            hoverEnabled: false
                            enabled: false
                            icon.source: Theme.icon(row.modelData.kind === "pointcloud" ? "cloud" : row.modelData.kind === "terrain" ? "dem" : "cube")
                            icon.width: 14; icon.height: 14; icon.color: Theme.muted
                            background: null
                        }
                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.name
                            elide: Text.ElideMiddle
                            color: Theme.text; font.pixelSize: 12; font.bold: true
                        }
                        ToolButton {
                            implicitWidth: 20; implicitHeight: 20; padding: 0
                            display: AbstractButton.IconOnly
                            icon.source: Theme.icon("close"); icon.width: 12; icon.height: 12; icon.color: Theme.muted
                            text: "Убрать из сцены"
                            onClicked: scene3d.removeLayer(row.modelData.id)
                            ToolTip.visible: hovered; ToolTip.text: text; ToolTip.delay: 500
                            background: Rectangle { radius: 3; color: parent.hovered ? Theme.hover : "transparent" }
                        }
                    }
                    Label {
                        Layout.fillWidth: true; Layout.leftMargin: 25
                        text: row.modelData.kindTitle
                        color: Theme.faint; font.pixelSize: 10
                    }
                    Label {
                        Layout.fillWidth: true; Layout.leftMargin: 25
                        wrapMode: Text.Wrap
                        text: (row.modelData.loading ? "⏳ " : row.modelData.error.length > 0 ? "⚠ " : "") + row.modelData.info
                        color: row.modelData.error.length > 0 ? Theme.bad : row.modelData.loading ? "#9fc3f0" : Theme.muted
                        font.pixelSize: 10
                    }
                    Label {
                        Layout.fillWidth: true; Layout.leftMargin: 25
                        text: row.modelData.source
                        elide: Text.ElideMiddle
                        color: Theme.faint; font.pixelSize: 9
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.panelHeader
            color: Theme.toolbar
            SectionTitle { anchors.verticalCenter: parent.verticalCenter; x: 10; text: "ОТОБРАЖЕНИЕ" }
        }
        GridLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            columns: 2
            columnSpacing: 8
            rowSpacing: 6
            Label { text: "Размер точек"; color: Theme.muted; font.pixelSize: 11 }
            RowLayout {
                Layout.fillWidth: true
                Slider {
                    id: sizeSlider
                    Layout.fillWidth: true
                    from: 1; to: 12; stepSize: 0.5
                    value: scene3d.pointSize
                    onMoved: scene3d.pointSize = value
                }
                Label { text: scene3d.pointSize.toFixed(1) + " px"; color: Theme.text; font.pixelSize: 11; Layout.preferredWidth: 42 }
            }
            Label { text: "Освещение"; color: Theme.muted; font.pixelSize: 11 }
            CheckBox {
                text: "солнце с северо-запада"
                checked: scene3d.lighting
                onToggled: scene3d.lighting = checked
                font.pixelSize: 11
                ToolTip.visible: hovered; ToolTip.delay: 500
                ToolTip.text: "Затенение моделей без текстур и рельефа (текстурированные модели уже содержат освещение снимков)"
            }
            Label { text: "Точек в слое"; color: Theme.muted; font.pixelSize: 11 }
            ComboBox {
                Layout.fillWidth: true
                implicitHeight: 26
                font.pixelSize: 11
                model: [{ text: "до 1 млн", value: 1000000 }, { text: "до 5 млн", value: 5000000 },
                        { text: "до 20 млн", value: 20000000 }, { text: "до 50 млн", value: 50000000 }]
                textRole: "text"; valueRole: "value"
                Component.onCompleted: currentIndex = Math.max(0, indexOfValue(scene3d.pointBudget))
                onActivated: scene3d.pointBudget = currentValue
                ToolTip.visible: hovered; ToolTip.delay: 500
                ToolTip.text: "Сколько точек облака загружать в память (каждая k-я); действует на новые слои"
            }
        }
    }
}
