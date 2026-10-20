import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// "Задания и журнал": the processing jobs of the project (real engine stage and state) and the
// operations journal (AppLog: what the services reported). Collapses to its header.
Rectangle {
    id: panel
    property var host
    property bool collapsed: false
    color: Theme.panel

    function levelColor(level) {
        return level === 1 ? Theme.good : level === 2 ? Theme.warn : level === 3 ? Theme.bad : Theme.muted
    }
    function stateColor(state) {
        switch (state) {
        case "completed": return Theme.good
        case "failed": return Theme.bad
        case "running": case "preparing": return "#7FB3F0"
        case "cancelling": case "cancelled": case "interrupted": return Theme.warn
        default: return Theme.muted
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.panelHeader
            color: Theme.toolbar
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10; anchors.rightMargin: 4
                spacing: 6
                SectionTitle { text: "ЗАДАНИЯ И ЖУРНАЛ" }
                Label {
                    text: processing.active ? "· идёт обработка" : ""
                    color: "#7FB3F0"; font.pixelSize: 10
                }
                Item { Layout.fillWidth: true }
                Label {
                    visible: panel.collapsed && appLog.count > 0
                    text: appLog.lastText
                    color: Theme.muted; font.pixelSize: 11
                    elide: Text.ElideRight
                    Layout.maximumWidth: panel.width * 0.6
                }
                ToolButton {
                    implicitWidth: 26; implicitHeight: 26; padding: 0
                    icon.source: Theme.icon(panel.collapsed ? "chevron-up" : "chevron-down")
                    icon.width: 16; icon.height: 16; icon.color: Theme.muted
                    display: AbstractButton.IconOnly
                    text: panel.collapsed ? "Развернуть панель" : "Свернуть панель"
                    ToolTip.visible: hovered; ToolTip.text: text; ToolTip.delay: 500
                    background: Rectangle { radius: 4; color: parent.hovered ? Theme.hover : "transparent" }
                    onClicked: panel.collapsed = !panel.collapsed
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !panel.collapsed
            spacing: 0
            // Jobs
            ListView {
                id: jobList
                Layout.preferredWidth: Math.min(460, panel.width * 0.4)
                Layout.fillHeight: true
                clip: true
                model: processing.jobModel
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                delegate: Rectangle {
                    id: jobRow
                    required property string jobId
                    required property string jobState
                    required property string stateText
                    required property string profileTitle
                    required property string chunkId
                    required property int stageIndex
                    required property int stageCount
                    required property string stageTitle
                    width: jobList.width
                    height: 24
                    color: jobMouse.containsMouse ? Theme.hover : "transparent"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10; anchors.rightMargin: 8
                        spacing: 8
                        Rectangle { implicitWidth: 7; implicitHeight: 7; radius: 4; color: panel.stateColor(jobRow.jobState) }
                        Label { text: jobRow.profileTitle + (chunks.count > 1 ? " · " + chunks.nameOf(jobRow.chunkId) : ""); color: Theme.text; font.pixelSize: 11 }
                        Label { text: jobRow.jobId; color: Theme.faint; font.pixelSize: 10 }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: jobRow.jobState === "running" && jobRow.stageIndex > 0
                                ? "этап " + jobRow.stageIndex + "/" + jobRow.stageCount + " · " + jobRow.stageTitle : jobRow.stateText
                            color: panel.stateColor(jobRow.jobState); font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }
                    MouseArea {
                        id: jobMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onDoubleClicked: panel.host.openJob(jobRow.jobId)
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: jobList.count === 0
                    text: projects.hasProject ? "Заданий обработки нет" : "Проект не открыт"
                    color: Theme.faint; font.pixelSize: 11
                }
            }
            Rectangle { Layout.fillHeight: true; implicitWidth: 1; color: Theme.line }
            // Journal
            ListView {
                id: logList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: appLog
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                onCountChanged: Qt.callLater(positionViewAtEnd)
                delegate: RowLayout {
                    required property string time
                    required property int level
                    required property string source
                    required property string text
                    id: logRow
                    width: logList.width - 12
                    x: 10
                    height: Math.max(20, message.implicitHeight + 6)   // long messages wrap, whole
                    spacing: 8
                    Label { text: logRow.time; color: Theme.faint; font.pixelSize: 11; font.family: "Consolas"; Layout.alignment: Qt.AlignTop; Layout.topMargin: 3 }
                    Label { text: logRow.source; color: panel.levelColor(logRow.level); font.pixelSize: 11; Layout.preferredWidth: 76; elide: Text.ElideRight
                            Layout.alignment: Qt.AlignTop; Layout.topMargin: 3 }
                    Label {
                        id: message
                        text: logRow.text
                        color: logRow.level === 3 ? Theme.bad : Theme.text
                        font.pixelSize: 11
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 3
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: logList.count === 0
                    text: "Журнал пуст"
                    color: Theme.faint; font.pixelSize: 11
                }
            }
        }
    }
}
