import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// "Проект" panel: the project tree (ProjectTreeModel through its search filter). A double click
// or Enter opens the item in a suitable view (photos, map, processing, external program); the
// context menu offers the same actions. Unavailable sections are greyed with their status.
Rectangle {
    id: panel
    property var host
    property string selectedKey: ""
    color: Theme.panel

    function keyOf(kind, payload) { return kind + ":" + (payload || "") }
    function itemOf(d) {
        return { kind: d.kind, title: d.title, payload: d.payload || "", jobId: d.jobId || "", file: d.file || "",
                 available: d.available, status: d.status }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.panelHeader
            color: Theme.toolbar
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10; anchors.rightMargin: 8
                SectionTitle { text: "ПРОЕКТ" }
                Item { Layout.fillWidth: true }
                Label {
                    text: projects.hasProject ? "АКТИВЕН" : "НЕ ОТКРЫТ"
                    color: projects.hasProject ? Theme.good : Theme.faint; font.pixelSize: 9; font.bold: true
                }
            }
        }
        TextField {
            id: search
            Layout.fillWidth: true
            Layout.margins: 6
            implicitHeight: 26
            enabled: projects.hasProject
            placeholderText: "Поиск в проекте"
            placeholderTextColor: Theme.faint
            color: Theme.text
            font.pixelSize: 12
            leftPadding: 8
            background: Rectangle { color: Theme.field; radius: 4; border.color: search.activeFocus ? Theme.accent : Theme.line }
            onTextChanged: {
                projectTreeView.filterText = text
                Qt.callLater(tree.expandRecursively)
            }
        }
        TreeView {
            id: tree
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: projectTreeView
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            Component.onCompleted: expandRecursively()
            Connections {
                target: projectTreeView
                function onModelReset() { Qt.callLater(tree.expandRecursively) }
            }
            delegate: Item {
                id: row
                required property TreeView treeView
                required property bool isTreeNode
                required property bool expanded
                required property bool hasChildren
                required property int depth
                required property int row
                required property string title
                required property string kind
                required property var status
                required property string iconName
                required property bool available
                required property var payload
                required property var jobId
                required property var file
                readonly property bool selected: panel.selectedKey === panel.keyOf(kind, payload)
                implicitWidth: tree.width
                implicitHeight: 26
                Accessible.role: Accessible.TreeItem
                Accessible.name: title + (status ? ", " + status : "")
                Accessible.onPressAction: panel.host.openTreeItem(panel.itemOf(row))

                Rectangle {
                    anchors.fill: parent
                    color: row.selected ? Theme.accentSoft : rowHover.hovered ? Theme.hover : "transparent"
                }
                HoverHandler { id: rowHover }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6 + row.depth * 14
                    anchors.rightMargin: 8
                    spacing: 5
                    Item {
                        implicitWidth: 14; implicitHeight: 14
                        Image {
                            anchors.fill: parent
                            visible: row.hasChildren
                            source: Theme.icon(row.expanded ? "chevron-down" : "chevron-right")
                            sourceSize: Qt.size(28, 28)
                            opacity: 0.7
                        }
                        TapHandler { enabled: row.hasChildren; onTapped: row.treeView.toggleExpanded(row.row) }
                    }
                    ToolButton {
                        implicitWidth: 18; implicitHeight: 18
                        padding: 0
                        enabled: false
                        icon.source: Theme.icon(row.iconName)
                        icon.width: 16; icon.height: 16
                        icon.color: row.available ? "#9FC3F0" : Theme.faint
                        background: null
                    }
                    Label {
                        text: row.title
                        color: row.available ? Theme.text : Theme.faint
                        font.pixelSize: 12
                        font.bold: row.depth === 0
                        elide: Text.ElideRight
                        Layout.maximumWidth: row.width * 0.62
                    }
                    Label {
                        Layout.fillWidth: true
                        text: row.status || ""
                        color: Theme.faint
                        font.pixelSize: 10
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignRight
                    }
                }
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onTapped: {
                        panel.selectedKey = panel.keyOf(row.kind, row.payload)
                        panel.host.selectTreeItem(panel.itemOf(row))
                    }
                    onDoubleTapped: panel.host.openTreeItem(panel.itemOf(row))
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        panel.selectedKey = panel.keyOf(row.kind, row.payload)
                        contextMenu.item = panel.itemOf(row)
                        contextMenu.popup()
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            Layout.margins: 10
            visible: !projects.hasProject
            text: "Создайте или откройте проект: меню «Файл» или Ctrl+N / Ctrl+O."
            wrapMode: Text.Wrap
            color: Theme.faint; font.pixelSize: 11
        }
    }

    // Context menu: the actions of panel.host.treeActions(item) — the same functions as the double click.
    Menu {
        id: contextMenu
        property var item: ({})
        readonly property var actions: panel.host ? panel.host.treeActions(item) : []
        Instantiator {
            model: contextMenu.actions
            delegate: MenuItem {
                required property var modelData
                text: modelData.title
                enabled: modelData.enabled !== false
                implicitHeight: 26
                onTriggered: panel.host.runTreeAction(modelData.id, contextMenu.item)
            }
            onObjectAdded: (index, object) => contextMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => contextMenu.removeItem(object)
        }
    }
}
