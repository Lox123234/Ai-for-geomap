import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoMapAI.Survey

// «Схема съёмки» (the GPS tab): where the photos were taken and where the alignment placed them,
// their errors, footprints and flight path, the control points (SurveyView), with a toolbar on
// top and the summary, the cameras and the points on the right — as Metashape's Reference pane.
Item {
    id: workspace
    property var host

    function fmt(v, d) { return v === undefined ? "—" : Number(v).toLocaleString(Qt.locale("ru_RU"), 'f', d === undefined ? 2 : d) }

    RowLayout {
        anchors.fill: parent
        spacing: 0
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            // --- Toolbar: wraps onto a second row when the view is narrow.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: toolbar.implicitHeight + 12
                color: Theme.toolbar
                Flow {
                    id: toolbar
                    anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 10; anchors.rightMargin: 8
                    spacing: 6
                    Label { text: "Выравнивание"; color: Theme.muted; font.pixelSize: 11; height: 28; verticalAlignment: Text.AlignVCenter }
                    ComboBox {
                        id: jobBox
                        objectName: "surveyJob"
                        width: 250
                        implicitHeight: 28
                        enabled: view.jobs.length > 0
                        model: view.jobs.length > 0 ? view.jobs : [{ id: "", title: "ещё не было — только GPS" }]
                        textRole: "title"; valueRole: "id"
                        currentIndex: Math.max(0, indexOfValue(view.shownJob))
                        onActivated: view.jobId = currentValue
                        font.pixelSize: 12
                    }
                    Rectangle { width: 1; height: 28; color: Theme.line }
                    Repeater {
                        model: [{ t: "Подложка", p: "showBackground" }, { t: "Охваты", p: "showFootprints" },
                                { t: "Маршрут", p: "showPath" }, { t: "Подписи", p: "showLabels" }]
                        delegate: FlatButton {
                            required property var modelData
                            compact: true
                            checkable: true
                            checked: view[modelData.p]
                            emphasis: checked
                            text: modelData.t
                            onToggled: view[modelData.p] = checked
                        }
                    }
                    ComboBox {
                        id: scaleBox
                        implicitHeight: 28
                        width: 130
                        model: [{ t: "Ошибки: авто", v: 0 }, { t: "Ошибки ×1", v: 1 }, { t: "Ошибки ×10", v: 10 },
                                { t: "Ошибки ×100", v: 100 }, { t: "Ошибки ×1000", v: 1000 }]
                        textRole: "t"; valueRole: "v"
                        onActivated: view.errorScale = currentValue
                        font.pixelSize: 12
                    }
                    Rectangle { width: 1; height: 28; color: Theme.line }
                    FlatButton {
                        objectName: "surveyPlace"
                        compact: true
                        checkable: true
                        checked: view.placing
                        emphasis: checked
                        text: checked ? "Щёлкните по схеме…" : "Опорная точка"
                        enabled: view.shownJob.length > 0
                        onToggled: view.placing = checked
                        ToolTip.visible: hovered; ToolTip.delay: 500
                        ToolTip.text: view.shownJob.length > 0
                                      ? "Щелчок по схеме — опорная точка, высота по модели"
                                      : "Сначала выровняйте снимки"
                    }
                    FlatButton { compact: true; text: "Отметить на снимках"; enabled: survey.controlCount + survey.checkCount > 0
                                 onClicked: workspace.host.runCommand("project.gcpTag") }
                    FlatButton {
                        objectName: "surveyGeoref"
                        compact: true; text: "Привязать снимок"
                        enabled: projects.selectedIndex >= 0 && gis.crsDefined && !georef.busy
                        onClicked: workspace.host.runCommand("tool.georeferencePhoto")
                        ToolTip.visible: hovered; ToolTip.delay: 500
                        ToolTip.text: projects.selectedIndex >= 0 ? "Выровнять выбранный снимок по точкам на карте" : "Сначала выберите снимок"
                    }
                    FlatButton { compact: true; text: "Показать всё"; onClicked: view.fitAll() }
                }
            }
            // --- The scheme.
            SurveyView {
                id: view
                objectName: "surveyView"
                Layout.fillWidth: true
                Layout.fillHeight: true
                projectManager: projects
                processingService: processing
                surveyControl: survey
                selectedRow: projects.selectedIndex
                onPhotoActivated: function(row) { projects.selectPhoto(row); workspace.host.runCommand("view.photo") }
                onPointPlaced: function(id, name, fromModel) {
                    view.placing = false
                    workspace.host.flash("Точка " + name + " поставлена" + (fromModel ? "" : " (высота примерная)") + ". Отметьте её на снимках")
                }
                onMessage: function(text) { view.placing = false; workspace.host.showNotice(text) }

                // Legend.
                Rectangle {
                    anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 12
                    visible: view.status.length === 0
                    color: "#E0111B26"; radius: 6
                    border.color: Theme.line
                    implicitWidth: legend.implicitWidth + 20
                    implicitHeight: legend.implicitHeight + 16
                    ColumnLayout {
                        id: legend
                        anchors.centerIn: parent
                        spacing: 4
                        RowLayout {
                            spacing: 6
                            Rectangle { width: 10; height: 10; radius: 5; color: "transparent"; border.color: "#EBF0F6"; border.width: 1.5 }
                            Label { text: "по GPS снимка"; color: Theme.text; font.pixelSize: 11 }
                        }
                        RowLayout {
                            visible: view.shownJob.length > 0
                            spacing: 6
                            Rectangle {
                                width: 46; height: 8; radius: 4
                                gradient: Gradient { orientation: Gradient.Horizontal
                                    GradientStop { position: 0; color: "#3AD07A" } GradientStop { position: 0.5; color: "#F5C542" } GradientStop { position: 1; color: "#EF5350" } }
                            }
                            Label { text: "после выравнивания; цвет — ошибка по высоте"; color: Theme.text; font.pixelSize: 11 }
                        }
                        Label {
                            visible: view.shownJob.length > 0
                            text: "линия — ошибка в плане, увеличена ×" + view.effectiveErrorScale.toLocaleString(Qt.locale("ru_RU"), 'f', 0)
                            color: Theme.muted; font.pixelSize: 11
                        }
                        RowLayout {
                            visible: view.pointRows.length > 0
                            spacing: 10
                            Label { text: "▲ опорная"; color: "#FF7A45"; font.pixelSize: 11 }
                            Label { text: "▲ контрольная"; color: "#B284FF"; font.pixelSize: 11 }
                        }
                        Label {
                            visible: view.summary.background !== undefined && view.summary.background.length > 0 && view.showBackground
                            text: "подложка: " + (view.summary.background || "") + (view.summary.crs ? " · " + view.summary.crs : "")
                            color: Theme.faint; font.pixelSize: 10
                        }
                    }
                }
                // Nothing to show / computing.
                ColumnLayout {
                    anchors.centerIn: parent
                    visible: view.status.length > 0
                    spacing: 10
                    BusyIndicator { running: view.busy; visible: view.busy; Layout.alignment: Qt.AlignHCenter }
                    Label {
                        text: view.status
                        color: Theme.muted; font.pixelSize: 13
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignHCenter
                        Layout.maximumWidth: 460
                    }
                }
            }
        }
        Rectangle { Layout.fillHeight: true; implicitWidth: 1; color: Theme.line }
        // --- Summary, cameras, points.
        Rectangle {
            Layout.preferredWidth: 300
            Layout.fillHeight: true
            color: Theme.panel
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 6
                SectionTitle { text: "СВОДКА" }
                Label {
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: view.summary.photos === undefined ? "—"
                          : "Снимков " + view.summary.photos + " · с GPS " + view.summary.withGps
                            + (view.shownJob.length > 0 ? "\nВыровнено " + view.summary.aligned + (view.summary.notAligned > 0 ? " · не выровнено " + view.summary.notAligned : "") : "\nВыравнивания ещё не было")
                    color: Theme.text; font.pixelSize: 12
                }
                GridLayout {
                    visible: view.summary.rmseXY !== undefined
                    columns: 2; columnSpacing: 12; rowSpacing: 2
                    Label {
                        text: "Ошибка камер, м"; color: Theme.muted; font.pixelSize: 11; Layout.columnSpan: 2
                        ToolTip.visible: camHover.hovered; ToolTip.delay: 400; ToolTip.text: "GPS снимков против выравнивания (СКО)"
                        HoverHandler { id: camHover }
                    }
                    Label { text: "X / Y"; color: Theme.faint; font.pixelSize: 11 }
                    Label { text: workspace.fmt(view.summary.rmseX) + " / " + workspace.fmt(view.summary.rmseY); color: Theme.text; font.pixelSize: 11 }
                    Label { text: "в плане"; color: Theme.faint; font.pixelSize: 11 }
                    Label { text: workspace.fmt(view.summary.rmseXY); color: Theme.text; font.pixelSize: 11 }
                    Label { text: "по высоте"; color: Theme.faint; font.pixelSize: 11 }
                    Label { text: workspace.fmt(view.summary.rmseZ); color: Theme.text; font.pixelSize: 11 }
                    Label { text: "общая"; color: Theme.faint; font.pixelSize: 11 }
                    Label { text: workspace.fmt(view.summary.rmse); color: Theme.text; font.pixelSize: 11; font.bold: true }
                }
                SectionTitle { text: "КАМЕРЫ"; Layout.topMargin: 6 }
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: "снимок"; color: Theme.faint; font.pixelSize: 10; Layout.fillWidth: true }
                    Label { text: "в плане"; color: Theme.faint; font.pixelSize: 10; Layout.preferredWidth: 56; horizontalAlignment: Text.AlignRight }
                    Label { text: "по высоте"; color: Theme.faint; font.pixelSize: 10; Layout.preferredWidth: 60; horizontalAlignment: Text.AlignRight }
                }
                ListView {
                    id: cameraList
                    objectName: "surveyCameras"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 120
                    clip: true
                    model: view.cameraRows
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    delegate: Rectangle {
                        required property var modelData
                        width: cameraList.width
                        height: 22
                        color: modelData.row === projects.selectedIndex ? Theme.accentSoft : rowMouse.containsMouse ? Theme.hover : "transparent"
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 4; anchors.rightMargin: 4
                            Label { text: modelData.name; color: Theme.text; font.pixelSize: 11; elide: Text.ElideMiddle; Layout.fillWidth: true }
                            Label {
                                Layout.preferredWidth: 56; horizontalAlignment: Text.AlignRight; font.pixelSize: 11
                                text: modelData.dxy !== undefined ? workspace.fmt(modelData.dxy) : modelData.alignedKnown && !modelData.aligned ? "не выровнен" : !modelData.gps ? "нет GPS" : "—"
                                color: modelData.alignedKnown && !modelData.aligned ? Theme.bad : Theme.text
                            }
                            Label {
                                Layout.preferredWidth: 60; horizontalAlignment: Text.AlignRight; font.pixelSize: 11
                                text: modelData.dz !== undefined ? workspace.fmt(modelData.dz) : ""
                                color: Theme.muted
                            }
                        }
                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: { projects.selectPhoto(modelData.row); view.zoomToRow(modelData.row) }
                            onDoubleClicked: { projects.selectPhoto(modelData.row); workspace.host.runCommand("view.photo") }
                        }
                    }
                }
                SectionTitle { text: "ОПОРНЫЕ И КОНТРОЛЬНЫЕ ТОЧКИ"; Layout.topMargin: 6 }
                Label {
                    visible: view.pointRows.length === 0
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: "Точек нет — поставьте на схеме"
                    color: Theme.faint; font.pixelSize: 11
                }
                Repeater {
                    model: view.pointRows
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Label { text: "▲"; color: modelData.check ? "#B284FF" : "#FF7A45"; font.pixelSize: 11 }
                        Label { text: modelData.name; color: Theme.text; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight }
                        Label {
                            font.pixelSize: 11; color: modelData.dxy !== undefined ? Theme.text : Theme.faint
                            text: modelData.dxy !== undefined ? workspace.fmt(modelData.dxy, 3) + " / " + workspace.fmt(modelData.dz, 3) + " м"
                                                               : "на снимках: " + modelData.photos
                        }
                    }
                }
                Label {
                    visible: view.pointSummary.control !== undefined || view.pointSummary.check !== undefined
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: (view.pointSummary.control ? "СКО опорных: " + workspace.fmt(view.pointSummary.control.rmse, 3) + " м" : "")
                          + (view.pointSummary.check ? (view.pointSummary.control ? " · " : "") + "контрольных: " + workspace.fmt(view.pointSummary.check.rmse, 3) + " м" : "")
                    color: Theme.good; font.pixelSize: 11
                }
                FlatButton { compact: true; text: "Управление GCP…"; Layout.fillWidth: true; onClicked: workspace.host.runCommand("project.gcp") }
            }
        }
    }
}
